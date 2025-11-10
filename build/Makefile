.PHONY: help docker-build docker-shell clean-docker clean build-% flash-% docker-build-% docker-flash-%

# Default target
help:
	@echo "DroneBridge ESP32 Build Makefile"
	@echo ""
	@echo "Quick Start (Fresh System):"
	@echo "  make docker-build        - Build Docker image (first time only)"
	@echo "  make build-esp32c3       - Build firmware for ESP32-C3 official"
	@echo ""
	@echo "Docker Targets:"
	@echo "  make docker-build        - Build/rebuild Docker image"
	@echo "  make docker-shell        - Open interactive shell in container"
	@echo "  make clean-docker        - Remove Docker image"
	@echo ""
	@echo "Build Targets (use docker- prefix for Docker builds):"
	@echo "  make build-MODEL         - Build for specific model"
	@echo "  make docker-build-MODEL  - Build using Docker"
	@echo ""
	@echo "Flash Targets:"
	@echo "  make flash-MODEL         - Build and flash"
	@echo "  make docker-flash-MODEL  - Build and flash using Docker"
	@echo ""
	@echo "Available Models:"
	@echo "  esp32, esp32s2, esp32s3, esp32c3, esp32c6"
	@echo "  esp32c3_official, esp32c6_official (recommended)"
	@echo ""
	@echo "Examples:"
	@echo "  make build-esp32c3_official"
	@echo "  make docker-build-esp32c3_official"
	@echo "  make flash-esp32c6_official"
	@echo "  make docker-flash-esp32s3"
	@echo ""
	@echo "Clean:"
	@echo "  make clean               - Clean build/tmp directory"
	@echo ""
	@echo "Note: Build artifacts are stored in build/tmp/"

# Docker image management
docker-build:
	./build_docker.sh --build-image

docker-shell:
	./build_docker.sh --shell

clean-docker:
	docker rmi dronebridge-esp32-build:latest 2>/dev/null || true

# Pattern rules for building with Docker
docker-build-%:
	./build_docker.sh $* --clean

docker-flash-%:
	./build_docker.sh $* --clean --flash

# Pattern rules for native building
build-%:
	./build_local.sh $* --clean

flash-%:
	./build_local.sh $* --clean --flash

# Clean build artifacts - ONLY tmp directory, not scripts!
clean:
	@echo "Cleaning build artifacts in build/tmp/..."
	@rm -rf tmp/*
	@echo "Done! Build scripts are safe."

# Aliases for common builds
docker-c3: docker-build-esp32c3_official
docker-c6: docker-build-esp32c6_official
docker-s3: docker-build-esp32s3

c3: build-esp32c3_official
c6: build-esp32c6_official
s3: build-esp32s3

# All-in-one targets
docker-all: docker-build
	./build_docker.sh esp32 --clean
	./build_docker.sh esp32s2 --clean
	./build_docker.sh esp32s3 --clean
	./build_docker.sh esp32c3 --clean
	./build_docker.sh esp32c6 --clean

# Check Docker installation
check-docker:
	@which docker > /dev/null || (echo "Error: Docker not installed" && exit 1)
	@echo "✓ Docker is installed"

# Check ESP-IDF installation
check-idf:
	@which idf.py > /dev/null || (echo "Error: ESP-IDF not installed or not activated" && exit 1)
	@echo "✓ ESP-IDF is available"
