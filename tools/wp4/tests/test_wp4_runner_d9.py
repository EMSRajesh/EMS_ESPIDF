"""Tests for the D9 analyses of tools/wp4/myems_wp4_runner.py: two_dev, ratio,
load (PERF-001) and soak (PERF-002..006).

A scripted firmware (console lines in the exact format of app_main.c,
ems_acquisition.c and ems_stack_diag.h) and a scripted simulator log follow
the designed behaviour; each deliberate deviation must fail the check that
covers it.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import myems_wp4_runner as run  # noqa: E402
from myems_ac_meter_sim import with_crc  # noqa: E402
from test_wp4_runner import NORMAL, UNITS, esp  # noqa: E402
from test_wp4_runner_d7 import WALL0, iso  # noqa: E402

CRC = "0xE3FF2DF7"
TASKS = ("ems_acquisition", "ems_scheduler", "modbus_master", "rs485_transport", "main")


class D9Script:
    """Console + simulator log for any number of slots."""

    def __init__(self, slots=1, crc=CRC):
        self.lines: list[tuple[float, str]] = []
        self.sim: list[str] = []
        self.phases: list[run.Phase] = []
        self.t = 100.0
        self.slots = slots
        self.mode = {s: "ONLINE" for s in range(slots)}
        self.q = {s: {k: "GOOD" for k in NORMAL} for s in range(slots)}
        self.vals = {s: dict(NORMAL) for s in range(slots)}
        self.seq = 0
        self.emit(self.t, "BUILD_MODE=PRODUCTION CONFIG_TARGET=sim", tag="myems_app")
        self.emit(self.t, f"CONFIG site=X target=sim classification=SIMULATION_ONLY crc={crc} devices={slots}",
                  tag="myems_app")
        self.emit(self.t, "EVT_CONFIG_ACTIVATED site=X", tag="myems_app")
        self.emit(self.t, "PERF heap_free_at_start=200000 heap_min_at_start=199000", tag="myems_app")
        self.t += 3

    def emit(self, t, msg, level="I", tag="ems_acquisition"):
        self.lines.append((t, esp(int(t * 1000), msg, level, tag)))

    def dump(self, t):
        for s in range(self.slots):
            self.emit(t, f"VALUES slot={s} DEV{s} mode={self.mode[s]} count=14")
            for k, v in self.vals[s].items():
                self.seq += 1
                self.emit(t, f"VAL slot={s} {k}={v:.9g} {UNITS[k]} q={self.q[s][k]} age_ms=100 seq={self.seq}")

    def set_all(self, slot, q, mode):
        self.q[slot] = {k: q for k in NORMAL}
        self.mode[slot] = mode

    def rx(self, t, slave, start, count, reply=True):
        req = with_crc(bytes((slave, 3, start >> 8, start & 0xFF, 0, count)))
        self.sim.append(f"{iso(t + WALL0)}   {t * 1000:10.1f} RX slave={slave} fc=03 {req.hex(' ').upper()} mode=X")
        if reply:
            self.sim.append(f"{iso(t + 0.01 + WALL0)}   {t * 1000:10.1f} TX slave={slave} 01 03 02 00 00")

    def periodic(self, t, perf=None, stack_min=4000, entries=None, backpressure=0):
        p = {"heap": 200000, "elec_max": 900, "out": 1, "back": 1}
        p.update(perf or {})
        self.emit(t, "STATS sched: submitted=10 skipped=0 rejected=0 guard_expired=0 fb_unmatched=0 fb_late=0 "
                     "health=0x00 in_flight=0 bus_holds=0 bus_cancelled=0 | acq: results=10 ok=10 failed=0 "
                     "cancelled=0 routing_errors=0 stale=0", tag="myems_app")
        self.emit(t, f"PERF heap_free={p['heap']} heap_min=198000 | acq_proc_us elec_last=800 "
                     f"elec_max={p['elec_max']} max={p['elec_max'] + 100} | master_outstanding_hwm={p['out']} "
                     f"completion_backlog_hwm={p['back']}", tag="myems_app")
        for e, (slot, grp, sub, lat) in (entries or {}).items():
            self.emit(t, f"DIAG entry={e} slot={slot} grp={grp} submitted={sub} skipped=0 rejected=0 "
                         f"latency_ms={lat} max_latency_ms={lat + 10}", tag="myems_app")
        self.emit(t, "DIAG bus accepted=10 completed=10 timeouts=0 crc=0 protocol=0 transport=0 completion_drops=0 "
                     f"backpressure={backpressure} unexpected_slave=0 | sched_queue_hwm=2 sched_queue_full=0 | "
                     "events=3 event_drops=0 | health=BUS_OK reason=NONE transport=IDLE bus_faults=0 bus_recoveries=0",
                  tag="myems_app")
        for task in TASKS:
            self.emit(t, f"STACK task={task} allocated=6144B current_free={stack_min + 100}B "
                         f"min_ever_free={stack_min}B", tag=task)

    def phase(self, name, mode, dur, events, dump_every=5.0):
        t0 = self.t
        n = int(dur // dump_every)
        timeline = sorted(events + [(k * dump_every, lambda s, t: s.dump(t)) for k in range(1, n + 1)],
                          key=lambda e: e[0])
        for off, fn in timeline:
            fn(self, t0 + off)
        self.t = t0 + dur
        self.phases.append(run.Phase(name, mode, t0, self.t, t0 + WALL0, self.t + WALL0))


def assert_fails(tc, rep, test_id):
    tc.assertTrue([r for r in rep.rows if r.startswith("FAIL") and f" {test_id} " in r + " "],
                  f"expected a FAIL for {test_id}:\n" + "\n".join(rep.rows))


# ------------------------------------------------------------------ two_dev ---
def polls2(t0, t1, every=5.0, slaves=(1, 2), skip=()):
    ev = []
    t = t0
    while t < t1:
        for sl in slaves:
            if sl not in skip:
                ev.append((t + 0.2 * sl, lambda sc, tt, s=sl: sc.rx(tt, s, 0, 14)))
        t += every
    return ev


def good_two_dev(o=None):
    o = o or {}
    s = D9Script(slots=2)
    s.phase("normal", "NORMAL", 40, polls2(0.5, 40))
    s.phase("dev1_offline", "1=TIMEOUT", 90, o.get("off", [
        (8.0, lambda sc, t: (sc.set_all(0, "COMM_LOST", "OFFLINE"), sc.emit(
            t, "EVT_DEVICE_OFFLINE slot=0 GRID_INCOMER_01: 2 consecutive failed transactions; all parameters "
               "COMM_LOST; probing group 2 every 30000 ms", "W")))]) + polls2(0.5, 90, slaves=(2,)))
    s.phase("recover", "NORMAL", 45, o.get("recover", [
        (20.0, lambda sc, t: (sc.set_all(0, "GOOD", "ONLINE"),
                              sc.emit(t, "EVT_DEVICE_ONLINE slot=0 GRID_INCOMER_01: device answered")))]) + polls2(0.5, 45))
    s.phase("final", "NORMAL", 15, polls2(0.5, 15))
    s.periodic(s.t)
    return s


class TwoDevTests(unittest.TestCase):

    def test_designed_behaviour_passes(self):
        rep = run.analyse_two_dev(*_args(good_two_dev()))
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        self.assertTrue(any("slave 2 electrical poll interval" in r for r in rep.rows))

    def test_slave2_disturbed_fails_dev003(self):
        ev = [(8.0, lambda sc, t: (sc.set_all(0, "COMM_LOST", "OFFLINE"), sc.emit(t, "EVT_DEVICE_OFFLINE slot=0 X", "W"))),
              (30.0, lambda sc, t: sc.q[1].__setitem__("ac.frequency", "STALE"))]
        assert_fails(self, run.analyse_two_dev(*_args(good_two_dev({"off": ev}))), "DEV-003")

    def test_wrong_device_offline_fails_dev003(self):
        ev = [(8.0, lambda sc, t: (sc.set_all(1, "COMM_LOST", "OFFLINE"), sc.emit(t, "EVT_DEVICE_OFFLINE slot=1 X", "W")))]
        assert_fails(self, run.analyse_two_dev(*_args(good_two_dev({"off": ev}))), "DEV-003")

    def test_slave2_poll_gap_fails_dev003(self):
        s = good_two_dev()
        ph = s.phases[1]
        s.sim = [ln for ln in s.sim if not (" RX slave=2 " in ln and _wall(ln) and
                                            ph.w0 + 30 < _wall(ln) < ph.w0 + 40)]
        assert_fails(self, run.analyse_two_dev(*_args(s)), "DEV-003")

    def test_overlap_fails_sch003(self):
        s = good_two_dev()
        s.sim.append(f"{iso(s.t + WALL0)} 1.0 OVERLAP slave=2: new request while a reply is still pending")
        assert_fails(self, run.analyse_two_dev(*_args(s)), "SCH-003")

    def test_no_recovery_fails_dev002(self):
        assert_fails(self, run.analyse_two_dev(*_args(good_two_dev({"recover": []}))), "DEV-002")

    def test_cross_talk_fails_rte005(self):
        s = D9Script(slots=2)
        s.vals[1]["ac.current.l1"] = 99.0              # slot 1 shows a value that is not slave 2's
        s.phase("normal", "NORMAL", 40, polls2(0.5, 40))
        assert_fails(self, run.analyse_two_dev(*_args(s)), "RTE-005")


def _wall(line):
    try:
        import datetime as dt
        return dt.datetime.fromisoformat(line.split()[0]).timestamp()
    except ValueError:
        return None


def _args(s, crc=CRC):
    return s.lines, s.phases, crc, s.sim


# -------------------------------------------------------------------- ratio ---
def good_ratio(vals=None):
    s = D9Script(slots=1, crc="0x5A2CC286")
    s.vals[0].update({"ac.current.l1": 500.0, "ac.current.l2": 1200.0, "ac.power.active.total": 200.0,
                      "ac.energy.import.active": 20050.0})
    s.q[0]["ac.current.l2"] = "OUT_OF_RANGE"
    for k, v in (vals or {}).items():
        s.vals[0][k] = v
    s.phase("vectors", run.SUITE_RATIO[0][1], 30, [])
    s.periodic(s.t)
    return s


class RatioTests(unittest.TestCase):

    def test_vectors_4_3_pass(self):
        rep = run.analyse_ratio(*_args(good_ratio(), "0x5A2CC286"))
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        self.assertEqual(6, sum(1 for r in rep.rows if r.startswith("PASS  CONV")))

    def test_energy_times_ct_squared_fails_conv005(self):
        assert_fails(self, run.analyse_ratio(*_args(good_ratio({"ac.energy.import.active": 4010000.0}),
                                                     "0x5A2CC286")), "CONV-005")

    def test_ratio_not_applied_fails_conv004(self):
        assert_fails(self, run.analyse_ratio(*_args(good_ratio({"ac.current.l1": 2.5}), "0x5A2CC286")), "CONV-004")

    def test_out_of_range_not_flagged_fails_conv006(self):
        s = good_ratio()
        s2 = D9Script(slots=1, crc="0x5A2CC286")
        s2.vals = s.vals
        s2.phase("vectors", "NORMAL", 30, [])        # l2 left GOOD
        assert_fails(self, run.analyse_ratio(*_args(s2, "0x5A2CC286")), "CONV-006")

    def test_wrong_firmware_crc_fails_config(self):
        assert_fails(self, run.analyse_ratio(*_args(good_ratio(), "0x4B8B5924")), "config")


# --------------------------------------------------------------------- load ---
SITE2 = run.PROJECT_ROOT / "config" / "site" / "lab_bench_2dev.site.json"


def good_load(lat=(70, 50, 43), minutes=10, o=None):
    o = o or {}
    s = D9Script(slots=2)
    ev = polls2(0.5, minutes * 60, every=5.0)
    # DIAG entries every 60 s: 6 entries (2 devices x electrical/energy/status)
    rates = (12, 6, 2)                     # per minute
    for k in range(1, minutes + 1):
        entries = {}
        for e in range(6):
            slot, grp = divmod(e, 3)
            entries[e] = (slot, grp, rates[grp] * k, lat[grp])
        ev.append((k * 60.0 - 0.1, lambda sc, t, en=entries: sc.periodic(t, entries=en, **o)))
    s.phase("load", "NORMAL", minutes * 60, ev)
    return s


class LoadTests(unittest.TestCase):

    def analyse(self, s):
        return run.analyse_load(s.lines, s.phases, CRC, s.sim, site=SITE2)

    def test_designed_load_passes(self):
        rep = self.analyse(good_load())
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        row = next(r for r in rep.rows if "measured bus load" in r)
        self.assertIn("budget nominal 6.3", row)

    def test_load_far_below_budget_fails_perf001(self):
        assert_fails(self, self.analyse(good_load(lat=(20, 15, 12))), "PERF-001")

    def test_load_far_above_budget_fails_perf001(self):
        assert_fails(self, self.analyse(good_load(lat=(250, 200, 180))), "PERF-001")

    def test_unanswered_requests_fail_perf001(self):
        s = good_load()
        s.sim = [ln for ln in s.sim if " TX " not in ln][:]
        assert_fails(self, self.analyse(s), "PERF-001")

    def test_queue_high_water_fails_perf004(self):
        assert_fails(self, self.analyse(good_load(o={"perf": {"out": 5}})), "PERF-004")

    def test_backpressure_fails_perf004(self):
        assert_fails(self, self.analyse(good_load(o={"backpressure": 1})), "PERF-004")

    def test_slow_processing_fails_perf006(self):
        assert_fails(self, self.analyse(good_load(o={"perf": {"elec_max": 2500}})), "PERF-006")

    def test_low_stack_fails_perf003(self):
        assert_fails(self, self.analyse(good_load(o={"stack_min": 900})), "PERF-003")


# --------------------------------------------------------------------- soak ---
def soak_script(n_phases=6, o=None):
    """NORMAL / TIMEOUT / OFFLINE_THEN_RECOVER:300 phases of 600 s, dumps every 5 s."""
    o = o or {}
    s = D9Script(slots=1, crc="0x4B8B5924")
    for k, (name, mode, dur) in enumerate(run.soak_phases(n_phases * 10)[:n_phases]):
        ev = []
        if mode == "TIMEOUT":
            ev.append((o.get("off_at", 9.0), lambda sc, t: (sc.set_all(0, "COMM_LOST", "OFFLINE"),
                                                            sc.emit(t, "EVT_DEVICE_OFFLINE slot=0 X", "W"))))
        if mode.startswith("OFFLINE_THEN_RECOVER"):
            ev.append((o.get("on_at", 325.0), lambda sc, t: (sc.set_all(0, "GOOD", "ONLINE"),
                                                             sc.emit(t, "EVT_DEVICE_ONLINE slot=0 X"))))
        for m in range(1, 11):
            ev.append((m * 60.0 - 0.1, lambda sc, t: sc.periodic(t, **o.get("periodic", {}))))
        ev += o.get("extra", {}).get(k, [])
        s.phase(name, mode, dur, ev)
    return s


class SoakTests(unittest.TestCase):

    def analyse(self, s, minutes=None):
        return run.analyse_soak(s.lines, s.phases, "0x4B8B5924", s.sim, requested_min=minutes)

    def test_phase_plan(self):
        ph = run.soak_phases(1440)
        self.assertEqual(144, len(ph))
        self.assertEqual(("normal_1", "NORMAL", 600), ph[0])
        self.assertEqual(("timeout_2", "TIMEOUT", 600), ph[1])
        self.assertEqual(("offline_recover_3", "OFFLINE_THEN_RECOVER:300", 600), ph[2])
        self.assertEqual(6, len(run.soak_phases(60)))
        self.assertEqual(3, len(run.soak_phases(5)))
        self.assertEqual([("load", "NORMAL", 3600)], run.load_phases(60))
        self.assertEqual(run.suite_phases("soak", 60), run.soak_phases(60))
        self.assertEqual(run.SUITE_TWO_DEV, run.suite_phases("two_dev"))

    def test_designed_soak_passes(self):
        rep = self.analyse(soak_script(), minutes=60)
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        self.assertTrue(any("2 EVT_DEVICE_OFFLINE (expected 2" in r for r in rep.rows))

    def test_too_short_run_fails_perf005(self):
        assert_fails(self, self.analyse(soak_script(), minutes=1440), "PERF-005")

    def test_offline_in_normal_phase_fails_perf005(self):
        extra = {0: [(300.0, lambda sc, t: sc.emit(t, "EVT_DEVICE_OFFLINE slot=0 X", "W"))]}
        assert_fails(self, self.analyse(soak_script(o={"extra": extra}), minutes=60), "PERF-005")

    def test_late_recovery_fails_perf005(self):
        assert_fails(self, self.analyse(soak_script(o={"on_at": 400.0}), minutes=60), "PERF-005")

    def test_good_values_while_silent_fail_perf005(self):
        extra = {1: [(100.0, lambda sc, t: sc.q[0].__setitem__("ac.frequency", "GOOD"))]}
        assert_fails(self, self.analyse(soak_script(o={"extra": extra}), minutes=60), "PERF-005")

    def test_heap_drift_fails_perf002(self):
        s = soak_script()
        s.periodic(s.t + 1, perf={"heap": 198000})
        assert_fails(self, self.analyse(s, minutes=60), "PERF-002")

    def test_missing_task_stack_fails_perf003(self):
        s = soak_script()
        s.lines = [(t, x) for t, x in s.lines if "STACK task=main " not in x]
        assert_fails(self, self.analyse(s, minutes=60), "PERF-003")

    def test_bus_fault_fails_perf005(self):
        extra = {0: [(100.0, lambda sc, t: sc.emit(t, "EVT_BUS_FAULT bus=B reason=TRANSPORT_FAILED transport=FAILED", "E"))]}
        assert_fails(self, self.analyse(soak_script(o={"extra": extra}), minutes=60), "PERF-005")

    def test_reboot_fails_stability(self):
        extra = {2: [(100.0, lambda sc, t: sc.emit(t, "rst:0xc (RTC_SW_CPU_RST),boot:0x8", tag="rom"))]}
        assert_fails(self, self.analyse(soak_script(o={"extra": extra}), minutes=60), "stability")


# ------------------------------------------------------------------ control ---
class ControlD9Tests(unittest.TestCase):

    def test_control_text_items(self):
        self.assertEqual("bus QUIET\nmode * NORMAL\nmode * TIMEOUT\n", run.control_text("TIMEOUT"))
        self.assertEqual("bus QUIET\nmode * NORMAL\nmode 1 TIMEOUT\n", run.control_text("1=TIMEOUT"))
        self.assertEqual("bus QUIET\nmode * NORMAL\nmode * NORMAL\nset 1 ac.current.l1 2.5\n",
                         run.control_text("NORMAL|set 1 ac.current.l1 2.5"))
        self.assertEqual("bus QUIET\nmode * NORMAL\nbus BABBLE\n", run.control_text("BUS:BABBLE"))

    def test_simulator_accepts_every_d9_control_text(self):
        import myems_ac_meter_sim as sim_mod
        for site, modes in ((SITE2, [m for _, m, _ in run.SUITE_TWO_DEV]),
                            (run.suite_site("ratio"), [m for _, m, _ in run.SUITE_RATIO]),
                            (run.suite_site("soak"), [m for _, m, _ in run.soak_phases(30)])):
            lines: list[str] = []
            sim = sim_mod.Simulator(sim_mod.load_layout(site), log=lines.append)
            for m in modes:
                sim.apply_control(run.control_text(m))
            self.assertFalse([ln for ln in lines if "CONTROL line" in ln], (site, lines))

    def test_ratio_sets_reach_the_register_image(self):
        import myems_ac_meter_sim as sim_mod
        sim = sim_mod.Simulator(sim_mod.load_layout(run.suite_site("ratio")))
        sim.apply_control(run.control_text(run.SUITE_RATIO[0][1]))
        self.assertEqual(2.5, sim.devices[1].values["ac.current.l1"])
        self.assertEqual(6.0, sim.devices[1].values["ac.current.l2"])

    def test_suite_sites_and_builds(self):
        self.assertEqual("lab_bench_2dev.site.json", run.suite_site("two_dev").name)
        self.assertEqual("lab_ratio_ct200.site.json", run.suite_site("ratio").name)
        self.assertEqual("lab_bench.site.json", run.suite_site("soak").name)
        self.assertEqual("Wp4Sim2Dev", run.SUITE_BUILD["load"])
        for name in run.SUITES:
            self.assertIn(name, run.ANALYSERS)
            self.assertIn(name, run.BOOT_TARGET)


if __name__ == "__main__":
    unittest.main()
