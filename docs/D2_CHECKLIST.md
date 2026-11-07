# DP-WP4 milestone D2 — Register map engine (host)

Reference: Development Plan v1.1 §10 (D2), Design Specification v1.1 §7 and §8.5. Exit criteria: **DEC-\*, CONV-\*, ENG-\* pass on host; branch coverage ≥ 90 % for `ems_decode.c` and `ems_counter.c`.**

## Delivered files

| File | Status | Purpose |
|---|---|---|
| `components/ems_register_map/CMakeLists.txt` | new | ESP-IDF component, pure C |
| `components/ems_register_map/include/ems_register_map.h` | new | Types (data type, word order, mapping, poll group, ratio), decode, convert, validate |
| `components/ems_register_map/include/ems_counter.h` | new | Energy counter tracker |
| `components/ems_register_map/src/ems_decode.c` | new | Extract + decode UINT16/INT16/UINT32/INT32/FLOAT32/BITFIELD16 in ABCD/CDAB/BADC/DCBA |
| `components/ems_register_map/src/ems_convert.c` | new | Scale/offset, CT/PT ratio, range check, structural validation |
| `components/ems_register_map/src/ems_counter.c` | new | Delta, reset/discontinuity, rollover with plausibility |
| `test/host/test_ems_register_map.c` | new | 26 tests: DEC-001…010, CONV-001…008, validation |
| `test/host/test_ems_counter.c` | new | 8 tests: ENG-001…007 |
| `test/host/test_main_regmap.c` | new | Runner for the second test executable |
| `test/host/CMakeLists.txt` | **changed** | Adds `ems_register_map` library and `test_ems_register_map` |
| `test/host/README.md` | **changed** | Lists both test executables |
| `docs/D2_CHECKLIST.md` | new | This file |

No firmware source (`main/`, COM components, `ems_measurement`) was changed. ESP-IDF compiles the new component in every build mode, which checks it for the ESP32-S3.

## Deviations from the design (recorded)

| Design v1.1 | D2 as delivered | Reason |
|---|---|---|
| §7.2 mapping entry 28 bytes, `float scale/offset/min/max` | 48 bytes, **`double`** | `float` 0.01 is off by 1.5e-8 relative: on a 42,949,672.95 kWh counter that is about 0.6 kWh, failing CONV-007. 128 mappings × 48 B ≈ 6 KB flash. |
| §7.3 step 3 `eng = raw × scale + offset` | Same formula, but when 1/scale is a whole number (10, 100, 1000 …) the engine divides by it | Gives the value nearest to the decimal number, e.g. 2300 × 0.001 = 2.3000000000000003 but 2300 / 1000 = 2.3 |
| §7.1 16-bit orders "AB, BA" | `EMS_ORDER_AB` / `EMS_ORDER_BA` are aliases of ABCD / BADC; CDAB and DCBA are rejected for 16-bit types | One enum for all types |

Design §7.2 should be updated to match when the design is next revised.

## Design decisions made in D2 (for review)

1. A response with a register count different from the poll group's count invalidates the whole group (`ems_regmap_response_complete`, DEC-009). The caller (acquisition, D6) checks this first and publishes nothing from such a response.
2. Range limits are inclusive and are checked after the ratio is applied.
3. A non-finite result after scaling (overflow) is INVALID, not OUT_OF_RANGE.
4. Counters: only unsigned types (UINT16, UINT32) may carry the counter flag; rollover only for UINT32 counters.
5. Counter delta for a normal increase is computed from the engineering values (keeps decimal exactness); a rollover delta from the raw count difference × units per count.
6. Rollover plausibility: delta ≤ 2 × max rate × elapsed hours (Design §8.5). Zero elapsed time never justifies a rollover. A plausibility limit ≤ 0 accepts any rollover.
7. Structural validation (`ems_regmap_validate`) also enforces: poll interval ≥ 1000 ms, stale timeout > interval, group inside the 16-bit address space, unique mapping IDs. It is the firmware side of rules V-05…V-09; the host profile tool (D3) checks the same and more.

## Steps

| # | Step | Command (ESP-IDF v6.1 PowerShell, project root) | Expected |
|---|---|---|---|
| 1 | Host tests (rebuild picks up the new CMakeLists) | `cmake -S test/host -B build_host -G Ninja -DCMAKE_C_COMPILER=gcc` then `cmake --build build_host` then `ctest --test-dir build_host --output-on-failure` | no warnings; `100% tests passed, 0 tests failed out of 2` |
| 2 | Per-test output (optional) | `.\build_host\test_ems_register_map.exe` | `34 Tests 0 Failures 0 Ignored` |
| 3 | D1 still OK | `python tools/profile/umd_gen.py --check` | `OK: ...` |
| 4 | Firmware still builds | `.\tools\build_firmware.ps1 -Mode Production` | build succeeds; `ems_register_map` in the component list |
| 5 | Commit (you) | `git add components/ems_register_map test/host docs/D2_CHECKLIST.md` then `git commit -m "D2: register map engine (decode, convert, validate, counters), host-tested"` | |
| 6 | Tag (you) | `git tag -a dp-wp4-d2 -m "DP-WP4 D2 complete"` | |

## Test coverage of D2

| Test ID | Test function(s) |
|---|---|
| DEC-001 | `test_DEC001_uint16` (incl. D11, BA order) |
| DEC-002 | `test_DEC002_int16_positive_and_negative` (incl. D10) |
| DEC-003 | `test_DEC003_int32_abcd_negative` (D5) |
| DEC-004 | `test_DEC004_int32_cdab` (D6) |
| DEC-005 | `test_DEC005_uint32_abcd_and_cdab` (D7) |
| DEC-006 | `test_DEC006_float32_all_four_orders` (D1–D4) |
| DEC-007 | `test_DEC007_float32_nan_and_inf_are_invalid` (D8, D9, −Inf, sNaN) |
| DEC-008 | `test_DEC008_bitfield_mask_and_shift` |
| DEC-009 | `test_DEC009_short_response_is_rejected_for_the_whole_group` |
| DEC-010 | `test_DEC010_last_registers_of_a_125_register_group` (run under ASan) |
| CONV-001…008 | `test_CONV00x_*`, plus all 14 Test Spec §4.1 vectors and exact-decimal checks |
| ENG-001…007 | `test_ENG00x_*` |
| CFG-006/008/009 (firmware side) | `test_validate_rejects_bad_mappings` |
| ENG-008 | HIL with simulator `COUNTER_RESET` — D6/D9 |

Verified on Linux before delivery: gcc 13 and clang with `-Werror`; gcc AddressSanitizer + UBSan; mutation check (10 deliberate bugs, each caught by at least one test); coverage:

| File | Branches taken | Lines |
|---|---|---|
| `ems_decode.c` | 90.3 % | 96.8 % |
| `ems_counter.c` | 100 % | 100 % |
| `ems_convert.c` | 85.1 % | 88.2 % |

Run against a Unity-compatible stand-in; **your run with ESP-IDF's Unity on Windows is the acceptance run.**

## Exit record

| Item | Result | Note |
|---|---|---|
| Step 1 host tests (2 executables) | | |
| Step 3 `--check` | | |
| Step 4 firmware build | | |
| Commit / tag `dp-wp4-d2` | | |
