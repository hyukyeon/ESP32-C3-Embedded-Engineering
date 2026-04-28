#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

QueueHandle_t queue;

void sender(void *p) {
    int val = 0;
    while(1) {
        val++;
        xQueueSend(queue, &val, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    queue = xQueueCreate(10, sizeof(int));
    xTaskCreate(sender, "sender", 2048, NULL, 5, NULL);
    int rx;
    while(1) {
        if(xQueueReceive(queue, &rx, portMAX_DELAY)) {
            printf("Received: %d\n", rx);
        }
    }
}
