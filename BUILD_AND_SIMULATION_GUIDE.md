# ESP32-C3 Embedded Engineering — 실행 가이드

이 문서는 예제를 실행하는 두 가지 방법을 설명합니다.

| 환경 | OS | ESP-IDF | 실행 방식 |
|---|---|---|---|
| **VS Code + ESP-IDF 확장** | Windows | v6.0.1 | 실제 하드웨어 (USB-JTAG) |
| **Ubuntu CLI** | Ubuntu | v5.4 | QEMU 에뮬레이터 또는 실제 하드웨어 |

---

## 목차

- [환경 1 — VS Code (Windows)](#환경-1--vs-code-windows)
- [환경 2 — Ubuntu CLI](#환경-2--ubuntu-cli)
- [예제 등급 분류](#예제-등급-분류)
- [예제별 특이사항](#예제별-특이사항)

---

## 환경 1 — VS Code (Windows)

### 사전 요구사항

| 항목 | 버전/경로 |
|---|---|
| ESP-IDF | v6.0.1 (`C:\esp\v6.0.1\esp-idf`) |
| 툴체인 | `C:\Espressif\tools` |
| VS Code 확장 | [Espressif IDF](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension) |
| 디바이스 연결 | ESP32-C3 보드 → USB (내장 USB-JTAG, COM4) |

설정 파일(`.vscode/`)은 리포지토리에 포함되어 있으므로 별도 수정 없이 사용 가능합니다.

---

### 빌드

```
Ctrl+Shift+P  →  Tasks: Run Task  →  Build <예제명>
```

또는 `Ctrl+Shift+B`로 빌드 태스크 목록 바로 열기.

내부적으로 `build_example.ps1 -Action build`를 호출합니다.  
처음 빌드 시 `idf.py set-target esp32c3`를 자동 실행하여 타깃을 설정합니다.

---

### 플래시 & 모니터

```
Ctrl+Shift+P  →  Tasks: Run Task  →  Flash + Monitor <예제명> (COM4)
```

| 태스크 | 동작 |
|---|---|
| `Flash <예제> (COM4)` | 빌드된 바이너리를 COM4로 플래시 |
| `Monitor <예제> (COM4)` | 시리얼 모니터 실행 (115200 baud) |
| `Flash + Monitor <예제> (COM4)` | 플래시 후 모니터 연속 실행 |

모니터 종료: `Ctrl+]`

포트가 COM4가 아닌 경우 `.vscode/settings.json`의 `idf.portWin`을 수정합니다.

```json
"idf.portWin": "COM5"
```

---

### GDB 디버깅 (OpenOCD + JTAG)

ESP32-C3 내장 USB-JTAG를 사용하므로 외부 디버거 없이 USB 케이블 하나로 디버깅 가능합니다.

**1단계 — OpenOCD 서버 실행** (PowerShell 별도 창)

```powershell
openocd -f board/esp32c3-builtin.cfg
```

**2단계 — VS Code에서 디버그 시작**

```
F5  또는  Run → Start Debugging  →  Debug <예제명> 선택
```

`launch.json`에 등록된 구성이 `preLaunchTask`로 빌드를 자동 선행 실행합니다.

> 하드웨어 브레이크포인트는 최대 4개 사용 가능합니다 (ESP32-C3 제한).

---

### IntelliSense 갱신

특정 예제에 맞는 자동완성을 사용하려면 `.vscode/c_cpp_properties.json`의  
`compileCommands` 경로를 해당 예제의 빌드 디렉토리로 변경합니다.

```json
"compileCommands": "${workspaceFolder}/examples/03_tasks/build/compile_commands.json"
```

빌드 전에는 `compile_commands.json`이 존재하지 않으므로 빌드 후 적용됩니다.

---

### 클린 빌드

```powershell
$ex = "C:\...\ESP32-C3-Embedded-Engineering\examples\03_tasks"
Remove-Item -Recurse -Force "$ex\build"
Remove-Item -Force "$ex\sdkconfig"
```

이후 Build 태스크 재실행 시 `sdkconfig.defaults`를 기반으로 처음부터 빌드됩니다.

---

## 환경 2 — Ubuntu CLI

### 사전 요구사항

```bash
# ESP-IDF v5.4 설치 경로: ~/esp/esp-idf
# QEMU (선택): ~/.espressif/tools/qemu-riscv32/

# IDF 환경 로드 (터미널 세션마다 1회)
source ~/esp/esp-idf/export.sh
```

---

### build_example.sh 사용법

모든 동작은 리포지토리 루트의 `build_example.sh` 스크립트로 수행합니다.

```bash
./build_example.sh <예제명> [action] [timeout_sec]
```

`examples/` 접두어는 자동으로 붙으므로 예제 이름만 입력합니다.

| action | 설명 |
|---|---|
| `build` | 빌드만 (기본값) |
| `qemu` | 빌드 후 QEMU 대화형 실행 |
| `qemu-log` | 빌드 후 QEMU 실행, timeout 후 자동 종료 및 로그 저장 |

---

### 빌드

```bash
./build_example.sh 03_tasks
# 또는
./build_example.sh 03_tasks build
```

처음 빌드 시 `idf.py set-target esp32c3`를 자동 실행합니다.  
이후 빌드는 증분 빌드(incremental build)로 처리됩니다.

---

### QEMU 실행

**대화형 실행** — 직접 출력 확인

```bash
./build_example.sh 03_tasks qemu
# 종료: Ctrl+A 누른 후 X
```

**로그 캡처** — 자동 종료 후 출력 확인

```bash
./build_example.sh 03_tasks qemu-log 30   # 30초 후 자동 종료
# 로그: examples/03_tasks/build/qemu_output.log
```

---

### 전체 예제 자동 검증

`run_all_qemu.sh`로 전체 예제를 일괄 빌드 + QEMU 실행하고 결과를 리포트로 저장합니다.

```bash
# 전체 실행
./run_all_qemu.sh

# 등급 A만 실행
./run_all_qemu.sh --grade A

# 특정 예제만 실행
./run_all_qemu.sh --example 03_tasks,04_queues

# 결과 리포트
cat logs/report.txt
```

---

### 직접 idf.py 사용 (스크립트 없이)

```bash
cd examples/03_tasks

# 첫 빌드 시 타깃 지정 필요
idf.py set-target esp32c3
idf.py build

# 이후부터는
idf.py build
idf.py qemu --qemu-extra-args="-nographic"
```

> `source ~/esp/esp-idf/export.sh`를 먼저 실행하지 않으면  
> `idf.py: command not found` 오류가 발생합니다.

---

### 클린 빌드

```bash
rm -rf examples/03_tasks/build examples/03_tasks/sdkconfig
./build_example.sh 03_tasks build
```

---

## 예제 등급 분류

`run_all_qemu.sh`는 QEMU 호환성에 따라 예제를 세 등급으로 분류합니다.

| 등급 | 설명 | QEMU 결과 |
|---|---|---|
| **A** | FreeRTOS 순수 로직 — 하드웨어 의존 없음 | 완전 동작 |
| **B** | 하드웨어 수치(사이클, 주파수 등)는 부정확하나 로직 실행 가능 | 실행은 되나 수치 다름 |
| **C** | GPIO, DMA, eFuse 등 실제 주변장치 필요 | SKIP (하드웨어 필요) |

| 등급 | 예제 |
|---|---|
| A | 03_tasks, 04_queues, 07_race_mutex, 08_priority_inversion, 13_data_locality, 19_heap_fragmentation, 24_event_groups |
| B | 01_mcsr, 05_watchdog, 06_low_power, 09_iram_timing, 10_isr_latency, 15_flash_mmu, 16_pmp_sandbox, 18_boot_diagnosis, 21_core_dump, 23_timer_compare |
| C | 02_uart_jtag, 11_logic_analyzer, 12_cache_miss, 14_cache_coherency, 17_ota_mmu, 20_dma_cache, 22_gdb_jtag, 25_flash_security |

---

## 예제별 특이사항

### 02_uart_jtag — USB CDC 콘솔

`sdkconfig.defaults`에 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 설정 포함.  
USB GPIO18/19를 콘솔로 사용하므로 외부 USB-UART 칩 불필요.

### 17_ota_mmu — OTA 파티션 전환

OTA 파티션 테이블(ota_0 + ota_1)이 필요합니다.  
첫 번째 부팅에서 파티션 전환을 시도하므로 유효한 앱 이미지가 양쪽 슬롯에 있어야 정상 동작합니다.

### 21_core_dump — Core Dump 분석

`sdkconfig.defaults`에 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` 설정 포함.  
coredump 파티션 포함 시 파티션 테이블이 2MB를 초과하므로 4MB 플래시가 필요합니다.

### 22_gdb_jtag — JTAG 디버그

`sdkconfig.defaults`에 `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` 포함.  
`vTaskList()` 함수는 이 옵션이 활성화된 경우에만 링크됩니다.

### 25_flash_security — Flash 암호화/Secure Boot

현재 보안 상태를 읽기만 합니다. eFuse 소각이나 암호화 활성화는 수행하지 않습니다.  
RELEASE 모드 활성화는 되돌릴 수 없으므로 개발 보드에서 실행 시 주의가 필요합니다.
