#!/bin/bash

# DroneBridge ESP32 Local Build Script
# This script builds the firmware for a specific ESP32 board configuration
# 
# Usage: ./build_local.sh <model> [options]
# Example: ./build_local.sh esp32c3_official
#
# Build artifacts are stored in: build/tmp/

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SCRIPT_DIR/tmp"

# Print usage
print_usage() {
    echo -e "${BLUE}DroneBridge ESP32 Local Build Script${NC}"
    echo ""
    echo "Usage: $0 <model> [options]"
    echo ""
    echo "Available models:"
    echo "  esp32                        - ESP32 (standard)"
    echo "  esp32s2                      - ESP32-S2"
    echo "  esp32s2_noUARTConsole        - ESP32-S2 (no UART console)"
    echo "  esp32s3                      - ESP32-S3"
    echo "  esp32s3_noUARTConsole        - ESP32-S3 (no UART console)"
    echo "  esp32s3_usb_serial           - ESP32-S3 (USB Serial via JTAG)"
    echo "  esp32s3_usb_host             - ESP32-S3 (USB Host for FCU)"
    echo "  esp32c3                      - ESP32-C3"
    echo "  esp32c3_official             - ESP32-C3 (official board)"
    echo "  esp32c3_usb_serial           - ESP32-C3 (USB Serial via JTAG)"
    echo "  esp32c3_noUARTConsole        - ESP32-C3 (no UART console)"
    echo "  esp32c6                      - ESP32-C6"
    echo "  esp32c6_official             - ESP32-C6 (official board)"
    echo "  esp32c6_official_usb_serial  - ESP32-C6 official (USB Serial via JTAG)"
    echo "  esp32c6_official_noUARTConsole - ESP32-C6 official (no UART console)"
    echo "  esp32c6_usb_serial           - ESP32-C6 (USB Serial via JTAG)"
    echo "  esp32c6_noUARTConsole        - ESP32-C6 (no UART console)"
    echo ""
    echo "Options:"
    echo "  --clean                      - Clean build (fullclean before build)"
    echo "  --flash                      - Flash after successful build"
    echo "  --monitor                    - Start monitor after flash"
    echo "  -h, --help                   - Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 esp32c3_official"
    echo "  $0 esp32s3 --clean"
    echo "  $0 esp32c6_official --clean --flash --monitor"
    echo ""
    echo "Note: Build artifacts are stored in: build/tmp/"
}

# Map model name to sdkconfig file
get_sdkconfig() {
    local model=$1
    case $model in
        esp32) echo "sdkconfig_esp32" ;;
        esp32s2) echo "sdkconfig_s2" ;;
        esp32s2_noUARTConsole) echo "sdkconfig_s2_noUARTConsole" ;;
        esp32s3) echo "sdkconfig_s3" ;;
        esp32s3_noUARTConsole) echo "sdkconfig_s3_noUARTConsole" ;;
        esp32s3_usb_serial) echo "sdkconfig_s3_serial_via_JTAG" ;;
        esp32s3_usb_host) echo "sdkconfig_s3_usb_host" ;;
        esp32c3) echo "sdkconfig_c3" ;;
        esp32c3_official) echo "sdkconfig_c3_official" ;;
        esp32c3_usb_serial) echo "sdkconfig_c3_serial_via_JTAG" ;;
        esp32c3_noUARTConsole) echo "sdkconfig_c3_noUARTConsole" ;;
        esp32c6) echo "sdkconfig_c6" ;;
        esp32c6_official) echo "sdkconfig_c6_official" ;;
        esp32c6_official_usb_serial) echo "sdkconfig_c6_official_serial_via_JTAG" ;;
        esp32c6_official_noUARTConsole) echo "sdkconfig_c6_official_noUARTConsole" ;;
        esp32c6_usb_serial) echo "sdkconfig_c6_serial_via_JTAG" ;;
        esp32c6_noUARTConsole) echo "sdkconfig_c6_noUARTConsole" ;;
        *) echo "" ;;
    esac
}

# Check if model is provided
if [ $# -eq 0 ]; then
    echo -e "${RED}Error: No model specified${NC}"
    echo ""
    print_usage
    exit 1
fi

# Parse arguments
MODEL=""
DO_CLEAN=false
DO_FLASH=false
DO_MONITOR=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            print_usage
            exit 0
            ;;
        --clean)
            DO_CLEAN=true
            shift
            ;;
        --flash)
            DO_FLASH=true
            shift
            ;;
        --monitor)
            DO_MONITOR=true
            shift
            ;;
        *)
            if [ -z "$MODEL" ]; then
                MODEL=$1
            else
                echo -e "${RED}Error: Unknown argument: $1${NC}"
                print_usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Get sdkconfig file for the model
SDKCONFIG=$(get_sdkconfig "$MODEL")

if [ -z "$SDKCONFIG" ]; then
    echo -e "${RED}Error: Invalid model '$MODEL'${NC}"
    echo ""
    print_usage
    exit 1
fi

# Check if sdkconfig file exists
if [ ! -f "$PROJECT_ROOT/$SDKCONFIG" ]; then
    echo -e "${RED}Error: Configuration file '$SDKCONFIG' not found${NC}"
    exit 1
fi

# Check if ESP-IDF is available
if ! command -v idf.py &> /dev/null; then
    echo -e "${RED}Error: ESP-IDF not found. Please source the ESP-IDF environment first:${NC}"
    echo -e "${YELLOW}  source \$IDF_PATH/export.sh${NC}"
    echo ""
    echo -e "${YELLOW}Or use Docker build instead (no ESP-IDF installation needed):${NC}"
    echo -e "${BLUE}  ./build_docker.sh $MODEL${NC}"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}DroneBridge ESP32 Local Build${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Model:${NC} $MODEL"
echo -e "${GREEN}Config:${NC} $SDKCONFIG"
echo -e "${GREEN}Clean build:${NC} $DO_CLEAN"
echo -e "${GREEN}Flash after build:${NC} $DO_FLASH"
echo -e "${GREEN}Monitor after flash:${NC} $DO_MONITOR"
echo -e "${GREEN}Build artifacts:${NC} build/tmp/"
echo -e "${BLUE}========================================${NC}"
echo ""

# Create tmp directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Change to project directory
cd "$PROJECT_ROOT"

# Clean if requested - ONLY clean build/tmp directory, never the scripts!
if [ "$DO_CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory (build/tmp)...${NC}"
    # Safety check: only delete if we're in the right place
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"/*
        echo -e "${GREEN}Cleaned: $BUILD_DIR${NC}"
    fi
    # Also clean any build artifacts in project root
    rm -f sdkconfig
    rm -f sdkconfig.old
    echo ""
fi

# Copy sdkconfig
echo -e "${YELLOW}Copying configuration file...${NC}"
# Special handling for USB Host configs - they are override files, not complete configs
if [ "$SDKCONFIG" = "sdkconfig_s3_usb_host" ]; then
    # Copy base sdkconfig_s3 first, then apply USB Host overrides
    if [ ! -f "$PROJECT_ROOT/sdkconfig_s3" ]; then
        echo -e "${RED}Error: Base configuration file 'sdkconfig_s3' not found${NC}"
        exit 1
    fi
    cp "$PROJECT_ROOT/sdkconfig_s3" sdkconfig
    echo -e "${GREEN}Copied base config: sdkconfig_s3${NC}"
    # Apply USB Host overrides
    cat "$PROJECT_ROOT/$SDKCONFIG" >> sdkconfig
    echo -e "${GREEN}Applied USB Host overrides from: $SDKCONFIG${NC}"
else
    cp "$PROJECT_ROOT/$SDKCONFIG" sdkconfig
    echo -e "${GREEN}Copied: $SDKCONFIG${NC}"
fi
echo ""

# Build with custom build directory
echo -e "${YELLOW}Building firmware...${NC}"
# Reconfigure if sdkconfig was modified (especially important for USB Host configs)
if [ "$SDKCONFIG" = "sdkconfig_s3_usb_host" ]; then
    echo -e "${YELLOW}Reconfiguring build system for USB Host...${NC}"
    idf.py -B "$BUILD_DIR" reconfigure
fi
idf.py -B "$BUILD_DIR" build

if [ $? -eq 0 ]; then
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo -e "${GREEN}Binary files location:${NC}"
    echo -e "  Firmware:      ${BLUE}$BUILD_DIR/db_esp32.bin${NC}"
    echo -e "  Bootloader:    ${BLUE}$BUILD_DIR/bootloader/bootloader.bin${NC}"
    echo -e "  Partition:     ${BLUE}$BUILD_DIR/partition_table/partition-table.bin${NC}"
    echo -e "  Web interface: ${BLUE}$BUILD_DIR/www.bin${NC}"
    echo -e "  Flash args:    ${BLUE}$BUILD_DIR/flash_args${NC}"
    echo ""
    
    # Flash if requested
    if [ "$DO_FLASH" = true ]; then
        echo -e "${YELLOW}Flashing firmware...${NC}"
        echo -e "${YELLOW}Detecting ESP32 device...${NC}"
        
        # Detect ESP32 USB devices (filter out non-ESP devices)
        ESP_DEVICES=$(ls /dev/{ttyUSB*,ttyACM*,cu.usbserial*,cu.SLAB_USBtoUART,cu.wchusbserial*} 2>/dev/null | grep -v "Bluetooth\|Buds\|debug-console\|SRS-" || true)
        
        if [ -z "$ESP_DEVICES" ]; then
            echo -e "${RED}No ESP32 device found!${NC}"
            echo -e "${YELLOW}Please connect your ESP32 board and try again.${NC}"
            echo -e "${YELLOW}Looking for devices like: /dev/ttyUSB*, /dev/ttyACM*, /dev/cu.usbserial*${NC}"
            exit 1
        fi
        
        # Use the first detected ESP device
        ESP_PORT=$(echo "$ESP_DEVICES" | head -1)
        echo -e "${GREEN}Found ESP32 device: ${BLUE}$ESP_PORT${NC}"
        echo ""
        
        # Try flashing with retries
        MAX_RETRIES=3
        RETRY_COUNT=0
        FLASH_SUCCESS=false
        
        while [ $RETRY_COUNT -lt $MAX_RETRIES ] && [ "$FLASH_SUCCESS" = false ]; do
            if [ $RETRY_COUNT -gt 0 ]; then
                echo -e "${YELLOW}Retry attempt $RETRY_COUNT of $((MAX_RETRIES-1))...${NC}"
                echo -e "${YELLOW}Hold the BOOT button on ESP32 if flashing fails${NC}"
                sleep 2
            fi
            
            idf.py -B "$BUILD_DIR" -p "$ESP_PORT" flash
            
            if [ $? -eq 0 ]; then
                FLASH_SUCCESS=true
                echo -e "${GREEN}Flash successful!${NC}"
                echo ""
            else
                RETRY_COUNT=$((RETRY_COUNT + 1))
                if [ $RETRY_COUNT -lt $MAX_RETRIES ]; then
                    echo -e "${YELLOW}Flash failed, retrying...${NC}"
                fi
            fi
        done
        
        if [ "$FLASH_SUCCESS" = false ]; then
            echo -e "${RED}Flash failed after $MAX_RETRIES attempts!${NC}"
            echo -e "${YELLOW}Troubleshooting tips:${NC}"
            echo -e "  1. Hold the BOOT button while connecting"
            echo -e "  2. Check USB cable (use data cable, not charge-only)"
            echo -e "  3. Try different USB port"
            echo -e "  4. Install USB drivers: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/establish-serial-connection.html"
            exit 1
        fi
        
        # Monitor if requested
        if [ "$DO_MONITOR" = true ]; then
            echo -e "${YELLOW}Starting monitor...${NC}"
            idf.py -B "$BUILD_DIR" -p "$ESP_PORT" monitor
        fi
    else
        echo -e "${YELLOW}To flash the firmware, run:${NC}"
        echo -e "  ${BLUE}idf.py -B build/tmp flash${NC}"
        echo ""
        echo -e "${YELLOW}Or rebuild with:${NC}"
        echo -e "  ${BLUE}$0 $MODEL --flash${NC}"
        echo ""
    fi
else
    echo ""
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Build failed!${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi

