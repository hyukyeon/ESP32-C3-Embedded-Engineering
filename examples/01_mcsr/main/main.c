/*
 * 01_mcsr: RISC-V Machine CSR Access
 *
 * Demonstrates direct access to Machine-mode Control and Status Registers:
 *   mcycle / mcycleh  - 64-bit hardware cycle counter
 *   minstret          - instructions retired counter
 *   misa              - ISA capabilities
 *   mhartid           - hardware thread ID
 *
 * Key insight: mcycle counts every CPU clock tick (160 MHz = 1 tick per 6.25 ns),
 * giving nanosecond-precision profiling without any OS timer overhead.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MCSR";

#define CPU_FREQ_HZ  160000000ULL
#define CYCLES_TO_NS(c) ((double)(c) * 1e9 / (double)CPU_FREQ_HZ)

/* ---------- inline CSR readers ---------- */

static inline uint32_t csr_mcycle(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v) :: "memory");
    return v;
}

static inline uint32_t csr_mcycleh(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycleh" : "=r"(v) :: "memory");
    return v;
}

static inline uint32_t csr_minstret(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, minstret" : "=r"(v) :: "memory");
    return v;
}

static inline uint32_t csr_misa(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, misa" : "=r"(v) :: "memory");
    return v;
}

static inline uint32_t csr_mhartid(void) {
    uint32_t v;
    __asm__ __volatile__("csrr %0, mhartid" : "=r"(v) :: "memory");
    return v;
}

/*
 * 64-bit safe read: re-reads if a carry from lo→hi happened mid-read.
 * Without this, a read across a 32-bit rollover (~26 s at 160 MHz)
 * can produce a value that is off by ~2^32.
 */
static uint64_t mcycle64(void) {
    uint32_t hi1, lo, hi2;
    do {
        hi1 = csr_mcycleh();
        lo  = csr_mcycle();
        hi2 = csr_mcycleh();
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

/* ---------- MISA decoder ---------- */

static void print_misa(uint32_t misa) {
    static const char *exts[] = {
        "A(Atomic)",  "B",           "C(Compressed)", "D(Double-FP)",
        "E(Embedded)","F(Single-FP)","G",             "H",
        "I(Integer)", "J",           "K",             "L",
        "M(Multiply)","N",           "O",             "P",
        "Q",          "R",           "S",             "T",
        "U(UserMode)","V",           "W",             "X",
        "Y",          "Z"
    };
    int mxl = (misa >> 30) & 0x3;
    printf("  MXLEN : %d-bit\n", mxl == 1 ? 32 : (mxl == 2 ? 64 : 128));
    printf("  Extensions: ");
    for (int i = 0; i < 26; i++) {
        if (misa & (1u << i)) printf("%s ", exts[i]);
    }
    printf("\n");
}

/* ---------- benchmark targets ---------- */

/* Simple accumulation — tests ALU throughput */
static uint32_t bench_sum(int n) {
    uint32_t s = 0;
    for (int i = 1; i <= n; i++) s += (uint32_t)i;
    return s;
}

/* Multiply-accumulate — tests M-extension (hardware multiply) */
static uint32_t bench_mac(int n) {
    uint32_t a = 1;
    for (int i = 1; i <= n; i++) a = a * (uint32_t)i + (uint32_t)i;
    return a;
}

/* ---------- profiling helper ---------- */

typedef struct {
    uint32_t cycles;
    uint32_t instrs;
} PerfSample;

static PerfSample profile_begin(void) {
    return (PerfSample){ .cycles = csr_mcycle(), .instrs = csr_minstret() };
}

static void profile_end(PerfSample start, const char *label, volatile uint32_t result) {
    uint32_t cy = csr_mcycle()   - start.cycles;
    uint32_t ir = csr_minstret() - start.instrs;
    double ipc  = (cy > 0) ? (double)ir / cy : 0.0;
    printf("  [%-18s]  result=%-10u  cycles=%-6u  instrs=%-6u  time=%-8.1f ns  IPC=%.2f\n",
           label, result, cy, ir, CYCLES_TO_NS(cy), ipc);
}

/* ---------- app_main ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== RISC-V Machine CSR Demo ===");

    /* --- One-time hardware identity dump --- */
    printf("\n[Hardware Identity]\n");
    printf("  mhartid : %u  (single-core ESP32-C3 is always 0)\n", csr_mhartid());
    printf("  misa    : 0x%08X\n", csr_misa());
    print_misa(csr_misa());

    uint64_t boot_cycles = mcycle64();
    printf("  Cycles since reset: %llu  (~%.1f ms at 160 MHz)\n\n",
           boot_cycles, (double)boot_cycles / (CPU_FREQ_HZ / 1000.0));

    uint32_t iter = 0;
    while (1) {
        iter++;
        printf("--- Iteration %u ---\n", iter);

        /* --- Benchmark 1: sum (tests ADD throughput) --- */
        PerfSample s1 = profile_begin();
        volatile uint32_t r1 = bench_sum(1000);
        profile_end(s1, "sum(1~1000)", r1);

        /* --- Benchmark 2: MAC (tests MUL throughput) --- */
        PerfSample s2 = profile_begin();
        volatile uint32_t r2 = bench_mac(20);
        profile_end(s2, "mac(1~20)", r2);

        /* --- vTaskDelay accuracy vs nominal 100 ms --- */
        uint64_t before = mcycle64();
        vTaskDelay(pdMS_TO_TICKS(100));
        uint64_t after  = mcycle64();
        uint64_t delta  = after - before;
        printf("  [vTaskDelay 100 ms]  actual = %.3f ms  (delta = %llu cycles)\n",
               (double)delta / (CPU_FREQ_HZ / 1000.0), delta);

        /* --- 64-bit uptime --- */
        printf("  [Uptime]  %llu cycles  = %.3f s\n\n",
               mcycle64(), (double)mcycle64() / (double)CPU_FREQ_HZ);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
