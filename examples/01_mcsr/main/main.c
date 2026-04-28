#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint32_t read_mcycle() {
    uint32_t count;
    // ESP-IDF environments often allow direct CSR access in M-mode or via config
    __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(count));
    return count;
}

void app_main(void) {
    printf("--- ESP32-C3 M-mode CSR Access ---\n");
    while(1) {
        uint32_t start = read_mcycle();
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t end = read_mcycle();
        printf("Cycles for 100ms: %u\n", end - start);
    }
}
