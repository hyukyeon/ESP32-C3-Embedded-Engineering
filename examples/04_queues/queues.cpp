/*
 * Feature: FreeRTOS Queues
 * Description: Safe data passing between threads.
 */

#include <Arduino.h>

// Global Handles
QueueHandle_t data_queue;

// Function Prototypes
void sender(void *p);

void sender(void *p) {
    int val = 0;
    for (;;) {
        val++;
        xQueueSend(data_queue, &val, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    Serial.begin(115200);
    data_queue = xQueueCreate(10, sizeof(int));

    xTaskCreate(sender, "Sender", 2048, NULL, 1, NULL);
}

void loop() {
    int received;
    if (xQueueReceive(data_queue, &received, portMAX_DELAY)) {
        Serial.printf("Received data: %d\n", received);
    }
}
