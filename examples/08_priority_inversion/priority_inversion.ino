/*
 * Feature: Priority Inversion
 * Description: Demonstrating how a medium priority task can block a high priority task.
 */

#include <Arduino.h>

SemaphoreHandle_t xResource;

void task_low(void *p) {
    for(;;) {
        xSemaphoreTake(xResource, portMAX_DELAY);
        Serial.println("Low task holding resource...");
        delay(3000); // Simulate long work
        xSemaphoreGive(xResource);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task_mid(void *p) {
    vTaskDelay(pdMS_TO_TICKS(500)); // Start after Low
    for(;;) {
        Serial.println("Mid task running (No resource needed)...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void task_high(void *p) {
    vTaskDelay(pdMS_TO_TICKS(1000)); // Start last
    for(;;) {
        Serial.println("High task trying to take resource...");
        xSemaphoreTake(xResource, portMAX_DELAY);
        Serial.println("High task GOT resource!");
        xSemaphoreGive(xResource);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void setup() {
    Serial.begin(115200);
    xResource = xSemaphoreCreateMutex();
    
    xTaskCreate(task_low,  "Low",  2048, NULL, 1, NULL);
    xTaskCreate(task_mid,  "Mid",  2048, NULL, 2, NULL);
    xTaskCreate(task_high, "High", 2048, NULL, 3, NULL);
}

void loop() { vTaskDelete(NULL); }
