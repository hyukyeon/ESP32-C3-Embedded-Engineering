#include <stdio.h>
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    printf("Deep Sleep Demo\n");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_sleep_enable_timer_wakeup(5 * 1000000);
    esp_deep_sleep_start();
}
