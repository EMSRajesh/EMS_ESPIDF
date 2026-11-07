#!/usr/bin/env python3
"""MyEMS DP-WP4 AC meter simulator (milestone D4). SIMULATION ONLY.

A Modbus RTU slave that serves the synthetic register maps of config/maps/
through a USB-RS485 adapter, so the ESP32 can be tested without a real meter.
The register layout is NOT written here: it is taken from the same JSON files
as the firmware tables (myems_profile_tool.py export-sim), so firmware and
simulator cannot drift apart (Plan risk table, GEN-008).

Read-only meter: FC03/FC04 only. Writes and coil reads get exception 01.

Modes (Test Spec Section 18). Syntax NAME[:ARG][@GROUP]; with @GROUP a fault
mode only affects requests for that poll group of the device's map.
  NORMAL                    Test Spec 4.1 values, static
  LOAD_CHANGE[:STEP_S]      deterministic load steps from --seed (default step 10 s)
  IMPORT                    +12.5 kW; import counter increases
  EXPORT                    -12.5 kW, PF -0.972; export counter increases
  TIMEOUT                   no response
  EXCEPTION[:CODE]          exception response (default code 02)
  BAD_LENGTH                byte count field does not match the payload (CRC valid)
  BAD_CRC                   CRC wrong
  OUT_OF_RANGE              frequency 30.00 Hz
  COUNTER_RESET             import counter returns to 0 once, then counts up (IMPORT)
  SLOW[:MS]                 response after MS ms (default 400)
  OFFLINE_THEN_RECOVER[:S]  no response for S s (default 60), then NORMAL

Examples (project root):
  python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --port COM12
  python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench_2dev.site.json --port COM12 \\
         --mode-for 1=TIMEOUT --log-file tools/test_results/sim.log
  python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --dump
  python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --self-test

Runtime control (for runners): with --control-file PATH the simulator rereads
the file whenever it changes. One command per line:
  mode <slave|*> <MODE>             e.g.  mode 1 EXCEPTION:02@energy
  set <slave|*> <param> <value>     meter-side engineering value, e.g. set 1 ac.current.l1 2.5

Never connect this simulator to a bus with real meters or equipment.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import re
import struct
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

SIM_VERSION = "1.0.0"
PROJECT_ROOT = Path(__file__).resolve().parents[2]
MAX_READ_REGS = 125
FRAME_GAP_S = 0.05      # same partial-frame discard rule as the COM simulators

# Test Spec 4.1: meter-side values of the NORMAL image (CT = PT = 1).
NORMAL_VALUES = {
    "ac.voltage.l1_n": 230.0, "ac.voltage.l2_n": 231.0, "ac.voltage.l3_n": 229.5,
    "ac.current.l1": 10.0, "ac.current.l2": 10.1, "ac.current.l3": 9.9,
    "ac.power.active.total": 12.5, "ac.power.reactive.total": 3.0,
    "ac.power.apparent.total": 12.855, "ac.power.factor.total": 0.972,
    "ac.frequency": 50.0,
    "ac.energy.import.active": 100.25, "ac.energy.export.active": 0.0,
    "dev.status.code": 0.0,
}
P_PARAM, Q_PARAM, S_PARAM, PF_PARAM = ("ac.power.active.total", "ac.power.reactive.total",
                                       "ac.power.apparent.total", "ac.power.factor.total")
CURRENTS = ("ac.current.l1", "ac.current.l2", "ac.current.l3")
IMPORT_PARAM, EXPORT_PARAM, FREQ_PARAM = "ac.energy.import.active", "ac.energy.export.active", "ac.frequency"
OUT_OF_RANGE_HZ = 30.0

MODES_WITH_ARG = {"LOAD_CHANGE": 10.0, "EXCEPTION": 2, "SLOW": 400, "OFFLINE_THEN_RECOVER": 60.0}
MODES = {"NORMAL", "IMPORT", "EXPORT", "TIMEOUT", "BAD_LENGTH", "BAD_CRC", "OUT_OF_RANGE",
         "COUNTER_RESET"} | set(MODES_WITH_ARG)
FAULT_MODES = {"TIMEOUT", "EXCEPTION", "BAD_LENGTH", "BAD_CRC", "SLOW", "OFFLINE_THEN_RECOVER"}
INTEGRATING = {"IMPORT", "EXPORT", "LOAD_CHANGE", "COUNTER_RESET"}


# ================================================================ Modbus RTU ===
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
    return len(frame) >= 4 and with_crc(frame[:-2]) == frame


def request_length(buf: bytes) -> int | None:
    """Length of the request at the start of buf, or None if not yet known."""
    if len(buf) < 2:
        return None
    fc = buf[1]
    if fc in (1, 2, 3, 4, 5, 6):
        return 8
    if fc in (15, 16):
        return 9 + buf[6] if len(buf) >= 7 else None
    if fc == 23:
        return 13 + buf[10] if len(buf) >= 11 else None
    return None


# ================================================================== encoding ===
def _float_bits(value: float) -> int:
    return struct.unpack(">I", struct.pack(">f", value))[0]


def encode_words(reg: dict, value: float) -> list[int]:
    """Meter-side engineering value -> register words, inverse of the firmware decode."""
    typ, order = reg["type"], reg["order"]
    if typ == "FLOAT32":
        bits = _float_bits(value)
    else:
        raw = round((value - reg["offset"]) / reg["scale"])
        lim = {"UINT16": (0, 0xFFFF), "INT16": (-0x8000, 0x7FFF), "BITFIELD16": (0, 0xFFFF),
               "UINT32": (0, 0xFFFFFFFF), "INT32": (-0x80000000, 0x7FFFFFFF)}[typ]
        if not lim[0] <= raw <= lim[1]:
            raise ValueError(f"{reg['param']}: value {value} -> raw {raw} does not fit {typ}")
        bits = raw & (0xFFFF if reg["regs"] == 1 else 0xFFFFFFFF)
        if typ == "BITFIELD16" and reg.get("bit_mask"):
            mask = reg["bit_mask"]
            shift = (mask & -mask).bit_length() - 1
            bits = (bits << shift) & mask
    if reg["regs"] == 1:
        a, b = bits >> 8, bits & 0xFF
        return [(a << 8) | b] if order == "AB" else [(b << 8) | a]
    A, B, C, D = (bits >> 24) & 0xFF, (bits >> 16) & 0xFF, (bits >> 8) & 0xFF, bits & 0xFF
    return {"ABCD": [(A << 8) | B, (C << 8) | D], "CDAB": [(C << 8) | D, (A << 8) | B],
            "BADC": [(B << 8) | A, (D << 8) | C], "DCBA": [(D << 8) | C, (B << 8) | A]}[order]


def parse_test_vector(text: str) -> float | None:
    """'D1 230.5' -> 230.5, 'D8 NaN -> INVALID' -> nan, 'D9 +Inf ...' -> inf."""
    parts = text.split()
    if len(parts) < 2:
        return None
    tok = parts[1]
    if tok.lower() == "nan":
        return math.nan
    if tok.lower() in ("+inf", "inf"):
        return math.inf
    if tok.lower() == "-inf":
        return -math.inf
    try:
        return float(tok)
    except ValueError:
        return None


# ===================================================================== modes ===
@dataclass(frozen=True)
class Mode:
    name: str
    arg: float | int | None = None
    group: str | None = None

    def __str__(self) -> str:
        s = self.name
        if self.arg is not None:
            s += f":{self.arg:02d}" if self.name == "EXCEPTION" else f":{self.arg:g}"
        return s + (f"@{self.group}" if self.group else "")


def parse_mode(text: str) -> Mode:
    m = re.fullmatch(r"([A-Z_]+)(?::([0-9.xXa-fA-F]+))?(?:@([a-z][a-z0-9_]*))?", text.strip())
    if not m or m.group(1) not in MODES:
        raise ValueError(f"unknown mode '{text}'; valid: {', '.join(sorted(MODES))}")
    name, arg_s, group = m.group(1), m.group(2), m.group(3)
    arg: float | int | None = None
    if name in MODES_WITH_ARG:
        if arg_s is None:
            arg = MODES_WITH_ARG[name]
        elif name == "EXCEPTION":
            arg = int(arg_s, 0) if arg_s.lower().startswith("0x") else int(arg_s, 10)
            if not 1 <= arg <= 255:
                raise ValueError("exception code must be 1..255")
        else:
            arg = float(arg_s) if name != "SLOW" else int(float(arg_s))
            if arg <= 0:
                raise ValueError(f"{name} argument must be > 0")
    elif arg_s is not None:
        raise ValueError(f"mode {name} takes no argument")
    if group and name not in FAULT_MODES and name != "OUT_OF_RANGE":
        raise ValueError(f"@group is only allowed for fault modes, not {name}")
    return Mode(name, arg, group)


# ==================================================================== device ===
@dataclass
class Reply:
    """What the simulator sends for one request: data after delay_s, or nothing."""
    data: bytes | None
    delay_s: float = 0.0
    note: str = ""


@dataclass
class SimDevice:
    layout: dict
    seed: int
    log: Callable[[str], None]
    values: dict[str, float] = field(default_factory=dict)
    mode: Mode = field(default_factory=lambda: Mode("NORMAL"))
    mode_since: float = 0.0
    last_update: float | None = None
    energy_acc: dict[str, float] = field(default_factory=dict)   # kWh not yet in the counter
    overrides: dict[str, float] = field(default_factory=dict)
    recovered_logged: bool = False

    def __post_init__(self) -> None:
        self.slave_id: int = self.layout["slave_id"]
        self.regs = self.layout["registers"]
        self.by_param = {r["param"]: r for r in self.regs}
        self.groups = self.layout["poll_groups"]
        # (fc, address) -> group name, for every register inside a poll group block
        self.space: dict[tuple[int, int], str] = {}
        for g in self.groups:
            for a in range(g["start"], g["start"] + g["count"]):
                self.space[(g["function_code"], a)] = g["name"]
        self.reset_values()

    # ---- values
    def reset_values(self) -> None:
        """Base image: the map's test_vector if it has one, else Test Spec 4.1, else 0."""
        self.base: dict[str, float] = {}
        for r in self.regs:
            v = NORMAL_VALUES.get(r["param"])
            tv = parse_test_vector(r.get("test_vector", ""))
            self.base[r["param"]] = tv if tv is not None else (v if v is not None else 0.0)
        self.values = dict(self.base)
        self.energy_acc = {IMPORT_PARAM: 0.0, EXPORT_PARAM: 0.0}

    def set_value(self, param: str, value: float) -> None:
        if param not in self.by_param:
            raise ValueError(f"slave {self.slave_id}: map {self.layout['map_id']} has no '{param}'")
        encode_words(self.by_param[param], value)           # range check now, not at read time
        self.overrides[param] = value
        self.values[param] = value

    def _set_instant(self, vals: dict[str, float]) -> None:
        for k, v in vals.items():
            if k in self.values and k not in self.overrides:
                self.values[k] = v

    def _load_change_values(self, step: int) -> dict[str, float]:
        rng = random.Random(self.seed * 1_000_003 + step)
        f = round(rng.uniform(0.2, 1.2), 2)
        p, q = round(12.5 * f, 3), round(3.0 * f, 3)
        s = round(math.hypot(p, q), 3)
        vals = {P_PARAM: p, Q_PARAM: q, S_PARAM: s, PF_PARAM: round(p / s, 3)}
        for c in CURRENTS:
            vals[c] = round(NORMAL_VALUES[c] * f, 2)
        return vals

    def set_mode(self, mode: Mode, now: float) -> None:
        self.update(now)
        self.mode, self.mode_since, self.recovered_logged = mode, now, False
        self.values.update({k: v for k, v in self.base.items()
                            if k not in (IMPORT_PARAM, EXPORT_PARAM) and k not in self.overrides})
        if mode.name == "EXPORT":
            self._set_instant({P_PARAM: -12.5, PF_PARAM: -0.972})
        elif mode.name == "OUT_OF_RANGE" and FREQ_PARAM in self.values:
            self._set_instant({FREQ_PARAM: OUT_OF_RANGE_HZ})
        elif mode.name == "COUNTER_RESET" and IMPORT_PARAM in self.values:
            self.values[IMPORT_PARAM] = 0.0
            self.energy_acc[IMPORT_PARAM] = 0.0
            self.log(f"EVENT slave={self.slave_id} COUNTER_RESET import counter -> 0")
        elif mode.name == "LOAD_CHANGE":
            self._set_instant(self._load_change_values(0))
        self.log(f"MODE slave={self.slave_id} mode={mode}")

    def effective_mode(self, now: float) -> Mode:
        m = self.mode
        if m.name == "OFFLINE_THEN_RECOVER" and now - self.mode_since >= float(m.arg):
            if not self.recovered_logged:
                self.recovered_logged = True
                self.log(f"MODE slave={self.slave_id} OFFLINE_THEN_RECOVER elapsed -> NORMAL")
            return Mode("NORMAL")
        return m

    def update(self, now: float) -> None:
        """Advance the simulated meter to time 'now' (load steps, energy counters)."""
        last, self.last_update = self.last_update, now
        if last is None or now <= last:
            return
        m = self.mode
        if m.name == "LOAD_CHANGE":
            step = int((now - self.mode_since) // float(m.arg))
            self._set_instant(self._load_change_values(step))
        if m.name not in INTEGRATING or P_PARAM not in self.values:
            return
        p = self.values[P_PARAM]
        target = IMPORT_PARAM if p >= 0 else EXPORT_PARAM
        if target not in self.values:
            return
        self.energy_acc[target] += abs(p) * (now - last) / 3600.0
        scale = self.by_param[target]["scale"]
        counts = math.floor(self.energy_acc[target] / scale + 1e-9)
        if counts > 0:
            self.energy_acc[target] -= counts * scale
            raw = round(self.values[target] / scale) + counts
            self.values[target] = raw * scale

    # ---- register image
    def image(self, fc: int) -> dict[int, int]:
        words: dict[int, int] = {}
        for r in self.regs:
            g = next(x for x in self.groups if x["name"] == r["group"])
            if g["function_code"] != fc:
                continue
            for i, w in enumerate(encode_words(r, self.values[r["param"]])):
                words[r["address"] + i] = words.get(r["address"] + i, 0) | w
        return words

    # ---- request handling
    def handle(self, req: bytes, now: float) -> Reply:
        sid, fc = req[0], req[1]
        self.update(now)
        mode = self.effective_mode(now)
        if fc not in (3, 4):
            return Reply(with_crc(bytes((sid, fc | 0x80, 0x01))), note="EXCEPTION 01 (read-only meter)")
        start, count = (req[2] << 8) | req[3], (req[4] << 8) | req[5]
        if not 1 <= count <= MAX_READ_REGS:
            return Reply(with_crc(bytes((sid, fc | 0x80, 0x03))), note="EXCEPTION 03 (quantity)")
        groups = {self.space.get((fc, a)) for a in range(start, start + count)}
        if None in groups:
            return Reply(with_crc(bytes((sid, fc | 0x80, 0x02))), note="EXCEPTION 02 (address)")
        if mode.group and mode.group not in groups:
            mode = Mode("NORMAL")              # fault targeted at another group
        name = mode.name
        if name in ("TIMEOUT", "OFFLINE_THEN_RECOVER"):
            return Reply(None, note=f"NO_RESPONSE {mode}")
        if name == "EXCEPTION":
            return Reply(with_crc(bytes((sid, fc | 0x80, int(mode.arg)))), note=f"EXCEPTION {int(mode.arg):02d}")
        img = self.image(fc)
        data = bytearray()
        for a in range(start, start + count):
            w = img.get(a, 0)
            data += bytes((w >> 8, w & 0xFF))
        if name == "BAD_LENGTH":
            return Reply(with_crc(bytes((sid, fc, len(data) + 2)) + data), note="BAD_LENGTH")
        resp = with_crc(bytes((sid, fc, len(data))) + data)
        if name == "BAD_CRC":
            return Reply(resp[:-1] + bytes((resp[-1] ^ 0xFF,)), note="BAD_CRC")
        if name == "SLOW":
            return Reply(resp, delay_s=int(mode.arg) / 1000.0, note=f"SLOW {int(mode.arg)} ms")
        return Reply(resp)


# ================================================================= simulator ===
class Simulator:
    """All simulated meters on one bus. Pure logic: time is passed in."""

    def __init__(self, layout: dict, seed: int = 1, log: Callable[[str], None] | None = None,
                 clock: Callable[[], float] = time.monotonic):
        self.layout = layout
        self.clock = clock
        self.t0 = clock()
        self._log = log or (lambda s: None)
        self.devices = {d["slave_id"]: SimDevice(d, seed, self.log) for d in layout["devices"]}
        self.pending: dict[int, float] = {}          # slave -> time its delayed reply is due
        self.counters = {"rx": 0, "tx": 0, "no_response": 0, "bad_request_crc": 0,
                         "ignored": 0, "overlap": 0}

    def log(self, text: str) -> None:
        self._log(f"{(self.clock() - self.t0) * 1000:10.1f} {text}")

    def set_mode(self, target: str, mode: Mode, now: float | None = None) -> None:
        now = self.clock() if now is None else now
        for d in self._targets(target):
            d.set_mode(mode, now)

    def set_value(self, target: str, param: str, value: float) -> None:
        for d in self._targets(target):
            d.set_value(param, value)
            self.log(f"SET slave={d.slave_id} {param}={value:g}")

    def _targets(self, target: str) -> list[SimDevice]:
        if target == "*":
            return list(self.devices.values())
        sid = int(target)
        if sid not in self.devices:
            raise ValueError(f"no simulated device with slave ID {sid}; have {sorted(self.devices)}")
        return [self.devices[sid]]

    def handle(self, req: bytes, now: float | None = None) -> Reply:
        now = self.clock() if now is None else now
        self.counters["rx"] += 1
        if not valid_crc(req):
            self.counters["bad_request_crc"] += 1
            self.log(f"RX {req.hex(' ').upper()} REJECT bad request CRC")
            return Reply(None, note="bad request CRC")
        sid = req[0]
        dev = self.devices.get(sid)
        if dev is None:
            self.counters["ignored"] += 1
            self.log(f"RX {req.hex(' ').upper()} IGNORE slave={sid}" + (" (broadcast)" if sid == 0 else ""))
            return Reply(None, note="not addressed to a simulated device")
        if sid in self.pending and self.pending[sid] > now:
            self.counters["overlap"] += 1
            self.log(f"OVERLAP slave={sid}: new request while a reply is still pending")
        mode = dev.effective_mode(now)
        reply = dev.handle(req, now)
        self.log(f"RX slave={sid} fc={req[1]:02X} {req.hex(' ').upper()} mode={mode}")
        if reply.data is None:
            self.counters["no_response"] += 1
            self.log(f"   {reply.note}")
        else:
            self.counters["tx"] += 1
            if reply.delay_s > 0:
                self.pending[sid] = now + reply.delay_s
            self.log(f"TX slave={sid} {reply.data.hex(' ').upper()}" + (f" [{reply.note}]" if reply.note else ""))
        return reply

    def apply_control(self, text: str) -> None:
        for n, line in enumerate(text.splitlines(), 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            try:
                if parts[0] == "mode" and len(parts) == 3:
                    self.set_mode(parts[1], parse_mode(parts[2]))
                elif parts[0] == "set" and len(parts) == 4:
                    self.set_value(parts[1], parts[2], float(parts[3]))
                else:
                    raise ValueError("expected 'mode <slave|*> <MODE>' or 'set <slave|*> <param> <value>'")
            except ValueError as exc:
                self.log(f"CONTROL line {n} ignored: {exc}")

    def dump(self, now: float | None = None) -> str:
        now = self.clock() if now is None else now
        out = []
        for d in self.devices.values():
            d.update(now)
            out.append(f"slave {d.slave_id} ({d.layout['instance_id']}, map {d.layout['map_id']} "
                       f"{d.layout['map_version']}, {d.layout['classification']}) mode {d.mode}")
            for g in d.groups:
                img = d.image(g["function_code"])
                words = " ".join(f"{img.get(a, 0):04X}" for a in range(g["start"], g["start"] + g["count"]))
                out.append(f"  {g['name']:<11} FC{g['function_code']:02d} {g['start']:>5}+{g['count']:<3} {words}")
            for r in d.regs:
                out.append(f"    {r['address']:>5} {r['param']:<26} {d.values[r['param']]!r:>12} {r['unit']}")
        return "\n".join(out)


# ================================================================ layout load ===
def load_layout(site: Path | None = None, layout: Path | None = None) -> dict:
    if layout is not None:
        return json.loads(Path(layout).read_text(encoding="utf-8"))
    sys.path.insert(0, str(PROJECT_ROOT / "tools" / "profile"))
    import myems_profile_tool as tool  # noqa: E402
    model, rep = tool.build_model(Path(site), "sim")
    if model is None:
        raise SystemExit("\n".join(str(e) for e in rep.errors) + "\nsite configuration invalid; simulator not started")
    return tool.export_sim(model)


# ================================================================ serial loop ===
def serve(sim: Simulator, ser, stop: Callable[[], bool] = lambda: False,
          control_file: Path | None = None) -> None:
    """Receive RTU frames from 'ser' (pyserial-like: read(n), write(b), flush()) and answer them."""
    buf = bytearray()
    last_rx: float | None = None
    queued: list[tuple[float, bytes]] = []
    ctl_mtime = None
    while not stop():
        now = sim.clock()
        if control_file is not None:
            try:
                mt = control_file.stat().st_mtime_ns
                if mt != ctl_mtime:
                    ctl_mtime = mt
                    sim.log(f"CONTROL reload {control_file}")
                    sim.apply_control(control_file.read_text(encoding="utf-8"))
            except FileNotFoundError:
                pass
        due = [q for q in queued if q[0] <= now]
        if due:
            queued = [q for q in queued if q[0] > now]
            for _, data in due:
                ser.write(data)
                ser.flush()
                sim.log(f"TX_DELAYED {data.hex(' ').upper()}")
        b = ser.read(1)
        now = sim.clock()
        if b:
            if last_rx is not None and buf and now - last_rx > FRAME_GAP_S:
                sim.log(f"DISCARD partial {buf.hex(' ').upper()}")
                buf.clear()
            buf += b
            last_rx = now
            need = request_length(buf)
            if need is None and len(buf) >= 2 and buf[1] not in (1, 2, 3, 4, 5, 6, 15, 16, 23):
                need = 8 if len(buf) >= 8 else None        # unknown FC: assume 8-byte request
            if need is not None and len(buf) >= need:
                req = bytes(buf[:need])
                del buf[:need]
                reply = sim.handle(req, now)
                if reply.data is not None:
                    if reply.delay_s > 0:
                        queued.append((now + reply.delay_s, reply.data))
                    else:
                        ser.write(reply.data)
                        ser.flush()
        elif buf and last_rx is not None and now - last_rx > 0.2:
            sim.log(f"DISCARD incomplete {buf.hex(' ').upper()}")
            buf.clear()
            last_rx = None


# ================================================================== self-test ===
def self_test(layout: dict) -> int:
    sim = Simulator(layout, clock=lambda: 0.0)
    ok = True
    for d in sim.devices.values():
        for g in d.groups:
            req = with_crc(bytes((d.slave_id, g["function_code"], g["start"] >> 8, g["start"] & 0xFF, 0, g["count"])))
            r = sim.handle(req, 0.0)
            good = r.data is not None and valid_crc(r.data) and r.data[2] == 2 * g["count"]
            ok &= good
            print(f"{'PASS' if good else 'FAIL'}: slave {d.slave_id} group {g['name']} ({g['count']} registers)")
        w = sim.handle(with_crc(bytes((d.slave_id, 6, 0, 0, 0, 1))), 0.0)
        good = w.data is not None and w.data[1] == 0x86 and w.data[2] == 1
        ok &= good
        print(f"{'PASS' if good else 'FAIL'}: slave {d.slave_id} write rejected with exception 01")
    print("SELF-TEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ======================================================================= main ===
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="MyEMS DP-WP4 AC meter simulator (SIMULATION ONLY)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--site", type=Path, help="site file; layout via myems_profile_tool")
    src.add_argument("--layout", type=Path, help="layout from 'myems_profile_tool.py export-sim'")
    ap.add_argument("--port", help="serial port of the USB-RS485 adapter, e.g. COM12")
    ap.add_argument("--baud", type=int, help="default: from the site file")
    ap.add_argument("--parity", choices=("N", "E", "O"), help="default: from the site file")
    ap.add_argument("--stopbits", type=int, choices=(1, 2), help="default: from the site file")
    ap.add_argument("--mode", default="NORMAL", help="mode for all devices (see module help)")
    ap.add_argument("--mode-for", action="append", default=[], metavar="SLAVE=MODE")
    ap.add_argument("--set", action="append", default=[], metavar="SLAVE:PARAM=VALUE",
                    help="meter-side value, e.g. 1:ac.current.l1=2.5")
    ap.add_argument("--seed", type=int, default=1, help="seed for LOAD_CHANGE")
    ap.add_argument("--control-file", type=Path)
    ap.add_argument("--log-file", type=Path)
    ap.add_argument("--dump", action="store_true", help="print the register image and exit")
    ap.add_argument("--self-test", action="store_true", help="check every poll group offline and exit")
    a = ap.parse_args(argv)

    layout = load_layout(a.site, a.layout)
    if a.log_file:
        a.log_file.parent.mkdir(parents=True, exist_ok=True)
    logf = open(a.log_file, "a", encoding="utf-8") if a.log_file else None

    def log(text: str) -> None:
        line = f"{datetime.now(timezone.utc).isoformat(timespec='milliseconds')} {text}"
        if not a.dump:
            print(line, flush=True)
        if logf:
            logf.write(line + "\n")
            logf.flush()

    try:
        sim = Simulator(layout, seed=a.seed, log=log)
        sim.log(f"START myems_ac_meter_sim {SIM_VERSION} site={layout['site_id']} config_crc={layout['config_crc32']} "
                f"slaves={sorted(sim.devices)} SIMULATION ONLY")
        sim.set_mode("*", parse_mode(a.mode))
        for item in a.mode_for:
            sid, _, m = item.partition("=")
            sim.set_mode(sid.strip(), parse_mode(m))
        for item in a.set:
            m = re.fullmatch(r"\s*(\*|\d+):([a-z0-9_.]+)=(\S+)\s*", item)
            if not m:
                raise ValueError(f"--set '{item}': expected SLAVE:PARAM=VALUE")
            sim.set_value(m.group(1), m.group(2), float(m.group(3)))
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if a.dump:
        print(sim.dump())
        return 0
    if a.self_test:
        return self_test(layout)
    if not a.port:
        print("ERROR: --port is required (or use --dump / --self-test)", file=sys.stderr)
        return 2
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial missing; run: python -m pip install -r tools/requirements-usb-rs485.txt", file=sys.stderr)
        return 2
    bus = layout["buses"][0]
    baud = a.baud or bus["baud"]
    parity = a.parity or bus["parity"][0]
    stop = a.stopbits or bus["stop_bits"]
    sim.log(f"PORT {a.port} {baud} {parity}{stop}")
    try:
        with serial.Serial(a.port, baud, bytesize=8,
                           parity={"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[parity],
                           stopbits={1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}[stop],
                           timeout=0.005, write_timeout=1.0) as ser:
            serve(sim, ser, control_file=a.control_file)
    except KeyboardInterrupt:
        sim.log(f"STOP user requested counters={sim.counters}")
    except Exception as exc:  # serial errors
        sim.log(f"ERROR {exc}")
        return 1
    finally:
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
