/*
 * Feature: UART and Internal JTAG
 * Description: Configuration and usage of the built-in USB-Serial/JTAG controller.
 */

#include <Arduino.h>

void setup() {
    // ESP32-C3 uses the internal USB-Serial/JTAG controller by default
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("--- UART/JTAG Status ---");
    Serial.println("USB Serial is active.");
    Serial.println("JTAG is available on GPIO 18/19 via internal controller.");
}

void loop() {
    if (Serial.available()) {
        String msg = Serial.readStringUntil('\n');
        Serial.print("Echo: ");
        Serial.println(msg);
    }
}
