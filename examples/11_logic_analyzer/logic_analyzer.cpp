/*
 * Feature: Software Logic Analyzer
 * Description: High-speed sampling using ring buffer and task separation.
 */

#include <Arduino.h>

#define BUFFER_SIZE 1000

// Global Buffers
uint8_t sample_buffer[BUFFER_SIZE];
volatile int head = 0;

// Function Prototypes
void sampling_task(void *p);
void analysis_task(void *p);

void sampling_task(void *p) {
    for(;;) {
        if (head < BUFFER_SIZE) {
            sample_buffer[head++] = digitalRead(2);
        }
        delayMicroseconds(10); 
    }
}

void analysis_task(void *p) {
    for(;;) {
        if (head >= BUFFER_SIZE) {
            Serial.println("--- Sampled Data ---");
            for(int i=0; i<BUFFER_SIZE; i++) {
                Serial.print(sample_buffer[i]);
                if(i % 50 == 49) Serial.println();
            }
            head = 0; 
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(2, INPUT);
    
    xTaskCreate(sampling_task, "Sampler", 2048, NULL, 10, NULL);
    xTaskCreate(analysis_task, "Analyzer", 2048, NULL, 1, NULL);
}

void loop() { 
    vTaskDelay(portMAX_DELAY);
}
