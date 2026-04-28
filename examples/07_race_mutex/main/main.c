#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

volatile int counter = 0;
SemaphoreHandle_t mutex;

void task(void *p) {
    for(int i=0; i<1000; i++) {
        if(xSemaphoreTake(mutex, portMAX_DELAY)) {
            counter++;
            xSemaphoreGive(mutex);
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    mutex = xSemaphoreCreateMutex();
    xTaskCreate(task, "t1", 2048, NULL, 5, NULL);
    xTaskCreate(task, "t2", 2048, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("Final Count: %d\n", counter);
}
