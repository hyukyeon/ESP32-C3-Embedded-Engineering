/*
 * 22_gdb_jtag: GDB via USB-JTAG — 브레이크포인트와 백트레이스
 *
 * ESP32-C3는 별도 하드웨어 없이 내장 USB-JTAG로 GDB 디버깅이 가능합니다.
 * (USB 포트가 USB-Serial과 JTAG 두 가지 역할을 동시에 수행)
 *
 * 연결 방법:
 *   PC → USB → ESP32-C3 내장 USB-JTAG → CPU JTAG TAP
 *
 * 디버깅 세션 시작:
 *   터미널 1: idf.py openocd          (OpenOCD 서버, 포트 3333)
 *   터미널 2: idf.py gdb              (GDB 클라이언트)
 *   또는:     idf.py openocd gdb      (동시 실행)
 *
 * 핵심 GDB 명령:
 *   break app_main          — app_main에 브레이크포인트 설정
 *   break main.c:42         — 파일:라인에 설정
 *   watch g_counter         — 변수 값 변경 감지 (하드웨어 워치포인트)
 *   continue (c)            — 실행 재개
 *   next (n)                — 한 줄 실행 (함수 호출은 넘김)
 *   step (s)                — 한 줄 실행 (함수 내부 진입)
 *   bt                      — 백트레이스 (호출 스택)
 *   info locals             — 지역 변수 목록
 *   info tasks              — FreeRTOS 태스크 목록
 *   p variable              — 변수 값 출력
 *   p/x variable            — 16진수로 출력
 *   x/16xw 0x3FC80000       — 메모리 주소 직접 덤프
 *   set variable = value    — 변수 값 강제 변경
 *   monitor reset halt      — 디바이스 리셋 후 정지
 *
 * RISC-V 소프트웨어 브레이크포인트:
 *   asm volatile("ebreak");  — EBREAK 명령어 = 디버거 트랩
 *   GDB 연결 시: 이 지점에서 자동 정지 → 변수/스택 검사 가능
 *   GDB 미연결 시: 예외 발생 (패닉)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_debug_helpers.h"

static const char *TAG = "GDB_JTAG";

/* ---------- 디버깅 대상 데이터 구조 ---------- */

typedef struct {
    uint32_t id;
    float    value;
    char     name[16];
} SensorRecord_t;

static volatile uint32_t g_counter  = 0;
static volatile uint32_t g_overflow = 0;
static SensorRecord_t    g_records[8];

/* ---------- 백트레이스 출력 (esp_debug_helpers) ---------- */

static void print_backtrace_here(int depth) {
    printf("  [소프트웨어 백트레이스 — 현재 위치]\n");
    /*
     * esp_backtrace_print(depth):
     *   현재 호출 스택을 depth 프레임까지 출력.
     *   GDB 없이도 런타임에 호출 경로 확인 가능.
     */
    esp_backtrace_print(depth);
}

/* ---------- 깊은 호출 스택 시연 ---------- */

static void deep_level4(uint32_t val) {
    g_records[val % 8].value = (float)val * 1.23f;
    printf("    deep_level4: val=%u, 백트레이스 출력:\n", (unsigned)val);
    print_backtrace_here(6);
    /* GDB 브레이크포인트 권장 위치: 여기서 'bt' 명령으로 4단계 스택 확인 */
}

static void deep_level3(uint32_t val) { deep_level4(val + 10); }
static void deep_level2(uint32_t val) { deep_level3(val + 10); }
static void deep_level1(uint32_t val) { deep_level2(val + 10); }

/* ---------- 워치포인트 시연 대상 ---------- */

/*
 * GDB 워치포인트 설정 예시:
 *   (gdb) watch g_counter
 *   → g_counter 값이 변경될 때마다 GDB가 실행을 정지시킴
 *   RISC-V에서 하드웨어 워치포인트는 4개까지 지원
 */
static void counter_task(void *arg) {
    (void)arg;
    while (1) {
        g_counter++;
        if (g_counter == 0) g_overflow++;   /* 오버플로 감지 */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---------- EBREAK 소프트웨어 브레이크포인트 ---------- */

static void demo_ebreak(void) {
    printf("\n[EBREAK 소프트웨어 브레이크포인트]\n");
    printf("  GDB 연결 상태: EBREAK 실행 시 GDB가 이 지점에서 정지.\n");
    printf("  GDB 미연결:    패닉 발생 (Breakpoint exception).\n");
    printf("  → 아래 주석을 해제하면 GDB 세션에서 이 지점에 멈춥니다.\n\n");

    /*
     * [GDB 연결 후 주석 해제]
     * __asm__ volatile("ebreak");
     *
     * GDB 출력 예시:
     *   Thread 1 "main" received signal SIGTRAP, Trace/breakpoint trap.
     *   demo_ebreak () at main.c:NNN
     *   (gdb) bt
     *   #0  demo_ebreak () at main.c:NNN
     *   #1  app_main () at main.c:NNN
     */
}

/* ---------- GDB 세션 시나리오 출력 ---------- */

static void print_gdb_session_guide(void) {
    printf("\n  ┌── GDB 디버깅 세션 예시 ────────────────────────────────────┐\n");
    printf("  │ $ idf.py openocd gdb                                      │\n");
    printf("  │ (gdb) break counter_task                                  │\n");
    printf("  │ (gdb) continue                                            │\n");
    printf("  │ Breakpoint 1, counter_task at main.c:NNN                  │\n");
    printf("  │ (gdb) p g_counter                                         │\n");
    printf("  │ $1 = 42                                                   │\n");
    printf("  │ (gdb) watch g_counter                                     │\n");
    printf("  │ Hardware watchpoint 2: g_counter                         │\n");
    printf("  │ (gdb) continue                                            │\n");
    printf("  │ Hardware watchpoint 2: g_counter  Old=42  New=43         │\n");
    printf("  │ (gdb) info tasks                                          │\n");
    printf("  │   Name       State  Priority  Stack                      │\n");
    printf("  │   counter    Running  5        0x3FC...                  │\n");
    printf("  │ (gdb) bt                                                  │\n");
    printf("  │ #0  counter_task at main.c:NNN                           │\n");
    printf("  │ #1  0x42001234 in vPortTaskWrapper                       │\n");
    printf("  └───────────────────────────────────────────────────────────┘\n");
}

/* ---------- 메인 ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "=== GDB via USB-JTAG Demo ===");

    /* 디버깅 대상 데이터 초기화 */
    for (int i = 0; i < 8; i++) {
        g_records[i].id = (uint32_t)i;
        snprintf(g_records[i].name, sizeof(g_records[i].name), "sensor_%d", i);
        g_records[i].value = 0.0f;
    }

    /* 카운터 태스크 생성 (워치포인트 시연용) */
    xTaskCreate(counter_task, "counter", 2048, NULL, 5, NULL);

    printf("\n[1] USB-JTAG 연결 정보\n");
    printf("  ESP32-C3 내장 USB-JTAG: 별도 하드웨어 불필요\n");
    printf("  OpenOCD 서버: idf.py openocd\n");
    printf("  GDB 클라이언트: idf.py gdb\n");

    printf("\n[2] 깊은 호출 스택 (백트레이스 시연)\n");
    deep_level1(100);

    printf("\n[3] 현재 실행 중인 태스크 목록\n");
    char buf[512];
    vTaskList(buf);
    printf("%s\n", buf);

    demo_ebreak();
    print_gdb_session_guide();

    printf("\n[GDB + JTAG 핵심 포인트]\n");
    printf("  • 브레이크포인트: 코드 실행을 특정 지점에서 정지\n");
    printf("  • 워치포인트: 변수 값 변경 시 자동 정지 (하드웨어 지원)\n");
    printf("  • esp_backtrace_print(): GDB 없이 소프트웨어 백트레이스\n");
    printf("  • EBREAK 명령어: 소프트웨어 브레이크포인트 삽입\n");
    printf("  • Core Dump(챕터 21)와 함께: 사후 분석도 가능\n\n");

    /* 워치포인트 시연: g_counter 값이 계속 증가 */
    uint32_t prev = 0;
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t cur = g_counter;
        printf("  g_counter: %u (+%u/sec)\n", (unsigned)cur, (unsigned)(cur - prev));
        prev = cur;
    }
}
