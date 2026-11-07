#!/usr/bin/env python3
"""USB-to-RS485 Modbus RTU master test tool (FC03/FC04).

This script tests a real Modbus RTU SLAVE through a USB-to-RS485 adapter.
It is not a simulator for the ESP32 firmware. The MyEMS ESP32 firmware is
itself a Modbus master; do not run two masters as transmitters on the same bus.

Examples:
  py usb_rs485_modbus_test.py --self-test
  py usb_rs485_modbus_test.py --port COM5 --slave 1 --fc 3 --start 0 --count 10
  py usb_rs485_modbus_test.py --port COM5 --slave 2 --fc 4 --start 100 --count 4 --baud 9600 --parity E
  py usb_rs485_modbus_test.py --port COM5 --slave 1 --fc 3 --start 0 --count 10 --interval 2 --iterations 10 --csv readings.csv

Addresses passed to --start are zero-based Modbus PDU addresses. Confirm the
meter manual's convention; 40001 is often represented by PDU address 0.
"""
from __future__ import annotations

import argparse
import csv
import sys
import time
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    serial = None


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def append_crc(payload: bytes) -> bytes:
    crc = crc16_modbus(payload)
    return payload + bytes((crc & 0xFF, crc >> 8))


def check_crc(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    crc = crc16_modbus(frame[:-2])
    return frame[-2:] == bytes((crc & 0xFF, crc >> 8))


def build_read_request(slave: int, function: int, start: int, count: int) -> bytes:
    if not 1 <= slave <= 247:
        raise ValueError("Slave address must be 1..247; broadcast reads are unsupported.")
    if function not in (3, 4):
        raise ValueError("Only FC03 and FC04 are supported.")
    if not 1 <= count <= 125:
        raise ValueError("FC03/FC04 count must be 1..125 registers.")
    if not 0 <= start <= 0xFFFF or start + count > 0x10000:
        raise ValueError("Address/count exceeds the 16-bit address space.")
    payload = bytes((slave, function, start >> 8, start & 0xFF,
                     count >> 8, count & 0xFF))
    return append_crc(payload)


def read_exact(ser, count: int, deadline: float) -> bytes:
    result = bytearray()
    while len(result) < count and time.monotonic() < deadline:
        chunk = ser.read(count - len(result))
        if chunk:
            result.extend(chunk)
    return bytes(result)


def read_response(ser, slave: int, function: int, count: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    header = read_exact(ser, 2, deadline)
    if len(header) != 2:
        raise TimeoutError("Timed out waiting for response address/function.")
    if header[0] != slave:
        # Read no more than an exception-sized tail; caller will report wrong ID.
        return header + read_exact(ser, 3, deadline)
    if header[1] == (function | 0x80):
        return header + read_exact(ser, 3, deadline)
    if header[1] != function:
        return header + read_exact(ser, 3, deadline)
    byte_count = read_exact(ser, 1, deadline)
    if len(byte_count) != 1:
        raise TimeoutError("Timed out waiting for response byte count.")
    return header + byte_count + read_exact(ser, byte_count[0] + 2, deadline)


def decode_response(frame: bytes, slave: int, function: int, count: int) -> list[int]:
    if len(frame) < 5:
        raise ValueError(f"Response too short: {frame.hex(' ')}")
    if not check_crc(frame):
        raise ValueError(f"CRC error: {frame.hex(' ')}")
    if frame[0] != slave:
        raise ValueError(f"Wrong slave address {frame[0]}, expected {slave}.")
    if frame[1] == (function | 0x80):
        code = frame[2]
        names = {
            1: "Illegal Function", 2: "Illegal Data Address",
            3: "Illegal Data Value", 4: "Slave Device Failure",
            5: "Acknowledge", 6: "Slave Device Busy",
            8: "Memory Parity Error", 10: "Gateway Path Unavailable",
            11: "Gateway Target Failed to Respond",
        }
        raise RuntimeError(f"Modbus exception 0x{code:02X}: {names.get(code, 'Unknown exception')}")
    if frame[1] != function:
        raise ValueError(f"Wrong function code 0x{frame[1]:02X}, expected 0x{function:02X}.")
    expected_bytes = count * 2
    if frame[2] != expected_bytes:
        raise ValueError(f"Byte count {frame[2]} != expected {expected_bytes}.")
    if len(frame) != 5 + expected_bytes:
        raise ValueError(f"Frame length {len(frame)} != expected {5 + expected_bytes}.")
    return [(frame[3 + i * 2] << 8) | frame[4 + i * 2] for i in range(count)]


def self_test() -> None:
    request = build_read_request(1, 3, 0, 10)
    assert request[:6] == bytes.fromhex("01 03 00 00 00 0A")
    assert check_crc(request)
    assert request[-2:] == bytes.fromhex("C5 CD"), request.hex(" ")
    values = [0x0001, 0x1234, 0xABCD]
    payload = bytes((1, 3, len(values) * 2))
    for value in values:
        payload += bytes((value >> 8, value & 0xFF))
    response = append_crc(payload)
    assert decode_response(response, 1, 3, len(values)) == values
    corrupted = response[:-1] + bytes((response[-1] ^ 1,))
    assert not check_crc(corrupted)
    print("PASS: request CRC, response decoding, and corrupted-CRC detection.")


def parse_args():
    p = argparse.ArgumentParser(description="USB-RS485 Modbus RTU test client for FC03/FC04")
    p.add_argument("--port", help="Serial port, e.g. COM5")
    p.add_argument("--slave", type=int, default=1)
    p.add_argument("--fc", type=int, choices=(3, 4), default=3)
    p.add_argument("--start", type=lambda x: int(x, 0), default=0)
    p.add_argument("--count", type=int, default=10)
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--parity", choices=("N", "E", "O"), default="N")
    p.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
    p.add_argument("--timeout", type=float, default=1.0)
    p.add_argument("--interval", type=float, default=0.0)
    p.add_argument("--iterations", type=int, default=1, help="0 = continuous until Ctrl+C")
    p.add_argument("--csv", dest="csv_path")
    p.add_argument("--self-test", action="store_true")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.port:
        print("ERROR: --port is required unless --self-test is used.", file=sys.stderr)
        return 2
    if serial is None:
        print("ERROR: install dependency with: py -m pip install -r requirements-usb-rs485.txt",
              file=sys.stderr)
        return 2
    if args.timeout <= 0 or args.interval < 0 or args.iterations < 0:
        print("ERROR: timeout must be > 0; interval/iterations must be >= 0.", file=sys.stderr)
        return 2

    parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[args.parity]
    stopbits = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}[args.stopbits]
    csv_file = None
    writer = None
    try:
        with serial.Serial(
            args.port, args.baud, bytesize=serial.EIGHTBITS, parity=parity,
            stopbits=stopbits, timeout=0.05, write_timeout=args.timeout
        ) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            print(f"Port={args.port}, serial={args.baud}/8{args.parity}{args.stopbits}, "
                  f"slave={args.slave}, FC{args.fc}, start={args.start}, count={args.count}")
            print("Ensure no other Modbus master is transmitting on this bus. Ctrl+C stops.")
            if args.csv_path:
                csv_file = open(args.csv_path, "w", newline="", encoding="utf-8")
                writer = csv.writer(csv_file)
                writer.writerow(["timestamp_utc", "slave", "function", "start_address",
                                 "offset", "pdu_address", "raw_uint16", "hex"])
            iteration = 0
            while args.iterations == 0 or iteration < args.iterations:
                iteration += 1
                request = build_read_request(args.slave, args.fc, args.start, args.count)
                ser.reset_input_buffer()
                print(f"\n[{iteration}] TX {request.hex(' ').upper()}")
                ser.write(request)
                ser.flush()
                try:
                    frame = read_response(ser, args.slave, args.fc, args.count, args.timeout)
                    print(f"RX {frame.hex(' ').upper()}")
                    values = decode_response(frame, args.slave, args.fc, args.count)
                    timestamp = datetime.now(timezone.utc).isoformat()
                    for offset, value in enumerate(values):
                        address = args.start + offset
                        print(f"  PDU {address:5d}: {value:5d} (0x{value:04X})")
                        if writer:
                            writer.writerow([timestamp, args.slave, args.fc, args.start,
                                             offset, address, value, f"0x{value:04X}"])
                    if csv_file:
                        csv_file.flush()
                    print("RESULT: PASS")
                except (TimeoutError, ValueError, RuntimeError) as exc:
                    print(f"RESULT: FAIL — {exc}")
                if args.iterations == 0 or iteration < args.iterations:
                    time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nStopped by user.")
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        if csv_file:
            csv_file.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
