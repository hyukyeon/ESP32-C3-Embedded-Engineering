/*
 * 21_core_dump: Core Dump — 패닉 사후 분석 (Post-mortem Debugging)
 *
 * Core Dump는 패닉 발생 시 CPU 레지스터, 스택 프레임, 힙 상태를
 * Flash 전용 파티션에 저장하여 사후 분석을 가능하게 합니다.
 *
 * 분석 워크플로우:
 *   1. 패닉 발생 → 부트로더가 Core Dump를 Flash에 저장
 *   2. 디바이스 재부팅 (정상 동작 유지)
 *   3. PC에서 분석:
 *        idf.py coredump-info   → 요약 (레지스터, 백트레이스, 태스크 목록)
 *        idf.py coredump-debug  → GDB 세션 (변수 검사, 메모리 덤프)
 *
 * 필수 설정 (menuconfig):
 *   Component config → ESP System Settings
 *     → Core dump destination: Flash
 *     → Core dump data format: ELF
 *
 * 파티션 테이블 (partitions.csv):
 *   # Name,     Type, SubType,   Offset,  Size
 *   nvs,        data, nvs,       0x9000,  0x6000
 *   phy_init,   data, phy,       0xF000,  0x1000
 *   factory,    app,  factory,   0x10000, 0x1F0000
 *   coredump,   data, coredump,  ,        64K
 *
 * 이 예제 동작:
 *   부팅 1회차: 파티션 확인 → 패닉 트리거 (NULL 역참조)
 *   부팅 2회차: Core Dump 유효성 확인 → 분석 명령 안내
 *   부팅 3회차 이상: 상태 모니터링 → 종료
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_attr.h"

static const char *TAG = "COREDUMP";

RTC_DATA_ATTR static uint32_t g_boot_count = 0;

/* ---------- Core Dump 파티션 검사 ---------- */

static const esp_partition_t *find_coredump_partition(void) {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
}

static void print_partition_info(const esp_partition_t *p) {
    if (!p) {
        printf("  ✗ coredump 파티션 없음\n");
        printf("    → partitions.csv에 'coredump, data, coredump, , 64K' 추가\n");
        printf("    → menuconfig에서 Core dump → Flash 설정\n");
        return;
    }
    printf("  ✓ coredump 파티션: label='%s'  offset=0x%08X  size=%u KB\n",
           p->label,
           (unsigned)p->address,
           (unsigned)(p->size / 1024));
}

/* ---------- Core Dump 데이터 헤더 확인 ---------- */

/*
 * ELF Core Dump 형식: 첫 4바이트 = ELF 매직 (0x7F 'E' 'L' 'F')
 * 파티션에 유효한 덤프가 없으면 0xFF...FF (Flash 소거 상태)
 */
static void check_dump_content(const esp_partition_t *p) {
    if (!p) return;

    uint8_t header[4];
    esp_err_t err = esp_partition_read(p, 0, header, sizeof(header));
    if (err != ESP_OK) {
        printf("  파티션 읽기 실패: %s\n", esp_err_to_name(err));
        return;
    }

    printf("  파티션 첫 4바이트: %02X %02X %02X %02X\n",
           header[0], header[1], header[2], header[3]);

    /* ELF 매직: 0x7F 0x45 0x4C 0x46 */
    if (header[0] == 0x7F && header[1] == 'E' &&
        header[2] == 'L'  && header[3] == 'F') {
        printf("  ✓ 유효한 ELF Core Dump 발견!\n");
    } else if (header[0] == 0xFF && header[1] == 0xFF) {
        printf("  ○ 파티션 소거 상태 — 저장된 덤프 없음\n");
    } else {
        printf("  ? 알 수 없는 포맷 (Binary 덤프이거나 부분 기록)\n");
    }
}

/* ---------- 분석 명령 출력 ---------- */

static void print_analysis_guide(void) {
    printf("\n  ┌── Core Dump 분석 명령 ─────────────────────────────────────┐\n");
    printf("  │ # 요약 분석 (레지스터, 백트레이스, 태스크 목록)           │\n");
    printf("  │ idf.py -p /dev/ttyUSB0 coredump-info                     │\n");
    printf("  │                                                           │\n");
    printf("  │ # GDB 대화형 세션 (변수 검사, 메모리 덤프 가능)           │\n");
    printf("  │ idf.py -p /dev/ttyUSB0 coredump-debug                    │\n");
    printf("  │                                                           │\n");
    printf("  │ # GDB 주요 명령                                           │\n");
    printf("  │   bt          — 백트레이스 (호출 스택)                   │\n");
    printf("  │   info locals — 현재 프레임 지역 변수                    │\n");
    printf("  │   info tasks  — FreeRTOS 태스크 목록                     │\n");
    printf("  │   p variable  — 변수 값 출력                             │\n");
    printf("  │   x/16xb addr — 메모리 덤프 (16 bytes hex)               │\n");
    printf("  └───────────────────────────────────────────────────────────┘\n");
}

/* ---------- 의도적 패닉 트리거 ---------- */

/*
 * IRAM_ATTR: 이 함수를 IRAM에 배치.
 * Core Dump에서 백트레이스가 이 함수를 포함하도록 의도.
 */
static void IRAM_ATTR level3_crash(void) {
    /* NULL 역참조 → Load access fault (mcause=5) */
    volatile uint32_t *null_ptr = (volatile uint32_t *)0x00000000;
    (void)(*null_ptr);  /* ← 이 줄에서 예외 발생 */
}

static void level2_call(void) {
    printf("  level2_call → level3_crash 호출\n");
    level3_crash();
}

static void level1_trigger(void) {
    printf("  level1_trigger → level2_call 호출\n");
    level2_call();
}

/* ---------- 메인 ---------- */

void app_main(void) {
    g_boot_count++;
    ESP_LOGI(TAG, "=== Core Dump Post-mortem Debug Demo (부팅 #%u) ===",
             (unsigned)g_boot_count);

    const esp_partition_t *cdump = find_coredump_partition();

    printf("\n[A] Core Dump 파티션 상태\n");
    print_partition_info(cdump);

    if (g_boot_count == 1) {
        /* ──────────────────────────────
         * 1회차: 패닉을 유발하여 Core Dump 생성
         * ────────────────────────────── */
        printf("\n[B] 패닉 트리거 (3초 후)\n");
        printf("  3단계 호출 스택 후 NULL 역참조:\n");
        printf("  app_main → level1_trigger → level2_call → level3_crash\n");
        printf("\n  패닉 후 코어 덤프가 Flash에 저장됩니다.\n");
        printf("  재부팅 후 'idf.py coredump-info' 로 분석하세요.\n\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(3000));

        level1_trigger();   /* ← 여기서 패닉 발생 */

    } else {
        /* ──────────────────────────────
         * 2회차 이상: 덤프 확인 및 분석 안내
         * ────────────────────────────── */
        printf("\n[B] Core Dump 내용 확인\n");
        check_dump_content(cdump);

        if (g_boot_count == 2) {
            printf("\n[C] 분석 명령 안내\n");
            print_analysis_guide();
        }

        printf("\n[Core Dump 이점]\n");
        printf("  • 재현 불가능한 패닉 원인 분석\n");
        printf("  • 패닉 시점 모든 태스크의 스택 프레임 보존\n");
        printf("  • GDB로 패닉 시점 변수 값 확인 가능\n");
        printf("  • 전원 차단 후에도 Flash에 덤프 유지\n");

        if (g_boot_count >= 3) {
            printf("\n  완전 리셋(전원 차단)하면 Cold Boot로 재시작합니다.\n");
        }
    }
    printf("\n");
}
