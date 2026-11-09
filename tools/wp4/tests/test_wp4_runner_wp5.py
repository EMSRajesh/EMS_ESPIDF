"""Tests for the DP-WP5 analysis of tools/wp4/myems_wp4_runner.py: suite dc (DC-001..DC-012).

A scripted firmware (console lines in the format of app_main.c and
ems_acquisition.c) and a scripted simulator log follow the designed
behaviour of the DC meter (site lab_dc_bench, slave 3 = slot 0); each
deliberate deviation must fail the check that covers it.

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
from test_wp4_runner import esp  # noqa: E402
from test_wp4_runner_d7 import WALL0, iso  # noqa: E402
from test_wp4_runner_d9 import assert_fails  # noqa: E402

LAYOUT = run.sim_mod.load_layout(run.suite_site("dc"))
MI = run.meter_info(LAYOUT, run.DC_SLAVE)
CRC = LAYOUT["config_crc32"]
UNITS = {r["param"]: r["unit"] for r in next(d for d in LAYOUT["devices"] if d["slave_id"] == 3)["registers"]}
V, I, P, IMP, EXP = "dc.voltage", "dc.current", "dc.power", "dc.energy.import", "dc.energy.export"
MEAS, ENERGY, STATUS = ("measurements", 4, 0, 6), ("energy", 4, 100, 4), ("status", 3, 200, 1)
GROUPS = {g[0]: g for g in (MEAS, ENERGY, STATUS)}
GROUP_OF = MI.group_of


class DcScript:
    """Console + simulator log of the DC meter, driven by a per-phase behaviour."""

    def __init__(self):
        self.lines: list[tuple[float, str]] = []
        self.sim: list[str] = []
        self.phases: list[run.Phase] = []
        self.t = 100.0
        self.vals = dict(MI.normal)
        self.q = {k: "GOOD" for k in MI.params}
        self.mode = "ONLINE"
        self.seq = 0
        self.next_poll = {"measurements": 100.3, "energy": 101.2, "status": 102.2}
        self.fail = 0
        self.emit(self.t, "BUILD_MODE=PRODUCTION CONFIG_TARGET=sim", tag="myems_app")
        self.emit(self.t, f"CONFIG site=LAB_DC_BENCH target=sim classification=SIMULATION_ONLY crc={CRC} "
                          "devices=1", tag="myems_app")
        self.emit(self.t, "EVT_CONFIG_ACTIVATED site=LAB_DC_BENCH", tag="myems_app")
        self.emit(self.t, "MyEMS DP-WP4 polling started: 1 devices, 3 poll entries", tag="myems_app")

    def emit(self, t, msg, level="I", tag="ems_acquisition"):
        self.lines.append((t, esp(int(t * 1000), msg, level, tag)))

    def dump(self, t):
        self.emit(t, f"VALUES slot=0 DC_BATT_STRING_01 mode={self.mode} count=6")
        for k in MI.params:
            self.seq += 1
            self.emit(t, f"VAL slot=0 {k}={self.vals[k]:.9g} {UNITS[k]} q={self.q[k]} age_ms=100 seq={self.seq}")

    def rx(self, t, group, fc=None):
        _, gfc, start, count = GROUPS[group]
        req = with_crc(bytes((3, fc or gfc, start >> 8, start & 0xFF, 0, count)))
        self.sim.append(f"{iso(t + WALL0)}   {t * 1000:10.1f} RX slave=3 fc={fc or gfc:02X} "
                        f"{req.hex(' ').upper()} mode=X")

    def phase(self, name, mode, dur, behave="answer", hooks=(), answer_after=None, probes_only=False):
        """behave: 'answer' (polls succeed, values set by hooks), 'fail' (no answer: offline after 2 failures),
        hooks: (offset, fn(script, t)) events; answer_after: in 'fail', answer again from this offset."""
        t0 = self.t
        t_end = t0 + dur
        dumps = [t0 + 5.0 * k for k in range(1, int(dur // 5) + 1)]
        hooks = sorted(hooks, key=lambda h: h[0])
        t = t0
        while t < t_end - 1e-9:
            nxt = min([self.next_poll[g] for g in self.next_poll if self.mode == "ONLINE" or g == "status"]
                      + [d for d in dumps if d > t] + [t0 + h[0] for h in hooks if t0 + h[0] > t] + [t_end])
            for h in [h for h in hooks if t < t0 + h[0] <= nxt]:
                h[1](self, t0 + h[0])
            t = nxt
            for d in [d for d in dumps if abs(d - t) < 1e-9]:
                self.dump(t)
            for g in list(self.next_poll):
                if abs(self.next_poll[g] - t) > 1e-9 or (self.mode == "OFFLINE" and g != "status"):
                    continue
                answering = behave == "answer" or (answer_after is not None and t - t0 >= answer_after)
                self.rx(t, g)
                if answering:
                    if self.mode == "OFFLINE":
                        self.mode = "ONLINE"
                        self.emit(t + 0.1, "EVT_DEVICE_ONLINE slot=0 DC_BATT_STRING_01", "W")
                        for k in MI.params:
                            self.q[k] = "GOOD"
                        for g2 in self.next_poll:          # all groups scheduled at once (SCH-007)
                            if g2 != g:
                                self.next_poll[g2] = t + 0.2 + 0.1 * list(self.next_poll).index(g2)
                    self.fail = 0
                    self.next_poll[g] += MI.interval_s[g]
                else:
                    self.rx(t + 1.6, g)                    # the master's retry
                    self.fail += 1
                    for k in MI.params:
                        if GROUP_OF[k] == g and self.q[k] == "GOOD":
                            self.q[k] = "STALE"
                    if self.fail == 2 and self.mode == "ONLINE":
                        self.mode = "OFFLINE"
                        self.emit(t + 3.1, "EVT_DEVICE_OFFLINE slot=0 DC_BATT_STRING_01: 2 consecutive failed "
                                           "transactions; all parameters COMM_LOST", "W")
                        for k in MI.params:
                            self.q[k] = "COMM_LOST"
                        self.next_poll["status"] = t + 3.1 + 30.0
                    elif self.mode == "OFFLINE":
                        self.next_poll[g] += 30.0
                    else:
                        self.next_poll[g] += MI.interval_s[g]
        self.t = t_end
        self.phases.append(run.Phase(name, mode, t0, t_end, t0 + WALL0, t_end + WALL0))

    def stats(self):
        self.emit(self.t, "STATS sched: submitted=10 skipped=0 rejected=0 guard_expired=0 fb_unmatched=0 "
                          "fb_late=0 health=0x00 in_flight=0 bus_holds=0 bus_cancelled=0 | acq: results=10 ok=10 "
                          "failed=0 cancelled=0 routing_errors=0 stale=0", tag="myems_app")


def setv(**kw):
    def fn(s, t):
        for k, v in kw.items():
            s.vals[k.replace("_", ".")] = v
    return fn


def setq(params, q):
    def fn(s, t):
        for k in params:
            s.q[k] = q
    return fn


def counter_ramp(param, start, step):
    def fn(s, t):
        s.vals[param] = start if s.vals[param] < start else round(s.vals[param] + step, 2)
    return fn


def good_dc(o=None):
    """The designed run; o: dict of deviations, by name."""
    o = o or {}
    s = DcScript()
    n = MI.normal
    s.phase("normal", "NORMAL", 40)
    s.phase("import", "IMPORT", 40, hooks=[(k, setv(**{"dc_energy_import": n[IMP] + 0.28 * k / 10}))
                                            for k in (2, 12, 22, 32)])
    exp_val = o.get("export_power", -abs(n[P]))
    s.phase("export", "EXPORT", 40, hooks=[(0.1, setv(dc_power=exp_val, dc_current=-abs(n[I])))]
            + [(k, setv(**{"dc_energy_export": 0.28 * k / 10})) for k in (2, 12, 22, 32)])
    oor_q = o.get("oor_q", "OUT_OF_RANGE")
    s.phase("out_of_range", "OUT_OF_RANGE", 20,
            hooks=[(0.1, setv(dc_power=n[P], dc_current=n[I])), (1.0, setv(dc_voltage=1800.0)),
                   (1.0, setq([V], oor_q))])
    resets = o.get("resets", 1)
    hooks = [(0.2, setv(dc_voltage=n[V])), (0.2, setq([V], "GOOD"))]
    for k in range(resets):
        hooks.append((1.5 + k, lambda s, t: s.emit(t, f"EVT_COUNTER_DISCONTINUITY slot=0 DC_BATT_STRING_01 "
                                                     f"param={IMP}: counter decreased or reset; new baseline", "W")))
    hooks += [(1.5, setv(dc_energy_import=0.0)), (11.5, setv(dc_energy_import=0.28)),
              (21.5, setv(dc_energy_import=0.56))]
    s.phase("counter_reset", "COUNTER_RESET", 25, hooks=hooks)
    slow_hooks = []
    if o.get("slow_retry"):
        slow_hooks.append((10.0, lambda s, t: s.rx(t + 1.6, "measurements")))
    s.phase("slow", "SLOW:400", 30, hooks=slow_hooks)
    s.phase("exception_energy", "EXCEPTION:02@energy", 25,
            hooks=[(2.0, setq([IMP, EXP], "INVALID")),
                   (2.0, lambda s, t: s.emit(t, "EVT_MODBUS_EXCEPTION slot=0 DC_BATT_STRING_01 grp1 code=0x02: "
                                                "group INVALID", "W"))])
    s.phase("timeout", "TIMEOUT", 30, behave="fail", hooks=[(0.0, setq([IMP, EXP], "GOOD"))]
            + ([(25.0, setq([P], "GOOD"))] if o.get("good_after_loss") else []))
    s.phase("recover", "NORMAL", 45)
    s.phase("offline_recover", "OFFLINE_THEN_RECOVER:65", 100, behave="fail", answer_after=65.0)
    if o.get("probe_other"):
        s.rx(s.phases[-1].t0 + 50.0, "measurements")
    s.phase("bad_crc", "BAD_CRC", 30, behave="fail")
    s.phase("recover_crc", "NORMAL", 45)
    s.phase("final", "NORMAL", 15)
    s.stats()
    if o.get("wrong_fc"):
        s.rx(150.0, "status", fc=4)
    if o.get("drift"):        # measurements polled at 4.3 s instead of 4 s for a stretch
        s.sim = [ln for ln in s.sim if not (" fc=04 03 04 00 00 " in ln and 100 < float(ln.split()[1]) / 1000 < 200)]
        for k in range(23):
            s.rx(100.3 + 4.3 * k, "measurements")
    return s


def analyse(s):
    return run.analyse_dc(s.lines, s.phases, CRC, s.sim, layout=LAYOUT)


class DcAnalysisTests(unittest.TestCase):

    def test_meter_info_comes_from_roles(self):
        self.assertEqual((3, 0), (MI.slave, MI.slot))
        self.assertEqual("dc.power", MI.power)
        self.assertEqual(["dc.power", "dc.current"], MI.signed)          # pf_sign NOT_APPLICABLE
        self.assertEqual(("dc.energy.import", "dc.energy.export"), (MI.imp, MI.exp))
        self.assertEqual({"dc.voltage": 1800.0}, MI.oor)
        self.assertEqual("status", MI.status_group)
        self.assertEqual(800.0, MI.normal["dc.voltage"])                   # from the map test vector

    def test_designed_run_passes(self):
        rep = analyse(good_dc())
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))
        for tid in ("DC-001", "DC-002", "DC-003", "DC-004", "DC-005", "DC-006", "DC-007", "DC-008",
                    "DC-009", "DC-010", "DC-011", "DC-012"):
            self.assertTrue(any(f" {tid} " in r + " " for r in rep.rows if r.startswith("PASS")), tid)

    def test_wrong_function_code_fails_DC001(self):
        assert_fails(self, analyse(good_dc({"wrong_fc": True})), "DC-001")

    def test_wrong_crc_fails_config(self):
        s = good_dc()
        rep = run.analyse_dc(s.lines, s.phases, "0x12345678", s.sim, layout=LAYOUT)
        assert_fails(self, rep, "config")

    def test_interval_drift_fails_DC003(self):
        assert_fails(self, analyse(good_dc({"drift": True})), "DC-003")

    def test_wrong_export_sign_fails_DC005(self):
        assert_fails(self, analyse(good_dc({"export_power": abs(MI.normal[P])})), "DC-005")

    def test_out_of_range_not_flagged_fails_DC006(self):
        assert_fails(self, analyse(good_dc({"oor_q": "GOOD"})), "DC-006")

    def test_two_discontinuities_fail_DC007(self):
        assert_fails(self, analyse(good_dc({"resets": 2})), "DC-007")

    def test_retry_during_slow_fails_DC012(self):
        assert_fails(self, analyse(good_dc({"slow_retry": True})), "DC-012")

    def test_good_value_after_loss_fails_DC008(self):
        assert_fails(self, analyse(good_dc({"good_after_loss": True})), "DC-008")

    def test_other_group_polled_while_offline_fails_DC009(self):
        assert_fails(self, analyse(good_dc({"probe_other": True})), "DC-009")

    def test_suite_wiring(self):
        self.assertIs(run.ANALYSERS["dc"], run.analyse_dc)
        self.assertEqual("Wp5Dc", run.SUITE_BUILD["dc"])
        self.assertEqual("lab_dc_bench.site.json", run.suite_site("dc").name)
        self.assertEqual(485, sum(d for _, _, d in run.suite_phases("dc")))
        ctl = run.control_text("EXCEPTION:02@energy")
        self.assertIn("mode * EXCEPTION:02@energy", ctl)


if __name__ == "__main__":
    unittest.main()
