/*
 * 07_race_mutex: Race Condition and Mutual Exclusion
 *
 * RISC-V "count++" compiles to THREE instructions:
 *   lw   a5, 0(s1)     ; Load  from RAM to register
 *   addi a5, a5, 1     ; Add   in register
 *   sw   a5, 0(s1)     ; Store back to RAM
 *
 * If the FreeRTOS scheduler switches tasks between LOAD and STORE,
 * one increment is silently lost — "Read-Modify-Write" race condition.
 *
 * Phase 1 — NO protection:
 *   Two tasks each increment a global counter 10000 times.
 *   Expected result: 20000. Actual result: < 20000 (data lost).
 *
 * Phase 2 — WITH Mutex:
 *   Same tasks, but they lock a mutex around the increment.
 *   Expected result: 20000. Actual result: exactly 20000.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "RACE";

#define ITERS_PER_TASK  10000

/* ---------- shared state ---------- */

static volatile uint32_t g_counter;
static SemaphoreHandle_t  g_mutex;

/* Completion sync: main waits for both tasks to finish */
static volatile int g_done_count;

/* ---------- unprotected tasks ---------- */

static void unprotected_task(void *arg) {
    for (int i = 0; i < ITERS_PER_TASK; i++) {
        g_counter++;   /* three-instruction RMW — NOT atomic! */
    }
    g_done_count++;
    vTaskDelete(NULL);
}

/* ---------- mutex-protected tasks ---------- */

static void protected_task(void *arg) {
    for (int i = 0; i < ITERS_PER_TASK; i++) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_counter++;
        xSemaphoreGive(g_mutex);
    }
    g_done_count++;
    vTaskDelete(NULL);
}

/* ---------- runner ---------- */

static void run_phase(const char *label, TaskFunction_t fn) {
    g_counter    = 0;
    g_done_count = 0;
    uint32_t expected = ITERS_PER_TASK * 2;

    printf("\n[%s] Starting — expected final count: %" PRIu32 "\n", label, expected);

    /* Same priority ensures time-slicing (frequent context switches) */
    xTaskCreate(fn, "T1", 2048, NULL, 5, NULL);
    xTaskCreate(fn, "T2", 2048, NULL, 5, NULL);

    /* Wait for both tasks to finish */
    while (g_done_count < 2) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint32_t result = g_counter;
    int32_t  lost   = (int32_t)expected - (int32_t)result;

    printf("[%s] Final count : %" PRIu32 " / %" PRIu32 "\n", label, result, expected);
    if (lost > 0) {
        printf("[%s] Data LOST   : %" PRId32 " increments vanished (%.2f%%)\n",
               label, lost, 100.0f * lost / expected);
    } else {
        printf("[%s] Result      : CORRECT — no data loss\n", label);
    }
}

/* ---------- critical section micro-benchmark ---------- */

static void bench_critical_section(void) {
    uint32_t start, end;

    /* Measure overhead of portENTER/EXIT_CRITICAL vs Mutex for 1000 iterations */
    volatile uint32_t cs_count = 0;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(start) :: "memory");
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    for (int i = 0; i < 1000; i++) {
        portENTER_CRITICAL(&mux);
        cs_count++;
        portEXIT_CRITICAL(&mux);
    }
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(end) :: "memory");
    printf("\n[Critical Section] 1000x portENTER/EXIT_CRITICAL: %" PRIu32 " cycles total (%.1f/iter)\n",
           end - start, (float)(end - start) / 1000.0f);

    volatile uint32_t mx_count = 0;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(start) :: "memory");
    for (int i = 0; i < 1000; i++) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        mx_count++;
        xSemaphoreGive(g_mutex);
    }
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(end) :: "memory");
    printf("[Critical Section] 1000x xSemaphoreTake/Give:     %" PRIu32 " cycles total (%.1f/iter)\n",
           end - start, (float)(end - start) / 1000.0f);
    printf("  Critical section is faster but blocks interrupts.\n"
           "  Mutex is slower but allows interrupts — prefer mutex for longer sections.\n");
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Race Condition & Mutex Demo ===");
    printf("Each task increments shared counter %d times.\n", ITERS_PER_TASK);
    printf("Two tasks run concurrently → expected total: %d\n\n", ITERS_PER_TASK * 2);

    g_mutex = xSemaphoreCreateMutex();

    run_phase("UNPROTECTED (race)", unprotected_task);
    vTaskDelay(pdMS_TO_TICKS(500));
    run_phase("MUTEX PROTECTED",    protected_task);

    bench_critical_section();

    ESP_LOGI(TAG, "\nConclusion:");
    ESP_LOGI(TAG, "  Even on a SINGLE-CORE CPU, context switches make RMW unsafe.");
    ESP_LOGI(TAG, "  Use Mutex for shared data, Critical Section for single-cycle ops.");
}
