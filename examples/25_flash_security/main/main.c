/*
 * 25_flash_security: Flash 암호화 + Secure Boot 상태 진단
 *
 * ⚠ 경고: 이 예제는 현재 보안 상태를 읽기만 합니다.
 *   Flash 암호화나 Secure Boot를 실제로 활성화하면 되돌릴 수 없습니다.
 *   개발 보드에서 RELEASE 모드 활성화는 eFuse를 영구 소각하며 브릭 위험이 있습니다.
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Flash 암호화 (Flash Encryption):
 *
 *   활성화 시 Flash 내용을 AES-XTS 방식으로 자동 암호화.
 *   CPU는 복호화된 데이터를 투명하게 읽으나, JTAG나 플래셔로는
 *   암호화된 원시 바이트만 보임.
 *
 *   암호화 모드:
 *     DISABLED   : 비활성화 (개발 기본값)
 *     DEVELOPMENT: 암호화 활성화, JTAG 허용, eFuse 재프로그래밍 가능
 *     RELEASE    : 암호화 + JTAG 비활성화, eFuse 영구 잠금 ← 되돌릴 수 없음
 *
 *   암호화 후 동작:
 *     esp_partition_write() → 자동 암호화 후 Flash 기록
 *     esp_partition_read()  → 자동 복호화 후 반환
 *     SPI Flash 직접 읽기  → 암호화된 원시 데이터 반환 (해독 불가)
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Secure Boot (보안 부팅):
 *
 *   부트로더가 앱 이미지의 RSA-PSS 서명을 검증 후에만 실행.
 *   공개 키 해시가 eFuse에 소각되어, 서명되지 않은 이미지 실행 거부.
 *
 *   v2 체인 of Trust:
 *     ROM Bootloader (변경 불가)
 *       → 2차 부트로더 서명 검증
 *           → 앱 이미지 서명 검증
 *               → 앱 실행
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * eFuse:
 *   1회 프로그래밍 가능한 비트 배열 (OTP: One-Time Programmable).
 *   일단 1로 설정되면 0으로 되돌릴 수 없음.
 *   Flash 암호화 키, Secure Boot 공개키 해시, WR_DIS/RD_DIS 등 저장.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_flash_encrypt.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_flash.h"

/* Secure Boot 헤더는 IDF 버전별 위치가 다름 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  include "esp_secure_boot.h"
#  define HAS_SECURE_BOOT 1
#else
#  define HAS_SECURE_BOOT 0
#endif

static const char *TAG = "FLASH_SEC";

/* ---------- Flash 암호화 상태 ---------- */

static void print_flash_encryption_status(void) {
    printf("\n[A] Flash 암호화 상태\n");

    bool enc_enabled = esp_flash_encryption_enabled();
    printf("  암호화 활성화: %s\n", enc_enabled ? "예 (활성)" : "아니오 (비활성)");

    esp_flash_enc_mode_t mode = esp_get_flash_encryption_mode();
    const char *mode_str;
    switch (mode) {
    case ESP_FLASH_ENC_MODE_DISABLED:    mode_str = "DISABLED    (암호화 없음)"; break;
    case ESP_FLASH_ENC_MODE_DEVELOPMENT: mode_str = "DEVELOPMENT (JTAG 허용, eFuse 재기록 가능)"; break;
    case ESP_FLASH_ENC_MODE_RELEASE:     mode_str = "RELEASE     (JTAG 차단, eFuse 잠금)"; break;
    default:                             mode_str = "알 수 없음"; break;
    }
    printf("  암호화 모드  : %s\n", mode_str);

    if (!enc_enabled) {
        printf("\n  [암호화 비활성 상태의 의미]\n");
        printf("  • JTAG/플래셔로 Flash 내용 평문 읽기 가능\n");
        printf("  • 펌웨어 추출 및 역공학 가능\n");
        printf("  • 개발 단계: 디버깅 편의를 위해 비활성이 일반적\n");
    } else {
        printf("\n  [암호화 활성 상태의 의미]\n");
        printf("  • Flash 원시 읽기 시 AES-XTS 암호화된 바이트만 보임\n");
        printf("  • CPU XIP 경로에서 자동 복호화 (투명 암호화)\n");
        printf("  • 암호화 키는 eFuse에 저장 (외부 노출 없음)\n");
    }
}

/* ---------- Secure Boot 상태 ---------- */

static void print_secure_boot_status(void) {
    printf("\n[B] Secure Boot 상태\n");

#if HAS_SECURE_BOOT
    bool sb_enabled = esp_secure_boot_enabled();
    printf("  Secure Boot 활성화: %s\n",
           sb_enabled ? "예 (서명 검증 중)" : "아니오 (비활성)");

    if (sb_enabled) {
        printf("\n  [Secure Boot 활성 상태]\n");
        printf("  • 부트로더가 앱 이미지 RSA-PSS 서명 검증\n");
        printf("  • 서명 불일치 시 부팅 거부\n");
        printf("  • eFuse에 공개키 해시 영구 소각됨\n");
    } else {
        printf("\n  [Secure Boot 비활성 상태]\n");
        printf("  • 임의의 펌웨어 플래싱 및 실행 가능\n");
        printf("  • 물리 접근 시 악성 코드 주입 가능\n");
    }
#else
    printf("  Secure Boot API 없음 (IDF < 5.0)\n");
#endif
}

/* ---------- eFuse 블록 상태 ---------- */

static void print_efuse_status(void) {
    printf("\n[C] eFuse 주요 필드 상태\n");

    /* WR_DIS: 쓰기 비활성화 비트 — 어떤 eFuse 필드가 잠겼는지 */
    uint8_t wr_dis = 0;
    esp_efuse_read_field_blob(ESP_EFUSE_WR_DIS, &wr_dis, 8);
    printf("  WR_DIS[7:0]   : 0x%02X  (1=해당 eFuse 블록 쓰기 잠금)\n",
           (unsigned)wr_dis);

    /* RD_DIS: 읽기 비활성화 — KEY 블록 읽기 차단 여부 */
    uint8_t rd_dis = 0;
    esp_efuse_read_field_blob(ESP_EFUSE_RD_DIS, &rd_dis, 8);
    printf("  RD_DIS[7:0]   : 0x%02X  (1=해당 KEY 블록 읽기 차단)\n",
           (unsigned)rd_dis);

    /* JTAG_SEL_ENABLE: JTAG 소프트 비활성화 여부 */
    uint8_t jtag_dis = 0;
    esp_efuse_read_field_blob(ESP_EFUSE_DIS_PAD_JTAG, &jtag_dis, 1);
    printf("  DIS_PAD_JTAG  : %u    (%s)\n", (unsigned)jtag_dis,
           jtag_dis ? "JTAG 물리 핀 비활성화됨" : "JTAG 핀 활성 (디버깅 가능)");

    /* CHIP_ID / MAC 주소로 디바이스 식별 */
    uint8_t mac[6] = {0};
    esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, mac, 48);
    printf("  MAC 주소      : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------- 암호화 동작 시연 ---------- */

static void demo_transparent_encryption(void) {
    printf("\n[D] 투명 암호화 동작 시연\n");

    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!nvs) { printf("  NVS 파티션 없음\n"); return; }

    printf("  NVS 파티션: offset=0x%08X  size=%u bytes\n",
           (unsigned)nvs->address, (unsigned)nvs->size);

    uint8_t buf_api[16], buf_raw[16];

    /* esp_partition_read: 드라이버가 복호화 후 반환 */
    esp_partition_read(nvs, 0, buf_api, sizeof(buf_api));

    /* esp_flash_read: SPI 직접 읽기 (암호화 시 암호문) */
    esp_flash_read(NULL, buf_raw, nvs->address, sizeof(buf_raw));

    printf("  partition_read (복호화): ");
    for (int i = 0; i < 16; i++) printf("%02X ", buf_api[i]);
    printf("\n");
    printf("  esp_flash_read (원시):   ");
    for (int i = 0; i < 16; i++) printf("%02X ", buf_raw[i]);
    printf("\n");

    bool enc = esp_flash_encryption_enabled();
    if (enc && memcmp(buf_api, buf_raw, 16) != 0) {
        printf("  → 두 값이 다름: 암호화 활성 확인! (API는 투명 복호화)\n");
    } else if (!enc) {
        printf("  → 두 값이 같음: 암호화 비활성 (평문 저장)\n");
    } else {
        printf("  → 두 값이 같음: 첫 페이지가 소거 상태이거나 비암호화 데이터\n");
    }
}

/* ---------- 보안 설정 활성화 가이드 ---------- */

static void print_activation_guide(void) {
    printf("\n[E] 보안 설정 활성화 방법 (menuconfig)\n");
    printf("  ⚠  RELEASE 모드 활성화는 되돌릴 수 없습니다.\n\n");

    printf("  Flash 암호화 활성화:\n");
    printf("    Security features\n");
    printf("      → Enable flash encryption on boot: Y\n");
    printf("      → Flash encryption mode: Development (개발) / Release (출시)\n\n");

    printf("  Secure Boot 활성화:\n");
    printf("    Security features\n");
    printf("      → Enable hardware Secure Boot in bootloader: Y\n");
    printf("      → Secure Boot Version: 2\n");
    printf("      → 서명 키: idf.py secure-generate-signing-key key.pem\n");
    printf("      → 서명:    idf.py sign-binary --keyfile key.pem app.bin\n\n");

    printf("  권장 순서:\n");
    printf("    1. 개발 단계: Flash 암호화 DEVELOPMENT 모드로 테스트\n");
    printf("    2. 서명 키 생성 및 보관 (key.pem은 절대 유출 금지)\n");
    printf("    3. 양산 직전: RELEASE 모드 + Secure Boot 활성화\n");
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== Flash Encryption & Secure Boot Status ===");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    print_flash_encryption_status();
    print_secure_boot_status();
    print_efuse_status();
    demo_transparent_encryption();
    print_activation_guide();

    printf("\n[보안 체인 핵심 요약]\n");
    printf("  ROM BL(불변) → 2차 BL 서명 검증 → 앱 서명 검증 → 앱 실행\n");
    printf("  Flash 암호화: 물리 접근에서 펌웨어 내용 보호\n");
    printf("  Secure Boot : 인가된 펌웨어만 실행 보장\n");
    printf("  eFuse       : 설정 영구 소각 → 공장 초기화 불가\n\n");
}
