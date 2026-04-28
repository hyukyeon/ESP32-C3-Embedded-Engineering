#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void task_one(void *pvParameters) {
    while(1) {
        printf("Task 1 running on core %d\n", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    xTaskCreate(task_one, "task_one", 2048, NULL, 5, NULL);
}
