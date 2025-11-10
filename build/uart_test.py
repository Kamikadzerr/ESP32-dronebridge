#!/usr/bin/env python3
"""
DroneBridge ESP32 - UART Connection Tester
===========================================

This tool helps test and diagnose UART connections between your computer and ESP32,
or between ESP32 and Flight Controller.

Usage:
    python3 uart_test.py [options]

Examples:
    python3 uart_test.py --port /dev/ttyUSB0 --baud 115200
    python3 uart_test.py --scan
    python3 uart_test.py --port /dev/ttyUSB0 --test mavlink
"""

import serial
import serial.tools.list_ports
import time
import sys
import argparse
import struct
from collections import defaultdict

# Color codes for terminal output


class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'


def print_header(text):
    print(f"\n{Colors.BOLD}{Colors.BLUE}{'='*60}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.BLUE}{text:^60}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.BLUE}{'='*60}{Colors.ENDC}\n")


def print_success(text):
    print(f"{Colors.GREEN}✓ {text}{Colors.ENDC}")


def print_error(text):
    print(f"{Colors.RED}✗ {text}{Colors.ENDC}")


def print_warning(text):
    print(f"{Colors.YELLOW}⚠ {text}{Colors.ENDC}")


def print_info(text):
    print(f"{Colors.CYAN}ℹ {text}{Colors.ENDC}")


def scan_ports():
    """Scan and list all available serial ports"""
    print_header("Scanning Serial Ports")

    ports = serial.tools.list_ports.comports()
    esp_ports = []
    other_ports = []

    for port in ports:
        # Filter ESP32-like devices
        port_name_lower = port.device.lower()
        desc_lower = port.description.lower()

        is_esp = False
        if any(x in port_name_lower for x in ['ttyusb', 'ttyacm', 'usbserial', 'slab_usb']):
            if not any(x in port_name_lower for x in ['bluetooth', 'buds', 'debug', 'srs-']):
                is_esp = True

        port_info = {
            'device': port.device,
            'description': port.description,
            'hwid': port.hwid
        }

        if is_esp:
            esp_ports.append(port_info)
        else:
            other_ports.append(port_info)

    if esp_ports:
        print_success("Found potential ESP32/UART devices:")
        for i, port in enumerate(esp_ports, 1):
            print(f"{Colors.GREEN}  [{i}] {port['device']}{Colors.ENDC}")
            print(f"      Description: {port['description']}")
            print(f"      Hardware ID: {port['hwid']}")
            print()
    else:
        print_warning("No ESP32/UART devices found")

    if other_ports:
        print_info("Other devices (filtered out):")
        for port in other_ports:
            print(f"      {port['device']} - {port['description']}")

    return esp_ports


def test_connection(port, baudrate):
    """Test if we can open the serial port"""
    print_header(f"Testing Connection: {port} @ {baudrate}")

    try:
        ser = serial.Serial(port, baudrate, timeout=1)
        print_success(f"Successfully opened {port}")
        print_info(f"Baudrate: {baudrate}")
        print_info(f"Timeout: 1 second")
        ser.close()
        return True
    except serial.SerialException as e:
        print_error(f"Failed to open {port}: {e}")
        return False


def detect_protocol(data):
    """Detect protocol type from data"""
    if not data:
        return None

    # MAVLink v1: starts with 0xFE
    # MAVLink v2: starts with 0xFD
    if data[0] == 0xFE:
        return "MAVLink v1"
    elif data[0] == 0xFD:
        return "MAVLink v2"

    # MSP: starts with '$M' (0x24 0x4D)
    if len(data) >= 2 and data[0] == 0x24 and data[1] == 0x4D:
        return "MSP"

    # LTM: starts with '$T' (0x24 0x54)
    if len(data) >= 2 and data[0] == 0x24 and data[1] == 0x54:
        return "LTM"

    # CRSF: starts with 0xC8
    if data[0] == 0xC8:
        return "CRSF"

    return "Unknown"


def listen_data(port, baudrate, duration=10, protocol=None):
    """Listen for data on the serial port"""
    print_header(f"Listening for Data: {port} @ {baudrate}")

    if protocol:
        print_info(f"Testing for {protocol.upper()} protocol")
    else:
        print_info("Auto-detecting protocol")

    print_info(f"Duration: {duration} seconds")
    print_info("Press Ctrl+C to stop early\n")

    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)

        start_time = time.time()
        bytes_received = 0
        protocols_detected = defaultdict(int)
        last_print = time.time()

        print("Listening", end='', flush=True)

        while time.time() - start_time < duration:
            if ser.in_waiting > 0:
                data = ser.read(ser.in_waiting)
                bytes_received += len(data)

                # Detect protocol
                proto = detect_protocol(data)
                if proto != "Unknown":
                    protocols_detected[proto] += 1

                # Print dots to show activity
                if time.time() - last_print > 0.5:
                    print(".", end='', flush=True)
                    last_print = time.time()
            else:
                time.sleep(0.1)

        print()  # New line after dots
        ser.close()

        # Results
        print_header("Results")

        if bytes_received > 0:
            print_success(
                f"Received {bytes_received} bytes in {duration} seconds")
            print_info(
                f"Data rate: {bytes_received/duration:.1f} bytes/second")

            if protocols_detected:
                print_success("Detected protocols:")
                for proto, count in sorted(protocols_detected.items(), key=lambda x: x[1], reverse=True):
                    print(
                        f"  {Colors.GREEN}• {proto}: {count} packets{Colors.ENDC}")

                # Check if we found what we were looking for
                if protocol:
                    found = any(protocol.lower() in p.lower()
                                for p in protocols_detected.keys())
                    if found:
                        print_success(
                            f"\n{protocol.upper()} protocol detected! Connection working!")
                    else:
                        print_warning(
                            f"\n{protocol.upper()} not detected. Check flight controller configuration.")
            else:
                print_warning("Data received but no known protocol detected")
                print_info(
                    "This might be binary data or an unsupported protocol")
        else:
            print_warning("No data received")
            print_info("Possible issues:")
            print("  • Flight controller not powered")
            print("  • Wrong UART port on flight controller")
            print("  • Wrong baudrate (try common rates: 57600, 115200, 921600)")
            print("  • TX/RX wires swapped")
            print("  • Flight controller UART not configured")

        return bytes_received > 0

    except serial.SerialException as e:
        print_error(f"Serial error: {e}")
        return False
    except KeyboardInterrupt:
        print(f"\n{Colors.YELLOW}Stopped by user{Colors.ENDC}")
        if ser and ser.is_open:
            ser.close()
        return False


def send_test_data(port, baudrate):
    """Send test data to verify TX is working"""
    print_header(f"Testing TX: {port} @ {baudrate}")
    print_info("Sending test pattern...")

    try:
        ser = serial.Serial(port, baudrate, timeout=1)

        # Send test pattern
        test_data = b"DroneBridge UART Test\n"
        for i in range(5):
            ser.write(test_data)
            print(f"  Sent: {test_data.decode().strip()}")
            time.sleep(0.5)

        ser.close()
        print_success("Test data sent successfully")
        print_info("If connected to ESP32, check the serial monitor")
        return True

    except serial.SerialException as e:
        print_error(f"Failed to send: {e}")
        return False


def loopback_test(port, baudrate):
    """Test loopback (TX connected to RX)"""
    print_header(f"Loopback Test: {port} @ {baudrate}")
    print_info("This test checks if TX and RX are properly connected")
    print_warning("For this test, connect TX to RX with a jumper wire\n")

    try:
        ser = serial.Serial(port, baudrate, timeout=2)

        test_messages = [
            b"TEST123",
            b"HELLO",
            b"DroneBridge",
        ]

        success_count = 0
        for msg in test_messages:
            ser.reset_input_buffer()
            ser.write(msg)
            time.sleep(0.1)

            received = ser.read(len(msg))

            if received == msg:
                print_success(f"'{msg.decode()}' - OK")
                success_count += 1
            else:
                print_error(f"'{msg.decode()}' - Failed (got: {received})")

        ser.close()

        if success_count == len(test_messages):
            print_success("\nLoopback test PASSED! TX and RX are working")
        else:
            print_error(
                f"\nLoopback test FAILED ({success_count}/{len(test_messages)} passed)")

        return success_count == len(test_messages)

    except serial.SerialException as e:
        print_error(f"Serial error: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='DroneBridge ESP32 UART Connection Tester',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  Scan for devices:
    python3 uart_test.py --scan
  
  Test connection:
    python3 uart_test.py --port /dev/ttyUSB0 --baud 115200
  
  Listen for MAVLink:
    python3 uart_test.py --port /dev/ttyUSB0 --test mavlink
  
  Test loopback:
    python3 uart_test.py --port /dev/ttyUSB0 --loopback
        """
    )

    parser.add_argument('--scan', action='store_true',
                        help='Scan for available serial ports')
    parser.add_argument(
        '--port', '-p', help='Serial port (e.g., /dev/ttyUSB0, COM3)')
    parser.add_argument('--baud', '-b', type=int,
                        default=115200, help='Baudrate (default: 115200)')
    parser.add_argument('--test', choices=['mavlink', 'msp', 'ltm', 'auto'], default='auto',
                        help='Protocol to test (default: auto-detect)')
    parser.add_argument('--duration', '-d', type=int, default=10,
                        help='Listen duration in seconds (default: 10)')
    parser.add_argument('--send', action='store_true', help='Send test data')
    parser.add_argument('--loopback', action='store_true',
                        help='Run loopback test (TX→RX)')

    args = parser.parse_args()

    print_header("DroneBridge ESP32 - UART Tester")

    # Scan mode
    if args.scan:
        scan_ports()
        return

    # Need a port for other operations
    if not args.port:
        print_error(
            "Error: --port is required (use --scan to find available ports)")
        parser.print_help()
        return

    # Test connection first
    if not test_connection(args.port, args.baud):
        return

    # Loopback test
    if args.loopback:
        loopback_test(args.port, args.baud)
        return

    # Send test
    if args.send:
        send_test_data(args.port, args.baud)
        return

    # Listen for data
    protocol = None if args.test == 'auto' else args.test
    listen_data(args.port, args.baud, args.duration, protocol)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{Colors.YELLOW}Interrupted by user{Colors.ENDC}")
        sys.exit(0)
