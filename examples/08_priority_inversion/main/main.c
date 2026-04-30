/*
 * 08_priority_inversion: Priority Inversion and the Mutex Solution
 *
 * The Pathfinder Bug (Mars, 1997): A low-priority task held a shared resource.
 * A medium-priority task preempted it. A high-priority task was blocked waiting
 * for the resource. Result: HIGH waited on MID — priority was effectively inverted.
 * The system watchdog eventually fired and reset the spacecraft.
 *
 * Reproduction scenario (three tasks, one shared resource):
 *   LOW  (prio 1) acquires the resource, starts long work
 *   HIGH (prio 3) wakes up, tries to acquire → BLOCKED waiting for LOW
 *   MID  (prio 2) wakes up, needs NO resource → preempts LOW
 *   NOW:  MID runs freely, LOW is stuck, HIGH keeps waiting
 *   RESULT: HIGH finishes AFTER MID despite higher priority ← inversion!
 *
 * Fix: FreeRTOS Mutex has built-in Priority Inheritance.
 *   When HIGH blocks on a mutex held by LOW, LOW's priority is temporarily
 *   raised to match HIGH. LOW now preempts MID, finishes quickly, releases
 *   the mutex, then drops back to its original priority.
 *
 * RUN WITH Binary Semaphore → see inversion (MID runs between LOW and HIGH)
 * RUN WITH Mutex            → inversion resolved (HIGH runs before MID completes)
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "PINV";

/* Timestamps (ms since boot, approximate via tick count) */
#define NOW_MS()  ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS))

#define PRIO_LOW   1
#define PRIO_MID   2
#define PRIO_HIGH  3

static SemaphoreHandle_t g_resource;

/* ---------- task implementations ---------- */

static void low_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(100));   /* let HIGH and MID be created first */

    printf("[t=%4u ms] LOW  acquires resource\n", NOW_MS());
    xSemaphoreTake(g_resource, portMAX_DELAY);

    printf("[t=%4u ms] LOW  holding resource — doing long work (3 s)...\n", NOW_MS());
    /* Simulate 3 s of work while holding the resource */
    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("[t=%4u ms] LOW  releases resource\n", NOW_MS());
    xSemaphoreGive(g_resource);

    printf("[t=%4u ms] LOW  done\n", NOW_MS());
    vTaskDelete(NULL);
}

static void mid_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500));   /* start after LOW has the resource */

    printf("[t=%4u ms] MID  running (no resource needed)\n", NOW_MS());

    /* MID does CPU work for 2 s — with inversion this will preempt LOW */
    for (int i = 0; i < 4; i++) {
        printf("[t=%4u ms] MID  working... (%d/4)\n", NOW_MS(), i + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    printf("[t=%4u ms] MID  done\n", NOW_MS());
    vTaskDelete(NULL);
}

static void high_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(300));   /* start after LOW has the resource */

    printf("[t=%4u ms] HIGH tries to acquire resource\n", NOW_MS());
    uint32_t wait_start = NOW_MS();
    xSemaphoreTake(g_resource, portMAX_DELAY);

    uint32_t waited = NOW_MS() - wait_start;
    printf("[t=%4u ms] HIGH acquired resource (waited %u ms)\n", NOW_MS(), waited);
    xSemaphoreGive(g_resource);

    printf("[t=%4u ms] HIGH done (waited %u ms for resource)\n", NOW_MS(), waited);
    vTaskDelete(NULL);
}

/* ---------- run one scenario ---------- */

static void run_scenario(const char *label, SemaphoreHandle_t resource) {
    g_resource = resource;
    printf("\n============================================================\n");
    printf(" Scenario: %s\n", label);
    printf("============================================================\n");
    printf(" Timeline  Task  Action\n");
    printf("------------------------------------------------------------\n");

    xTaskCreate(low_task,  "LOW",  2048, NULL, PRIO_LOW,  NULL);
    xTaskCreate(mid_task,  "MID",  2048, NULL, PRIO_MID,  NULL);
    xTaskCreate(high_task, "HIGH", 2048, NULL, PRIO_HIGH, NULL);

    /* Wait long enough for all tasks to finish */
    vTaskDelay(pdMS_TO_TICKS(6000));
    vSemaphoreDelete(resource);
    printf("\n[End of scenario: %s]\n", label);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Priority Inversion Demo ===");
    printf("Priority order: HIGH(%d) > MID(%d) > LOW(%d)\n\n",
           PRIO_HIGH, PRIO_MID, PRIO_LOW);
    printf("Expected with NO inversion: HIGH finishes before MID.\n");
    printf("Actual with Binary Semaphore: MID can finish before HIGH (INVERSION!).\n");
    printf("Actual with Mutex: FreeRTOS raises LOW prio → HIGH finishes first.\n\n");

    /* --- Scenario A: Binary Semaphore — NO priority inheritance --- */
    run_scenario("Binary Semaphore (no priority inheritance — inversion possible)",
                 xSemaphoreCreateBinary());
    /* pre-give the binary semaphore so LOW can take it */
    /* Note: binary sem starts empty; re-create and give */
    {
        SemaphoreHandle_t bs = xSemaphoreCreateBinary();
        xSemaphoreGive(bs);   /* make it available */
        run_scenario("Binary Semaphore (given, re-run — inversion visible)", bs);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    /* --- Scenario B: Mutex — WITH priority inheritance --- */
    run_scenario("Mutex (priority inheritance built-in — inversion resolved)",
                 xSemaphoreCreateMutex());

    ESP_LOGI(TAG, "\nKey takeaway:");
    ESP_LOGI(TAG, "  Always protect shared resources with xSemaphoreCreateMutex(),");
    ESP_LOGI(TAG, "  NOT xSemaphoreCreateBinary(), to get priority inheritance for free.");
}
