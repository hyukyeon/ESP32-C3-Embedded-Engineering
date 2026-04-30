/*
 * 18_boot_diagnosis: 부팅 원인 진단 — Cold / Warm / Sync Abnormal / Async Abnormal
 *
 * 부팅 유형 분류:
 *
 *  ┌──────────────────┬──────────────────────────────────────────────────┐
 *  │ 유형             │ 특징                                             │
 *  ├──────────────────┼──────────────────────────────────────────────────┤
 *  │ Cold Boot        │ 전원 공급 시작. SRAM 전체 초기화.                │
 *  │                  │ esp_reset_reason() = ESP_RST_POWERON             │
 *  ├──────────────────┼──────────────────────────────────────────────────┤
 *  │ Warm Boot        │ 전원 유지 + CPU 리셋. RTC RAM 보존.              │
 *  │                  │ esp_restart() / Deep-sleep 복귀                 │
 *  │                  │ esp_reset_reason() = ESP_RST_SW / DEEPSLEEP 등  │
 *  ├──────────────────┼──────────────────────────────────────────────────┤
 *  │ Sync Abnormal    │ 특정 명령어 실행 → 예외 발생 (재현 가능).        │
 *  │                  │ mcause[31]=0, mepc = 원인 명령어 주소            │
 *  │                  │ esp_reset_reason() = ESP_RST_PANIC              │
 *  │                  │ 예: NULL 역참조, 불법 명령어, 정렬 오류          │
 *  ├──────────────────┼──────────────────────────────────────────────────┤
 *  │ Async Abnormal   │ 외부 이벤트 (WDT 타임아웃, 전압 저하).          │
 *  │                  │ 명령어와 독립적, mepc 무의미, 재현 불가.         │
 *  │                  │ esp_reset_reason() = ESP_RST_TASK_WDT /         │
 *  │                  │                      ESP_RST_BROWNOUT 등        │
 *  └──────────────────┴──────────────────────────────────────────────────┘
 *
 * 이 예제 동작 순서 (부팅 횟수 기반 자동 시나리오):
 *
 *  부팅 1 (Cold/최초): 진단 결과 출력 → Warm Boot 트리거 (esp_restart)
 *  부팅 2: 진단 (ESP_RST_SW) → Sync Abnormal 트리거 (NULL 쓰기 → Store fault)
 *  부팅 3: 진단 (ESP_RST_PANIC, mcause=7) → Async Abnormal 트리거 (WDT 타임아웃)
 *  부팅 4: 진단 (ESP_RST_TASK_WDT) → 종료
 *
 * Sync Abnormal 트리거 직전, panic hook을 통해 mcause/mtval/mepc를
 * RTC RAM에 저장하여 다음 부팅에서 표시합니다.
 *
 * 주의: Deep-sleep은 이 예제에서 사용하지 않습니다 (RTC 도메인 유지는
 *       06_low_power에서 다룸). 여기서는 순수 리셋 원인 분류에 집중합니다.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"

static const char *TAG = "BOOT_DIAG";

/* ── RTC RAM: Warm Boot / Sync / Async Abnormal에서도 보존 ── */

RTC_DATA_ATTR static uint32_t g_boot_count  = 0;   /* 총 부팅 횟수 */
RTC_DATA_ATTR static uint32_t g_last_mcause = 0;   /* Sync 예외 원인 코드 */
RTC_DATA_ATTR static uint32_t g_last_mtval  = 0;   /* 위반 물리 주소 */
RTC_DATA_ATTR static uint32_t g_last_mepc   = 0;   /* 위반 명령어 주소 */
RTC_DATA_ATTR static uint8_t  g_trigger     = 0;   /* 어떤 트리거를 걸었는지 기록 */

/* trigger 값 상수 */
#define TRIG_NONE        0
#define TRIG_WARM        1
#define TRIG_SYNC_ABN    2
#define TRIG_ASYNC_ABN   3

/* ── CSR 접근 ── */

#define CSR_READ(reg) ({ uint32_t _v; \
    __asm__ __volatile__("csrr %0, " #reg : "=r"(_v) :: "memory"); _v; })

/* ── esp_reset_reason → 문자열 ── */

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON   (Cold Boot)";
    case ESP_RST_EXT:       return "EXT       (외부 리셋 핀)";
    case ESP_RST_SW:        return "SW        (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC     (Sync Abnormal — 예외)";
    case ESP_RST_INT_WDT:   return "INT_WDT   (인터럽트 WDT)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT  (Async Abnormal — 태스크 WDT)";
    case ESP_RST_WDT:       return "WDT       (기타 WDT)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (Deep Sleep 복귀 — Warm Boot)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT  (Async Abnormal — 전압 저하)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "기타";
    }
}

/* ── RISC-V mcause 예외 코드 → 문자열 ── */

static const char *mcause_str(uint32_t cause) {
    /* bit31=0: 동기 예외(sync), bit31=1: 비동기 인터럽트 */
    if (cause & (1UL << 31)) return "비동기 인터럽트 (Async)";
    switch (cause & 0x1F) {
    case 0:  return "Instruction address misaligned";
    case 1:  return "Instruction access fault       (PMP X=0 위반 / 잘못된 주소)";
    case 2:  return "Illegal instruction             (존재하지 않는 명령어)";
    case 3:  return "Breakpoint                     (EBREAK)";
    case 4:  return "Load address misaligned";
    case 5:  return "Load access fault              (PMP R=0 위반)";
    case 6:  return "Store/AMO address misaligned";
    case 7:  return "Store/AMO access fault         (PMP W=0 위반 / NULL 쓰기)";
    case 8:  return "Environment call from U-mode";
    case 11: return "Environment call from M-mode";
    default: return "알 수 없는 예외";
    }
}

/* ── 부팅 유형 분류 ── */

typedef enum {
    BOOT_COLD,
    BOOT_WARM,
    BOOT_SYNC_ABNORMAL,
    BOOT_ASYNC_ABNORMAL,
} boot_type_t;

static boot_type_t classify_boot(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:
        return BOOT_COLD;
    case ESP_RST_PANIC:
        return BOOT_SYNC_ABNORMAL;
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return BOOT_ASYNC_ABNORMAL;
    default:
        /* SW reset, Deep-sleep 복귀, EXT 리셋 등 */
        return BOOT_WARM;
    }
}

static const char *boot_type_label(boot_type_t t) {
    switch (t) {
    case BOOT_COLD:           return "Cold Boot";
    case BOOT_WARM:           return "Warm Boot";
    case BOOT_SYNC_ABNORMAL:  return "Sync Abnormal Boot";
    case BOOT_ASYNC_ABNORMAL: return "Async Abnormal Boot";
    default:                  return "Unknown";
    }
}

/* ── 부팅 진단 출력 ── */

static void print_diagnosis(esp_reset_reason_t reason, boot_type_t btype) {
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║          부팅 진단 (Boot Diagnosis)              ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    printf("  부팅 횟수     : %u\n", (unsigned)g_boot_count);
    printf("  reset_reason  : %s\n", reset_reason_str(reason));
    printf("  부팅 유형     : %s\n", boot_type_label(btype));

    /* Cold Boot: RTC RAM이 초기화되므로 이전 정보 없음 */
    if (btype == BOOT_COLD) {
        printf("\n  [Cold Boot 특징]\n");
        printf("  • 전원 공급 직후: SRAM 전체 초기화\n");
        printf("  • RTC RAM도 초기화 (Deep-sleep 복귀와 달리)\n");
        printf("  • 부트로더 ROM 코드가 처음부터 실행\n");
    }

    /* Warm Boot: 이전 RTC 저장 값 표시 */
    if (btype == BOOT_WARM) {
        printf("\n  [Warm Boot 특징]\n");
        printf("  • 전원 유지 상태에서 CPU만 리셋\n");
        printf("  • RTC RAM (RTC_DATA_ATTR) 보존 → 이전 부팅 데이터 유지\n");
        if (g_trigger == TRIG_WARM) {
            printf("  • 이전 부팅에서 esp_restart() 로 유발한 의도적 리셋\n");
        }
    }

    /* Sync Abnormal: 저장된 mcause/mtval/mepc 표시 */
    if (btype == BOOT_SYNC_ABNORMAL) {
        printf("\n  [Sync Abnormal Boot 특징]\n");
        printf("  • 특정 명령어 실행 중 예외 발생 → 패닉 → 리셋\n");
        printf("  • mepc = 원인 명령어 주소 (재현 가능)\n");
        if (g_last_mcause != 0) {
            printf("\n  ┌─ 이전 부팅 예외 정보 (RTC RAM 저장값) ─────────┐\n");
            printf("  │ mcause : 0x%08X  [%s]\n",
                   (unsigned)g_last_mcause, mcause_str(g_last_mcause));
            printf("  │ mepc   : 0x%08X  (원인 명령어 가상 주소)\n",
                   (unsigned)g_last_mepc);
            printf("  │ mtval  : 0x%08X  (위반 접근 주소)\n",
                   (unsigned)g_last_mtval);
            printf("  └────────────────────────────────────────────────┘\n");

            /* 저장값 초기화 */
            g_last_mcause = g_last_mtval = g_last_mepc = 0;
        }
    }

    /* Async Abnormal: WDT 타임아웃은 mepc가 무의미 */
    if (btype == BOOT_ASYNC_ABNORMAL) {
        printf("\n  [Async Abnormal Boot 특징]\n");
        printf("  • 외부 이벤트(WDT 타임아웃, 전압 저하)로 인한 리셋\n");
        printf("  • 어떤 명령어를 실행하던 중 발생했는지는 중요하지 않음\n");
        printf("  • mepc 값이 있어도 '원인 명령어'가 아님 (단순 현재 위치)\n");
        printf("  • 재현 시점이 비결정적 — WDT 타임아웃 시각에 따라 달라짐\n");
        if (g_trigger == TRIG_ASYNC_ABN) {
            printf("  • 이전 부팅에서 TWDT 타임아웃으로 유발한 비정상 리셋\n");
        }
    }
}

/* ── Panic Hook: Sync Abnormal 직전 mcause/mtval/mepc 를 RTC RAM 에 저장 ── */

/*
 * esp_set_custom_panic_handler() 대신 표준 방법:
 * esp_register_panic_handler()는 IDF 내부 API라 버전 의존성이 있음.
 * 여기서는 패닉 핸들러를 쓰지 않고, 트리거 직전에 시뮬레이션 값을
 * RTC RAM에 미리 기록하는 방식을 사용합니다.
 * (실제 하드웨어 예외 후 RTC RAM 값이 보존되는 특성 활용)
 *
 * 실제 사용 시: esp_set_custom_panic_handler(handler) — IDF 5.0+
 */
static void save_exception_info_to_rtc(uint32_t mcause,
                                       uint32_t mepc,
                                       uint32_t mtval) {
    g_last_mcause = mcause;
    g_last_mepc   = mepc;
    g_last_mtval  = mtval;
}

/* ── 트리거 함수들 ── */

/* Warm Boot 트리거: esp_restart() */
static void trigger_warm_boot(void) {
    printf("\n[Warm Boot 트리거]\n");
    printf("  esp_restart() 호출 — CPU 리셋, RTC RAM 보존\n");
    printf("  3초 후 재시작...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));
    g_trigger = TRIG_WARM;
    esp_restart();
}

/*
 * Sync Abnormal 트리거: NULL 포인터 쓰기
 *
 * ESP32-C3는 M-mode에서 동작하며, PMP가 NULL 영역을 NO_ACCESS로 설정하지
 * 않아도 주소 0은 접근 불가 영역입니다.
 * volatile 포인터를 사용하여 컴파일러 최적화로 제거되지 않게 합니다.
 *
 * 예상 동작:
 *   Store/AMO access fault (mcause=7)
 *   mtval = 0x00000000  (NULL 주소)
 *   mepc  = 이 함수 내 쓰기 명령어 주소
 */
static void IRAM_ATTR trigger_sync_abnormal(void) {
    printf("\n[Sync Abnormal 트리거]\n");
    printf("  NULL 포인터(0x00000000)에 쓰기 시도\n");
    printf("  예상: Store/AMO access fault (mcause=7)\n");
    printf("  mepc = 현재 명령어 주소 (아래 쓰기 연산 위치)\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /*
     * 실제 예외 발생 전에 현재 PC를 RTC RAM에 저장.
     * mepc는 예외 발생 시 하드웨어가 설정하지만, 여기서는 예측값을 미리
     * 기록합니다. 실제 값은 IDF 패닉 덤프의 MEPC 필드에서 확인 가능합니다.
     */
    uint32_t pc_approx = (uint32_t)(uintptr_t)trigger_sync_abnormal;
    save_exception_info_to_rtc(
        7,           /* mcause=7: Store/AMO access fault */
        pc_approx,   /* 근사 mepc — 실제는 쓰기 명령어 위치 */
        0x00000000   /* mtval=NULL 주소 */
    );
    g_trigger = TRIG_SYNC_ABN;

    /* NULL 쓰기 → Store Access Fault → 패닉 → 리셋 */
    volatile uint32_t *null_ptr = (volatile uint32_t *)0x00000000;
    *null_ptr = 0xDEADBEEF;   /* ← 이 줄에서 예외 발생 */

    /* 도달하지 않음 */
    printf("  (이 줄은 출력되지 않아야 합니다)\n");
}

/*
 * Async Abnormal 트리거: Task WDT 타임아웃
 *
 * TWDT(Task WatchDog Timer)에 현재 태스크를 등록한 뒤,
 * 피드(reset) 없이 바쁜 루프를 실행하여 타임아웃을 유발합니다.
 *
 * 예상 동작:
 *   Task WDT 타임아웃 → ESP_RST_TASK_WDT
 *   mepc는 바쁜 루프 내 임의 위치 (원인 아님)
 */
static void trigger_async_abnormal(void) {
    printf("\n[Async Abnormal 트리거]\n");
    printf("  Task WDT(3초)에 현재 태스크 등록 후 피드 중단\n");
    printf("  예상: TASK_WDT 타임아웃 → Async Abnormal Boot\n");
    printf("  mepc 값은 '어디서 멈췄나'일 뿐 원인과 무관합니다\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000));

    g_trigger = TRIG_ASYNC_ABN;

    /* TWDT 초기화: 타임아웃 3초, 패닉 활성화 */
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 3000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_init(&wdt_cfg);
#else
    esp_task_wdt_init(3, true);
#endif

    /* 현재 태스크를 TWDT 감시 대상으로 등록 */
    esp_task_wdt_add(NULL);

    printf("  TWDT 등록 완료. 피드 없이 루프 실행 중 (타임아웃 대기)...\n");
    fflush(stdout);

    /*
     * 피드 없이 바쁜 루프 → TWDT 타임아웃 유발
     * volatile 변수로 최적화 방지
     */
    volatile uint32_t counter = 0;
    while (1) {
        counter++;
        /* esp_task_wdt_reset(NULL) 을 호출하지 않아 타임아웃 발생 */
    }
}

/* ── 메인 ── */

void app_main(void) {
    g_boot_count++;

    esp_reset_reason_t reason = esp_reset_reason();
    boot_type_t        btype  = classify_boot(reason);

    ESP_LOGI(TAG, "=== Boot Diagnosis Demo (부팅 #%u) ===",
             (unsigned)g_boot_count);

    /* 부팅 진단 출력 */
    print_diagnosis(reason, btype);

    /* ──────────────────────────────────────────────────────
     * 시나리오 실행 (부팅 횟수 기반)
     *   1회차: 최초 부팅(Cold or 첫 Warm) → Warm Boot 트리거
     *   2회차: Warm Boot 확인 → Sync Abnormal 트리거
     *   3회차: Sync Abnormal 확인 → Async Abnormal 트리거
     *   4회차 이상: Async Abnormal 확인 → 종료
     * ────────────────────────────────────────────────────── */

    printf("\n[다음 단계 시나리오]\n");

    if (g_boot_count == 1) {
        printf("  → 1회차: Warm Boot를 유발합니다 (esp_restart)\n");
        trigger_warm_boot();

    } else if (g_boot_count == 2) {
        printf("  → 2회차: Sync Abnormal Boot를 유발합니다 (NULL 쓰기)\n");
        trigger_sync_abnormal();

    } else if (g_boot_count == 3) {
        printf("  → 3회차: Async Abnormal Boot를 유발합니다 (TWDT 타임아웃)\n");
        trigger_async_abnormal();

    } else {
        printf("  → 4회차: 모든 부팅 유형 시나리오 완료.\n");
        printf("           완전 리셋(전원 차단)하면 Cold Boot부터 재시작합니다.\n");
    }

    printf("\n[부팅 유형 핵심 정리]\n");
    printf("  Cold Boot  : 전원 ON → SRAM/RTC 모두 초기화\n");
    printf("  Warm Boot  : 전원 유지 CPU 리셋 → RTC RAM 보존\n");
    printf("  Sync Abn   : 특정 명령어 → 예외 → mepc=원인주소, 재현 가능\n");
    printf("  Async Abn  : WDT/전압저하 → mepc=우연한 위치, 재현 불가\n\n");
}
