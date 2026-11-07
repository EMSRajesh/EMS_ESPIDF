#!/usr/bin/env python3
"""
MyEMS USB-to-RS485 Modbus RTU SLAVE simulator.

Use this when testing the ESP32-S3 firmware, which is the Modbus MASTER.
The PC script responds as a simulated Modbus SLAVE through a USB-RS485 adapter.

Supports FC03 (Read Holding Registers) and FC04 (Read Input Registers).
It can serve more than one slave ID on the same physical bus, e.g. IDs 1 and 2.

Example (Windows):
  py usb_rs485_modbus_slave_sim.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1

This is an end-to-end transport/protocol test, not a test of real meter scaling or register maps.
"""

from __future__ import annotations
import argparse
import sys
import time

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


def make_register_value(slave: int, function: int, address: int) -> int:
    """Deterministic simulated value, visibly different by slave/address."""
    # Keep values in uint16 range. Replace with your own expected test values if needed.
    return (1000 * slave + address + (0 if function == 3 else 100)) & 0xFFFF


def make_read_response(slave: int, function: int, start: int, count: int) -> bytes:
    if function not in (3, 4):
        return append_crc(bytes((slave, function | 0x80, 0x01)))  # Illegal Function
    if not 1 <= count <= 125 or start + count > 0x10000:
        return append_crc(bytes((slave, function | 0x80, 0x03)))  # Illegal Data Value
    payload = bytearray((slave, function, count * 2))
    for address in range(start, start + count):
        value = make_register_value(slave, function, address)
        payload.extend((value >> 8, value & 0xFF))
    return append_crc(bytes(payload))


def build_exception(slave: int, function: int, code: int) -> bytes:
    return append_crc(bytes((slave, function | 0x80, code)))


def handle_request(frame: bytes, slave_ids: set[int]) -> tuple[bytes | None, str]:
    if len(frame) != 8:
        return None, f"ignore: expected 8-byte FC03/FC04 request, got {len(frame)} bytes"
    if not check_crc(frame):
        return None, f"reject bad CRC: {frame.hex(' ').upper()}"
    slave, function = frame[0], frame[1]
    if slave not in slave_ids:
        return None, f"ignore slave {slave}; simulator configured for {sorted(slave_ids)}"
    if function not in (3, 4):
        response = build_exception(slave, function, 0x01)
        return response, f"unsupported FC{function:02X}; sending Illegal Function"
    start = (frame[2] << 8) | frame[3]
    count = (frame[4] << 8) | frame[5]
    if count == 0 or count > 125 or start + count > 0x10000:
        response = build_exception(slave, function, 0x03)
        return response, f"invalid start/count {start}/{count}; sending Illegal Data Value"
    response = make_read_response(slave, function, start, count)
    return response, f"FC{function:02X} slave={slave} start={start} count={count}"


def self_test() -> None:
    req = append_crc(bytes.fromhex("01 03 00 00 00 02"))
    assert len(req) == 8 and check_crc(req)
    response, message = handle_request(req, {1, 2})
    assert response is not None and check_crc(response)
    assert response[:3] == bytes.fromhex("01 03 04")
    assert response[3:7] == bytes.fromhex("03 E8 03 E9")
    req2 = append_crc(bytes.fromhex("02 04 00 0A 00 01"))
    response2, _ = handle_request(req2, {1, 2})
    assert response2 is not None and response2[:3] == bytes.fromhex("02 04 02")
    assert response2[3:5] == bytes.fromhex("08 3E")  # 2*1000 + 100 + address 10 = 2110
    print("PASS: FC03/FC04 request parsing, deterministic register data, and response CRC.")


def parse_args():
    p = argparse.ArgumentParser(description="USB-RS485 Modbus RTU slave simulator for MyEMS master testing")
    p.add_argument("--port", help="Serial port, e.g. COM12")
    p.add_argument("--slaves", default="1,2", help="Comma-separated slave IDs to simulate (default: 1,2)")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--parity", choices=("N", "E", "O"), default="E")
    p.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
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
        print("ERROR: pyserial is missing. Install with: py -m pip install pyserial", file=sys.stderr)
        return 2
    try:
        slave_ids = {int(x.strip(), 0) for x in args.slaves.split(",") if x.strip()}
        if not slave_ids or any(x < 1 or x > 247 for x in slave_ids):
            raise ValueError("slave IDs must be in range 1..247")
    except ValueError as exc:
        print(f"ERROR: invalid --slaves: {exc}", file=sys.stderr)
        return 2

    parity_map = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}
    stop_map = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}
    print("MyEMS Modbus RTU simulated SLAVE")
    print(f"Port={args.port}, {args.baud}/8{args.parity}{args.stopbits}, slave IDs={sorted(slave_ids)}")
    print("Waiting for ESP32 master requests. This script does not transmit until a valid request arrives.")
    print("Ctrl+C to stop. Do not run another master on this bus.")
    try:
        with serial.Serial(
            port=args.port, baudrate=args.baud, bytesize=serial.EIGHTBITS,
            parity=parity_map[args.parity], stopbits=stop_map[args.stopbits],
            timeout=0.25, write_timeout=1.0
        ) as ser:
            # Read one full FC03/FC04 request (8 bytes). Serial timeout permits
            # partial reads to be accumulated without requiring exact RTU gap timing.
            buffer = bytearray()
            last_byte_at = None
            while True:
                chunk = ser.read(1)
                now = time.monotonic()
                if chunk:
                    if last_byte_at is not None and now - last_byte_at > 0.05 and buffer:
                        print(f"RX partial/stale bytes discarded after gap: {buffer.hex(' ').upper()}")
                        buffer.clear()
                    buffer.extend(chunk)
                    last_byte_at = now
                    # Standard read request is 8 bytes for FC03/FC04.
                    if len(buffer) >= 8:
                        frame = bytes(buffer[:8])
                        del buffer[:8]
                        print(f"\nRX: {frame.hex(' ').upper()}")
                        response, message = handle_request(frame, slave_ids)
                        print(message)
                        if response:
                            # UART half-duplex transceiver direction is handled by
                            # the USB-RS485 adapter's auto-direction feature.
                            ser.write(response)
                            ser.flush()
                            print(f"TX: {response.hex(' ').upper()}")
                        last_byte_at = time.monotonic() if buffer else None
                else:
                    # If no byte arrived for a while, clear an incomplete request.
                    if buffer and last_byte_at is not None and time.monotonic() - last_byte_at > 0.25:
                        print(f"RX incomplete request discarded: {buffer.hex(' ').upper()}")
                        buffer.clear()
                        last_byte_at = None
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
