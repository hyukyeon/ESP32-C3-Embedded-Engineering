/*
 * Feature: ISR Latency Measurement
 * Description: Measuring cycles from interrupt trigger to ISR execution.
 */

#include <Arduino.h>

volatile uint32_t trigger_time = 0;
volatile uint32_t isr_time = 0;

uint32_t IRAM_ATTR get_cycles_isr() {
    uint32_t c;
    __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(c));
    return c;
}

void IRAM_ATTR handle_interrupt() {
    isr_time = get_cycles_isr();
}

void setup() {
    Serial.begin(115200);
    pinMode(2, INPUT_PULLUP);
    pinMode(3, OUTPUT);
    attachInterrupt(digitalPinToInterrupt(2), handle_interrupt, FALLING);
}

void loop() {
    delay(2000);
    
    // Trigger interrupt by software (connect Pin 3 to Pin 2)
    digitalWrite(3, HIGH);
    delay(10);
    
    uint32_t t_start = get_cycles_isr();
    digitalWrite(3, LOW); // Trigger FALLING
    
    delay(100);
    Serial.printf("Interrupt Latency (Cycles): %u\n", isr_time - t_start);
}
