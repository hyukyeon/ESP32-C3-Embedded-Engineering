# ESP32-C3 Super Mini: ESP-IDF Embedded Engineering

이 저장소는 ESP32-C3 Super Mini 보드를 활용한 **ESP-IDF** 기반 임베디드 시스템 설계 예제를 담고 있습니다. Arduino 프레임워크를 벗어나 RISC-V Machine Mode 접근 및 로우레벨 제어를 학습하는 것을 목표로 합니다.

## 프로젝트 구조
각 폴더는 독립적인 ESP-IDF 프로젝트입니다.

- `01_mcsr`: RISC-V Machine CSR 직접 접근 및 사이클 카운팅.
- `02_uart_jtag`: ESP-IDF 로깅 시스템 및 내장 JTAG 활용.
- `03_tasks`: FreeRTOS 태스크 관리.
- `04_queues`: 태스크 간 데이터 통신.
- `05_watchdog`: Task WDT 설정 및 안정성 확보.
- `06_low_power`: Deep Sleep 및 저전력 설계.
- `07_race_mutex`: 공유 자원 경합 및 Mutex 동기화.
- `08_priority_inversion`: 우선순위 역전 및 상속 메커니즘.
- `09_iram_timing`: IRAM 실행을 통한 결정론적 타이밍 확보.
- `10_isr_latency`: 인터럽트 지연 시간 정밀 측정.
- `11_logic_analyzer`: 고속 샘플링 종합 프로젝트.

## 빌드 및 실행 (ESP-IDF CLI)
각 예제 디렉토리에서 아래 명령어를 사용합니다.

```bash
# 타겟 설정 (최초 1회)
idf.py set-target esp32c3

# 빌드, 플래싱 및 모니터링
idf.py build flash monitor
```

## 기술 사양
- **Framework**: ESP-IDF (v5.0 이상 권장)
- **Toolchain**: RISC-V GCC
- **Hardware**: ESP32-C3 Super Mini (Single-core RISC-V)
