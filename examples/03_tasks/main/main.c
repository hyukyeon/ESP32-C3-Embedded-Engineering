/*
 * 03_tasks: FreeRTOS Preemptive Task Scheduling
 *
 * ESP32-C3 has a single RISC-V core, so only ONE task runs at any instant.
 * The FreeRTOS scheduler switches between tasks every 1 ms (Tick interrupt),
 * creating the illusion of concurrency.
 *
 * This demo runs three tasks at different priority levels:
 *   HIGH  (prio 10) — "Sensor" — wakes every 200 ms, simulates urgent sampling
 *   MID   (prio  5) — "Process"— CPU-bound work, must yield or it starves LOW
 *   LOW   (prio  1) — "Logger" — logs stats, never gets CPU if HIGH/MID hog it
 *
 * Watch the stack High Water Mark (HWM) to catch stack overflows before they happen.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "TASKS";

/* Shared counters — written by each task, read by logger */
static volatile uint32_t sensor_runs   = 0;
static volatile uint32_t process_runs  = 0;

/* ---------- High-priority: Sensor sampling ---------- */

static void sensor_task(void *arg) {
    ESP_LOGI(TAG, "[Sensor ] started on core %d, prio %d",
             xPortGetCoreID(),
             uxTaskPriorityGet(NULL));

    while (1) {
        sensor_runs++;

        /*
         * vTaskDelay voluntarily blocks this task, allowing lower-priority
         * tasks to run. Without vTaskDelay, this HIGH-prio task would
         * starve everything else — "starvation" (기아 현상).
         */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---------- Medium-priority: Processing ---------- */

static void process_task(void *arg) {
    ESP_LOGI(TAG, "[Process] started on core %d, prio %d",
             xPortGetCoreID(),
             uxTaskPriorityGet(NULL));

    while (1) {
        process_runs++;

        /* Simulate moderate computation — yields regularly so LOW task can run */
        volatile uint32_t dummy = 0;
        for (int i = 0; i < 50000; i++) dummy += i;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- Low-priority: Logging / UI ---------- */

static void logger_task(void *arg) {
    ESP_LOGI(TAG, "[Logger ] started on core %d, prio %d",
             xPortGetCoreID(),
             uxTaskPriorityGet(NULL));

    static TaskHandle_t sensor_h  = NULL;
    static TaskHandle_t process_h = NULL;
    (void)sensor_h; (void)process_h;  /* reserved for pvParameters pass-through */

    while (1) {
        uint32_t s = sensor_runs;
        uint32_t p = process_runs;

        printf("[Logger] sensor_runs=%-5" PRIu32 "  process_runs=%-5" PRIu32 "\n", s, p);

        /*
         * uxTaskGetStackHighWaterMark returns the minimum free stack words
         * ever recorded. If it reaches 0, the next push causes a stack overflow.
         * Rule of thumb: allocate stack so HWM stays above 64 words.
         */
        printf("  Stack HWM  logger=%u words  (allocated 2048 bytes)\n",
               (unsigned)uxTaskGetStackHighWaterMark(NULL));

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ---------- Stack monitor task ---------- */

static TaskHandle_t g_sensor_h  = NULL;
static TaskHandle_t g_process_h = NULL;
static TaskHandle_t g_logger_h  = NULL;

static void monitor_task(void *arg) {
    /* Give other tasks time to start */
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        printf("\n[Monitor] Stack High Water Marks (words remaining):\n");
        printf("  Sensor  task HWM = %u\n", uxTaskGetStackHighWaterMark(g_sensor_h));
        printf("  Process task HWM = %u\n", uxTaskGetStackHighWaterMark(g_process_h));
        printf("  Logger  task HWM = %u\n", uxTaskGetStackHighWaterMark(g_logger_h));

        /* Print task list (state: R=Running, B=Blocked, S=Suspended) */
        char task_list[512];
        vTaskList(task_list);
        printf("[Monitor] Task list:\n  Name            State  Prio  Stack  Num\n%s\n",
               task_list);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Preemptive Scheduling Demo ===");
    ESP_LOGI(TAG, "Core: single RISC-V  |  Tick: 1 ms  |  Scheduler: priority-based preemptive");

    xTaskCreate(sensor_task,  "Sensor",  2048, NULL, 10, &g_sensor_h);
    xTaskCreate(process_task, "Process", 2048, NULL,  5, &g_process_h);
    xTaskCreate(logger_task,  "Logger",  3072, NULL,  1, &g_logger_h);
    xTaskCreate(monitor_task, "Monitor", 3072, NULL,  2, NULL);
}
