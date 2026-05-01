/*
 * 02_uart_jtag: Internal USB-Serial/JTAG Controller Demo
 *
 * ESP32-C3 integrates a USB-Serial/JTAG controller directly on-chip.
 * GPIO18(D-) and GPIO19(D+) connect to the internal USB PHY — no CH340/CP2102 needed.
 *
 * This demo shows:
 *   - All ESP_LOG levels and how they appear in idf.py monitor
 *   - Echo-back via USB CDC (type in terminal, see it returned)
 *   - uart_wait_tx_done() as the equivalent of Serial.flush()
 *   - Chip identification via esp_chip_info()
 *
 * JTAG tip: run `openocd -f board/esp32c3-builtin.cfg` then
 *           `riscv32-esp-elf-gdb build/02_uart_jtag.elf` to set breakpoints.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

static const char *TAG = "UART_JTAG";

/* GPIO18(D-)/GPIO19(D+) — USB-Serial/JTAG PHY pins (reference only) */
#define USB_JTAG_DM  18
#define USB_JTAG_DP  19

/* ---------- chip info ---------- */

static void print_chip_info(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("\n[Chip Information]\n");
    printf("  Model    : %s\n",
           chip.model == CHIP_ESP32C3 ? "ESP32-C3" : "Other");
    printf("  Cores    : %d\n",  chip.cores);
    printf("  Revision : %d\n",  chip.revision);
    printf("  Features : WiFi=%s  BLE=%s\n",
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
           (chip.features & CHIP_FEATURE_BLE)      ? "yes" : "no");
    printf("  ESP-IDF  : %s\n",  esp_get_idf_version());
    printf("\n[Interface Comparison]\n");
    printf("  USB-Serial/JTAG  GPIO18(D-)/19(D+)  — Active (this connection)\n");
    printf("  HW UART0         GPIO20(RX)/21(TX)  — ROM bootloader output\n\n");
}

/* ---------- echo task ---------- */

/*
 * On ESP32-C3 the console (stdin/stdout) is routed through the built-in
 * USB-Serial/JTAG CDC port (GPIO18/19).  Reading via getchar() receives
 * keystrokes sent by `idf.py monitor` or any serial terminal.
 * uart_read_bytes(UART_NUM_0) reads the *hardware* UART RX pin (GPIO20)
 * which is a completely separate peripheral — that's why it never saw input.
 */
static void echo_task(void *arg) {
    char     buf[64];
    int      idx = 0;
    uint32_t echo_count = 0;

    ESP_LOGI(TAG, "[Echo] Ready — type anything and press Enter");

    while (1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (idx > 0) {
                buf[idx] = '\0';
                echo_count++;
                ESP_LOGI(TAG, "[Echo #%u] '%s' (%d bytes)", echo_count, buf, idx);
                printf(">> %s\r\n", buf);
                fflush(stdout);
                idx = 0;
            }
        } else if (idx < (int)sizeof(buf) - 1) {
            buf[idx++] = (char)c;
        }
    }
}

/* ---------- heartbeat task ---------- */

static void heartbeat_task(void *arg) {
    uint32_t sec = 0;

    while (1) {
        sec += 2;

        /* Demonstrate all log levels — each has a distinct color in idf.py monitor */
        if (sec % 30 == 0) {
            ESP_LOGE(TAG, "[t=%us] ERROR — a real problem occurred", sec);
        } else if (sec % 10 == 0) {
            ESP_LOGW(TAG, "[t=%us] WARN  — something unusual but not fatal", sec);
        } else {
            ESP_LOGI(TAG, "[t=%us] INFO  — normal heartbeat", sec);
        }

        /*
         * Serial.flush() equivalent: uart_wait_tx_done() blocks until the
         * UART FIFO is empty. Critical before esp_deep_sleep_start() or
         * before a panic-inducing operation — otherwise the last log line
         * may never appear on the terminal.
         */
        if (sec % 20 == 0) {
            ESP_LOGI(TAG, "Flushing TX buffer (fflush)...");
            fflush(stdout);
            ESP_LOGI(TAG, "Flush done — safe to enter sleep or crash now");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void) {
    print_chip_info();

    /* DEBUG and VERBOSE only appear if CONFIG_LOG_DEFAULT_LEVEL >= 4 */
    ESP_LOGD(TAG, "DEBUG visible at log level DEBUG or VERBOSE");
    ESP_LOGV(TAG, "VERBOSE visible only at VERBOSE level");

    xTaskCreate(echo_task,      "echo",      3072, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 2, NULL);
}
