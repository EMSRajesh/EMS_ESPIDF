"""DP-WP5 simulator tests: AC regression (SIM5-001) and layout format (SIM5-010).

The DC meter tests (SIM5-002..SIM5-009) are added with the DC map in milestone M3.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import myems_ac_meter_sim as sim_mod  # noqa: E402
import sim_trace  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
