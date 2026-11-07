# DP-WP4 milestone D3 — Profiles, validation tool and code generation

Plan v1.1 milestone D3 (sub-package 4A). Tag after acceptance: `dp-wp4-d3`.

## Delivered

| Path | Content |
|---|---|
| `config/schema/*.schema.json` | JSON Schemas (draft 2020-12) for profile, register map and site files; used by editors. The tool enforces the same rules itself. |
| `config/profiles/` | `AC_METER_3PH_SIM` 1.1.0 (SIMULATION_ONLY), `DECODER_TEST_SIM` 1.1.0 (TEST_ONLY) |
| `config/maps/` | `MYEMS_SIM_ACMETER_3PH` 1.1.0 (Design 8.2), `MYEMS_SIM_DECODER_TEST` 1.1.0 (Test Spec 4.2) |
| `config/site/` | `lab_bench`, `lab_bench_2dev`, `lab_ratio_ct200`, `lab_decoder_test` |
| `components/ems_profile/` | `ems_config_types.h` (table types, limits), `ems_config_crc.c` (firmware CRC32 of the tables) |
| `tools/profile/myems_profile_tool.py` | `validate`, `generate`, `budget`, `export-sim`; rules V-01…V-15; standard library only |
| `main/generated/ems_config_gen.{h,c}` | Tables for `lab_bench`, target `sim`. **Not yet in `main/CMakeLists.txt`** (added in D7, activation). |
| `test/host/generated/decoder_test/` | Tables for `lab_decoder_test`, used only by host tests |
| `tools/profile/tests/test_profile_tool.py` | 46 tests: GEN-001…009, CFG-001…016, extra rules |
| `test/host/test_ems_config_*.c` | Generated tables in C: CRC tool = firmware, firmware validation, Test Spec 4.1, 4.2, 4.3 vectors end to end |

## Exit criteria

| # | Criterion | How checked | Result |
|---|---|---|---|
| 1 | All shipped sites validate | `validate` for the 4 sites | |
| 2 | `--target prod` rejects simulation config (GEN-004) | command below, exit 1, no files | |
| 3 | Budget for v1.1 defaults: T_wc ≈ 3.1 s, load ≈ 3.2 % (GEN-006) | `budget` | |
| 4 | Python tests pass (17 UMD + 46 tool) | `unittest` | |
| 5 | Generated files in the repo are up to date | `generate --check` (also a unit test) | |
| 6 | Host C tests pass: 4 executables | `ctest` | |
| 7 | CRC computed by the tool equals the CRC computed by the firmware code | `test_GEN004_stored_crc_matches_firmware_crc` | |
| 8 | Production firmware still builds (REG-01 part) | `idf.py build` | |

Verified before delivery (Linux): gcc, clang, gcc with x87 maths (`-mfpmath=387`, same as your mingw32), AddressSanitizer/UBSan; 9 deliberate tool defects all caught by the tests.

## Design decisions recorded in D3

- **Error format:** `ERROR [V-xx] <file>: <field>: <message>`; exit code 1; nothing is written.
- **Classification:** a build takes the highest classification of all its profiles and maps (TEST_ONLY > SIMULATION_ONLY > PRODUCTION). `--target prod` accepts only PRODUCTION.
- **Ranges are after the ratio** (as Test Spec 4.3: 600 raw × 0.01 × CT 200 = 1200 A → OUT_OF_RANGE with max 650).
- **Counter plausibility limit** (`counter_max_rate_per_hour`) = largest |active power| allowed by the map, i.e. per hour in kWh, **before** CT/PT. The acquisition (D6) multiplies it by the device's ratio factor.
- **Capabilities ↔ mappings (V-11)** in both directions: a capability set to true needs a mapping, and every mapping must belong to a true capability.
- **COUNTER flag** must be set exactly for dictionary parameters of value type `counter`. ROLLOVER only on UINT32 counters; `bit_mask` only on BITFIELD16; 16-bit types accept only AB/BA.
- **Slots** must be 0…n−1 in file order.
- **Site `comm_overrides`** (timeout, retries, comm-lost threshold, probe interval) override the profile defaults per device and are checked by the same rules.
- **Reproducible output:** no timestamps or git hash in generated files; `inputs_sha256` (first 16 hex) over the canonical JSON of all inputs; LF line endings.
- **CRC32** (zlib/IEEE) over a little-endian canonical serialisation of the table contents (not the C struct memory), so host and ESP32 agree regardless of padding.

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | |
| Commit | |
| Config CRC `lab_bench` | 0x4B8B5924 (expected) |
| Result | |
