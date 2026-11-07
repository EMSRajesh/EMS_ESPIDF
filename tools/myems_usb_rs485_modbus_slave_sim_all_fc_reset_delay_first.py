#!/usr/bin/env python3
"""
MyEMS Modbus RTU slave simulator / fault-injection harness.

Purpose:
  Test an ESP32 Modbus RTU MASTER through a USB-to-RS485 adapter.
  Implements common public function codes:
    FC01 Read Coils
    FC02 Read Discrete Inputs
    FC03 Read Holding Registers
    FC04 Read Input Registers
    FC05 Write Single Coil
    FC06 Write Single Register
    FC15 (0x0F) Write Multiple Coils
    FC16 (0x10) Write Multiple Registers
    FC23 (0x17) Read/Write Multiple Registers

Fault injection:
  --fault none|drop|bad-crc|delay|delay-first|exception|wrong-slave|short-response
  --fault-every N applies fault on every Nth valid request (0 disables)
  --delay-ms N delay response for the delay fault
  --exception-code N selects exception code (default 2)
  --log-file PATH appends request/result records as text.
  --reset-on-fc01 restores deterministic state before FC01 address 0/count 8.

Example:
  py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1
  py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault bad-crc --fault-every 3

This is a test slave, not a real meter model. Do not put it on a live control bus.
"""
from __future__ import annotations
import argparse, sys, time
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    serial = None

MAX_READ_BITS = 2000
MAX_READ_REGS = 125
MAX_WRITE_BITS = 1968
MAX_WRITE_REGS = 123


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def with_crc(payload: bytes) -> bytes:
    c = crc16(payload)
    return payload + bytes((c & 0xFF, c >> 8))


def valid_crc(frame: bytes) -> bool:
    return len(frame) >= 4 and frame[-2:] == bytes((crc16(frame[:-2]) & 0xFF, crc16(frame[:-2]) >> 8))


def u16(b: bytes, i: int) -> int:
    return (b[i] << 8) | b[i+1]


def bit_pack(values: list[bool]) -> bytes:
    out = bytearray((len(values) + 7) // 8)
    for i, value in enumerate(values):
        if value:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def bit_get(store: list[bool], addr: int) -> bool:
    return store[addr % len(store)]


class Device:
    def __init__(self, slave_id: int):
        self.slave_id = slave_id
        # Deterministic initial register map; distinct values per slave.
        self.holding = [(slave_id * 1000 + i) & 0xFFFF for i in range(65536)]
        self.inputs = [(slave_id * 1000 + 100 + i) & 0xFFFF for i in range(65536)]
        self.coils = [bool((i + slave_id) & 1) for i in range(65536)]
        self.discrete = [bool((i + slave_id + 1) & 1) for i in range(65536)]

    def reset(self) -> None:
        """Restore deterministic initial simulator state."""
        sid = self.slave_id
        self.holding = [(sid * 1000 + i) & 0xFFFF for i in range(65536)]
        self.inputs = [(sid * 1000 + 100 + i) & 0xFFFF for i in range(65536)]
        self.coils = [bool((i + sid) & 1) for i in range(65536)]
        self.discrete = [bool((i + sid + 1) & 1) for i in range(65536)]

    def exception(self, fc: int, code: int) -> bytes:
        return with_crc(bytes((self.slave_id, fc | 0x80, code)))

    def response(self, req: bytes) -> bytes:
        fc = req[1]
        if fc in (1, 2, 3, 4):
            start, count = u16(req, 2), u16(req, 4)
            if fc in (1, 2):
                if count < 1 or count > MAX_READ_BITS:
                    return self.exception(fc, 3)
                store = self.coils if fc == 1 else self.discrete
                data = bit_pack([bit_get(store, a) for a in range(start, start + count)])
                return with_crc(bytes((self.slave_id, fc, len(data))) + data)
            if count < 1 or count > MAX_READ_REGS:
                return self.exception(fc, 3)
            store = self.holding if fc == 3 else self.inputs
            data = bytearray()
            for a in range(start, start + count):
                v = store[a]
                data.extend((v >> 8, v & 0xFF))
            return with_crc(bytes((self.slave_id, fc, len(data))) + data)

        if fc == 5:
            addr, value = u16(req, 2), u16(req, 4)
            if value not in (0x0000, 0xFF00):
                return self.exception(fc, 3)
            self.coils[addr] = (value == 0xFF00)
            return with_crc(req[:6])

        if fc == 6:
            addr, value = u16(req, 2), u16(req, 4)
            self.holding[addr] = value
            return with_crc(req[:6])

        if fc in (15, 16):
            start, count, byte_count = u16(req, 2), u16(req, 4), req[6]
            data = req[7:7 + byte_count]
            if fc == 15:
                if not 1 <= count <= MAX_WRITE_BITS or byte_count != (count + 7) // 8:
                    return self.exception(fc, 3)
                for i in range(count):
                    self.coils[start + i] = bool(data[i // 8] & (1 << (i % 8)))
            else:
                if not 1 <= count <= MAX_WRITE_REGS or byte_count != count * 2:
                    return self.exception(fc, 3)
                for i in range(count):
                    self.holding[start + i] = (data[2*i] << 8) | data[2*i+1]
            return with_crc(bytes((self.slave_id, fc)) + req[2:6])

        if fc == 23:
            read_start, read_count = u16(req, 2), u16(req, 4)
            write_start, write_count, byte_count = u16(req, 6), u16(req, 8), req[10]
            data = req[11:11 + byte_count]
            if not 1 <= read_count <= MAX_READ_REGS or not 1 <= write_count <= MAX_WRITE_REGS or byte_count != write_count * 2:
                return self.exception(fc, 3)
            for i in range(write_count):
                self.holding[write_start + i] = (data[2*i] << 8) | data[2*i+1]
            result = bytearray((self.slave_id, fc, read_count * 2))
            for i in range(read_count):
                v = self.holding[read_start + i]
                result.extend((v >> 8, v & 0xFF))
            return with_crc(bytes(result))

        return self.exception(fc, 1)


def request_length_from_prefix(buf: bytearray) -> int | None:
    if len(buf) < 2:
        return None
    fc = buf[1]
    if fc in (1, 2, 3, 4, 5, 6):
        return 8
    if fc in (15, 16):
        if len(buf) < 7:
            return None
        return 9 + buf[6]  # address+fc+start+count+bytecount+data+CRC
    if fc == 23:
        if len(buf) < 11:
            return None
        return 13 + buf[10]  # 11-byte header before data, plus data + CRC
    # Unknown FC: standard request length cannot be inferred safely.
    return None


def log_line(path: str | None, text: str) -> None:
    print(text, flush=True)
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{datetime.now(timezone.utc).isoformat()} {text}\n")


def self_test() -> None:
    d = Device(1)
    tests = [
        with_crc(bytes.fromhex("01 01 00 00 00 08")),
        with_crc(bytes.fromhex("01 02 00 00 00 08")),
        with_crc(bytes.fromhex("01 03 00 00 00 02")),
        with_crc(bytes.fromhex("01 04 00 00 00 02")),
        with_crc(bytes.fromhex("01 05 00 01 FF 00")),
        with_crc(bytes.fromhex("01 06 00 05 12 34")),
        with_crc(bytes.fromhex("01 0F 00 01 00 08 01 55")),
        with_crc(bytes.fromhex("01 10 00 05 00 02 04 12 34 AB CD")),
        with_crc(bytes.fromhex("01 17 00 00 00 02 00 05 00 02 04 00 01 00 02")),
    ]
    expected = [1, 2, 3, 4, 5, 6, 15, 16, 23]
    for req, fc in zip(tests, expected):
        assert valid_crc(req), (fc, req.hex())
        response = d.response(req)
        assert valid_crc(response), (fc, response.hex())
        assert response[1] == fc or response[1] == (fc | 0x80), (fc, response.hex())
    assert d.holding[5] == 0x0001
    assert d.holding[6] == 0x0002
    print("PASS: FC01/02/03/04/05/06/15/16/23 response generation and CRC.")

def parse_args():
    p = argparse.ArgumentParser(description="MyEMS Modbus RTU simulated slave with fault injection")
    p.add_argument("--port")
    p.add_argument("--slaves", default="1,2")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--parity", choices=("N", "E", "O"), default="E")
    p.add_argument("--stopbits", type=int, choices=(1, 2), default=1)
    p.add_argument("--fault", choices=("none", "drop", "bad-crc", "delay", "delay-first", "exception", "wrong-slave", "short-response"), default="none")
    p.add_argument("--fault-every", type=int, default=0)
    p.add_argument("--delay-ms", type=int, default=1500)
    p.add_argument("--exception-code", type=int, default=2)
    p.add_argument("--log-file")
    p.add_argument(
        "--reset-on-fc01",
        action="store_true",
        help="Reset simulator state before FC01 read of address 0, quantity 8",
    )
    p.add_argument("--self-test", action="store_true")
    return p.parse_args()

def main() -> int:
    a = parse_args()
    if a.self_test:
        self_test()
        return 0
    if not a.port:
        print("ERROR: --port required unless --self-test", file=sys.stderr); return 2
    if serial is None:
        print("ERROR: pyserial missing; run py -m pip install pyserial", file=sys.stderr); return 2
    try:
        ids = {int(x.strip(), 0) for x in a.slaves.split(",") if x.strip()}
        if not ids or any(i < 1 or i > 247 for i in ids): raise ValueError("IDs must be 1..247")
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr); return 2
    parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[a.parity]
    stopbits = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}[a.stopbits]
    devices = {i: Device(i) for i in ids}
    count = 0
    buf = bytearray()
    last_byte = None
    log_line(a.log_file, f"START port={a.port} baud={a.baud} parity={a.parity} stopbits={a.stopbits} slaves={sorted(ids)} fault={a.fault}")
    try:
        with serial.Serial(a.port, a.baud, bytesize=serial.EIGHTBITS, parity=parity,
                           stopbits=stopbits, timeout=0.02, write_timeout=1.0) as ser:
            reset_done = False
            while True:
                b = ser.read(1)
                now = time.monotonic()
                if b:
                    if last_byte is not None and now - last_byte > 0.05 and buf:
                        log_line(a.log_file, f"DISCARD partial={buf.hex(' ').upper()}")
                        buf.clear()
                    buf.extend(b); last_byte = now
                    needed = request_length_from_prefix(buf)
                    if needed and len(buf) >= needed:
                        req = bytes(buf[:needed]); del buf[:needed]
                        last_byte = time.monotonic() if buf else None
                        log_line(a.log_file, f"RX {req.hex(' ').upper()}")
                        if not valid_crc(req):
                            log_line(a.log_file, "REJECT bad request CRC")
                            continue
                        sid, fc = req[0], req[1]
                        if sid == 0:
                            # Broadcast write: apply writes without response; no broadcast reads.
                            if fc in (5, 6, 15, 16) and req[1:] and ids:
                                for dev in devices.values():
                                    try: dev.response(bytes((dev.slave_id,)) + req[1:])
                                    except Exception: pass
                            log_line(a.log_file, "BROADCAST: no response transmitted")
                            continue
                        if sid not in devices:
                            log_line(a.log_file, f"IGNORE unknown slave={sid}")
                            continue
                        count += 1
                        if (
                            a.reset_on_fc01
                            and not reset_done
                            and fc == 1
                            and u16(req, 2) == 0
                            and u16(req, 4) == 8
                        ):
                            devices[sid].reset()
                            reset_done = True
                            log_line(
                                a.log_file,
                                f"RESET slave={sid} initial simulator state",
                            )
                        response = devices[sid].response(req)
                        apply_fault = a.fault != "none" and (a.fault_every <= 1 or (count % a.fault_every == 0))
                        if apply_fault:
                            if a.fault == "drop":
                                log_line(a.log_file, "FAULT drop response"); continue
                            if a.fault == "delay":
                                log_line(a.log_file, f"FAULT delaying {a.delay_ms} ms")
                                time.sleep(max(0, a.delay_ms) / 1000.0)
                            elif a.fault == "delay-first":
                                if count == 1:
                                    log_line(
                                        a.log_file,
                                        f"FAULT delay-first: delaying first response {a.delay_ms} ms",
                                    )
                                    time.sleep(max(0, a.delay_ms) / 1000.0)
                                else:
                                    log_line(
                                        a.log_file,
                                        "FAULT delay-first: subsequent response sent immediately",
                                    )
                            elif a.fault == "bad-crc":
                                response = response[:-1] + bytes((response[-1] ^ 0xFF,))
                            elif a.fault == "exception":
                                response = with_crc(bytes((sid, fc | 0x80, a.exception_code & 0xFF)))
                            elif a.fault == "wrong-slave":
                                response = bytes(((sid % 247) + 1,)) + response[1:]
                                response = with_crc(response[:-2])
                            elif a.fault == "short-response":
                                response = response[:-2]
                            log_line(a.log_file, f"FAULT applied={a.fault}")
                        ser.write(response); ser.flush()
                        log_line(a.log_file, f"TX {response.hex(' ').upper()}")
                elif buf and last_byte is not None and time.monotonic() - last_byte > 0.2:
                    log_line(a.log_file, f"DISCARD incomplete={buf.hex(' ').upper()}")
                    buf.clear(); last_byte = None
    except KeyboardInterrupt:
        log_line(a.log_file, "STOP user requested")
    except Exception as e:
        log_line(a.log_file, f"ERROR {e}")
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
