/*
 * Feature: Task Watchdog Timer (TWDT)
 * Description: Monitoring task health and preventing system hangs.
 */

#include <Arduino.h>
#include <esp_task_wdt.h>

void setup() {
    Serial.begin(115200);
    
    // Initialize Watchdog with 3 second timeout
    esp_task_wdt_init(3, true);
    esp_task_wdt_add(NULL); // Monitor current task
}

void loop() {
    Serial.println("Watchdog fed.");
    esp_task_wdt_reset();
    
    // Uncommenting the delay below will trigger a reset
    // delay(4000); 
    
    delay(1000);
}
