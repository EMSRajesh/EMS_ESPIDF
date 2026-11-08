"""Tests for the analysis in tools/wp4/myems_wp4_runner.py (DP-WP4 D6).

A scripted 'firmware' produces console lines in the exact format of
ems_acquisition.c (ESP-IDF log prefix and colour codes included), phase by
phase. The good script must PASS; each deliberate deviation must FAIL the
check that covers it.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import myems_ac_meter_sim as sim_mod  # noqa: E402
import myems_wp4_runner as run  # noqa: E402

NORMAL = dict(sim_mod.NORMAL_VALUES)
UNITS = {"ac.voltage.l1_n": "V", "ac.voltage.l2_n": "V", "ac.voltage.l3_n": "V", "ac.current.l1": "A",
         "ac.current.l2": "A", "ac.current.l3": "A", "ac.power.active.total": "kW",
         "ac.power.reactive.total": "kvar", "ac.power.apparent.total": "kVA", "ac.power.factor.total": "1",
         "ac.frequency": "Hz", "ac.energy.import.active": "kWh", "ac.energy.export.active": "kWh",
         "dev.status.code": "-"}
CRC = "0x4B8B5924"


def esp(t_ms: int, msg: str, level: str = "I", tag: str = "ems_acquisition") -> str:
    color = {"I": "\x1b[0;32m", "W": "\x1b[0;33m", "E": "\x1b[0;31m"}[level]
    return f"{color}{level} ({t_ms}) {tag}: {msg}\x1b[0m"


class FakeFirmware:
    """Emits boot lines, then a VAL dump every 5 s according to the phase script."""

    def __init__(self):
        self.lines: list[tuple[float, str]] = []
        self.phases: list[run.Phase] = []
        self.t = 100.0
        self.vals = {k: [v, "INITIALIZING", 0.0] for k, v in NORMAL.items()}   # value, q, sample time
        self.seq = 0

    def boot(self, crc: str = "4B8B5924", target: str = "sim"):
        self.emit(f"BUILD_MODE=PRODUCTION CONFIG_TARGET={target}", tag="myems_app")
        self.emit(f"CONFIG site=LAB_BENCH_01 target=sim classification=SIMULATION_ONLY crc=0x{crc} devices=1",
                  tag="myems_app")
        self.emit("MyEMS DP-WP4 polling started: 1 devices, 3 poll entries", tag="myems_app")
        self.t += 3

    def emit(self, msg, level="I", tag="ems_acquisition", t=None):
        tt = self.t if t is None else t
        self.lines.append((tt, esp(int(tt * 1000), msg, level, tag)))

    def set(self, params, value=None, q=None, fresh=True):
        for k in params:
            if value is not None:
                self.vals[k][0] = value if not callable(value) else value(self.vals[k][0])
            if q is not None:
                self.vals[k][1] = q
            if fresh and q == "GOOD":
                self.vals[k][2] = self.t

    def dump(self):
        self.emit("VALUES slot=0 GRID_INCOMER_01 count=14")
        for k, (v, q, ts) in self.vals.items():
            self.seq += 1
            age = int((self.t - ts) * 1000) if ts > 0 else -1
            self.emit(f"VAL slot=0 {k}={v:.9g} {UNITS[k]} q={q} age_ms={age} seq={self.seq}")

    def phase(self, name, mode, dur, script):
        t0 = self.t
        steps = int(dur // 5)
        for i in range(steps):
            self.t = t0 + (i + 1) * 5.0
            script(self, (i + 1) * 5.0)
            self.dump()
        self.t = t0 + dur
        self.phases.append(run.Phase(name, mode, t0, self.t))


ELEC, ENER = run.ELECTRICAL, run.ENERGY
ALL = ELEC + ENER + ("dev.status.code",)


def good_run(overrides=None):
    """A firmware that behaves as designed. overrides: phase name -> replacement script."""
    o = overrides or {}
    fw = FakeFirmware()
    fw.boot()

    def normal(f, s):
        f.set(ALL, q="GOOD")
        for k in ELEC:
            f.vals[k][0] = NORMAL[k]

    def imp(f, s):
        normal(f, s)
        f.set(["ac.energy.import.active"], value=lambda v: round(v + 0.04, 2), q="GOOD")

    def exp(f, s):
        f.set(ELEC + ENER, q="GOOD")
        f.vals["ac.power.active.total"][0] = -12.5
        f.vals["ac.power.factor.total"][0] = -0.972
        f.set(["ac.energy.export.active"], value=lambda v: round(v + 0.04, 2), q="GOOD")

    def timeout(f, s):
        if s >= 10:
            f.set(ELEC, q="STALE", fresh=False)
        if s >= 20:
            f.set(ENER, q="STALE", fresh=False)

    def recover(f, s):
        normal(f, s)

    def oor(f, s):
        normal(f, s)
        f.vals["ac.frequency"][0] = 30.0
        f.vals["ac.frequency"][1] = "OUT_OF_RANGE"

    def exc(f, s):
        f.set(ELEC, q="GOOD")
        for k in ELEC:
            f.vals[k][0] = NORMAL[k]
        if s == 10:
            f.emit("EVT_MODBUS_EXCEPTION slot=0 GRID_INCOMER_01 grp1 code=0x02: group INVALID", "W")
        if s >= 10:
            f.set(ENER, q="INVALID", fresh=False)

    def reset(f, s):
        normal(f, s)
        if s == 10:
            f.emit("EVT_COUNTER_DISCONTINUITY slot=0 GRID_INCOMER_01 param=ac.energy.import.active: "
                   "counter decreased or reset; no delta, new baseline", "W")
            f.vals["ac.energy.import.active"][0] = 0.02
        elif s > 10:
            f.vals["ac.energy.import.active"][0] = round(f.vals["ac.energy.import.active"][0] + 0.02, 2)

    scripts = {"normal": normal, "import": imp, "export": exp, "timeout": timeout, "recover": recover,
               "out_of_range": oor, "exception_energy": exc, "counter_reset": reset, "final": normal}
    for name, mode, dur in run.SUITE_D6:
        fw.phase(name, mode, dur, o.get(name, scripts[name]))
        if name == "exception_energy":
            fw.emit("STACK task=ems_acquisition allocated=6144B current_free=2900B min_ever_free=2712B")
    fw.emit("STATS sched: submitted=60 skipped=0 rejected=0 guard_expired=0 fb_unmatched=0 fb_late=0 "
            "health=0x00 | acq: results=60 ok=50 failed=10 routing_errors=0 stale=13", tag="myems_app")
    return fw


def analyse(fw, crc=CRC):
    return run.analyse_d6(fw.lines, fw.phases, crc)


class RunnerAnalysisTests(unittest.TestCase):

    def assertPass(self, rep):
        self.assertEqual(0, rep.failures, "\n".join(rep.rows))

    def assertFails(self, rep, test_id):
        fails = [r for r in rep.rows if r.startswith("FAIL") and f" {test_id} " in r + " "]
        self.assertTrue(fails, f"expected a FAIL for {test_id}:\n" + "\n".join(rep.rows))

    def test_designed_behaviour_passes(self):
        self.assertPass(analyse(good_run()))

    def test_parse_handles_esp_prefix_and_colours(self):
        fw = good_run()
        dumps = run.parse_console(fw.lines)
        self.assertGreater(len(dumps), 40)
        self.assertEqual(14, len(dumps[0].vals))
        self.assertEqual("V", dumps[0].vals["ac.voltage.l1_n"].unit)

    def test_wrong_value_fails_q002(self):
        def bad(f, s):
            f.set(ALL, q="GOOD")
            f.vals["ac.voltage.l2_n"][0] = 23.1          # scale error
        self.assertFails(analyse(good_run({"normal": bad})), "Q-002")

    def test_missing_parameter_fails_umd001(self):
        fw = good_run()
        fw.lines = [(t, s) for t, s in fw.lines if "ac.current.l3=" not in s]
        self.assertFails(analyse(fw), "UMD-001")

    def test_import_counter_not_increasing_fails(self):
        def flat(f, s):
            f.set(ALL, q="GOOD")
        self.assertFails(analyse(good_run({"import": flat})), "UMD-006")

    def test_export_sign_wrong_fails(self):
        def wrong(f, s):
            f.set(ELEC + ENER, q="GOOD")
            f.vals["ac.power.active.total"][0] = 12.5        # sign wrong, PF right
            f.vals["ac.power.factor.total"][0] = -0.972
            f.set(["ac.energy.export.active"], value=lambda v: v + 0.04, q="GOOD")
        self.assertFails(analyse(good_run({"export": wrong})), "UMD-007")

    def test_still_good_during_loss_fails_q003_q009(self):
        def stays_good(f, s):
            pass                                        # values stay GOOD: freshness/failure not applied
        rep = analyse(good_run({"timeout": stays_good}))
        self.assertFails(rep, "Q-003")
        self.assertFails(rep, "Q-009")

    def test_zero_published_on_loss_fails_q003(self):
        def zero(f, s):
            if s >= 10:
                f.set(ELEC + ENER, value=0.0, q="STALE", fresh=False)
        self.assertFails(analyse(good_run({"timeout": zero})), "Q-003")

    def test_no_recovery_fails_q007(self):
        def stuck(f, s):
            pass
        self.assertFails(analyse(good_run({"recover": stuck})), "Q-007")

    def test_out_of_range_published_good_fails_q006(self):
        def good30(f, s):
            f.set(ALL, q="GOOD")
            f.vals["ac.frequency"][0] = 30.0
        self.assertFails(analyse(good_run({"out_of_range": good30})), "Q-006")

    def test_exception_without_event_fails_q005(self):
        def silent(f, s):
            f.set(ELEC, q="GOOD")
            if s >= 10:
                f.set(ENER, q="INVALID", fresh=False)
        self.assertFails(analyse(good_run({"exception_energy": silent})), "Q-005")

    def test_two_discontinuities_fail_eng008(self):
        def twice(f, s):
            f.set(ALL, q="GOOD")
            if s in (10, 15):
                f.emit("EVT_COUNTER_DISCONTINUITY slot=0 X param=ac.energy.import.active: reset", "W")
                f.vals["ac.energy.import.active"][0] = 0.02
        self.assertFails(analyse(good_run({"counter_reset": twice})), "ENG-008")

    def test_reboot_fails_stability(self):
        fw = good_run()
        fw.emit("Guru Meditation Error: Core  0 panic'ed (LoadProhibited)", "E", "cpu")
        self.assertFails(analyse(fw), "stability")

    def test_runner_reset_before_boot_is_not_a_crash(self):
        fw = good_run()
        fw.lines.insert(0, (99.0, "rst:0x15 (USB_UART_CHIP_RESET),boot:0x39 (SPI_FAST_FLASH_BOOT)"))
        self.assertPass(analyse(fw))

    def test_reset_after_boot_fails_stability(self):
        fw = good_run()
        fw.lines.insert(0, (99.0, "rst:0x15 (USB_UART_CHIP_RESET),boot:0x39 (SPI_FAST_FLASH_BOOT)"))
        fw.lines.insert(len(fw.lines) // 2, (fw.lines[len(fw.lines) // 2][0],
                                             "rst:0xc (RTC_SW_CPU_RST),boot:0x39 (SPI_FAST_FLASH_BOOT)"))
        self.assertFails(analyse(fw), "stability")

    def test_second_boot_banner_fails_stability(self):
        fw = good_run()
        fw.lines.append((fw.lines[-1][0] + 1, esp(1, "BUILD_MODE=PRODUCTION CONFIG_TARGET=sim", tag="myems_app")))
        self.assertFails(analyse(fw), "stability")

    def test_routing_errors_fail(self):
        fw = good_run()
        fw.lines = [(t, s.replace("routing_errors=0", "routing_errors=3")) for t, s in fw.lines]
        self.assertFails(analyse(fw), "RTE")

    def test_crc_mismatch_fails(self):
        self.assertFails(analyse(good_run(), crc="0x12345678"), "config")

    def test_production_target_fails_boot(self):
        fw = FakeFirmware()
        fw.boot(target="prod")
        self.assertFails(run.analyse_d6(fw.lines, [], CRC), "boot")

    def test_low_stack_fails(self):
        fw = good_run()
        fw.lines = [(t, s.replace("min_ever_free=2712B", "min_ever_free=700B")) for t, s in fw.lines]
        self.assertFails(analyse(fw), "stack")

    def test_saved_run_can_be_reanalysed(self):
        fw = good_run()
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / "console.log").write_text("".join(f"{t:.3f} {s}\n" for t, s in fw.lines), encoding="utf-8")
            (out / "phases.json").write_text(json.dumps(
                {"crc": CRC, "phases": [p.__dict__ for p in fw.phases]}), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as buf:
                rc = run.main(["--analyse", str(out)])
            self.assertEqual(0, rc, buf.getvalue())
            self.assertIn("RESULT: PASS", (out / "report.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
