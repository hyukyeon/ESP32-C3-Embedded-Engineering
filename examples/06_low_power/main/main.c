/*
 * 06_low_power: Deep Sleep and Power Domain Management
 *
 * ESP32-C3 power modes (typical current at 3.3 V):
 *   Active       ~20–130 mA   CPU + radio on
 *   Modem-sleep  ~15–25 mA    CPU on, radio off
 *   Light-sleep  ~0.3–0.8 mA  CPU paused, RAM retained, resumes where it left off
 *   Deep-sleep   ~5–10 µA     CPU/RAM off, RTC domain only, RESETS on wakeup
 *   Hibernation  ~1–3 µA      Only RTC timer running
 *
 * Deep-sleep is NOT a pause — it is a reset. Variables in normal SRAM are lost.
 * Use RTC_DATA_ATTR to place variables in RTC RAM (retained across deep-sleep).
 *
 * Wakeup sources demonstrated:
 *   1. Timer  — wake after N seconds
 *   2. GPIO   — wake when pin goes LOW (button press)
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "SLEEP";

/* RTC RAM: survives deep-sleep resets, lost only on power-off */
RTC_DATA_ATTR static uint32_t boot_count    = 0;
RTC_DATA_ATTR static uint32_t total_wake_ms = 0;  /* crude energy tracker */

#define SLEEP_SEC       10
#define WAKEUP_GPIO     GPIO_NUM_9   /* Boot button on most C3 Super Mini boards */

/* ---------- helpers ---------- */

static void print_wakeup_cause(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        printf("  Wakeup: Timer (%d s elapsed)\n", SLEEP_SEC);
        break;
    case ESP_SLEEP_WAKEUP_GPIO:
        printf("  Wakeup: GPIO %d went LOW (button pressed)\n", WAKEUP_GPIO);
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        /* First boot — not a wakeup from sleep */
        printf("  Wakeup: First power-on (no sleep event)\n");
        break;
    default:
        printf("  Wakeup: cause=%d\n", cause);
        break;
    }
}

static void print_reset_reason(void) {
    esp_reset_reason_t r = esp_reset_reason();
    const char *desc;
    switch (r) {
    case ESP_RST_POWERON:   desc = "Power-on";          break;
    case ESP_RST_DEEPSLEEP: desc = "Deep-sleep wakeup"; break;
    case ESP_RST_SW:        desc = "Software";          break;
    case ESP_RST_PANIC:     desc = "Panic";             break;
    default:                desc = "Other";             break;
    }
    printf("  Reset reason: %s\n", desc);
}

void app_main(void) {
    /* Increment before anything else — survives the next reset too */
    boot_count++;

    ESP_LOGI(TAG, "=== Deep-Sleep Demo ===");
    printf("\n[Boot #%" PRIu32 "]\n", boot_count);
    print_reset_reason();
    print_wakeup_cause();
    printf("  Total wakeups so far: %" PRIu32 "\n\n", boot_count - 1);

    /* Configure GPIO wakeup (active-low, internal pull-up) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << WAKEUP_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    esp_sleep_enable_gpio_wakeup();
    gpio_wakeup_enable(WAKEUP_GPIO, GPIO_INTR_LOW_LEVEL);

    /* Configure timer wakeup (backup: wake even without button) */
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SEC * 1000000ULL);

    ESP_LOGI(TAG, "Wakeup sources configured:");
    ESP_LOGI(TAG, "  1. Timer: %d seconds", SLEEP_SEC);
    ESP_LOGI(TAG, "  2. GPIO%d: press the BOOT button", WAKEUP_GPIO);

    /* Stay awake 3 s so log is visible */
    uint32_t awake_ms = 3000;
    ESP_LOGI(TAG, "Staying awake for %" PRIu32 " ms then entering deep-sleep...", awake_ms);
    vTaskDelay(pdMS_TO_TICKS(awake_ms));
    total_wake_ms += awake_ms;

    /*
     * Flush before sleep: without this, the "Entering sleep" message might
     * be cut off mid-transmission as the UART peripheral powers down.
     */
    ESP_LOGI(TAG, "Entering deep-sleep now. Goodbye.");
    vTaskDelay(pdMS_TO_TICKS(10));   /* allow UART to flush */

    esp_deep_sleep_start();
    /* Execution never reaches here — deep-sleep is a reset */
}
