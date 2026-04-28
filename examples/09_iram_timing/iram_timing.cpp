/*
 * Feature: IRAM vs Flash Timing
 */

#include <Arduino.h>

// Function Prototypes
void IRAM_ATTR fast_function();
void slow_function();
uint32_t get_cycles();

void IRAM_ATTR fast_function() {
    for(volatile int i=0; i<100; i++);
}

void slow_function() {
    for(volatile int i=0; i<100; i++);
}

uint32_t get_cycles() {
    uint32_t c;
    __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(c));
    return c;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
}

void loop() {
    uint32_t t1, t2;

    t1 = get_cycles();
    fast_function();
    t2 = get_cycles();
    Serial.printf("IRAM Cycles: %u\n", t2 - t1);

    t1 = get_cycles();
    slow_function();
    t2 = get_cycles();
    Serial.printf("Flash Cycles: %u\n", t2 - t1);

    delay(2000);
}
