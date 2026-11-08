"""Tests for tools/wp4/wp4_sched_check.py (DP-WP4 D5).

The logs are produced by the real simulator (fake clock), so the parser is
tested against the exact log format the HIL run produces.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import myems_ac_meter_sim as sim_mod  # noqa: E402
import wp4_sched_check as chk  # noqa: E402
from myems_ac_meter_sim import Simulator, parse_mode, with_crc  # noqa: E402

SITE = sim_mod.PROJECT_ROOT / "config" / "site"
LAB = sim_mod.load_layout(SITE / "lab_bench.site.json")
LAB2 = sim_mod.load_layout(SITE / "lab_bench_2dev.site.json")
GROUPS = [(0, 14, 5000, 500), (14, 4, 10000, 1500), (18, 1, 30000, 2500)]   # start, count, interval, offset


def req(slave: int, start: int, count: int) -> bytes:
    return with_crc(bytes((slave, 3, start >> 8, start & 0xFF, count >> 8, count & 0xFF)))


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


def make_log(events, layout=LAB, mode=None):
    """events: list of (time_ms, slave, start, count). Returns simulator log lines."""
    clk = Clock()
    lines = ["2026-01-01T00:00:00.000+00:00        0.0 START myems_ac_meter_sim 1.0.0 site=X"]
    s = Simulator(layout, log=lambda t: lines.append("2026-01-01T00:00:00.000+00:00 " + t), clock=clk)
    if mode:
        s.set_mode("*", parse_mode(mode))
    for t_ms, slave, start, count in sorted(events):
        clk.t = t_ms / 1000.0
        s.handle(req(slave, start, count))
    return lines


def schedule(duration_ms=95000, slaves=(1,), jitter=None, base=10000.0):
    ev = []
    for slave in slaves:
        for start, count, iv, off in GROUPS:
            t, k = base + off, 0
            while t <= base + duration_ms:
                j = jitter(k) if jitter else 0.0
                ev.append((t + j, slave, start, count))
                t += iv
                k += 1
    return ev


def run(layout, lines):
    return chk.analyse(layout, lines)


class SchedCheckTests(unittest.TestCase):

    def assertPass(self, rep):
        self.assertEqual(0, rep.failures, "\n".join(rep.lines))

    def assertFailIn(self, rep, test_id):
        self.assertGreater(rep.failures, 0, "\n".join(rep.lines))
        self.assertTrue(any(ln.startswith(f"FAIL  {test_id}") for ln in rep.lines), "\n".join(rep.lines))

    def test_perfect_schedule_passes(self):
        self.assertPass(run(LAB, make_log(schedule())))

    def test_two_devices(self):
        self.assertPass(run(LAB2, make_log(schedule(slaves=(1, 2)), layout=LAB2)))

    def test_small_jitter_passes(self):
        self.assertPass(run(LAB, make_log(schedule(jitter=lambda k: 40.0 if k % 2 else -40.0))))

    def test_large_jitter_fails_sch001(self):
        self.assertFailIn(run(LAB, make_log(schedule(jitter=lambda k: 150.0 if k == 3 else 0.0))), "SCH-001")

    def test_burst_fails_sch002(self):
        ev = schedule() + [(10500 + 5000 * 4 + 300, 1, 0, 14)]
        self.assertFailIn(run(LAB, make_log(ev)), "SCH-002")

    def test_wrong_offset_fails(self):
        ev = [e if e[2] != 14 else (e[0] + 300, *e[1:]) for e in schedule()]
        rep = run(LAB, make_log(ev))
        self.assertFailIn(rep, "SCH-001")
        self.assertTrue(any("energy offset 1000 ms" in ln and ln.startswith("FAIL") for ln in rep.lines))

    def test_offset_check_independent_of_when_the_log_starts(self):
        ev = [e for e in schedule(duration_ms=130000) if e[0] > 23000]   # simulator started late
        self.assertPass(run(LAB, make_log(ev)))

    def test_unexpected_request_fails(self):
        ev = schedule() + [(12000, 1, 0, 2)]               # 09_057 demo poll
        self.assertFailIn(run(LAB, make_log(ev)), "config")

    def test_overlap_fails_sch003(self):
        ev = schedule() + [(10500 + 5000 * 2 + 100, 1, 14, 4)]
        rep = run(LAB, make_log(ev, mode="SLOW:400"))
        self.assertFailIn(rep, "SCH-003")

    def test_late_first_polls_when_esp32_started_first(self):
        """Real D5 HIL run: the ESP32 polled before the simulator opened COM12, so the
        first energy and electrical polls were late; afterwards back on the grid."""
        ev = [e for e in schedule(base=5828.0 - 500.0) if e[0] > 5000.0]
        ev += [(1156.0, 1, 14, 4), (1219.0, 1, 0, 14)]
        rep = run(LAB, make_log(ev))
        self.assertPass(rep)
        self.assertTrue(any("first poll at 1156 ms not judged" in ln for ln in rep.lines))

    def test_late_second_poll_still_fails(self):
        ev = schedule()
        ev = [e for e in ev if not (e[2] == 0 and e[0] == 10000 + 500 + 5000)]
        ev.append((10000 + 500 + 5000 + 400, 1, 0, 14))
        self.assertFailIn(run(LAB, make_log(ev)), "SCH-001")

    def test_too_short_run_fails(self):
        self.assertFailIn(run(LAB, make_log(schedule(duration_ms=20000))), "SCH-001")

    def test_only_last_run_is_checked(self):
        bad = make_log(schedule(jitter=lambda k: 900.0 if k == 2 else 0.0))
        good = make_log(schedule())
        self.assertPass(run(LAB, bad + good))

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "sim.log"
            log.write_text("\n".join(make_log(schedule())) + "\n", encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = chk.main(["--site", str(SITE / "lab_bench.site.json"), "--sim-log", str(log)])
            self.assertEqual(0, rc, out.getvalue())
            self.assertIn("RESULT: PASS", out.getvalue())
            log.write_text("\n".join(make_log(schedule(duration_ms=20000))) + "\n", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(1, chk.main(["--site", str(SITE / "lab_bench.site.json"), "--sim-log", str(log)]))


if __name__ == "__main__":
    unittest.main()
