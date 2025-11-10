# DroneBridge ESP32 Local Build Script (PowerShell)
# This script builds the firmware for a specific ESP32 board configuration
# 
# Usage: .\build_local.ps1 <model>
# Example: .\build_local.ps1 esp32c3_official
#
# Build artifacts are stored in: build\tmp\

param(
    [Parameter(Position=0, Mandatory=$false)]
    [string]$Model,
    
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

# Script directory and project root
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ScriptDir "tmp"

# Print usage
function Print-Usage {
    Write-Host "DroneBridge ESP32 Local Build Script" -ForegroundColor Blue
    Write-Host ""
    Write-Host "Usage: .\build_local.ps1 <model> [options]"
    Write-Host ""
    Write-Host "Available models:"
    Write-Host "  esp32                        - ESP32 (standard)"
    Write-Host "  esp32s2                      - ESP32-S2"
    Write-Host "  esp32s2_noUARTConsole        - ESP32-S2 (no UART console)"
    Write-Host "  esp32s3                      - ESP32-S3"
    Write-Host "  esp32s3_noUARTConsole        - ESP32-S3 (no UART console)"
    Write-Host "  esp32s3_usb_serial           - ESP32-S3 (USB Serial via JTAG)"
    Write-Host "  esp32s3_usb_host             - ESP32-S3 (USB Host for FCU)"
    Write-Host "  esp32c3                      - ESP32-C3"
    Write-Host "  esp32c3_official             - ESP32-C3 (official board)"
    Write-Host "  esp32c3_usb_serial           - ESP32-C3 (USB Serial via JTAG)"
    Write-Host "  esp32c3_noUARTConsole        - ESP32-C3 (no UART console)"
    Write-Host "  esp32c6                      - ESP32-C6"
    Write-Host "  esp32c6_official             - ESP32-C6 (official board)"
    Write-Host "  esp32c6_official_usb_serial  - ESP32-C6 official (USB Serial via JTAG)"
    Write-Host "  esp32c6_official_noUARTConsole - ESP32-C6 official (no UART console)"
    Write-Host "  esp32c6_usb_serial           - ESP32-C6 (USB Serial via JTAG)"
    Write-Host "  esp32c6_noUARTConsole        - ESP32-C6 (no UART console)"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Clean                       - Clean build (fullclean before build)"
    Write-Host "  -Flash                       - Flash after successful build"
    Write-Host "  -Monitor                     - Start monitor after flash"
    Write-Host "  -Help                        - Show this help message"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\build_local.ps1 esp32c3_official"
    Write-Host "  .\build_local.ps1 esp32s3 -Clean"
    Write-Host "  .\build_local.ps1 esp32c6_official -Clean -Flash -Monitor"
    Write-Host ""
    Write-Host "Note: Build artifacts are stored in build\tmp\"
}

# Map model name to sdkconfig file
function Get-SdkConfig {
    param([string]$ModelName)

    switch ($ModelName) {
        "esp32" { return "sdkconfig_esp32" }
        "esp32s2" { return "sdkconfig_s2" }
        "esp32s2_noUARTConsole" { return "sdkconfig_s2_noUARTConsole" }
        "esp32s3" { return "sdkconfig_s3" }
        "esp32s3_noUARTConsole" { return "sdkconfig_s3_noUARTConsole" }
        "esp32s3_usb_serial" { return "sdkconfig_s3_serial_via_JTAG" }
        "esp32s3_usb_host" { return "sdkconfig_s3_usb_host" }
        "esp32c3" { return "sdkconfig_c3" }
        "esp32c3_official" { return "sdkconfig_c3_official" }
        "esp32c3_usb_serial" { return "sdkconfig_c3_serial_via_JTAG" }
        "esp32c3_noUARTConsole" { return "sdkconfig_c3_noUARTConsole" }
        "esp32c6" { return "sdkconfig_c6" }
        "esp32c6_official" { return "sdkconfig_c6_official" }
        "esp32c6_official_usb_serial" { return "sdkconfig_c6_official_serial_via_JTAG" }
        "esp32c6_official_noUARTConsole" { return "sdkconfig_c6_official_noUARTConsole" }
        "esp32c6_usb_serial" { return "sdkconfig_c6_serial_via_JTAG" }
        "esp32c6_noUARTConsole" { return "sdkconfig_c6_noUARTConsole" }
        default { return "" }
    }
}

# Show help if requested or no model provided
if ($Help -or -not $Model) {
    if (-not $Model) {
        Write-Host "Error: No model specified" -ForegroundColor Red
        Write-Host ""
    }
    Print-Usage
    exit $(if ($Help) { 0 } else { 1 })
}

# Get sdkconfig file for the model
$SdkConfig = Get-SdkConfig -ModelName $Model

if (-not $SdkConfig) {
    Write-Host "Error: Invalid model '$Model'" -ForegroundColor Red
    Write-Host ""
    Print-Usage
    exit 1
}

# Check if sdkconfig file exists
$SdkConfigPath = Join-Path $ProjectRoot $SdkConfig
if (-not (Test-Path $SdkConfigPath)) {
    Write-Host "Error: Configuration file '$SdkConfig' not found" -ForegroundColor Red
    exit 1
}

# Check if ESP-IDF is available
$IdfPyCmd = Get-Command "idf.py" -ErrorAction SilentlyContinue
if (-not $IdfPyCmd) {
    Write-Host "Error: ESP-IDF not found. Please run the ESP-IDF environment setup first:" -ForegroundColor Red
    Write-Host "  For PowerShell: $env:IDF_PATH\export.ps1" -ForegroundColor Yellow
    Write-Host "  For CMD: %IDF_PATH%\export.bat" -ForegroundColor Yellow
    exit 1
}

Write-Host "========================================" -ForegroundColor Blue
Write-Host "DroneBridge ESP32 Local Build" -ForegroundColor Blue
Write-Host "========================================" -ForegroundColor Blue
Write-Host "Model: " -NoNewline -ForegroundColor Green
Write-Host $Model
Write-Host "Config: " -NoNewline -ForegroundColor Green
Write-Host $SdkConfig
Write-Host "Clean build: " -NoNewline -ForegroundColor Green
Write-Host $Clean
Write-Host "Flash after build: " -NoNewline -ForegroundColor Green
Write-Host $Flash
Write-Host "Monitor after flash: " -NoNewline -ForegroundColor Green
Write-Host $Monitor
Write-Host "Build artifacts: " -NoNewline -ForegroundColor Green
Write-Host "build\tmp\"
Write-Host "========================================" -ForegroundColor Blue
Write-Host ""

# Create tmp directory if it doesn't exist
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Change to project directory
Set-Location $ProjectRoot

# Clean if requested - ONLY clean build/tmp directory, never the scripts!
if ($Clean) {
    Write-Host "Cleaning build directory (build\tmp)..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        Remove-Item -Path "$BuildDir\*" -Recurse -Force
        Write-Host "Cleaned: $BuildDir" -ForegroundColor Green
    }
    # Also clean any build artifacts in project root
    if (Test-Path "sdkconfig") { Remove-Item "sdkconfig" -Force }
    if (Test-Path "sdkconfig.old") { Remove-Item "sdkconfig.old" -Force }
    Write-Host ""
}

# Copy sdkconfig
Write-Host "Copying configuration file..." -ForegroundColor Yellow
Copy-Item $SdkConfigPath -Destination (Join-Path $ProjectRoot "sdkconfig") -Force
Write-Host ""

# Build with custom build directory
Write-Host "Building firmware..." -ForegroundColor Yellow
idf.py -B $BuildDir build

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Binary files location:" -ForegroundColor Green
    Write-Host "  Firmware:      " -NoNewline
    Write-Host "$BuildDir\db_esp32.bin" -ForegroundColor Blue
    Write-Host "  Bootloader:    " -NoNewline
    Write-Host "$BuildDir\bootloader\bootloader.bin" -ForegroundColor Blue
    Write-Host "  Partition:     " -NoNewline
    Write-Host "$BuildDir\partition_table\partition-table.bin" -ForegroundColor Blue
    Write-Host "  Web interface: " -NoNewline
    Write-Host "$BuildDir\www.bin" -ForegroundColor Blue
    Write-Host "  Flash args:    " -NoNewline
    Write-Host "$BuildDir\flash_args" -ForegroundColor Blue
    Write-Host ""
    
    # Flash if requested
    if ($Flash) {
        Write-Host "Flashing firmware..." -ForegroundColor Yellow
        idf.py -B $BuildDir flash

        if ($LASTEXITCODE -eq 0) {
            Write-Host "Flash successful!" -ForegroundColor Green
            Write-Host ""

            # Monitor if requested
            if ($Monitor) {
                Write-Host "Starting monitor..." -ForegroundColor Yellow
                idf.py -B $BuildDir monitor
            }
        } else {
            Write-Host "Flash failed!" -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "To flash the firmware, run:" -ForegroundColor Yellow
        Write-Host "  idf.py -B build\tmp flash" -ForegroundColor Blue
        Write-Host ""
        Write-Host "Or rebuild with:" -ForegroundColor Yellow
        Write-Host "  .\build_local.ps1 $Model -Flash" -ForegroundColor Blue
        Write-Host ""
    }
} else {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "Build failed!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit 1
}

