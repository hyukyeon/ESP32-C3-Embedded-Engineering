/*
 * Feature: UART and Internal JTAG
 */

#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("--- UART/JTAG Status ---");
    Serial.println("USB Serial is active.");
    Serial.println("JTAG is available on GPIO 18/19.");
}

void loop() {
    if (Serial.available()) {
        String msg = Serial.readStringUntil('\n');
        Serial.print("Echo: ");
        Serial.println(msg);
    }
}
