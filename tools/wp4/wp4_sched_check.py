#!/usr/bin/env python3
"""Check the ESP32 poll schedule from the AC meter simulator log (DP-WP4 D5, HIL).

The simulator logs every request it receives with its own timestamp, so the
schedule can be checked from the bus side, independently of the firmware log.

Checks (Test Spec Section 14):
  SCH-001  every poll group of every enabled device is polled at its interval,
           each gap within +-TOL ms, and the groups keep their configured
           offsets relative to each other
  SCH-002  no burst: no gap shorter than half the interval
  SCH-003  the simulator never saw a request for a slave while a reply for
           that slave was still pending (OVERLAP lines; meaningful with SLOW)
  extra    every request matches a configured poll group (nothing else polled)

Usage (project root, after a run of at least 70 s):
  python tools/wp4/wp4_sched_check.py --site config/site/lab_bench.site.json --sim-log tools/test_results/sim.log
Only the last simulator run in the log file (after the last START line) is checked.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import myems_ac_meter_sim as sim_mod  # noqa: E402

RX_RE = re.compile(r"^\S+\s+([0-9.]+) RX slave=(\d+) fc=([0-9A-F]{2}) ((?:[0-9A-F]{2} )+)mode=(\S+)")
START_RE = re.compile(r"\sSTART myems_ac_meter_sim ")
OVERLAP_RE = re.compile(r"\sOVERLAP slave=(\d+)")


@dataclass
class Group:
    slave: int
    name: str
    fc: int
    start: int
    count: int
    interval_ms: float
    offset_ms: float
    times: list[float] = field(default_factory=list)


@dataclass
class Report:
    lines: list[str] = field(default_factory=list)
    failures: int = 0

    def check(self, ok: bool, test: str, text: str) -> None:
        self.lines.append(f"{'PASS' if ok else 'FAIL'}  {test:<8} {text}")
        if not ok:
            self.failures += 1

    def info(self, text: str) -> None:
        self.lines.append(f"      {text}")


def last_run(lines: list[str]) -> list[str]:
    idx = [i for i, ln in enumerate(lines) if START_RE.search(ln)]
    return lines[idx[-1]:] if idx else lines


def analyse(layout: dict, log_lines: list[str], tol_ms: float = 100.0, min_polls: int = 3) -> Report:
    rep = Report()
    run = last_run(log_lines)
    groups: dict[tuple[int, int, int, int], Group] = {}
    for d in layout["devices"]:
        if not d["enabled"]:
            continue
        for g in d["poll_groups"]:
            groups[(d["slave_id"], g["function_code"], g["start"], g["count"])] = Group(
                d["slave_id"], g["name"], g["function_code"], g["start"], g["count"],
                float(g["interval_ms"]), float(g["offset_ms"]))
    unknown = 0
    rx = 0
    for ln in run:
        m = RX_RE.match(ln)
        if not m:
            continue
        rx += 1
        t = float(m.group(1))
        b = bytes.fromhex(m.group(4))
        if len(b) < 6:
            unknown += 1
            continue
        key = (int(m.group(2)), b[1], (b[2] << 8) | b[3], (b[4] << 8) | b[5])
        if key in groups:
            groups[key].times.append(t)
        else:
            unknown += 1
            rep.info(f"unexpected request at {t:.1f} ms: {m.group(4).strip()}")

    overlaps = sum(1 for ln in run if OVERLAP_RE.search(ln))
    duration = 0.0
    times_all = [t for g in groups.values() for t in g.times]
    if times_all:
        duration = (max(times_all) - min(times_all)) / 1000.0
    rep.info(f"{rx} requests over {duration:.1f} s in the last simulator run")

    for g in groups.values():
        label = f"slave {g.slave} {g.name:<10} FC{g.fc:02d} {g.start}+{g.count}"
        n = len(g.times)
        if n < min_polls:
            rep.check(False, "SCH-001", f"{label}: only {n} polls (need >= {min_polls}; run longer)")
            continue
        gaps = [b - a for a, b in zip(g.times, g.times[1:])]
        worst = max(abs(x - g.interval_ms) for x in gaps)
        rep.check(worst <= tol_ms, "SCH-001",
                  f"{label}: {n} polls, interval {g.interval_ms:.0f} ms, gaps {min(gaps):.0f}..{max(gaps):.0f} ms "
                  f"(worst deviation {worst:.0f} ms, limit {tol_ms:.0f})")
        rep.check(min(gaps) >= 0.5 * g.interval_ms, "SCH-002",
                  f"{label}: shortest gap {min(gaps):.0f} ms (no burst below {0.5 * g.interval_ms:.0f} ms)")

    # relative phase between groups of one device (configured offsets)
    for d in layout["devices"]:
        if not d["enabled"]:
            continue
        gs = [groups[(d["slave_id"], g["function_code"], g["start"], g["count"])] for g in d["poll_groups"]]
        offs = [g.get("offset_ms") for g in d["poll_groups"]]
        if any(o is None for o in offs) or not gs[0].times:
            continue
        base_t, base_off, base_iv = gs[0].times[0], offs[0], gs[0].interval_ms
        for g, off in zip(gs[1:], offs[1:]):
            if not g.times:
                continue
            # phase of g relative to the first group's grid, modulo the first group's interval
            phase = (g.times[0] - base_t - (off - base_off)) % base_iv
            dev = min(phase, base_iv - phase)
            rep.check(dev <= tol_ms, "SCH-001",
                      f"slave {g.slave} {g.name} offset {off - base_off:.0f} ms after {gs[0].name}: "
                      f"deviation {dev:.0f} ms")

    rep.check(overlaps == 0, "SCH-003", f"{overlaps} request(s) while a reply for the same slave was pending")
    rep.check(unknown == 0, "config", f"{unknown} request(s) outside the configured poll groups")
    return rep


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--site", type=Path, required=True)
    ap.add_argument("--sim-log", type=Path, required=True)
    ap.add_argument("--tolerance-ms", type=float, default=100.0)
    ap.add_argument("--min-polls", type=int, default=3)
    a = ap.parse_args(argv)
    layout = sim_mod.load_layout(a.site)
    lines = a.sim_log.read_text(encoding="utf-8", errors="replace").splitlines()
    rep = analyse(layout, lines, a.tolerance_ms, a.min_polls)
    print(f"Schedule check: site {layout['site_id']}, log {a.sim_log}")
    print("\n".join(rep.lines))
    print("RESULT: " + ("PASS" if rep.failures == 0 else f"FAIL ({rep.failures})"))
    return 0 if rep.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
