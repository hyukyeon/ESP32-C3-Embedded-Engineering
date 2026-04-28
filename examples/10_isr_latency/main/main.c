#include <stdio.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

volatile uint32_t isr_time = 0;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(isr_time));
}

void app_main(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<2),
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(2, gpio_isr_handler, (void*) 2);

    gpio_set_direction(3, GPIO_MODE_OUTPUT);

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t start;
        __asm__ __volatile__ ("csrr %0, mcycle" : "=r"(start));
        gpio_set_level(3, 0); // Assuming connected to 2
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("ISR Latency: %u\n", isr_time - start);
    }
}
