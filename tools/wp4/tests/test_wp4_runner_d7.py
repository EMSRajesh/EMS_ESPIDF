"""Tests for the d7 and reject analyses of tools/wp4/myems_wp4_runner.py (DP-WP4 D7).

A scripted firmware (console lines) and a scripted simulator log (RX lines)
follow the designed D7 timeline; each deliberate deviation must fail the
check that covers it.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import datetime as dt
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import myems_wp4_runner as run  # noqa: E402
from myems_ac_meter_sim import with_crc  # noqa: E402
from test_wp4_runner import ALL, ELEC, ENER, NORMAL, UNITS, esp  # noqa: E402

WALL0 = 1_794_000_000.0          # wall clock = monotonic + WALL0
CRC = "0x4B8B5924"


def iso(w: float) -> str:
    return dt.datetime.fromtimestamp(w, tz=dt.timezone.utc).isoformat(timespec="milliseconds")


class Script:
    def __init__(self):
        self.lines: list[tuple[float, str]] = []
        self.sim: list[str] = []
        self.phases: list[run.Phase] = []
        self.t = 100.0
        self.vals = {k: [v, "GOOD", 0.0] for k, v in NORMAL.items()}
        self.mode = "ONLINE"
        self.seq = 0

    # console
    def emit(self, t, msg, level="I", tag="ems_acquisition"):
        self.lines.append((t, esp(int(t * 1000), msg, level, tag)))

    def boot(self, target="sim"):
        self.emit(self.t, f"BUILD_MODE=PRODUCTION CONFIG_TARGET={target}", tag="myems_app")
        self.emit(self.t, "CONFIG site=LAB_BENCH_01 target=sim classification=SIMULATION_ONLY crc=0x4B8B5924 "
                          "devices=1", tag="myems_app")
        if target == "sim":
            self.emit(self.t, "EVT_CONFIG_ACTIVATED site=LAB_BENCH_01 crc=0x4B8B5924", tag="myems_app")
            self.emit(self.t, "MyEMS DP-WP4 polling started: 1 devices, 3 poll entries", tag="myems_app")
        else:
            self.emit(self.t, "EVT_CONFIG_REJECTED rule=V-13: SIMULATION_ONLY configuration (target sim) "
                              "in a CONFIG_TARGET=prod build", "E", "myems_app")
            self.emit(self.t, "No polling started (device stays READY). Fix the configuration or build mode.",
                      "E", "myems_app")
        self.t += 3

    def dump(self, t):
        self.emit(t, f"VALUES slot=0 GRID_INCOMER_01 mode={self.mode} count=14")
        for k, (v, q, ts) in self.vals.items():
            self.seq += 1
            self.emit(t, f"VAL slot=0 {k}={v:.9g} {UNITS[k]} q={q} age_ms={int((t - ts) * 1000)} seq={self.seq}")

    def set(self, keys, q, t=None):
        for k in keys:
            self.vals[k][1] = q
            if q == "GOOD" and t is not None:
                self.vals[k][2] = t

    # simulator
    def rx(self, t, start, count):
        req = with_crc(bytes((1, 3, start >> 8, start & 0xFF, 0, count)))
        self.sim.append(f"{iso(t + WALL0)}   {t * 1000:10.1f} RX slave=1 fc=03 {req.hex(' ').upper()} mode=X")

    def phase(self, name, mode, dur, events):
        """events: list of (offset s, callable(script, abs_time))."""
        t0 = self.t
        timeline = sorted(events + [(k * 5.0, lambda s, t: s.dump(t)) for k in range(1, int(dur // 5) + 1)],
                          key=lambda e: e[0])
        for off, fn in timeline:
            fn(self, t0 + off)
        self.t = t0 + dur
        self.phases.append(run.Phase(name, mode, t0, self.t, t0 + WALL0, self.t + WALL0))


def warn(s, t, status="TIMEOUT", n=1):
    s.emit(t, f"GRID_INCOMER_01 grp0 {status} (consecutive failures {n}): 11 parameter(s) GOOD -> STALE", "W")


def offline(s, t):
    s.mode = "OFFLINE"
    s.set(ALL, "COMM_LOST")
    s.emit(t, "EVT_DEVICE_OFFLINE slot=0 GRID_INCOMER_01: 2 consecutive failed transactions; all parameters "
              "COMM_LOST; probing group 2 every 30000 ms", "W")


def online(s, t):
    s.mode = "ONLINE"
    s.set(["dev.status.code"], "GOOD", t)
    s.emit(t, "EVT_DEVICE_ONLINE slot=0 GRID_INCOMER_01: device answered; all groups scheduled")


def all_good(s, t):
    s.set(ALL, "GOOD", t)


def polls(s, t0, t1, every=5.0):
    out = []
    t = t0
    while t < t1:
        out.append((t, lambda sc, tt: (sc.rx(tt, 0, 14), sc.set(ELEC, "GOOD", tt) if sc.mode == "ONLINE" else None)))
        out.append((t + 1.0, lambda sc, tt: sc.rx(tt, 14, 4)))
        t += every
    return out


def good_d7(o=None):
    o = o or {}
    s = Script()
    s.boot()
    s.phase("normal", "NORMAL", 25, polls(s, 0.5, 25) + [(1, all_good)])
    s.phase("offline_recover", "OFFLINE_THEN_RECOVER:65", 100, o.get("offline_recover", [
        (0.5, lambda sc, t: sc.rx(t, 0, 14)), (2.1, lambda sc, t: sc.rx(t, 0, 14)), (3.6, warn),
        (4.0, lambda sc, t: sc.rx(t, 14, 4)), (5.6, lambda sc, t: sc.rx(t, 14, 4)), (7.1, offline),
        (37.1, lambda sc, t: sc.rx(t, 18, 1)), (38.7, lambda sc, t: sc.rx(t, 18, 1)),
        (40.2, lambda sc, t: warn(sc, t, n=3)),
        (67.1, lambda sc, t: sc.rx(t, 18, 1)), (67.2, online),
        (67.3, lambda sc, t: sc.rx(t, 0, 14)), (67.4, lambda sc, t: (sc.rx(t, 14, 4), all_good(sc, t))),
    ] + polls(s, 72.3, 100)))
    s.phase("exception_energy", "EXCEPTION:02@energy", 25, o.get("exception_energy", [
        (3, lambda sc, t: sc.emit(t, "EVT_MODBUS_EXCEPTION slot=0 GRID_INCOMER_01 grp1 code=0x02: group INVALID", "W")),
        (3.1, lambda sc, t: sc.set(ENER, "INVALID")),
    ]))
    s.phase("bad_crc", "BAD_CRC", 30, o.get("bad_crc", [
        (2, lambda sc, t: warn(sc, t, "CRC_ERROR")), (6.5, offline)]))
    s.phase("recover", "NORMAL", 45, o.get("recover", [
        (21.0, online), (21.3, all_good)]))
    s.phase("final", "NORMAL", 15, [])
    t = s.t
    s.emit(t, "STATS sched: submitted=80 skipped=0 rejected=0 guard_expired=0 fb_unmatched=0 fb_late=0 health=0x00 "
              "| acq: results=80 ok=60 failed=20 routing_errors=0 stale=11", tag="myems_app")
    s.emit(t, o.get("diag", "DIAG slot=0 GRID_INCOMER_01 mode=ONLINE ok=60 timeouts=6 crc=4 protocol=0 transport=0 "
                            "exceptions=5 last_exc=0x02 mismatches=0 consec_fail=0 offline=2 online=2 "
                            "last_ok_age_ms=812 counter_disc=0"), tag="myems_app")
    s.emit(t, o.get("bus", "DIAG bus accepted=80 completed=80 timeouts=6 crc=4 protocol=0 transport=0 "
                           "completion_drops=0 backpressure=0 unexpected_slave=0 | sched_queue_hwm=2 "
                           "sched_queue_full=0 | events=8 event_drops=0"), tag="myems_app")
    s.emit(t, "STACK task=ems_acquisition allocated=6144B current_free=4700B min_ever_free=4600B")
    return s


def analyse(s):
    return run.analyse_d7(s.lines, s.phases, CRC, s.sim)


class D7AnalysisTests(unittest.TestCase):

    def assertPass(self, rep):
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))

    def assertFails(self, rep, test_id):
        self.assertTrue([r for r in rep.rows if r.startswith("FAIL") and f" {test_id} " in r + " "],
                        f"expected a FAIL for {test_id}:\n" + "\n".join(rep.rows))

    def test_designed_behaviour_passes(self):
        self.assertPass(analyse(good_d7()))

    def _offline_events(self, **changes):
        base = dict(offline_at=7.1, probe1=37.1, probe2=67.1, online_at=67.2, extra=[])
        base.update(changes)
        ev = [(0.5, lambda sc, t: sc.rx(t, 0, 14)), (3.6, warn), (base["offline_at"], offline),
              (base["probe1"], lambda sc, t: sc.rx(t, 18, 1)), (base["probe1"] + 3.1, lambda sc, t: warn(sc, t, n=3)),
              (base["probe2"], lambda sc, t: sc.rx(t, 18, 1)), (base["online_at"], online),
              (base["online_at"] + 0.1, lambda sc, t: sc.rx(t, 0, 14)),
              (base["online_at"] + 0.2, lambda sc, t: (sc.rx(t, 14, 4), all_good(sc, t)))] + base["extra"]
        return {"offline_recover": ev}

    def test_offline_too_late_fails_dev001(self):
        self.assertFails(analyse(good_d7(self._offline_events(offline_at=20.0, probe1=50.0, probe2=80.0,
                                                               online_at=80.1))), "DEV-001")

    def test_two_offline_events_fail_dev001(self):
        self.assertFails(analyse(good_d7(self._offline_events(extra=[(30.0, offline)]))), "DEV-001")

    def test_values_not_comm_lost_fail_dev001(self):
        ev = self._offline_events(extra=[(7.2, lambda sc, t: sc.set(ELEC, "STALE"))])
        self.assertFails(analyse(good_d7(ev)), "DEV-001")

    def test_other_group_polled_while_offline_fails_sch006(self):
        ev = self._offline_events(extra=[(20.0, lambda sc, t: sc.rx(t, 0, 14))])
        self.assertFails(analyse(good_d7(ev)), "SCH-006")

    def test_probe_interval_wrong_fails_sch006(self):
        self.assertFails(analyse(good_d7(self._offline_events(probe1=22.1))), "SCH-006")

    def test_online_too_early_fails_dev002(self):
        self.assertFails(analyse(good_d7(self._offline_events(probe2=40.0, online_at=40.1))), "DEV-002")

    def test_groups_not_polled_after_online_fail_sch007(self):
        ev = self._offline_events()
        ev["offline_recover"] = [e for e in ev["offline_recover"] if abs(e[0] - 67.3) > 0.05 and abs(e[0] - 67.4) > 0.05]
        ev["offline_recover"].append((67.4, all_good))
        self.assertFails(analyse(good_d7(ev)), "SCH-007")

    def test_warnings_too_frequent_fail_diag004(self):
        ev = self._offline_events(extra=[(5.0, warn), (6.0, warn)])
        self.assertFails(analyse(good_d7(ev)), "DIAG-004")

    def test_exception_taking_device_offline_fails_dev004(self):
        o = {"exception_energy": [(3, offline)]}
        self.assertFails(analyse(good_d7(o)), "DEV-004")

    def test_bad_crc_not_offline_fails_dev006(self):
        self.assertFails(analyse(good_d7({"bad_crc": [(2, lambda sc, t: warn(sc, t, "CRC_ERROR"))]})), "DEV-006")

    def test_no_recovery_after_bad_crc_fails_dev002(self):
        self.assertFails(analyse(good_d7({"recover": []})), "DEV-002")

    def test_missing_diag_lines_fail_diag005(self):
        s = good_d7()
        s.lines = [(t, x) for t, x in s.lines if "DIAG " not in x]
        self.assertFails(analyse(s), "DIAG-005")

    def test_event_drops_fail(self):
        s = good_d7({"bus": "DIAG bus accepted=1 completed=1 timeouts=0 crc=0 protocol=0 transport=0 "
                            "completion_drops=0 backpressure=0 unexpected_slave=0 | sched_queue_hwm=1 "
                            "sched_queue_full=0 | events=70 event_drops=6"})
        self.assertFails(analyse(s), "DIAG-003")

    def test_transition_counters_checked(self):
        s = good_d7({"diag": "DIAG slot=0 GRID_INCOMER_01 mode=ONLINE ok=60 timeouts=6 crc=4 protocol=0 "
                             "transport=0 exceptions=5 last_exc=0x02 mismatches=0 consec_fail=0 offline=1 "
                             "online=1 last_ok_age_ms=812 counter_disc=0"})
        self.assertFails(analyse(s), "DIAG")

    def test_no_activation_event_fails_cfg019(self):
        s = good_d7()
        s.lines = [(t, x) for t, x in s.lines if "EVT_CONFIG_ACTIVATED" not in x]
        self.assertFails(analyse(s), "CFG-019")

    def test_parse_sim_rx_and_transactions(self):
        s = Script()
        s.rx(10.0, 18, 1)
        s.rx(11.6, 18, 1)
        s.rx(40.0, 18, 1)
        rx = run.parse_sim_rx(s.sim)
        self.assertEqual([18, 18, 18], [r[2] for r in rx])
        self.assertAlmostEqual(10.0 + WALL0, rx[0][0], places=2)
        self.assertEqual(2, len(run.transactions([r[0] for r in rx])))


class RejectAnalysisTests(unittest.TestCase):

    def make(self, target="prod", polling=False, rx=False):
        s = Script()
        s.boot(target)
        if polling:
            s.emit(s.t, "MyEMS DP-WP4 polling started: 1 devices, 3 poll entries", tag="myems_app")
        if rx:
            s.rx(s.t + 2, 0, 14)
        s.phases.append(run.Phase("observe", "NORMAL", s.t, s.t + 25, s.t + WALL0, s.t + 25 + WALL0))
        return run.analyse_reject(s.lines, s.phases, CRC, s.sim)

    def test_production_build_rejects(self):
        rep = self.make()
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        self.assertTrue(any("rule=V-13" in r for r in rep.rows))

    def test_sim_build_fails_reject_suite(self):
        rep = self.make(target="sim")
        self.assertGreater(rep.failures, 0)

    def test_rejection_must_be_for_v13(self):
        s = Script()
        s.boot("prod")
        s.lines = [(t, x.replace("rule=V-13", "rule=V-06")) for t, x in s.lines]
        s.phases.append(run.Phase("observe", "NORMAL", s.t, s.t + 25, s.t + WALL0, s.t + 25 + WALL0))
        rep = run.analyse_reject(s.lines, s.phases, CRC, s.sim)
        self.assertTrue([r for r in rep.rows if r.startswith("FAIL  CFG-018")], "\n".join(rep.rows))
        s.lines = [(t, x) for t, x in s.lines if "EVT_CONFIG_REJECTED" not in x]
        rep = run.analyse_reject(s.lines, s.phases, CRC, s.sim)
        self.assertTrue([r for r in rep.rows if r.startswith("FAIL  CFG-018")], "\n".join(rep.rows))

    def test_rejection_line_is_not_polling_started(self):
        # HIL 2026-11-08: "No polling started" was mistaken for the start line.
        self.assertIsNone(run.POLL_START_RE.search("E (312) myems_app: No polling started (device stays READY)."))
        rep = self.make()
        self.assertTrue(any(r.startswith("PASS  CFG-018") and "polling not started" in r for r in rep.rows),
                        "\n".join(rep.rows))

    def test_d6_rejected_firmware_has_no_polling_started(self):
        s = Script()
        s.boot("prod")
        rep = run.analyse_d6(s.lines, [], CRC, s.sim)
        self.assertTrue([r for r in rep.rows if r.startswith("FAIL  config") and "polling started" in r],
                        "\n".join(rep.rows))

    def test_polling_or_bus_traffic_fails(self):
        self.assertGreater(self.make(polling=True).failures, 0)
        self.assertGreater(self.make(rx=True).failures, 0)


if __name__ == "__main__":
    unittest.main()
