#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUF_SIZE 500
uint8_t buf[BUF_SIZE];
volatile int head = 0;

void sampler(void *p) {
    while(1) {
        if(head < BUF_SIZE) {
            buf[head++] = gpio_get_level(2);
        }
        esp_rom_delay_us(10);
    }
}

void app_main(void) {
    gpio_set_direction(2, GPIO_MODE_INPUT);
    xTaskCreate(sampler, "sampler", 2048, NULL, 10, NULL);
    while(1) {
        if(head >= BUF_SIZE) {
            printf("Captured Data Ready\n");
            head = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
