#!/usr/bin/env bash
# run_all_qemu.sh — 전체 예제 자동 빌드 + QEMU 검증
#
# 사용법:
#   ./run_all_qemu.sh              # 전체 실행
#   ./run_all_qemu.sh --grade A    # A등급만
#   ./run_all_qemu.sh --grade A,B  # A,B등급
#   ./run_all_qemu.sh --example 03_tasks,04_queues  # 특정 예제만
#
# 결과: logs/report.txt 에 PASS/FAIL/SKIP 리포트 저장

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
LOGS_DIR="$REPO_ROOT/logs"
REPORT="$LOGS_DIR/report.txt"
TIMEOUT=30

mkdir -p "$LOGS_DIR"

# ── 등급 분류 ───────────────────────────────────────────────
declare -A GRADE
declare -A EXPECT  # 통과 조건 (grep 패턴, | 로 OR)

# A: FreeRTOS 순수 로직 — 완전 동작 기대
GRADE[03_tasks]="A";            EXPECT[03_tasks]="Sensor.*started|Process.*started|Logger"
GRADE[04_queues]="A";           EXPECT[04_queues]="Consumer|SensorData|sensor_id"
GRADE[07_race_mutex]="A";       EXPECT[07_race_mutex]="Phase 1|Phase 2|20000"
GRADE[08_priority_inversion]="A"; EXPECT[08_priority_inversion]="HIGH|MID|LOW|priority"
GRADE[13_data_locality]="A";    EXPECT[13_data_locality]="row|col|sequential|stride"
GRADE[19_heap_fragmentation]="A"; EXPECT[19_heap_fragmentation]="heap|free|fragment|alloc"
GRADE[24_event_groups]="A";     EXPECT[24_event_groups]="event|group|bit|wait"

# B: 로직은 동작하나 하드웨어 수치 부정확 — 실행 여부만 확인
GRADE[01_mcsr]="B";             EXPECT[01_mcsr]="CSR|misa|mhartid|MXLEN|Hardware"
GRADE[05_watchdog]="B";         EXPECT[05_watchdog]="TWDT|watchdog|wdt|Watchdog"
GRADE[06_low_power]="B";        EXPECT[06_low_power]="sleep|power|wake|boot"
GRADE[09_iram_timing]="B";      EXPECT[09_iram_timing]="IRAM|Flash|cycles|timing"
GRADE[10_isr_latency]="B";      EXPECT[10_isr_latency]="ISR|latency|interrupt|cycles"
GRADE[15_flash_mmu]="B";        EXPECT[15_flash_mmu]="MMU|flash|map|vaddr"
GRADE[16_pmp_sandbox]="B";      EXPECT[16_pmp_sandbox]="PMP|pmp|sandbox|pmpcfg"
GRADE[18_boot_diagnosis]="B";   EXPECT[18_boot_diagnosis]="boot|reset|reason|Cold"
GRADE[21_core_dump]="B";        EXPECT[21_core_dump]="core.dump|panic|backtrace|Core"
GRADE[23_timer_compare]="B";    EXPECT[23_timer_compare]="timer|gptimer|Timer|period"

# C: GPIO/DMA/eFuse 등 주변장치 의존 — SKIP
GRADE[02_uart_jtag]="C";        EXPECT[02_uart_jtag]=""
GRADE[11_logic_analyzer]="C";   EXPECT[11_logic_analyzer]=""
GRADE[12_cache_miss]="C";       EXPECT[12_cache_miss]=""
GRADE[14_cache_coherency]="C";  EXPECT[14_cache_coherency]=""
GRADE[17_ota_mmu]="C";          EXPECT[17_ota_mmu]=""
GRADE[20_dma_cache]="C";        EXPECT[20_dma_cache]=""
GRADE[22_gdb_jtag]="C";         EXPECT[22_gdb_jtag]=""
GRADE[25_flash_security]="C";   EXPECT[25_flash_security]=""

# ── 인자 파싱 ───────────────────────────────────────────────
FILTER_GRADE=""
FILTER_EXAMPLES=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --grade)    FILTER_GRADE="$2";    shift 2 ;;
        --example)  FILTER_EXAMPLES="$2"; shift 2 ;;
        *) echo "알 수 없는 옵션: $1"; exit 1 ;;
    esac
done

# ── 실행 대상 결정 ──────────────────────────────────────────
get_targets() {
    if [[ -n "$FILTER_EXAMPLES" ]]; then
        tr ',' '\n' <<< "$FILTER_EXAMPLES"
        return
    fi
    for ex in $(ls "$REPO_ROOT/examples" | sort); do
        local name="${ex#*_}"   # "03_tasks" → 사용하지 않음, 키는 "03_tasks"
        local key="$ex"
        if [[ -z "${GRADE[$key]+x}" ]]; then continue; fi
        if [[ -n "$FILTER_GRADE" ]]; then
            local g="${GRADE[$key]}"
            if ! grep -q "$g" <<< "${FILTER_GRADE//,/$'\n'}"; then continue; fi
        fi
        echo "$ex"
    done
}

# ── 단일 예제 실행 ──────────────────────────────────────────
run_example() {
    local ex="$1"
    local grade="${GRADE[$ex]:-?}"
    local expect="${EXPECT[$ex]:-}"
    local log="$LOGS_DIR/${ex}.log"

    printf "%-25s [%s]  " "$ex" "$grade"

    # C등급 SKIP
    if [[ "$grade" == "C" ]]; then
        echo "SKIP  (하드웨어 주변장치 필요)"
        echo "$ex [C] SKIP" >> "$REPORT"
        return
    fi

    # 빌드
    if ! bash "$REPO_ROOT/build_example.sh" "examples/$ex" build > "$log" 2>&1; then
        echo "FAIL  (빌드 실패)"
        echo "$ex [$grade] FAIL (build)" >> "$REPORT"
        return
    fi

    # QEMU 실행 + 로그 캡처
    if ! bash "$REPO_ROOT/build_example.sh" "examples/$ex" qemu-log "$TIMEOUT" \
            >> "$log" 2>&1; then
        : # timeout은 정상 종료로 처리
    fi

    # 기대 패턴 확인
    if [[ -z "$expect" ]]; then
        echo "PASS  (패턴 없음, 실행 완료)"
        echo "$ex [$grade] PASS" >> "$REPORT"
    elif grep -qE "$expect" "$log" 2>/dev/null; then
        echo "PASS"
        echo "$ex [$grade] PASS" >> "$REPORT"
    else
        echo "FAIL  (기대 출력 없음: $expect)"
        echo "$ex [$grade] FAIL (output)" >> "$REPORT"
    fi
}

# ── 메인 ───────────────────────────────────────────────────
echo "ESP32-C3 QEMU 자동 검증 시작 — $(date)" | tee "$REPORT"
echo "============================================" | tee -a "$REPORT"

mapfile -t TARGETS < <(get_targets)

for ex in "${TARGETS[@]}"; do
    run_example "$ex"
done

echo ""
echo "============================================"
echo "리포트 저장됨: $REPORT"
echo ""
echo "요약:"
grep -c "PASS"  "$REPORT" | xargs printf "  PASS: %s\n" || true
grep -c "FAIL"  "$REPORT" | xargs printf "  FAIL: %s\n" || true
grep -c "SKIP"  "$REPORT" | xargs printf "  SKIP: %s\n" || true
