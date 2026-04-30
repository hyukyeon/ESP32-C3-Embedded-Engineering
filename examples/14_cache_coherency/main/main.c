/*
 * 14_cache_coherency: 캐시 일관성 문제와 해결 (Cache Coherency)
 *
 * 문제 시나리오:
 *   CPU가 Flash 주소 X를 읽으면 → 캐시 라인(32 bytes)이 I/D 캐시로 로드됨
 *   이후 Flash의 물리 주소 X가 변경되어도 → 캐시 라인은 갱신되지 않음
 *   CPU가 다시 X를 읽으면 → 오래된 데이터(Stale Data) 반환
 *
 *   ┌─────────┐ read X   ┌─────────────┐   초기 값 A
 *   │   CPU   │ ──────→  │  I/D Cache  │ ←── Flash [X]
 *   └────┬────┘          └─────────────┘
 *        │  read X again      ↑ 여기는 A 그대로
 *        │               Flash [X] = B  (수정됨)
 *        └──────────────────────────────
 *          CPU는 여전히 A를 읽음 (Stale!)
 *
 * 해결책: 캐시 무효화(Cache Invalidation)
 *   esp_cache_msync(ptr, size, INVALIDATE) → 해당 캐시 라인을 비워 다음 접근 시
 *   Flash에서 강제 재로드.
 *
 * 실습 구성:
 *   1. NVS 파티션을 spi_flash_mmap()으로 가상 주소에 매핑
 *   2. 해당 주소 읽기 (캐시 워밍)
 *   3. spi_flash_write()로 같은 물리 주소에 다른 패턴 기록
 *      → Flash 내용이 바뀌었지만 캐시는 갱신 안 됨
 *   4. mmap 포인터로 다시 읽기 → Stale Data 관찰
 *   5. esp_cache_msync() 캐시 무효화
 *   6. 다시 읽기 → 최신 데이터 확인
 *
 * NOTE: ESP-IDF v5.0 이상에서 spi_flash_write 내부적으로 일부 무효화가
 *       수행될 수 있어 완전한 Stale 현상이 재현되지 않을 수 있습니다.
 *       이 경우 "이미 IDF가 처리 중" 메시지가 출력됩니다.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_idf_version.h"

/* esp_cache_msync: ESP-IDF v5.2+에서 공식 지원 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#  include "esp_cache.h"
#  define HAS_CACHE_MSYNC 1
#else
#  define HAS_CACHE_MSYNC 0
#endif

static const char *TAG = "COHERENCY";

#define TEST_OFFSET   0x1000   /* 파티션 내 테스트 오프셋 (4KB 정렬) */
#define TEST_SIZE     64       /* 읽기/쓰기 크기 (bytes) */

/* ---------- 도우미 함수 ---------- */

static void dump_hex(const char *label, const uint8_t *buf, int len) {
    printf("  %s: ", label);
    for (int i = 0; i < len && i < 16; i++) printf("%02X ", buf[i]);
    if (len > 16) printf("...");
    printf("\n");
}

/* ---------- 캐시 무효화 ---------- */

static void invalidate_cache_region(const void *ptr, size_t size) {
#if HAS_CACHE_MSYNC
    /*
     * ESP_CACHE_MSYNC_FLAG_INVALIDATE: 캐시 라인을 비워 다음 접근 시
     * Flash에서 강제 재로드하도록 마킹
     */
    esp_err_t err = esp_cache_msync((void *)ptr, size,
                                    ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA  |
                                    ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "esp_cache_msync(INVALIDATE): OK — 캐시 라인 무효화 완료");
    } else {
        ESP_LOGW(TAG, "esp_cache_msync 실패: %s", esp_err_to_name(err));
    }
#else
    /*
     * ESP-IDF v4.x: ROM 내부 함수 직접 호출 (비공개 API)
     * extern uint32_t Cache_Disable_ICache(void);
     * extern void Cache_Resume_ICache(uint32_t autoload);
     * Cache_Disable_ICache();  Cache_Resume_ICache(0);
     */
    ESP_LOGW(TAG, "esp_cache_msync 없음 (IDF < 5.2). ROM API 필요.");
    (void)ptr; (void)size;
#endif
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== Cache Coherency Demo ===");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    /* ──────────────────────────────
     * 1. 파티션 찾기
     * ────────────────────────────── */
    /*
     * nvs 파티션을 사용하되, TEST_OFFSET 위치에 임시로 데이터를 씀
     * 주의: NVS 운용 중에는 충돌 가능 — 실제 프로젝트에서는 전용 파티션 사용
     */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);

    if (!part) {
        ESP_LOGE(TAG, "NVS 파티션 없음. 파티션 테이블 확인 필요.");
        return;
    }
    ESP_LOGI(TAG, "파티션: '%s'  flash_offset=0x%08X  size=%u bytes",
             part->label, (unsigned)part->address, (unsigned)part->size);

    if (part->size < TEST_OFFSET + TEST_SIZE) {
        ESP_LOGE(TAG, "파티션이 너무 작음");
        return;
    }

    /* ──────────────────────────────
     * 2. 가상 주소에 파티션 매핑 (mmap)
     * ────────────────────────────── */
    esp_partition_mmap_handle_t mmap_handle;
    const void *mapped_ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &mapped_ptr, &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap 실패: %s", esp_err_to_name(err));
        return;
    }
    const uint8_t *test_virt = (const uint8_t *)mapped_ptr + TEST_OFFSET;
    ESP_LOGI(TAG, "mmap 성공: 가상 주소 = %p", (void *)test_virt);

    /* ──────────────────────────────
     * 3. 패턴 A 기록 + 캐시 워밍
     * ────────────────────────────── */
    uint8_t pattern_a[TEST_SIZE];
    uint8_t pattern_b[TEST_SIZE];
    memset(pattern_a, 0xAA, TEST_SIZE);
    memset(pattern_b, 0xBB, TEST_SIZE);

    /* Flash를 지우고 패턴 A 기록 */
    esp_partition_erase_range(part, TEST_OFFSET, TEST_SIZE);
    esp_partition_write(part, TEST_OFFSET, pattern_a, TEST_SIZE);
    ESP_LOGI(TAG, "Flash[0x%X]: 패턴 A(0xAA) 기록 완료", TEST_OFFSET);

    /* mmap 포인터로 읽어 캐시 워밍 */
    uint8_t read_buf[TEST_SIZE];
    memcpy(read_buf, test_virt, TEST_SIZE);
    dump_hex("캐시 워밍 후 read", read_buf, TEST_SIZE);

    /* ──────────────────────────────
     * 4. Flash 내용 변경 (캐시 무효화 없이)
     * ────────────────────────────── */
    ESP_LOGI(TAG, "\n--- Flash를 패턴 B(0xBB)로 덮어씀 ---");
    esp_partition_erase_range(part, TEST_OFFSET, TEST_SIZE);
    esp_partition_write(part, TEST_OFFSET, pattern_b, TEST_SIZE);
    ESP_LOGI(TAG, "Flash[0x%X]: 패턴 B(0xBB) 기록 완료", TEST_OFFSET);

    /* ──────────────────────────────
     * 5. 캐시 무효화 전 읽기 → Stale 가능성
     * ────────────────────────────── */
    memcpy(read_buf, test_virt, TEST_SIZE);
    dump_hex("무효화 전 read (Stale?)", read_buf, TEST_SIZE);

    if (read_buf[0] == 0xAA) {
        ESP_LOGW(TAG, ">>> Stale Data 확인! 캐시에 오래된 값(0xAA)이 남아 있음.");
    } else if (read_buf[0] == 0xBB) {
        ESP_LOGI(TAG, "IDF 내부에서 이미 캐시 무효화 수행됨 (v5.x 이후 동작).");
    } else {
        ESP_LOGW(TAG, "예상치 못한 값: 0x%02X", read_buf[0]);
    }

    /* ──────────────────────────────
     * 6. 캐시 무효화 후 읽기 → 최신 데이터
     * ────────────────────────────── */
    printf("\n--- esp_cache_msync(INVALIDATE) 호출 ---\n");
    invalidate_cache_region(test_virt, TEST_SIZE);

    memcpy(read_buf, test_virt, TEST_SIZE);
    dump_hex("무효화 후  read (Fresh)", read_buf, TEST_SIZE);

    if (read_buf[0] == 0xBB) {
        ESP_LOGI(TAG, ">>> 캐시 무효화 성공! 최신 값(0xBB) 읽힘.");
    } else {
        ESP_LOGW(TAG, "예상과 다른 값: 0x%02X", read_buf[0]);
    }

    /* ──────────────────────────────
     * 7. 정리: 패턴 A로 복원
     * ────────────────────────────── */
    esp_partition_erase_range(part, TEST_OFFSET, TEST_SIZE);
    esp_partition_write(part, TEST_OFFSET, pattern_a, TEST_SIZE);
    esp_partition_munmap(mmap_handle);
    ESP_LOGI(TAG, "\n파티션 복원 완료.");

    printf("\n[캐시 일관성 핵심 원칙]\n");
    printf("  1. mmap 포인터는 캐시를 통해 Flash를 읽음\n");
    printf("  2. esp_partition_write 후 캐시 라인은 자동으로 무효화되지 않을 수 있음\n");
    printf("  3. Flash 쓰기 후 mmap 읽기 전에 반드시 esp_cache_msync(INVALIDATE) 호출\n");
    printf("  4. spi_flash_write 직접 호출 시 더욱 주의 필요\n\n");
}
