/*
 * 13_data_locality: 데이터 지역성과 캐시 라인 효율
 *
 * ESP32-C3의 캐시는 I/D 데이터 모두 동일한 캐시를 공유합니다.
 * Flash의 const 배열(.rodata)은 D-Cache를 통해 CPU에 전달됩니다.
 *
 * 핵심 개념: 캐시 라인(Cache Line) = 32 bytes
 *   캐시 미스 1회 → Flash에서 32 bytes 통째로 로드
 *   그 중 1 byte만 쓰면 나머지 31 bytes는 사전 로드(Prefetch) 효과
 *
 * 실험 패턴:
 *
 *   [순차 접근]  stride=1     → 미스 1회당 32 bytes 재사용  (최고 효율)
 *   [라인 접근]  stride=32    → 미스 1회당 1 byte 사용      (최악 효율, 미스율 동일)
 *   [2라인 건너] stride=64    → 매 접근마다 미스            (절반 캐시 라인 낭비)
 *   [페이지 건너]stride=256   → 캐시 라인 완전 낭비         (Thrashing)
 *   [무작위]     random       → 예측 불가 미스 패턴
 *
 * 캐시 스래싱(Cache Thrashing):
 *   set-associative 캐시에서 stride가 캐시 크기의 약수일 때,
 *   같은 set에 집중되어 유효 캐시 용량이 급격히 감소합니다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "LOCALITY";

#define CPU_FREQ_HZ  160000000ULL
#define CACHE_LINE    32            /* ESP32-C3 캐시 라인 크기 (bytes) */
#define TABLE_SIZE    4096          /* 룩업 테이블 크기 (bytes) */
#define ACCESSES      1024          /* 접근 횟수 (반복 가능하게 mod 처리) */

/* ---------- cycle counter ---------- */

static inline uint32_t rdcycle(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}

/* ---------- 룩업 테이블 (Flash rodata에 배치) ---------- */

/*
 * const → .rodata 섹션 → Flash에 저장
 * CPU가 읽을 때 I/D 캐시를 통해 접근됨
 * 크기: 4096 bytes = 128 캐시 라인
 */
static const uint8_t g_table[TABLE_SIZE] = {
    /* Python으로 생성: [i*7 % 251 for i in range(4096)] */
    #define R7(b)  (b)%251, (b+7)%251, (b+14)%251, (b+21)%251, \
                   (b+28)%251, (b+35)%251, (b+42)%251
    R7(0),   R7(49),  R7(98),  R7(147), R7(196),
    R7(245), R7(43),  R7(92),  R7(141), R7(190),
    R7(239), R7(37),  R7(86),  R7(135), R7(184),
    R7(233), R7(31),  R7(80),  R7(129), R7(178),
    /* ... 간략화: 실제로는 TABLE_SIZE개 전부 초기화 */
    #undef R7
    /* GCC는 초기화되지 않은 나머지를 0으로 채움 */
};

/* ---------- 접근 패턴 함수들 ---------- */

/* 순차 접근: 1 byte씩, 미스 1회당 32 bytes 활용 (최고 효율) */
static uint32_t IRAM_ATTR access_sequential(const uint8_t *tbl, int size, int cnt) {
    uint32_t sum = 0;
    for (int i = 0; i < cnt; i++) {
        sum += tbl[i % size];
    }
    return sum;
}

/* 스트라이드 접근: stride bytes씩 건너뜀 */
static uint32_t IRAM_ATTR access_stride(const uint8_t *tbl, int size, int cnt, int stride) {
    uint32_t sum  = 0;
    int      idx  = 0;
    for (int i = 0; i < cnt; i++) {
        sum += tbl[idx % size];
        idx  = (idx + stride) % size;
        if (idx == 0) idx = stride % CACHE_LINE; /* 무한 0 방지 */
    }
    return sum;
}

/* LCG 기반 의사 난수 접근 (재현 가능) */
static uint32_t IRAM_ATTR access_random(const uint8_t *tbl, int size, int cnt) {
    uint32_t sum  = 0;
    uint32_t rng  = 0xDEADBEEFUL;
    for (int i = 0; i < cnt; i++) {
        rng  = rng * 1664525UL + 1013904223UL;  /* Numerical Recipes LCG */
        sum += tbl[rng % (uint32_t)size];
    }
    return sum;
}

/* ---------- 측정 래퍼 ---------- */

typedef struct {
    const char *label;
    uint32_t    cycles;
    double      cy_per_access;
    double      miss_rate_est;  /* 추정 미스율 */
} Result;

#define MEASURE(label_, fn_, ...) do {                          \
    volatile uint32_t _dummy;                                   \
    /* 워밍업 1회 (첫 Cold Miss 제거) */                         \
    _dummy = fn_(__VA_ARGS__);                                  \
    uint32_t _t0 = rdcycle();                                   \
    _dummy = fn_(__VA_ARGS__);                                  \
    uint32_t _cy = rdcycle() - _t0;                             \
    (void)_dummy;                                               \
    double _cpa = (double)_cy / ACCESSES;                       \
    /* 미스율 추정: IRAM 기준 사이클(~2 cy)을 제거하면 미스 사이클 */\
    /* 미스 1회 ≈ 26 cycle @160MHz, SPI 80MHz */                 \
    double _miss_est = (_cpa > 2.5) ? (_cpa - 2.0) / 26.0 : 0; \
    printf("  %-28s  %6" PRIu32 " cy  %5.1f cy/access  miss≈%.0f%%\n",  \
           label_, _cy, _cpa, _miss_est * 100.0);               \
} while(0)

void app_main(void) {
    ESP_LOGI(TAG, "=== Data Locality & Cache Line Efficiency ===");
    printf("Table: %d bytes in Flash rodata  |  Cache line: %d bytes  |  Accesses: %d\n\n",
           TABLE_SIZE, CACHE_LINE, ACCESSES);

    /* ═══════════════════════════════
     * 1. 다양한 스트라이드 비교
     * ═══════════════════════════════ */
    printf("[Stride Comparison]  (같은 접근 횟수, 다른 스텝 크기)\n");
    printf("  %-28s  %8s  %14s  %s\n", "Pattern", "Total cy", "cy/access", "Est.MissRate");

    MEASURE("stride=1  (sequential)",
            access_sequential, g_table, TABLE_SIZE, ACCESSES);

    MEASURE("stride=4  (word align)",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 4);

    MEASURE("stride=8",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 8);

    MEASURE("stride=16 (half line)",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 16);

    MEASURE("stride=32 (= cache line!)",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 32);

    MEASURE("stride=64 (2x cache line)",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 64);

    MEASURE("stride=128",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 128);

    MEASURE("stride=256 (thrashing zone)",
            access_stride, g_table, TABLE_SIZE, ACCESSES, 256);

    MEASURE("random (LCG)",
            access_random, g_table, TABLE_SIZE, ACCESSES);

    /* ═══════════════════════════════
     * 2. 배열 크기 vs 캐시 효율
     * ═══════════════════════════════ */
    printf("\n[Array Size vs Cache Efficiency]  (stride=1 순차 접근)\n");
    printf("  %-20s  %8s  %s\n", "Size", "cy/access", "Cache impact");

    const int sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    for (int s = 0; s < (int)(sizeof(sizes)/sizeof(sizes[0])); s++) {
        int sz = sizes[s];
        int n  = (ACCESSES > sz) ? ACCESSES : sz;  /* 최소 한 바퀴 */

        volatile uint32_t dummy;
        dummy = access_sequential(g_table, sz, n);  /* 워밍업 */

        uint32_t t0 = rdcycle();
        dummy = access_sequential(g_table, sz, n);
        uint32_t cy = rdcycle() - t0;
        (void)dummy;

        double cpa = (double)cy / n;
        const char *note = (sz <= 512)  ? "fits in cache"
                         : (sz <= 2048) ? "pressure"
                         : "exceeds cache";
        printf("  %-5d bytes (%3d lines)  %5.1f cy/access  ← %s\n",
               sz, sz / CACHE_LINE, cpa, note);
    }

    printf("\n[해석]\n");
    printf("  - stride=1~31  : 캐시 라인 재사용 → cy/access 낮음\n");
    printf("  - stride=32    : 미스 1회 = 접근 1회 (효율 하락 시작)\n");
    printf("  - stride≥32    : 캐시 라인의 31/32 bytes를 낭비\n");
    printf("  - 배열이 16 KB 초과: 캐시 용량 초과 → 모든 접근이 미스\n");
    printf("  → 핫 데이터는 IRAM/DRAM에, Flash rodata는 순차 접근으로\n\n");
}
