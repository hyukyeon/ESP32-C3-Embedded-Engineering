/*
 * Feature: RISC-V Machine Control and Status Registers (MCSR)
 * Description: Directly accessing hardware registers for performance profiling.
 */

#include <Arduino.h>

// Function Prototypes
uint32_t read_mcycle();

uint32_t read_mcycle() {
    uint32_t count;
    __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(count));
    return count;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
}

void loop() {
    uint32_t start = read_mcycle();
    delayMicroseconds(100);
    uint32_t end = read_mcycle();
    
    Serial.printf("Cycles elapsed: %u\n", end - start);
    delay(1000);
}
