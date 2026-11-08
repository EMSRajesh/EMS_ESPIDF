---
title: "MyEMS DP-WP5 — DC Meter Reference Profile"
subtitle: "Design Specification v1.0"
---

| Document control | Value |
|---|---|
| Project | MyEMS — Energy Management System |
| Work package | DP-WP5 — DC Meter Reference Profile |
| Document version | 1.0 — first issue (M1 implementation notes folded in, 9 Nov 2026) |
| Status | Design proposal for review; not frozen |
| Platform | Waveshare ESP32-S3-POE-ETH-8DI-8DO |
| Toolchain | Windows 10/11, VS Code, ESP-IDF v6.1, FreeRTOS, Python 3.11 |
| Code baseline | DP-WP4 milestone D9 (`dp-wp4-d9`, once tagged) on `myems_rs485_modbus_rtu_09_057` |
| Companion documents | DP-WP5 Development Plan v1.0 ("Plan"); DP-WP5 Verification & Test Specification v1.0 ("Test Spec"); DP-WP4 Design Specification v1.1 ("WP4 Design") |

# 1. Purpose and scope

DP-WP4 built a profile-driven measurement path and proved it with a simulated three-phase AC meter. Its stated framework acceptance test is: **adding a supported device means adding a profile and a register map, not changing code** (WP4 Design Section 1).

DP-WP5 is the first real test of that claim. It adds a **simulation-only DC meter** (the kind used on battery strings, PV DC buses and DC feeders) and runs it alone and **side by side with the AC meter on one RS-485 bus**.

The work has two parts, kept strictly apart so the claim can be checked:

| Part | Content | May change code? |
|---|---|---|
| 5A Framework generalisation | Remove the AC-only knowledge that DP-WP4 left in the host tool and simulator; move it into the dictionary as data | Yes — host tool, simulator, build plumbing. **No change to measurement firmware logic.** AC behaviour must stay identical. |
| 5B DC meter data | DC dictionary entries, DC profile, DC register map, DC and mixed sites, generated tables, build configurations | **No.** Only data, generated files, build configs and tests. Checked by test NOC-001. |

**In scope:** DC voltage, current, power and import/export energy; device status; read-only FC03/FC04; one RS-485 bus; the Python simulator; a mixed AC + DC site.

**Out of scope:** real DC meter vendor maps; shunt or sensor temperature (no °C unit yet); insulation monitoring; derived values (for example V × I plausibility); control or write commands; Modbus TCP; persistent storage.

# 2. What DP-WP4 left AC-specific

A review of the D9 code base found the firmware already generic: AC parameter names and IDs appear only in the generated files (`ems_param_ids.h`, `ems_param_table.c`). The AC knowledge that remains is outside the firmware:

| # | Location | AC-specific item | Effect on a DC meter |
|---|---|---|---|
| 1 | Host tool, `CAPABILITIES` table | Capability → parameter list for rule V-11 is a Python constant with AC names only | A DC profile's capabilities would be rejected as unknown |
| 2 | Host tool, `ACTIVE_POWER_PARAM` | Counter plausibility (WP4 Design 8.5) looks for `ac.power.active.total` | DC counters get a plausibility limit of 0; any rollover would be treated as a discontinuity |
| 3 | Host tool, conventions | `pf_sign` is mandatory with values that only make sense for AC | A DC profile has to state a meaningless power factor convention |
| 4 | Host tool, schema versions | One `SUPPORTED_MAJOR/MINOR` pair for all file kinds | Cannot raise the profile schema minor without accepting unknown map or site minors |
| 5 | Simulator, value model | `NORMAL_VALUES`, P/Q/S/PF, import/export and load-change logic name AC parameters | DC meter would serve zeros and IMPORT/EXPORT would have no effect |
| 6 | Simulator, `OUT_OF_RANGE` | Hard-wired to `ac.frequency` = 30 Hz | No out-of-range case for a DC meter |
| 7 | Build plumbing | Kconfig choice `MYEMS_SIM_SITE` lists each site; CMake maps each choice to a folder | Every new site needs a Kconfig and CMake edit |

Items 1–7 are fixed in 5A. Items 1, 2, 5 and 6 are fixed by putting **parameter roles** and **capabilities** into the dictionary (Section 4); the tool and the simulator then work on roles, never on parameter names.

# 3. Design principles (in addition to WP4 Design Section 3)

1. **Names live in data.** No tool or simulator source may contain a dictionary parameter name such as `ac.frequency` or `dc.power`, except usage examples in help text and the marked `LEGACY_WP4` AC image of the simulator. Code refers to roles.
2. **AC behaviour is frozen.** 5A must not change any AC measurement, simulator output or test expectation. Only version strings in the generated files may change; the configuration CRCs stay the same.
3. **Additive versions.** All schema changes are MINOR: existing 1.1 files stay valid without edits.
4. **Same safety rules.** DC artifacts are `SIMULATION_ONLY`; their addresses are synthetic and must never be used with a physical meter.

# 4. Measurement dictionary 1.1

## 4.1 Changes

The dictionary file keeps its name (`config/umd/umd_v1.json`) and moves to `schema: myems.umd/1.1`, `umd_version: 1.1.0`. Two new optional sections, one new optional parameter field, and the DC parameters are added. Every v1.0 entry keeps its runtime ID, unit and value type.

| Addition | Form | Used by |
|---|---|---|
| `roles` | List of role names the tools understand (Section 4.3) | Validator (U-04) |
| Parameter field `role` | Optional; one role per parameter | Host tool (counter plausibility, V-16, V-17), simulator value model |
| `capabilities` | Map capability name → list of parameter IDs | Rule V-11 (replaces the Python table) |
| DC parameters | Domain `dc`, runtime IDs 0x02xx | DC profile and map |

## 4.2 DC parameters

| String ID | Runtime ID | Unit | Value type | Role | Meaning |
|---|---|---|---|---|---|
| `dc.voltage` | 0x0201 | V | float | `voltage` | DC bus voltage at the meter |
| `dc.current` | 0x0211 | A | float | `current.signed` | DC current; positive in the reference direction (Section 7) |
| `dc.power` | 0x0221 | kW | float | `power.active` | DC power; positive in the reference direction |
| `dc.energy.import` | 0x0240 | kWh | counter | `energy.import` | Cumulative energy in the reference direction |
| `dc.energy.export` | 0x0241 | kWh | counter | `energy.export` | Cumulative energy against the reference direction |

`dev.status.code` (0x0001) is shared. The DC numbering follows the AC pattern (voltages 0x02x0–0x020F, currents 0x021x, power 0x022x, energy 0x024x) and leaves room for per-string values later. No new unit is needed.

## 4.3 Roles

| Role | Meaning | Assigned in 1.1.0 |
|---|---|---|
| `power.active` | The device's active power, import positive. At most one per map (V-17). | `ac.power.active.total`, `dc.power` |
| `power.reactive` | Reactive power | `ac.power.reactive.total` |
| `power.apparent` | Apparent power | `ac.power.apparent.total` |
| `power.factor` | Power factor; sign follows active power when `pf_sign = SIGNED_BY_ACTIVE_POWER` | `ac.power.factor.total` |
| `current` | Magnitude-only current (RMS) | `ac.current.l1…l3` |
| `current.signed` | Current whose sign follows the power direction | `dc.current` |
| `voltage` | Voltage | `ac.voltage.l1_n…l3_n`, `dc.voltage` |
| `frequency` | System frequency | `ac.frequency` |
| `energy.import` | Counter that advances while active power ≥ 0 | `ac.energy.import.active`, `dc.energy.import` |
| `energy.export` | Counter that advances while active power < 0 | `ac.energy.export.active`, `dc.energy.export` |
| `status` | Device status word | `dev.status.code` |

Roles are **not** emitted into the firmware tables. The firmware does not need them.

## 4.4 Capabilities

The capability table moves unchanged from the tool into the dictionary, and the DC capabilities are added. Capability names are global and unique.

| Capability | Parameters |
|---|---|
| AC capabilities (12, unchanged) | As in WP4 (`phase_voltage` … `demand_measurement`) |
| `device_status` | `dev.status.code` (shared) |
| `dc_voltage` | `dc.voltage` |
| `dc_current` | `dc.current` |
| `dc_power` | `dc.power` |
| `dc_energy_import` | `dc.energy.import` |
| `dc_energy_export` | `dc.energy.export` |
| `dc_shunt_temperature` | none (reserved; must be `false` until a °C unit exists) |

## 4.5 Dictionary rules (new)

The tool validates the dictionary itself before any profile:

| ID | Rule |
|---|---|
| U-01 | String IDs and runtime IDs unique; runtime ID ≠ 0 |
| U-02 | Runtime ID high byte equals the domain of the ID prefix (`dc.*` → 0x02) |
| U-03 | Unit and value type are in the declared lists |
| U-04 | Every `role` is in `roles`; every capability lists only existing parameters |
| U-05 | No runtime ID listed under `retired` is reused |
| U-06 | **Compatibility:** compared with the last released dictionary (`config/umd/released/umd_1.0.0.json`), every released parameter still exists with the same runtime ID, unit and value type |

# 5. Profile and register map schema changes

## 5.1 Per-kind schema versions

The tool accepts, per file kind:

| Kind | Accepted | New in WP5 |
|---|---|---|
| `myems.umd` | 1.0, 1.1 | 1.1 adds `roles`, `role`, `capabilities` |
| `myems.profile` | 1.1, 1.2 | 1.2 adds `pf_sign: NOT_APPLICABLE` |
| `myems.regmap` | 1.1, 1.2 | 1.2 adds optional mapping field `sim_out_of_range` |
| `myems.site` | 1.1 | – |

Firmware activation checks the MAJOR only (WP4 Design V-01) and is unaffected.

## 5.2 Profile 1.2

- `conventions.pf_sign` gains `NOT_APPLICABLE`.
- `conventions.power_sign` stays `IMPORT_POSITIVE` (the only value); for DC it means "positive in the reference direction" (Section 7).
- `device_type` stays a free string; WP5 uses `DC_METER`.
- Rule V-11 now takes the capability table from the dictionary. The profile tool therefore needs dictionary schema 1.1 and refuses a 1.0 dictionary with a clear message; no built-in capability table is kept. (`umd_gen.py` still reads a 1.0 dictionary, so the released copy can be checked.)

## 5.3 Register map 1.2

`sim_out_of_range` (number, optional) on a mapping tells the simulator which engineering value to serve in `OUT_OF_RANGE` mode. It is a simulation hint: the generator ignores it, it is not in the C tables and does not affect the configuration CRC. If no mapping in a map has the field, the simulator falls back to the role `frequency` at 30.0 Hz, which keeps the unchanged AC map 1.1.0 behaving as in WP4.

## 5.4 New validation rules

| ID | Rule | Host | FW |
|---|---|---|---|
| V-16 | `pf_sign = NOT_APPLICABLE` ⇒ the map has no mapping with role `power.factor` | ✓ | – |
| V-17 | A map has at most one mapping with role `power.active`. A map with a `ROLLOVER` mapping must have exactly one. | ✓ | – |
| V-18 | `sim_out_of_range` must lie outside the mapping's [min, max] and be encodable in its type | ✓ | – |

## 5.5 Counter plausibility

`counter_max_rate` becomes "largest |min| or |max| of the map's `power.active` mapping" (0 if none). For the AC map the result is unchanged (1000 kW). For the DC map it is 3000 kW.

# 6. DC meter reference profile

## 6.1 Profile

```json
{
  "schema": "myems.profile/1.2",
  "profile_id": "DC_METER_SIM",
  "profile_version": "1.0.0",
  "classification": "SIMULATION_ONLY",
  "device_type": "DC_METER",
  "vendor": { "vendor_id": "MYEMS_SIMULATOR", "model_number": "SIM_DCMETER_V1",
              "protocol_document_reference": "MYEMS_SIMULATED_PROTOCOL_V1" },
  "protocol": "MODBUS_RTU",
  "register_map_ref": { "map_id": "MYEMS_SIM_DCMETER", "map_version": "1.0.0" },
  "comm_defaults": { "timeout_ms": 500, "max_retries": 1,
                     "comm_lost_threshold": 2, "probe_group": "status",
                     "probe_interval_ms": 30000 },
  "capabilities": { "dc_voltage": true, "dc_current": true, "dc_power": true,
                    "dc_energy_import": true, "dc_energy_export": true,
                    "device_status": true, "dc_shunt_temperature": false },
  "conventions": { "power_sign": "IMPORT_POSITIVE", "pf_sign": "NOT_APPLICABLE",
                   "ratio_handling": "METER_REPORTS_PRIMARY" }
}
```

`METER_REPORTS_PRIMARY` reflects how DC meters work: the shunt or Hall sensor ratio is set in the meter, which reports primary values. Rule V-12 therefore requires CT = PT = 1.0 in every site that uses this profile, and no mapping carries `APPLY_CT` or `APPLY_PT`.

## 6.2 Register map (simulation only)

All addresses are synthetic PDU addresses for the MyEMS simulator. **They are not manufacturer addresses and must never be used with a physical meter.**

The map is chosen to exercise what the AC map does not: input registers (FC04), FLOAT32 and word-swapped 32-bit values, negative 32-bit current, a group that does not start at address 0, and two function codes on one device.

| Group | FC | Block | Interval | Offset | Stale timeout |
|---|---|---|---|---|---|
| `measurements` | 04 | 0–5 (6 regs) | 4,000 ms | 300 ms | 9,000 ms |
| `energy` | 04 | 100–103 (4 regs) | 10,000 ms | 1,200 ms | 21,000 ms |
| `status` | 03 | 200 (1 reg) | 30,000 ms | 2,200 ms | 61,000 ms |

| ID | Address | Parameter | Type | Order | Scale | Unit | Min | Max | Flags | `sim_out_of_range` |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0–1 | dc.voltage | FLOAT32 | ABCD | 1 | V | 0 | 1500 | – | 1800.0 |
| 2 | 2–3 | dc.current | INT32 | CDAB | 0.001 | A | −2000 | 2000 | – | – |
| 3 | 4–5 | dc.power | FLOAT32 | CDAB | 1 | kW | −3000 | 3000 | – | – |
| 4 | 100–101 | dc.energy.import | UINT32 | CDAB | 0.01 | kWh | 0 | 4.3e7 | COUNTER | – |
| 5 | 102–103 | dc.energy.export | UINT32 | CDAB | 0.01 | kWh | 0 | 4.3e7 | COUNTER | – |
| 6 | 200 | dev.status.code | UINT16 | AB | 1 | – | 0 | 65535 | – | – |

The 4 s interval (not 2 s) follows from rule V-14: with the default timeout 500 ms and one retry, the worst-case failed transaction is about 3.1 s, which must be shorter than the device's smallest interval. A 2 s interval is a deliberate negative test (Test Spec GEN5-006).

Each mapping carries a `test_vector` (the NORMAL value), so the simulator's base image comes from the map, not from code.

## 6.3 Sites

| Site file | Devices | Purpose |
|---|---|---|
| `config/site/lab_dc_bench.site.json` | `DC_BATT_STRING_01`: slot 0, `DC_METER_SIM`, slave 3, CT = PT = 1 | DC meter alone |
| `config/site/lab_mixed.site.json` | `GRID_INCOMER_01`: slot 0, `AC_METER_3PH_SIM`, slave 1; `DC_BATT_STRING_01`: slot 1, `DC_METER_SIM`, slave 3 | Two device types on one bus |

Slave 3 is used for the DC meter in both files so that the simulator setup is the same; slave 2 stays reserved for `FEEDER_MCC_01` from WP4.

## 6.4 Timing budget (WP4 Design 9.6 formula, 9600 8E1)

| Group | Request / response bytes | t_frame | Nominal transaction | Load |
|---|---|---|---|---|
| DC `measurements` (6 regs) | 8 / 17 | 29 ms | ≈ 87 ms every 4 s | 2.2 % |
| DC `energy` (4 regs) | 8 / 13 | 24 ms | ≈ 82 ms every 10 s | 0.8 % |
| DC `status` (1 reg) | 8 / 7 | 17 ms | ≈ 75 ms every 30 s | 0.25 % |
| **DC meter total** | | | | **≈ 3.2 %** |
| **Mixed site (AC 3.2 % + DC 3.2 %)** | | | | **≈ 6.4 %** (limit 30 %) |

Worst-case failed transaction: T_wc ≈ 3.1 s < 4 s. Probe load with both devices OFFLINE: 2 × 3.1 / 30 ≈ 21 % (limit 50 %). The `budget` command is the authority; these figures are the expected output.

# 7. DC sign and electrical conventions

- **Reference direction:** each DC meter installation defines a reference direction, stated in the site documentation. By convention it is **from the DC bus into the monitored branch** (for a battery string: charging; for a load feeder: consumption). For a PV string meter the natural flow is then negative (export from the branch to the bus).
- **Current and power:** positive in the reference direction, negative against it. Both always have the same sign; the simulator keeps them consistent.
- **Energy:** `dc.energy.import` advances while power ≥ 0, `dc.energy.export` while power < 0. Both are normally non-decreasing counters; counter rules are those of WP4 Design 8.5. Net energy is not derived.
- **Voltage:** magnitude, never negative for a correctly wired meter (min 0).
- **Power factor and frequency:** not applicable; no mappings exist.
- **Reversed installation:** if a meter is installed against the convention, WP5 requires a correction in the site documentation, not in the data path. A per-device `reverse_direction` flag (which would also serve AC meters) is an open item (Section 12).

# 8. Simulator value model

The simulator (`tools/wp4/myems_ac_meter_sim.py`, kept under its existing name so the WP4 runners and scripts keep working) is changed to work on roles taken from the dictionary through `export-sim`.

| Mode | Rule by role | AC result (must equal WP4) | DC result |
|---|---|---|---|
| NORMAL | Every parameter = its map `test_vector`, else WP4 built-in AC value, else 0 | Test Spec WP4 4.1 | 800.0 V, 125.000 A, 100.0 kW, 2500.00 kWh, 0, 0 |
| IMPORT | NORMAL; the `energy.import` counter integrates \|`power.active`\| | +12.5 kW, import counter rises | +100.0 kW, import counter rises |
| EXPORT | Negate `power.active`, `current.signed` and (if `pf_sign` is signed) `power.factor`; `energy.export` integrates | −12.5 kW, PF −0.972 | −125.000 A, −100.0 kW, export counter rises |
| LOAD_CHANGE:s | Every s seconds draw factor f (seeded); scale `power.*`, `current`, `current.signed` by f; recompute `power.factor` = P/S if both exist; round to the mapping's resolution (3 decimals for FLOAT32) | Same sequence as WP4 for the same seed | P and I scale together; V constant |
| OUT_OF_RANGE | Mapping with `sim_out_of_range` gets that value; fallback `frequency` → 30.0 Hz | 30.00 Hz | `dc.voltage` = 1800.0 V |
| COUNTER_RESET | `energy.import` → 0 once, then IMPORT | As WP4 | As AC |
| Fault modes | Unchanged (protocol level, role-independent) | As WP4 | As AC |

The simulator rejects a `set` command whose value its mapping cannot encode, as today.

# 9. Firmware

No change to the measurement firmware logic is expected. The DC meter reaches the firmware only as generated tables.

| Area | WP5 change |
|---|---|
| `main/*.c`, `components/*` (excluding generated folders) | None in 5B. If 5A review finds an AC assumption in firmware code, it is fixed in M1 with AC regression evidence, never in M2. |
| Generated tables | Regenerated for all sites. Only the version strings change (tool 1.1.0, dictionary 1.1.0, inputs hash); the configuration CRC covers table contents, not versions, so the WP4 site CRCs stay the same (0x4B8B5924, 0xE3FF2DF7, 0x5A2CC286). The dictionary C files (`components/ems_measurement/include/ems_param_ids.h`, `src/ems_param_table.c`) are generated by `umd_gen.py` and change with the dictionary. |
| Site selection | Kconfig choice `MYEMS_SIM_SITE` is replaced by a string `MYEMS_SIM_SITE_DIR` (default `generated`); `main/CMakeLists.txt` uses it directly. Existing sdkconfig files set the same folders as before. A new site then needs only its generated folder and an sdkconfig file. |
| Build modes | New: `Wp5Dc` (`sdkconfig.wp5_dc`, site `generated_sites/lab_dc_bench`) and `Wp5Mixed` (`sdkconfig.wp5_mixed`, site `generated_sites/lab_mixed`); both set `MYEMS_CONFIG_TARGET_SIM=y`. Boot banner `BUILD_VARIANT=WP5_DC_SIM` / `WP5_MIXED_SIM`. |
| Resources | Mixed site: 2 devices, 6 poll entries (limit 16), 20 mappings (limit 128), at most 14 parameters per device (limit 32). No limit changes. |

The FC04 path (scheduler request, master, routing check of function code in acquisition) exists since WP4 but has only been exercised on the host and by the decoder-test map. WP5 is its first sustained HIL use.

# 10. Safety and security boundaries

WP4 Design Section 12 applies unchanged. In addition:

- A DC measurement that is not GOOD must never be read as 0 A or 0 kW. For a battery string this matters directly: "no current" must not be inferred from a lost meter.
- The DC map's addresses, ranges and sign convention are for the simulator only. A real DC meter needs its datasheet map, confirmation of the shunt ratio configuration, and confirmation of the reference direction at installation.

# 11. Design requirements

| ID | Requirement | Section |
|---|---|---|
| DR5-01 | Dictionary 1.1 adds the DC parameters of Section 4.2; every 1.0 entry is unchanged | 4.1, 4.2 |
| DR5-02 | Capabilities and roles live in the dictionary; no tool or simulator source names a dictionary parameter | 3, 4.3, 4.4 |
| DR5-03 | Dictionary rules U-01…U-06 are enforced before generation | 4.5 |
| DR5-04 | Per-kind schema versions; profile 1.2 and regmap 1.2 accepted; all 1.1 files still accepted unchanged | 5.1–5.3 |
| DR5-05 | Rules V-16, V-17 and V-18 | 5.4 |
| DR5-06 | Counter plausibility from the `power.active` role; AC value unchanged | 5.5 |
| DR5-07 | DC profile, map and sites as in Section 6; classification SIMULATION_ONLY | 6 |
| DR5-08 | DC sign convention of Section 7 | 7 |
| DR5-09 | No CT/PT ratio is applied to DC values; V-12 enforced | 6.1 |
| DR5-10 | Simulator value model by role; AC output identical to WP4 for every mode and seed | 8 |
| DR5-11 | Adding the DC meter (5B) changes no file under `main/` or `components/` other than generated files (site tables and the two dictionary C files) | 1, 9 |
| DR5-12 | AC and DC devices on one bus keep independent state, quality and timing; no cross-talk | 6.3, 9 |
| DR5-13 | Timing budget V-14 holds for the DC and mixed sites; a 2 s DC interval is rejected | 6.4 |
| DR5-14 | All DP-WP4 requirements (DR-01…DR-24) continue to hold | – |
| DR5-15 | New sites need no Kconfig or CMake source change | 9 |

# 12. Assumptions and open items

| # | Item | Owner / closure |
|---|---|---|
| 1 | DP-WP4 D9 HIL evidence on the D9 commit | Closed for configuration: M1 keeps the WP4 CRCs, so the D9 HIL run may be done on the D9 commit (c987e4e) or later |
| 2 | COM-WP2.4 closure and CR-D8-01 still gate hardware acceptance (as for WP4) | COM track |
| 3 | `reverse_direction` site flag for meters installed against the convention (AC and DC) | Proposed for a later site schema 1.2; not in WP5 |
| 4 | Temperature parameters and a °C unit | Dictionary 1.2 with the BMS work (DP-WP7) |
| 5 | Real DC meter selection, response latency and register map | When the first real DC meter is chosen |
| 6 | Whether the runner's per-suite analyses can be made fully table-driven from `export-sim` test vectors | Plan M4; partial use in WP5 |
