---
title: "MyEMS DP-WP5 — DC Meter Reference Profile"
subtitle: "Development Plan & Implementation Specification v1.0"
---

| Document control | Value |
|---|---|
| Project | MyEMS — Energy Management System |
| Work package | DP-WP5 — DC Meter Reference Profile |
| Document version | 1.0 — first issue (M1 implementation notes folded in, 9 Nov 2026) |
| Status | Plan for review; not frozen |
| Platform | Waveshare ESP32-S3-POE-ETH-8DI-8DO |
| Toolchain | Windows 10/11, VS Code + ESP-IDF extension, ESP-IDF v6.1, Python 3.11 |
| Code baseline | DP-WP4 D9 commit on `myems_rs485_modbus_rtu_09_057` |
| Companion documents | DP-WP5 Design Specification v1.0 ("Design"); DP-WP5 Verification & Test Specification v1.0 ("Test Spec") |

# 1. Objective

Implement DP-WP5 as described in the Design, in an order that proves the DP-WP4 framework claim: first generalise the remaining AC-only tool and simulator code **with AC data only** (M1), then add the DC meter **as data only** (M2), then verify on host, simulator and hardware.

# 2. Baseline and dependencies

| Dependency | State | Effect on DP-WP5 |
|---|---|---|
| DP-WP4 D9 | Delivered; HIL runs pending (`tools/d9/run_d9_all.ps1`) | M0: commit D9 and, ideally, record its HIL run first (Design open item 1) |
| COM-WP2.4, CR-D8-01 | Open on the COM track | Host milestones proceed; HIL results provisional until closed, as for WP4 |
| Host test setup (`test/host`, Unity, ctest 10/10) | In place | Extended with DC and mixed site tests |
| WP4 runner (`tools/wp4/myems_wp4_runner.py`) and simulator | In place | Extended with suites `dc` and `mixed`; simulator made role-driven |

# 3. Work breakdown

## 3.1 Files changed in M1 (code, AC data only)

| File | Change | Design ref |
|---|---|---|
| `config/umd/umd_v1.json` | Schema 1.1, version 1.1.0; add `roles`, `role` on every AC/dev parameter, `capabilities` (AC table moved from the tool). **No DC parameters yet.** | 4 |
| `config/umd/released/umd_1.0.0.json` | New: frozen copy of the WP4 dictionary for rule U-06 | 4.5 |
| `config/schema/*.schema.json` | Profile 1.2 (`pf_sign` NOT_APPLICABLE), regmap 1.2 (`sim_out_of_range`); new `umd.schema.json` | 5 |
| `tools/profile/umd_gen.py` | Accepts dictionary 1.0 and 1.1; U-04 (roles, capabilities) and U-06 (released dictionaries); roles are not written to C | 4.5 |
| `tools/profile/myems_profile_tool.py` | Per-kind supported versions; capabilities and roles read from the dictionary (dictionary 1.1 required); `CAPABILITIES` and `ACTIVE_POWER_PARAM` removed; `counter_max_rate` by role; V-16, V-17, V-18; checked convention values; `export-sim` 1.1 with `role`, `pf_sign`, `device_type`, `sim_out_of_range`; new command `umd-check` | 4.5, 5 |
| `tools/wp4/myems_ac_meter_sim.py` | Value model by role (Design 8); base image from `test_vector`; `OUT_OF_RANGE` from `sim_out_of_range` with frequency fallback; AC constants kept only as the fallback base image | 8 |
| `main/Kconfig.projbuild`, `main/CMakeLists.txt` | Replace choice `MYEMS_SIM_SITE` with string `MYEMS_SIM_SITE_DIR` | 9 |
| `sdkconfig.wp4_sim`, `.wp4_sim_2dev`, `.wp4_ratio`, `.wp4_esc` | Replace the choice lines with the equivalent `CONFIG_MYEMS_SIM_SITE_DIR` line (CRLF kept) | 9 |
| `main/generated/`, `main/generated_sites/*`, `test/host/generated/*`, `components/ems_measurement/.../ems_param_ids.h`, `ems_param_table.c` | Regenerated: version strings and inputs hash only; CRCs unchanged | 9 |
| `tools/profile/tests`, `tools/wp4/tests`, `test/host` | New tests for M1 (Test Spec U, GEN5, SIM5-AC) | – |

**Rule for M1:** the regenerated AC tables may differ from D9 only in the tool and dictionary version strings and the inputs hash; the configuration CRCs stay the same. The generated parameter, mapping and poll-group tables must be identical (Test Spec GEN5-010). Simulator AC output must be byte-identical for every mode (SIM5-001).

## 3.2 Files added in M2 (data only)

| File | Content | Design ref |
|---|---|---|
| `config/umd/umd_v1.json` | Add the five DC parameters and the DC capabilities, still within working version 1.1.0 (see the note below) | 4.2 |
| `config/profiles/dc_meter_sim.profile.json` | Design 6.1 | 6.1 |
| `config/maps/myems_sim_dcmeter.map.json` | Design 6.2, with `test_vector` and `sim_out_of_range` | 6.2 |
| `config/site/lab_dc_bench.site.json`, `lab_mixed.site.json` | Design 6.3 | 6.3 |
| `main/generated_sites/lab_dc_bench/`, `lab_mixed/` | Generated | 9 |
| `sdkconfig.wp5_dc`, `sdkconfig.wp5_mixed` | Production + `MYEMS_CONFIG_TARGET_SIM=y` + site dir + banner variant | 9 |
| `tools/build_firmware.ps1`, `BUILD_MODES.md` | Modes `Wp5Dc`, `Wp5Mixed` (build tooling, not firmware) | 9 |
| `test/host/CMakeLists.txt` | Executables `test_ems_config_site_dc`, `test_ems_config_site_mixed` built from the existing test source | – |

**Rule for M2:** `git diff --stat <M1 tag> <M2 tag> -- main components` lists only generated files: `main/generated*/` and the two dictionary C files of `components/ems_measurement` (Test Spec NOC-001). If anything else is needed, M2 stops, the change moves back to an M1-style commit with AC regression, and the finding is recorded against the framework.

Note on the dictionary version: M1 and M2 are both part of dictionary release 1.1.0. It is "released" (copied to `config/umd/released/`) only at WP5 acceptance. Between M1 and M2 it is a working version and only rule U-06 against 1.0.0 applies.

## 3.3 Runner and scripts (M3–M5)

| File | Change |
|---|---|
| `tools/wp4/myems_wp4_runner.py` | Suites `dc`, `mixed`, `mixed_esc`; site-aware `load` and `soak`; expected NORMAL values read from the `export-sim` test vectors where possible (Design open item 6) |
| `tools/wp4/tests/test_wp4_runner_wp5.py` | Unit tests for the new analyses (offline log fixtures, as for D9) |
| `tools/wp5/run_wp5_all.ps1` | Unattended run of the WP5 HIL steps, same structure as `run_d9_all.ps1` (keep-awake, per-step logs, `summary.txt`, `-SkipSoak`, `-SoakMinutes`) |
| `sdkconfig.wp5_mixed_esc`, build mode `Wp5MixedEsc` | Mixed site + `MYEMS_WP4_ESC_TEST`, for bus escalation with two device types |
| `docs/WP5_CHECKLIST.md`, `docs/DP-WP5_ACCEPTANCE_RECORD.md` | Checklist and acceptance record |

# 4. Key interface changes (host tool)

```python
SUPPORTED = {                       # kind -> (major, highest minor)
    "myems.umd": (1, 1), "myems.profile": (1, 2),
    "myems.regmap": (1, 2), "myems.site": (1, 1),
}
ROLES = ("power.active", "power.reactive", "power.apparent", "power.factor",
         "current", "current.signed", "voltage", "frequency",
         "energy.import", "energy.export", "status")   # vocabulary, not parameter names

umd_gen.capabilities(umd) -> dict[str, tuple[str, ...]]   # from the dictionary (1.1 required)
Mapping.role                                            # role of the mapped parameter
def counter_max_rate(model, prof) -> float              # |min|/|max| of the power.active mapping
```

`export-sim` output (`myems.simlayout/1.0` → `1.1`) adds `role` per register, `pf_sign` per device and `sim_out_of_range` where present. The simulator refuses a 1.0 layout with a clear message, so an old layout file cannot silently drive the new model.

# 5. Simulator implementation notes

- Role lookup is done once per slave at start-up from the layout; modes then work on role → parameter lists.
- Load-change rounding uses the mapping's resolution: decimals of `scale` for integer types, 3 decimals for FLOAT32. For the AC map this reproduces the WP4 rounding exactly (currents 2, powers and PF 3).
- `EXPORT` negates the base values of `power.active`, `current.signed`, and `power.factor` when the device's `pf_sign` is `SIGNED_BY_ACTIVE_POWER`.
- Energy integration and `COUNTER_RESET` use the `energy.import` / `energy.export` roles.
- SIM5-001 drives 60 AC scenarios (every mode, two sites, seeds 1 and 7, plus mode sequences and overrides) for 120 s and compares the SHA-256 of all replies with digests recorded from the D9 simulator (`tools/wp4/tests/data/sim_ac_golden_wp4.json`).

# 6. Implementation rules

WP4 Plan Section 9 applies unchanged. In addition:

1. No parameter name from the dictionary may appear in `tools/profile/*.py` or the simulator, except in the simulator's AC fallback base image (marked `LEGACY_WP4`) and help-text usage examples. Checked by a grep test (Test Spec NOC-002).
2. A runtime ID, once in a released dictionary, never changes. Dictionary entries may be added freely until release.
3. Every new site is validated, generated and covered by a host config test before any HIL run uses it.
4. Text files keep their existing line endings (sdkconfig and PowerShell: CRLF; Python and JSON: LF).
5. The user commits and tags; this plan lists the commands at each milestone exit.

# 7. Milestones

| # | Milestone | Work | Exit criteria (Test Spec IDs) | Tag |
|---|---|---|---|---|
| M0 | Prerequisites | Commit D9; run `run_d9_all.ps1` on that commit (or record it as deferred) | Done: D9 committed as c987e4e; D9 HIL deferred by the user | – |
| M1 | Framework generalisation | Section 3.1 | U-001…U-008, GEN5-001…GEN5-004, GEN5-010, SIM5-001, NOC-002; all WP4 host and tool tests pass unchanged | `dp-wp5-m1` |
| M2 | DC data | Section 3.2 | NOC-001, NOC-003, GEN5-005…GEN5-009, CFG5-001…CFG5-008, UMD5-001…UMD5-004, host config tests 12/12 | `dp-wp5-m2` |
| M3 | Host decode and simulator | DC vectors through the host engine; simulator DC modes | DCDEC-001…DCDEC-008, SIM5-002…SIM5-010 | `dp-wp5-m3` |
| M4 | HIL, DC meter alone | Build `Wp5Dc`; runner suite `dc` | DC-001…DC-012 | `dp-wp5-m4` |
| M5 | HIL, mixed site | Builds `Wp5Mixed`, `Wp5MixedEsc`; suites `mixed`, `mixed_esc` | MIX-001…MIX-008 | `dp-wp5-m5` |
| M6 | Stabilisation and acceptance | `run_wp5_all.ps1`: mixed load 1 h, mixed soak, full WP4 regression on the WP5 commit | PERF5-001…PERF5-005, REG5-001…REG5-006; acceptance gate | `dp-wp5` |

M1–M3 need no ESP32 and can run while D9 HIL and COM-WP2.4 are still open. M4–M6 need the board; their results become acceptance evidence only after COM-WP2.4 is closed.

# 8. Build modes

| Mode | sdkconfig | Site folder | Banner variant | Purpose |
|---|---|---|---|---|
| Production, SelfTest, Wp4Sim, Wp4Sim2Dev, Wp4Ratio, Wp4Esc | unchanged | unchanged | unchanged | WP4 regression |
| **Wp5Dc** | `sdkconfig.wp5_dc` | `generated_sites/lab_dc_bench` | `WP5_DC_SIM` | DC-xxx |
| **Wp5Mixed** | `sdkconfig.wp5_mixed` | `generated_sites/lab_mixed` | `WP5_MIXED_SIM` | MIX-xxx, PERF5 |
| **Wp5MixedEsc** | `sdkconfig.wp5_mixed_esc` | `generated_sites/lab_mixed` | `WP5_MIXED_ESC_TEST` | MIX-007, MIX-008 |

Each mode has its own build directory, as before (`build_wp5_dc`, …).

# 9. Build and test workflow

1. Host: `cmake -S test/host -B build_host && cmake --build build_host && ctest --test-dir build_host`.
2. Tools: `python -m unittest discover tools/profile/tests` and `python -m unittest discover tools/wp4/tests`.
3. Dictionary: `python tools/profile/myems_profile_tool.py umd-check`.
4. Generate each site and check determinism, for example: `python tools/profile/myems_profile_tool.py generate --site config/site/lab_mixed.site.json --target sim --out-dir main/generated_sites/lab_mixed`, then `git diff --exit-code main/generated_sites`.
5. Firmware: `.\tools\build_firmware.ps1 -Mode Wp5Mixed`.
6. HIL: `python tools/wp4/myems_wp4_runner.py --console-port COM11 --rs485-port COM12 --suite mixed`.
7. Overnight: `.\tools\wp5\run_wp5_all.ps1` → `tools/test_results/wp5_all_<timestamp>/summary.txt`.

# 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| A hidden AC assumption in firmware code appears in M2 | The "data only" claim fails | Stop M2; fix in an M1-style commit with AC regression; record it as a framework finding |
| Role-driven simulator changes AC output | WP4 tests change meaning | SIM5-001 byte comparison before/after |
| Dictionary edit changes an existing runtime ID | Stored data and consumers break | U-06 against the frozen 1.0.0 copy; UMD5-002 |
| CRC change invalidates pending D9 HIL evidence | D9 evidence mixes configurations | Resolved in M1: the CRC does not cover version strings; WP4 CRCs unchanged |
| FC04 path has a defect not seen in WP4 HIL | DC readings fail | DCDEC host tests first; DC-001 checks the simulator log for FC04 |
| Mixed site exposes scheduling unfairness between devices with different intervals | One device starves | MIX-003/MIX-004 interval checks; PERF5-001 |

# 11. Development acceptance criteria

- DC meter added with no change under `main/` or `components/` other than generated tables (NOC-001).
- No dictionary parameter name in tool or simulator code outside the marked legacy items (NOC-002).
- All WP4 host, tool and HIL tests still pass on the WP5 commit.
- Both new build modes start polling with the expected configuration CRC and classification `SIMULATION_ONLY`.
- No new heap use after activation; no new task, queue or limit change.
