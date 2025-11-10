#!/bin/bash

# DroneBridge ESP32 Docker Build Script
# This script builds the firmware using Docker (no local ESP-IDF installation needed)
# 
# Usage: ./build_docker.sh <model> [options]
# Example: ./build_docker.sh esp32c3_official
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

# Docker image name
DOCKER_IMAGE="dronebridge-esp32-build:latest"
CONTAINER_NAME="dronebridge-esp32-builder-temp"

# Print usage
print_usage() {
    echo -e "${BLUE}DroneBridge ESP32 Docker Build Script${NC}"
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
    echo "  --build-image                - Build/rebuild the Docker image"
    echo "  --shell                      - Open interactive shell in container"
    echo "  -h, --help                   - Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 esp32c3_official"
    echo "  $0 esp32s3 --clean"
    echo "  $0 esp32c6_official --clean --flash"
    echo "  $0 --build-image             # Build Docker image first"
    echo "  $0 --shell                   # Interactive shell"
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

# Build Docker image
build_docker_image() {
    echo -e "${YELLOW}Building Docker image...${NC}"
    cd "$SCRIPT_DIR"
    docker build -t "$DOCKER_IMAGE" .
    echo -e "${GREEN}Docker image built successfully!${NC}"
    echo ""
}

# Check if Docker is installed
check_docker() {
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}Error: Docker is not installed${NC}"
        echo ""
        echo "Please install Docker:"
        echo "  macOS:   https://docs.docker.com/desktop/install/mac-install/"
        echo "  Linux:   https://docs.docker.com/engine/install/"
        echo "  Windows: https://docs.docker.com/desktop/install/windows-install/"
        exit 1
    fi
}

# Check if Docker image exists
check_docker_image() {
    if ! docker image inspect "$DOCKER_IMAGE" &> /dev/null; then
        echo -e "${YELLOW}Docker image not found. Building it now...${NC}"
        build_docker_image
    fi
}

# Parse arguments
MODEL=""
DO_CLEAN=false
DO_FLASH=false
BUILD_IMAGE=false
OPEN_SHELL=false

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
        --build-image)
            BUILD_IMAGE=true
            shift
            ;;
        --shell)
            OPEN_SHELL=true
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

# Check Docker installation
check_docker

# Build image if requested
if [ "$BUILD_IMAGE" = true ]; then
    build_docker_image
    if [ -z "$MODEL" ] && [ "$OPEN_SHELL" = false ]; then
        exit 0
    fi
fi

# Open shell if requested
if [ "$OPEN_SHELL" = true ]; then
    check_docker_image
    echo -e "${BLUE}Opening interactive shell in Docker container...${NC}"
    echo -e "${YELLOW}You're now inside the build environment. Use 'exit' to leave.${NC}"
    echo ""
    docker run --rm -it \
        -v "$PROJECT_ROOT:/project" \
        -w /project \
        "$DOCKER_IMAGE" \
        /bin/bash
    exit 0
fi

# Check if model is provided
if [ -z "$MODEL" ]; then
    echo -e "${RED}Error: No model specified${NC}"
    echo ""
    print_usage
    exit 1
fi

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

# Check Docker image
check_docker_image

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}DroneBridge ESP32 Docker Build${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Model:${NC} $MODEL"
echo -e "${GREEN}Config:${NC} $SDKCONFIG"
echo -e "${GREEN}Clean build:${NC} $DO_CLEAN"
echo -e "${GREEN}Flash after build:${NC} $DO_FLASH"
echo -e "${GREEN}Docker image:${NC} $DOCKER_IMAGE"
echo -e "${GREEN}Build artifacts:${NC} build/tmp/"
echo -e "${BLUE}========================================${NC}"
echo ""

# Create tmp directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Build command - handle override files (like sdkconfig_s3_usb_host)
BUILD_CMD=""
if [ "$DO_CLEAN" = true ]; then
    BUILD_CMD="rm -rf build/tmp/* && rm -f sdkconfig sdkconfig.old && "
fi

# Handle override files (e.g., sdkconfig_s3_usb_host needs base sdkconfig_s3)
# Note: Inside Docker, project root is /project
if [ "$SDKCONFIG" = "sdkconfig_s3_usb_host" ]; then
    BUILD_CMD="${BUILD_CMD}cp /project/sdkconfig_s3 sdkconfig && cat /project/$SDKCONFIG >> sdkconfig"
else
    BUILD_CMD="${BUILD_CMD}cp /project/$SDKCONFIG sdkconfig"
fi
BUILD_CMD="$BUILD_CMD && idf.py -B build/tmp build"

# Run build in Docker
echo -e "${YELLOW}Running build in Docker container...${NC}"
docker run --rm \
    -v "$PROJECT_ROOT:/project" \
    -w /project \
    --name "$CONTAINER_NAME" \
    "$DOCKER_IMAGE" \
    /bin/bash -c "$BUILD_CMD"

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
    echo ""
    
    # Flash if requested
    if [ "$DO_FLASH" = true ]; then
        echo -e "${YELLOW}Flashing firmware...${NC}"
        echo -e "${YELLOW}Detecting ESP32 device...${NC}"
        
        # Detect ESP32 USB devices (filter out non-ESP devices like Bluetooth)
        ESP_DEVICES=$(ls /dev/{ttyUSB*,ttyACM*,cu.usbserial*,cu.SLAB_USBtoUART,cu.wchusbserial*} 2>/dev/null | grep -v "Bluetooth\|Buds\|debug-console\|SRS-" || true)
        
        if [ -z "$ESP_DEVICES" ]; then
            echo -e "${RED}No ESP32 device found!${NC}"
            echo -e "${YELLOW}Please connect your ESP32 board and try again.${NC}"
            echo -e "${YELLOW}Looking for devices like: /dev/ttyUSB*, /dev/ttyACM*, /dev/cu.usbserial*${NC}"
            echo ""
            echo -e "${YELLOW}To flash manually:${NC}"
            echo -e "  ${BLUE}docker run --rm -v \"$PROJECT_ROOT:/project\" -w /project --device=/dev/ttyUSB0:/dev/ttyUSB0 $DOCKER_IMAGE idf.py -B build/tmp flash -p /dev/ttyUSB0${NC}"
            exit 1
        fi
        
        # Use the first detected ESP device
        USB_DEVICE=$(echo "$ESP_DEVICES" | head -1)
        echo -e "${GREEN}Found ESP32 device: ${BLUE}$USB_DEVICE${NC}"
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
            
            docker run --rm \
                -v "$PROJECT_ROOT:/project" \
                -w /project \
                --device="$USB_DEVICE:$USB_DEVICE" \
                "$DOCKER_IMAGE" \
                /bin/bash -c "idf.py -B build/tmp flash -p $USB_DEVICE"
            
            if [ $? -eq 0 ]; then
                FLASH_SUCCESS=true
                echo -e "${GREEN}Flash successful!${NC}"
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
    else
        echo -e "${YELLOW}To flash the firmware:${NC}"
        echo -e "  ${BLUE}docker run --rm -v \"$PROJECT_ROOT:/project\" -w /project --device=/dev/ttyUSB0:/dev/ttyUSB0 $DOCKER_IMAGE idf.py -B build/tmp flash -p /dev/ttyUSB0${NC}"
        echo ""
        echo -e "${YELLOW}Or use the native script if ESP-IDF is installed:${NC}"
        echo -e "  ${BLUE}idf.py -B build/tmp flash${NC}"
        echo ""
    fi
else
    echo ""
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Build failed!${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
