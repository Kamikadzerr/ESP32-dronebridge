#!/bin/bash

# DroneBridge ESP32 Setup Checker
# This script helps you determine which build method to use

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo ""
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo -e "${BLUE}  DroneBridge ESP32 Build Setup Checker${NC}"
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo ""

# Check Docker
echo -e "${CYAN}Checking for Docker...${NC}"
if command -v docker &> /dev/null; then
    DOCKER_VERSION=$(docker --version 2>&1)
    echo -e "${GREEN}✓ Docker is installed:${NC} $DOCKER_VERSION"
    DOCKER_AVAILABLE=true
else
    echo -e "${RED}✗ Docker is NOT installed${NC}"
    DOCKER_AVAILABLE=false
fi
echo ""

# Check ESP-IDF
echo -e "${CYAN}Checking for ESP-IDF...${NC}"
if [ -n "$IDF_PATH" ] && [ -d "$IDF_PATH" ]; then
    echo -e "${GREEN}✓ IDF_PATH is set:${NC} $IDF_PATH"
    
    if command -v idf.py &> /dev/null; then
        IDF_VERSION=$(idf.py --version 2>&1 | grep -oP "ESP-IDF v\K[0-9.]+")
        echo -e "${GREEN}✓ idf.py is available:${NC} ESP-IDF v$IDF_VERSION"
        ESP_IDF_AVAILABLE=true
    else
        echo -e "${YELLOW}⚠ IDF_PATH is set but idf.py not found${NC}"
        echo -e "${YELLOW}  Try running: source \$IDF_PATH/export.sh${NC}"
        ESP_IDF_AVAILABLE=false
    fi
else
    echo -e "${RED}✗ ESP-IDF is NOT installed (IDF_PATH not set)${NC}"
    ESP_IDF_AVAILABLE=false
fi
echo ""

# Recommendations
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo -e "${BLUE}  Recommendation:${NC}"
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo ""

if [ "$DOCKER_AVAILABLE" = true ]; then
    echo -e "${GREEN}✓ RECOMMENDED: Use Docker Build${NC}"
    echo ""
    echo -e "  ${CYAN}Quick Start:${NC}"
    echo -e "  ${YELLOW}./build_docker.sh --build-image${NC}    # First time only (10 min)"
    echo -e "  ${YELLOW}./build_docker.sh esp32c3_official${NC} # Build firmware"
    echo ""
    echo -e "  ${CYAN}Advantages:${NC}"
    echo -e "  • No ESP-IDF installation needed"
    echo -e "  • Consistent build environment"
    echo -e "  • Quick setup"
    echo -e "  • Build artifacts in build/tmp/ (safe)"
    echo ""
    
    if [ "$ESP_IDF_AVAILABLE" = true ]; then
        echo -e "${GREEN}✓ ALTERNATIVE: Use Native Build${NC}"
        echo ""
        echo -e "  ${CYAN}Quick Start:${NC}"
        echo -e "  ${YELLOW}source \$IDF_PATH/export.sh${NC}         # Activate ESP-IDF"
        echo -e "  ${YELLOW}./build_local.sh esp32c3_official${NC}   # Build firmware"
        echo ""
        echo -e "  ${CYAN}Advantages:${NC}"
        echo -e "  • Faster builds (no Docker overhead)"
        echo -e "  • Better for active development"
        echo -e "  • Build artifacts in build/tmp/ (safe)"
        echo ""
    fi
    
elif [ "$ESP_IDF_AVAILABLE" = true ]; then
    echo -e "${GREEN}✓ Use Native Build${NC}"
    echo ""
    echo -e "  ${CYAN}Quick Start:${NC}"
    echo -e "  ${YELLOW}source \$IDF_PATH/export.sh${NC}         # Activate ESP-IDF"
    echo -e "  ${YELLOW}./build_local.sh esp32c3_official${NC}   # Build firmware"
    echo ""
    echo -e "  ${YELLOW}Consider installing Docker for easier builds!${NC}"
    echo -e "  Download from: https://www.docker.com/get-started"
    echo ""
    
else
    echo -e "${RED}⚠ WARNING: Neither Docker nor ESP-IDF is available!${NC}"
    echo ""
    echo -e "${CYAN}You have two options:${NC}"
    echo ""
    echo -e "${GREEN}Option 1: Install Docker (RECOMMENDED - Easier & Faster)${NC}"
    echo -e "  • macOS: https://www.docker.com/products/docker-desktop"
    echo -e "  • Linux: ${YELLOW}curl -fsSL https://get.docker.com | sh${NC}"
    echo -e "  • Windows: https://www.docker.com/products/docker-desktop"
    echo -e "  Setup time: ~10 minutes"
    echo ""
    echo -e "${YELLOW}Option 2: Install ESP-IDF (Advanced Users)${NC}"
    echo -e "  • Follow: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/"
    echo -e "  Setup time: ~30-60 minutes"
    echo ""
fi

echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}Important:${NC}"
echo -e "  Build artifacts are stored in ${GREEN}build/tmp/${NC}"
echo -e "  Your scripts are always safe from deletion!"
echo ""
echo -e "${CYAN}For more details, see:${NC}"
echo -e "  • QUICKSTART.md - Fast start guide"
echo -e "  • README.md - Complete documentation"
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo ""
