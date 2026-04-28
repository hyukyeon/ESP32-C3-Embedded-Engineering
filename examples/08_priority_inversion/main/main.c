#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

SemaphoreHandle_t res;

void low(void *p) {
    while(1) {
        xSemaphoreTake(res, portMAX_DELAY);
        printf("Low holding...\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
        xSemaphoreGive(res);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mid(void *p) {
    vTaskDelay(pdMS_TO_TICKS(500));
    while(1) {
        printf("Mid running...\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void high(void *p) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    while(1) {
        printf("High waiting...\n");
        xSemaphoreTake(res, portMAX_DELAY);
        printf("High GOT resource!\n");
        xSemaphoreGive(res);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void) {
    res = xSemaphoreCreateMutex();
    xTaskCreate(low, "L", 2048, NULL, 1, NULL);
    xTaskCreate(mid, "M", 2048, NULL, 2, NULL);
    xTaskCreate(high, "H", 2048, NULL, 3, NULL);
}
