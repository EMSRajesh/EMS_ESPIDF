"""DP-WP5 milestone M2 tests: the DC meter added as data only.

Test IDs are those of the DP-WP5 Verification & Test Specification v1.0:
GEN5-005..GEN5-009 (Section 4), CFG5-001..CFG5-008 (Section 5) and the
dictionary part of UMD5-001/UMD5-002 (Section 6). NOC-001 and NOC-003 are git
checks between the tags dp-wp5-m1 and dp-wp5-m2 (docs/WP5_CHECKLIST.md).

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
from test_profile_tool import CONFIG, SITE_TABLES, TempConfig, mapping, run  # noqa: E402

DC_SITE = CONFIG / "site" / "lab_dc_bench.site.json"
MIXED_SITE = CONFIG / "site" / "lab_mixed.site.json"
DC_PROFILE = "profiles/dc_meter_sim.profile.json"
DC_MAP = "maps/myems_sim_dcmeter.map.json"
UMD_FILE = "umd/umd_v1.json"
SITE_CRC = {"lab_dc_bench": 0xCC22DC64, "lab_mixed": 0xC04114E4}


def errors(rep) -> str:
    return "\n".join(str(e) for e in rep.errors)


class Gen5Tests(unittest.TestCase):

    def test_GEN5_005_validate_dc_site(self):
        rc, out, err = run("validate", "--site", str(DC_SITE))
        self.assertEqual(0, rc, err)
        self.assertIn("1 device(s)", out)
        self.assertIn("slave 3", out)
        self.assertIn("3 poll groups, 6 mappings", out)
        self.assertIn("VALID", out)

    def test_GEN5_005_validate_mixed_site(self):
        rc, out, err = run("validate", "--site", str(MIXED_SITE))
        self.assertEqual(0, rc, err)
        self.assertIn("2 device(s)", out)
        self.assertIn("AC_METER_3PH_SIM", out)
        self.assertIn("DC_METER_SIM", out)

    def test_GEN5_006_two_second_interval_violates_budget(self):
        with TempConfig() as t:
            t.edit(DC_MAP, lambda m: m["poll_groups"][0].update(interval_ms=2000))
            model, rep = t.check("lab_dc_bench")
            self.assertIsNone(model)
            self.assertIn("[V-14]", errors(rep))

    def test_GEN5_007_budget_for_the_mixed_site(self):
        rc, out, err = run("budget", "--site", str(MIXED_SITE))
        self.assertEqual(0, rc, err)
        self.assertIn("nominal load 6.4% (limit 30%)", out)
        self.assertIn("all devices offline 20.8% (limit 50%)", out)
        self.assertRegex(out, r"DC_BATT_STRING_01\s+500ms\s+1\s+3\.12s\s+4\.0s")
        self.assertIn("RESULT: PASS", out)

    def test_GEN5_008_generation_is_deterministic_and_committed(self):
        for site in (DC_SITE, MIXED_SITE):
            name = site.name.replace(".site.json", "")
            with self.subTest(site=name), tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
                for d in (a, b):
                    self.assertEqual(0, run("generate", "--site", str(site), "--target", "sim", "--out-dir", d)[0])
                for f in ("ems_config_gen.h", "ems_config_gen.c"):
                    self.assertEqual((Path(a) / f).read_bytes(), (Path(b) / f).read_bytes(), f)
                rc, _, err = run("generate", "--site", str(site), "--target", "sim", "--check",
                                 "--out-dir", str(SITE_TABLES / name))
                self.assertEqual(0, rc, err)
                model, _ = tool.build_model(site, "sim")
                self.assertEqual(SITE_CRC[name], tool.config_crc32(model))

    def test_GEN5_009_export_sim_for_the_mixed_site(self):
        model, _ = tool.build_model(MIXED_SITE, "sim")
        _, c = tool.render(model)
        sim = tool.export_sim(model)
        self.assertEqual("myems.simlayout/1.1", sim["format"])
        self.assertEqual(f"0x{tool.config_crc32(model):08X}", sim["config_crc32"])
        ac, dc = sim["devices"]
        self.assertEqual(("AC_METER", "SIGNED_BY_ACTIVE_POWER", 1), (ac["device_type"], ac["pf_sign"], ac["slave_id"]))
        self.assertEqual(("DC_METER", "NOT_APPLICABLE", 3), (dc["device_type"], dc["pf_sign"], dc["slave_id"]))
        roles = {r["param"]: r["role"] for r in dc["registers"]}
        self.assertEqual({"dc.voltage": "voltage", "dc.current": "current.signed", "dc.power": "power.active",
                          "dc.energy.import": "energy.import", "dc.energy.export": "energy.export",
                          "dev.status.code": "status"}, roles)
        self.assertEqual(1800.0, next(r for r in dc["registers"] if r["param"] == "dc.voltage")["sim_out_of_range"])
        # registers and groups identical to the C tables (WP4 GEN-008 method), both maps in site order
        c_maps = re.findall(r"\.address = (\d+)u, .*?\n\s+\.group_index = (\d+)u, \.type = EMS_REG_(\w+), "
                            r"\.order = EMS_ORDER_(\w+)", c)
        c_groups = [(int(s), int(n), int(f)) for s, n, f in re.findall(
            r"\.start_address = (\d+)u, \.count = (\d+)u, \.function_code = (\d+)u", c)]
        regs, groups = [], []
        for dev in sim["devices"]:
            regs += [(str(r["address"]), str(next(i for i, g in enumerate(dev["poll_groups"]) if g["name"] == r["group"])),
                      r["type"], r["order"]) for r in dev["registers"]]
            groups += [(g["start"], g["count"], g["function_code"]) for g in dev["poll_groups"]]
        self.assertEqual(c_maps, regs)
        self.assertEqual(c_groups, groups)
        self.assertEqual([(0, 6, 4), (100, 4, 4), (200, 1, 3)], groups[3:])


class Cfg5Tests(unittest.TestCase):

    def assertRejected(self, t: TempConfig, rule: str, site: str = "lab_dc_bench", needle: str = "") -> None:
        model, rep = t.check(site)
        self.assertIsNone(model)
        self.assertIn(f"[{rule}]", errors(rep))
        if needle:
            self.assertIn(needle, errors(rep))

    def test_CFG5_001_shipped_sites_valid(self):
        for site in (DC_SITE, MIXED_SITE):
            with self.subTest(site=site.name):
                model, rep = tool.build_model(site, "sim")
                self.assertIsNotNone(model, errors(rep))

    def test_CFG5_001_sim_profile_rejected_for_prod(self):
        model, rep = tool.build_model(DC_SITE, "prod")
        self.assertIsNone(model)
        self.assertIn("[V-13]", errors(rep))

    def test_CFG5_002_power_factor_mapping_with_not_applicable(self):
        def edit_map(m):
            m["poll_groups"][0]["count"] = 7
            m["mappings"].append({"mapping_id": 7, "param": "ac.power.factor.total", "group": "measurements",
                                  "address": 6, "type": "INT16", "order": "AB", "scale": 0.001, "offset": 0.0,
                                  "unit": "1", "min": -1, "max": 1, "flags": []})
        with TempConfig() as t:
            t.edit(DC_MAP, edit_map)
            t.edit(DC_PROFILE, lambda p: p["capabilities"].update(power_factor=True))
            self.assertRejected(t, "V-16")

    def test_CFG5_003_ac_profile_with_not_applicable(self):
        with TempConfig() as t:
            t.edit("profiles/ac_meter_3ph_sim.profile.json",
                   lambda p: (p.update(schema="myems.profile/1.2"), p["conventions"].update(pf_sign="NOT_APPLICABLE")))
            self.assertRejected(t, "V-16", site="lab_mixed")

    def test_CFG5_004_two_active_power_roles(self):
        with TempConfig() as t:
            t.edit(UMD_FILE, lambda d: next(p for p in d["parameters"] if p["id"] == "dc.current")
                   .update(role="power.active"))
            self.assertRejected(t, "V-17")

    def test_CFG5_005_rollover_without_power(self):
        def edit_map(m):
            m["mappings"] = [x for x in m["mappings"] if x["param"] != "dc.power"]
            mapping(m, "dc.energy.import")["flags"].append("ROLLOVER")
        with TempConfig() as t:
            t.edit(DC_MAP, edit_map)
            t.edit(DC_PROFILE, lambda p: p["capabilities"].update(dc_power=False))
            self.assertRejected(t, "V-17", needle="ROLLOVER")

    def test_CFG5_006_sim_out_of_range_inside_range(self):
        with TempConfig() as t:
            t.edit(DC_MAP, lambda m: mapping(m, "dc.voltage").update(sim_out_of_range=1000.0))
            self.assertRejected(t, "V-18", needle="inside the range")

    def test_CFG5_007_ct_ratio_with_meter_reports_primary(self):
        with TempConfig() as t:
            t.edit("site/lab_dc_bench.site.json", lambda s: s["devices"][0].update(ct_ratio=200.0))
            self.assertRejected(t, "V-12")

    def test_CFG5_008_capability_without_mapping(self):
        with TempConfig() as t:
            t.edit(DC_PROFILE, lambda p: p["capabilities"].update(dc_shunt_temperature=True))
            self.assertRejected(t, "V-11", needle="dc_shunt_temperature")

    def test_dc_profile_needs_profile_schema_1_2(self):
        with TempConfig() as t:
            t.edit(DC_PROFILE, lambda p: p.update(schema="myems.profile/1.1"))
            self.assertRejected(t, "V-01", needle="conventions.pf_sign")


class Umd5Tests(unittest.TestCase):

    def test_UMD5_001_dc_runtime_ids(self):
        params = {p.name: p for p in umd_gen.validate(json.loads((CONFIG / UMD_FILE).read_text(encoding="utf-8")))}
        expected = {"dc.voltage": (0x0201, "V", "float", "voltage"),
                    "dc.current": (0x0211, "A", "float", "current.signed"),
                    "dc.power": (0x0221, "kW", "float", "power.active"),
                    "dc.energy.import": (0x0240, "kWh", "counter", "energy.import"),
                    "dc.energy.export": (0x0241, "kWh", "counter", "energy.export")}
        for name, (rid, unit, vt, role) in expected.items():
            p = params[name]
            self.assertEqual((rid, unit, vt, role), (p.runtime_id, p.unit, p.value_type, p.role), name)

    def test_UMD5_002_released_ids_unchanged(self):
        released = umd_gen.validate(json.loads((CONFIG / "umd" / "released" / "umd_1.0.0.json")
                                               .read_text(encoding="utf-8")))
        now = {p.name: p for p in umd_gen.validate(json.loads((CONFIG / UMD_FILE).read_text(encoding="utf-8")))}
        for p in released:
            self.assertEqual((p.runtime_id, p.unit, p.value_type),
                             (now[p.name].runtime_id, now[p.name].unit, now[p.name].value_type), p.name)
        header = (tool.PROJECT_ROOT / "components" / "ems_measurement" / "include" / "ems_param_ids.h").read_text()
        self.assertIn("EMS_PARAM_COUNT 19u", header)
        self.assertIn("EMS_PARAM_DC_POWER                ((uint16_t)0x0221u)", header)


if __name__ == "__main__":
    unittest.main()
