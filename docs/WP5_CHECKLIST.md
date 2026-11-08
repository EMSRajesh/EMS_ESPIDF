# DP-WP5 checklist — DC meter reference profile

Specifications: `docs/DP-WP5_Design_Specification_v1.0.md`, `docs/DP-WP5_Development_Plan_v1.0.md`,
`docs/DP-WP5_Test_Specification_v1.0.md` (also in the EMS project as `claude/MyEMS_WP5_*.md`).

| Milestone | Content | State |
|---|---|---|
| M0 | D9 committed (c987e4e); D9 HIL run deferred by the user | Done |
| M1 | Framework generalisation, AC data only | Delivered, to be committed |
| M2 | DC meter data only | – |
| M3 | Host decode and simulator (DC) | – |
| M4 | HIL, DC meter alone | – |
| M5 | HIL, mixed AC + DC site | – |
| M6 | Stabilisation and acceptance | – |

## M1 — framework generalisation (AC data only)

### What changed

| File | Change |
|---|---|
| `config/umd/umd_v1.json` | Dictionary 1.1.0 (`myems.umd/1.1`): `roles`, a `role` on every parameter, `capabilities` (the table formerly inside the profile tool). No new parameters. |
| `config/umd/released/umd_1.0.0.json` | Frozen copy of the DP-WP4 dictionary, reference for rule U-06 |
| `config/schema/profile.schema.json`, `regmap.schema.json`, `umd.schema.json` (new) | Profile 1.2 (`pf_sign: NOT_APPLICABLE`), regmap 1.2 (`sim_out_of_range`), dictionary schema |
| `tools/profile/umd_gen.py` | 1.1.0: reads dictionary 1.0 and 1.1; U-04 (roles, capabilities), U-06 (released dictionaries). Roles are not written to C. |
| `tools/profile/myems_profile_tool.py` | 1.1.0: per-kind schema versions; capabilities and roles from the dictionary; counter plausibility by role `power.active`; rules V-16, V-17, V-18; convention values checked; `export-sim` format 1.1 (role, pf_sign, device_type, sim_out_of_range); command `umd-check`. No dictionary parameter names left in the tool. |
| `tools/wp4/myems_ac_meter_sim.py` | 1.1.0: value model by role (IMPORT/EXPORT/LOAD_CHANGE/OUT_OF_RANGE/COUNTER_RESET); NORMAL image from map test vectors, else the marked `LEGACY_WP4` AC image; refuses a 1.0 layout |
| `main/Kconfig.projbuild`, `main/CMakeLists.txt` | Choice `MYEMS_SIM_SITE` replaced by string `MYEMS_SIM_SITE_DIR` (folder under `main/`); CMake stops with a clear error if the folder has no tables |
| `sdkconfig.wp4_sim`, `.wp4_esc`, `.wp4_sim_2dev`, `.wp4_ratio` | The three choice lines replaced by one `CONFIG_MYEMS_SIM_SITE_DIR=...` line (same folders as D9; CRLF kept) |
| Generated files (`main/generated`, `main/generated_sites/*`, `test/host/generated/decoder_test`, `components/ems_measurement/.../ems_param_ids.h`, `ems_param_table.c`) | Regenerated: only tool/dictionary version strings and the inputs hash changed |
| `test/host/test_ems_dictionary.c` | Expects dictionary version 1.1.0 |
| `tools/profile/tests/test_wp5_m1.py` (new) | U-001…U-008, GEN5-001…004, GEN5-010, V-16…V-18, NOC-002 |
| `tools/wp4/tests/test_sim_wp5.py`, `sim_trace.py`, `data/sim_ac_golden_wp4.json` (new) | SIM5-001: 60 AC scenarios (14 modes × 2 sites × 2 seeds, plus mode sequences and overrides), 120 s each, compared with digests recorded from the D9 simulator; SIM5-010 |
| `tools/profile/tests/test_profile_tool.py`, `test_umd_gen.py` | Dictionary version 1.1.0; a deleted parameter also leaves its capability |
| `BUILD_MODES.md` | Site selection note |

### Exit evidence (host, 9 Nov 2026)

| Check | Result |
|---|---|
| `python -m unittest discover -s tools/profile/tests` | 92 tests OK (64 WP4 + 28 new) |
| `python -m unittest discover -s tools/wp4/tests` | 165 tests OK (162 WP4 + 3 new) |
| `ctest` (host C) | 10/10 |
| `python tools/profile/myems_profile_tool.py umd-check` | OK, UMD 1.1.0, 14 parameters |
| GEN5-001: WP4 site CRCs | Unchanged: lab_bench 0x4B8B5924, lab_bench_2dev 0xE3FF2DF7, lab_ratio_ct200 0x5A2CC286, decoder 0x5A334BC5 |
| GEN5-010: generated table diff vs D9 | Version strings and inputs hash only |
| SIM5-001: AC simulator output vs D9 | Identical for all 60 scenarios (mutation check: dropping the PF sign rule is detected) |
| NOC-002 | No parameter names in tool/simulator code outside `LEGACY_WP4` lines and help examples |

### To run on the PC before committing

```powershell
python -m unittest discover -s tools/profile/tests
python -m unittest discover -s tools/wp4/tests
python tools/profile/myems_profile_tool.py umd-check
cmake -S test/host -B build_host; cmake --build build_host; ctest --test-dir build_host
.\tools\build_firmware.ps1 -Mode Wp4Sim2Dev     # checks the new site selection; banner CRC 0xE3FF2DF7
.\tools\build_firmware.ps1 -Mode Wp4Sim         # CRC 0x4B8B5924
```

No HIL run is needed for M1: the firmware tables have the same CRCs and the
simulator output is byte-identical. The D9 HIL evidence (`run_d9_all.ps1`) stays valid on this commit.
