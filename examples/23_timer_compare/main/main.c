/*
 * 23_timer_compare: esp_timer vs FreeRTOS 소프트웨어 타이머
 *
 * 두 타이머 시스템 비교:
 *
 *  ┌─────────────────┬──────────────────────────────┬───────────────────────┐
 *  │ 항목            │ esp_timer                    │ FreeRTOS xTimerCreate │
 *  ├─────────────────┼──────────────────────────────┼───────────────────────┤
 *  │ 분해능          │ 마이크로초 (μs)               │ 틱 (기본 1ms = 1틱)   │
 *  │ 내부 클럭       │ APB/XTAL 하드웨어 타이머      │ FreeRTOS 틱 카운터    │
 *  │ 콜백 컨텍스트   │ 전용 태스크 또는 ISR          │ Timer Daemon 태스크   │
 *  │ 최소 주기       │ ~50 μs                       │ 1 틱 (1 ms)           │
 *  │ 64비트 타임스탬프│ esp_timer_get_time() [μs]    │ xTaskGetTickCount()   │
 *  │ 원샷/반복       │ 모두 지원                     │ 모두 지원             │
 *  └─────────────────┴──────────────────────────────┴───────────────────────┘
 *
 * esp_timer ISR 모드 (ESP_TIMER_ISR):
 *   콜백이 ISR에서 직접 실행 → 레이턴시 최소, 처리 시간 제한적
 *   esp_timer_create() 시 .dispatch_method = ESP_TIMER_ISR
 *
 * esp_timer TASK 모드 (ESP_TIMER_TASK):
 *   콜백이 esp_timer 전용 태스크에서 실행 → 긴 처리 가능
 *
 * 이 예제 측정 방법:
 *   esp_timer_get_time() [μs] 으로 콜백 호출 간격의 실제 드리프트/지터 측정
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "TIMER_CMP";

#define SAMPLE_N       20          /* 측정 횟수 */
#define ESP_PERIOD_US  10000       /* esp_timer 주기: 10 ms */
#define RTOS_PERIOD_MS 10          /* FreeRTOS 타이머 주기: 10 ms */

/* ---------- 통계 ---------- */

typedef struct {
    int64_t  samples[SAMPLE_N];   /* 실제 호출 간격 (μs) */
    int      count;
    int64_t  sum;
    int64_t  min_val;
    int64_t  max_val;
} TimerStat_t;

static TimerStat_t g_esp_stat;
static TimerStat_t g_rtos_stat;

static void stat_record(TimerStat_t *s, int64_t interval_us) {
    if (s->count >= SAMPLE_N) return;
    s->samples[s->count++] = interval_us;
    s->sum += interval_us;
    if (s->count == 1 || interval_us < s->min_val) s->min_val = interval_us;
    if (s->count == 1 || interval_us > s->max_val) s->max_val = interval_us;
}

static void stat_print(const char *label, const TimerStat_t *s, int64_t nominal_us) {
    if (s->count == 0) { printf("  %s: 데이터 없음\n", label); return; }
    int64_t avg = s->sum / s->count;
    int64_t drift_avg = avg - nominal_us;

    printf("  [%s]  공칭=%lld μs  평균=%lld μs  드리프트=%+lld μs\n",
           label, (long long)nominal_us, (long long)avg, (long long)drift_avg);
    printf("          min=%lld μs  max=%lld μs  지터=%lld μs\n",
           (long long)s->min_val,
           (long long)s->max_val,
           (long long)(s->max_val - s->min_val));

    /* ASCII 히스토그램 (드리프트 분포) */
    printf("          [드리프트 분포] ");
    int64_t lo = s->min_val - nominal_us;
    int64_t hi = s->max_val - nominal_us;
    int64_t range = (hi > lo) ? (hi - lo) : 1;
    const int WIDTH = 20;
    char bar[22];
    for (int i = 0; i <= WIDTH; i++) bar[i] = '-';
    bar[WIDTH + 1] = '\0';
    for (int i = 0; i < s->count; i++) {
        int64_t d = s->samples[i] - nominal_us;
        int pos = (int)((d - lo) * WIDTH / range);
        if (pos >= 0 && pos <= WIDTH) bar[pos] = '*';
    }
    printf("|%s|  %+lld~%+lld μs\n", bar, (long long)lo, (long long)hi);
}

/* ---------- esp_timer 콜백 ---------- */

static int64_t g_esp_prev_us = 0;

static void esp_timer_cb(void *arg) {
    int64_t now = esp_timer_get_time();
    if (g_esp_prev_us != 0)
        stat_record(&g_esp_stat, now - g_esp_prev_us);
    g_esp_prev_us = now;
}

/* ---------- FreeRTOS 타이머 콜백 ---------- */

static int64_t g_rtos_prev_us = 0;

static void rtos_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    int64_t now = esp_timer_get_time();  /* μs 기준 비교 */
    if (g_rtos_prev_us != 0)
        stat_record(&g_rtos_stat, now - g_rtos_prev_us);
    g_rtos_prev_us = now;
}

/* ---------- 원샷 타이머 레이턴시 측정 ---------- */

static volatile int64_t g_oneshot_start = 0;
static volatile int64_t g_oneshot_fired = 0;

static void oneshot_cb(void *arg) {
    g_oneshot_fired = esp_timer_get_time();
}

static void measure_oneshot_latency(void) {
    printf("\n[원샷 타이머 레이턴시 (요청 지연 vs 실제 발사)]\n");

    esp_timer_handle_t h;
    esp_timer_create_args_t args = {
        .callback        = oneshot_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "oneshot",
    };
    esp_timer_create(&args, &h);

    for (int delay_us = 1000; delay_us <= 100000; delay_us *= 10) {
        g_oneshot_fired = 0;
        g_oneshot_start = esp_timer_get_time();
        esp_timer_start_once(h, delay_us);

        while (g_oneshot_fired == 0) taskYIELD();
        int64_t actual = g_oneshot_fired - g_oneshot_start;
        printf("  요청 %6d μs → 실제 %6lld μs  오차 %+lld μs\n",
               delay_us, (long long)actual, (long long)(actual - delay_us));
    }
    esp_timer_delete(h);
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== esp_timer vs FreeRTOS Timer Compare ===");

    memset(&g_esp_stat,  0, sizeof(g_esp_stat));
    memset(&g_rtos_stat, 0, sizeof(g_rtos_stat));

    /* ── esp_timer (TASK 모드) 생성 ── */
    esp_timer_handle_t esp_h;
    esp_timer_create_args_t esp_args = {
        .callback        = esp_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "esp_periodic",
    };
    esp_timer_create(&esp_args, &esp_h);
    esp_timer_start_periodic(esp_h, ESP_PERIOD_US);

    /* ── FreeRTOS 소프트웨어 타이머 생성 ── */
    TimerHandle_t rtos_h = xTimerCreate(
        "rtos_periodic",
        pdMS_TO_TICKS(RTOS_PERIOD_MS),
        pdTRUE,   /* auto-reload */
        NULL,
        rtos_timer_cb);
    xTimerStart(rtos_h, portMAX_DELAY);

    /* SAMPLE_N 샘플 수집 대기 */
    printf("  %d ms 주기 타이머 %d 샘플 수집 중...\n",
           RTOS_PERIOD_MS, SAMPLE_N);
    while (g_esp_stat.count < SAMPLE_N || g_rtos_stat.count < SAMPLE_N)
        vTaskDelay(pdMS_TO_TICKS(50));

    /* 타이머 정지 */
    esp_timer_stop(esp_h);
    xTimerStop(rtos_h, portMAX_DELAY);

    /* 결과 출력 */
    printf("\n[주기 타이머 정밀도 비교 (공칭 10 ms = 10000 μs)]\n");
    stat_print("esp_timer (TASK)", &g_esp_stat,  (int64_t)ESP_PERIOD_US);
    stat_print("FreeRTOS xTimer", &g_rtos_stat, (int64_t)RTOS_PERIOD_MS * 1000);

    /* 원샷 레이턴시 */
    measure_oneshot_latency();

    /* 정리 */
    esp_timer_delete(esp_h);
    xTimerDelete(rtos_h, portMAX_DELAY);

    printf("\n[타이머 선택 기준]\n");
    printf("  정밀도 필요 (< 1ms): esp_timer (ISR 모드)\n");
    printf("  긴 처리 허용:        esp_timer (TASK 모드)\n");
    printf("  틱 단위 충분, 간단: FreeRTOS xTimerCreate\n");
    printf("  esp_timer_get_time(): 64비트 μs 타임스탬프 기준점\n\n");
}
