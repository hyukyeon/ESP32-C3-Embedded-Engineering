# VS Code 개발 환경 설정 가이드

## 환경 정보

| 항목 | 값 |
|---|---|
| ESP-IDF 버전 | v6.0.1 |
| ESP-IDF 경로 | `C:/esp/v6.0.1/esp-idf` |
| 툴체인 경로 | `C:/Espressif/tools` |
| Python venv | `C:/Espressif/tools/python/v6.0.1/venv` |
| 타깃 | ESP32-C3 (RISC-V RV32IMC) |
| 시리얼 포트 | COM4, 115200 baud |
| OS | Windows |

---

## 워크스페이스 구조

```
Git_Embedded/                          ← VS Code 워크스페이스 루트
├── .vscode/
│   ├── settings.json                  ← ESP-IDF 확장 경로 설정
│   ├── tasks.json                     ← Build / Flash / Monitor 태스크
│   ├── c_cpp_properties.json          ← IntelliSense 설정
│   └── launch.json                    ← GDB 디버그 설정
├── build_example.ps1                  ← 빌드/플래시/모니터 실행 스크립트
├── CMakeLists.txt                     ← 플레이스홀더 (빌드 없음)
└── ESP32-C3-Embedded-Engineering/     ← 클론된 예제 리포지토리
    └── examples/
        ├── 01_mcsr/
        │   ├── CMakeLists.txt
        │   ├── sdkconfig              ← 빌드 후 생성
        │   ├── build/                 ← 빌드 후 생성 (예제별 독립)
        │   └── main/main.c
        ├── 02_uart_jtag/
        │   ├── CMakeLists.txt
        │   ├── sdkconfig.defaults     ← USB CDC 콘솔 설정 포함
        │   ├── build/                 ← 빌드 후 생성
        │   └── main/main.c
        └── 03_.../
```

> **핵심**: 각 예제는 완전히 독립된 IDF 프로젝트입니다.  
> `build/`, `sdkconfig`는 예제 폴더 안에 각자 생성됩니다.  
> 루트 `CMakeLists.txt`는 빌드에 사용되지 않습니다.

---

## VS Code에서 예제 빌드/플래시/모니터 방법

### 기본 방법: Tasks 메뉴 사용

```
Ctrl+Shift+P  →  Tasks: Run Task  →  원하는 태스크 선택
```

또는 `Ctrl+Shift+B` (빌드 태스크 목록 표시)

### 등록된 태스크 목록

| 태스크 이름 | 동작 |
|---|---|
| `Build 01_mcsr` | 01_mcsr 빌드 |
| `Flash 01_mcsr (COM4)` | 빌드 후 COM4로 플래시 |
| `Monitor 01_mcsr (COM4)` | 시리얼 모니터 실행 |
| `Flash + Monitor 01_mcsr (COM4)` | 플래시 후 모니터 |
| `Build 02_uart_jtag` | 02_uart_jtag 빌드 |
| `Flash 02_uart_jtag (COM4)` | 빌드 후 COM4로 플래시 |
| `Monitor 02_uart_jtag (COM4)` | 시리얼 모니터 실행 |
| `Flash + Monitor 02_uart_jtag (COM4)` | 플래시 후 모니터 |

### 모니터 종료

```
Ctrl+]
```

### 새 예제 추가 시 tasks.json에 추가할 패턴

```json
{
  "label": "Build 03_xxx",
  "type": "shell",
  "command": "powershell",
  "args": [
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", "${workspaceFolder}\\build_example.ps1",
    "-ExamplePath", "ESP32-C3-Embedded-Engineering\\examples\\03_xxx",
    "-Action", "build"
  ],
  "group": "build",
  "presentation": { "reveal": "always", "panel": "dedicated" },
  "problemMatcher": ["$gcc"]
}
```

---

## 설정 파일 설명

### `build_example.ps1`

태스크에서 호출되는 빌드/플래시/모니터 스크립트입니다.

**파라미터:**

| 파라미터 | 설명 | 예시 |
|---|---|---|
| `-ExamplePath` | 워크스페이스 루트 기준 예제 경로 | `ESP32-C3-Embedded-Engineering\examples\01_mcsr` |
| `-Action` | 실행할 동작 | `build` / `flash` / `monitor` / `flash-monitor` |
| `-Port` | 시리얼 포트 | `COM4` |

**내부 동작 순서:**
1. `IDF_PATH`, `IDF_TOOLS_PATH` 등 환경변수 설정 (미설정 시 기본값 사용)
2. `C:\esp\v6.0.1\esp-idf\export.ps1` 실행 → IDF 툴체인 PATH 주입
3. 예제 폴더로 이동 (`Push-Location`)
4. `idf.py set-target esp32c3` 실행 (항상 올바른 타깃 보장)
5. `-Action`에 따라 `idf.py build` / `flash` / `monitor` / `flash monitor` 실행

**직접 실행 예시 (PowerShell):**
```powershell
.\build_example.ps1 -ExamplePath ESP32-C3-Embedded-Engineering\examples\01_mcsr -Action flash-monitor -Port COM4
```

---

### `.vscode/settings.json`

ESP-IDF VS Code 확장(Espressif IDF)의 경로 설정입니다.

```json
{
  "idf.espIdfPath": "C:/esp/v6.0.1/esp-idf",      // IDF 설치 경로
  "idf.toolsPath": "C:/Espressif/tools",            // 툴체인 루트
  "idf.pythonBinPath": "..../venv/Scripts/python.exe",
  "idf.portWin": "COM4",                            // 기본 시리얼 포트
  "idf.openOcdConfigs": ["board/esp32c3-builtin.cfg"], // 내장 USB-JTAG 사용
  "idf.customExtraVars": {
    "IDF_TARGET": "esp32c3",
    ...
  }
}
```

> `idf.openOcdConfigs`에 `board/esp32c3-builtin.cfg`를 지정하면  
> 외부 디버거 없이 USB 케이블 하나로 OpenOCD + GDB 디버깅이 가능합니다.

---

### `.vscode/tasks.json`

각 예제에 대한 Build / Flash / Monitor 태스크를 정의합니다.

**구조:**
```
Build XX      → build_example.ps1 -Action build
Flash XX      → build_example.ps1 -Action flash  (dependsOn: Build XX)
Monitor XX    → build_example.ps1 -Action monitor
Flash+Monitor → build_example.ps1 -Action flash-monitor  (dependsOn: Build XX)
```

- `problemMatcher: ["$gcc"]` — 빌드 에러를 VS Code Problems 패널에 표시
- `dependsOn` — Flash 태스크 실행 전 Build를 자동 선행 실행
- `panel: "dedicated"` — 태스크마다 별도 터미널 패널 사용

---

### `.vscode/c_cpp_properties.json`

C/C++ IntelliSense(자동완성, 오류 표시) 설정입니다.

```json
{
  "compilerPath": "...riscv32-esp-elf-gcc.exe",
  "compileCommands": ".../examples/02_uart_jtag/build/compile_commands.json"
}
```

- `compileCommands`가 핵심입니다. 빌드 시 생성되는 `compile_commands.json`에  
  모든 include 경로와 define이 이미 포함되어 있어 수동 설정이 불필요합니다.
- **예제를 전환하면 이 경로를 해당 예제의 `build/` 로 바꿔주면** IntelliSense가 갱신됩니다.
  ```json
  "compileCommands": "${workspaceFolder}/ESP32-C3-Embedded-Engineering/examples/01_mcsr/build/compile_commands.json"
  ```
- 빌드 전에는 `compile_commands.json`이 없으므로 IntelliSense 오류가 표시될 수 있습니다.  
  빌드 후 자동으로 해결됩니다.

---

### `.vscode/launch.json`

OpenOCD + GDB를 이용한 하드웨어 디버그 설정입니다.

```json
{
  "name": "Debug 01_mcsr",
  "program": ".../01_mcsr/build/mcsr.elf",  // 디버그할 ELF 파일
  "miDebuggerServerAddress": "localhost:3333", // OpenOCD GDB 서버 포트
  "miDebuggerPath": "...riscv32-esp-elf-gdb.exe",
  "preLaunchTask": "Build 01_mcsr"           // 디버그 전 자동 빌드
}
```

**디버그 사용 방법:**
1. PowerShell에서 OpenOCD 서버 먼저 실행:
   ```powershell
   openocd -f board/esp32c3-builtin.cfg
   ```
2. VS Code `F5` 또는 Run and Debug 패널에서 구성 선택 후 실행
3. 브레이크포인트, 변수 감시, 스텝 실행 가능

---

## 클린 빌드 방법

특정 예제의 `build/` 폴더와 `sdkconfig`를 삭제하면 됩니다:

```powershell
$ex = "D:\ESP_Projects\Embedded\Git_Embedded\ESP32-C3-Embedded-Engineering\examples\01_mcsr"
Remove-Item -Recurse -Force "$ex\build"
Remove-Item -Force "$ex\sdkconfig"
```

이후 Build 태스크를 실행하면 처음부터 재빌드됩니다.  
`sdkconfig.defaults`가 있는 경우 해당 설정이 자동 반영됩니다.

---

## 예제별 특이사항

### 01_mcsr — RISC-V CSR 접근

- 표준 `mcycle`(0xB00), `mcycleh`(0xB80), `minstret`(0xB02) CSR은 **ESP32-C3에 미구현** → 접근 시 Illegal Instruction 예외 발생
- `mcycle` 대신 ESP32-C3 전용 PCCR(0x7E2) CSR 사용
- 64비트 cycle 카운터는 소프트웨어 롤오버 감지로 구현 (32비트 PCCR 기준 ~26.8초 주기)

### 02_uart_jtag — USB CDC 콘솔

- ESP32-C3 내장 USB-Serial/JTAG (GPIO18/19) 사용 — 외부 USB-UART 칩 불필요
- `sdkconfig.defaults`에 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 설정 필요
  - 미설정 시 stdin이 HW UART0(GPIO20)으로 연결되어 모니터 키 입력이 전달되지 않음
- 콘솔 입력: `getchar()` / 출력: `printf()` + `fflush(stdout)`
- `uart_read_bytes(UART_NUM_0)`는 GPIO20 핀을 읽으므로 USB CDC 입력과 무관
