#!/usr/bin/env python3
"""Capture ESP32-S3 serial log (USB-Serial-JTAG console) with timestamps.

Survives USB re-enumeration: if the port disappears (cable wiggle, reset, reset into
bootloader) it reopens it and keeps appending to the same file.

Usage: python3 serial_capture.py <port> <seconds> <outfile>
"""
import sys
import time

import serial

port, seconds, outfile = sys.argv[1], int(sys.argv[2]), sys.argv[3]

t0 = time.time()
total = 0
ser = None

with open(outfile, "a") as f:
    f.write(f"\n--- capture start {time.strftime('%H:%M:%S')} port={port} ---\n")
    while time.time() - t0 < seconds:
        if ser is None:
            try:
                ser = serial.Serial(port, 115200, timeout=0.3)
                f.write(f"\n--- reconnected {time.strftime('%H:%M:%S')} ---\n")
                f.flush()
            except Exception:
                time.sleep(0.5)
                continue
        try:
            data = ser.read(4096)
            if data:
                f.write(data.decode("utf-8", errors="replace"))
                f.flush()
                total += len(data)
        except Exception:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            f.write(f"\n--- port lost {time.strftime('%H:%M:%S')}, retrying ---\n")
            f.flush()

if ser is not None:
    try:
        ser.close()
    except Exception:
        pass
print(f"captured {total} bytes in {seconds}s -> {outfile}")
