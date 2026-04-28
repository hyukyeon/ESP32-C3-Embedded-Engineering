/*
 * Feature: Task Watchdog Timer (TWDT)
 */

#include <Arduino.h>
#include <esp_task_wdt.h>

void setup() {
    Serial.begin(115200);
    esp_task_wdt_init(3, true);
    esp_task_wdt_add(NULL); 
}

void loop() {
    Serial.println("Watchdog fed.");
    esp_task_wdt_reset();
    delay(1000);
}
