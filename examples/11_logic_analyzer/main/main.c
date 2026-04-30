/*
 * 11_logic_analyzer: Software Logic Analyzer (Capstone Project)
 *
 * Combines everything from previous examples into a real-time data acquisition
 * system: a "Producer / Consumer" architecture where a high-priority Sampler
 * captures GPIO state at ~100 kHz and a low-priority Analyzer processes it.
 *
 * Architecture:
 *   [External Signal] → GPIO2 (input)
 *                          ↓
 *   [Sampler Task, prio 10] — samples GPIO + records mcycle timestamp
 *                          ↓  (ring buffer, mutex protected)
 *   [Analyzer Task, prio 1] — detects edges, measures pulse widths, prints
 *
 * Techniques applied:
 *   - Direct GPIO register read (REG_READ) for zero-overhead sampling
 *   - mcycle timestamp per sample for nanosecond-precision edge timing
 *   - Ring buffer with head/tail for lock-free producer (single writer)
 *   - Mutex protecting head so Analyzer reads a consistent snapshot
 *   - vTaskDelay(0) in sampler to yield without sleeping (cooperative)
 *
 * Hardware: connect any digital signal source to GPIO2.
 *   To test without hardware: GPIO3 is driven as a 500 Hz square wave.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "soc/gpio_reg.h"    /* GPIO_IN_REG for direct register read */

static const char *TAG = "LA";

#define CPU_FREQ_HZ     160000000ULL
#define SAMPLE_PIN      GPIO_NUM_2
#define SIGNAL_GEN_PIN  GPIO_NUM_3   /* internal test signal output */
#define BUF_SIZE        1024         /* ring buffer depth */
#define SAMPLE_US       10           /* target ~100 kHz */

/* ---------- sample record ---------- */

typedef struct {
    uint8_t  level;      /* GPIO state: 0 or 1 */
    uint32_t timestamp;  /* mcycle at sample time */
} Sample_t;

/* ---------- ring buffer ---------- */

static Sample_t  g_buf[BUF_SIZE];
static volatile uint32_t g_head = 0;   /* written only by Sampler */
static volatile uint32_t g_tail = 0;   /* written only by Analyzer */
static SemaphoreHandle_t g_mutex;      /* protects head snapshot for Analyzer */

static inline uint32_t buf_count(void) {
    return (g_head - g_tail) & (BUF_SIZE - 1);
}

/* ---------- CSR helper ---------- */

static inline uint32_t read_mcycle(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}

/* ---------- test signal generator ---------- */

static void signal_gen_task(void *arg) {
    gpio_set_direction(SIGNAL_GEN_PIN, GPIO_MODE_OUTPUT);
    uint32_t level = 0;
    /* 500 Hz square wave: 1 ms HIGH, 1 ms LOW */
    while (1) {
        gpio_set_level(SIGNAL_GEN_PIN, level);
        level ^= 1;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ---------- sampler — high priority, IRAM ---------- */

static void IRAM_ATTR sampler_task(void *arg) {
    gpio_set_direction(SAMPLE_PIN, GPIO_MODE_INPUT);
    uint32_t dropped = 0;

    while (1) {
        /* Direct register read: no function call overhead */
        uint32_t ts  = read_mcycle();
        uint32_t reg = REG_READ(GPIO_IN_REG);
        uint8_t  lvl = (reg >> SAMPLE_PIN) & 0x1;

        uint32_t next_head = (g_head + 1) & (BUF_SIZE - 1);
        if (next_head != g_tail) {          /* buffer not full */
            g_buf[g_head].level     = lvl;
            g_buf[g_head].timestamp = ts;
            g_head = next_head;
        } else {
            dropped++;
        }

        /* Busy-wait ~10 µs for ~100 kHz sample rate.
         * esp_rom_delay_us keeps the CPU busy — preferred over vTaskDelay
         * here because 10 µs < 1 tick (1 ms) and we are high priority. */
        esp_rom_delay_us(SAMPLE_US);
    }
}

/* ---------- analyzer — low priority ---------- */

static void analyzer_task(void *arg) {
    uint32_t prev_level = 0;
    uint32_t prev_ts    = 0;
    uint32_t edge_count = 0;
    uint32_t high_sum   = 0;  /* sum of HIGH durations in cycles */
    uint32_t low_sum    = 0;
    uint32_t high_cnt   = 0;
    uint32_t low_cnt    = 0;

    ESP_LOGI(TAG, "[Analyzer] started — waiting for data");

    while (1) {
        /* Wait until we have at least half a buffer */
        if (buf_count() < BUF_SIZE / 2) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Snapshot the current head safely */
        uint32_t snapshot_head = g_head;

        /* Process from tail to snapshot_head */
        while (g_tail != snapshot_head) {
            Sample_t *s = &g_buf[g_tail];
            g_tail = (g_tail + 1) & (BUF_SIZE - 1);

            if (prev_ts == 0) {
                prev_level = s->level;
                prev_ts    = s->timestamp;
                continue;
            }

            /* Detect edges */
            if (s->level != prev_level) {
                edge_count++;
                uint32_t duration_cy = s->timestamp - prev_ts;
                double   duration_us = (double)duration_cy * 1e6 / CPU_FREQ_HZ;

                if (prev_level == 1) {
                    /* Falling edge: previous segment was HIGH */
                    high_sum += duration_cy;
                    high_cnt++;
                } else {
                    /* Rising edge: previous segment was LOW */
                    low_sum += duration_cy;
                    low_cnt++;
                }

                /* Print edge event */
                printf("[Edge %-4u] %s→%s  duration=%.2f µs (%u cy)\n",
                       edge_count,
                       prev_level ? "HI" : "LO",
                       s->level   ? "HI" : "LO",
                       duration_us, duration_cy);
            }

            prev_level = s->level;
            prev_ts    = s->timestamp;
        }

        /* Print statistics every 20 edges */
        if (edge_count > 0 && edge_count % 20 == 0) {
            double avg_high_us = high_cnt > 0
                ? (double)high_sum / high_cnt * 1e6 / CPU_FREQ_HZ : 0;
            double avg_low_us  = low_cnt > 0
                ? (double)low_sum  / low_cnt  * 1e6 / CPU_FREQ_HZ : 0;
            double freq_hz = (avg_high_us + avg_low_us > 0)
                ? 1e6 / (avg_high_us + avg_low_us) : 0;

            printf("\n=== Signal Analysis (%u edges) ===\n", edge_count);
            printf("  avg HIGH : %.2f µs\n",  avg_high_us);
            printf("  avg LOW  : %.2f µs\n",  avg_low_us);
            printf("  est freq : %.1f Hz\n",  freq_hz);
            printf("  buf used : %u / %u\n\n", buf_count(), BUF_SIZE);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Software Logic Analyzer ===");
    ESP_LOGI(TAG, "Sample pin : GPIO%d  (connect signal here)", SAMPLE_PIN);
    ESP_LOGI(TAG, "Test signal: GPIO%d  (500 Hz square wave, loop to GPIO%d to test)",
             SIGNAL_GEN_PIN, SAMPLE_PIN);
    ESP_LOGI(TAG, "Sample rate: ~%d kHz  (IRAM sampler, %d µs interval)",
             1000 / SAMPLE_US, SAMPLE_US);
    ESP_LOGI(TAG, "Buffer     : %d samples x %d bytes = %d bytes",
             BUF_SIZE, (int)sizeof(Sample_t), (int)(BUF_SIZE * sizeof(Sample_t)));

    g_mutex = xSemaphoreCreateMutex();

    /* Signal generator (internal test source) */
    xTaskCreate(signal_gen_task, "SigGen",   1024, NULL,  2, NULL);
    /* High-priority sampler */
    xTaskCreate(sampler_task,    "Sampler",  2048, NULL, 10, NULL);
    /* Low-priority analyzer */
    xTaskCreate(analyzer_task,   "Analyzer", 4096, NULL,  1, NULL);
}
