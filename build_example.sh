#!/usr/bin/env bash
# build_example.sh — ESP32-C3 예제 빌드 및 QEMU 실행
#
# 사용법:
#   ./build_example.sh <example_path> [action] [timeout]
#
# action:
#   build      — 빌드만 (기본값)
#   qemu       — 빌드 후 QEMU 실행 (Ctrl+A X 로 종료)
#   qemu-log   — 빌드 후 QEMU 실행, timeout 후 자동 종료 및 로그 저장
#
# 예시:
#   ./build_example.sh examples/03_tasks build
#   ./build_example.sh examples/03_tasks qemu
#   ./build_example.sh examples/03_tasks qemu-log 30

set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
QEMU_BIN="$(find "$HOME/.espressif/tools/qemu-riscv32" -name "qemu-system-riscv32" 2>/dev/null | head -1)"
REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"

EXAMPLE_PATH="${1:-}"
ACTION="${2:-build}"
TIMEOUT="${3:-30}"

# ── 인자 검증 ──────────────────────────────────────────────
if [[ -z "$EXAMPLE_PATH" ]]; then
    echo "사용법: $0 <example_path> [build|qemu|qemu-log] [timeout_sec]"
    echo "  예)  $0 03_tasks qemu 30"
    exit 1
fi

# examples/ 접두어 자동 추가
FULL_PATH="$REPO_ROOT/examples/$EXAMPLE_PATH"
if [[ ! -d "$FULL_PATH" ]]; then
    echo "오류: 예제 경로를 찾을 수 없습니다 — $FULL_PATH"
    exit 1
fi

# ── ESP-IDF 환경 로드 ───────────────────────────────────────
if [[ -z "${IDF_TOOLS_PATH:-}" ]]; then
    # shellcheck source=/dev/null
    source "$IDF_PATH/export.sh" > /dev/null 2>&1
fi

# ── 빌드 ───────────────────────────────────────────────────
build() {
    pushd "$FULL_PATH" > /dev/null
    if [[ ! -f "build/CMakeCache.txt" ]]; then
        idf.py set-target esp32c3
    fi
    idf.py build
    popd > /dev/null
}

# ── QEMU 실행 (대화형) ──────────────────────────────────────
run_qemu_interactive() {
    pushd "$FULL_PATH" > /dev/null
    echo ""
    echo "QEMU 실행 중... 종료: Ctrl+A 누른 후 X"
    echo ""
    idf.py qemu --qemu-extra-args="-nographic"
    popd > /dev/null
}

# ── QEMU 실행 (자동 종료 + 로그 저장) ─────────────────────
run_qemu_log() {
    local log_file="$FULL_PATH/build/qemu_output.log"
    pushd "$FULL_PATH" > /dev/null
    echo "QEMU 실행 중 (타임아웃: ${TIMEOUT}s) → $log_file"
    timeout "$TIMEOUT" idf.py qemu --qemu-extra-args="-nographic -serial file:$log_file" \
        2>/dev/null || true
    popd > /dev/null
    echo "--- QEMU 출력 ---"
    cat "$log_file"
    echo "-----------------"
}

# ── QEMU 바이너리 확인 ──────────────────────────────────────
if [[ "$ACTION" == qemu* ]] && [[ -z "$QEMU_BIN" ]]; then
    echo "오류: qemu-system-riscv32 를 찾을 수 없습니다."
    echo "  python3 tools/idf_tools.py install qemu-riscv32  실행 후 재시도하세요."
    exit 1
fi

# ── 메인 ───────────────────────────────────────────────────
case "$ACTION" in
    build)
        build
        ;;
    qemu)
        build
        run_qemu_interactive
        ;;
    qemu-log)
        build
        run_qemu_log
        ;;
    *)
        echo "알 수 없는 action: $ACTION  (build | qemu | qemu-log)"
        exit 1
        ;;
esac
