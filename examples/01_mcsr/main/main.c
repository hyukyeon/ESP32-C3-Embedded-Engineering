/*
 * 01_mcsr: RISC-V Machine CSR Access
 *
 * Demonstrates direct access to Machine-mode Control and Status Registers.
 *
 * ESP32-C3 Note: The standard mcycle (0xB00), mcycleh (0xB80), and minstret
 * (0xB02) CSRs are NOT implemented. Reading them raises Illegal Instruction.
 * The ESP32-C3 uses a custom 32-bit performance counter register:
 *   PCER (0x7E0) - event select  (pre-configured by boot ROM to count cycles)
 *   PCMR (0x7E1) - enable/mode
 *   PCCR (0x7E2) - 32-bit count value  ← equivalent of mcycle on this chip
 *
 * This example reads PCCR (0x7E2) for cycle measurement.  minstret is
 * unavailable; the IPC column is omitted from profiling output.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MCSR";

#define CPU_FREQ_HZ  160000000ULL
#define CYCLES_TO_NS(c) ((double)(c) * 1e9 / (double)CPU_FREQ_HZ)

/* ---------- inline CSR readers ---------- */

static inline uint32_t csr_mcycle(void) {
    uint32_t v;
    /* ESP32-C3 replaces standard mcycle (0xB00) with custom PCCR at 0x7E2.
     * Both mcycle and mcycleh raise Illegal Instruction on this chip. */
    __asm__ __volatile__("csrr %0, 0x7e2" : "=r"(v) :: "memory");
    return v;
}

static inline uint32_t csr_minstret(void) {
    /* minstret (0xB02) is NOT implemented on ESP32-C3; returns 0. */
    return 0;
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
 * mcycleh (CSR 0xB80) is OPTIONAL in RISC-V RV32 and NOT implemented on
 * ESP32-C3. Accessing an absent CSR raises Illegal Instruction → reboot.
 * We extend the mandatory 32-bit mcycle to 64 bits in software by detecting
 * wrap-arounds. At 160 MHz the counter rolls over every ~26.8 s; calling
 * this function at least that often prevents missing a wrap.
 */
static uint64_t mcycle64(void) {
    static uint32_t prev_lo = 0;
    static uint64_t hi_bits = 0;
    uint32_t lo = csr_mcycle();
    if (lo < prev_lo) {
        hi_bits += (uint64_t)1 << 32;   /* wrap-around detected */
    }
    prev_lo = lo;
    return hi_bits | (uint64_t)lo;
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
    uint32_t cy = csr_mcycle() - start.cycles;
    /* minstret unavailable on ESP32-C3; IPC omitted */
    printf("  [%-18s]  result=%-10" PRIu32 "  cycles=%-6" PRIu32 "  time=%-8.1f ns\n",
           label, result, cy, CYCLES_TO_NS(cy));
}

/* ---------- app_main ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== RISC-V Machine CSR Demo ===");

    /* --- One-time hardware identity dump --- */
    printf("\n[Hardware Identity]\n");
    printf("  mhartid : %" PRIu32 "  (single-core ESP32-C3 is always 0)\n", csr_mhartid());
    printf("  misa    : 0x%08" PRIX32 "\n", csr_misa());
    print_misa(csr_misa());

    uint64_t boot_cycles = mcycle64();
    printf("  Cycles since reset: %" PRIu64 "  (~%.1f ms at 160 MHz)\n\n",
           boot_cycles, (double)boot_cycles / (CPU_FREQ_HZ / 1000.0));

    uint32_t iter = 0;
    while (1) {
        iter++;
        printf("--- Iteration %" PRIu32 " ---\n", iter);

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
        printf("  [vTaskDelay 100 ms]  actual = %.3f ms  (delta = %" PRIu64 " cycles)\n",
               (double)delta / (CPU_FREQ_HZ / 1000.0), delta);

        /* --- 64-bit uptime --- */
        uint64_t uptime = mcycle64();
        printf("  [Uptime]  %" PRIu64 " cycles  = %.3f s\n\n",
               uptime, (double)uptime / (double)CPU_FREQ_HZ);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
