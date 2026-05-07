/*
 * 17_ota_mmu: OTA 업데이트와 MMU 리매핑(Bank Switching)
 *
 * ESP32-C3 OTA의 핵심은 "Flash 복사"가 아니라 "부트로더가 MMU를 교체"하는 것입니다.
 *
 * 파티션 구조 (기본 OTA 파티션 테이블):
 *   ┌────────────┬──────────┬───────────────────────────────┐
 *   │ Name       │ Offset   │ 역할                          │
 *   ├────────────┼──────────┼───────────────────────────────┤
 *   │ otadata    │ 0xD000   │ 현재 부팅할 OTA 슬롯 기록      │
 *   │ ota_0      │ 0x10000  │ 앱 슬롯 A (부팅 0)            │
 *   │ ota_1      │ 0x1B0000 │ 앱 슬롯 B (부팅 1)            │
 *   └────────────┴──────────┴───────────────────────────────┘
 *
 * MMU 리매핑 동작:
 *   재부팅 전:  CPU 가상 0x42000000 → Flash ota_0 (0x10000)
 *   OTA 전환:   otadata에 ota_1을 "활성 파티션"으로 기록
 *   재부팅 후:  CPU 가상 0x42000000 → Flash ota_1 (0x1B0000)
 *
 *               ┌─────────────────────────────┐
 *               │  가상 주소 0x42000000 (고정)  │
 *               └─────────────┬───────────────┘
 *                              │ MMU 테이블
 *                    ┌─────────┴──────────┐
 *               부팅0│   Flash ota_0       │부팅1│ Flash ota_1
 *                    │   0x10000~         │     │ 0x1B0000~
 *                    └────────────────────┘     └──────────
 *
 * 이 예제가 보여주는 것:
 *   1. 현재 실행 중인 앱의 파티션 정보 (이름, Flash 오프셋, 크기)
 *   2. app_main 함수의 가상 주소 → Flash 물리 오프셋 역산 후 파티션과 대조
 *   3. 다음 부팅에 사용할 파티션 변경 (esp_ota_set_boot_partition)
 *   4. RTC 메모리에 "변경 전 파티션 오프셋" 기록
 *   5. 재부팅 후 "변경 후 파티션 오프셋" 비교 → 리매핑 확인
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_attr.h"           /* RTC_DATA_ATTR */
#include "soc/soc.h"

static const char *TAG = "OTA_MMU";

/* ---------- RTC 메모리: Deep-sleep·재부팅에서도 유지 ---------- */

RTC_DATA_ATTR static uint32_t g_boot_count       = 0;
RTC_DATA_ATTR static uint32_t g_prev_flash_offset = 0;
RTC_DATA_ATTR static char     g_prev_label[16]    = {0};

/* ---------- MMU 관련 상수 ---------- */

#define MMU_TABLE_BASE   0x600C4800UL
#define MMU_PAGE_SIZE    (64 * 1024)
#define MMU_VALID_BIT    (1 << 8)
#define MMU_PAGE_MASK    0xFF
#define IBUS_VADDR_BASE  0x42000000UL

/* ---------- 가상 주소 → Flash 물리 오프셋 ---------- */

static int vaddr_to_flash(uint32_t vaddr, uint32_t *flash_off) {
    if (vaddr < IBUS_VADDR_BASE) return -1;
    uint32_t page_idx    = (vaddr - IBUS_VADDR_BASE) / MMU_PAGE_SIZE;
    uint32_t page_offset = (vaddr - IBUS_VADDR_BASE) % MMU_PAGE_SIZE;
    volatile uint32_t *tbl = (volatile uint32_t *)MMU_TABLE_BASE;
    uint32_t entry = tbl[page_idx];
    if (!(entry & MMU_VALID_BIT)) return -2;
    *flash_off = (entry & MMU_PAGE_MASK) * MMU_PAGE_SIZE + page_offset;
    return 0;
}

/* ---------- 파티션 정보 출력 ---------- */

static void print_partition_info(const esp_partition_t *p, const char *label) {
    if (!p) { printf("  %s: (없음)\n", label); return; }
    printf("  %-18s  label='%-8s'  type=%u/%u  offset=0x%08X  size=%u bytes\n",
           label,
           p->label,
           p->type,
           p->subtype,
           (unsigned)p->address,
           (unsigned)p->size);
}

/* ---------- MMU 테이블 — 현재 앱이 매핑된 페이지 출력 ---------- */

static void dump_ibus_pages(int max_pages) {
    volatile uint32_t *tbl = (volatile uint32_t *)MMU_TABLE_BASE;
    printf("\n  [IBUS MMU 테이블 — 유효 항목만 출력 (최대 %d개)]\n", max_pages);
    printf("  Idx  Entry      Flash Page  Flash Offset  Virt Addr\n");
    printf("  ---  ---------  ----------  ------------  ----------\n");
    int shown = 0;
    for (int i = 0; i < 64 && shown < max_pages; i++) {
        uint32_t e = tbl[i];
        if (!(e & MMU_VALID_BIT)) continue;
        uint32_t phys_pg  = e & MMU_PAGE_MASK;
        uint32_t fl_off   = phys_pg * MMU_PAGE_SIZE;
        uint32_t vaddr    = IBUS_VADDR_BASE + (uint32_t)i * MMU_PAGE_SIZE;
        printf("  [%2d] 0x%08X  page %-4u    0x%06X      0x%08X\n",
               i, (unsigned)e, (unsigned)phys_pg, (unsigned)fl_off, (unsigned)vaddr);
        shown++;
    }
}

/* ---------- OTA 슬롯 전환 ---------- */

static void switch_to_other_ota(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = NULL;

    /* 현재와 다른 OTA 파티션 탐색 */
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p && p->address != running->address &&
            p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
            p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            next = p;
            break;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (!next) {
        ESP_LOGW(TAG, "다른 OTA 파티션 없음 — OTA 파티션 테이블이 필요합니다");
        ESP_LOGW(TAG, "menuconfig → Partition Table → Factory app, two OTA definitions");
        return;
    }

    printf("\n  현재: '%s' (offset=0x%08X)\n",
           running->label, (unsigned)running->address);
    printf("  전환 → '%s' (offset=0x%08X)\n",
           next->label, (unsigned)next->address);

    /* g_prev_* 에 현재 파티션 정보 저장 (재부팅 후 비교용) */
    g_prev_flash_offset = running->address;
    strncpy(g_prev_label, running->label, sizeof(g_prev_label) - 1);

    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "다음 부팅 슬롯을 '%s'으로 변경 — 5초 후 재부팅", next->label);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 실패: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "힌트: 대상 파티션에 유효한 앱 이미지가 있어야 합니다");
    }
}

/* ---------- 메인 ---------- */

void app_main(void) {
    g_boot_count++;
    ESP_LOGI(TAG, "=== OTA MMU Bank Switching Demo ===");
    ESP_LOGI(TAG, "부팅 횟수: %u", (unsigned)g_boot_count);

    /* ──────────────────────────────
     * A. 현재 부팅 파티션 정보
     * ────────────────────────────── */
    const esp_partition_t *running  = esp_ota_get_running_partition();
    const esp_partition_t *booted   = esp_ota_get_boot_partition();
    const esp_partition_t *update   = esp_ota_get_next_update_partition(NULL);

    printf("\n[A] 현재 실행 파티션 정보\n");
    print_partition_info(running, "running (현재 실행)");
    print_partition_info(booted,  "boot   (otadata 지정)");
    print_partition_info(update,  "update (다음 OTA 대상)");

    /* ──────────────────────────────
     * B. app_main 가상 주소 → Flash 물리 주소 역산
     * ────────────────────────────── */
    printf("\n[B] 현재 코드의 가상→물리 주소 역산\n");
    uint32_t code_vaddr  = (uint32_t)(uintptr_t)app_main;
    uint32_t code_paddr  = 0;
    int      conv_result = vaddr_to_flash(code_vaddr, &code_paddr);

    printf("  app_main 가상 주소   : 0x%08X\n", (unsigned)code_vaddr);
    if (conv_result == 0) {
        printf("  app_main Flash 오프셋: 0x%08X\n", (unsigned)code_paddr);
        if (running && code_paddr >= (uint32_t)running->address &&
            code_paddr < (uint32_t)running->address + running->size) {
            printf("  ✓ 파티션 '%s' 범위 내 — MMU 매핑 정상\n", running->label);
        } else {
            printf("  ! 파티션 범위 밖 — 계산 방식 차이 또는 XIP 주소 보정 필요\n");
        }
    } else {
        printf("  MMU 역산 불가 (%d) — IRAM 실행 중이거나 범위 외\n", conv_result);
    }

    dump_ibus_pages(8);

    /* ──────────────────────────────
     * C. 이전 부팅과 비교 (재부팅 후)
     * ────────────────────────────── */
    if (g_boot_count > 1 && g_prev_flash_offset != 0) {
        printf("\n[C] OTA 전환 전후 비교\n");
        printf("  이전 부팅 파티션: '%s'  offset=0x%08X\n",
               g_prev_label, (unsigned)g_prev_flash_offset);
        if (running) {
            printf("  현재 부팅 파티션: '%s'  offset=0x%08X\n",
                   running->label, (unsigned)running->address);
            if (running->address != g_prev_flash_offset) {
                printf("\n  *** 가상 주소 0x42000000은 동일하지만 ***\n");
                printf("  *** Flash 물리 오프셋이 변경되었습니다! ***\n");
                printf("  이것이 하드웨어 MMU Bank Switching입니다.\n\n");
            } else {
                printf("  파티션 동일 — 전환 전 상태\n");
            }
        }
    }

    /* ──────────────────────────────
     * D. OTA 슬롯 전환 시도
     * ────────────────────────────── */
    printf("\n[D] OTA 파티션 전환 시도\n");
    if (g_boot_count == 1) {
        switch_to_other_ota();
    } else {
        printf("  이미 전환 완료 (boot_count=%u). 전환을 원하면 디바이스를 완전 리셋하세요.\n",
               (unsigned)g_boot_count);
    }

    printf("\n[OTA MMU 핵심 정리]\n");
    printf("  1. 앱 코드 가상 주소(0x42000000)는 항상 동일\n");
    printf("  2. 부트로더가 MMU 테이블의 Flash 페이지 번호만 교체\n");
    printf("  3. ota_0/ota_1은 서로 다른 물리 Flash 오프셋에 존재\n");
    printf("  4. OTA = 다른 파티션에 이미지 쓰기 + otadata 업데이트 + 재부팅\n");
    printf("  5. 재부팅 후 부트로더가 otadata 읽고 MMU를 새 파티션으로 설정\n\n");
}
