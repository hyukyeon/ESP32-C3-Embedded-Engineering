/*
 * 04_queues: FreeRTOS Queue — Safe Inter-Task Data Transfer
 *
 * A global shared variable is unsafe: if Task A reads it while Task B writes,
 * you get data corruption. FreeRTOS queues solve this by copying data atomically
 * into a kernel-managed FIFO buffer — neither sender nor receiver ever accesses
 * the same memory simultaneously.
 *
 * This demo uses a SensorData_t struct (id + value + timestamp) so that a single
 * Consumer task can service multiple sensor producers without knowing in advance
 * which sensor will send next.
 *
 * Key concepts shown:
 *   - Pass-by-copy semantics (struct is cloned into queue at send time)
 *   - Blocking with timeout (xTicksToWait != portMAX_DELAY)
 *   - Queue full detection (pdFALSE return from xQueueSend)
 *   - Queue depth monitoring (uxQueueMessagesWaiting)
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "QUEUE";

#define QUEUE_DEPTH  8   /* max pending items before sender blocks */

/* Sensor IDs */
#define SENSOR_TEMP    0
#define SENSOR_HUMID   1
#define SENSOR_PRESS   2

typedef struct {
    uint8_t  sensor_id;
    float    value;
    uint32_t timestamp_ms;   /* milliseconds since boot */
} SensorData_t;

static QueueHandle_t g_data_queue;

/* ---------- helpers ---------- */

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Simulate sensor reading with mild drift */
static float fake_read(uint8_t id, uint32_t iteration) {
    switch (id) {
    case SENSOR_TEMP:  return 25.0f + (iteration % 10) * 0.3f;
    case SENSOR_HUMID: return 60.0f - (iteration % 5)  * 1.2f;
    case SENSOR_PRESS: return 1013.0f + (iteration % 7) * 0.5f;
    default:           return 0.0f;
    }
}

/* ---------- producer tasks ---------- */

static void producer_task(void *arg) {
    uint8_t  id   = (uint8_t)(uintptr_t)arg;
    uint32_t iter = 0;
    uint32_t dropped = 0;

    static const char *names[] = {"Temp", "Humid", "Press"};
    static const TickType_t periods[] = {
        pdMS_TO_TICKS(500),   /* Temperature: 2 Hz */
        pdMS_TO_TICKS(1000),  /* Humidity:    1 Hz */
        pdMS_TO_TICKS(2000),  /* Pressure:  0.5 Hz */
    };

    ESP_LOGI(TAG, "[%s producer] started", names[id]);

    while (1) {
        iter++;
        SensorData_t pkt = {
            .sensor_id    = id,
            .value        = fake_read(id, iter),
            .timestamp_ms = now_ms(),
        };

        /*
         * Timeout of 50 ms: if the queue is full (consumer too slow),
         * we log a drop instead of blocking indefinitely.
         * This is safer than portMAX_DELAY in a real system.
         */
        if (xQueueSend(g_data_queue, &pkt, pdMS_TO_TICKS(50)) != pdTRUE) {
            dropped++;
            ESP_LOGW(TAG, "[%s] Queue FULL — packet dropped! (total drops: %" PRIu32 ")",
                     names[id], dropped);
        }

        vTaskDelay(periods[id]);
    }
}

/* ---------- consumer task ---------- */

static void consumer_task(void *arg) {
    SensorData_t rx;
    uint32_t     received[3] = {0};
    uint32_t     total       = 0;
    static const char *names[] = {"Temp  ", "Humid ", "Press "};
    static const char *units[] = {"°C", "%RH", "hPa"};

    ESP_LOGI(TAG, "[Consumer] waiting for sensor data...");

    while (1) {
        /*
         * Block up to 2 s waiting for any producer.
         * When data arrives, process it immediately.
         * This Blocked state costs zero CPU — the scheduler skips us entirely.
         */
        if (xQueueReceive(g_data_queue, &rx, pdMS_TO_TICKS(2000)) == pdTRUE) {
            total++;
            received[rx.sensor_id]++;

            printf("[Consumer] #%-4" PRIu32 "  %s id=%u  val=%.2f %s  t=%" PRIu32 " ms  queue_depth=%u\n",
                   total,
                   names[rx.sensor_id],
                   rx.sensor_id,
                   rx.value,
                   units[rx.sensor_id],
                   rx.timestamp_ms,
                   (unsigned)uxQueueMessagesWaiting(g_data_queue));
        } else {
            /* Timeout — no data for 2 s, print summary */
            printf("[Consumer] === Summary after %" PRIu32 " received ===\n", total);
            printf("  Temp:  %" PRIu32 "  Humid: %" PRIu32 "  Press: %" PRIu32 "\n",
                   received[SENSOR_TEMP], received[SENSOR_HUMID], received[SENSOR_PRESS]);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Queue Demo  (depth=%d, item_size=%d bytes) ===",
             QUEUE_DEPTH, (int)sizeof(SensorData_t));

    /*
     * xQueueCreate copies sizeof(SensorData_t) bytes per slot.
     * Sending modifies only the queue's internal buffer — the original
     * local variable in the producer is never touched again.
     */
    g_data_queue = xQueueCreate(QUEUE_DEPTH, sizeof(SensorData_t));
    if (g_data_queue == NULL) {
        ESP_LOGE(TAG, "Queue creation failed — out of memory");
        return;
    }

    /* Three producers at different rates, one consumer */
    xTaskCreate(producer_task, "Temp",     2048, (void*)SENSOR_TEMP,   4, NULL);
    xTaskCreate(producer_task, "Humid",    2048, (void*)SENSOR_HUMID,  4, NULL);
    xTaskCreate(producer_task, "Press",    2048, (void*)SENSOR_PRESS,  4, NULL);
    xTaskCreate(consumer_task, "Consumer", 3072, NULL,                  5, NULL);
}
