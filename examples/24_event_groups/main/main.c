/*
 * 24_event_groups: FreeRTOS 이벤트 그룹 — 다중 조건 동기화
 *
 * 이벤트 그룹(Event Groups)은 여러 태스크 간에 비트 플래그로
 * 이벤트를 알리고 기다리는 동기화 메커니즘입니다.
 *
 * 세마포어와의 차이:
 *   세마포어: 1:1 단순 신호 (자원 1개)
 *   이벤트 그룹: N:1 복합 조건 (AND/OR), 비트마스크로 다수 이벤트 동시 표현
 *
 *  ┌──────────────┐        ┌──────────────────────────┐
 *  │ sensor_A 태스크│──BIT0→│                          │
 *  │ sensor_B 태스크│──BIT1→│  EventGroup (32비트)     │
 *  │ sensor_C 태스크│──BIT2→│                          │
 *  └──────────────┘        └────────────┬─────────────┘
 *                                       │ AND: BIT0|1|2 모두 설정 시 깨움
 *                                       │ OR:  BIT0|1|2 중 하나라도 설정 시 깨움
 *                                 ┌─────▼────────┐
 *                                 │ controller   │
 *                                 └──────────────┘
 *
 * xEventGroupWaitBits() 파라미터:
 *   uxBitsToWaitFor : 기다릴 비트 마스크
 *   xClearOnExit    : pdTRUE = 반환 전 해당 비트 자동 클리어
 *   xWaitForAllBits : pdTRUE = AND 조건  pdFALSE = OR 조건
 *   xTicksToWait    : 대기 타임아웃
 *
 * 반환값: 대기 종료 시점의 이벤트 그룹 값 (어떤 비트가 설정되었는지 확인 가능)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "EVT_GRP";

/* 이벤트 비트 정의 */
#define BIT_SENSOR_A   (1 << 0)   /* 센서 A 준비 */
#define BIT_SENSOR_B   (1 << 1)   /* 센서 B 준비 */
#define BIT_SENSOR_C   (1 << 2)   /* 센서 C 준비 */
#define BIT_ALL_READY  (BIT_SENSOR_A | BIT_SENSOR_B | BIT_SENSOR_C)
#define BIT_ALARM      (1 << 3)   /* 경보 이벤트 (OR 대기 시연용) */
#define BIT_DONE       (1 << 4)   /* 처리 완료 신호 */

static EventGroupHandle_t g_eg;

/* ---------- 센서 시뮬레이터 태스크 ---------- */

typedef struct {
    const char *name;
    EventBits_t  bit;
    uint32_t     delay_ms;
} SensorArg_t;

static void sensor_task(void *arg) {
    const SensorArg_t *cfg = (const SensorArg_t *)arg;
    for (int round = 1; ; round++) {
        /* 센서 샘플링 시뮬레이션 (지연 시간이 서로 다름) */
        vTaskDelay(pdMS_TO_TICKS(cfg->delay_ms));

        ESP_LOGI(TAG, "  %s: 샘플링 완료 (round %d)", cfg->name, round);
        xEventGroupSetBits(g_eg, cfg->bit);

        /* 컨트롤러가 처리를 마칠 때까지 대기 (BIT_DONE 체크) */
        xEventGroupWaitBits(g_eg, BIT_DONE, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
    }
}

/* ---------- 경보 태스크 (OR 시연용) ---------- */

static void alarm_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3500));   /* 3.5초 후 경보 발생 */
    ESP_LOGW(TAG, "  [ALARM] 경보 이벤트 발생!");
    xEventGroupSetBits(g_eg, BIT_ALARM);
    vTaskDelete(NULL);
}

/* ---------- 컨트롤러 태스크 ---------- */

static void controller_task(void *arg) {
    (void)arg;
    int round = 0;

    while (round < 3) {
        round++;
        printf("\n  ── Round %d ──\n", round);

        /* ── AND 대기: 모든 센서가 준비될 때까지 ── */
        printf("  [AND 대기] BIT_SENSOR_A & B & C 전부 설정 대기...\n");
        int64_t t0 = esp_timer_get_time();

        EventBits_t bits = xEventGroupWaitBits(
            g_eg,
            BIT_ALL_READY,   /* 기다릴 비트 */
            pdTRUE,          /* 반환 시 자동 클리어 */
            pdTRUE,          /* AND 조건 */
            pdMS_TO_TICKS(5000));

        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

        if ((bits & BIT_ALL_READY) == BIT_ALL_READY) {
            printf("  → 모든 센서 준비 완료! (%lld ms 소요)\n",
                   (long long)elapsed_ms);
            printf("  → 반환된 비트: 0x%02X\n", (unsigned)bits);
        } else {
            printf("  → 타임아웃! 설정된 비트: 0x%02X\n", (unsigned)bits);
        }

        /* 처리 완료 신호 → 센서 태스크들이 다음 샘플링 시작 */
        xEventGroupSetBits(g_eg, BIT_DONE);
        vTaskDelay(pdMS_TO_TICKS(100));
        xEventGroupClearBits(g_eg, BIT_DONE);   /* 다음 라운드를 위해 클리어 */
    }

    /* ── OR 대기: 어느 하나라도 설정되면 ── */
    printf("\n  ── OR 대기 시연 ──\n");
    printf("  [OR 대기] BIT_SENSOR_A OR BIT_ALARM 중 하나라도 설정 대기...\n");

    EventBits_t or_bits = xEventGroupWaitBits(
        g_eg,
        BIT_SENSOR_A | BIT_ALARM,
        pdTRUE,    /* 자동 클리어 */
        pdFALSE,   /* OR 조건 */
        pdMS_TO_TICKS(5000));

    printf("  → OR 조건 충족! 설정된 비트: 0x%02X  (%s)\n",
           (unsigned)or_bits,
           (or_bits & BIT_ALARM) ? "경보 이벤트" : "센서 A 이벤트");

    ESP_LOGI(TAG, "컨트롤러 완료");
    vTaskDelete(NULL);
}

/* ---------- 세마포어와 비교 출력 ---------- */

static void print_comparison(void) {
    printf("\n[이벤트 그룹 vs 세마포어 비교]\n");
    printf("  세마포어:\n");
    printf("    xSemaphoreGive() / xSemaphoreTake()\n");
    printf("    1:1 신호 전달 — 1개 이벤트 대기만 가능\n");
    printf("    여러 이벤트 대기 시 중첩 Take() 필요 → 복잡\n\n");
    printf("  이벤트 그룹:\n");
    printf("    xEventGroupSetBits() / xEventGroupWaitBits()\n");
    printf("    N:1 복합 조건 — AND/OR 로 다수 이벤트 동시 대기\n");
    printf("    비트 클리어 여부 선택 가능 (다중 소비자 가능)\n");
    printf("    xEventGroupSync(): 여러 태스크가 서로를 기다림 (배리어)\n\n");
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Event Groups Demo ===");

    g_eg = xEventGroupCreate();
    if (!g_eg) { ESP_LOGE(TAG, "이벤트 그룹 생성 실패"); return; }

    print_comparison();

    /* 센서 태스크 3개 (지연 시간이 각기 다름) */
    static const SensorArg_t sensor_args[3] = {
        { "sensor_A", BIT_SENSOR_A, 300 },
        { "sensor_B", BIT_SENSOR_B, 600 },
        { "sensor_C", BIT_SENSOR_C, 900 },
    };
    for (int i = 0; i < 3; i++) {
        xTaskCreate(sensor_task, sensor_args[i].name,
                    2048, (void *)&sensor_args[i], 4, NULL);
    }

    /* 경보 태스크 (3.5초 후 BIT_ALARM 설정) */
    xTaskCreate(alarm_task, "alarm", 2048, NULL, 3, NULL);

    /* 컨트롤러 태스크 */
    xTaskCreate(controller_task, "controller", 4096, NULL, 5, NULL);

    /* 메인은 이벤트 그룹 상태 모니터링 */
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        EventBits_t cur = xEventGroupGetBits(g_eg);
        printf("  [모니터] 이벤트 그룹 비트: 0x%02X  A=%d B=%d C=%d ALARM=%d\n",
               (unsigned)cur,
               !!(cur & BIT_SENSOR_A),
               !!(cur & BIT_SENSOR_B),
               !!(cur & BIT_SENSOR_C),
               !!(cur & BIT_ALARM));
    }
}
