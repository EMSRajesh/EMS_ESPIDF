"""DP-WP5 simulator tests: AC regression (SIM5-001) and layout format (SIM5-010).

SIM5-002..SIM5-009: the DC meter (map MYEMS_SIM_DCMETER, Test Spec 2.1)
served by the same role-based value model, without DC-specific code.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import myems_ac_meter_sim as sim_mod  # noqa: E402
import sim_trace  # noqa: E402
from test_ac_meter_sim import Clock, read_req, words  # noqa: E402

GOLDEN = HERE / "data" / "sim_ac_golden_wp4.json"
SITE_DIR = sim_mod.PROJECT_ROOT / "config" / "site"


class Sim5Tests(unittest.TestCase):

    def test_SIM5_001_ac_output_identical_to_wp4(self):
        """Role-based value model reproduces the D9 simulator byte for byte (every mode, two seeds)."""
        golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["digests"]
        now = sim_trace.all_digests(sim_mod, SITE_DIR)
        self.assertEqual(sorted(golden), sorted(now))
        diff = [k for k in golden if golden[k] != now[k]]
        self.assertEqual([], diff, "scenarios whose replies changed since DP-WP4 D9")

    def test_SIM5_010_old_layout_format_refused(self):
        layout = sim_mod.load_layout(SITE_DIR / "lab_bench.site.json")
        self.assertEqual(sim_mod.LAYOUT_FORMAT, layout["format"])
        layout["format"] = "myems.simlayout/1.0"
        with self.assertRaises(SystemExit) as cm:
            sim_mod.check_layout(layout)
        self.assertIn("myems.simlayout/1.1", str(cm.exception))
        self.assertIn("export-sim", str(cm.exception))

    def test_layout_carries_roles_and_pf_sign(self):
        layout = sim_mod.load_layout(SITE_DIR / "lab_bench.site.json")
        dev = layout["devices"][0]
        self.assertEqual("SIGNED_BY_ACTIVE_POWER", dev["pf_sign"])
        self.assertEqual("AC_METER", dev["device_type"])
        roles = {r["role"] for r in dev["registers"]}
        self.assertTrue({"power.active", "energy.import", "energy.export", "frequency"} <= roles)



# ===================================================================== DC ===
DC_LAYOUT = sim_mod.load_layout(SITE_DIR / "lab_dc_bench.site.json")
MIXED_LAYOUT = sim_mod.load_layout(SITE_DIR / "lab_mixed.site.json")
DC = 3                                       # slave ID of the DC meter
# Test Spec 2.1, NORMAL (import case)
MEAS_2_1 = "4448 0000 E848 0001 0000 42C8".split()
ENERGY_2_1 = "D090 0003 0000 0000".split()


def dc_sim(mode: str | None = None, layout: dict = DC_LAYOUT, seed: int = 1):
    clk = Clock(1000.0)
    s = sim_mod.Simulator(layout, seed=seed, clock=clk)
    if mode:
        s.set_mode(str(DC), sim_mod.parse_mode(mode))
    return s, clk


def meas(s) -> list[str]:
    return words(s.handle(read_req(DC, 0, 6, 4)).data)


def energy(s) -> list[str]:
    return words(s.handle(read_req(DC, 100, 4, 4)).data)


def f32(hi: str, lo: str) -> float:
    return struct.unpack(">f", bytes.fromhex(hi + lo))[0]


def i32(hi: str, lo: str) -> int:
    return struct.unpack(">i", bytes.fromhex(hi + lo))[0]


def decode_meas(ws: list[str]) -> tuple[float, float, float]:
    """(V, A, kW): FLOAT32 ABCD, INT32 CDAB x 0.001, FLOAT32 CDAB."""
    return f32(ws[0], ws[1]), i32(ws[3], ws[2]) / 1000.0, f32(ws[5], ws[4])


def counters(ws: list[str]) -> tuple[int, int]:
    """Raw import and export counters (UINT32 CDAB)."""
    return int(ws[1] + ws[0], 16), int(ws[3] + ws[2], 16)


class DcSimTests(unittest.TestCase):

    def test_SIM5_002_normal_image_equals_vectors_2_1(self):
        s, clk = dc_sim()
        self.assertEqual(MEAS_2_1, meas(s))
        self.assertEqual(ENERGY_2_1, energy(s))
        self.assertEqual(["0000"], words(s.handle(read_req(DC, 200, 1, 3)).data))
        clk.t += 3600
        self.assertEqual(MEAS_2_1, meas(s))                  # NORMAL is static
        self.assertEqual(ENERGY_2_1, energy(s))

    def test_SIM5_003_function_codes_per_group(self):
        s, _ = dc_sim()
        for fc, start, count in ((3, 0, 6), (4, 200, 1), (3, 100, 4)):
            with self.subTest(fc=fc, start=start):
                r = s.handle(read_req(DC, start, count, fc)).data
                self.assertEqual(bytes((DC, fc | 0x80, 0x02)), r[:3])
        r = s.handle(sim_mod.with_crc(bytes((DC, 6, 0, 0, 0, 1)))).data   # write single register
        self.assertEqual(bytes((DC, 0x86, 0x01)), r[:3])
        self.assertEqual(MEAS_2_1, meas(s))

    def test_SIM5_004_import(self):
        s, clk = dc_sim("IMPORT")
        v, a, kw = decode_meas(meas(s))
        self.assertEqual((800.0, 125.0, 100.0), (v, a, kw))
        clk.t += 36.0                                        # 100 kW x 36 s = 1.00 kWh
        self.assertEqual((250100, 0), counters(energy(s)))
        clk.t += 0.36
        self.assertEqual((250101, 0), counters(energy(s)))

    def test_SIM5_005_export(self):
        s, clk = dc_sim("EXPORT")
        ws = meas(s)
        self.assertEqual("4448 0000 17B8 FFFE 0000 C2C8".split(), ws)
        self.assertEqual((800.0, -125.0, -100.0), decode_meas(ws))
        clk.t += 36.0
        self.assertEqual((250000, 100), counters(energy(s)))  # import frozen, export +1.00 kWh

    def test_SIM5_006_load_change(self):
        def run(seed):
            s, clk = dc_sim("LOAD_CHANGE:10", seed=seed)
            out = []
            for _ in range(8):
                out.append(decode_meas(meas(s)))
                clk.t += 10.0
            return out
        a, b, c = run(1), run(1), run(2)
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        self.assertGreater(len(set(a)), 1, "load must change between steps")
        for v, amps, kw in a:
            self.assertEqual(800.0, v)                       # voltage constant
            self.assertGreater(kw, 0.0)
            self.assertAlmostEqual(kw / 100.0, amps / 125.0, places=4)   # P and I scaled by one factor

    def test_SIM5_007_out_of_range(self):
        s, _ = dc_sim("OUT_OF_RANGE")
        ws = meas(s)
        self.assertEqual(["44E1", "0000"], ws[:2])           # 1800.0 V (sim_out_of_range)
        self.assertEqual(MEAS_2_1[2:], ws[2:])
        self.assertEqual(ENERGY_2_1, energy(s))

    def test_SIM5_008_counter_reset(self):
        s, clk = dc_sim()
        self.assertEqual((250000, 0), counters(energy(s)))
        s.set_mode(str(DC), sim_mod.parse_mode("COUNTER_RESET"))
        self.assertEqual((0, 0), counters(energy(s)))
        clk.t += 36.0
        self.assertEqual((100, 0), counters(energy(s)))

    def test_SIM5_009_mixed_signs_independent(self):
        s, clk = dc_sim(layout=MIXED_LAYOUT)
        s.set_mode("1", sim_mod.parse_mode("EXPORT"))
        s.set_mode(str(DC), sim_mod.parse_mode("IMPORT"))
        ac = words(s.handle(read_req(1, 0, 14)).data)
        self.assertEqual(-12500, i32(ac[6], ac[7]))          # AC -12.5 kW
        self.assertEqual(100.0, decode_meas(meas(s))[2])     # DC +100 kW
        clk.t += 36.0
        ac_e = words(s.handle(read_req(1, 14, 4)).data)
        self.assertEqual(10025, int(ac_e[0] + ac_e[1], 16))  # AC import frozen
        self.assertGreater(int(ac_e[2] + ac_e[3], 16), 0)    # AC export rises
        self.assertEqual((250100, 0), counters(energy(s)))   # DC import rises

    def test_dc_fault_modes_are_role_independent(self):
        s, _ = dc_sim("TIMEOUT")
        self.assertIsNone(s.handle(read_req(DC, 0, 6, 4)).data)
        s.set_mode(str(DC), sim_mod.parse_mode("EXCEPTION:02@energy"))
        self.assertEqual(MEAS_2_1, meas(s))
        self.assertEqual(bytes((DC, 0x84, 0x02)), s.handle(read_req(DC, 100, 4, 4)).data[:3])


if __name__ == "__main__":
    unittest.main()
