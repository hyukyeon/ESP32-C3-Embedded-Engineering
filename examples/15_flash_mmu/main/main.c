/*
 * 15_flash_mmu: Flash MMU — 가상 주소↔물리 주소 매핑 분석
 *
 * ESP32-C3 메모리 맵 요약:
 *
 *  가상 주소 공간         물리 주소         경로
 *  ─────────────────────  ──────────────  ─────────────────────
 *  0x42000000–0x44000000  Flash (코드)    IBUS → I-Cache → SPI
 *  0x3C000000–0x3E000000  Flash (데이터)  DBUS → D-Cache → SPI
 *  0x3FC80000–0x3FCFFFFF  Internal SRAM   직결 (캐시 없음)
 *
 * Flash MMU 동작:
 *   Flash 주소 공간(최대 16 MB)을 64 KB 단위 페이지로 분할하여
 *   CPU의 가상 주소 공간에 매핑합니다.
 *
 *   MMU Table (128 entries × 4 bytes = 512 bytes @ 0x600C4800):
 *   ┌───────┐
 *   │ Entry │ bits[8:0] = Flash Page Number (0~255)
 *   │  [0]  │ bit[8]    = Valid
 *   │  [1]  │
 *   │  ...  │  가상 페이지 0 → Flash 물리 페이지 N
 *   └───────┘  가상 주소 = 0x42000000 + page_idx × 64KB
 *              물리 주소 = page_value × 64KB
 *
 * 이 예제가 보여주는 것:
 *   1. esp_partition_mmap()으로 파티션을 가상 주소에 매핑
 *   2. 가상 주소에서 물리 Flash 오프셋 역산
 *   3. MMU 테이블 레지스터를 직접 읽어 매핑 확인
 *   4. 포인터 접근 vs esp_partition_read() 결과 비교
 *   5. 현재 실행 중인 코드(.text)의 가상→물리 매핑 출력
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "soc/soc.h"           /* SOC_DROM_LOW, SOC_IROM_LOW */
#include "soc/ext_mem_defs.h"  /* MMU 페이지 크기, 주소 정의 */

static const char *TAG = "FLASH_MMU";

/* ESP32-C3 MMU 테이블 기준 주소 (TRM Table 5-2) */
#define MMU_TABLE_BASE   0x600C4800UL
#define MMU_PAGE_SIZE    (64 * 1024)    /* 64 KB per page */
#define MMU_VALID_BIT    (1 << 8)
#define MMU_PAGE_MASK    0xFF           /* bits[7:0] = page number */

/* ESP32-C3 가상 주소 기준점 */
#define IBUS_VADDR_BASE  0x42000000UL   /* Instruction bus */
#define DBUS_VADDR_BASE  0x3C000000UL   /* Data bus */

/* ---------- MMU 테이블 읽기 ---------- */

static uint32_t mmu_read_entry(int idx) {
    volatile uint32_t *tbl = (volatile uint32_t *)MMU_TABLE_BASE;
    return tbl[idx];
}

/*
 * 가상 주소 → Flash 물리 오프셋 계산
 *   1. 가상 주소가 어느 가상 페이지에 속하는지 계산
 *   2. MMU 테이블의 해당 엔트리에서 물리 페이지 번호 읽기
 *   3. 물리 오프셋 = 물리 페이지 번호 × 64KB + 페이지 내 오프셋
 */
static int32_t vaddr_to_flash_offset(uint32_t vaddr, uint32_t *flash_offset) {
    uint32_t base;
    if (vaddr >= IBUS_VADDR_BASE && vaddr < IBUS_VADDR_BASE + 0x2000000U) {
        base = IBUS_VADDR_BASE;
    } else if (vaddr >= DBUS_VADDR_BASE && vaddr < DBUS_VADDR_BASE + 0x2000000U) {
        base = DBUS_VADDR_BASE;
    } else {
        return -1; /* 매핑 안 된 주소 */
    }
    uint32_t page_idx    = (vaddr - base) / MMU_PAGE_SIZE;
    uint32_t page_offset = (vaddr - base) % MMU_PAGE_SIZE;
    uint32_t entry       = mmu_read_entry((int)page_idx);

    if (!(entry & MMU_VALID_BIT)) return -2; /* 유효하지 않은 엔트리 */

    uint32_t phys_page = entry & MMU_PAGE_MASK;
    *flash_offset      = phys_page * MMU_PAGE_SIZE + page_offset;
    return 0;
}

/* ---------- MMU 테이블 덤프 ---------- */

static void dump_mmu_range(const char *label, uint32_t vbase, int pages) {
    printf("\n[MMU Table: %s]\n", label);
    printf("  Idx  Entry     PhysPage  Flash Offset  Valid\n");
    printf("  ---  --------  --------  ------------  -----\n");
    for (int i = 0; i < pages; i++) {
        uint32_t entry    = mmu_read_entry(i);
        int      valid    = (entry & MMU_VALID_BIT) ? 1 : 0;
        uint32_t phys_pg  = entry & MMU_PAGE_MASK;
        uint32_t vaddr    = vbase + (uint32_t)i * MMU_PAGE_SIZE;
        uint32_t fl_off   = phys_pg * MMU_PAGE_SIZE;
        printf("  [%2d] 0x%08X  page%-4u    0x%06X    %s\n",
               i, (unsigned)entry, (unsigned)phys_pg, (unsigned)fl_off, valid ? "YES" : "no");
        if (!valid) break; /* 무효 엔트리 이후는 건너뜀 */
    }
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== Flash MMU Virtual→Physical Mapping ===");

    /* ──────────────────────────────
     * 1. 현재 실행 코드의 MMU 매핑 확인
     * ────────────────────────────── */
    printf("\n[1] 현재 실행 중인 앱 코드의 MMU 매핑\n");
    /* app_main 함수 자체의 주소를 가상 주소로 사용 */
    uint32_t code_vaddr = (uint32_t)(uintptr_t)app_main;
    uint32_t code_paddr = 0;
    int ret = vaddr_to_flash_offset(code_vaddr, &code_paddr);

    printf("  app_main 가상 주소  : 0x%08X\n", (unsigned)code_vaddr);
    if (ret == 0) {
        printf("  app_main 물리 오프셋: 0x%08X (Flash 내 위치)\n", (unsigned)code_paddr);
        printf("  소속 가상 페이지    : %u (0x%08X)\n",
               (unsigned)((code_vaddr - IBUS_VADDR_BASE) / MMU_PAGE_SIZE),
               (unsigned)(IBUS_VADDR_BASE + ((code_vaddr - IBUS_VADDR_BASE) / MMU_PAGE_SIZE) * MMU_PAGE_SIZE));
    } else {
        printf("  변환 실패 (%d) — IRAM 배치 함수이거나 MMU 미지원\n", ret);
    }

    /* IBUS 매핑 첫 8 페이지 출력 */
    dump_mmu_range("Instruction Bus (0x42000000)", IBUS_VADDR_BASE, 8);
    dump_mmu_range("Data Bus       (0x3C000000)", DBUS_VADDR_BASE, 8);

    /* ──────────────────────────────
     * 2. 파티션 mmap + 역산 검증
     * ────────────────────────────── */
    printf("\n[2] NVS 파티션 mmap 후 MMU 역산\n");
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!part) {
        ESP_LOGW(TAG, "NVS 파티션 없음 — 이 실험을 건너뜀");
        goto done;
    }

    esp_partition_mmap_handle_t handle;
    const void *mptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &mptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmap 실패: %s", esp_err_to_name(err));
        goto done;
    }

    uint32_t mapped_vaddr = (uint32_t)(uintptr_t)mptr;
    uint32_t mapped_paddr = 0;
    ret = vaddr_to_flash_offset(mapped_vaddr, &mapped_paddr);

    printf("  파티션 '%s'  Flash 물리 주소: 0x%08X\n",
           part->label, (unsigned)part->address);
    printf("  mmap 가상 주소              : 0x%08X\n", (unsigned)mapped_vaddr);
    if (ret == 0) {
        printf("  MMU 역산 물리 오프셋        : 0x%08X\n", (unsigned)mapped_paddr);
        if (mapped_paddr == (uint32_t)part->address) {
            printf("  ✓ 역산 결과 = 파티션 물리 주소 (일치!)\n");
        } else {
            printf("  ! 역산 결과와 파티션 주소 불일치 (예상 가능: mmap 정렬 차이)\n");
        }
    } else {
        printf("  MMU 역산 실패 (%d)\n", ret);
    }

    /* 포인터 접근 vs partition_read 비교 */
    uint8_t via_ptr[8], via_api[8];
    memcpy(via_ptr, mptr, 8);
    esp_partition_read(part, 0, via_api, 8);

    printf("  포인터 접근 (첫 8B): ");
    for (int i = 0; i < 8; i++) printf("%02X ", via_ptr[i]);
    printf("\n");
    printf("  partition_read API : ");
    for (int i = 0; i < 8; i++) printf("%02X ", via_api[i]);
    printf("\n");

    if (memcmp(via_ptr, via_api, 8) == 0) {
        printf("  ✓ 두 방법 결과 동일 — mmap 정상 작동\n");
    } else {
        printf("  ! 결과 다름 — 캐시 불일치 또는 암호화 여부 확인 필요\n");
    }

    esp_partition_munmap(handle);

done:
    printf("\n[MMU 핵심 요약]\n");
    printf("  • Flash 64 KB 페이지 → CPU 가상 주소 공간에 매핑\n");
    printf("  • 코드(0x42xxxxxx)와 데이터(0x3Cxxxxxx)는 별도 MMU 경로\n");
    printf("  • esp_partition_mmap()이 내부적으로 MMU 테이블 엔트리 설정\n");
    printf("  • OTA 재부팅 시 같은 가상 주소(0x42000000)가 다른 물리 페이지로 전환\n\n");
}
