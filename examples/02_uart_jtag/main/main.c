#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UART_JTAG";

void app_main(void) {
    ESP_LOGI(TAG, "Starting UART/JTAG Demo...");
    // ESP-IDF uses ESP_LOGx for better debugging via JTAG
    while(1) {
        ESP_LOGI(TAG, "Pulse...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
