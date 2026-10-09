#!/usr/bin/env python3
"""
===============================================================================
ESP32 ILI9341 USB-Serial Live Telemetry Streamer & Status Flag Tester
===============================================================================
Streams live laptop CPU / RAM telemetry and actuator commands over USB-UART
to the ESP32 to verify:
  1. The "STREAMING [LIVE]" green header pill (when packets are sent <= 3s)
  2. The "STREAMING [OFFLINE]" amber pill (when stream is paused/idle > 3s)
  3. Dynamic real-time gauge card updates (Temp, Gas, Soil, Servo, Pump, Alarm)

Usage:
  python stream_test.py
  python stream_test.py --port COM3
  python stream_test.py --port /dev/ttyUSB0 --interval 0.8
===============================================================================
"""

import sys
import time
import json
import math
import argparse

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[ERROR] 'pyserial' is not installed.")
    print("Please install it with: pip install pyserial")
    sys.exit(1)

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False


def find_default_com_port():
    """Auto-detect connected ESP32 / USB-UART serial ports."""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    # Priority keywords for USB-Serial chips commonly used with ESP32
    keywords = ["cp210", "ch340", "ch910", "ftdi", "usb serial", "uart", "silicon labs", "espressif"]
    for p in ports:
        desc = (p.description or "").lower()
        mfg = (p.manufacturer or "").lower()
        for kw in keywords:
            if kw in desc or kw in mfg:
                return p.device

    # Default to first available port
    return ports[0].device


def get_system_metrics(step_counter: int):
    """
    Collect live laptop CPU & RAM metrics.
    Falls back to realistic oscillating telemetry if psutil is unavailable.
    """
    if HAS_PSUTIL:
        cpu = psutil.cpu_percent(interval=None)
        ram = psutil.virtual_memory().percent
    else:
        # Realistic oscillating simulation (Sine wave)
        cpu = round(35.0 + 25.0 * math.sin(step_counter * 0.15), 1)
        ram = round(55.0 + 15.0 * math.cos(step_counter * 0.10), 1)

    # Derived sensors mapped from laptop load
    temp = round(26.0 + (cpu * 0.35), 1)         # e.g., 26°C - 58°C
    gas = int(60 + (cpu * 1.8))                  # e.g., 60 - 240 PPM
    soil = max(10, min(95, int(100 - ram)))      # e.g., 10% - 95%
    servo = int((cpu / 100.0) * 180)             # 0° - 180° mapped to CPU
    pump = 1 if ram > 85.0 else 0                # Emergency coolant pump if RAM > 85%
    alarm = 1 if cpu > 90.0 else 0               # Audio buzzer alarm if CPU > 90%

    msg = f"LAPTOP STREAM [CPU:{cpu:.1f}% RAM:{ram:.1f}%]"

    return {
        "temp": temp,
        "gas": gas,
        "soil": soil,
        "servo": servo,
        "pump": pump,
        "alarm": alarm,
        "msg": msg,
        "_raw_cpu": cpu,
        "_raw_ram": ram,
    }


def main():
    parser = argparse.ArgumentParser(description="Stream laptop metrics to ESP32 ILI9341 display.")
    parser.add_argument("--port", type=str, default=None, help="COM Port (e.g. COM3, COM4, /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--interval", type=float, default=1.0, help="Streaming interval in seconds (default: 1.0s)")
    args = parser.parse_args()

    # Determine Port
    port = args.port
    if not port:
        port = find_default_com_port()
        if not port:
            print("[ERROR] No USB serial ports detected!")
            print("Please plug in your ESP32 board or specify manually with --port COMx")
            sys.exit(1)

    print("=" * 70)
    print("  ESP32 ILI9341 USB-SERIAL TELEMETRY STREAM TESTER")
    print("=" * 70)
    print(f" Port        : {port}")
    print(f" Baud Rate   : {args.baud}")
    print(f" Interval    : {args.interval} sec (Timeout on display is 3.0 sec)")
    print(f" Data Mode   : {'Real Hardware (psutil)' if HAS_PSUTIL else 'Simulated Dynamic Load (install psutil for real metrics)'}")
    print("=" * 70)
    print("Controls:")
    print("  - Streaming runs automatically every second.")
    print("  - To test the 'STREAMING [OFFLINE]' indicator: Press Ctrl+C once to pause.")
    print("    Wait > 3 seconds, and watch the top-right pill turn amber [OFFLINE]!")
    print("=" * 70)

    try:
        ser = serial.Serial(port=port, baudrate=args.baud, timeout=0.1)
        time.sleep(1.5)  # Allow ESP32 UART auto-reset to settle
    except serial.SerialException as err:
        print(f"[FAIL] Could not open port {port}: {err}")
        print("Tip: Make sure the Arduino Serial Monitor or other tool is closed.")
        sys.exit(1)

    packet_num = 0
    try:
        while True:
            packet_num += 1
            metrics = get_system_metrics(packet_num)

            payload = {
                "temp": metrics["temp"],
                "gas": metrics["gas"],
                "soil": metrics["soil"],
                "servo": metrics["servo"],
                "pump": metrics["pump"],
                "alarm": metrics["alarm"],
                "msg": metrics["msg"],
            }

            json_line = json.dumps(payload) + "\n"
            ser.write(json_line.encode("utf-8"))
            ser.flush()

            # Read any acknowledgment back from ESP32
            ack = ser.readline().decode("utf-8", errors="ignore").strip()

            print(
                f"[{packet_num:04d}] Tx -> Temp: {metrics['temp']}C | "
                f"Gas: {metrics['gas']}ppm | Soil: {metrics['soil']}% | "
                f"Servo: {metrics['servo']}° | Pump: {metrics['pump']} | "
                f"Alarm: {metrics['alarm']} | Status: LIVE"
            )
            if ack:
                print(f"       Rx <- {ack}")

            time.sleep(args.interval)

    except KeyboardInterrupt:
        print("\n" + "=" * 70)
        print(" [PAUSED] Stream transmission halted by user.")
        print(" Watch the ESP32 display now: after 3 seconds, the header pill")
        print(" will automatically transition to 'STREAMING [OFFLINE]'!")
        print("=" * 70)
        user_choice = input("Press ENTER to resume streaming, or 'q' to quit: ")
        if user_choice.strip().lower() != 'q':
            main()
        else:
            ser.close()
            print("Session terminated.")


if __name__ == "__main__":
    main()
