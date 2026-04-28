#include <stdio.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void IRAM_ATTR fast_func() {
    for(volatile int i=0; i<100; i++);
}

void app_main(void) {
    uint32_t start, end;
    while(1) {
        __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(start));
        fast_func();
        __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(end));
        printf("IRAM Cycles: %u\n", end - start);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
