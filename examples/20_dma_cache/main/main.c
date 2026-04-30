/*
 * 20_dma_cache: DMA 동작과 캐시 일관성
 *
 * DMA(Direct Memory Access)는 CPU를 우회하여 주변장치 ↔ SRAM 간
 * 데이터를 직접 전송합니다.
 *
 * 캐시 일관성 문제 발생 경로:
 *
 *  TX (CPU → DMA → 주변장치):
 *    CPU가 버퍼에 쓴다 → CPU D-Cache에 dirty로 기록
 *    DMA가 SRAM을 읽는다 → 아직 플러시 안 된 구 데이터 전송
 *    해결: DMA 시작 전 WRITEBACK (캐시 → SRAM 플러시)
 *
 *  RX (주변장치 → DMA → SRAM):
 *    DMA가 SRAM에 새 데이터를 씀 → CPU 캐시는 여전히 구 값
 *    CPU가 버퍼를 읽는다 → 스테일 캐시 값 반환
 *    해결: DMA 완료 후 INVALIDATE (캐시 라인 무효화)
 *
 * ESP32-C3 특이사항:
 *   내부 SRAM(0x3FC80000)은 write-back 캐시를 거치지 않습니다.
 *   따라서 내부 SRAM용 DMA 버퍼에서 캐시 일관성 문제는 실제 발생하지
 *   않습니다. 그러나 PSRAM(외부 RAM)을 사용하는 칩(ESP32-S3 등)에서는
 *   캐시 동기화가 반드시 필요합니다.
 *   → esp_cache_msync 호출은 이식성을 위한 올바른 패턴입니다.
 *
 * DMA 버퍼 필수 요구사항:
 *   1. MALLOC_CAP_DMA 로 힙에서 할당 (스택/rodata 불가)
 *   2. 4바이트 정렬 (SPI DMA 요구)
 *   3. 32바이트 정렬 권장 (캐시 라인 단위, partial-flush 방지)
 *
 * 루프백 테스트 배선:
 *   GPIO7(MOSI) ↔ GPIO2(MISO) 점퍼 연결
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "driver/spi_master.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#  include "esp_cache.h"
#  define HAS_CACHE_MSYNC 1
#else
#  define HAS_CACHE_MSYNC 0
#endif

static const char *TAG = "DMA_CACHE";

#define PIN_MOSI   7
#define PIN_MISO   2
#define PIN_SCLK   6
#define PIN_CS    10
#define SPI_HZ     4000000
#define BUF_SZ     64

/* ---------- 캐시 동기화 헬퍼 ---------- */

static void dma_pre_tx_writeback(void *ptr, size_t size) {
#if HAS_CACHE_MSYNC
    /*
     * WRITEBACK: CPU 캐시 dirty 라인을 SRAM에 반영.
     * 내부 SRAM은 no-cache이므로 ESP32-C3에서는 no-op.
     * PSRAM 사용 칩에서는 반드시 필요.
     */
    esp_cache_msync(ptr, size,
                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    /* WRITEBACK flag: IDF 5.3+. 하위 호환: INVALIDATE로 대체 */
#endif
    (void)ptr; (void)size;
    printf("    [pre-TX writeback] 캐시 → SRAM 플러시 (이식성 패턴)\n");
}

static void dma_post_rx_invalidate(void *ptr, size_t size) {
#if HAS_CACHE_MSYNC
    /* INVALIDATE: 다음 CPU 읽기 시 SRAM에서 재로드 강제 */
    esp_cache_msync(ptr, size,
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA  |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    printf("    [post-RX invalidate] 캐시 무효화 완료\n");
#else
    printf("    [post-RX invalidate] esp_cache_msync 없음 (IDF < 5.2)\n");
    (void)ptr; (void)size;
#endif
}

/* ---------- DMA 버퍼 요구사항 시연 ---------- */

static void demo_buffer_requirements(void) {
    printf("\n[1] DMA 버퍼 요구사항\n");

    /* 올바른 할당: MALLOC_CAP_DMA + 32바이트 정렬 */
    uint8_t *dma = (uint8_t *)heap_caps_aligned_alloc(
                        32, BUF_SZ,
                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    /* 일반 힙 할당 (DMA 비적합 예시) */
    uint8_t *normal = (uint8_t *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_8BIT);

    /* 스택 버퍼 (DMA 불가) */
    uint8_t stack_buf[BUF_SZ];

    printf("  스택 버퍼      : %p  → DMA 불가 (스택은 DMA 주소 범위 외)\n",
           (void *)stack_buf);
    printf("  일반 힙 버퍼   : %p  → 정렬 보장 없음, DMA 가능 여부 불확실\n",
           (void *)normal);
    printf("  DMA 힙 버퍼    : %p  → MALLOC_CAP_DMA + 32B 정렬 ✓\n",
           (void *)dma);

    if (dma) {
        printf("  주소 4B 정렬   : %s\n",
               ((uintptr_t)dma % 4  == 0) ? "OK" : "FAIL");
        printf("  주소 32B 정렬  : %s\n",
               ((uintptr_t)dma % 32 == 0) ? "OK" : "FAIL");
        heap_caps_free(dma);
    }
    if (normal) heap_caps_free(normal);
}

/* ---------- 캐시 일관성 절차 시연 ---------- */

static void demo_cache_coherency_flow(void) {
    printf("\n[2] TX/RX 캐시 동기화 절차\n");

    uint8_t *tx = (uint8_t *)heap_caps_aligned_alloc(32, BUF_SZ,
                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rx = (uint8_t *)heap_caps_aligned_alloc(32, BUF_SZ,
                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!tx || !rx) {
        ESP_LOGE(TAG, "버퍼 할당 실패");
        if (tx) heap_caps_free(tx);
        if (rx) heap_caps_free(rx);
        return;
    }

    /* ── TX 방향 ── */
    printf("\n  TX 방향 (CPU가 버퍼에 쓴 후 DMA 전송):\n");
    for (int i = 0; i < BUF_SZ; i++) tx[i] = (uint8_t)(0xA0 + (i % 16));
    printf("    Step 1: CPU가 tx_buf 작성 완료\n");
    dma_pre_tx_writeback(tx, BUF_SZ);
    printf("    Step 2: DMA 전송 시작 (SRAM 내용이 최신임을 보장)\n");

    /* ── RX 방향 ── */
    printf("\n  RX 방향 (DMA가 수신 후 CPU가 읽기):\n");
    memset(rx, 0x00, BUF_SZ);
    printf("    Step 1: DMA가 SRAM에 수신 데이터 기록 (캐시 미갱신)\n");
    /* 실제 DMA 수신 시뮬레이션 (아래 SPI 루프백에서 실제 수행) */
    dma_post_rx_invalidate(rx, BUF_SZ);
    printf("    Step 2: CPU가 rx_buf 읽기 → SRAM에서 최신 DMA 결과 반환\n");

    heap_caps_free(tx);
    heap_caps_free(rx);
}

/* ---------- SPI DMA 루프백 테스트 ---------- */

static void demo_spi_dma_loopback(void) {
    printf("\n[3] SPI DMA 루프백 테스트\n");
    printf("  배선: GPIO%d(MOSI) ↔ GPIO%d(MISO) 점퍼 연결 필요\n",
           PIN_MOSI, PIN_MISO);

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BUF_SZ,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI 버스 초기화 실패: %s", esp_err_to_name(err));
        return;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 1,
    };
    spi_device_handle_t spi;
    err = spi_bus_add_device(SPI2_HOST, &dev, &spi);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI 디바이스 추가 실패: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return;
    }

    /* SPI 드라이버가 요구하는 DMA 버퍼 할당 */
    uint8_t *tx = (uint8_t *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_DMA);
    uint8_t *rx = (uint8_t *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_DMA);
    if (!tx || !rx) { goto spi_end; }

    for (int i = 0; i < BUF_SZ; i++) tx[i] = (uint8_t)i;
    memset(rx, 0, BUF_SZ);

    spi_transaction_t t = {
        .length    = BUF_SZ * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    /*
     * spi_device_transmit():
     * IDF v5.x에서 TX 전 writeback, RX 후 invalidate를 드라이버가 내부 처리.
     * GDMA를 직접 제어할 경우 수동으로 esp_cache_msync 호출 필요.
     */
    err = spi_device_transmit(spi, &t);
    if (err == ESP_OK) {
        printf("  TX: "); for (int i = 0; i < 8; i++) printf("%02X ", tx[i]);
        printf("...\n");
        printf("  RX: "); for (int i = 0; i < 8; i++) printf("%02X ", rx[i]);
        printf("...\n");
        printf("  결과: %s\n",
               memcmp(tx, rx, BUF_SZ) == 0
               ? "TX == RX ✓ (점퍼 연결 확인)"
               : "TX != RX  (점퍼 미연결 또는 핀 오류)");
    }

spi_end:
    if (tx) heap_caps_free(tx);
    if (rx) heap_caps_free(rx);
    spi_bus_remove_device(spi);
    spi_bus_free(SPI2_HOST);
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== DMA Buffer & Cache Coherency Demo ===");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    demo_buffer_requirements();
    demo_cache_coherency_flow();
    demo_spi_dma_loopback();

    printf("\n[DMA 캐시 일관성 원칙]\n");
    printf("  TX: DMA 시작 전  → esp_cache_msync(WRITEBACK)  캐시→SRAM 플러시\n");
    printf("  RX: DMA 완료 후  → esp_cache_msync(INVALIDATE) 캐시 무효화\n");
    printf("  ESP32-C3 내부 SRAM: 비캐시 → 실질 무관 (이식성 패턴으로 권장)\n");
    printf("  PSRAM 사용 칩(ESP32-S3 등): 반드시 필요\n");
    printf("  IDF SPI/I2S 드라이버 v5.x: 드라이버 내부에서 자동 처리\n\n");
}
