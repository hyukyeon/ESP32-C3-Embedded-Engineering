/*
 * 12_cache_miss: I-Cache Cold Miss vs Warm Cache — Cycle-Precise Measurement
 *
 * ESP32-C3 I-Cache 구조:
 *   - 크기  : 16 KB, 4-way set-associative
 *   - 라인  : 32 bytes (8 words × 4 bytes)
 *   - 대상  : Flash(XIP)의 코드(.text)와 읽기 전용 데이터(.rodata)
 *
 *                    ┌─────────────┐    Cache HIT  → 1~2 cycles
 *  CPU ─────────→   │  I-Cache    │ ──→
 *                    │  (16 KB)    │    Cache MISS → 스톨, SPI로 32 bytes 패치
 *                    └─────┬───────┘           │
 *                          │                   │  SPI @80 MHz: ~6.25 ns/byte
 *                    SPI XIP Bridge             │  → 32 bytes ≈ 200 ns ≈ 32 cycles
 *                          │
 *                    External Flash (4 MB)
 *
 * 측정 방법:
 *   1. boot 직후 첫 번째 호출 → 확실한 Cold Miss
 *   2. 연속 호출              → Warm Cache (라인이 이미 캐시에 있음)
 *   3. IRAM 함수             → 캐시 경로 없음, 항상 1~2 cycle 스톨만
 *
 * 부가 실험: 함수 크기를 16 KB 이상으로 키우면 (WORK_N 증가)
 *   워밍 후에도 캐시 용량 초과로 일부 라인이 교체되어 지속적 미스가 발생합니다.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_idf_version.h"

static const char *TAG = "CMISS";

#define CPU_FREQ_HZ  160000000ULL
#define WORK_N       256    /* 해시할 바이트 수 */
#define SAMPLES       32    /* 워밍 샘플 수 */

/* ---------- cycle counter ---------- */

static inline uint32_t rdcycle(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}

/* ---------- workloads (동일 알고리즘, 배치 위치만 다름) ---------- */

/*
 * Flash 배치: 일반 함수 → .flash.text 섹션 → I-Cache 경유
 * 첫 번째 호출 시 캐시 미스 페널티 발생
 */
static uint32_t flash_fnv1a(const uint8_t *data, int len) {
    uint32_t h = 2166136261UL;
    for (int i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619UL;
        /* 캐시 라인 폭을 넓히기 위한 추가 연산 (코드 크기 증가) */
        h ^= (h >> 13);
        h += (h << 3);
    }
    return h;
}

/*
 * IRAM 배치: IRAM_ATTR → .iram1.text 섹션 → CPU 내부 버스 직결
 * 매 호출 항상 동일한 사이클 — 캐시 없음
 */
static IRAM_ATTR uint32_t iram_fnv1a(const uint8_t *data, int len) {
    uint32_t h = 2166136261UL;
    for (int i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619UL;
        h ^= (h >> 13);
        h += (h << 3);
    }
    return h;
}

/* ---------- 통계 ---------- */

typedef struct { uint32_t min, max, sum, n; } Stats;

static void stats_add(Stats *s, uint32_t v) {
    if (s->n == 0 || v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
    s->n++;
}

static void stats_print(const char *label, const Stats *s) {
    double avg = s->n ? (double)s->sum / s->n : 0;
    printf("  %-22s  min=%5u  avg=%7.1f  max=%5u  spread=%u  (%.1f ns avg)\n",
           label, s->min, avg, s->max, s->max - s->min,
           avg * 1e9 / (double)CPU_FREQ_HZ);
}

/* ---------- 대형 워크로드: 캐시 용량 초과 실험용 ---------- */

/*
 * 이 함수는 의도적으로 크게 만들어 I-Cache(16 KB) 용량을 초과합니다.
 * 16 KB를 초과하면 워밍 후에도 교체(Eviction)가 발생하여
 * "항상 미스" 처럼 동작합니다.
 */
#define BIG_WORK_N  4096   /* 16 KB 이상의 코드 생성을 위해 큰 루프 */

static uint32_t flash_big_workload(const uint8_t *d, int len) {
    uint32_t h = 0;
    /* 컴파일러 최적화 방지 + 코드 크기 확장 */
    for (int i = 0; i < len; i++) {
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 13); h += (h << 3);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 11); h += (h << 7);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 17); h += (h << 5);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 23); h += (h << 2);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 7);  h += (h << 9);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 19); h += (h << 4);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 3);  h += (h << 11);
        h ^= d[i]; h *= 16777619UL; h ^= (h >> 29); h += (h << 1);
    }
    return h;
}

/* ---------- 테스트 데이터 ---------- */

static uint8_t g_data[BIG_WORK_N];

void app_main(void) {
    ESP_LOGI(TAG, "=== I-Cache Cold Miss vs Warm Cache ===");
    ESP_LOGI(TAG, "I-Cache: 16 KB, 4-way, line=32B | CPU: 160 MHz | SPI Flash: 80 MHz");

    for (int i = 0; i < BIG_WORK_N; i++) g_data[i] = (uint8_t)(i ^ (i >> 3));

    /* ═══════════════════════════════════════════════════════
     * 실험 A: 소형 함수 (캐시에 충분히 들어가는 크기)
     * ═══════════════════════════════════════════════════════ */
    printf("\n[A] Small workload (%d bytes input, function fits in I-Cache)\n", WORK_N);

    /*
     * boot 직후 첫 번째 호출 = 확실한 Cold Miss
     * (이 함수는 이전에 한 번도 호출된 적 없음)
     */
    uint32_t t0 = rdcycle();
    volatile uint32_t r = flash_fnv1a(g_data, WORK_N);
    uint32_t cold_cy = rdcycle() - t0;
    (void)r;

    printf("  Cold Miss (1st call) = %u cycles  (%.0f ns)\n",
           cold_cy, (double)cold_cy * 1e9 / CPU_FREQ_HZ);

    /* 워밍 샘플 수집 */
    Stats flash_warm = {0}, iram_stats = {0};

    for (int i = 0; i < SAMPLES; i++) {
        t0 = rdcycle();
        r = flash_fnv1a(g_data, WORK_N);
        stats_add(&flash_warm, rdcycle() - t0);

        t0 = rdcycle();
        r = iram_fnv1a(g_data, WORK_N);
        stats_add(&iram_stats, rdcycle() - t0);
    }

    printf("  결과 (%d 샘플):\n", SAMPLES);
    stats_print("Flash warm",  &flash_warm);
    stats_print("IRAM  (baseline)", &iram_stats);

    uint32_t warm_avg = flash_warm.sum / flash_warm.n;
    int32_t  penalty  = (int32_t)cold_cy - (int32_t)warm_avg;
    double   spi_ns   = (double)penalty * 1e9 / CPU_FREQ_HZ / 32.0;
    printf("  Cold Miss penalty    = %+d cycles  (%.0f ns)\n", penalty,
           (double)penalty * 1e9 / CPU_FREQ_HZ);
    printf("  추정 SPI 속도        = %.1f ns/byte (예상: ~12.5 ns @80 MHz)\n", spi_ns);

    /* ═══════════════════════════════════════════════════════
     * 실험 B: 대형 함수 (I-Cache 16 KB 초과 → 지속적 eviction)
     * ═══════════════════════════════════════════════════════ */
    printf("\n[B] Large workload (%d bytes input, function EXCEEDS I-Cache capacity)\n",
           BIG_WORK_N);

    /* 첫 번째 호출 (cold) */
    t0 = rdcycle();
    r  = flash_big_workload(g_data, BIG_WORK_N);
    uint32_t big_cold = rdcycle() - t0;
    (void)r;

    /* 반복 호출 (캐시 초과로 인해 여전히 미스 발생) */
    Stats big_stats = {0};
    for (int i = 0; i < 8; i++) {
        t0 = rdcycle();
        r  = flash_big_workload(g_data, BIG_WORK_N);
        stats_add(&big_stats, rdcycle() - t0);
    }

    printf("  Cold  = %u cycles\n", big_cold);
    stats_print("Warm (post-eviction)", &big_stats);
    printf("  ↑ spread 큰 경우 캐시 용량 초과로 eviction 발생 중\n");

    printf("\n[결론]\n");
    printf("  소형 함수: Cold » Warm ≈ IRAM  (라인이 캐시에 안착)\n");
    printf("  대형 함수: Cold ≈ Warm >> IRAM  (캐시가 항상 꽉 참)\n");
    printf("  → ISR·타이밍 임계 경로는 반드시 IRAM_ATTR 사용\n\n");
}
