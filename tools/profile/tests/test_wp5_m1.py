"""DP-WP5 milestone M1 tests for the profile tool and the dictionary.

Test IDs are those of the DP-WP5 Verification & Test Specification v1.0:
U-001..U-008 (dictionary rules), GEN5-001..GEN5-004 and GEN5-010 (generation
with the generalised tool), the M1 part of rules V-16..V-18, and NOC-002
(no dictionary parameter names in tool or simulator code).

Run from the project root with
    python -m unittest discover -s tools/profile/tests -v
"""
from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import myems_profile_tool as tool  # noqa: E402
import umd_gen  # noqa: E402
from test_profile_tool import CONFIG, LAB, LAB2, CT200, DECODER, MAP_FILE, PROFILE_FILE, TempConfig, mapping, run  # noqa: E402

ROOT = tool.PROJECT_ROOT
UMD_FILE = "umd/umd_v1.json"
RELEASED = CONFIG / "umd" / "released" / "umd_1.0.0.json"


def param(d: dict, pid: str) -> dict:
    return next(p for p in d["parameters"] if p["id"] == pid)


class UmdRuleTests(unittest.TestCase):
    """U-001..U-008: the dictionary is checked before any profile."""

    def assertUmdRejected(self, edit, *needles: str) -> None:
        with TempConfig() as t:
            t.edit(UMD_FILE, edit)
            model, rep = t.check()
            self.assertIsNone(model)
            text = "\n".join(str(e) for e in rep.errors)
            for n in needles:
                self.assertIn(n, text)

    def test_U001_umd_check_passes(self):
        rc, out, err = run("umd-check")
        self.assertEqual(0, rc, err)
        self.assertIn("UMD 1.1.0", out)

    def test_U002_duplicate_runtime_id(self):
        self.assertUmdRejected(lambda d: param(d, "ac.voltage.l2_n").update(runtime_id="0x0101"),
                               "already used by")

    def test_U003_runtime_id_outside_domain(self):
        self.assertUmdRejected(lambda d: d["parameters"].append(
            {"id": "dc.test_value", "runtime_id": "0x0105", "unit": "V", "value_type": "float",
             "description": "test"}), "outside domain 'dc'")

    def test_U004_unit_not_in_list(self):
        self.assertUmdRejected(lambda d: param(d, "ac.voltage.l1_n").update(unit="mV"), "not in units list")

    def test_U005_unknown_role(self):
        self.assertUmdRejected(lambda d: param(d, "ac.power.active.total").update(role="power.real"),
                               "U-04", "power.real")

    def test_U006_capability_lists_missing_parameter(self):
        self.assertUmdRejected(lambda d: d["capabilities"]["frequency"].append("ac.frequency.l1"),
                               "U-04", "ac.frequency.l1")

    def test_U007_retired_runtime_id_reused(self):
        def edit(d):
            d["retired"].append({"id": "ac.old_value", "runtime_id": "0x0150"})
            d["parameters"].append({"id": "ac.new_value", "runtime_id": "0x0150", "unit": "V",
                                    "value_type": "float", "description": "test"})
        self.assertUmdRejected(edit, "retired")

    def test_U008_released_parameter_changed(self):
        for attr, value, needle in (("unit", "kVA", "unit changed"), ("runtime_id", "0x0125", "runtime_id changed"),
                                    ("value_type", "counter", "value_type changed")):
            with self.subTest(attr=attr):
                # change the reference (released) copy, so the working dictionary itself stays valid
                with TempConfig() as t:
                    rel = "umd/released/umd_1.0.0.json"
                    t.edit(rel, lambda d: param(d, "ac.power.active.total").update({attr: value}))
                    model, rep = t.check()
                    self.assertIsNone(model)
                    self.assertIn("U-06", "\n".join(str(e) for e in rep.errors))
                    self.assertIn(needle, "\n".join(str(e) for e in rep.errors))

    def test_U008_released_parameter_removed(self):
        def edit(d):
            d["parameters"] = [p for p in d["parameters"] if p["id"] != "ac.frequency"]
            d["capabilities"]["frequency"] = []
        with TempConfig() as t:
            t.edit(UMD_FILE, edit)
            # also drop it from the map so only the dictionary rule can fire
            t.edit(MAP_FILE, lambda m: m.update(mappings=[x for x in m["mappings"] if x["param"] != "ac.frequency"]))
            model, rep = t.check()
            self.assertIsNone(model)
            self.assertIn("U-06", "\n".join(str(e) for e in rep.errors))

    def test_released_copy_is_the_wp4_dictionary(self):
        rel = json.loads(RELEASED.read_text(encoding="utf-8"))
        self.assertEqual("1.0.0", rel["umd_version"])
        self.assertEqual("myems.umd/1.0", rel["schema"])
        self.assertEqual(14, len(rel["parameters"]))

    def test_roles_and_capabilities_need_schema_1_1(self):
        d = json.loads((CONFIG / UMD_FILE).read_text(encoding="utf-8"))
        d["schema"] = "myems.umd/1.0"
        with self.assertRaises(umd_gen.UmdError):
            umd_gen.validate(d)

    def test_profile_tool_needs_dictionary_1_1(self):
        with TempConfig() as t:
            (t.dir / UMD_FILE).write_text(RELEASED.read_text(encoding="utf-8"), encoding="utf-8")
            model, rep = t.check()
            self.assertIsNone(model)
            self.assertIn("myems.umd/1.1", "\n".join(str(e) for e in rep.errors))


class Gen5Tests(unittest.TestCase):

    def test_GEN5_001_wp4_sites_generate_unchanged_tables(self):
        """CRC32 covers every table value the firmware uses; it must equal the D9 values."""
        d9_crc = {LAB: 0x4B8B5924, LAB2: 0xE3FF2DF7, CT200: 0x5A2CC286, DECODER: 0x5A334BC5}
        for site, crc in d9_crc.items():
            with self.subTest(site=site.name):
                model, rep = tool.build_model(site, "sim")
                self.assertIsNotNone(model, [str(e) for e in rep.errors])
                self.assertEqual(crc, tool.config_crc32(model))

    def test_GEN5_002_schema_1_1_files_still_accepted(self):
        for rel in (PROFILE_FILE, MAP_FILE):
            data = json.loads((CONFIG / rel).read_text(encoding="utf-8"))
            self.assertTrue(data["schema"].endswith("/1.1"), rel)
        model, _ = tool.build_model(LAB, "sim")
        self.assertIsNotNone(model)

    def test_GEN5_002_schema_1_2_accepted(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d.update(schema="myems.profile/1.2"))
            t.edit(MAP_FILE, lambda d: d.update(schema="myems.regmap/1.2"))
            model, rep = t.check()
            self.assertIsNotNone(model, [str(e) for e in rep.errors])

    def test_GEN5_003_newer_minor_rejected_per_kind(self):
        for rel, schema, kind in ((PROFILE_FILE, "myems.profile/1.3", "myems.profile"),
                                  (MAP_FILE, "myems.regmap/1.3", "myems.regmap"),
                                  ("site/lab_bench.site.json", "myems.site/1.2", "myems.site")):
            with self.subTest(schema=schema), TempConfig() as t:
                t.edit(rel, lambda d: d.update(schema=schema))
                model, rep = t.check()
                self.assertIsNone(model)
                text = "\n".join(str(e) for e in rep.errors)
                self.assertIn(f"unsupported {kind} version", text)
                self.assertIn("this tool supports 1.0-", text)

    def test_GEN5_004_counter_plausibility_unchanged(self):
        model, _ = tool.build_model(LAB, "sim")
        self.assertEqual(1000.0, tool.counter_max_rate(model, model.profiles[0]))
        _, c = tool.render(model)
        self.assertIn(".counter_max_rate_per_hour = 1000.0", c)

    def test_GEN5_004_counter_plausibility_follows_the_role(self):
        """Move the role to another parameter: the limit follows it (no parameter name in the tool)."""
        def edit(d):
            param(d, "ac.power.active.total").pop("role")
            param(d, "ac.power.reactive.total")["role"] = "power.active"
        with TempConfig() as t:
            t.edit(UMD_FILE, edit)
            t.edit(MAP_FILE, lambda m: mapping(m, "ac.power.reactive.total").update(min=-777.0, max=500.0))
            model, rep = t.check()
            self.assertIsNotNone(model, [str(e) for e in rep.errors])
            self.assertEqual(777.0, tool.counter_max_rate(model, model.profiles[0]))

    def test_GEN5_010_generated_tables_differ_only_in_version_lines(self):
        """Regenerate into a temp folder and compare with the committed files, ignoring version lines."""
        allowed = re.compile(r"(Generator:|Inputs|tool_version|umd_version|inputs_sha256|inputs_summary)")
        for site, out in ((LAB, ROOT / "main" / "generated"),
                          (LAB2, ROOT / "main" / "generated_sites" / "lab_bench_2dev")):
            with self.subTest(site=site.name), tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(0, run("generate", "--site", str(site), "--target", "sim", "--out-dir", tmp)[0])
                for name in ("ems_config_gen.c",):
                    new = [l for l in (Path(tmp) / name).read_text().splitlines() if not allowed.search(l)]
                    old = [l for l in (out / name).read_text().splitlines() if not allowed.search(l)]
                    # Regenerate comment names the out dir; drop it too
                    new = [l for l in new if "Regenerate:" not in l]
                    old = [l for l in old if "Regenerate:" not in l]
                    self.assertEqual(old, new)


class RuleV16toV18Tests(unittest.TestCase):

    def check(self, t: TempConfig) -> tuple[object, str]:
        model, rep = t.check()
        return model, "\n".join(str(e) for e in rep.errors)

    def test_V16_not_applicable_with_power_factor_mapping(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: (d.update(schema="myems.profile/1.2"),
                                            d["conventions"].update(pf_sign="NOT_APPLICABLE")))
            model, text = self.check(t)
            self.assertIsNone(model)
            self.assertIn("[V-16]", text)

    def test_V16_not_applicable_needs_profile_1_2(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["conventions"].update(pf_sign="NOT_APPLICABLE"))
            model, text = self.check(t)
            self.assertIsNone(model)
            self.assertIn("conventions.pf_sign", text)

    def test_conventions_values_checked(self):
        for key, value in (("power_sign", "EXPORT_POSITIVE"), ("pf_sign", "SIGNED")):
            with self.subTest(key=key), TempConfig() as t:
                t.edit(PROFILE_FILE, lambda d: d["conventions"].update({key: value}))
                model, text = self.check(t)
                self.assertIsNone(model)
                self.assertIn(f"conventions.{key}", text)

    def test_V17_two_active_power_roles(self):
        with TempConfig() as t:
            t.edit(UMD_FILE, lambda d: param(d, "ac.power.reactive.total").update(role="power.active"))
            model, text = self.check(t)
            self.assertIsNone(model)
            self.assertIn("[V-17]", text)

    def test_V17_rollover_needs_active_power(self):
        with TempConfig() as t:
            t.edit(UMD_FILE, lambda d: param(d, "ac.power.active.total").pop("role"))
            t.edit(MAP_FILE, lambda m: mapping(m, "ac.energy.import.active")["flags"].append("ROLLOVER"))
            model, text = self.check(t)
            self.assertIsNone(model)
            self.assertIn("[V-17]", text)
            self.assertIn("ROLLOVER", text)

    def test_V18_sim_out_of_range(self):
        cases = ((30.0, None), (50.0, "inside the range"), (1000.0, "cannot be encoded as UINT16"))
        for value, error in cases:
            with self.subTest(value=value), TempConfig() as t:
                t.edit(MAP_FILE, lambda m: (m.update(schema="myems.regmap/1.2"),
                                            mapping(m, "ac.frequency").update(sim_out_of_range=value)))
                model, text = self.check(t)
                if error is None:
                    self.assertIsNotNone(model, text)
                    self.assertEqual(30.0, next(x for x in model.maps[0].mappings
                                                if x.param == "ac.frequency").sim_out_of_range)
                else:
                    self.assertIsNone(model)
                    self.assertIn("[V-18]", text)
                    self.assertIn(error, text)

    def test_V18_field_needs_regmap_1_2(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda m: mapping(m, "ac.frequency").update(sim_out_of_range=30.0))
            model, text = self.check(t)
            self.assertIsNone(model)
            self.assertIn("sim_out_of_range: unknown field", text)

    def test_sim_hint_is_not_in_the_crc(self):
        base, _ = tool.build_model(LAB, "sim")
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda m: (m.update(schema="myems.regmap/1.2"),
                                        mapping(m, "ac.frequency").update(sim_out_of_range=30.0)))
            model, text = self.check(t)
            self.assertIsNotNone(model, text)
            self.assertEqual(tool.config_crc32(base), tool.config_crc32(model))
            regs = tool.export_sim(model)["devices"][0]["registers"]
            self.assertEqual(30.0, next(r for r in regs if r["param"] == "ac.frequency")["sim_out_of_range"])


class NocTests(unittest.TestCase):

    def test_NOC_002_no_parameter_names_in_tool_or_simulator(self):
        """Dictionary parameter IDs may appear only on LEGACY_WP4 lines or in usage examples ('e.g.')."""
        umd = json.loads((CONFIG / UMD_FILE).read_text(encoding="utf-8"))
        names = [p["id"] for p in umd["parameters"]]
        pat = re.compile("|".join(re.escape(n) for n in sorted(names, key=len, reverse=True)))
        sources = sorted((ROOT / "tools" / "profile").glob("*.py")) + [ROOT / "tools" / "wp4" / "myems_ac_meter_sim.py"]
        bad = []
        for src in sources:
            for n, line in enumerate(src.read_text(encoding="utf-8").splitlines(), 1):
                if pat.search(line) and "LEGACY_WP4" not in line and "e.g." not in line:
                    bad.append(f"{src.relative_to(ROOT)}:{n}: {line.strip()}")
        self.assertEqual([], bad)


if __name__ == "__main__":
    unittest.main()
