/*
 * Feature: Race Condition & Mutex
 * Description: Demonstrating data corruption on single-core and fixing it with Mutex.
 */

#include <Arduino.h>

volatile int shared_counter = 0;
SemaphoreHandle_t xMutex;

void increment_task(void *pv) {
    for (int i = 0; i < 10000; i++) {
        // [Unsafe Version]
        // shared_counter++; 

        // [Safe Version]
        if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            shared_counter++;
            xSemaphoreGive(xMutex);
        }
    }
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
    xMutex = xSemaphoreCreateMutex();
    
    xTaskCreate(increment_task, "Inc1", 2048, NULL, 1, NULL);
    xTaskCreate(increment_task, "Inc2", 2048, NULL, 1, NULL);
}

void loop() {
    delay(2000);
    Serial.printf("Final Counter: %d (Expected 20000)\n", shared_counter);
}
