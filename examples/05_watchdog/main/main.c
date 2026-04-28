#include <stdio.h>
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    printf("WDT Demo\n");
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);
    while(1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
