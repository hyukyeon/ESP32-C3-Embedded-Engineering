/*
 * Feature: ISR Latency Measurement
 */

#include <Arduino.h>

// Global Volatiles
volatile uint32_t isr_time = 0;

// Function Prototypes
uint32_t IRAM_ATTR get_cycles_isr();
void IRAM_ATTR handle_interrupt();

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
    
    digitalWrite(3, HIGH);
    delay(10);
    
    uint32_t t_start = get_cycles_isr();
    digitalWrite(3, LOW); 
    
    delay(100);
    Serial.printf("Interrupt Latency (Cycles): %u\n", isr_time - t_start);
}
