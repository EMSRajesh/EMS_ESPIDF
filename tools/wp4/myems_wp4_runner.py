#!/usr/bin/env python3
"""MyEMS DP-WP4 HIL runner (milestone D6 suite).

Runs the AC meter simulator on the RS-485 adapter, captures the ESP32 console,
steps the simulator through a sequence of modes (control file) and checks the
values and qualities the firmware publishes (VAL dump lines) in each phase.

Prerequisite: the board runs the wp4_sim firmware
    .\\tools\\build_firmware.ps1 -Mode Wp4Sim -Port COM11 -Flash
Close any serial monitor on the console port before starting.

Run (project root, about 5 minutes):
    python tools/wp4/myems_wp4_runner.py --console-port COM11 --rs485-port COM12
Re-analyse a saved run without hardware:
    python tools/wp4/myems_wp4_runner.py --analyse tools/test_results/wp4_d6_<timestamp>

Evidence is written to tools/test_results/wp4_d6_<timestamp>/:
console.log, sim.log, phases.json, report.txt.

Checks (Test Spec v1.1): UMD-001/004/006/007/008, Q-002/003/005/006/007/009,
ENG-008, SIM-009 end to end, plus no reboot, no routing errors, stack margin.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import myems_ac_meter_sim as sim_mod  # noqa: E402

PROJECT_ROOT = sim_mod.PROJECT_ROOT
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
VALUES_RE = re.compile(r"VALUES slot=(\d+) (\S+) count=(\d+)")
VAL_RE = re.compile(r"VAL slot=(\d+) ([a-z0-9_.]+)=(\S+) (\S+) q=([A-Z_]+) age_ms=(-?\d+) seq=(\d+)")
BOOT_RE = re.compile(r"BUILD_MODE=\S+ CONFIG_TARGET=(\w+)")
CRC_RE = re.compile(r"CONFIG site=(\S+) .*crc=0x([0-9A-Fa-f]{8})")
STATS_RE = re.compile(r"STATS sched: .*guard_expired=(\d+).*routing_errors=(\d+)")
STACK_RE = re.compile(r"STACK task=ems_acquisition allocated=(\d+)B current_free=(\d+)B min_ever_free=(\d+)B")
CRASH_RE = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|Rebooting\.\.\.|STACK CRITICAL")
RESET_RE = re.compile(r"\brst:0x[0-9a-fA-F]+")

ELECTRICAL = ("ac.voltage.l1_n", "ac.voltage.l2_n", "ac.voltage.l3_n", "ac.current.l1", "ac.current.l2",
              "ac.current.l3", "ac.power.active.total", "ac.power.reactive.total", "ac.power.apparent.total",
              "ac.power.factor.total", "ac.frequency")
ENERGY = ("ac.energy.import.active", "ac.energy.export.active")
IMPORT, EXPORT, POWER, PF, FREQ = ENERGY[0], ENERGY[1], "ac.power.active.total", "ac.power.factor.total", "ac.frequency"

# (name, simulator mode, duration s)
SUITE_D6 = [
    ("normal", "NORMAL", 40),
    ("import", "IMPORT", 40),
    ("export", "EXPORT", 40),
    ("timeout", "TIMEOUT", 30),
    ("recover", "NORMAL", 25),
    ("out_of_range", "OUT_OF_RANGE", 20),
    ("exception_energy", "EXCEPTION:02@energy", 30),
    ("counter_reset", "COUNTER_RESET", 25),
    ("final", "NORMAL", 15),
]


# ================================================================ parsing ===
@dataclass
class Val:
    value: float
    unit: str
    q: str
    age_ms: int
    seq: int


@dataclass
class Dump:
    t: float
    slot: int
    vals: dict[str, Val] = field(default_factory=dict)


@dataclass
class Phase:
    name: str
    mode: str
    t0: float
    t1: float


def parse_console(lines: list[tuple[float, str]]) -> list[Dump]:
    dumps: list[Dump] = []
    current: dict[int, Dump] = {}
    for t, raw in lines:
        text = ANSI_RE.sub("", raw)
        m = VALUES_RE.search(text)
        if m:
            d = Dump(t, int(m.group(1)))
            dumps.append(d)
            current[d.slot] = d
            continue
        m = VAL_RE.search(text)
        if m and int(m.group(1)) in current:
            try:
                v = float(m.group(3))
            except ValueError:
                continue
            current[int(m.group(1))].vals[m.group(2)] = Val(v, m.group(4), m.group(5), int(m.group(6)),
                                                           int(m.group(7)))
    return [d for d in dumps if d.vals]


def dumps_in(dumps: list[Dump], ph: Phase, settle: float = 0.0, slot: int = 0) -> list[Dump]:
    return [d for d in dumps if d.slot == slot and ph.t0 + settle <= d.t <= ph.t1]


def lines_in(lines: list[tuple[float, str]], ph: Phase) -> list[str]:
    return [ANSI_RE.sub("", s) for t, s in lines if ph.t0 <= t <= ph.t1]


# =============================================================== analysis ===
@dataclass
class Report:
    rows: list[str] = field(default_factory=list)
    failures: int = 0

    def check(self, ok: bool, test: str, text: str) -> bool:
        self.rows.append(f"{'PASS' if ok else 'FAIL'}  {test:<10} {text}")
        if not ok:
            self.failures += 1
        return ok

    def info(self, text: str) -> None:
        self.rows.append(f"            {text}")


def _close(a: float, b: float) -> bool:
    return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def _phase(phases: list[Phase], name: str) -> Phase | None:
    return next((p for p in phases if p.name == name), None)


def _last(rep: Report, dumps: list[Dump], ph: Phase | None, settle: float, test: str) -> Dump | None:
    if ph is None:
        rep.check(False, test, "phase missing")
        return None
    ds = dumps_in(dumps, ph, settle)
    if not ds:
        rep.check(False, test, f"no value dump in phase '{ph.name}' after {settle:.0f} s")
        return None
    return ds[-1]


def analyse_d6(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None) -> Report:
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    normal = sim_mod.NORMAL_VALUES

    # ---- boot and configuration
    boot = [m for m in (BOOT_RE.search(s) for s in text_all) if m]
    rep.check(bool(boot) and boot[-1].group(1) == "sim", "boot", "BUILD_MODE ... CONFIG_TARGET=sim seen"
              if boot else "no BUILD_MODE line (was the board reset?)")
    crc = [m for m in (CRC_RE.search(s) for s in text_all) if m]
    if expected_crc is not None:
        got = crc[-1].group(2).upper() if crc else None
        rep.check(got == expected_crc.upper().replace("0X", ""), "config",
                  f"firmware config CRC 0x{got} = simulator layout CRC {expected_crc}" if got else "no CONFIG line")
    rep.check(any("polling started" in s for s in text_all), "config", "polling started")
    rep.info(f"{len(dumps)} value dumps captured")

    # ---- normal: all 14 GOOD with Test Spec 4.1 values (UMD-001/004, Q-002, UMD-008)
    d = _last(rep, dumps, _phase(phases, "normal"), 20, "UMD-001")
    if d:
        missing = [p for p in normal if p not in d.vals]
        rep.check(not missing and len(d.vals) == len(normal), "UMD-001",
                  f"{len(d.vals)} parameters published" + (f"; missing {missing}" if missing else ""))
        bad = [f"{k}={v.value:g} {v.q}" for k, v in d.vals.items()
               if k in normal and (v.q != "GOOD" or not _close(v.value, normal[k]))]
        rep.check(not bad, "Q-002", "all parameters GOOD with the Test Spec 4.1 values" +
                  (f"; wrong: {', '.join(bad)}" if bad else ""))
        units = {"ac.voltage.l1_n": "V", "ac.current.l1": "A", POWER: "kW", FREQ: "Hz", IMPORT: "kWh"}
        wrong = [k for k, u in units.items() if k in d.vals and d.vals[k].unit != u]
        rep.check(not wrong, "UMD-004", "canonical units V, A, kW, Hz, kWh" + (f"; wrong: {wrong}" if wrong else ""))
        ages = [v.age_ms for k, v in d.vals.items() if k in ELECTRICAL]
        rep.check(bool(ages) and 0 <= max(ages) <= 11000, "UMD-008",
                  f"electrical sample age {min(ages) if ages else '-'}..{max(ages) if ages else '-'} ms (<= 11000)")

    # ---- import: +12.5 kW GOOD, import counter increases (UMD-006)
    ph = _phase(phases, "import")
    ds = dumps_in(dumps, ph, 15) if ph else []
    if rep.check(len(ds) >= 2, "UMD-006", f"{len(ds)} dumps in import phase"):
        p = ds[-1].vals.get(POWER)
        rep.check(p is not None and p.q == "GOOD" and _close(p.value, 12.5), "UMD-006",
                  f"active power {p.value:g} kW {p.q}" if p else "no active power")
        a, b = ds[0].vals.get(IMPORT), ds[-1].vals.get(IMPORT)
        rep.check(bool(a and b and b.q == "GOOD" and b.value > a.value), "UMD-006",
                  f"import counter {a.value:g} -> {b.value:g} kWh" if a and b else "no import counter")

    # ---- export: -12.5 kW, PF -0.972, export counter increases (UMD-007, CONV-008)
    ph = _phase(phases, "export")
    ds = dumps_in(dumps, ph, 15) if ph else []
    if rep.check(len(ds) >= 2, "UMD-007", f"{len(ds)} dumps in export phase"):
        p, f = ds[-1].vals.get(POWER), ds[-1].vals.get(PF)
        rep.check(bool(p and f and p.q == "GOOD" and _close(p.value, -12.5) and _close(f.value, -0.972)),
                  "UMD-007", f"active power {p.value:g} kW, PF {f.value:g}" if p and f else "missing values")
        a, b = ds[0].vals.get(EXPORT), ds[-1].vals.get(EXPORT)
        rep.check(bool(a and b and b.value > a.value), "UMD-007",
                  f"export counter {a.value:g} -> {b.value:g} kWh" if a and b else "no export counter")

    # ---- timeout: electrical + energy STALE, last good values kept, never GOOD (Q-003, Q-009)
    ph = _phase(phases, "timeout")
    d = _last(rep, dumps, ph, 15, "Q-003")
    if d:
        not_stale = [k for k in ELECTRICAL + ENERGY if k in d.vals and d.vals[k].q != "STALE"]
        rep.check(not not_stale, "Q-003", "electrical and energy parameters STALE" +
                  (f"; not STALE: {not_stale}" if not_stale else ""))
        p = d.vals.get(POWER)
        rep.check(p is not None and _close(p.value, -12.5), "Q-003",
                  f"active power keeps last GOOD value {p.value:g} kW (from export phase)" if p else "no power")
        # energy can wait behind failed electrical/status transactions (3.1 s each): 20 s margin
        late = [x for x in dumps_in(dumps, ph, 20)
                for k in ELECTRICAL + ENERGY if k in x.vals and x.vals[k].q == "GOOD"]
        rep.check(not late, "Q-009", "no electrical/energy value GOOD after the loss was detected")

    # ---- recovery (Q-007)
    d = _last(rep, dumps, _phase(phases, "recover"), 15, "Q-007")
    if d:
        bad = [k for k in ELECTRICAL + ENERGY if k in d.vals and d.vals[k].q != "GOOD"]
        rep.check(not bad, "Q-007", "back to GOOD after recovery" + (f"; not GOOD: {bad}" if bad else ""))
        v = d.vals.get("ac.voltage.l1_n")
        rep.check(v is not None and 0 <= v.age_ms <= 11000, "Q-007",
                  f"new timestamp (age {v.age_ms} ms)" if v else "no voltage")

    # ---- out of range (Q-006, SIM-009)
    d = _last(rep, dumps, _phase(phases, "out_of_range"), 8, "Q-006")
    if d:
        f = d.vals.get(FREQ)
        rep.check(f is not None and f.q == "OUT_OF_RANGE" and _close(f.value, 30.0), "Q-006",
                  f"frequency {f.value:g} Hz {f.q}" if f else "no frequency")
        v = d.vals.get("ac.voltage.l1_n")
        rep.check(v is not None and v.q == "GOOD", "Q-006", "other parameters of the group stay GOOD")

    # ---- exception on the energy group (Q-005)
    ph = _phase(phases, "exception_energy")
    d = _last(rep, dumps, ph, 15, "Q-005")
    if d:
        e = [d.vals[k].q for k in ENERGY if k in d.vals]
        rep.check(e and all(q == "INVALID" for q in e), "Q-005", f"energy parameters {e}")
        v = d.vals.get("ac.voltage.l1_n")
        rep.check(v is not None and v.q == "GOOD", "Q-005", "electrical group unaffected (GOOD)")
        evt = [s for s in lines_in(lines, ph) if "EVT_MODBUS_EXCEPTION" in s and "code=0x02" in s]
        rep.check(bool(evt), "Q-005", f"{len(evt)} EVT_MODBUS_EXCEPTION code=0x02 line(s)")

    # ---- counter reset (ENG-008)
    ph = _phase(phases, "counter_reset")
    if ph:
        evt = [s for s in lines_in(lines, ph) if "EVT_COUNTER_DISCONTINUITY" in s]
        rep.check(len(evt) == 1 and IMPORT in evt[0], "ENG-008",
                  f"{len(evt)} EVT_COUNTER_DISCONTINUITY (expected exactly 1, for {IMPORT})")
        d = _last(rep, dumps, ph, 15, "ENG-008")
        if d and IMPORT in d.vals:
            c = d.vals[IMPORT]
            rep.check(c.q == "GOOD" and c.value < 1.0, "ENG-008", f"import counter {c.value:g} kWh {c.q} after reset")

    # ---- whole run
    # The runner's own reset prints one ROM 'rst:' line before the first boot banner;
    # any reset line or second banner after that is a reboot.
    first_boot = next((i for i, s in enumerate(text_all) if BOOT_RE.search(s)), len(text_all))
    after = text_all[first_boot + 1:]
    crash = [s for s in text_all if CRASH_RE.search(s)]
    crash += [s for s in after if RESET_RE.search(s) or BOOT_RE.search(s)]
    rep.check(not crash, "stability", "no crash, reboot or critical stack after boot" +
              (f": {crash[0][:80]}" if crash else ""))
    stats = [m for m in (STATS_RE.search(s) for s in text_all) if m]
    if stats:
        rep.check(stats[-1].group(2) == "0" and stats[-1].group(1) == "0", "RTE",
                  f"last STATS: routing_errors={stats[-1].group(2)} guard_expired={stats[-1].group(1)}")
    else:
        rep.check(False, "RTE", "no STATS line (printed every 60 s)")
    stack = [int(m.group(3)) for m in (STACK_RE.search(s) for s in text_all) if m]
    if stack:
        rep.check(min(stack) >= 1024, "stack", f"ems_acquisition minimum free stack {min(stack)} B (>= 1024)")
    return rep


# ================================================================ running ===
class Console(threading.Thread):
    def __init__(self, ser, path: Path):
        super().__init__(daemon=True)
        self.ser, self.path = ser, path
        self.lines: list[tuple[float, str]] = []
        self.stop = threading.Event()
        self.lock = threading.Lock()

    def run(self) -> None:
        buf = b""
        with open(self.path, "w", encoding="utf-8") as fh:
            while not self.stop.is_set():
                chunk = self.ser.read(256)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    t = time.monotonic()
                    with self.lock:
                        self.lines.append((t, line))
                    fh.write(f"{t:.3f} {line}\n")
                    fh.flush()

    def seen(self, pattern: re.Pattern) -> bool:
        with self.lock:
            return any(pattern.search(ANSI_RE.sub("", s)) for _, s in self.lines)


def hard_reset(ser) -> None:
    """Same sequence as idf.py monitor: EN low via RTS, then release."""
    ser.rts = True
    ser.dtr = ser.dtr      # Windows: re-apply DTR so the RTS change is sent
    time.sleep(0.2)
    ser.rts = False
    ser.dtr = ser.dtr


def write_control(path: Path, mode: str) -> None:
    path.write_text(f"mode * {mode}\n", encoding="utf-8")


def run_suite(args) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial missing: python -m pip install -r tools/requirements-usb-rs485.txt", file=sys.stderr)
        return 2
    layout = sim_mod.load_layout(args.site)
    out = args.out_dir or (PROJECT_ROOT / "tools" / "test_results" /
                           f"wp4_d6_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}")
    out.mkdir(parents=True, exist_ok=True)
    ctl, sim_log = out / "control.txt", out / "sim.log"
    write_control(ctl, "NORMAL")
    print(f"Evidence: {out}")
    print("SAFETY: RS-485 must be connected only to the USB simulator (synthetic addresses).")

    sim_cmd = [sys.executable, str(HERE / "myems_ac_meter_sim.py"), "--site", str(args.site),
               "--port", args.rs485_port, "--log-file", str(sim_log), "--control-file", str(ctl)]
    with open(out / "sim_stdout.log", "w", encoding="utf-8") as sim_out:
        sim = subprocess.Popen(sim_cmd, stdout=sim_out, stderr=subprocess.STDOUT, cwd=PROJECT_ROOT)
    con = None
    phases: list[Phase] = []
    try:
        for _ in range(100):
            if sim.poll() is not None:
                raise RuntimeError(f"simulator exited (code {sim.returncode}); see sim_stdout.log")
            if sim_log.exists() and " PORT " in sim_log.read_text(encoding="utf-8", errors="replace"):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("simulator did not open its port within 10 s")
        print(f"Simulator running on {args.rs485_port}")

        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = args.console_port, 115200, 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
        con = Console(ser, out / "console.log")
        con.start()
        if not args.no_reset:
            hard_reset(ser)
            print("ESP32 reset; waiting for boot ...")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not con.seen(BOOT_RE):
            time.sleep(0.2)
        if not con.seen(BOOT_RE):
            print("No boot banner yet: press the EN/RESET button on the board (waiting 60 s) ...")
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and not con.seen(BOOT_RE):
                time.sleep(0.2)
        if not con.seen(BOOT_RE):
            raise RuntimeError("no BUILD_MODE line on the console: is the wp4_sim firmware flashed?")
        time.sleep(3)

        total = sum(d for _, _, d in SUITE_D6)
        print(f"Running {len(SUITE_D6)} phases, about {total // 60} min {total % 60} s")
        for name, mode, dur in SUITE_D6:
            write_control(ctl, mode)
            t0 = time.monotonic()
            print(f"  {dt.datetime.now():%H:%M:%S}  phase {name:<17} simulator {mode} for {dur} s")
            time.sleep(dur)
            phases.append(Phase(name, mode, t0, time.monotonic()))
        # wait for the next STATS line (every 60 s) if none has appeared yet
        deadline = time.monotonic() + 65
        while time.monotonic() < deadline and not con.seen(STATS_RE):
            time.sleep(0.5)
    finally:
        if con is not None:
            con.stop.set()
            con.join(timeout=2)
            con.ser.close()
        sim.terminate()
        try:
            sim.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sim.kill()

    (out / "phases.json").write_text(json.dumps({"crc": layout["config_crc32"],
                                                 "phases": [asdict(p) for p in phases]}, indent=2),
                                     encoding="utf-8")
    return report(out, con.lines if con else [], phases, layout["config_crc32"])


def report(out: Path, lines, phases, crc) -> int:
    rep = analyse_d6(lines, phases, crc)
    text = "\n".join([f"DP-WP4 D6 HIL report ({out.name})"] + rep.rows +
                     [f"RESULT: {'PASS' if rep.failures == 0 else f'FAIL ({rep.failures})'}"])
    (out / "report.txt").write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if rep.failures == 0 else 1


def load_saved(out: Path):
    meta = json.loads((out / "phases.json").read_text(encoding="utf-8"))
    phases = [Phase(**p) for p in meta["phases"]]
    lines = []
    for raw in (out / "console.log").read_text(encoding="utf-8", errors="replace").splitlines():
        t, _, text = raw.partition(" ")
        try:
            lines.append((float(t), text))
        except ValueError:
            continue
    return lines, phases, meta.get("crc")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="MyEMS DP-WP4 HIL runner (suite d6)")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--site", type=Path, default=PROJECT_ROOT / "config" / "site" / "lab_bench.site.json")
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--no-reset", action="store_true", help="do not reset the ESP32 via RTS (press EN instead)")
    ap.add_argument("--analyse", type=Path, metavar="RESULT_DIR", help="re-analyse a saved run, no hardware")
    a = ap.parse_args(argv)
    if a.analyse:
        lines, phases, crc = load_saved(a.analyse)
        return report(a.analyse, lines, phases, crc)
    try:
        return run_suite(a)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
