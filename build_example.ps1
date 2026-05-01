<#
Usage examples:
  .\build_example.ps1 -ExamplePath examples/04_queues -Action build
  .\build_example.ps1 -ExamplePath examples/04_queues -Action flash -Port COM3
  .\build_example.ps1 -ExamplePath examples/04_queues -Action flash-monitor -Port COM3
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$ExamplePath,

    [ValidateSet("build","flash","monitor","flash-monitor")]
    [string]$Action = "build",

    [string]$Port = $env:COMPORT
)

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Ensure common IDF env vars are set if not provided by the caller
if (-not $env:IDF_PATH -or $env:IDF_PATH -eq '') { $env:IDF_PATH = 'C:\esp\v6.0.1\esp-idf' }
if (-not $env:IDF_PYTHON_ENV_PATH -or $env:IDF_PYTHON_ENV_PATH -eq '') { $env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0.1\venv' }
if (-not $env:IDF_TOOLS_PATH -or $env:IDF_TOOLS_PATH -eq '') { $env:IDF_TOOLS_PATH = 'C:\Espressif\tools' }
if (-not $env:ESP_ROM_ELF_DIR -or $env:ESP_ROM_ELF_DIR -eq '') { $env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011' }

# Try to initialize ESP-IDF environment for this PowerShell session
if (Test-Path (Join-Path $env:IDF_PATH 'export.ps1')) {
    try {
        & (Join-Path $env:IDF_PATH 'export.ps1')
    } catch {
        Write-Warning "Failed to run export.ps1: $_"
    }
} elseif (Test-Path (Join-Path $env:IDF_PATH 'export.bat')) {
    # Fallback to export.bat via cmd
    cmd.exe /c "`"$env:IDF_PATH\export.bat`""
}
$fullPath = Resolve-Path -LiteralPath (Join-Path $root $ExamplePath) -ErrorAction SilentlyContinue
if (-not $fullPath) {
    Write-Error "Example path not found: $ExamplePath (from $root)"
    exit 1
}
Push-Location $fullPath

switch ($Action) {
    'build' {
        # set-target only on explicit build; it wipes the build dir if run unconditionally
        if (-not (Test-Path "build\CMakeCache.txt")) {
            Try { idf.py set-target esp32c3 } Catch { }
        }
        idf.py build
    }
    'flash' {
        if ($Port) { idf.py -p $Port --no-build flash } else { idf.py --no-build flash }
    }
    'monitor' {
        if ($Port) { idf.py -p $Port monitor } else { idf.py monitor }
    }
    'flash-monitor' {
        if ($Port) { idf.py -p $Port --no-build flash monitor } else { idf.py --no-build flash monitor }
    }
}

Pop-Location
