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
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "UART_JTAG";

#define UART_PORT    UART_NUM_0
#define UART_BUF_SZ  512
#define ECHO_PIN_TX  GPIO_NUM_21   /* HW UART0 TX — ROM bootloader uses this */
#define ECHO_PIN_RX  GPIO_NUM_20   /* HW UART0 RX */

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

static void echo_task(void *arg) {
    uint8_t  buf[64];
    uint32_t echo_count = 0;

    /* Install UART driver for bidirectional use on the same CDC port */
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_PORT, &cfg);
    uart_driver_install(UART_PORT, UART_BUF_SZ * 2, 0, 0, NULL, 0);

    ESP_LOGI(TAG, "[Echo] Ready — type anything, it will be echoed back");

    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf) - 1,
                                  pdMS_TO_TICKS(50));
        if (len > 0) {
            buf[len] = '\0';
            echo_count++;
            /* Strip CR/LF for cleaner log */
            while (len > 0 &&
                   (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
                buf[--len] = '\0';
            }
            ESP_LOGI(TAG, "[Echo #%u] '%s' (%d bytes)", echo_count, buf, len);
            /* Echo back with newline */
            char reply[80];
            int  rlen = snprintf(reply, sizeof(reply), ">> %s\r\n", buf);
            uart_write_bytes(UART_PORT, reply, rlen);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------- heartbeat task ---------- */

static void heartbeat_task(void *arg) {
    uint32_t sec = 0;

    while (1) {
        sec += 2;

        /* Demonstrate all log levels — each has a distinct color in idf.py monitor */
        if (sec % 10 == 0) {
            ESP_LOGW(TAG, "[t=%us] WARN  — something unusual but not fatal", sec);
        } else if (sec % 30 == 0) {
            ESP_LOGE(TAG, "[t=%us] ERROR — a real problem occurred", sec);
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
            ESP_LOGI(TAG, "Flushing TX buffer (uart_wait_tx_done)...");
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
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
