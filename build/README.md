# DroneBridge ESP32 - Local Build Scripts

Complete guide for building DroneBridge ESP32 firmware locally using Docker or native ESP-IDF.

## Table of Contents
- [Quick Start](#quick-start-fresh-system)
- [Build Artifacts Location](#️-important-build-artifacts-location)
- [Build Methods](#build-methods)
- [Available Models](#available-models-all-build-methods)
- [Cleaning & Maintenance](#cleaning-build-artifacts)
- [Troubleshooting](#troubleshooting)
- [Files in This Directory](#files-in-this-directory)

---

## Quick Start (Fresh System)

**Not sure which method to use?** Run the setup checker:

```bash
./check_setup.sh
```

This will detect what you have installed and recommend the best approach!

**Don't have ESP-IDF installed?** Use Docker! No local setup required:

```bash
# Build the Docker image (first time only)
./build_docker.sh --build-image

# Build firmware for your board
./build_docker.sh esp32c3_official
```

That's it! The Docker container has everything you need.

## ⚠️ Important: Build Artifacts Location

**All build artifacts are stored in `build/tmp/` directory!**

This keeps your scripts safe and makes cleaning easy:
- ✅ Build scripts are never deleted by clean operations
- ✅ Easy to clean: `rm -rf build/tmp/*` or `make clean`
- ✅ Clear separation between scripts and build files
- ✅ All binaries in one place: `build/tmp/db_esp32.bin`, etc.

## Build Methods

Choose one of two methods based on your preference:

### Method 1: Docker Build (Recommended for Fresh Systems)
✅ No local ESP-IDF installation required
✅ Consistent build environment
✅ Works on any platform with Docker

### Method 2: Native Build (For Development)
✅ Faster builds (no Docker overhead)
✅ Better for active development
⚠️ Requires ESP-IDF installation

---

## Method 1: Docker Build

### Prerequisites

- **Docker**: Install from [docker.com](https://www.docker.com/get-started)
  - macOS: Docker Desktop
  - Linux: Docker Engine
  - Windows: Docker Desktop with WSL2

No ESP-IDF installation needed!

### First Time Setup

Build the Docker image (only needed once):
```bash
./build_docker.sh --build-image
```

This creates a container with:
- ESP-IDF v5.4.2
- Node.js 18 (for web interface)
- All build tools and dependencies

### Docker Build Usage

```bash
./build_docker.sh <model> [options]
```

**Options:**
- `--clean` - Clean build (fullclean before build)
- `--flash` - Flash after successful build (auto-detects USB device)
- `--build-image` - Build/rebuild the Docker image
- `--shell` - Open interactive shell in container
- `-h, --help` - Show help message

**Examples:**
```bash
# Simple build
./build_docker.sh esp32c3_official

# Clean build
./build_docker.sh esp32s3 --clean
./build_docker.sh esp32s3_usb_host --clean
# Build and flash
./build_docker.sh esp32s3 --flash

# Interactive shell for custom commands
./build_docker.sh --shell
```

### Using Docker Compose

For more control, use `docker-compose`:

```bash
# Start container in background
docker-compose up -d

# Execute commands
docker-compose exec dronebridge-build bash -c "cp sdkconfig_c3_official sdkconfig && idf.py build"

# Stop container
docker-compose down
```

---

## Method 2: Native Build

### Prerequisites

⚠️ **Important:** This method requires ESP-IDF to be installed on your system. If you don't have it installed or get errors like `source: no such file or directory: /export.sh`, **use the Docker method instead** - it's much easier!

1. **ESP-IDF Installation**: You must have ESP-IDF v5.4.2 (or compatible) installed
   - Follow the [ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
   - This takes 30-60 minutes for first-time setup

2. **Activate ESP-IDF** before each build session:
   - **macOS/Linux**: `source $IDF_PATH/export.sh`
   - **Windows PowerShell**: `$env:IDF_PATH\export.ps1`
   - **Windows CMD**: `%IDF_PATH%\export.bat`

   If you get an error about `$IDF_PATH` not being set, ESP-IDF is not installed.

3. **Python dependencies**: All ESP-IDF Python dependencies should be installed (automatically handled by ESP-IDF setup)

**💡 Tip:** If you're not sure if ESP-IDF is installed, run:
```bash
echo $IDF_PATH  # Should show a path like /home/user/esp/esp-idf
idf.py --version  # Should show ESP-IDF version
```

If either command fails, ESP-IDF is not installed. **Use Docker method instead!**

### Native Build Scripts

#### `build_local.sh` (macOS/Linux/WSL)

Bash script for building firmware on Unix-like systems.

**Usage:**
```bash
./build_local.sh <model> [options]
```

**Options:**
- `--clean` - Clean build (fullclean before build)
- `--flash` - Flash after successful build
- `--monitor` - Start monitor after flash
- `-h, --help` - Show help message

**Examples:**
```bash
# Simple build
./build_local.sh esp32c3_official

# Clean build
./build_local.sh esp32s3 --clean

# Build, flash, and monitor
./build_local.sh esp32s3 --clean --flash --monitor

./build_local.sh esp32s3_usb_host --clean --flash --monitor

# Quick flash and monitor
./build_local.sh esp32s3 --flash --monitor
```

#### `build_local.ps1` (Windows PowerShell)

PowerShell script for building firmware on Windows.

**Usage:**
```powershell
.\build_local.ps1 <model> [options]
```

**Options:**
- `-Clean` - Clean build (fullclean before build)
- `-Flash` - Flash after successful build
- `-Monitor` - Start monitor after flash
- `-Help` - Show help message

**Examples:**
```powershell
# Simple build
.\build_local.ps1 esp32c3_official

# Clean build
.\build_local.ps1 esp32s3 -Clean

# Build, flash, and monitor
.\build_local.ps1 esp32c6_official -Clean -Flash -Monitor

# Quick flash and monitor
.\build_local.ps1 esp32s3_official -Flash -Monitor
```

---

## Available Models (All Build Methods)

### ESP32 (Classic)
- `esp32` - ESP32 (standard configuration)

### ESP32-S2
- `esp32s2` - ESP32-S2 (standard)
- `esp32s2_noUARTConsole` - ESP32-S2 (no UART console)

### ESP32-S3
- `esp32s3` - ESP32-S3 (standard)
- `esp32s3_noUARTConsole` - ESP32-S3 (no UART console)
- `esp32s3_usb_serial` - ESP32-S3 (USB Serial via JTAG)
- `esp32s3_usb_host` - ESP32-S3 (USB Host for FCU direct connection)

### ESP32-C3
- `esp32c3` - ESP32-C3 (standard)
- `esp32c3_official` - ESP32-C3 (official DroneBridge board)
- `esp32c3_usb_serial` - ESP32-C3 (USB Serial via JTAG)
- `esp32c3_noUARTConsole` - ESP32-C3 (no UART console)

### ESP32-C6
- `esp32c6` - ESP32-C6 (standard)
- `esp32c6_official` - ESP32-C6 (official DroneBridge board)
- `esp32c6_official_usb_serial` - ESP32-C6 official (USB Serial via JTAG)
- `esp32c6_official_noUARTConsole` - ESP32-C6 official (no UART console)
- `esp32c6_usb_serial` - ESP32-C6 (USB Serial via JTAG)
- `esp32c6_noUARTConsole` - ESP32-C6 (no UART console)

## Build Output

After a successful build, the following files will be available in the `build/tmp/` directory:

- `db_esp32.bin` - Main firmware binary
- `bootloader/bootloader.bin` - Bootloader binary
- `partition_table/partition-table.bin` - Partition table
- `www.bin` - Web interface binary
- `flash_args` - Flash arguments for manual flashing

**Note:** All build artifacts are in `build/tmp/` to keep scripts safe!

## Manual Flashing

If you built without the `--flash` option, you can flash manually:

```bash
# Using idf.py (with build directory)
idf.py -B build/tmp flash

# Or using esptool.py (check flash_args for addresses)
esptool.py --chip <chip_type> write_flash @build/tmp/flash_args
```

## Monitoring Serial Output

To monitor the serial output after flashing:

```bash
idf.py -B build/tmp monitor
```

Or use the `--monitor` option with the build script.

## Cleaning Build Artifacts

To clean build artifacts safely (scripts are never deleted):

```bash
# Using make (easiest)
make clean

# Using manual command
rm -rf build/tmp/*

# Using build scripts with --clean flag
./build_local.sh esp32c3_official --clean
./build_docker.sh esp32c3_official --clean
```

**The `--clean` flag and `make clean` only delete `build/tmp/*`, never your scripts!**

## Troubleshooting

### Docker Issues

#### Docker Not Installed
Install Docker from [docker.com](https://www.docker.com/get-started):
- **macOS**: Docker Desktop for Mac
- **Linux**: `curl -fsSL https://get.docker.com | sh`
- **Windows**: Docker Desktop for Windows (requires WSL2)

#### Docker Build Fails
```bash
# Rebuild the image from scratch
./build_docker.sh --build-image
```

#### Cannot Access USB Device in Docker
On Linux, you may need to add your user to the `dialout` group:
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in for changes to take effect
```

### Native Build Issues

#### ESP-IDF Not Found / $IDF_PATH Not Set

**Error:** `source: no such file or directory: /export.sh` or `$IDF_PATH not set`

**Cause:** ESP-IDF is not installed on your system.

**Solutions:**

1. **Recommended:** Use Docker instead (no ESP-IDF installation needed):
   ```bash
   ./build_docker.sh --build-image  # First time only
   ./build_docker.sh esp32c3_official
   ```

2. **Or install ESP-IDF** (takes 30-60 minutes):
   - Follow the [official ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
   - After installation, activate it:
     ```bash
     # macOS/Linux
     source $HOME/esp/esp-idf/export.sh

     # Windows PowerShell
     $HOME\esp\esp-idf\export.ps1
     ```
   - Add to your `.bashrc` or `.zshrc` for permanent setup:
     ```bash
     alias get_idf='source $HOME/esp/esp-idf/export.sh'
     ```

#### Verifying ESP-IDF Installation
```bash
echo $IDF_PATH           # Should show: /path/to/esp-idf
idf.py --version         # Should show: ESP-IDF v5.x.x
which idf.py            # Should show: /path/to/esp-idf/tools/idf.py
```

If any of these fail, ESP-IDF is not properly installed or activated.

#### Build Fails
Try a clean build:
```bash
./build_local.sh <model> --clean
# or with Docker
./build_docker.sh <model> --clean
```

### Flashing Issues

**Smart Device Detection:** Scripts now automatically filter ESP32 devices and ignore Bluetooth/audio devices!

Common issues:
- **No ESP32 device found**: Connect your ESP32 board via USB
- **Flash fails**: Hold the BOOT button on ESP32 during flashing
- **Permission denied** (Linux/macOS): Add user to `dialout` group:
  ```bash
  sudo usermod -a -G dialout $USER
  # Log out and log back in
  ```
- **Wrong cable**: Use a data cable, not charge-only cable
- **Retries**: Scripts automatically retry 3 times with 2-second delays

**Check connected devices:**
```bash
# Use the UART tester to scan devices
python3 uart_test.py --scan

# Or manually list devices
ls /dev/tty* | grep -E "USB|ACM|usbserial"
```

**Test UART connection:**
```bash
# Test if ESP32 is receiving data from flight controller
python3 uart_test.py --port /dev/ttyUSB0 --test mavlink --duration 10
```

---

## UART Connection Testing

Having trouble with your UART connection? Use the built-in test tool!

### Quick UART Test

```bash
# Scan for available devices
python3 uart_test.py --scan

# Test connection and listen for data
python3 uart_test.py --port /dev/ttyUSB0 --baud 115200

# Test for specific protocol (MAVLink, MSP, LTM)
python3 uart_test.py --port /dev/ttyUSB0 --test mavlink

# Test loopback (connect TX to RX with jumper)
python3 uart_test.py --port /dev/ttyUSB0 --loopback
```

**Features:**
- ✅ Auto-detects ESP32 devices (filters out Bluetooth/audio)
- ✅ Protocol detection (MAVLink v1/v2, MSP, LTM, CRSF)
- ✅ Data rate monitoring
- ✅ Loopback testing
- ✅ Color-coded output
- ✅ Helpful troubleshooting tips

See `uart_test.py --help` for more options.

---

## Files in This Directory

| File | Purpose |
|------|---------|
| `check_setup.sh` ⭐ | Setup checker - recommends best build method |
| `uart_test.py` 🔧 | UART connection tester - diagnose connection issues |
| `build_docker.sh` | Docker build script (no ESP-IDF needed) |
| `build_local.sh` | Native build script (macOS/Linux) |
| `build_local.ps1` | Native build script (Windows) |
| `Dockerfile` | Docker image with ESP-IDF v5.4.2 |
| `docker-compose.yml` | Docker Compose configuration |
| `Makefile` | Simplified build commands |
| `.dockerignore` | Docker build optimization |
| `README.md` | This complete documentation |

### Related Scripts
- `../create_release_zip.sh` - Creates release packages for all board variants
- `../create_release_zip.ps1` - PowerShell version of release script

## Comparison: Docker vs Native

| Feature | Docker Build | Native Build |
|---------|-------------|--------------|
| Setup Time | 5-10 min (first time) | 30-60 min |
| Build Speed | Slower (~10-20% overhead) | Faster |
| Disk Space | ~2-3 GB | ~5-10 GB |
| Consistency | Always same environment | Depends on local setup |
| Best For | Fresh systems, CI/CD | Active development |
| Prerequisites | Docker only | ESP-IDF installation |

---

## Additional Resources

- **DroneBridge ESP32 GitHub:** https://github.com/DroneBridge/ESP32
- **Documentation:** https://dronebridge.gitbook.io/docs/
- **ESP-IDF Docs:** https://docs.espressif.com/projects/esp-idf/
- **Docker:** https://www.docker.com/get-started

## Support

For questions or issues:
- Check this README first
- Run `./check_setup.sh` for personalized help
- Visit the [DroneBridge Discord](https://discord.gg/pqmHJNArE3)
- Open an issue on [GitHub](https://github.com/DroneBridge/ESP32/issues)

