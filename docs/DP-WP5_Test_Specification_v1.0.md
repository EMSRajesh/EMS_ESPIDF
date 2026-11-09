---
title: "MyEMS DP-WP5 — DC Meter Reference Profile"
subtitle: "Verification & Test Specification v1.0"
---

| Document control | Value |
|---|---|
| Project | MyEMS — Energy Management System |
| Work package | DP-WP5 — DC Meter Reference Profile |
| Document version | 1.0 — first issue (M1 implementation notes folded in, 9 Nov 2026) |
| Status | Test planning artifact; not frozen |
| Platform | Waveshare ESP32-S3-POE-ETH-8DI-8DO; USB-RS485 adapter; Windows host |
| Companion documents | DP-WP5 Design Specification v1.0 ("Design"); DP-WP5 Development Plan v1.0 ("Plan"); DP-WP4 Test Specification v1.1 ("WP4 Test Spec") |

# 1. Purpose

Defines how DP-WP5 is verified. Test levels L1–L6, the safety assumptions, the record format, evidence and execution record are those of the WP4 Test Spec (Sections 2, 3, 5, 22, 24) and are not repeated. This document adds the DC test data, the new test groups and the WP5 acceptance gate.

Two questions are answered separately:

1. **Does the DC meter work?** (DCDEC, SIM5, DC, MIX, PERF5)
2. **Was it added without code?** (NOC, GEN5-010, SIM5-001, REG5)

# 2. Test data

## 2.1 DC meter vectors (map MYEMS_SIM_DCMETER 1.0.0, NORMAL image)

| Parameter | Address | Type / order | Register words (hex) | Raw | Expected value |
|---|---|---|---|---|---|
| dc.voltage | 0–1 | FLOAT32 ABCD | 4448 0000 | 800.0 | 800.0 V |
| dc.current (import) | 2–3 | INT32 CDAB | E848 0001 | 125000 | 125.000 A |
| dc.current (export) | 2–3 | INT32 CDAB | 17B8 FFFE | −125000 | −125.000 A |
| dc.power (import) | 4–5 | FLOAT32 CDAB | 0000 42C8 | 100.0 | 100.0 kW |
| dc.power (export) | 4–5 | FLOAT32 CDAB | 0000 C2C8 | −100.0 | −100.0 kW |
| dc.energy.import | 100–101 | UINT32 CDAB | D090 0003 | 250000 | 2,500.00 kWh |
| dc.energy.export | 102–103 | UINT32 CDAB | 0000 0000 | 0 | 0.00 kWh |
| dev.status.code | 200 | UINT16 AB | 0000 | 0 | 0 |

## 2.2 DC edge vectors (host only)

| Case | Parameter | Words (hex) | Expected |
|---|---|---|---|
| E1 | dc.current = 0 | 0000 0000 | 0.000 A, GOOD (valid zero) |
| E2 | dc.current = −2000.000 (min) | 7B80 FFE1 | GOOD (limit inclusive) |
| E3 | dc.current = −2000.001 | 7B7F FFE1 | OUT_OF_RANGE, value kept |
| E4 | dc.voltage = 1500.0 (max) | 44BB 8000 | GOOD |
| E5 | dc.voltage = 1800.0 (`sim_out_of_range`) | 44E1 0000 | OUT_OF_RANGE |
| E6 | dc.power = quiet NaN | 0000 7FC0 | INVALID; no value published |
| E7 | dc.power = 3000.0 (max) | 8000 453B | GOOD |

## 2.3 Expected budget (`budget --site lab_mixed`)

| Item | Expected |
|---|---|
| DC T_wc (500 ms, 1 retry) | ≈ 3.1 s, < 4 s interval |
| DC nominal load | ≈ 3.2 % |
| Mixed nominal load | ≈ 6.4 % (≤ 30 %) |
| Mixed probe load, both OFFLINE | ≈ 21 % (≤ 50 %) |

# 3. Dictionary (U, L2)

| ID | Test | Expected result |
|---|---|---|
| U-001 | `umd-check` on the WP5 dictionary | Exit 0 |
| U-002 | Duplicate runtime ID | Rejected, U-01 |
| U-003 | `dc.voltage` with runtime ID 0x0101 | Rejected, U-02 (domain mismatch) |
| U-004 | Unit `mV` not in the unit list | Rejected, U-03 |
| U-005 | Unknown role `power.real` | Rejected, U-04 |
| U-006 | Capability lists a missing parameter | Rejected, U-04 |
| U-007 | Retired runtime ID reused | Rejected, U-05 |
| U-008 | Change `ac.power.active.total` unit to W, or its runtime ID, or remove it | Rejected, U-06 against `released/umd_1.0.0.json` |

# 4. Host tool and generation (GEN5, L2)

| ID | Test | Expected result |
|---|---|---|
| GEN5-001 | All WP4 sites validate and generate with the WP5 tool | Exit 0; configuration CRCs equal the D9 values (0x4B8B5924, 0xE3FF2DF7, 0x5A2CC286, decoder 0x5A334BC5) |
| GEN5-002 | Profile 1.1 and regmap 1.1 files unchanged | Accepted |
| GEN5-003 | Profile `myems.profile/1.3`, regmap `myems.regmap/1.3`, site `myems.site/1.2` | Each rejected, naming the kind and the supported range |
| GEN5-004 | AC counter plausibility after the role change | Generated max rate for the AC device = 1000 kW (unchanged) |
| GEN5-005 | Validate `lab_dc_bench.site.json` | Exit 0; 1 device, 3 groups, 6 mappings |
| GEN5-006 | DC map with `measurements` interval 2000 ms | V-14 error (T_wc ≈ 3.1 s ≥ 2 s) |
| GEN5-007 | `budget --site lab_mixed` | Matches Section 2.3 |
| GEN5-008 | Generate the DC and mixed sites twice | Byte-identical output |
| GEN5-009 | `export-sim` for the mixed site | Layout 1.1; roles, `pf_sign` and `sim_out_of_range` present; registers identical to the C tables (WP4 GEN-008 method) |
| GEN5-010 | Diff of AC site generated files, D9 vs M1 | Only the tool version, dictionary version and inputs hash lines differ (CRC unchanged) |

# 5. Configuration validation (CFG5, L2)

| ID | Input defect | Rule | Expected result |
|---|---|---|---|
| CFG5-001 | Valid DC and mixed sites | – | Accepted |
| CFG5-002 | DC profile with `pf_sign: NOT_APPLICABLE` and a `power.factor` mapping added to its map | V-16 | Rejected |
| CFG5-003 | AC profile with `pf_sign: NOT_APPLICABLE` | V-16 | Rejected (AC map has a PF mapping) |
| CFG5-004 | DC map with two mappings of role `power.active` | V-17 | Rejected |
| CFG5-005 | DC counter with `ROLLOVER` and the `dc.power` mapping removed | V-17 | Rejected |
| CFG5-006 | `sim_out_of_range` = 1000.0 for `dc.voltage` (inside 0–1500) | V-18 | Rejected |
| CFG5-007 | DC site with CT 200 | V-12 | Rejected (`METER_REPORTS_PRIMARY`) |
| CFG5-008 | DC profile with capability `dc_shunt_temperature: true` | V-11 | Rejected (no mapping exists) |

# 6. Dictionary in firmware tables (UMD5, L1)

| ID | Test | Expected result |
|---|---|---|
| UMD5-001 | DC mappings resolve to runtime IDs 0x0201, 0x0211, 0x0221, 0x0240, 0x0241 | As Design 4.2 |
| UMD5-002 | All AC and dev runtime IDs | Identical to WP4 (`ems_param_ids.h` diff shows additions only) |
| UMD5-003 | Units of published DC parameters | V, A, kW, kWh |
| UMD5-004 | Host config tests for the DC and mixed sites | Activation OK; 3 and 6 poll entries; CRC as generated |

# 7. DC decoding and conversion (DCDEC, L1)

Run through the host engine (`ems_regmap_convert`, `ems_counter_update`) with the generated DC tables.

| ID | Test | Expected result |
|---|---|---|
| DCDEC-001 | NORMAL vectors 2.1 | Exact values |
| DCDEC-002 | Export vectors 2.1 | −125.000 A, −100.0 kW |
| DCDEC-003 | Edge vectors E1–E7 | As listed |
| DCDEC-004 | No ratio applied (CT = PT = 1, no ratio flags) | Values equal the scaled raw values |
| DCDEC-005 | Group start ≠ 0 (energy at 100) | Offsets computed from the group start; correct values |
| DCDEC-006 | DC import counter 2,500.00 → 2,500.28 | Delta +0.28 kWh |
| DCDEC-007 | DC import counter reset to 0 | GOOD 0.00; no delta; discontinuity event |
| DCDEC-008 | FC04 entries | Generated poll groups carry FC 4 for `measurements` and `energy`, FC 3 for `status`; routing check accepts FC 4 results |

# 8. Simulator (SIM5, L3)

| ID | Test | Expected result |
|---|---|---|
| SIM5-001 | **AC regression:** every WP4 mode (incl. LOAD_CHANGE seed 1) for 120 s, old vs new simulator | Identical register images for every request |
| SIM5-002 | DC NORMAL | Image equals vectors 2.1 (import) |
| SIM5-003 | DC FC04 on `measurements`/`energy`, FC03 on `status` | Served; FC03 on address 0 and FC04 on address 200 → exception 02 |
| SIM5-004 | DC IMPORT | +100.0 kW, +125.000 A; import counter +0.01 kWh about every 0.36 s |
| SIM5-005 | DC EXPORT | −100.0 kW, −125.000 A; export counter rises; import counter frozen |
| SIM5-006 | DC LOAD_CHANGE:10 seed 1 | Current and power scale by the same factor; V constant; sequence identical across two runs |
| SIM5-007 | DC OUT_OF_RANGE | `dc.voltage` = 1800.0 V; other values NORMAL |
| SIM5-008 | DC COUNTER_RESET | Import counter → 0 once, then rises |
| SIM5-009 | Mixed: slave 1 EXPORT, slave 3 IMPORT at the same time | Signs independent per slave |
| SIM5-010 | Old layout file `myems.simlayout/1.0` | Simulator refuses to start with a clear message |

# 9. HIL, DC meter alone (DC, L4, build `Wp5Dc`)

| ID | Scenario | Expected result |
|---|---|---|
| DC-001 | Boot | Banner: site LAB_DC_BENCH, classification SIMULATION_ONLY, CRC 0xCC22DC64; polling starts; simulator log shows FC04 and FC03 requests to slave 3 only |
| DC-002 | NORMAL | All six parameters GOOD with the 2.1 values |
| DC-003 | Group intervals (simulator receive times, first attempts) | `measurements` 4 s, `energy` 10 s, `status` 30 s: mean interval within ± 0.1 s, every interval within ± 0.5 s. Offsets are not measured on the board (the firmware start time is not visible to the simulator) |
| DC-004 | IMPORT | Power and current positive GOOD; import counter increases between energy polls |
| DC-005 | EXPORT | Power and current negative GOOD; export counter increases; import counter unchanged |
| DC-006 | OUT_OF_RANGE | `dc.voltage` OUT_OF_RANGE; other parameters stay GOOD |
| DC-007 | COUNTER_RESET | One discontinuity event; then deltas from the new baseline |
| DC-008 | TIMEOUT | After 2 failed transactions (≈ 6.2 s): OFFLINE, all parameters COMM_LOST; no GOOD 0 A / 0 kW at any time |
| DC-009 | OFFLINE_THEN_RECOVER:60 | Probes on `status` every 30 s; first OK → ONLINE; each group back to GOOD on its own read |
| DC-010 | EXCEPTION:02 | Device stays ONLINE; affected group INVALID; exception code in diagnostics |
| DC-011 | BAD_CRC | As DC-008 |
| DC-012 | SLOW:400 | Responses inside the 500 ms timeout; all parameters stay GOOD; no timeouts or retries counted |

# 10. HIL, mixed site (MIX, L4, builds `Wp5Mixed`, `Wp5MixedEsc`)

| ID | Scenario | Expected result |
|---|---|---|
| MIX-001 | Boot | Banner: site LAB_MIXED, CRC 0xC04114E4; two devices (`AC_METER`, `DC_METER`); 6 poll entries |
| MIX-002 | Both NORMAL, 10 min | AC values = WP4 Test Spec 4.1, DC values = 2.1, all GOOD; records under the right slot; no routing errors |
| MIX-003 | AC TIMEOUT, DC NORMAL | AC OFFLINE; DC stays GOOD; DC `measurements` interval within 4 s ± 3.2 s |
| MIX-004 | AC NORMAL, DC TIMEOUT | DC OFFLINE; AC stays GOOD; AC `electrical` interval within 5 s ± 3.2 s |
| MIX-005 | AC EXPORT, DC IMPORT | AC −12.5 kW, DC +100.0 kW; each export/import counter per its own sign |
| MIX-006 | Never two outstanding requests on the bus; at most one per slave | Checked over the whole simulator log |
| MIX-007 | `Wp5MixedEsc`, forced transport recovery FAILED (as WP4 ESC) | BUS_FAULT; both devices OFFLINE, all parameters COMM_LOST; `EVT_BUS_FAULT` |
| MIX-008 | `Wp5MixedEsc`, bus recovers | `EVT_BUS_OK`; both devices recover through probing; BLOCKED items as for WP4 ESC (CR-D8-01) |

# 11. "Data, not code" (NOC, L2)

| ID | Test | Expected result |
|---|---|---|
| NOC-001 | `git diff --stat dp-wp5-m1 dp-wp5-m2 -- main components` | Only paths under `main/generated*/` and `components/ems_measurement/include/ems_param_ids.h`, `components/ems_measurement/src/ems_param_table.c` (generated by `umd_gen.py`) |
| NOC-002 | Search tool and simulator sources for every dictionary parameter ID | Found only in lines marked `LEGACY_WP4` or in help-text usage examples (`e.g.`) |
| NOC-003 | `git diff --stat dp-wp5-m1 dp-wp5-m2 -- tools/profile tools/wp4/myems_ac_meter_sim.py` | Empty |

# 12. Performance and soak (PERF5, L6, build `Wp5Mixed`)

| ID | Test | Acceptance |
|---|---|---|
| PERF5-001 | Bus load, mixed NORMAL, 1 h | Measured load within ±50 % of `budget` (≈ 6.4 %); no overlapping transactions |
| PERF5-002 | Heap | Free heap at start and end of the soak differ by less than 1 KB |
| PERF5-003 | Stacks | Every task ≥ 1024 B minimum free stack |
| PERF5-004 | Queues | Master outstanding and completion backlog high-water ≤ 4 in NORMAL |
| PERF5-005 | Soak 8 h, AC and DC cycling NORMAL / TIMEOUT / OFFLINE_THEN_RECOVER / EXPORT every 10 min, offset by 5 min between devices | No reset; every quality transition explained by a simulator mode change; acquisition processing max ≤ 2 ms per result |

The 24 h soak of DP-WP4 covers framework stability; WP5 adds data only, so an 8 h mixed soak is sufficient. If NOC-001 fails (code had to change), the soak returns to 24 h.

# 13. Regression (REG5, L4/L5, on the WP5 commit)

| ID | Test | Status |
|---|---|---|
| REG5-001 | All host tests (ctest) and tool tests | Mandatory |
| REG5-002 | Production, SelfTest and all WP4/WP5 build modes build from a fresh clone | Mandatory |
| REG5-003 | WP4 suites `d6`, `d7`, `reject`, `two_dev`, `ratio` with the WP4 builds | Mandatory; same results as D9 |
| REG5-004 | WP4 suite `esc` with `Wp4Esc` | PASS with the same BLOCKED items as D8 |
| REG5-005 | COM self-test and WP24 A–I campaign | Mandatory (WP4 REG-02/03) |
| REG5-006 | Partition table and flash layout | Unchanged |

# 14. Traceability

| Design requirement | Tests |
|---|---|
| DR5-01 DC dictionary, 1.0 unchanged | UMD5-001, UMD5-002, U-008 |
| DR5-02 roles and capabilities as data | NOC-002, GEN5-004, CFG5-008 |
| DR5-03 dictionary rules | U-001…U-008 |
| DR5-04 schema versions | GEN5-002, GEN5-003 |
| DR5-05 V-16…V-18 | CFG5-002…CFG5-006 |
| DR5-06 counter plausibility by role | GEN5-004, CFG5-005 |
| DR5-07 DC artifacts | GEN5-005, DC-001, DC-002 |
| DR5-08 sign convention | DCDEC-002, DC-004, DC-005, MIX-005 |
| DR5-09 no ratio on DC | DCDEC-004, CFG5-007 |
| DR5-10 role-driven simulator, AC identical | SIM5-001…SIM5-010 |
| DR5-11 DC added without code | NOC-001, NOC-003 |
| DR5-12 mixed bus isolation | MIX-002…MIX-006 |
| DR5-13 timing budget | GEN5-006, GEN5-007, PERF5-001 |
| DR5-14 WP4 requirements hold | REG5-001…REG5-006 |
| DR5-15 new site without Kconfig/CMake edit | NOC-001 (no Kconfig/CMake change in M2) |

# 15. Acceptance gate

DP-WP5 is accepted when, on one tagged commit:

1. All L1 and L2 tests pass, including U, GEN5, CFG5, UMD5, DCDEC and NOC.
2. SIM5 passes, including the AC byte comparison SIM5-001.
3. DC-001…DC-012 and MIX-001…MIX-006 pass; MIX-007/008 pass or are BLOCKED only against CR-D8-01.
4. PERF5-001…PERF5-005 and REG5-001…REG5-006 pass.
5. COM-WP2.4 is closed (otherwise hardware results stay provisional, as for WP4).
6. No open critical or high-severity defect.
7. The acceptance record states that no real DC meter or vendor register map has been validated, and states the NOC-001 result explicitly.
