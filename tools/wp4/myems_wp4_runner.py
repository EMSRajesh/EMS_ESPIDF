#!/usr/bin/env python3
"""MyEMS DP-WP4/WP5 HIL runner (suites d6, d7, esc, reject, two_dev, ratio, load, soak, dc).

Runs the AC meter simulator on the RS-485 adapter, captures the ESP32 console,
steps the simulator through a sequence of modes (control file) and checks the
values and qualities the firmware publishes (VAL dump lines) in each phase.

Prerequisite: flash the firmware the suite expects, then close any serial
monitor on the console port:
    suites d6, d7:  .\\tools\\build_firmware.ps1 -Mode Wp4Sim -Port COM11 -Flash
    suite esc:      .\\tools\\build_firmware.ps1 -Mode Wp4Esc -Port COM11 -Flash
    suite reject:   .\\tools\\build_firmware.ps1 -Mode Production -Port COM11 -Flash
    suite dc:       .\\tools\\build_firmware.ps1 -Mode Wp5Dc -Port COM11 -Flash

Suites:
  d6      values and qualities (~5 min): UMD-001/004/006/007/008,
          Q-002/003/005/006/007/009, ENG-008, SIM-009
  d7      device state, probing, diagnostics (~5 min): CFG-019, DEV-001/002/
          004/006, SCH-006/007, DIAG-004/005
  esc     bus recovery escalation (~2 min, D8), on the Wp4Esc build whose
          transport test hook makes RS-485 recovery fail: the simulator stops
          answering, the first failed transaction takes the bus to BUS_FAULT;
          ESC-001/002/003 and the fail-closed hold are checked. ESC-004/005
          are reported BLOCK against COM change request CR-D8-01 (the 09_057
          master cannot retry recovery). Flash Wp4Sim again for other suites.
  reject  production firmware must refuse the simulation configuration
          (CFG-018); flash it first with -Mode Production (~1 min)
  two_dev two meters (-Mode Wp4Sim2Dev, site lab_bench_2dev, ~4 min): DEV-003,
          RTE-005, SCH-003 (no overlap)
  ratio   CT 200 (-Mode Wp4Ratio, site lab_ratio_ct200, ~1 min): CONV-004..006
          vectors of Test Spec 4.3 on the board
  load    PERF-001 bus load, two meters NORMAL (-Mode Wp4Sim2Dev); default
          60 min (--duration-min); also PERF-003/004/006
  soak    PERF-005 soak (-Mode Wp4Sim): NORMAL / TIMEOUT /
          OFFLINE_THEN_RECOVER:300 every 10 min; default 24 h
          (--duration-min 60 for the dry run); PERF-002..006. Ctrl+C stops
          early and still analyses what was captured.
  dc      DP-WP5 DC meter alone (-Mode Wp5Dc, site lab_dc_bench, slave 3,
          ~9 min): DC-001..DC-012. The expected values, signs and the
          out-of-range case come from the map (export-sim test vectors and
          parameter roles), not from DC constants in this file.
All suites also check: no reboot, no routing errors, stack margin.
Result: FAIL if any check fails; BLOCK rows (requirement not testable against
the current COM layer) are listed but do not fail the run.

Run (project root):
    python tools/wp4/myems_wp4_runner.py --suite d7 --console-port COM11 --rs485-port COM12
Re-analyse a saved run without hardware:
    python tools/wp4/myems_wp4_runner.py --analyse tools/test_results/wp4_d7_<timestamp>

Evidence is written to tools/test_results/wp4_<suite>_<timestamp>/:
console.log, sim.log, phases.json, report.txt.
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
VALUES_RE = re.compile(r"VALUES slot=(\d+) (\S+)(?: mode=([A-Z]+))? count=(\d+)")
VAL_RE = re.compile(r"VAL slot=(\d+) ([a-z0-9_.]+)=(\S+) (\S+) q=([A-Z_]+) age_ms=(-?\d+) seq=(\d+)")
BOOT_RE = re.compile(r"BUILD_MODE=\S+ CONFIG_TARGET=(\w+)")
VARIANT_RE = re.compile(r"BUILD_VARIANT=(\w+)")
# The firmware start line; must not match the rejection line "No polling started (...)".
POLL_START_RE = re.compile(r"MyEMS DP-WP4 polling started:")
CRC_RE = re.compile(r"CONFIG site=(\S+) .*crc=0x([0-9A-Fa-f]{8})")
STATS_RE = re.compile(r"STATS sched: .*guard_expired=(\d+).*routing_errors=(\d+)")
STACK_RE = re.compile(r"STACK task=ems_acquisition allocated=(\d+)B current_free=(\d+)B min_ever_free=(\d+)B")
EVT_RE = re.compile(r"EVT_([A-Z_]+)")
TIMEOUT_WARN_RE = re.compile(r"ems_acquisition: \S+ grp\d+ (TIMEOUT|CRC_ERROR|PROTOCOL_ERROR|TRANSPORT_ERROR) \(consecutive")
DIAG_DEV_RE = re.compile(r"DIAG slot=(\d+) \S+ mode=(\w+) .* offline=(\d+) online=(\d+)")
DIAG_BUS_RE = re.compile(r"DIAG bus .*sched_queue_hwm=(\d+).*events=(\d+) event_drops=(\d+)")
DIAG_DEV_TR_RE = re.compile(r"DIAG slot=(\d+) \S+ mode=(\w+) .* transport=(\d+) ")
DIAG_HEALTH_RE = re.compile(r"DIAG bus .*\| health=(\w+) reason=(\w+) transport=(\w+) bus_faults=(\d+)")
STATS_BUS_RE = re.compile(r"STATS sched: .*health=0x([0-9A-Fa-f]+) in_flight=(\d+) bus_holds=(\d+) "
                          r"bus_cancelled=(\d+)")
BUS_FAULT_RE = re.compile(r"EVT_BUS_FAULT bus=(\S+) reason=(\w+) transport=(\w+)")
MASTER_FAIL_RE = re.compile(r"RS485 bus unavailable; recovery failed: (.+)")
BUS_HOLD_RE = re.compile(r"BUS_HOLD: polling stopped")
BUS_CANCEL_RE = re.compile(r"BUS_HOLD: cancelled (\d+) request")
CANCELLED_RE = re.compile(r"ems_acquisition: \S+ grp\d+ request=0x[0-9A-Fa-f]+ CANCELLED")
PERF_RE = re.compile(r"PERF heap_free=(\d+) heap_min=(\d+) \| acq_proc_us elec_last=(\d+) elec_max=(\d+) "
                     r"max=(\d+) \| master_outstanding_hwm=(\d+) completion_backlog_hwm=(\d+)")
PERF_START_RE = re.compile(r"PERF heap_free_at_start=(\d+) heap_min_at_start=(\d+)")
STACK_ANY_RE = re.compile(r"STACK task=(\S+) allocated=(\d+)B current_free=(\d+)B min_ever_free=(\d+)B")
BACKPRESSURE_RE = re.compile(r"DIAG bus .* backpressure=(\d+)")
DIAG_ENTRY_RE = re.compile(r"DIAG entry=(\d+) slot=(\d+) grp=(\d+) submitted=(\d+) skipped=(\d+) rejected=(\d+) "
                           r"latency_ms=(\d+) max_latency_ms=(\d+)")
SIM_TX_RE = re.compile(r"^(\S+)\s+[0-9.]+ TX slave=(\d+) ")
SIM_RX_RE = re.compile(r"^(\S+)\s+[0-9.]+ RX slave=(\d+) fc=([0-9A-F]{2}) ((?:[0-9A-F]{2} )+)mode=")
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
    ("recover", "NORMAL", 45),     # D7 firmware: up to one 30 s probe interval to come back
    ("out_of_range", "OUT_OF_RANGE", 20),
    ("exception_energy", "EXCEPTION:02@energy", 30),
    ("counter_reset", "COUNTER_RESET", 25),
    ("final", "NORMAL", 15),
]

# D7: device state. OFFLINE_THEN_RECOVER:65 -> offline after 2 failures (~6-11 s),
# one failing probe 30 s later, the next probe (after 65 s) brings it back.
SUITE_D7 = [
    ("normal", "NORMAL", 25),
    ("offline_recover", "OFFLINE_THEN_RECOVER:65", 100),
    ("exception_energy", "EXCEPTION:02@energy", 25),
    ("bad_crc", "BAD_CRC", 30),
    ("recover", "NORMAL", 45),
    ("final", "NORMAL", 15),
]

# D8: bus recovery escalation, Wp4Esc build (RS-485 recovery forced to fail by
# the transport test hook). The simulator stops answering; the first failed
# transaction's recovery fails -> BUS_FAULT within ~8 s (next poll <= 5 s +
# 0.5 s timeout + 2 s recovery timeout); then polling stays held (fail closed)
# although the simulator answers again.
# (A babbling simulator, "BUS:BABBLE", does NOT make recovery fail with the
# 09_057 transport: it only sees RX activity per 120-byte UART FIFO event, see
# docs/D8_ESCALATION_REVIEW.md, CR-D8-01 (e).)
SUITE_ESC = [
    ("normal", "NORMAL", 25),
    ("fault", "TIMEOUT", 15),
    ("fail_closed", "NORMAL", 75),
]
ESC_VARIANT = "WP4_ESC_TEST"

# Production firmware with the simulation configuration: must refuse to poll.
SUITE_REJECT = [
    ("observe", "NORMAL", 25),
]

# D9: two meters on one bus. Slave 1 stops answering; slave 2 must not notice
# (DEV-003), then slave 1 comes back through probing.
SUITE_TWO_DEV = [
    ("normal", "NORMAL", 40),
    ("dev1_offline", "1=TIMEOUT", 90),
    ("recover", "NORMAL", 45),
    ("final", "NORMAL", 15),
]

# D9: Test Spec 4.3 ratio vectors on the board (meter-side secondary values, CT 200).
RATIO_SETS = "set 1 ac.current.l1 2.5|set 1 ac.current.l2 6.0|set 1 ac.power.active.total 1.0"
SUITE_RATIO = [
    ("vectors", "NORMAL|" + RATIO_SETS, 30),
]

# DP-WP5 M4: DC meter alone (Test Spec 9, DC-001..DC-012), site lab_dc_bench.
SUITE_DC = [
    ("normal", "NORMAL", 40),
    ("import", "IMPORT", 40),
    ("export", "EXPORT", 40),
    ("out_of_range", "OUT_OF_RANGE", 20),
    ("counter_reset", "COUNTER_RESET", 25),
    ("slow", "SLOW:400", 30),
    ("exception_energy", "EXCEPTION:02@energy", 25),
    ("timeout", "TIMEOUT", 30),
    ("recover", "NORMAL", 45),
    ("offline_recover", "OFFLINE_THEN_RECOVER:65", 100),
    ("bad_crc", "BAD_CRC", 30),
    ("recover_crc", "NORMAL", 45),
    ("final", "NORMAL", 15),
]
DC_SLAVE = 3

SOAK_CYCLE = [("normal", "NORMAL"), ("timeout", "TIMEOUT"), ("offline_recover", "OFFLINE_THEN_RECOVER:300")]
SOAK_PHASE_S = 600
OTR_SILENT_S = 300


def soak_phases(duration_min: int) -> list[tuple[str, str, int]]:
    """PERF-005: the simulator mode changes every 10 min, cycling SOAK_CYCLE."""
    n = max(len(SOAK_CYCLE), duration_min * 60 // SOAK_PHASE_S)
    return [(f"{SOAK_CYCLE[k % 3][0]}_{k + 1}", SOAK_CYCLE[k % 3][1], SOAK_PHASE_S) for k in range(n)]


def load_phases(duration_min: int) -> list[tuple[str, str, int]]:
    return [("load", "NORMAL", max(60, duration_min * 60))]


SUITES = {"d6": SUITE_D6, "d7": SUITE_D7, "esc": SUITE_ESC, "reject": SUITE_REJECT,
          "two_dev": SUITE_TWO_DEV, "ratio": SUITE_RATIO, "load": None, "soak": None, "dc": SUITE_DC}
BOOT_TARGET = {"d6": "sim", "d7": "sim", "esc": "sim", "reject": "prod", "two_dev": "sim", "ratio": "sim",
               "load": "sim", "soak": "sim", "dc": "sim"}
SUITE_SITE = {"two_dev": "lab_bench_2dev", "load": "lab_bench_2dev", "ratio": "lab_ratio_ct200",
              "dc": "lab_dc_bench"}
SUITE_BUILD = {"two_dev": "Wp4Sim2Dev", "load": "Wp4Sim2Dev", "ratio": "Wp4Ratio", "esc": "Wp4Esc",
               "reject": "Production", "dc": "Wp5Dc"}
DEFAULT_DURATION_MIN = {"load": 60, "soak": 1440}


def suite_phases(name: str, duration_min: int | None = None) -> list[tuple[str, str, int]]:
    if name == "soak":
        return soak_phases(duration_min or DEFAULT_DURATION_MIN["soak"])
    if name == "load":
        return load_phases(duration_min or DEFAULT_DURATION_MIN["load"])
    return SUITES[name]


def suite_site(name: str) -> Path:
    return PROJECT_ROOT / "config" / "site" / f"{SUITE_SITE.get(name, 'lab_bench')}.site.json"
CR_D8 = "COM change request CR-D8-01 (docs/D8_ESCALATION_REVIEW.md)"


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
    mode: str = ""
    vals: dict[str, Val] = field(default_factory=dict)


@dataclass
class Phase:
    name: str
    mode: str
    t0: float              # monotonic host time (console timestamps)
    t1: float
    w0: float | None = None  # wall clock (time.time()) at t0, to align the simulator log
    w1: float | None = None


def parse_console(lines: list[tuple[float, str]]) -> list[Dump]:
    dumps: list[Dump] = []
    current: dict[int, Dump] = {}
    for t, raw in lines:
        text = ANSI_RE.sub("", raw)
        m = VALUES_RE.search(text)
        if m:
            d = Dump(t, int(m.group(1)), m.group(3) or "")
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
    blocked: int = 0

    def check(self, ok: bool, test: str, text: str) -> bool:
        self.rows.append(f"{'PASS' if ok else 'FAIL'}  {test:<10} {text}")
        if not ok:
            self.failures += 1
        return ok

    def block(self, test: str, text: str) -> None:
        """Requirement that cannot be tested against the current COM layer (recorded, not failed)."""
        self.rows.append(f"BLOCK {test:<10} {text}")
        self.blocked += 1

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


def _whole_run(rep: Report, text_all: list[str]) -> None:
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


def _boot(rep: Report, text_all: list[str], target: str, expected_crc: str | None,
          variant: str | None = None) -> None:
    boot = [m for m in (BOOT_RE.search(s) for s in text_all) if m]
    rep.check(bool(boot) and boot[-1].group(1) == target, "boot",
              f"BUILD_MODE ... CONFIG_TARGET={boot[-1].group(1)} (expected {target})" if boot
              else "no BUILD_MODE line (was the board reset?)")
    got = [m.group(1) for m in (VARIANT_RE.search(s) for s in text_all) if m]
    got_v = got[-1] if got else None
    if variant is not None or got_v is not None:
        hint = "flash -Mode Wp4Esc" if variant == ESC_VARIANT else "flash -Mode Wp4Sim"
        rep.check(got_v == variant, "boot", f"BUILD_VARIANT={got_v or 'none'} (expected {variant or 'none'})"
                  + ("" if got_v == variant else f": {hint}"))
    crc = [m for m in (CRC_RE.search(s) for s in text_all) if m]
    if expected_crc is not None:
        got = crc[-1].group(2).upper() if crc else None
        rep.check(got == expected_crc.upper().replace("0X", ""), "config",
                  f"firmware config CRC 0x{got} = simulator layout CRC {expected_crc}" if got else "no CONFIG line")


def analyse_d6(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
               sim_lines: list[str] | None = None) -> Report:
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    normal = sim_mod.NORMAL_VALUES

    # ---- boot and configuration
    _boot(rep, text_all, "sim", expected_crc)
    rep.check(any(POLL_START_RE.search(s) for s in text_all), "config", "polling started")
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
        # STALE after the first failure; COMM_LOST once the device is OFFLINE (D7 firmware)
        not_stale = [k for k in ELECTRICAL + ENERGY if k in d.vals and d.vals[k].q not in ("STALE", "COMM_LOST")]
        rep.check(not not_stale, "Q-003", "electrical and energy parameters STALE/COMM_LOST" +
                  (f"; not: {not_stale}" if not_stale else ""))
        p = d.vals.get(POWER)
        rep.check(p is not None and _close(p.value, -12.5), "Q-003",
                  f"active power keeps last GOOD value {p.value:g} kW (from export phase)" if p else "no power")
        # energy can wait behind failed electrical/status transactions (3.1 s each): 20 s margin
        late = [x for x in dumps_in(dumps, ph, 20)
                for k in ELECTRICAL + ENERGY if k in x.vals and x.vals[k].q == "GOOD"]
        rep.check(not late, "Q-009", "no electrical/energy value GOOD after the loss was detected")

    # ---- recovery (Q-007)
    d = _last(rep, dumps, _phase(phases, "recover"), 38, "Q-007")
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

    _whole_run(rep, text_all)
    return rep


# ------------------------------------------------------------- D7 suite ---
def parse_sim_rx(sim_lines: list[str]) -> list[tuple[float, int, int]]:
    """Simulator log -> (wall time, slave, start address) of every received request."""
    out = []
    for ln in sim_lines:
        m = SIM_RX_RE.match(ln)
        if not m:
            continue
        b = bytes.fromhex(m.group(4))
        if len(b) < 6:
            continue
        try:
            w = dt.datetime.fromisoformat(m.group(1)).timestamp()
        except ValueError:
            continue
        out.append((w, int(m.group(2)), (b[2] << 8) | b[3]))
    return out


def transactions(times: list[float], gap: float = 5.0) -> list[float]:
    """Start times of transactions: a retry follows its request within ~1.6 s."""
    starts: list[float] = []
    for t in sorted(times):
        if not starts or t - starts[-1] > gap:
            starts.append(t)
    return starts


def _evt_times(lines: list[tuple[float, str]], name: str, ph: Phase | None) -> list[float]:
    if ph is None:
        return []
    return [t for t, s in lines if ph.t0 <= t <= ph.t1 and f"EVT_{name}" in ANSI_RE.sub("", s)]


def _to_wall(ph: Phase, t_mono: float) -> float | None:
    return None if ph.w0 is None else ph.w0 + (t_mono - ph.t0)


def analyse_d7(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
               sim_lines: list[str] | None = None, status_start: int = 18) -> Report:
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    rx = parse_sim_rx(sim_lines or [])

    _boot(rep, text_all, "sim", expected_crc)
    rep.check(any("EVT_CONFIG_ACTIVATED" in s for s in text_all), "CFG-019",
              "EVT_CONFIG_ACTIVATED after the activation checks")
    rep.info(f"{len(dumps)} value dumps, {len(rx)} simulator requests captured")

    d = _last(rep, dumps, _phase(phases, "normal"), 15, "DEV")
    if d:
        bad = [k for k, v in d.vals.items() if v.q != "GOOD"]
        rep.check(d.mode == "ONLINE" and not bad, "DEV", f"normal: mode {d.mode}, all GOOD"
                  + (f"; not GOOD: {bad}" if bad else ""))

    # ---- offline -> probe -> recover (DEV-001, SCH-006, DEV-002, SCH-007)
    ph = _phase(phases, "offline_recover")
    off = _evt_times(lines, "DEVICE_OFFLINE", ph)
    on = _evt_times(lines, "DEVICE_ONLINE", ph)
    if rep.check(len(off) == 1, "DEV-001", f"{len(off)} EVT_DEVICE_OFFLINE (expected 1)") and ph:
        rep.check(off[0] - ph.t0 <= 16.0, "DEV-001",
                  f"OFFLINE {off[0] - ph.t0:.1f} s after the simulator stopped answering (<= 16 s: "
                  "next poll <= 5 s + 2 failed transactions)")
        t_end = on[0] if on else ph.t1
        mid = [x for x in dumps if x.slot == 0 and off[0] + 0.5 < x.t < t_end - 0.5]
        if rep.check(bool(mid), "DEV-001", f"{len(mid)} value dump(s) while OFFLINE"):
            x = mid[-1]
            nl = [k for k, v in x.vals.items() if v.q != "COMM_LOST"]
            rep.check(x.mode == "OFFLINE" and not nl, "DEV-001",
                      f"mode {x.mode}, all parameters COMM_LOST" + (f"; not: {nl}" if nl else ""))
            p = x.vals.get(POWER)
            rep.check(p is not None and p.value != 0.0, "Q-009",
                      f"COMM_LOST keeps the last value ({p.value:g} kW), never 0" if p else "no power")
        w_off, w_on = _to_wall(ph, off[0]), _to_wall(ph, t_end)
        if w_off is not None and rx:
            between = [r for r in rx if w_off + 0.2 < r[0] < w_on - 0.2]
            others = [r for r in between if r[2] != status_start]
            rep.check(not others, "SCH-006", f"only the status group polled while OFFLINE "
                      f"({len(between)} request(s), {len(others)} other)")
            probes = transactions([r[0] for r in rx if r[2] == status_start and w_off + 0.2 < r[0] <= w_on + 0.5])
            if rep.check(len(probes) >= 2, "SCH-006", f"{len(probes)} probe transaction(s) while OFFLINE"):
                first = probes[0] - w_off
                gaps = [b - a for a, b in zip(probes, probes[1:])]
                rep.check(29.0 <= first <= 32.0 and all(abs(g - 30.0) <= 1.0 for g in gaps), "SCH-006",
                          f"first probe {first:.1f} s after OFFLINE, then every "
                          f"{', '.join(f'{g:.1f}' for g in gaps)} s (30 +- 1 s)")
        elif w_off is None:
            rep.info("phase wall times missing: simulator log not aligned, SCH-006 not checked")
    if rep.check(len(on) == 1, "DEV-002", f"{len(on)} EVT_DEVICE_ONLINE (expected 1)") and ph:
        rep.check(on[0] - ph.t0 >= 64.0, "DEV-002", f"ONLINE {on[0] - ph.t0:.1f} s into the phase "
                  "(simulator answers again after 65 s)")
        w_on = _to_wall(ph, on[0])
        if w_on is not None and rx:
            after = [r for r in rx if w_on - 0.5 <= r[0] <= w_on + 3.0 and r[2] != status_start]
            starts = {r[2] for r in after}
            rep.check(len(starts) >= 2, "SCH-007",
                      f"after ONLINE all groups polled at once (start addresses {sorted(starts)} within 3 s)")
        d = _last(rep, dumps, ph, 0, "DEV-002")
        if d:
            bad = [k for k in ELECTRICAL + ENERGY if k in d.vals and d.vals[k].q != "GOOD"]
            rep.check(d.mode == "ONLINE" and not bad, "DEV-002", f"end of phase: mode {d.mode}, groups GOOD again"
                      + (f"; not GOOD: {bad}" if bad else ""))
    if ph:
        warns = [t for t, s in lines if ph.t0 <= t <= ph.t1 and TIMEOUT_WARN_RE.search(ANSI_RE.sub("", s))]
        gaps = [b - a for a, b in zip(warns, warns[1:])]
        rep.check(bool(warns) and all(g >= 9.9 for g in gaps), "DIAG-004",
                  f"{len(warns)} failure warning(s), min spacing "
                  f"{min(gaps) if gaps else float('nan'):.1f} s (<= 1 per device per 10 s)")

    # ---- exception: device stays ONLINE (DEV-004)
    ph = _phase(phases, "exception_energy")
    if ph:
        rep.check(not _evt_times(lines, "DEVICE_OFFLINE", ph), "DEV-004", "no EVT_DEVICE_OFFLINE during exceptions")
        d = _last(rep, dumps, ph, 15, "DEV-004")
        if d:
            e = [d.vals[k].q for k in ENERGY if k in d.vals]
            rep.check(d.mode == "ONLINE" and e and all(q == "INVALID" for q in e), "DEV-004",
                      f"mode {d.mode}, energy {e}")

    # ---- bad CRC: same as timeout (DEV-006)
    ph = _phase(phases, "bad_crc")
    off = _evt_times(lines, "DEVICE_OFFLINE", ph)
    if rep.check(len(off) == 1, "DEV-006", f"{len(off)} EVT_DEVICE_OFFLINE on BAD_CRC (expected 1)") and ph:
        x = [y for y in dumps if y.slot == 0 and off[0] + 0.5 < y.t <= ph.t1]
        if x:
            nl = [k for k, v in x[-1].vals.items() if v.q != "COMM_LOST"]
            rep.check(x[-1].mode == "OFFLINE" and not nl, "DEV-006", f"mode {x[-1].mode}, all COMM_LOST"
                      + (f"; not: {nl}" if nl else ""))

    ph = _phase(phases, "recover")
    on = _evt_times(lines, "DEVICE_ONLINE", ph)
    if rep.check(len(on) == 1, "DEV-002", f"{len(on)} EVT_DEVICE_ONLINE after BAD_CRC ended") and ph:
        rep.check(on[0] - ph.t0 <= 33.0, "DEV-002", f"ONLINE {on[0] - ph.t0:.1f} s later (<= one probe interval)")
        d = _last(rep, dumps, ph, 0, "DEV-002")
        if d:
            bad = [k for k in ELECTRICAL + ENERGY if k in d.vals and d.vals[k].q != "GOOD"]
            rep.check(d.mode == "ONLINE" and not bad, "DEV-002", f"mode {d.mode}, values GOOD"
                      + (f"; not GOOD: {bad}" if bad else ""))

    # ---- diagnostics (DIAG-005) and counters
    devs = [m for m in (DIAG_DEV_RE.search(s) for s in text_all) if m]
    if rep.check(bool(devs), "DIAG-005", f"{len(devs)} per-device DIAG line(s)"):
        last = devs[-1]
        rep.check(int(last.group(3)) >= 2 and int(last.group(4)) >= 2, "DIAG",
                  f"last DIAG: offline transitions {last.group(3)}, online transitions {last.group(4)} (>= 2 each)")
    bus = [m for m in (DIAG_BUS_RE.search(s) for s in text_all) if m]
    if rep.check(bool(bus), "DIAG-005", "DIAG bus line with scheduler queue high-water mark"):
        rep.check(bus[-1].group(3) == "0", "DIAG-003",
                  f"events {bus[-1].group(2)}, event drops {bus[-1].group(3)}, queue high-water {bus[-1].group(1)}")
    rep.check(any(STACK_RE.search(s) for s in text_all), "DIAG-005", "stack high-water lines present")
    _whole_run(rep, text_all)
    return rep


# --------------------------------------------------------- reject suite ---
def analyse_reject(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                   sim_lines: list[str] | None = None) -> Report:
    rep = Report()
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    _boot(rep, text_all, "prod", None)
    rej = [s for s in text_all if "EVT_CONFIG_REJECTED" in s]
    rep.check(bool(rej) and "rule=V-13" in rej[0], "CFG-018",
              f"EVT_CONFIG_REJECTED: {rej[0].split('EVT_CONFIG_REJECTED', 1)[1].strip()[:90]}" if rej
              else "no EVT_CONFIG_REJECTED line")
    rep.check(not any(POLL_START_RE.search(s) or "EVT_CONFIG_ACTIVATED" in s for s in text_all), "CFG-018",
              "polling not started")
    rep.check(not parse_console(lines), "CFG-018", "no values published")
    rx = parse_sim_rx(sim_lines or [])
    rep.check(not rx, "CFG-018", f"{len(rx)} request(s) on the RS-485 bus (expected 0)")
    first_boot = next((i for i, s in enumerate(text_all) if BOOT_RE.search(s)), len(text_all))
    after = text_all[first_boot + 1:]
    crash = [s for s in text_all if CRASH_RE.search(s)] + [s for s in after if RESET_RE.search(s) or BOOT_RE.search(s)]
    rep.check(not crash, "stability", "no crash or reboot after boot")
    return rep


# ------------------------------------------------------------ esc suite ---
def analyse_esc(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                sim_lines: list[str] | None = None) -> Report:
    """D8, Design 9.7 / Test Spec 17 against the 09_057 master (no health API, no recovery retry)."""
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    rx = parse_sim_rx(sim_lines or [])
    _boot(rep, text_all, "sim", expected_crc, ESC_VARIANT)
    rep.check(any("EVT_CONFIG_ACTIVATED" in s for s in text_all), "CFG-019", "EVT_CONFIG_ACTIVATED")

    d = _last(rep, dumps, _phase(phases, "normal"), 15, "ESC")
    if d:
        bad = [k for k, v in d.vals.items() if v.q != "GOOD"]
        rep.check(d.mode == "ONLINE" and not bad, "ESC", f"before the fault: mode {d.mode}, all GOOD"
                  + (f"; not GOOD: {bad}" if bad else ""))

    fault, closed = _phase(phases, "fault"), _phase(phases, "fail_closed")

    # ---- ESC-002: BUS_FAULT detected and reported
    faults = [(t, BUS_FAULT_RE.search(ANSI_RE.sub("", s))) for t, s in lines]
    faults = [(t, m) for t, m in faults if m]
    t_fault = faults[0][0] if faults else None
    if rep.check(len(faults) == 1, "ESC-002", f"{len(faults)} EVT_BUS_FAULT (expected 1)") and fault:
        t, m = faults[0]
        rep.check(fault.t0 <= t <= fault.t0 + 12.0, "ESC-002",
                  f"BUS_FAULT {t - fault.t0:.1f} s after the fault phase started (<= 12 s), "
                  f"reason={m.group(2)} transport={m.group(3)}")

    # ---- ESC-001: terminal TRANSPORT_ERROR, no stuck slot
    mfail = [m.group(1) for m in (MASTER_FAIL_RE.search(s) for s in text_all) if m]
    rep.check(bool(mfail), "ESC-001", "master published a terminal TRANSPORT_ERROR after recovery FAILED"
              + (f" ({mfail[0].strip()[:60]})" if mfail else ""))
    tr = [int(m.group(3)) for m in (DIAG_DEV_TR_RE.search(s) for s in text_all) if m]
    if tr:
        rep.check(tr[-1] >= 1, "ESC-001", f"device transport errors {tr[-1]} (>= 1, from DIAG)")
    rep.check(any(BUS_HOLD_RE.search(s) for s in text_all), "ESC-001", "scheduler held polling (BUS_HOLD)")
    cancelled = sum(int(m.group(1)) for m in (BUS_CANCEL_RE.search(s) for s in text_all) if m)
    acq_cancel = sum(1 for s in text_all if CANCELLED_RE.search(s))
    rep.check(acq_cancel == cancelled, "ESC-001",
              f"{cancelled} request(s) cancelled in the master, {acq_cancel} CANCELLED result(s) consumed")
    st_after = [(t, m) for t, m in ((t, STATS_BUS_RE.search(ANSI_RE.sub("", s))) for t, s in lines)
                if m and t_fault is not None and t > t_fault + 1.0]
    if rep.check(bool(st_after), "ESC-001", f"{len(st_after)} STATS line(s) after the fault"):
        m = st_after[-1][1]
        flags = int(m.group(1), 16)
        rep.check(m.group(2) == "0", "ESC-001", f"no stuck slot: in_flight={m.group(2)} after the fault")
        rep.check(flags & 0x02 != 0 and m.group(3) == "1", "ESC-002",
                  f"scheduler health=0x{flags:02X} (BUS_HOLD 0x02 raised), bus_holds={m.group(3)}")

    # ---- ESC-003: devices OFFLINE, COMM_LOST, last value kept
    off = [t for t, s in lines if "EVT_DEVICE_OFFLINE" in ANSI_RE.sub("", s)]
    rep.check(len(off) == 1 and t_fault is not None and off[0] >= t_fault - 0.5, "ESC-003",
              f"{len(off)} EVT_DEVICE_OFFLINE (expected 1, with the bus fault)")
    after = [x for x in dumps if x.slot == 0 and t_fault is not None and x.t > t_fault + 0.5]
    if rep.check(bool(after), "ESC-003", f"{len(after)} value dump(s) after the fault"):
        x = after[-1]
        nl = [k for k, v in x.vals.items() if v.q != "COMM_LOST"]
        rep.check(x.mode == "OFFLINE" and not nl, "ESC-003",
                  f"mode {x.mode}, all parameters COMM_LOST" + (f"; not: {nl}" if nl else ""))
        p = x.vals.get(POWER)
        rep.check(p is not None and p.value != 0.0, "Q-009",
                  f"COMM_LOST keeps the last value ({p.value:g} kW), never 0" if p else "no power")

    # ---- fail closed: the bus stays held, health reported (ESC-002 / Design 9.7 step 6)
    if closed:
        held = [r for r in rx if closed.w0 is not None and closed.w0 + 1.0 < r[0] < (closed.w1 or 0)]
        if closed.w0 is not None:
            rep.check(not held, "ESC-002", f"fail closed: {len(held)} request(s) on the bus after the fault "
                      "(expected 0)")
        else:
            rep.info("phase wall times missing: bus silence not checked")
    health = [m for m in (DIAG_HEALTH_RE.search(s) for s in text_all) if m]
    if rep.check(bool(health), "ESC-002", "DIAG bus line with bus health"):
        h = health[-1]
        rep.check(h.group(1) == "BUS_FAULT" and h.group(4) == "1", "ESC-002",
                  f"last DIAG bus: health={h.group(1)} reason={h.group(2)} transport={h.group(3)} "
                  f"bus_faults={h.group(4)}")

    # ---- ESC-004 / ESC-005: need the master side of Design 9.7 (steps 2, 3)
    rep.block("ESC-004", f"recovery backoff 2 s / 5 s / 30 s: the master does not retry recovery; {CR_D8}")
    oks = [s for s in text_all if "EVT_BUS_OK" in s]
    rep.check(not oks, "ESC-005", "BUS_FAULT latched without a master health report (no EVT_BUS_OK): "
              "fail closed as designed")
    rep.block("ESC-005", f"EVT_BUS_OK and recovery through probing: needs bus health from the master; {CR_D8}")
    _whole_run(rep, text_all)
    return rep



# ============================================================== D9 suites ===
def parse_sim_tx(sim_lines: list[str]) -> list[tuple[float, int]]:
    """Simulator log -> (wall time, slave) of every reply sent."""
    out = []
    for ln in sim_lines:
        m = SIM_TX_RE.match(ln)
        if m:
            try:
                out.append((dt.datetime.fromisoformat(m.group(1)).timestamp(), int(m.group(2))))
            except ValueError:
                continue
    return out


def _no_overlap(rep: Report, sim_lines: list[str], test: str) -> None:
    """SCH-003 / PERF-001: one transaction at a time on the bus (simulator view)."""
    overlaps = [ln for ln in sim_lines if "OVERLAP" in ln]
    rep.check(not overlaps, test, f"{len(overlaps)} OVERLAP line(s) in the simulator log (expected 0)")


def _perf(rep: Report, text_all: list[str], elec_limit_us: int = 2000) -> None:
    """PERF-002/003/004/006 from the PERF, STACK and DIAG bus lines."""
    perf = [m for m in (PERF_RE.search(s) for s in text_all) if m]
    if not rep.check(bool(perf), "PERF", f"{len(perf)} PERF line(s) (printed every 60 s)"):
        return
    first, last = perf[0], perf[-1]
    start = [m for m in (PERF_START_RE.search(s) for s in text_all) if m]
    ref = int(start[0].group(1)) if start else int(first.group(1))
    drift = ref - int(last.group(1))
    rep.check(abs(drift) < 1024 and abs(int(first.group(1)) - int(last.group(1))) < 1024, "PERF-002",
              f"free heap at start {ref} B, first PERF {first.group(1)} B, last {last.group(1)} B "
              f"(change {drift:+d} B, limit 1024 B); minimum ever {last.group(2)} B")
    stacks: dict[str, int] = {}
    for m in (STACK_ANY_RE.search(s) for s in text_all):
        if m:
            stacks[m.group(1)] = min(stacks.get(m.group(1), 1 << 30), int(m.group(4)))
    need = {"ems_acquisition", "ems_scheduler", "modbus_master", "rs485_transport", "main"}
    missing = sorted(need - set(stacks))
    low = {k: v for k, v in stacks.items() if v < 1024}
    rep.check(not missing and not low, "PERF-003",
              "minimum free stack: " + ", ".join(f"{k} {v} B" for k, v in sorted(stacks.items()))
              + (f"; missing: {missing}" if missing else "") + (f"; below 1024 B: {low}" if low else ""))
    out_hwm, back_hwm = int(last.group(6)), int(last.group(7))
    bp = [int(m.group(1)) for m in (BACKPRESSURE_RE.search(s) for s in text_all) if m]
    rep.check(out_hwm <= 4 and back_hwm <= 4 and (not bp or bp[-1] == 0), "PERF-004",
              f"request queue + active high-water {out_hwm}, completion queue high-water {back_hwm} (<= 4); "
              f"backpressure rejections {bp[-1] if bp else 'n/a'}")
    rep.check(int(last.group(4)) <= elec_limit_us, "PERF-006",
              f"acquisition processing, electrical group: max {last.group(4)} us, last {last.group(3)} us "
              f"(<= {elec_limit_us} us); any result max {last.group(5)} us")


def analyse_two_dev(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                    sim_lines: list[str] | None = None) -> Report:
    """D9: DEV-003, RTE-005, SCH-003 with site lab_bench_2dev (slot 0 = slave 1, slot 1 = slave 2)."""
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    rx = parse_sim_rx(sim_lines or [])
    _boot(rep, text_all, "sim", expected_crc)
    rep.info(f"{len(dumps)} value dumps, {len(rx)} simulator requests captured")
    normal = sim_mod.NORMAL_VALUES

    ph = _phase(phases, "normal")
    for slot in (0, 1):
        ds = dumps_in(dumps, ph, 15, slot) if ph else []
        if rep.check(bool(ds), "RTE-005", f"slot {slot}: {len(ds)} dump(s) in phase 'normal'"):
            d = ds[-1]
            bad = [k for k, v in d.vals.items() if v.q != "GOOD" or not _close(v.value, normal.get(k, v.value))]
            rep.check(d.mode == "ONLINE" and len(d.vals) == 14 and not bad, "RTE-005",
                      f"slot {slot}: mode {d.mode}, 14 parameters GOOD with the Test Spec 4.1 values"
                      + (f"; wrong: {bad}" if bad else ""))

    ph = _phase(phases, "dev1_offline")
    if ph:
        off = _evt_times(lines, "DEVICE_OFFLINE", ph)
        offs = [s for t, s in lines if ph.t0 <= t <= ph.t1 and "EVT_DEVICE_OFFLINE" in s]
        rep.check(len(off) == 1 and "slot=0" in ANSI_RE.sub("", offs[0]) and off[0] - ph.t0 <= 16.0, "DEV-003",
                  f"{len(off)} EVT_DEVICE_OFFLINE, slot 0 only, "
                  f"{(off[0] - ph.t0) if off else float('nan'):.1f} s after slave 1 stopped answering (<= 16 s)")
        d0 = dumps_in(dumps, ph, 30, 0)
        if d0:
            nl = [k for k, v in d0[-1].vals.items() if v.q != "COMM_LOST"]
            rep.check(d0[-1].mode == "OFFLINE" and not nl, "DEV-003", f"slot 0: mode {d0[-1].mode}, all COMM_LOST"
                      + (f"; not: {nl}" if nl else ""))
        d1 = dumps_in(dumps, ph, 0, 1)
        bad1 = [(round(x.t - ph.t0, 1), k) for x in d1 for k, v in x.vals.items() if v.q != "GOOD"]
        rep.check(bool(d1) and all(x.mode == "ONLINE" for x in d1) and not bad1, "DEV-003",
                  f"slot 1: {len(d1)} dump(s), always ONLINE and all GOOD" + (f"; not GOOD: {bad1[:3]}" if bad1 else ""))
        if ph.w0 is not None and rx:
            # a retry follows its request within ~1.6 s; polls are ~5 s apart
            starts = transactions([r[0] for r in rx if r[1] == 2 and r[2] == 0 and ph.w0 <= r[0] <= (ph.w1 or 0)],
                                  gap=2.0)
            gaps = [b - a for a, b in zip(starts, starts[1:])]
            rep.check(len(gaps) >= 10 and all(5.0 - 3.2 <= g <= 5.0 + 3.2 for g in gaps), "DEV-003",
                      f"slave 2 electrical poll interval {min(gaps) if gaps else float('nan'):.2f}..."
                      f"{max(gaps) if gaps else float('nan'):.2f} s over {len(gaps)} interval(s) (5 s +- 3.2 s)")

    ph = _phase(phases, "recover")
    if ph:
        on = _evt_times(lines, "DEVICE_ONLINE", ph)
        rep.check(len(on) == 1 and on[0] - ph.t0 <= 33.0, "DEV-002",
                  f"{len(on)} EVT_DEVICE_ONLINE, {(on[0] - ph.t0) if on else float('nan'):.1f} s after slave 1 "
                  "answers again (<= one probe interval)")
    ph = _phase(phases, "final")
    for slot in (0, 1):
        ds = dumps_in(dumps, ph, 0, slot) if ph else []
        if rep.check(bool(ds), "RTE-005", f"slot {slot}: {len(ds)} dump(s) in phase 'final'"):
            d = ds[-1]
            bad = [k for k, v in d.vals.items() if v.q != "GOOD"]
            rep.check(d.mode == "ONLINE" and not bad, "RTE-005", f"end: slot {slot} mode {d.mode}, all GOOD"
                      + (f"; not GOOD: {bad}" if bad else ""))
    _no_overlap(rep, sim_lines or [], "SCH-003")
    _whole_run(rep, text_all)
    return rep


# Test Spec 4.3 with CT 200, PT 1 (secondary values from RATIO_SETS; others NORMAL)
RATIO_EXPECT = {
    "ac.current.l1": (500.0, "GOOD"),
    "ac.current.l2": (1200.0, "OUT_OF_RANGE"),
    "ac.voltage.l1_n": (230.0, "GOOD"),
    "ac.power.active.total": (200.0, "GOOD"),
    "ac.energy.import.active": (20050.0, "GOOD"),     # 100.25 kWh x CT, not x CT^2
    "ac.frequency": (50.0, "GOOD"),
}


def analyse_ratio(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                  sim_lines: list[str] | None = None) -> Report:
    """D9: CONV-004..006 on the board (site lab_ratio_ct200)."""
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    _boot(rep, text_all, "sim", expected_crc)
    d = _last(rep, dumps, _phase(phases, "vectors"), 15, "CONV")
    if d:
        for k, (want, q) in RATIO_EXPECT.items():
            v = d.vals.get(k)
            test = "CONV-005" if k == "ac.energy.import.active" else ("CONV-006" if q != "GOOD" else "CONV-004")
            rep.check(v is not None and _close(v.value, want) and v.q == q, test,
                      f"{k} = {v.value:g} {v.unit} {v.q} (expected {want:g} {q})" if v else f"{k} missing")
    _whole_run(rep, text_all)
    return rep


def _budget(site: Path | None):
    """Budget of the site from the profile tool (same code as 'budget')."""
    if site is None:
        return None
    sys.path.insert(0, str(PROJECT_ROOT / "tools" / "profile"))
    import myems_profile_tool as tool  # noqa: E402
    model, _ = tool.build_model(Path(site), "sim")
    return None if model is None else tool.budget(model)


def analyse_load(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                 sim_lines: list[str] | None = None, site: Path | None = None) -> Report:
    """D9 PERF-001: bus load of two meters in NORMAL against the budget; PERF-003/004/006."""
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    _boot(rep, text_all, "sim", expected_crc)
    ph = _phase(phases, "load")
    b = _budget(site or suite_site("load"))
    if ph is None or b is None:
        rep.check(False, "PERF-001", "load phase or budget missing")
        _whole_run(rep, text_all)
        return rep
    hours = (ph.t1 - ph.t0) / 3600.0
    rep.info(f"load phase {hours * 60:.0f} min")
    # Per entry: submitted counter at the first and last DIAG inside the phase, latency samples in between.
    entries: dict[int, list[tuple[float, int, int, int, int]]] = {}
    for t, raw in lines:
        m = DIAG_ENTRY_RE.search(ANSI_RE.sub("", raw))
        if m and ph.t0 <= t <= ph.t1:
            entries.setdefault(int(m.group(1)), []).append((t, int(m.group(2)), int(m.group(3)), int(m.group(4)),
                                                            int(m.group(7))))
    if not rep.check(len(entries) >= 2 and all(len(v) >= 2 for v in entries.values()), "PERF-001",
                     f"{len(entries)} poll entries with >= 2 DIAG samples"):
        _whole_run(rep, text_all)
        return rep
    measured = 0.0
    rows = []
    for e, smp in sorted(entries.items()):
        t_a, slot, grp, sub_a, _ = smp[0]
        t_b, _, _, sub_b, _ = smp[-1]
        lat = [x[4] for x in smp[1:] if x[4] > 0]
        if t_b <= t_a or not lat:
            continue
        rate = (sub_b - sub_a) / (t_b - t_a)
        mean_ms = sum(lat) / len(lat)
        measured += rate * mean_ms / 1000.0
        rows.append(f"entry {e} (slot {slot} grp {grp}): {rate * 60:.1f}/min x {mean_ms:.0f} ms")
    nominal = b.buses[0]["nominal_load"]
    ratio = measured / nominal if nominal else float("nan")
    rep.info("measured transaction time (submit -> result) per entry: " + "; ".join(rows))
    rep.check(0.5 <= ratio <= 1.5, "PERF-001",
              f"measured bus load {measured:.2%} vs budget nominal {nominal:.2%} (ratio {ratio:.2f}, limit 0.5..1.5; "
              "budget assumes 50 ms device latency)")
    _no_overlap(rep, sim_lines or [], "PERF-001")
    rx, tx = parse_sim_rx(sim_lines or []), parse_sim_tx(sim_lines or [])
    if ph.w0 is not None and rx:
        rx_in = [r for r in rx if ph.w0 <= r[0] <= (ph.w1 or 0)]
        tx_in = [x for x in tx if ph.w0 <= x[0] <= (ph.w1 or 0) + 1]
        rep.check(len(rx_in) > 0 and abs(len(rx_in) - len(tx_in)) <= 1, "PERF-001",
                  f"{len(rx_in)} requests, {len(tx_in)} replies in the load phase (every request answered)")
    for slot in (0, 1):
        ds = dumps_in(dumps, ph, 0, slot)
        bad = sum(1 for x in ds for v in x.vals.values() if v.q != "GOOD")
        rep.check(bool(ds) and not bad and all(x.mode == "ONLINE" for x in ds[3:]), "PERF-001",
                  f"slot {slot}: {len(ds)} dump(s), {bad} non-GOOD value(s)")
    _perf(rep, text_all)
    _whole_run(rep, text_all)
    return rep


def _answering(ph: Phase) -> tuple[float, float] | None:
    """Time window of a soak phase in which the simulator answers (None: never)."""
    if ph.mode.startswith("TIMEOUT"):
        return None
    if ph.mode.startswith("OFFLINE_THEN_RECOVER"):
        return (ph.t0 + OTR_SILENT_S, ph.t1)
    return (ph.t0, ph.t1)


def analyse_soak(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
                 sim_lines: list[str] | None = None, requested_min: int | None = None) -> Report:
    """D9 PERF-005 (+ PERF-002/003/004/006): every quality transition explained by a simulator mode change."""
    rep = Report()
    dumps = parse_console(lines)
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    _boot(rep, text_all, "sim", expected_crc)
    run_s = sum(p.t1 - p.t0 for p in phases)
    want_s = (requested_min or run_s / 60.0) * 60.0
    rep.check(run_s >= want_s - 5.0, "PERF-005", f"soak ran {run_s / 3600:.2f} h of {want_s / 3600:.2f} h requested, "
              f"{len(phases)} phase(s) of {SOAK_PHASE_S // 60} min")
    # 1. events only where the simulator changed behaviour
    off = [t for t, s in lines if "EVT_DEVICE_OFFLINE" in s]
    on = [t for t, s in lines if "EVT_DEVICE_ONLINE" in s]
    bad_off, bad_on = [], []
    for t in off:
        ph = next((p for p in phases if p.t0 <= t <= p.t1), None)
        if ph is None or _answering(ph) is not None:      # only a TIMEOUT phase takes the device OFFLINE
            bad_off.append(round(t - phases[0].t0))
    for t in on:
        ph = next((p for p in phases if p.t0 <= t <= p.t1), None)
        w = _answering(ph) if ph else None
        if w is None or not (w[0] <= t <= w[0] + 40.0):
            bad_on.append(round(t - phases[0].t0))
    n_timeout = sum(1 for p in phases if p.mode.startswith("TIMEOUT") and p.t1 - p.t0 >= 30)
    n_back = sum(1 for p in phases if p.mode.startswith("OFFLINE_THEN_RECOVER") and p.t1 - p.t0 >= OTR_SILENT_S + 40)
    rep.check(not bad_off and len(off) == n_timeout, "PERF-005",
              f"{len(off)} EVT_DEVICE_OFFLINE (expected {n_timeout}, one per TIMEOUT phase)"
              + (f"; unexplained at {bad_off[:5]} s" if bad_off else ""))
    rep.check(not bad_on and len(on) == n_back, "PERF-005",
              f"{len(on)} EVT_DEVICE_ONLINE (expected {n_back}, within 40 s after the simulator answers again)"
              + (f"; unexplained at {bad_on[:5]} s" if bad_on else ""))
    # 2. qualities match the simulator state in every dump
    wrong = []
    for ph in phases:
        w = _answering(ph)
        silent_end = ph.t1 if w is None else w[0]          # simulator silent from ph.t0 to here
        for d in dumps_in(dumps, ph, 0, 0):
            if w is not None and d.t >= w[0] + 45.0:       # answering, settled: everything GOOD
                bad = [k for k, v in d.vals.items() if v.q != "GOOD"]
                if bad or d.mode != "ONLINE":
                    wrong.append(f"{ph.name}+{d.t - ph.t0:.0f}s {d.mode} {bad[:2]}")
            elif ph.t0 + 20.0 <= d.t < silent_end:        # silent, settled: OFFLINE, nothing GOOD
                bad = [k for k, v in d.vals.items() if v.q == "GOOD"]
                if bad or d.mode != "OFFLINE":
                    wrong.append(f"{ph.name}+{d.t - ph.t0:.0f}s {d.mode} GOOD:{bad[:2]}")
    rep.check(not wrong, "PERF-005", f"qualities match the simulator mode in every dump checked"
              + (f"; {len(wrong)} wrong, first: {wrong[:3]}" if wrong else f" ({len(dumps)} dumps)"))
    rep.check(not any("EVT_BUS_FAULT" in s for s in text_all), "PERF-005", "no EVT_BUS_FAULT")
    _perf(rep, text_all)
    _whole_run(rep, text_all)
    return rep


# ========================================================= DP-WP5 meters ===
# The analysis works on parameter ROLES from the export-sim layout (DP-WP5
# Design 4.3), so it names no DC parameter: any meter described by a profile
# and a map can be checked the same way (Design open item 6).
@dataclass
class MeterInfo:
    slave: int
    slot: int
    normal: dict[str, float]          # NORMAL image (map test vectors)
    params: list[str]
    power: str | None                 # role power.active
    signed: list[str]                 # roles power.active, current.signed (and power.factor if signed)
    imp: str | None                   # role energy.import
    exp: str | None                   # role energy.export
    oor: dict[str, float]             # sim_out_of_range per parameter
    groups: dict[str, tuple[int, int, int]]   # name -> (function code, start, count)
    group_of: dict[str, str]          # parameter -> group name
    status_group: str | None          # group of the role 'status' parameter (probe group)
    interval_s: dict[str, float]


def meter_info(layout: dict, slave: int) -> MeterInfo:
    dev = next(d for d in layout["devices"] if d["slave_id"] == slave)
    sim = sim_mod.Simulator(layout, seed=1, clock=lambda: 0.0)
    regs = dev["registers"]
    roles: dict[str, list[str]] = {}
    for r in regs:
        roles.setdefault(r.get("role", ""), []).append(r["param"])
    first = lambda role: (roles.get(role) or [None])[0]                          # noqa: E731
    signed = [p for p in (first("power.active"),) if p] + roles.get("current.signed", [])
    if dev.get("pf_sign") == "SIGNED_BY_ACTIVE_POWER":
        signed += roles.get("power.factor", [])
    status = first("status")
    group_of = {r["param"]: r["group"] for r in regs}
    return MeterInfo(slave, dev["slot"], dict(sim.devices[slave].base), [r["param"] for r in regs],
                     first("power.active"), signed, first("energy.import"), first("energy.export"),
                     {r["param"]: r["sim_out_of_range"] for r in regs if r.get("sim_out_of_range") is not None},
                     {g["name"]: (g["function_code"], g["start"], g["count"]) for g in dev["poll_groups"]},
                     group_of, group_of.get(status) if status else None,
                     {g["name"]: g["interval_ms"] / 1000.0 for g in dev["poll_groups"]})


def parse_sim_rx_fc(sim_lines: list[str]) -> list[tuple[float, int, int, int]]:
    """Simulator log -> (wall time, slave, function code, start address) of every request."""
    out = []
    for ln in sim_lines:
        m = SIM_RX_RE.match(ln)
        if not m:
            continue
        b = bytes.fromhex(m.group(4))
        if len(b) < 6:
            continue
        try:
            w = dt.datetime.fromisoformat(m.group(1)).timestamp()
        except ValueError:
            continue
        out.append((w, int(m.group(2)), int(m.group(3), 16), (b[2] << 8) | b[3]))
    return out


def _meter_good(rep: Report, d: Dump | None, mi: MeterInfo, expect: dict[str, float], test: str, text: str) -> None:
    if d is None:
        return
    bad = [f"{k}={d.vals[k].value:g} {d.vals[k].q}" if k in d.vals else f"{k} missing"
           for k in mi.params if k not in d.vals or d.vals[k].q != "GOOD" or
           (k in expect and not _close(d.vals[k].value, expect[k]))]
    rep.check(d.mode == "ONLINE" and not bad, test, f"{text}: mode {d.mode}" + (f"; wrong: {', '.join(bad)}" if bad
                                                                                  else ", all GOOD"))


def _meter_offline(rep: Report, lines, dumps: list[Dump], ph: Phase | None, mi: MeterInfo, test: str,
                   max_s: float) -> None:
    if ph is None:
        rep.check(False, test, "phase missing")
        return
    offs = [(t, s) for t, s in lines if ph.t0 <= t <= ph.t1 and "EVT_DEVICE_OFFLINE" in ANSI_RE.sub("", s)]
    ok = len(offs) == 1 and f"slot={mi.slot}" in ANSI_RE.sub("", offs[0][1])
    if not rep.check(ok and offs[0][0] - ph.t0 <= max_s, test,
                     f"{len(offs)} EVT_DEVICE_OFFLINE for slot {mi.slot}"
                     + (f", {offs[0][0] - ph.t0:.1f} s after the simulator stopped answering (<= {max_s:.0f} s)"
                        if offs else "")):
        return
    on = [t for t in _evt_times(lines, "DEVICE_ONLINE", ph) if t > offs[0][0]]
    t_end = on[0] - 0.5 if on else ph.t1
    after = [x for x in dumps if x.slot == mi.slot and offs[0][0] + 0.5 < x.t <= t_end]
    if rep.check(bool(after), test, f"{len(after)} value dump(s) while OFFLINE"):
        x = after[-1]
        nl = [k for k, v in x.vals.items() if v.q != "COMM_LOST"]
        rep.check(x.mode == "OFFLINE" and not nl, test, f"mode {x.mode}, all parameters COMM_LOST"
                  + (f"; not: {nl}" if nl else ""))
    # never zero: no GOOD value at all after the loss was detected, and the kept values are the last GOOD ones
    zero = [(k, v.value) for x in after for k, v in x.vals.items() if v.q == "GOOD"]
    rep.check(not zero, test, "no parameter GOOD after the loss (failure is never published as a value)")
    if mi.power and after and mi.power in after[-1].vals:
        p = after[-1].vals[mi.power]
        rep.check(p.value != 0.0, test, f"{mi.power} keeps its last GOOD value {p.value:g} (not 0)")


def _meter_online(rep: Report, lines, dumps: list[Dump], ph: Phase | None, mi: MeterInfo, test: str,
                  min_s: float, max_s: float) -> float | None:
    if ph is None:
        rep.check(False, test, "phase missing")
        return None
    on = _evt_times(lines, "DEVICE_ONLINE", ph)
    if rep.check(len(on) == 1 and min_s <= on[0] - ph.t0 <= max_s, test,
                 f"{len(on)} EVT_DEVICE_ONLINE" + (f", {on[0] - ph.t0:.1f} s into the phase "
                                                    f"(expected {min_s:.0f}..{max_s:.0f} s)" if on else "")):
        d = [x for x in dumps if x.slot == mi.slot and on[0] < x.t <= ph.t1]
        if rep.check(bool(d), test, f"{len(d)} dump(s) after ONLINE"):
            bad = [k for k, v in d[-1].vals.items() if v.q != "GOOD"]
            rep.check(d[-1].mode == "ONLINE" and not bad, test, f"end of phase: mode {d[-1].mode}, "
                      + ("all GOOD again" if not bad else f"not GOOD: {bad}"))
        return on[0]
    return None


def analyse_dc(lines: list[tuple[float, str]], phases: list[Phase], expected_crc: str | None = None,
               sim_lines: list[str] | None = None, layout: dict | None = None, slave: int = DC_SLAVE) -> Report:
    """DP-WP5 Test Spec 9: DC-001..DC-012 for the DC meter alone (site lab_dc_bench)."""
    rep = Report()
    layout = layout or sim_mod.load_layout(suite_site("dc"))
    mi = meter_info(layout, slave)
    dumps = [d for d in parse_console(lines) if d.slot == mi.slot]
    text_all = [ANSI_RE.sub("", s) for _, s in lines]
    rx = parse_sim_rx_fc(sim_lines or [])
    meas_grp = mi.group_of.get(mi.power or "", "")

    # ---- DC-001 boot, configuration, function codes
    _boot(rep, text_all, "sim", expected_crc)
    rep.check(any(POLL_START_RE.search(s) for s in text_all), "DC-001", "polling started")
    rep.info(f"{len(dumps)} value dumps, {len(rx)} simulator requests captured; slave {slave} = slot {mi.slot}")
    if rx:
        slaves = sorted({r[1] for r in rx})
        rep.check(slaves == [slave], "DC-001", f"requests to slave(s) {slaves} (expected only {slave})")
        expected = {(fc, st) for fc, st, _ in mi.groups.values()}
        seen = {(r[2], r[3]) for r in rx if r[1] == slave}
        rep.check(seen == expected, "DC-001", "function code per group: "
                  + ", ".join(f"FC{fc:02d}@{st}" for fc, st in sorted(seen, key=lambda x: x[1]))
                  + ("" if seen == expected else
                     f" (expected {', '.join(f'FC{fc:02d}@{st}' for fc, st in sorted(expected, key=lambda x: x[1]))})"))
    else:
        rep.check(False, "DC-001", "no simulator requests in sim.log")

    # ---- DC-002 NORMAL values (map test vectors)
    d = _last(rep, dumps, _phase(phases, "normal"), 15, "DC-002")
    _meter_good(rep, d, mi, mi.normal, "DC-002", f"{len(mi.params)} parameters with the map test vectors")
    if d:
        rep.check(len(d.vals) == len(mi.params), "DC-002", f"{len(d.vals)} parameters published "
                  f"(map has {len(mi.params)})")

    # ---- DC-003 group intervals (simulator side, first attempts only)
    ans = [p for p in phases if p.name in ("normal", "import", "export", "out_of_range", "counter_reset")]
    if ans and ans[0].w0 is not None and rx:
        w0, w1 = ans[0].w0, ans[-1].w1 or 0.0
        for name, (fc, start, _) in sorted(mi.groups.items(), key=lambda g: g[1][1]):
            iv = mi.interval_s[name]
            starts = transactions([r[0] for r in rx if r[1] == slave and r[3] == start and w0 <= r[0] <= w1],
                                  gap=min(2.0, iv / 2))
            gaps = [b - a for a, b in zip(starts, starts[1:])]
            if not gaps:
                rep.check(iv > (w1 - w0) / 2, "DC-003", f"{name}: fewer than 2 polls in {w1 - w0:.0f} s "
                          f"(interval {iv:g} s)")
                continue
            mean = sum(gaps) / len(gaps)
            rep.check(abs(mean - iv) <= 0.1 and all(abs(g - iv) <= 0.5 for g in gaps), "DC-003",
                      f"{name} (FC{fc:02d} @{start}): mean interval {mean:.3f} s over {len(gaps)}, "
                      f"range {min(gaps):.2f}..{max(gaps):.2f} s (expected {iv:g} s, mean +- 0.1, each +- 0.5)")
    else:
        rep.info("DC-003 not checked: no simulator log or phase wall times")

    # ---- DC-004 IMPORT / DC-005 EXPORT
    for name, test, sign, counter, other in (("import", "DC-004", +1.0, mi.imp, mi.exp),
                                             ("export", "DC-005", -1.0, mi.exp, mi.imp)):
        ph = _phase(phases, name)
        ds = dumps_in(dumps, ph, 15, mi.slot) if ph else []
        if not rep.check(len(ds) >= 2, test, f"{len(ds)} dumps in phase '{name}' after 15 s"):
            continue
        expect = dict(mi.normal)
        for k in mi.signed:
            expect[k] = sign * abs(mi.normal[k])
        cols = {k: v for k, v in expect.items() if k not in (mi.imp, mi.exp)}
        _meter_good(rep, ds[-1], mi, cols, test, "signed values " + ", ".join(f"{k}={cols[k]:g}" for k in mi.signed))
        a, b = ds[0].vals.get(counter or ""), ds[-1].vals.get(counter or "")
        rep.check(bool(a and b and b.q == "GOOD" and b.value > a.value), test,
                  f"{counter} {a.value:g} -> {b.value:g} kWh (rising)" if a and b else f"no {counter}")
        a, b = ds[0].vals.get(other or ""), ds[-1].vals.get(other or "")
        rep.check(bool(a and b and _close(a.value, b.value)), test,
                  f"{other} unchanged ({b.value:g} kWh)" if a and b else f"no {other}")

    # ---- DC-006 OUT_OF_RANGE
    d = _last(rep, dumps, _phase(phases, "out_of_range"), 8, "DC-006")
    if d and rep.check(bool(mi.oor), "DC-006", f"out-of-range parameter(s) from the map: {sorted(mi.oor)}"):
        for k, v in mi.oor.items():
            x = d.vals.get(k)
            rep.check(x is not None and x.q == "OUT_OF_RANGE" and _close(x.value, v), "DC-006",
                      f"{k} {x.value:g} {x.q}" if x else f"no {k}")
        same = [k for k in mi.params if mi.group_of[k] in {mi.group_of[o] for o in mi.oor} and k not in mi.oor]
        bad = [k for k in same if k not in d.vals or d.vals[k].q != "GOOD"]
        rep.check(not bad, "DC-006", f"other parameters of the group stay GOOD ({', '.join(same)})"
                  + (f"; not GOOD: {bad}" if bad else ""))

    # ---- DC-007 COUNTER_RESET
    ph = _phase(phases, "counter_reset")
    if ph:
        evt = [s for s in lines_in(lines, ph) if "EVT_COUNTER_DISCONTINUITY" in s]
        rep.check(len(evt) == 1 and f"param={mi.imp}" in evt[0], "DC-007",
                  f"{len(evt)} EVT_COUNTER_DISCONTINUITY (expected exactly 1, for {mi.imp})")
        ds = [x for x in dumps_in(dumps, ph, 0, mi.slot) if mi.imp in x.vals]
        after = [x for x in ds if x.vals[mi.imp].value < mi.normal.get(mi.imp, 0.0)]
        if rep.check(len(after) >= 2, "DC-007", f"{len(after)} dump(s) after the reset"):
            a, b = after[0].vals[mi.imp], after[-1].vals[mi.imp]
            rep.check(a.q == b.q == "GOOD" and b.value > a.value, "DC-007",
                      f"{mi.imp} {a.value:g} -> {b.value:g} kWh, GOOD, counting from the new baseline")

    # ---- DC-012 SLOW:400 (answers inside the 500 ms timeout)
    ph = _phase(phases, "slow")
    if ph:
        warns = [s for s in lines_in(lines, ph) if TIMEOUT_WARN_RE.search(s) or "EVT_DEVICE_OFFLINE" in s]
        rep.check(not warns, "DC-012", "no timeout or OFFLINE while answers take 400 ms"
                  + (f": {warns[0][:80]}" if warns else ""))
        d = _last(rep, dumps, ph, 10, "DC-012")
        if d:
            bad = [k for k, v in d.vals.items() if v.q != "GOOD"]
            rep.check(d.mode == "ONLINE" and not bad, "DC-012", f"mode {d.mode}, all GOOD"
                      + (f"; not GOOD: {bad}" if bad else ""))
        if ph.w0 is not None and rx:
            req = [r for r in rx if r[1] == slave and ph.w0 + 1 <= r[0] <= (ph.w1 or 0)]
            retries = 0
            for _, start, _ in mi.groups.values():
                ts = [r[0] for r in req if r[3] == start]
                retries += len(ts) - len(transactions(ts, gap=2.0))   # a retry follows within ~1.6 s
            rep.check(bool(req) and retries == 0, "DC-012", f"{len(req)} request(s), {retries} retries")

    # ---- DC-010 EXCEPTION on the energy group
    ph = _phase(phases, "exception_energy")
    d = _last(rep, dumps, ph, 15, "DC-010")
    if d and ph:
        energy = [k for k in mi.params if mi.group_of[k] == mi.group_of.get(mi.imp or "")]
        e = {k: d.vals[k].q for k in energy if k in d.vals}
        rep.check(d.mode == "ONLINE" and e and all(q == "INVALID" for q in e.values()), "DC-010",
                  f"mode {d.mode}, energy group {e}")
        others = [k for k in mi.params if k not in energy and (k not in d.vals or d.vals[k].q != "GOOD")]
        rep.check(not others, "DC-010", "other groups stay GOOD" + (f"; not: {others}" if others else ""))
        evt = [s for s in lines_in(lines, ph) if "EVT_MODBUS_EXCEPTION" in s and "code=0x02" in s]
        rep.check(bool(evt), "DC-010", f"{len(evt)} EVT_MODBUS_EXCEPTION code=0x02 line(s)")
        rep.check(not _evt_times(lines, "DEVICE_OFFLINE", ph), "DC-010", "no EVT_DEVICE_OFFLINE")

    # ---- DC-008 TIMEOUT; recovery
    meas_iv = mi.interval_s.get(meas_grp, 5.0)
    _meter_offline(rep, lines, dumps, _phase(phases, "timeout"), mi, "DC-008", 2 * meas_iv + 3.2 + 3.0)
    _meter_online(rep, lines, dumps, _phase(phases, "recover"), mi, "DC-008", 0.0, 33.0)

    # ---- DC-009 OFFLINE_THEN_RECOVER:65, probing on the status group
    ph = _phase(phases, "offline_recover")
    _meter_offline(rep, lines, dumps, ph, mi, "DC-009", 2 * meas_iv + 3.2 + 3.0)
    on = _meter_online(rep, lines, dumps, ph, mi, "DC-009", 64.0, 100.0)
    off = _evt_times(lines, "DEVICE_OFFLINE", ph)
    if ph and off and ph.w0 is not None and rx and mi.status_group:
        w_off = _to_wall(ph, off[0])
        w_on = _to_wall(ph, on) if on is not None else ph.w1
        st = mi.groups[mi.status_group][1]
        between = [r for r in rx if r[1] == slave and w_off + 0.2 < r[0] < (w_on or 0) - 0.2]
        others = [r for r in between if r[3] != st]
        rep.check(not others, "DC-009", f"only the {mi.status_group} group (@{st}) polled while OFFLINE "
                  f"({len(between)} request(s), {len(others)} other)")
        probes = transactions([r[0] for r in rx if r[1] == slave and r[3] == st
                               and w_off + 0.2 < r[0] <= (w_on or 0) + 0.5])
        gaps = [b - a for a, b in zip(probes, probes[1:])]
        rep.check(len(probes) >= 2 and 29.0 <= probes[0] - w_off <= 32.0 and all(abs(g - 30.0) <= 1.0 for g in gaps),
                  "DC-009", f"{len(probes)} probe(s): first {probes[0] - w_off if probes else float('nan'):.1f} s "
                  f"after OFFLINE, then {', '.join(f'{g:.1f}' for g in gaps) or '-'} s (30 +- 1 s)")
        if on is not None:
            after = {r[3] for r in rx if r[1] == slave and (w_on or 0) - 0.5 <= r[0] <= (w_on or 0) + 3.0
                     and r[3] != st}
            rep.check(len(after) >= len(mi.groups) - 1, "DC-009",
                      f"after ONLINE all groups polled at once (start addresses {sorted(after)} within 3 s)")

    # ---- DC-011 BAD_CRC; recovery
    _meter_offline(rep, lines, dumps, _phase(phases, "bad_crc"), mi, "DC-011", 2 * meas_iv + 3.2 + 3.0)
    _meter_online(rep, lines, dumps, _phase(phases, "recover_crc"), mi, "DC-011", 0.0, 33.0)

    d = _last(rep, dumps, _phase(phases, "final"), 0, "DC-002")
    _meter_good(rep, d, mi, {k: v for k, v in mi.normal.items() if k not in (mi.imp, mi.exp)}, "DC-002",
                "end of run")
    _no_overlap(rep, sim_lines or [], "SCH-003")
    _whole_run(rep, text_all)
    return rep


ANALYSERS = {"d6": analyse_d6, "d7": analyse_d7, "esc": analyse_esc, "reject": analyse_reject,
             "two_dev": analyse_two_dev, "ratio": analyse_ratio, "load": analyse_load, "soak": analyse_soak,
             "dc": analyse_dc}


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


def control_text(mode: str) -> str:
    """Simulator control file for a phase. Items separated by '|':
    'BUS:<BABBLE|QUIET>' bus-level fault (D8); 'N=MODE' mode for slave N only
    (other slaves NORMAL); 'set ...' a meter-side value; otherwise the mode
    for every slave."""
    lines = ["bus QUIET", "mode * NORMAL"]
    for item in (x.strip() for x in mode.split("|")):
        if not item:
            continue
        if item.startswith("BUS:"):
            lines.append(f"bus {item[4:]}")
        elif item.startswith("set "):
            lines.append(item)
        elif re.fullmatch(r"\d+=\S+", item):
            slave, _, m = item.partition("=")
            lines.append(f"mode {slave} {m}")
        else:
            lines.append(f"mode * {item}")
    return "\n".join(lines) + "\n"


def write_control(path: Path, mode: str) -> None:
    path.write_text(control_text(mode), encoding="utf-8")


def run_suite(args) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial missing: python -m pip install -r tools/requirements-usb-rs485.txt", file=sys.stderr)
        return 2
    if args.site is None:
        args.site = suite_site(args.suite)
    layout = sim_mod.load_layout(args.site)
    suite = suite_phases(args.suite, args.duration_min)
    print(f"Suite {args.suite}: firmware -Mode {SUITE_BUILD.get(args.suite, 'Wp4Sim')}, site {args.site.name} "
          f"(config CRC {layout['config_crc32']})")
    out = args.out_dir or (PROJECT_ROOT / "tools" / "test_results" /
                           f"wp4_{args.suite}_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}")
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
            raise RuntimeError("no BUILD_MODE line on the console: is the firmware flashed and the port right?")
        time.sleep(3)

        total = sum(d for _, _, d in suite)
        print(f"Suite {args.suite}: {len(suite)} phase(s), about {total // 3600} h {total % 3600 // 60} min "
              f"{total % 60} s")
        interrupted = False
        for name, mode, dur in suite:
            write_control(ctl, mode)
            t0, w0 = time.monotonic(), time.time()
            print(f"  {dt.datetime.now():%H:%M:%S}  phase {name:<17} simulator {mode} for {dur} s")
            try:
                time.sleep(dur)
            except KeyboardInterrupt:
                interrupted = True    # keep what was captured; the report shows the shortfall
            phases.append(Phase(name, mode, t0, time.monotonic(), w0, time.time()))
            if interrupted:
                print("  interrupted: stopping the run and analysing what was captured")
                break
        # wait for the next STATS line (every 60 s) if none has appeared yet
        deadline = time.monotonic() + 65
        while (args.suite != "reject" and not interrupted and time.monotonic() < deadline
               and not con.seen(STATS_RE)):
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

    requested = args.duration_min or DEFAULT_DURATION_MIN.get(args.suite)
    (out / "phases.json").write_text(json.dumps({"suite": args.suite, "crc": layout["config_crc32"],
                                                 "site": str(args.site), "duration_min": requested,
                                                 "phases": [asdict(p) for p in phases]}, indent=2),
                                     encoding="utf-8")
    sim_lines = sim_log.read_text(encoding="utf-8", errors="replace").splitlines() if sim_log.exists() else []
    return report(out, args.suite, con.lines if con else [], phases, layout["config_crc32"], sim_lines,
                  {"duration_min": requested, "site": str(args.site)})


def report(out: Path, suite: str, lines, phases, crc, sim_lines=None, meta: dict | None = None) -> int:
    meta = meta or {}
    if suite == "soak":
        rep = analyse_soak(lines, phases, crc, sim_lines, requested_min=meta.get("duration_min"))
    elif suite == "load":
        rep = analyse_load(lines, phases, crc, sim_lines, site=Path(meta["site"]) if meta.get("site") else None)
    else:
        rep = ANALYSERS[suite](lines, phases, crc, sim_lines)
    result = "PASS" if rep.failures == 0 else f"FAIL ({rep.failures})"
    if rep.blocked:
        result += f", {rep.blocked} BLOCKED"
    text = "\n".join([f"DP-WP4 HIL report, suite {suite} ({out.name})"] + rep.rows + [f"RESULT: {result}"])
    (out / "report.txt").write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if rep.failures == 0 else 1


def load_saved(out: Path):
    meta = json.loads((out / "phases.json").read_text(encoding="utf-8"))
    phases = [Phase(**p) for p in meta["phases"]]
    sim_path = out / "sim.log"
    sim_lines = sim_path.read_text(encoding="utf-8", errors="replace").splitlines() if sim_path.exists() else []
    lines = []
    for raw in (out / "console.log").read_text(encoding="utf-8", errors="replace").splitlines():
        t, _, text = raw.partition(" ")
        try:
            lines.append((float(t), text))
        except ValueError:
            continue
    return meta.get("suite", "d6"), lines, phases, meta.get("crc"), sim_lines, meta


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="MyEMS DP-WP4/WP5 HIL runner")
    ap.add_argument("--suite", choices=sorted(SUITES), default="d7")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--site", type=Path, default=None,
                    help="site file for the simulator (default: the suite's site, e.g. lab_bench_2dev for two_dev)")
    ap.add_argument("--duration-min", type=int, default=None,
                    help="length of the load (default 60) and soak (default 1440) suites in minutes")
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--no-reset", action="store_true", help="do not reset the ESP32 via RTS (press EN instead)")
    ap.add_argument("--analyse", type=Path, metavar="RESULT_DIR", help="re-analyse a saved run, no hardware")
    a = ap.parse_args(argv)
    if a.analyse:
        suite, lines, phases, crc, sim_lines, meta = load_saved(a.analyse)
        return report(a.analyse, suite, lines, phases, crc, sim_lines, meta)
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
