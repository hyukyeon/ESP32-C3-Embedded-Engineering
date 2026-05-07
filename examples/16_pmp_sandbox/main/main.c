/*
 * 16_pmp_sandbox: RISC-V PMP(Physical Memory Protection) 분석 및 샌드박싱
 *
 * PMP(Physical Memory Protection)는 RISC-V M-mode에서 물리 메모리 영역별
 * 접근 권한(R/W/X)을 설정하는 하드웨어 보호 메커니즘입니다.
 *
 * PMP 레지스터 구조 (RISC-V spec 3.7):
 *
 *   pmpcfg0  [31:24]=cfg3 [23:16]=cfg2 [15:8]=cfg1 [7:0]=cfg0
 *   pmpcfg1  [31:24]=cfg7 [23:16]=cfg6 [15:8]=cfg5 [7:0]=cfg4
 *   ...
 *
 *   각 cfg 바이트:
 *   Bit 7 : L   — Lock (0=unlocked, 1=locked+M-mode도 적용)
 *   Bit 6 : (reserved)
 *   Bit 5 : (reserved)
 *   Bit 4 : A[1] ─┐  Address Matching Mode
 *   Bit 3 : A[0] ─┘  00=OFF, 01=TOR, 10=NA4, 11=NAPOT
 *   Bit 2 : X   — Execute 허용
 *   Bit 1 : W   — Write 허용
 *   Bit 0 : R   — Read 허용
 *
 *   pmpaddr0~15: 보호 영역 주소 (물리 주소 >> 2)
 *
 * Address Matching Mode 상세:
 *   TOR  (Top Of Range): pmpaddr[i-1]~pmpaddr[i] 범위 (하위 경계 포함 안 함)
 *   NA4  (Natural 4-byte): 주소의 4바이트 영역
 *   NAPOT: Naturally Aligned Power-Of-Two
 *           pmpaddr = (base | ((size/2 - 1))) >> 2
 *           예) 256B @ 0x3FC80000: pmpaddr = (0x3FC80000 | 0x7F) >> 2 = 0xFF20001F
 *
 * 위반 시 예외:
 *   Load  위반 → mcause = 5  (Load access fault)
 *   Store 위반 → mcause = 7  (Store/AMO access fault)
 *   Fetch 위반 → mcause = 1  (Instruction access fault)
 *   mtval = 위반이 발생한 물리 주소
 *
 * ESP32-C3에서 PMP 현황:
 *   ESP-IDF는 부팅 시 PMP 항목 0~5를 시스템 보호용으로 설정합니다.
 *   항목 6~15는 사용자 코드에서 활용 가능합니다.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "PMP";

/* ---------- PMP 설정 바이트 비트 정의 ---------- */

#define PMP_R    (1 << 0)
#define PMP_W    (1 << 1)
#define PMP_X    (1 << 2)
#define PMP_A_OFF   (0 << 3)
#define PMP_A_TOR   (1 << 3)
#define PMP_A_NA4   (2 << 3)
#define PMP_A_NAPOT (3 << 3)
#define PMP_L    (1 << 7)   /* Lock — M-mode도 적용, 재설정 불가 (리셋 전까지) */

/* ---------- CSR 접근 헬퍼 ---------- */

#define CSR_READ(reg)  ({ uint32_t _v; \
    __asm__ __volatile__("csrr %0, " #reg : "=r"(_v) :: "memory"); _v; })

#define CSR_WRITE(reg, val) \
    __asm__ __volatile__("csrw " #reg ", %0" :: "r"((uint32_t)(val)) : "memory")

/* ---------- PMP 항목 읽기/쓰기 ---------- */

static uint8_t pmp_get_cfg(int idx) {
    /* pmpcfg0~3 (4개 항목씩 패킹) */
    uint32_t raw;
    switch (idx / 4) {
    case 0: raw = CSR_READ(pmpcfg0); break;
    case 1: raw = CSR_READ(pmpcfg1); break;
    case 2: raw = CSR_READ(pmpcfg2); break;
    case 3: raw = CSR_READ(pmpcfg3); break;
    default: return 0;
    }
    return (uint8_t)(raw >> ((idx % 4) * 8));
}

static uint32_t pmp_get_addr(int idx) {
    switch (idx) {
    case  0: return CSR_READ(pmpaddr0);
    case  1: return CSR_READ(pmpaddr1);
    case  2: return CSR_READ(pmpaddr2);
    case  3: return CSR_READ(pmpaddr3);
    case  4: return CSR_READ(pmpaddr4);
    case  5: return CSR_READ(pmpaddr5);
    case  6: return CSR_READ(pmpaddr6);
    case  7: return CSR_READ(pmpaddr7);
    case  8: return CSR_READ(pmpaddr8);
    case  9: return CSR_READ(pmpaddr9);
    case 10: return CSR_READ(pmpaddr10);
    case 11: return CSR_READ(pmpaddr11);
    case 12: return CSR_READ(pmpaddr12);
    case 13: return CSR_READ(pmpaddr13);
    case 14: return CSR_READ(pmpaddr14);
    case 15: return CSR_READ(pmpaddr15);
    default: return 0;
    }
}

/* ---------- PMP 현황 덤프 ---------- */

static void pmp_dump_all(void) {
    static const char *mode_str[] = {"OFF ", "TOR ", "NA4 ", "NAPT"};

    printf("\n[Current PMP Configuration — all 16 entries]\n");
    printf("  Idx  Cfg   L  Mode  R W X  pmpaddr     Phys addr\n");
    printf("  ---  ----  -  ----  -----  ----------  ----------\n");

    for (int i = 0; i < 16; i++) {
        uint8_t  cfg  = pmp_get_cfg(i);
        uint32_t addr = pmp_get_addr(i);

        uint8_t  A    = (cfg >> 3) & 0x3;
        uint8_t  L    = (cfg >> 7) & 0x1;
        uint8_t  R    = (cfg >> 0) & 0x1;
        uint8_t  W    = (cfg >> 1) & 0x1;
        uint8_t  X    = (cfg >> 2) & 0x1;

        /* 물리 주소 역산 (NAPOT: addr << 2 하위 비트 마스크 제거) */
        uint32_t phys = (addr << 2);  /* 간략화 — NAPOT 경우 추가 계산 필요 */

        printf("  [%2d] 0x%02X  %u  %s   %u %u %u  0x%08X  0x%08X %s\n",
               i, cfg, L, mode_str[A], R, W, X, (unsigned)addr, (unsigned)phys,
               (A == 0) ? "(disabled)" : "");
    }
}

/* ---------- NAPOT 주소 인코딩 ---------- */

/*
 * NAPOT: base와 size는 size 단위로 정렬되어야 함
 * pmpaddr = (base | (size/2 - 1)) >> 2
 */
static uint32_t napot_encode(uint32_t base, uint32_t size) {
    return (base | (size / 2 - 1)) >> 2;
}

/* ---------- 보호 영역 설정 (항목 6번 사용) ---------- */

/* 보호할 스크래치 버퍼 (256바이트 정렬 필수 for NAPOT) */
static uint8_t __attribute__((aligned(256))) g_protected[256];
static uint8_t g_unprotected[64];  /* 비교용: 보호 안 됨 */

static void pmp_protect_scratch(void) {
    uint32_t base = (uint32_t)(uintptr_t)g_protected;
    uint32_t size = sizeof(g_protected);

    printf("\n[PMP 항목 6 설정: g_protected 버퍼 읽기 전용 보호]\n");
    printf("  버퍼 물리 주소: 0x%08X  크기: %u bytes\n", (unsigned)base, (unsigned)size);
    printf("  NAPOT pmpaddr : 0x%08X\n", (unsigned)napot_encode(base, size));
    printf("  cfg byte      : 0x%02X (R=1, W=0, X=0, A=NAPOT, L=0)\n",
           (uint8_t)(PMP_R | PMP_A_NAPOT));

    /* pmpaddr6 설정 */
    uint32_t pmpaddr_val = napot_encode(base, size);
    CSR_WRITE(pmpaddr6, pmpaddr_val);

    /* pmpcfg1의 비트[23:16] (항목 6 = cfg1의 3번째 바이트) 설정
     * 기존 cfg1 읽기 → 해당 바이트만 교체 → 다시 씀 */
    uint32_t cfg1 = CSR_READ(pmpcfg1);
    cfg1 &= ~(0xFFUL << 16);                    /* 항목 6 비트 초기화 */
    cfg1 |=  ((uint32_t)(PMP_R | PMP_A_NAPOT) << 16);  /* R전용, NAPOT */
    CSR_WRITE(pmpcfg1, cfg1);

    /* 설정 검증 */
    uint32_t read_back = pmp_get_addr(6);
    uint8_t  cfg_back  = pmp_get_cfg(6);
    printf("  pmpaddr6 읽기  : 0x%08X %s\n", (unsigned)read_back,
           (read_back == pmpaddr_val) ? "(OK)" : "(MISMATCH!)");
    printf("  pmpcfg1 항목6  : 0x%02X   %s\n", cfg_back,
           (cfg_back == (PMP_R | PMP_A_NAPOT)) ? "(OK)" : "(MISMATCH!)");
}

/* ---------- 쓰기 테스트 ---------- */

static void test_write_access(void) {
    printf("\n[읽기 접근 테스트]\n");
    g_protected[0] = 0xAB;       /* 설정 전 미리 씀 */
    g_unprotected[0] = 0xCD;

    volatile uint8_t v1 = g_protected[0];
    volatile uint8_t v2 = g_unprotected[0];
    printf("  g_protected[0]   읽기: 0x%02X (읽기 허용 — 정상)\n", v1);
    printf("  g_unprotected[0] 읽기: 0x%02X (보호 없음 — 정상)\n", v2);

    printf("\n[쓰기 접근 테스트]\n");
    printf("  g_unprotected[0] 쓰기: 시도 → 성공 예상\n");
    g_unprotected[0] = 0xEF;
    printf("  g_unprotected[0] 확인: 0x%02X\n", g_unprotected[0]);

    printf("\n  g_protected[0] 쓰기: PMP W=0이면 Store Access Fault 발생\n");
    printf("  ※ 아래 코드는 주석 처리 — 직접 해제하면 시스템이 패닉함\n");
    printf("  ※ 위반 시: mcause=7 (Store/AMO access fault), mtval=0x%08X\n",
           (unsigned)(uintptr_t)g_protected);
    /*
     * [주석 해제 시 패닉 발생]
     * g_protected[0] = 0xFF;   <-- Store Access Fault (mcause=7)
     *
     * 시리얼 출력 예시:
     *   Guru Meditation Error: Core 0 panic'ed (Store access fault).
     *   Exception was unhandled.
     *   Core 0 register dump:
     *   MEPC    : 0x420xxxxx  ← 위반 명령어 주소 (app_main 내)
     *   MTVAL   : 0x3FC8xxxx  ← 위반된 물리 주소 (g_protected)
     *   MCAUSE  : 0x00000007  ← Store/AMO access fault
     */
}

/* ---------- mcause / mtval 해석 가이드 ---------- */

static void print_mcause_table(void) {
    printf("\n[RISC-V mcause 예외 코드 — 메모리 접근 관련]\n");
    static const struct { uint32_t code; const char *desc; } causes[] = {
        {0,  "Instruction address misaligned"},
        {1,  "Instruction access fault         ← PMP X=0 위반"},
        {4,  "Load address misaligned"},
        {5,  "Load access fault                ← PMP R=0 위반"},
        {6,  "Store/AMO address misaligned"},
        {7,  "Store/AMO access fault           ← PMP W=0 위반"},
    };
    for (int i = 0; i < (int)(sizeof(causes)/sizeof(causes[0])); i++) {
        printf("  mcause=%-2u : %s\n", (unsigned)causes[i].code, causes[i].desc);
    }
    printf("\n  mtval = 위반이 발생한 물리 주소\n");
    printf("  mepc  = 위반을 일으킨 명령어의 주소 (함수+오프셋)\n");
}

/* ---------- PMP 해제 ---------- */

static void pmp_disable_entry6(void) {
    /* cfg를 0으로 → A=OFF → 비활성화 */
    uint32_t cfg1 = CSR_READ(pmpcfg1);
    cfg1 &= ~(0xFFUL << 16);  /* 항목 6 비트 지우기 */
    CSR_WRITE(pmpcfg1, cfg1);
    printf("\n[PMP 항목 6 비활성화 완료]\n");
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== RISC-V PMP Sandbox Demo ===");
    printf("PMP 항목: 16개  |  페이지 크기: 4B(NA4) ~ NAPOT  |  Privilege: M-mode\n");

    /* 1. 현재 ESP-IDF 부팅 후 PMP 설정 덤프 */
    pmp_dump_all();

    /* 2. g_protected 버퍼에 PMP 읽기 전용 보호 설정 */
    pmp_protect_scratch();

    /* 3. 접근 테스트 */
    test_write_access();

    /* 4. mcause/mtval 해석 안내 */
    print_mcause_table();

    /* 5. PMP 해제 */
    pmp_disable_entry6();
    pmp_dump_all();

    printf("\n[PMP 실무 활용]\n");
    printf("  • 커널 영역(IRAM)을 사용자 태스크에서 쓰기 불가로 보호\n");
    printf("  • NULL 포인터 역참조 감지: 0x0~0xFFF을 NO_ACCESS로 설정\n");
    printf("  • Flash 쓰기 API 호출 전 코드 영역을 읽기 전용으로 잠금\n");
    printf("  • 트랩 핸들러에서 mcause+mtval 기록 후 안전 복구 가능\n\n");
}
