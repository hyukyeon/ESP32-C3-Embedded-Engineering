/*
 * 05_watchdog: Task Watchdog Timer (TWDT) — System Self-Recovery
 *
 * The TWDT watches multiple registered tasks simultaneously.
 * ALL registered tasks must call esp_task_wdt_reset() within the timeout window.
 * If even one task misses its deadline, the WDT fires a panic → system reset.
 *
 * This demo shows:
 *   1. Normal operation: two tasks check in on time.
 *   2. Simulated hang: one task deliberately stops checking in after 15 s.
 *      → WDT triggers, system reboots.
 *   3. On reboot: esp_reset_reason() identifies the cause as WDT.
 *
 * TWDT "subscription model":
 *   esp_task_wdt_add(NULL)   — register current task
 *   esp_task_wdt_reset()     — "I'm alive" signal
 *   esp_task_wdt_delete(NULL)— unregister current task
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "WDT";

#define WDT_TIMEOUT_SEC  5

/* Shared flag: set to true to simulate a hang in task_b */
static volatile bool g_simulate_hang = false;

/* ---------- helper: describe reset reason ---------- */

static void print_reset_reason(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    const char *desc;
    switch (reason) {
    case ESP_RST_POWERON:   desc = "Power-on";                break;
    case ESP_RST_SW:        desc = "Software reset";          break;
    case ESP_RST_PANIC:     desc = "Panic / exception";       break;
    case ESP_RST_INT_WDT:   desc = "Interrupt WDT";           break;
    case ESP_RST_TASK_WDT:  desc = "Task WDT (TWDT)";         break;
    case ESP_RST_WDT:       desc = "Other WDT";               break;
    case ESP_RST_DEEPSLEEP: desc = "Deep-sleep wakeup";       break;
    case ESP_RST_BROWNOUT:  desc = "Brownout";                break;
    default:                desc = "Unknown";                 break;
    }
    printf("[Boot] Reset reason: %s (%d)\n\n", desc, reason);

    if (reason == ESP_RST_TASK_WDT) {
        printf("  *** TWDT fired last boot — the hang simulation worked! ***\n\n");
    }
}

/* ---------- Task A: critical worker, always healthy ---------- */

static void task_a(void *arg) {
    esp_task_wdt_add(NULL);  /* subscribe to TWDT */
    ESP_LOGI(TAG, "[Task A] registered with TWDT, prio=%d", uxTaskPriorityGet(NULL));

    uint32_t tick = 0;
    while (1) {
        tick++;
        esp_task_wdt_reset();  /* check in */
        ESP_LOGI(TAG, "[Task A] alive tick=%" PRIu32, tick);

        /* After 15 s, tell Task B to hang */
        if (tick == 15) {
            ESP_LOGW(TAG, "[Task A] Triggering Task B hang in 1 s...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            g_simulate_hang = true;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- Task B: secondary worker, will hang on command ---------- */

static void task_b(void *arg) {
    esp_task_wdt_add(NULL);  /* subscribe to TWDT */
    ESP_LOGI(TAG, "[Task B] registered with TWDT, prio=%d", uxTaskPriorityGet(NULL));

    uint32_t tick = 0;
    while (1) {
        if (g_simulate_hang) {
            /*
             * Simulated deadlock / infinite busy-loop.
             * Task B stops calling esp_task_wdt_reset().
             * After WDT_TIMEOUT_SEC seconds, TWDT fires.
             */
            ESP_LOGE(TAG, "[Task B] HANG SIMULATED — no more check-ins!");
            while (1) {
                /* busy loop — deliberately starves scheduler */
                volatile int x = 0;
                for (int i = 0; i < 1000000; i++) x++;
                /*
                 * Note: Task A still calls esp_task_wdt_reset() every 1 s,
                 * but TWDT requires ALL registered tasks to check in.
                 * Since Task B never resets, WDT triggers regardless of Task A.
                 */
            }
        }

        tick++;
        esp_task_wdt_reset();  /* check in */
        ESP_LOGI(TAG, "[Task B] alive tick=%" PRIu32, tick);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Task Watchdog Timer (TWDT) Demo ===");
    print_reset_reason();

    /*
     * Initialize TWDT: 5-second timeout, trigger system panic on expiry.
     * In ESP-IDF v5.x this becomes:
     *   const esp_task_wdt_config_t cfg = {
     *       .timeout_ms = WDT_TIMEOUT_SEC * 1000,
     *       .idle_core_mask = 0,
     *       .trigger_panic = true,
     *   };
     *   esp_task_wdt_init(&cfg);
     */
    const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_init(&wdt_cfg);
    ESP_LOGI(TAG, "TWDT initialized: timeout=%d s, panic=true", WDT_TIMEOUT_SEC);

    /* Both tasks subscribe; BOTH must check in to keep the timer alive */
    xTaskCreate(task_a, "TaskA", 2048, NULL, 5, NULL);
    xTaskCreate(task_b, "TaskB", 2048, NULL, 5, NULL);

    /*
     * app_main itself does NOT subscribe — it exits cleanly.
     * The IDLE task is excluded from TWDT monitoring by default.
     */
}
