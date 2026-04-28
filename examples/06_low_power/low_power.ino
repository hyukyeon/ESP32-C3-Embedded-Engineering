/*
 * Feature: Low Power (Deep Sleep)
 * Description: Minimizing power consumption for battery operations.
 */

#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("Waking up...");
    
    // Wake up every 10 seconds
    esp_sleep_enable_timer_wakeup(10 * 1000000);
    
    Serial.println("Going to sleep in 3 seconds...");
    delay(3000);
    
    Serial.flush();
    esp_deep_sleep_start();
}

void loop() {
    // Never reached in Deep Sleep
}
