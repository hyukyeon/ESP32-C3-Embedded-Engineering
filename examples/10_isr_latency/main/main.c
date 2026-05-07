/*
 * 10_isr_latency: Interrupt Latency Measurement
 *
 * ISR latency = time from hardware event → first instruction of ISR.
 * It consists of:
 *   1. Signal detection by PLIC (Platform-Level Interrupt Controller)
 *   2. Current instruction completes / pipeline flushes
 *   3. Register context saved to task stack (most expensive step)
 *   4. Vector table lookup → jump to ISR entry point
 *
 * Latency sources (Jitter):
 *   - Critical sections that disable interrupts (portENTER_CRITICAL)
 *   - I-Cache miss if ISR code is in Flash (use IRAM_ATTR to eliminate)
 *   - Nested interrupts (higher-prio ISR already running)
 *
 * Measurement method:
 *   1. Record mcycle at the software trigger point (T_trigger).
 *   2. ISR records mcycle at its first instruction (T_isr).
 *   3. Latency = T_isr − T_trigger.
 *
 * This is a SOFTWARE simulation: GPIO3 (output) is looped back to GPIO2 (input).
 * In real hardware you would use an oscilloscope on the GPIO pin.
 *
 * Hardware setup: connect GPIO3 → GPIO2 with a jumper wire.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "ISR_LAT";

#define CPU_FREQ_HZ   160000000ULL
#define TRIGGER_PIN   GPIO_NUM_3   /* output — connect to INPUT_PIN */
#define INPUT_PIN     GPIO_NUM_2   /* input  — triggers ISR */
#define SAMPLES        50

/* ---------- shared measurement state ---------- */

static volatile uint32_t g_trigger_cycle = 0;
static volatile uint32_t g_isr_cycle     = 0;
static volatile bool     g_sample_ready  = false;

/* ---------- ISR — placed in IRAM for deterministic latency ---------- */

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    /*
     * First instruction of ISR: capture cycle counter immediately.
     * __volatile__ + "memory" clobber prevent the compiler from
     * reordering this read relative to the function prologue.
     */
    uint32_t t;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(t) :: "memory");
    g_isr_cycle    = t;
    g_sample_ready = true;

    /* Re-arm: set output HIGH so next falling edge can be triggered */
    gpio_set_level(TRIGGER_PIN, 1);
}

/* ---------- statistics ---------- */

typedef struct {
    uint32_t min, max, avg;
    uint32_t data[SAMPLES];
} Stats;

static Stats g_stats;

static void compute_stats(void) {
    uint64_t sum = 0;
    g_stats.min = UINT32_MAX;
    g_stats.max = 0;
    for (int i = 0; i < SAMPLES; i++) {
        uint32_t v = g_stats.data[i];
        if (v < g_stats.min) g_stats.min = v;
        if (v > g_stats.max) g_stats.max = v;
        sum += v;
    }
    g_stats.avg = (uint32_t)(sum / SAMPLES);
}

static void print_stats(void) {
    compute_stats();
    printf("\n[ISR Latency over %d samples at 160 MHz]\n", SAMPLES);
    printf("  min = %4u cycles  = %.0f ns\n",
           (unsigned)g_stats.min, (double)g_stats.min * 1e9 / CPU_FREQ_HZ);
    printf("  max = %4u cycles  = %.0f ns\n",
           (unsigned)g_stats.max, (double)g_stats.max * 1e9 / CPU_FREQ_HZ);
    printf("  avg = %4u cycles  = %.0f ns\n",
           (unsigned)g_stats.avg, (double)g_stats.avg * 1e9 / CPU_FREQ_HZ);
    printf("  jitter (max-min) = %u cycles  = %.0f ns\n",
           (unsigned)(g_stats.max - g_stats.min),
           (double)(g_stats.max - g_stats.min) * 1e9 / CPU_FREQ_HZ);

    /* Simple ASCII histogram — 8 buckets */
    uint32_t lo = g_stats.min, hi = g_stats.max;
    if (lo == hi) { printf("  (all identical)\n"); return; }
    uint32_t w = (hi - lo + 7) / 8;
    uint32_t buckets[8] = {0};
    for (int i = 0; i < SAMPLES; i++) {
        int b = (int)((g_stats.data[i] - lo) / w);
        if (b >= 8) b = 7;
        buckets[b]++;
    }
    printf("  Distribution:\n");
    for (int b = 0; b < 8; b++) {
        printf("  %3u cy |", (unsigned)(lo + b * w));
        for (uint32_t j = 0; j < buckets[b] * 30 / SAMPLES; j++) putchar('#');
        printf(" %u\n", (unsigned)buckets[b]);
    }
}

/* ---------- main ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== ISR Latency Measurement ===");
    ESP_LOGI(TAG, "Hardware: connect GPIO%d (output) --> GPIO%d (input)",
             TRIGGER_PIN, INPUT_PIN);

    /* Configure trigger pin (output) */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << TRIGGER_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_set_level(TRIGGER_PIN, 1);   /* idle HIGH */

    /* Configure input pin — falling edge triggers ISR */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << INPUT_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&in_cfg);

    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);   /* ISR in IRAM */
    gpio_isr_handler_add(INPUT_PIN, gpio_isr_handler, NULL);

    ESP_LOGI(TAG, "Collecting %d samples...", SAMPLES);

    int collected = 0;
    while (collected < SAMPLES) {
        g_sample_ready = false;

        /* Trigger: record cycle, then drive pin LOW */
        uint32_t t_trigger;
        __asm__ __volatile__("csrr %0, mcycle" : "=r"(t_trigger) :: "memory");
        g_trigger_cycle = t_trigger;
        gpio_set_level(TRIGGER_PIN, 0);   /* falling edge → ISR fires */

        /* Wait for ISR to record its cycle */
        uint32_t timeout = 0;
        while (!g_sample_ready && ++timeout < 100000) {}

        if (g_sample_ready) {
            int32_t latency = (int32_t)(g_isr_cycle - g_trigger_cycle);
            if (latency > 0 && latency < 2000) {   /* sanity check */
                g_stats.data[collected++] = (uint32_t)latency;
                printf("[%2d] latency = %4u cycles  (%.0f ns)\n",
                       collected, (unsigned)latency,
                       (double)latency * 1e9 / CPU_FREQ_HZ);
            }
        } else {
            ESP_LOGW(TAG, "No ISR — is GPIO%d connected to GPIO%d?",
                     TRIGGER_PIN, INPUT_PIN);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    print_stats();

    ESP_LOGI(TAG, "\nTips to reduce latency:");
    ESP_LOGI(TAG, "  1. IRAM_ATTR on ISR  — eliminates cache-miss penalty (already done)");
    ESP_LOGI(TAG, "  2. Avoid long portENTER_CRITICAL() sections in other tasks");
    ESP_LOGI(TAG, "  3. Keep ISR body short — defer work to a task via queue");
    ESP_LOGI(TAG, "  4. Use esp_intr_alloc with high priority level");
}
