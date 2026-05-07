/*
 * 09_iram_timing: IRAM vs Flash Execution Timing
 *
 * Code normally lives in external SPI Flash. The CPU fetches it through an
 * Instruction Cache (I-Cache). Cache behavior is non-deterministic:
 *   Cache HIT  — near-zero wait, fast execution
 *   Cache MISS — SPI bus fetch, adds 20–100+ extra cycles (Jitter)
 *
 * IRAM (Internal SRAM) is directly wired to the CPU instruction bus.
 * Code placed there executes in exactly the same number of cycles every time
 * — "zero wait-state", deterministic (결정론적).
 *
 * This demo:
 *   1. Runs the SAME workload from Flash and from IRAM.
 *   2. Collects 30 samples each to show the cycle distribution.
 *   3. Prints min/max/avg and whether the difference is measurable.
 *
 * IRAM_ATTR tells the linker to put the function in .iram1.text instead of
 * .flash.text — the function is copied to SRAM at boot by the startup code.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "IRAM";

#define CPU_FREQ_HZ   160000000ULL
#define SAMPLES        30
#define WORK_ITERS    200    /* loop iterations per sample */

/* ---------- CSR helpers ---------- */

static inline uint32_t read_mcycle(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}

/* ---------- workloads ---------- */

/* In Flash — subject to I-Cache misses */
static uint32_t flash_workload(void) {
    volatile uint32_t acc = 0;
    for (int i = 0; i < WORK_ITERS; i++) {
        acc += (uint32_t)i * (uint32_t)(i + 1);
    }
    return acc;
}

/* In IRAM — direct CPU bus, zero wait-state, deterministic */
static IRAM_ATTR uint32_t iram_workload(void) {
    volatile uint32_t acc = 0;
    for (int i = 0; i < WORK_ITERS; i++) {
        acc += (uint32_t)i * (uint32_t)(i + 1);
    }
    return acc;
}

/* ---------- statistics ---------- */

typedef struct {
    uint32_t min;
    uint32_t max;
    uint32_t avg;
    uint32_t samples[SAMPLES];
} Stats;

static Stats collect(const char *label, uint32_t (*fn)(void), int count) {
    Stats s = { .min = UINT32_MAX, .max = 0, .avg = 0 };

    /* Warm-up: two calls discarded so I-Cache has a chance to load the function */
    fn(); fn();

    uint64_t sum = 0;
    for (int i = 0; i < count; i++) {
        uint32_t t0 = read_mcycle();
        volatile uint32_t r = fn();
        uint32_t cy = read_mcycle() - t0;
        (void)r;

        s.samples[i] = cy;
        if (cy < s.min) s.min = cy;
        if (cy > s.max) s.max = cy;
        sum += cy;
    }
    s.avg = (uint32_t)(sum / count);

    printf("[%-5s] min=%-5u  max=%-5u  avg=%-5u cycles  "
           "spread=%-4u  avg_ns=%.1f\n",
           label, (unsigned)s.min, (unsigned)s.max, (unsigned)s.avg,
           (unsigned)(s.max - s.min),
           (double)s.avg * 1e9 / (double)CPU_FREQ_HZ);
    return s;
}

static void print_histogram(const Stats *s, int count, const char *label) {
    /* Find max for scaling */
    uint32_t lo = s->min, hi = s->max;
    if (lo == hi) { printf("  (all samples identical)\n"); return; }

    uint32_t buckets[10] = {0};
    uint32_t width = (hi - lo + 9) / 10;
    for (int i = 0; i < count; i++) {
        int b = (int)((s->samples[i] - lo) / width);
        if (b >= 10) b = 9;
        buckets[b]++;
    }

    printf("[%s histogram]  (%u–%u cycles, bucket=%u)\n", label, (unsigned)lo, (unsigned)hi, (unsigned)width);
    for (int b = 0; b < 10; b++) {
        uint32_t bar = buckets[b] * 30 / count;
        printf("  %4u |", (unsigned)(lo + b * width));
        for (uint32_t j = 0; j < bar; j++) putchar('#');
        printf(" %u\n", (unsigned)buckets[b]);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== IRAM vs Flash Execution Timing ===");
    printf("Workload: %d multiply-add iterations  |  Samples: %d  |  CPU: 160 MHz\n\n",
           WORK_ITERS, SAMPLES);

    while (1) {
        printf("--- Round ---\n");

        Stats flash_stats = collect("Flash", flash_workload, SAMPLES);
        Stats iram_stats  = collect("IRAM",  iram_workload,  SAMPLES);

        int32_t avg_delta = (int32_t)flash_stats.avg - (int32_t)iram_stats.avg;
        printf("\nFlash vs IRAM avg delta: %+d cycles (%.1f ns)\n",
               (int)avg_delta, (double)avg_delta * 1e9 / (double)CPU_FREQ_HZ);
        printf("Flash spread: %u cycles  (non-determinism from cache)\n",
               (unsigned)(flash_stats.max - flash_stats.min));
        printf("IRAM  spread: %u cycles  (deterministic)\n\n",
               (unsigned)(iram_stats.max - iram_stats.min));

        print_histogram(&flash_stats, SAMPLES, "Flash");
        print_histogram(&iram_stats,  SAMPLES, "IRAM ");

        printf("\nObservation:\n");
        if (flash_stats.max - flash_stats.min > iram_stats.max - iram_stats.min + 5) {
            printf("  Flash has HIGHER jitter than IRAM — cache miss visible.\n");
        } else {
            printf("  Flash code was cached — run again to see a cold-start miss.\n");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
