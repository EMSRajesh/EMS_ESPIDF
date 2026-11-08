"""Tests for tools/profile/umd_gen.py (DP-WP4 D1).

Run from the project root with either
    python -m pytest tools/profile/tests
or (no pytest needed)
    python -m unittest discover -s tools/profile/tests -v
"""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import umd_gen  # noqa: E402

UMD_PATH = umd_gen.DEFAULT_UMD

# Design v1.1 Section 6.1
EXPECTED = {
    "dev.status.code": (0x0001, "-"),
    "ac.voltage.l1_n": (0x0101, "V"),
    "ac.voltage.l2_n": (0x0102, "V"),
    "ac.voltage.l3_n": (0x0103, "V"),
    "ac.current.l1": (0x0111, "A"),
    "ac.current.l2": (0x0112, "A"),
    "ac.current.l3": (0x0113, "A"),
    "ac.power.active.total": (0x0121, "kW"),
    "ac.power.reactive.total": (0x0122, "kvar"),
    "ac.power.apparent.total": (0x0123, "kVA"),
    "ac.power.factor.total": (0x0124, "1"),
    "ac.frequency": (0x0130, "Hz"),
    "ac.energy.import.active": (0x0140, "kWh"),
    "ac.energy.export.active": (0x0141, "kWh"),
    # DP-WP5 Design v1.0 Section 4.2
    "dc.voltage": (0x0201, "V"),
    "dc.current": (0x0211, "A"),
    "dc.power": (0x0221, "kW"),
    "dc.energy.import": (0x0240, "kWh"),
    "dc.energy.export": (0x0241, "kWh"),
}


def load() -> dict:
    return json.loads(UMD_PATH.read_text(encoding="utf-8"))


class TestDictionaryContent(unittest.TestCase):
    def test_umd001_all_wp4_parameters_present_with_design_ids(self):
        params = {p.name: p for p in umd_gen.validate(load())}
        self.assertEqual(set(params), set(EXPECTED))
        for name, (rid, unit) in EXPECTED.items():
            self.assertEqual(params[name].runtime_id, rid, name)
            self.assertEqual(params[name].unit, unit, name)

    def test_counters_are_energy_only(self):
        counters = {p.name for p in umd_gen.validate(load()) if p.value_type == "counter"}
        self.assertEqual(counters, {"ac.energy.import.active", "ac.energy.export.active",
                                    "dc.energy.import", "dc.energy.export"})


class TestValidationRules(unittest.TestCase):
    def assertRejected(self, data: dict, fragment: str):
        with self.assertRaises(umd_gen.UmdError) as ctx:
            umd_gen.validate(data)
        self.assertIn(fragment, str(ctx.exception))

    def test_duplicate_runtime_id(self):
        d = load(); d["parameters"][2]["runtime_id"] = d["parameters"][1]["runtime_id"]
        self.assertRejected(d, "already used")

    def test_duplicate_name(self):
        d = load(); d["parameters"].append(copy.deepcopy(d["parameters"][1])); d["parameters"][-1]["runtime_id"] = "0x01EE"
        self.assertRejected(d, "duplicate id")

    def test_runtime_id_outside_domain(self):
        d = load(); d["parameters"][1]["runtime_id"] = "0x0201"
        self.assertRejected(d, "outside domain")

    def test_unknown_domain(self):
        d = load(); d["parameters"][1]["id"] = "xx.voltage.l1_n"
        self.assertRejected(d, "unknown domain")

    def test_unknown_unit(self):
        d = load(); d["parameters"][1]["unit"] = "mV"
        self.assertRejected(d, "not in units list")

    def test_bad_name_format(self):
        d = load(); d["parameters"][1]["id"] = "AC.Voltage"
        self.assertRejected(d, "lower-case dotted")

    def test_wrong_schema(self):
        d = load(); d["schema"] = "myems.umd/2.0"
        self.assertRejected(d, "unsupported schema")

    def test_retired_id_cannot_be_reused(self):
        d = load(); d["retired"].append({"id": "ac.old.thing", "runtime_id": "0x01EE"})
        d["parameters"].append({"id": "ac.new.thing", "runtime_id": "0x01EE", "unit": "V",
                                "value_type": "float", "description": "x"})
        self.assertRejected(d, "retired")

    def test_description_with_quote_rejected(self):
        d = load(); d["parameters"][1]["description"] = 'bad "quote"'
        self.assertRejected(d, "description")


class TestGeneration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.umd = self.dir / "umd.json"
        self.h = self.dir / "ems_param_ids.h"
        self.c = self.dir / "ems_param_table.c"
        self.umd.write_text(UMD_PATH.read_text(encoding="utf-8"), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def gen(self, *extra: str) -> int:
        return umd_gen.main(["--umd", str(self.umd), "--out-header", str(self.h),
                             "--out-source", str(self.c), *extra])

    def test_deterministic_output(self):
        self.assertEqual(self.gen(), 0)
        first = (self.h.read_bytes(), self.c.read_bytes())
        self.assertEqual(self.gen(), 0)
        self.assertEqual(first, (self.h.read_bytes(), self.c.read_bytes()))

    def test_check_mode(self):
        self.assertEqual(self.gen("--check"), 1)          # nothing generated yet
        self.assertEqual(self.gen(), 0)
        self.assertEqual(self.gen("--check"), 0)
        # Simulate a Git CRLF checkout. Write bytes: text mode on Windows would
        # turn "\r\n" into "\r\r\n".
        self.h.write_bytes(self.h.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertIn(b"\r\n", self.h.read_bytes())
        self.assertEqual(self.gen("--check"), 0)          # CRLF checkout is not a difference

    def test_umd002_adding_a_parameter_keeps_existing_ids(self):
        self.assertEqual(self.gen(), 0)
        before = [l for l in self.h.read_text().splitlines() if l.startswith("#define EMS_PARAM_") and "uint16_t" in l]
        d = json.loads(self.umd.read_text())
        d["parameters"].append({"id": "ac.voltage.l1_l2", "runtime_id": "0x0104", "unit": "V",
                                "value_type": "float", "description": "Line L1-L2 RMS voltage"})
        self.umd.write_text(json.dumps(d), encoding="utf-8")
        self.assertEqual(self.gen(), 0)
        after = self.h.read_text().splitlines()
        for line in before:
            self.assertIn(line, after)

    def test_umd002_changing_an_existing_id_is_refused(self):
        self.assertEqual(self.gen(), 0)
        d = json.loads(self.umd.read_text())
        d["parameters"][1]["runtime_id"] = "0x0109"
        self.umd.write_text(json.dumps(d), encoding="utf-8")
        self.assertEqual(self.gen(), 1)

    def test_umd002_deleting_without_retiring_is_refused(self):
        self.assertEqual(self.gen(), 0)
        d = json.loads(self.umd.read_text())
        removed = d["parameters"].pop(1)
        for plist in d.get("capabilities", {}).values():      # a removed parameter leaves its capability
            if removed["id"] in plist:
                plist.remove(removed["id"])
        self.umd.write_text(json.dumps(d), encoding="utf-8")
        self.assertEqual(self.gen(), 1)
        d["retired"].append({"id": removed["id"], "runtime_id": removed["runtime_id"]})
        self.umd.write_text(json.dumps(d), encoding="utf-8")
        self.assertEqual(self.gen(), 0)

    def test_committed_files_are_up_to_date(self):
        """The generated files in the repository must match umd_v1.json."""
        rc = umd_gen.main(["--check"])
        self.assertEqual(rc, 0, "run: python tools/profile/umd_gen.py")


if __name__ == "__main__":
    unittest.main()
