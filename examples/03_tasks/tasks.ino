/*
 * Feature: FreeRTOS Tasks
 * Description: Creating and managing concurrent execution threads.
 */

#include <Arduino.h>

void blink_task(void *pvParameters) {
    pinMode(8, OUTPUT); // Built-in LED on many C3 boards
    for (;;) {
        digitalWrite(8, !digitalRead(8));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void setup() {
    Serial.begin(115200);
    
    xTaskCreate(
        blink_task,
        "Blink",
        2048,
        NULL,
        1,
        NULL
    );
}

void loop() {
    Serial.printf("Main loop running on core %d\n", xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(2000));
}
