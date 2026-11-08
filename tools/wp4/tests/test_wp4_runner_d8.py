"""Tests for the esc analysis of tools/wp4/myems_wp4_runner.py (DP-WP4 D8).

A scripted firmware follows the designed D8 timeline with the exact log lines
the firmware prints (ems_acquisition.c, ems_poll_scheduler.c, app_main.c,
modbus_rtu_master.c); each deliberate deviation must fail the check that
covers it. ESC-004/005 are BLOCK rows (COM change request CR-D8-01).

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
from test_wp4_runner import ALL  # noqa: E402
from test_wp4_runner_d7 import CRC, Script, all_good, polls  # noqa: E402


def master_fail(s, t):
    s.emit(t, "RS485 bus unavailable; recovery failed: transport reported FAILED", "E", "modbus_master")


def bus_fault(s, t, reason="TRANSPORT_FAILED"):
    s.emit(t, f"EVT_BUS_FAULT bus=RS485_0 reason={reason} transport=FAILED outstanding=1: HEALTH_BUS_FAULT; "
              "polling held, all devices on the bus OFFLINE (fail closed)", "E")
    s.emit(t, "bus RS485_0 stays BUS_FAULT until the master reports BUS_OK; the 09_057 master cannot retry "
              "recovery (COM change request CR-D8-01): restart the controller to recover", "E")


def hold(s, t, cancelled=1):
    s.emit(t, "BUS_HOLD: polling stopped, every device in probe mode; 1 request(s) in flight", "W", "ems_scheduler")
    s.emit(t, f"BUS_HOLD: cancelled {cancelled} request(s) in the master; their CANCELLED results release "
              "the slots", "W", "ems_scheduler")


def cancelled(s, t):
    s.emit(t, "GRID_INCOMER_01 grp0 request=0x00000011 CANCELLED")


def dev_offline(s, t):
    s.mode = "OFFLINE"
    s.set(ALL, "COMM_LOST")
    s.emit(t, "EVT_DEVICE_OFFLINE slot=0 GRID_INCOMER_01: bus fault; all parameters COMM_LOST", "W")


def stats(s, t, health="0x02", in_flight=0, holds=1):
    s.emit(t, f"STATS sched: submitted=14 skipped=0 rejected=0 guard_expired=0 fb_unmatched=0 fb_late=0 "
              f"health={health} in_flight={in_flight} bus_holds={holds} bus_cancelled=1 | acq: results=15 ok=13 "
              "failed=1 cancelled=1 routing_errors=0 stale=0", tag="myems_app")


def diag(s, t, health="BUS_FAULT", faults=1, transport=1):
    s.emit(t, f"DIAG slot=0 GRID_INCOMER_01 mode=OFFLINE ok=13 timeouts=0 crc=0 protocol=0 transport={transport} "
              "exceptions=0 last_exc=0x00 mismatches=0 consec_fail=1 offline=1 online=0 last_ok_age_ms=30000 "
              "counter_disc=0", tag="myems_app")
    s.emit(t, "DIAG bus accepted=15 completed=15 timeouts=0 crc=0 protocol=0 transport=1 completion_drops=0 "
              "backpressure=0 unexpected_slave=0 | sched_queue_hwm=2 sched_queue_full=0 | events=3 event_drops=0 "
              f"| health={health} reason=TRANSPORT_FAILED transport=FAILED bus_faults={faults} bus_recoveries=0",
              tag="myems_app")
    s.emit(t, "STACK task=ems_acquisition allocated=6144B current_free=4700B min_ever_free=4600B")


FAULT_EVENTS = [(3.0, lambda sc, t: sc.rx(t, 0, 14)), (5.6, master_fail), (5.65, bus_fault), (5.66, hold),
                (5.7, cancelled), (5.71, dev_offline)]


def esc_boot(s, variant="WP4_ESC_TEST"):
    s.boot()
    if variant:
        s.emit(s.t - 3, f"BUILD_VARIANT={variant}: RS-485 recovery is forced to fail (D8 HIL suite esc only)",
               "W", "myems_app")


def good_esc(o=None):
    o = o or {}
    s = Script()
    esc_boot(s, o.get("variant", "WP4_ESC_TEST"))
    s.phase("normal", "NORMAL", 25, polls(s, 0.5, 25) + [(1, all_good)])
    s.phase("fault", "TIMEOUT", 15, o.get("fault", FAULT_EVENTS))
    s.phase("fail_closed", "NORMAL", 75, o.get("fail_closed", [
        (20.0, lambda sc, t: stats(sc, t)), (20.0, lambda sc, t: diag(sc, t))]))
    return s


def analyse(s):
    return run.analyse_esc(s.lines, s.phases, CRC, s.sim)


class EscAnalysisTests(unittest.TestCase):

    def assertPass(self, rep):
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))

    def assertFails(self, rep, test_id):
        self.assertTrue([r for r in rep.rows if r.startswith("FAIL") and f" {test_id} " in r + " "],
                        f"expected a FAIL for {test_id}:\n" + "\n".join(rep.rows))

    def test_designed_behaviour_passes_with_two_blocked(self):
        rep = analyse(good_esc())
        self.assertPass(rep)
        self.assertEqual(2, rep.blocked)
        self.assertEqual(["ESC-004", "ESC-005"], [r.split()[1] for r in rep.rows if r.startswith("BLOCK")])
        self.assertTrue(any("CR-D8-01" in r for r in rep.rows if r.startswith("BLOCK")))
        self.assertTrue(any("BUILD_VARIANT=WP4_ESC_TEST" in r for r in rep.rows if r.startswith("PASS")))

    def test_esc_on_the_normal_simulation_build_fails_boot(self):
        rep = analyse(good_esc({"variant": None}))
        self.assertFails(rep, "boot")
        self.assertTrue(any("flash -Mode Wp4Esc" in r for r in rep.rows))

    def test_d7_on_the_esc_build_fails_boot(self):
        from test_wp4_runner_d7 import good_d7
        d = good_d7()
        d.emit(d.lines[0][0], "BUILD_VARIANT=WP4_ESC_TEST: RS-485 recovery is forced to fail", "W", "myems_app")
        rep = run.analyse_d7(d.lines, d.phases, CRC, d.sim)
        self.assertFails(rep, "boot")
        self.assertTrue(any("flash -Mode Wp4Sim" in r for r in rep.rows))

    def test_no_bus_fault_fails_esc002(self):
        ev = [e for e in FAULT_EVENTS if e[1] is not bus_fault]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-002")

    def test_bus_fault_too_late_fails_esc002(self):
        ev = [(e[0] + 9.0 if e[0] > 5 else e[0], e[1]) for e in FAULT_EVENTS]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-002")

    def test_two_bus_faults_fail_esc002(self):
        self.assertFails(analyse(good_esc({"fault": FAULT_EVENTS + [(9.0, bus_fault)]})), "ESC-002")

    def test_no_terminal_transport_error_fails_esc001(self):
        ev = [e for e in FAULT_EVENTS if e[1] is not master_fail]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-001")

    def test_stuck_slot_fails_esc001(self):
        rep = analyse(good_esc({"fail_closed": [(20.0, lambda sc, t: stats(sc, t, in_flight=1)),
                                                (20.0, lambda sc, t: diag(sc, t))]}))
        self.assertFails(rep, "ESC-001")

    def test_cancel_count_mismatch_fails_esc001(self):
        ev = [e for e in FAULT_EVENTS if e[1] is not cancelled]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-001")

    def test_no_hold_fails_esc001(self):
        ev = [e for e in FAULT_EVENTS if e[1] is not hold]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-001")

    def test_hold_flag_missing_fails_esc002(self):
        rep = analyse(good_esc({"fail_closed": [(20.0, lambda sc, t: stats(sc, t, health="0x00")),
                                                (20.0, lambda sc, t: diag(sc, t))]}))
        self.assertFails(rep, "ESC-002")

    def test_device_not_offline_fails_esc003(self):
        ev = [e for e in FAULT_EVENTS if e[1] is not dev_offline]
        self.assertFails(analyse(good_esc({"fault": ev})), "ESC-003")

    def test_values_zeroed_fail_q009(self):
        def zero(sc, t):
            sc.vals["ac.power.active.total"][0] = 0.0
        self.assertFails(analyse(good_esc({"fault": FAULT_EVENTS + [(6.0, zero)]})), "Q-009")

    def test_requests_after_fault_fail_fail_closed(self):
        rep = analyse(good_esc({"fail_closed": [(10.0, lambda sc, t: sc.rx(t, 18, 1)),
                                                (20.0, lambda sc, t: stats(sc, t)),
                                                (20.0, lambda sc, t: diag(sc, t))]}))
        self.assertFails(rep, "ESC-002")

    def test_health_not_reported_fails_esc002(self):
        rep = analyse(good_esc({"fail_closed": [(20.0, lambda sc, t: stats(sc, t)),
                                                (20.0, lambda sc, t: diag(sc, t, health="BUS_OK"))]}))
        self.assertFails(rep, "ESC-002")

    def test_bus_ok_without_master_report_fails_esc005(self):
        def ok(sc, t):
            sc.emit(t, "EVT_BUS_OK bus=RS485_0 after 30000 ms in BUS_FAULT: polling resumed")
        rep = analyse(good_esc({"fail_closed": [(5.0, ok), (20.0, lambda sc, t: stats(sc, t)),
                                                (20.0, lambda sc, t: diag(sc, t))]}))
        self.assertFails(rep, "ESC-005")

    def test_no_stats_after_fault_fails(self):
        rep = analyse(good_esc({"fail_closed": [(20.0, lambda sc, t: diag(sc, t))]}))
        self.assertFails(rep, "ESC-001")
        self.assertFails(rep, "RTE")

    def test_report_result_line_counts_blocked(self):
        import tempfile
        s = good_esc()
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()):
                rc = run.report(out, "esc", s.lines, s.phases, CRC, s.sim)
            self.assertEqual(0, rc)
            self.assertIn("RESULT: PASS, 2 BLOCKED", (out / "report.txt").read_text(encoding="utf-8"))


class ControlTests(unittest.TestCase):

    def test_bus_phase_control_text(self):
        self.assertEqual("mode * NORMAL\nbus BABBLE\n", run.control_text("BUS:BABBLE"))
        self.assertEqual("bus QUIET\nmode * EXCEPTION:02@energy\n", run.control_text("EXCEPTION:02@energy"))

    def test_esc_suite_definition(self):
        self.assertEqual(["normal", "fault", "fail_closed"], [p[0] for p in run.SUITE_ESC])
        self.assertEqual("TIMEOUT", run.SUITE_ESC[1][1])
        self.assertEqual("sim", run.BOOT_TARGET["esc"])
        self.assertIn("esc", run.ANALYSERS)

    def test_simulator_accepts_the_control_text(self):
        import myems_ac_meter_sim as sim_mod
        lines: list[str] = []
        sim = sim_mod.Simulator(sim_mod.load_layout(sim_mod.PROJECT_ROOT / "config" / "site" / "lab_bench.site.json"),
                                log=lines.append)
        sim.apply_control(run.control_text("BUS:BABBLE"))
        self.assertTrue(sim.babble)
        sim.apply_control(run.control_text("NORMAL"))
        self.assertFalse(sim.babble)
        self.assertFalse([ln for ln in lines if "CONTROL line" in ln])


if __name__ == "__main__":
    unittest.main()
