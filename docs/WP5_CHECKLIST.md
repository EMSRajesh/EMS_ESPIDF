# DP-WP5 checklist — DC meter reference profile

Specifications: `docs/DP-WP5_Design_Specification_v1.0.md`, `docs/DP-WP5_Development_Plan_v1.0.md`,
`docs/DP-WP5_Test_Specification_v1.0.md` (also in the EMS project as `claude/MyEMS_WP5_*.md`).

| Milestone | Content | State |
|---|---|---|
| M0 | D9 committed (c987e4e); D9 HIL run deferred by the user | Done |
| M1 | Framework generalisation, AC data only | Done, tag `dp-wp5-m1` (635edd4) |
| M2 | DC meter data only | Done, tag `dp-wp5-m2` (12a6bc0) |
| M3 | Host decode and simulator (DC) | Done, tag `dp-wp5-m3` (391e8fd) |
| M4 | HIL, DC meter alone | Runner suite `dc` delivered; HIL run pending |
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

## M2 — DC meter as data only

### What was added

| File | Kind | Content |
|---|---|---|
| `config/umd/umd_v1.json` | data | 5 DC parameters (0x0201 voltage, 0x0211 current, 0x0221 power, 0x0240/0x0241 energy) with roles; 6 DC capabilities (`dc_shunt_temperature` reserved, empty) |
| `config/profiles/dc_meter_sim.profile.json` | data | `DC_METER_SIM` 1.0.0, profile schema 1.2, `pf_sign: NOT_APPLICABLE`, `METER_REPORTS_PRIMARY` |
| `config/maps/myems_sim_dcmeter.map.json` | data | `MYEMS_SIM_DCMETER` 1.0.0, regmap 1.2: FC04 `measurements` 0..5 (FLOAT32 ABCD, INT32 CDAB, FLOAT32 CDAB), FC04 `energy` 100..103 (UINT32 CDAB counters), FC03 `status` 200; test vectors = Test Spec 2.1; `sim_out_of_range` 1800 V |
| `config/site/lab_dc_bench.site.json`, `lab_mixed.site.json` | data | DC meter slave 3; AC slave 1 + DC slave 3 |
| `main/generated_sites/lab_dc_bench/`, `lab_mixed/` | generated | CRC 0xCC22DC64, 0xC04114E4 |
| `components/ems_measurement/include/ems_param_ids.h`, `src/ems_param_table.c` | generated (`umd_gen.py`) | 19 parameters; existing IDs unchanged |
| `main/generated`, `main/generated_sites/lab_bench_2dev`, `lab_ratio_ct200`, `test/host/generated/decoder_test` | generated | Inputs hash only (the dictionary is an input); CRCs unchanged |
| `sdkconfig.wp5_dc`, `sdkconfig.wp5_mixed`, `sdkconfig.wp5_mixed_esc` | build config | `wp4_sim` / `wp4_esc` with another `CONFIG_MYEMS_SIM_SITE_DIR` |
| `tools/build_firmware.ps1`, `BUILD_MODES.md` | build tooling | Modes `Wp5Dc`, `Wp5Mixed`, `Wp5MixedEsc` |
| `test/host/CMakeLists.txt`, `test_ems_config_site.c`, `test_ems_dictionary.c` | tests | Executables `test_ems_config_site_dc`, `_mixed` (DC tables: FC04 groups, runtime IDs, no ratio flags, counter limit 3000 kW); DC entries in the dictionary test |
| `tools/profile/tests/test_umd_gen.py`, `test_wp5_m2.py` (new) | tests | DC dictionary entries; GEN5-005…009, CFG5-001…008, UMD5-001/002 |

No file under `main/` or `components/` other than generated files, and no tool or
simulator source, was changed: the DC meter is served by the simulator straight
from the map (`--dump` shows the Test Spec 2.1 words).

### Exit evidence (host, 9 Nov 2026)

| Check | Result |
|---|---|
| `python -m unittest discover -s tools/profile/tests` | 110 tests OK (92 + 18 new) |
| `python -m unittest discover -s tools/wp4/tests` | 165 tests OK |
| `ctest` (host C) | 12/12 (2 new site executables) |
| `umd-check` | OK, UMD 1.1.0, 19 parameters |
| GEN5-007 budget, `lab_mixed` | Nominal 6.4 %, all offline 20.8 %, DC T_wc 3.12 s < 4 s: PASS |
| GEN5-006 | DC interval 2000 ms rejected by V-14 |
| Simulator `--dump`, `lab_mixed` | DC: `4448 0000 E848 0001 0000 42C8` / `D090 0003 0000 0000` / `0000` |
| Firmware compile check (host, ESP32 types) of both new table sets with `app_main.c`, `ems_acquisition.c`, `ems_poll_scheduler.c` | OK |

### NOC checks after committing (Test Spec NOC-001, NOC-003)

```powershell
git diff --stat dp-wp5-m1 HEAD -- main components   # only main/generated*/ and the two ems_measurement generated files
git diff --stat dp-wp5-m1 HEAD -- tools/profile/myems_profile_tool.py tools/profile/umd_gen.py tools/wp4/myems_ac_meter_sim.py   # empty
```

### To run on the PC before committing

```powershell
python -m unittest discover -s tools/profile/tests
python -m unittest discover -s tools/wp4/tests
cmake -S test/host -B build_host; cmake --build build_host; ctest --test-dir build_host
.\tools\build_firmware.ps1 -Mode Wp5Dc       # banner: LAB_DC_BENCH, CRC 0xCC22DC64
.\tools\build_firmware.ps1 -Mode Wp5Mixed    # banner: LAB_MIXED, CRC 0xC04114E4
```

Optional quick look on the bench (not an exit criterion; the HIL suites come in M4/M5):

```powershell
.\tools\build_firmware.ps1 -Mode Wp5Dc -Port COM11 -Flash -Monitor
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_dc_bench.site.json --port COM12
```

## M3 — DC meter on the host and in the simulator

Test code only; no file under `main/`, `components/`, `config/` or in the
tool/simulator sources changed.

| File | Content |
|---|---|
| `test/host/test_ems_dc_meter.c` (new) | The DC meter through the firmware's generic acquisition core (`main/ems_acq_core.c`) with the generated DC tables: DCDEC-001 (vectors 2.1), DCDEC-002 (export sign), DCDEC-003 (edge vectors E1–E7), DCDEC-004 (no ratio), DCDEC-005 (group at 100), DCDEC-006/007 (counter delta, reset), DCDEC-008 (FC04 entries; an FC03 answer to an FC04 entry is a routing error); TIMEOUT → STALE → COMM_LOST with the last value kept; 9 s stale timeout. In the mixed site also: AC export and DC import at once, AC OFFLINE while DC stays GOOD, each device NOT_SUPPORTED for the other's parameters |
| `test/host/CMakeLists.txt`, `test_ems_config_site.c`, `README.md` | The DC and mixed site executables also build `ems_acq_core.c` and the new test file |
| `tools/wp4/tests/test_sim_wp5.py` | SIM5-002…SIM5-009 and DC fault modes, on the unchanged simulator |
| `docs/DP-WP5_Test_Specification_v1.0.md` | Edge vector E7 corrected (see finding) |

### Finding

The edge vector E7 in Test Spec 2.2 was wrong: 3000.0 as FLOAT32 is 0x453B8000,
so the CDAB words are `8000 453B`, not `0000 453B` (that is 2992.0). The host
test caught it; the specification is corrected. The map and firmware were right.

### Exit evidence (host, 9 Nov 2026)

| Check | Result |
|---|---|
| `ctest` (host C) | 12/12; `test_ems_config_site_dc` 16 tests, `_mixed` 17 tests |
| `ctest` with AddressSanitizer/UBSan (`-DEMS_HOST_SANITIZE=ON`) | 12/12 |
| `python -m unittest discover -s tools/wp4/tests` | 174 tests OK (165 + 9 new) |
| `python -m unittest discover -s tools/profile/tests` | 110 tests OK |

### To run on the PC before committing

```powershell
python -m unittest discover -s tools/wp4/tests
cmake -S test/host -B build_host; cmake --build build_host; ctest --test-dir build_host
```


## M4 — HIL, DC meter alone

Test code only (runner and its tests); firmware, configuration, tool and
simulator unchanged.

| File | Content |
|---|---|
| `tools/wp4/myems_wp4_runner.py` | Suite `dc` (site `lab_dc_bench`, build `Wp5Dc`, 13 phases, about 8 min plus boot and the STATS wait). Analysis `analyse_dc` works on parameter **roles** from the export-sim layout: the NORMAL values come from the map test vectors, the signed parameters from `power.active` / `current.signed`, the out-of-range case from `sim_out_of_range`, the probe group from the `status` role. It names no DC parameter. |
| `tools/wp4/tests/test_wp4_runner_wp5.py` (new) | A scripted DC run that follows the design passes every DC check; 9 deliberate deviations (wrong FC, wrong CRC, interval drift, wrong export sign, out-of-range not flagged, two discontinuities, retry under SLOW, GOOD value after the loss, other group polled while OFFLINE) each fail the check that covers them |
| `docs/DP-WP5_Test_Specification_v1.0.md` | DC-003 states how intervals are measured (simulator receive times; offsets not measurable on the board) |

### Phases of suite `dc`

| Phase | Simulator | s | Checks |
|---|---|---|---|
| normal | NORMAL | 40 | DC-001 (CRC, slave 3 only, FC04@0, FC04@100, FC03@200), DC-002 (6 values = map test vectors) |
| import | IMPORT | 40 | DC-004: +100 kW, +125 A GOOD; import counter rises, export unchanged |
| export | EXPORT | 40 | DC-005: −100 kW, −125 A GOOD; export counter rises, import unchanged |
| out_of_range | OUT_OF_RANGE | 20 | DC-006: 1800 V OUT_OF_RANGE; current and power stay GOOD |
| counter_reset | COUNTER_RESET | 25 | DC-007: exactly one discontinuity event for the import counter; counts again from 0 |
| slow | SLOW:400 | 30 | DC-012: no timeout, no retry, all GOOD |
| exception_energy | EXCEPTION:02@energy | 25 | DC-010: energy INVALID, others GOOD, device ONLINE, event code 0x02 |
| timeout | TIMEOUT | 30 | DC-008: OFFLINE within 14 s, all COMM_LOST, nothing GOOD, last values kept (not 0) |
| recover | NORMAL | 45 | DC-008: ONLINE within one probe interval, all GOOD |
| offline_recover | OFFLINE_THEN_RECOVER:65 | 100 | DC-009: only the status group probed, every 30 s; ONLINE after 65 s; all groups polled at once |
| bad_crc | BAD_CRC | 30 | DC-011: as DC-008 |
| recover_crc | NORMAL | 45 | DC-011: back ONLINE |
| final | NORMAL | 15 | DC-002 end of run; SCH-003 no overlap; stability, routing errors, stack |

DC-003 (4 s / 10 s / 30 s intervals) is measured over the first five answering phases.

### Run on the bench (about 10 minutes)

```powershell
.\tools\build_firmware.ps1 -Mode Wp5Dc -Port COM11 -Flash
python tools/wp4/myems_wp4_runner.py --suite dc --console-port COM11 --rs485-port COM12
```

Evidence: `tools/test_results/wp4_dc_<timestamp>/` (console.log, sim.log,
phases.json, report.txt). The run is provisional until COM-WP2.4 is closed.
