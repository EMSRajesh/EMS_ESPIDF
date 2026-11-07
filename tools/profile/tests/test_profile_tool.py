"""Tests for tools/profile/myems_profile_tool.py (DP-WP4 D3).

Test IDs are those of the DP-WP4 Verification & Test Specification v1.1:
GEN-001..GEN-009 (Section 6) and CFG-001..CFG-016 (Section 7, host part).

Run from the project root with either
    python -m pytest tools/profile/tests
or (no pytest needed)
    python -m unittest discover -s tools/profile/tests -v
"""
from __future__ import annotations

import contextlib
import io
import json
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import myems_profile_tool as tool  # noqa: E402

ROOT = tool.PROJECT_ROOT
CONFIG = tool.DEFAULT_CONFIG_DIR
LAB = CONFIG / "site" / "lab_bench.site.json"
LAB2 = CONFIG / "site" / "lab_bench_2dev.site.json"
CT200 = CONFIG / "site" / "lab_ratio_ct200.site.json"
DECODER = CONFIG / "site" / "lab_decoder_test.site.json"
MAP_FILE = "maps/myems_sim_acmeter_3ph.map.json"
PROFILE_FILE = "profiles/ac_meter_3ph_sim.profile.json"
DECODER_FIXTURE = ROOT / "test" / "host" / "generated" / "decoder_test"
ERROR_RE = re.compile(r"^ERROR \[V-\d\d\] [^:]+: .+: .+$")


def run(*argv: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = tool.main(list(argv))
    return rc, out.getvalue(), err.getvalue()


class TempConfig:
    """Copy of config/ in a temp folder that a test may edit."""

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name) / "config"
        shutil.copytree(CONFIG, self.dir)

    def __enter__(self) -> "TempConfig":
        return self

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()

    def load(self, rel: str) -> dict:
        return json.loads((self.dir / rel).read_text(encoding="utf-8"))

    def save(self, rel: str, data: dict) -> None:
        (self.dir / rel).write_text(json.dumps(data, indent=2), encoding="utf-8")

    def edit(self, rel: str, fn) -> None:
        data = self.load(rel)
        fn(data)
        self.save(rel, data)

    def site(self, name: str = "lab_bench") -> Path:
        return self.dir / "site" / f"{name}.site.json"

    def check(self, site: str = "lab_bench", target: str = "sim"):
        return tool.build_model(self.site(site), target, self.dir)


def mapping(data: dict, param: str) -> dict:
    return next(m for m in data["mappings"] if m["param"] == param)


# ====================================================================== GEN ===
class GenTests(unittest.TestCase):

    def test_GEN001_validate_lab_bench(self):
        rc, out, err = run("validate", "--site", str(LAB))
        self.assertEqual(0, rc, err)
        self.assertIn("1 device(s)", out)
        self.assertIn("3 poll groups, 14 mappings", out)
        self.assertIn("VALID", out)

    def test_all_shipped_sites_are_valid(self):
        for site in (LAB, LAB2, CT200, DECODER):
            with self.subTest(site=site.name):
                model, rep = tool.build_model(site, "sim")
                self.assertIsNotNone(model, [str(e) for e in rep.errors])

    def test_GEN002_generation_is_byte_identical(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(0, run("generate", "--site", str(LAB), "--target", "sim", "--out-dir", a)[0])
            self.assertEqual(0, run("generate", "--site", str(LAB), "--target", "sim", "--out-dir", b)[0])
            for name in ("ems_config_gen.h", "ems_config_gen.c"):
                self.assertEqual((Path(a) / name).read_bytes(), (Path(b) / name).read_bytes(), name)
                self.assertNotIn(b"\r\n", (Path(a) / name).read_bytes())

    def test_GEN003_build_info_lists_versions_tool_and_crc(self):
        model, _ = tool.build_model(LAB, "sim")
        _, c = tool.render(model)
        crc = f"0x{tool.config_crc32(model):08X}"
        self.assertIn(f'.tool_version = "{tool.TOOL_VERSION}"', c)
        self.assertIn('.umd_version = "1.0.0"', c)
        self.assertIn("profile:AC_METER_3PH_SIM@1.1.0", c)
        self.assertIn("map:MYEMS_SIM_ACMETER_3PH@1.1.0", c)
        self.assertIn(f".crc32 = {crc}u", c)
        self.assertIn(".classification = EMS_CLASS_SIMULATION_ONLY", c)
        self.assertIn("SIMULATION/TEST CONFIGURATION", c)
        self.assertRegex(c, r'\.inputs_sha256 = "[0-9a-f]{16}"')

    def test_GEN003_crc_changes_with_content_not_with_formatting(self):
        with TempConfig() as t:
            base = tool.config_crc32(t.check()[0])
            t.edit(MAP_FILE, lambda d: d.update(description="reformatted, same content"))
            self.assertEqual(base, tool.config_crc32(t.check()[0]))
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(max=66.0))
            self.assertNotEqual(base, tool.config_crc32(t.check()[0]))

    def test_GEN004_prod_target_rejects_simulation_site_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            rc, _, err = run("generate", "--site", str(LAB), "--target", "prod", "--out-dir", d)
            self.assertEqual(1, rc)
            self.assertIn("[V-13]", err)
            self.assertEqual([], list(Path(d).iterdir()))

    def test_GEN005_decoder_map_accepted_as_test_only(self):
        model, rep = tool.build_model(DECODER, "sim")
        self.assertIsNotNone(model, [str(e) for e in rep.errors])
        self.assertEqual("TEST_ONLY", model.classification)
        self.assertIn(".classification = EMS_CLASS_TEST_ONLY", tool.render(model)[1])

    def test_GEN006_budget_for_v11_defaults(self):
        model, _ = tool.build_model(LAB, "sim")
        res = tool.budget(model)
        self.assertEqual([], res.violations)
        self.assertAlmostEqual(3.1, res.devices[0]["t_wc_ms"] / 1000.0, delta=0.05)
        self.assertAlmostEqual(0.032, res.buses[0]["nominal_load"], delta=0.002)
        rc, out, _ = run("budget", "--site", str(LAB))
        self.assertEqual(0, rc)
        self.assertIn("RESULT: PASS", out)

    def test_GEN007_budget_with_v10_timing_is_rejected(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["comm_defaults"].update(timeout_ms=1000, max_retries=2))
            model, rep = t.check()
            self.assertIsNone(model)
            self.assertIn("V-14", rep.rules())
            msg = next(str(e) for e in rep.errors if e.rule == "V-14")
            self.assertIn("6.2", msg)
            self.assertIn("5.0 s", msg)

    def test_GEN008_export_sim_matches_firmware_tables(self):
        model, _ = tool.build_model(LAB2, "sim")
        _, c = tool.render(model)
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "sim.json"
            self.assertEqual(0, run("export-sim", "--site", str(LAB2), "--out", str(out))[0])
            sim = json.loads(out.read_text(encoding="utf-8"))
        # Parse the generated C tables independently of the tool's model.
        c_maps = re.findall(r"\.address = (\d+)u, .*?\n\s+\.group_index = (\d+)u, \.type = EMS_REG_(\w+), "
                            r"\.order = EMS_ORDER_(\w+)", c)
        c_groups = [(int(s), int(n), int(f)) for s, n, f in re.findall(
            r"\.start_address = (\d+)u, \.count = (\d+)u, \.function_code = (\d+)u", c)]
        self.assertEqual(f"0x{tool.config_crc32(model):08X}", sim["config_crc32"])
        self.assertEqual(2, len(sim["devices"]))
        for dev in sim["devices"]:
            regs = [(str(r["address"]), str(next(i for i, g in enumerate(dev["poll_groups"]) if g["name"] == r["group"])),
                     r["type"], r["order"]) for r in dev["registers"]]
            self.assertEqual(c_maps, regs)
            self.assertEqual(c_groups, [(g["start"], g["count"], g["function_code"]) for g in dev["poll_groups"]])
        self.assertEqual([1, 2], [d["slave_id"] for d in sim["devices"]])

    def test_GEN009_errors_name_file_field_and_rule(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(scale=0))
            rc, _, err = run("--config-dir", str(t.dir), "validate", "--site", str(t.site()))
            self.assertEqual(1, rc)
            lines = [ln for ln in err.splitlines() if ln.startswith("ERROR")]
            self.assertTrue(lines)
            for ln in lines:
                self.assertRegex(ln, ERROR_RE)
            self.assertIn(f"ERROR [V-08] {MAP_FILE}: mappings[10] (ac.frequency).scale:", lines[0])
            self.assertIn("nothing generated", err)

    def test_generated_files_in_repository_are_up_to_date(self):
        rc, _, err = run("generate", "--site", str(LAB), "--target", "sim", "--check")
        self.assertEqual(0, rc, err)
        rc, _, err = run("generate", "--site", str(DECODER), "--target", "sim", "--check",
                         "--out-dir", str(DECODER_FIXTURE))
        self.assertEqual(0, rc, err)

    def test_check_mode_detects_stale_files(self):
        with tempfile.TemporaryDirectory() as d:
            run("generate", "--site", str(LAB), "--target", "sim", "--out-dir", d)
            p = Path(d) / "ems_config_gen.c"
            p.write_bytes(p.read_bytes().replace(b"\n", b"\r\n"))       # line endings only: still OK
            self.assertEqual(0, run("generate", "--site", str(LAB), "--target", "sim", "--out-dir", d, "--check")[0])
            p.write_text(p.read_text(encoding="utf-8").replace(".max = 65.0", ".max = 66.0"), encoding="utf-8")
            self.assertEqual(1, run("generate", "--site", str(LAB), "--target", "sim", "--out-dir", d, "--check")[0])

    def test_c_double_literals_round_trip(self):
        for x in (0.1, 0.01, 0.001, 1.0, -1000.0, 43000000.0, 1e9, 5e-324, 1.7976931348623157e308):
            lit = tool._c_double(x)
            self.assertEqual(x, float(lit))
            self.assertRegex(lit, r"[.eE]")

    def test_canonical_bytes_layout(self):
        model, _ = tool.build_model(LAB, "sim")
        n = len(tool.canonical_bytes(model))
        # header 6 + bus 10 + map hdr 4 + 3 groups x 17 + 14 mappings x 44 + profile 11 + device 30
        self.assertEqual(6 + 10 + 4 + 3 * 17 + 14 * 44 + 11 + 30, n)


# ====================================================================== CFG ===
class CfgTests(unittest.TestCase):

    def assertRejected(self, t: TempConfig, rule: str, site: str = "lab_bench", target: str = "sim"):
        model, rep = t.check(site, target)
        self.assertIsNone(model)
        self.assertIn(rule, rep.rules(), [str(e) for e in rep.errors])
        return rep

    def test_CFG001_valid_configuration(self):
        with TempConfig() as t:
            model, rep = t.check()
            self.assertIsNotNone(model, [str(e) for e in rep.errors])
            self.assertEqual(1, len(model.devices))

    def test_CFG002_missing_profile_id(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d.pop("profile_id"))
            self.assertRejected(t, "V-01")

    def test_CFG003_unsupported_schema_major(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d.update(schema="myems.profile/2.0"))
            rep = self.assertRejected(t, "V-01")
            self.assertIn("unsupported", " ".join(str(e) for e in rep.errors))

    def test_CFG004_map_version_mismatch(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["register_map_ref"].update(map_version="1.0.0"))
            rep = self.assertRejected(t, "V-03")
            self.assertIn("available: 1.1.0", " ".join(str(e) for e in rep.errors))

    def test_CFG005_unknown_parameter(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.voltage.l3_n").update(param="ac.voltage.l4_n"))
            self.assertRejected(t, "V-04")

    def test_CFG006_duplicate_mapping_id(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.voltage.l2_n").update(mapping_id=1))
            self.assertRejected(t, "V-02")

    def test_CFG007_write_function_code(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: d["poll_groups"][0].update(function_code=6))
            self.assertRejected(t, "V-07")

    def test_CFG008_int32_without_second_register(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "dev.status.code").update(type="INT32", order="ABCD"))
            rep = self.assertRejected(t, "V-06")
            self.assertIn("18..19", " ".join(str(e) for e in rep.errors))

    def test_CFG008b_order_not_allowed_for_type(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(order="CDAB"))
            self.assertRejected(t, "V-05")

    def test_CFG009_scale_zero(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(scale=0))
            self.assertRejected(t, "V-08")

    def test_CFG009_offset_nan(self):
        with TempConfig() as t:
            rel = MAP_FILE
            text = (t.dir / rel).read_text(encoding="utf-8")
            data = json.loads(text)
            mapping(data, "ac.frequency")["offset"] = float("nan")
            (t.dir / rel).write_text(json.dumps(data), encoding="utf-8")   # writes NaN token
            self.assertRejected(t, "V-08")

    def test_CFG010_wrong_unit(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.voltage.l1_n").update(unit="mV"))
            rep = self.assertRejected(t, "V-04")
            self.assertIn("canonical unit 'V'", " ".join(str(e) for e in rep.errors))

    def test_CFG011_undefined_group(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(group="fast"))
            self.assertRejected(t, "V-06")

    def test_CFG012_simulation_map_with_prod_target(self):
        with TempConfig() as t:
            # Profile claims PRODUCTION; the map is still SIMULATION_ONLY.
            t.edit(PROFILE_FILE, lambda d: d.update(classification="PRODUCTION"))
            rep = self.assertRejected(t, "V-13", target="prod")
            self.assertEqual(1, sum(e.rule == "V-13" for e in rep.errors))
            self.assertIn("map", str(next(e for e in rep.errors if e.rule == "V-13")))

    def test_CFG013_duplicate_slave_id_on_bus(self):
        with TempConfig() as t:
            t.edit("site/lab_bench_2dev.site.json", lambda d: d["devices"][1].update(slave_id=1))
            self.assertRejected(t, "V-10", site="lab_bench_2dev")

    def test_CFG014_capability_without_mapping(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: d.update(mappings=[m for m in d["mappings"] if m["param"] != "ac.frequency"]))
            rep = self.assertRejected(t, "V-11")
            self.assertIn("capabilities.frequency", " ".join(str(e) for e in rep.errors))

    def test_CFG014b_mapping_without_capability(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["capabilities"].update(frequency=False))
            self.assertRejected(t, "V-11")

    def test_CFG015_meter_reports_primary_with_ct200(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["conventions"].update(ratio_handling="METER_REPORTS_PRIMARY"))
            self.assertRejected(t, "V-12", site="lab_ratio_ct200")
            self.assertIsNotNone(t.check("lab_bench")[0])          # CT = PT = 1 is fine

    def test_CFG016_interval_below_1000(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: d["poll_groups"][0].update(interval_ms=500))
            self.assertRejected(t, "V-09")

    def test_CFG016_stale_not_above_interval(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: d["poll_groups"][1].update(stale_timeout_ms=10000))
            self.assertRejected(t, "V-09")

    # ---- further rules not listed as CFG cases
    def test_counter_flag_must_match_dictionary(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.energy.import.active")["flags"].remove("COUNTER"))
            self.assertRejected(t, "V-04")

    def test_rollover_only_on_uint32_counter(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency")["flags"].append("ROLLOVER"))
            self.assertRejected(t, "V-05")

    def test_bit_mask_only_on_bitfield(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "dev.status.code").update(bit_mask=1))
            self.assertRejected(t, "V-05")

    def test_unknown_field_rejected(self):
        with TempConfig() as t:
            t.edit(MAP_FILE, lambda d: mapping(d, "ac.frequency").update(scael=0.1))
            self.assertRejected(t, "V-01")

    def test_bool_is_not_a_number(self):
        with TempConfig() as t:
            t.edit("site/lab_bench.site.json", lambda d: d["devices"][0].update(ct_ratio=True))
            self.assertRejected(t, "V-01")

    def test_unknown_bus_and_profile(self):
        with TempConfig() as t:
            t.edit("site/lab_bench.site.json", lambda d: d["devices"][0].update(bus_id="RS485_BUS_9"))
            self.assertRejected(t, "V-03")
        with TempConfig() as t:
            t.edit("site/lab_bench.site.json", lambda d: d["devices"][0].update(profile_id="NO_SUCH"))
            self.assertRejected(t, "V-03")

    def test_device_limit(self):
        with TempConfig() as t:
            def five(d):
                base = d["devices"][0]
                d["devices"] = [dict(base, device_instance_id=f"DEV_{i}", slot=i, slave_id=i + 1) for i in range(5)]
            t.edit("site/lab_bench.site.json", five)
            self.assertRejected(t, "V-15")

    def test_probe_group_must_exist(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d["comm_defaults"].update(probe_group="nope"))
            self.assertRejected(t, "V-10")

    def test_comm_overrides_apply_and_are_checked(self):
        with TempConfig() as t:
            t.edit("site/lab_bench.site.json",
                   lambda d: d["devices"][0].update(comm_overrides={"timeout_ms": 300, "max_retries": 0}))
            model, rep = t.check()
            self.assertIsNotNone(model, [str(e) for e in rep.errors])
            self.assertEqual((300, 0), (model.devices[0].timeout_ms, model.devices[0].max_retries))
            t.edit("site/lab_bench.site.json",
                   lambda d: d["devices"][0].update(comm_overrides={"timeout_ms": 20}))
            self.assertRejected(t, "V-10")

    def test_invalid_json_reported(self):
        with TempConfig() as t:
            (t.dir / MAP_FILE).write_text("{ not json", encoding="utf-8")
            self.assertRejected(t, "V-03")   # the map cannot be found among valid files

    def test_production_site_accepted_for_prod_target(self):
        with TempConfig() as t:
            t.edit(PROFILE_FILE, lambda d: d.update(classification="PRODUCTION"))
            t.edit(MAP_FILE, lambda d: d.update(classification="PRODUCTION"))
            model, rep = t.check(target="prod")
            self.assertIsNotNone(model, [str(e) for e in rep.errors])
            self.assertEqual("PRODUCTION", model.classification)
            self.assertNotIn("SIMULATION/TEST", tool.render(model)[1])


if __name__ == "__main__":
    unittest.main()
