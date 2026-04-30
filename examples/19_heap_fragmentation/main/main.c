/*
 * 19_heap_fragmentation: 힙 단편화 — 분석과 방지
 *
 * 힙 단편화(Heap Fragmentation): 총 여유 메모리는 충분하지만
 * 연속된 대형 블록은 할당할 수 없는 상태.
 *
 *  단편화 전:  [FFFF][FFFF][FFFF][FFFF]  ← 연속 16KB
 *  소블록 할당: [AAAA][BBBB][CCCC][DDDD]
 *  A/C 해제:   [FREE][BBBB][FREE][DDDD]  ← 비연속 여유
 *  8KB 요청:   실패! 각 FREE는 4KB뿐
 *
 * ESP32-C3 힙 영역:
 *   DRAM  (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)  : ~320 KB
 *   IRAM  (MALLOC_CAP_IRAM_8BIT)                   : ~128 KB (코드 미사용 영역)
 *   DMA   (MALLOC_CAP_DMA)                         : DRAM 내 DMA 가능 영역
 *
 * 핵심 API:
 *   heap_caps_get_free_size()         — 총 여유 바이트
 *   heap_caps_get_largest_free_block() — 실제 할당 가능한 최대 크기 ← 핵심
 *   heap_caps_get_minimum_free_size()  — 역대 최소 여유 (고수위 표시)
 *   heap_caps_dump()                  — 힙 맵 상세 출력
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "HEAP_FRAG";

#define SMALL_N     48          /* 소블록 개수 */
#define SMALL_SZ    128         /* 소블록 크기 (bytes) */
#define LARGE_SZ    (6 * 1024)  /* 대형 블록 요청 크기 */

/* ---------- 힙 상태 출력 ---------- */

static void heap_stat(const char *label) {
    size_t total  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t maxblk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t minev  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    float  frag   = (total > 0) ? (1.0f - (float)maxblk / total) * 100.0f : 0.0f;

    printf("  [%-22s]  total=%6u  max_block=%6u  min_ever=%6u  frag=%.0f%%\n",
           label,
           (unsigned)total,
           (unsigned)maxblk,
           (unsigned)minev,
           frag);
}

/* ---------- 의도적 단편화 ---------- */

static void *ptrs[SMALL_N];

static void create_fragmentation(void) {
    printf("\n[단계 1] %d × %d B 소블록 전량 할당\n", SMALL_N, SMALL_SZ);
    int ok = 0;
    for (int i = 0; i < SMALL_N; i++) {
        ptrs[i] = heap_caps_malloc(SMALL_SZ, MALLOC_CAP_8BIT);
        if (ptrs[i]) { memset(ptrs[i], (uint8_t)i, SMALL_SZ); ok++; }
    }
    printf("  할당 성공: %d / %d\n", ok, SMALL_N);
    heap_stat("전량 할당 후");

    printf("\n[단계 2] 짝수 인덱스(%d개) 해제 → 체스판 패턴 단편화\n", SMALL_N / 2);
    for (int i = 0; i < SMALL_N; i += 2) {
        heap_caps_free(ptrs[i]);
        ptrs[i] = NULL;
    }
    heap_stat("체스판 해제 후");
}

/* ---------- 대형 블록 할당 시도 ---------- */

static int try_large_alloc(const char *label) {
    size_t maxblk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    void  *p      = heap_caps_malloc(LARGE_SZ, MALLOC_CAP_8BIT);
    printf("  %s: %u B 요청 → %s  (최대 연속=%u B)\n",
           label,
           (unsigned)LARGE_SZ,
           p ? "성공" : "실패",
           (unsigned)maxblk);
    if (p) heap_caps_free(p);
    return (p != NULL);
}

/* ---------- 나머지 블록 정리 ---------- */

static void cleanup(void) {
    for (int i = 0; i < SMALL_N; i++) {
        if (ptrs[i]) { heap_caps_free(ptrs[i]); ptrs[i] = NULL; }
    }
}

/* ---------- 메모리 풀 패턴 ---------- */

/*
 * 단편화 방지: 시작 시 큰 블록 하나를 미리 확보하고 내부에서 슬롯 재활용.
 * 힙 레벨의 alloc/free가 없으므로 단편화 발생 없음.
 */
#define POOL_SLOT_SZ   128
#define POOL_SLOT_N    32

typedef struct {
    uint8_t  data[POOL_SLOT_SZ * POOL_SLOT_N];
    uint32_t used;   /* 비트맵 (1=사용 중) */
} Pool_t;

static Pool_t *g_pool;

static void *pool_alloc(void) {
    for (int i = 0; i < POOL_SLOT_N; i++) {
        if (!(g_pool->used & (1u << i))) {
            g_pool->used |= (1u << i);
            return g_pool->data + i * POOL_SLOT_SZ;
        }
    }
    return NULL;
}

static void pool_free(void *p) {
    int slot = ((uint8_t *)p - g_pool->data) / POOL_SLOT_SZ;
    if (slot >= 0 && slot < POOL_SLOT_N)
        g_pool->used &= ~(1u << slot);
}

static void demo_pool(void) {
    printf("\n[메모리 풀 패턴]\n");
    g_pool = (Pool_t *)heap_caps_malloc(sizeof(Pool_t), MALLOC_CAP_8BIT);
    if (!g_pool) { printf("  풀 할당 실패\n"); return; }
    g_pool->used = 0;

    heap_stat("풀 할당 후 (힙 변화 1회)");

    /* 풀 내부에서 반복 alloc/free → 힙 레벨 조각 없음 */
    void *s[8];
    for (int i = 0; i < 8; i++) s[i] = pool_alloc();
    for (int i = 0; i < 8; i += 2) pool_free(s[i]);
    for (int i = 0; i < 4; i++) s[i] = pool_alloc();  /* 재활용 */

    heap_stat("풀 내부 조작 후 (힙 변화 없음)");
    printf("  → 힙 max_block 변동 없음: 풀이 단편화 흡수\n");

    for (int i = 0; i < 8; i++) pool_free(s[i]);
    heap_caps_free(g_pool);
    g_pool = NULL;
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== Heap Fragmentation Demo ===");

    /* 0. 초기 상태 */
    printf("\n[0] 초기 힙 상태\n");
    heap_stat("초기");

    /* IRAM / DMA 힙은 별도 공간 */
    printf("  IRAM  여유: %u B  (MALLOC_CAP_IRAM_8BIT)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT));
    printf("  DMA   여유: %u B  (MALLOC_CAP_DMA)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));

    /* 1~2. 단편화 생성 */
    create_fragmentation();

    /* 3. 단편화 상태에서 대형 블록 시도 */
    printf("\n[단계 3] 단편화 상태에서 대형 블록(%u B) 할당\n", (unsigned)LARGE_SZ);
    try_large_alloc("단편화 후");

    /* 4. 힙 맵 상세 덤프 */
    printf("\n[단계 4] 힙 맵 덤프 (heap_caps_dump)\n");
    heap_caps_dump(MALLOC_CAP_8BIT);

    /* 5. 전체 해제 후 재시도 */
    printf("\n[단계 5] 모든 소블록 해제 후 재시도\n");
    cleanup();
    heap_stat("전체 해제 후");
    try_large_alloc("해제 후");

    /* 6. 메모리 풀 패턴 */
    demo_pool();

    printf("\n[힙 단편화 핵심 원칙]\n");
    printf("  1. 총 여유 ≠ 할당 가능 최대 크기\n");
    printf("  2. heap_caps_get_largest_free_block()이 실제 할당 한계\n");
    printf("  3. 장기 실행 시스템 → 메모리 풀로 단편화 원천 방지\n");
    printf("  4. IRAM / DMA 힙은 DRAM과 완전 분리된 별도 공간\n\n");
}
