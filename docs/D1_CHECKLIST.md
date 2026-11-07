# DP-WP4 milestone D1 — Measurement dictionary, quality, state store (host)

Reference: Development Plan v1.1 §10 (D1), Design Specification v1.1 §6. Exit criteria: **UMD-\* and Q-001…Q-008 (host part) pass on the host.**

## Delivered files

| File | Purpose |
|---|---|
| `config/umd/umd_v1.json` | Dictionary source: 14 parameters, domains, units, value types, retired list |
| `tools/profile/umd_gen.py` | Validates the dictionary and generates the C table; `--check` for "is the generated code current?"; refuses to change or drop an existing runtime ID |
| `tools/profile/tests/test_umd_gen.py` | 17 generator tests (incl. UMD-002 stability) |
| `components/ems_measurement/` | ESP-IDF component, pure C: `ems_measurement.h` (quality, units, record, lookup), `ems_state_store.h/.c`, generated `ems_param_ids.h` + `ems_param_table.c` |
| `test/host/` | Host test project (CMake) using ESP-IDF's Unity; 29 tests |
| `docs/D1_CHECKLIST.md` | This file |

No existing file was changed. `main/` does not use the component yet (that is D6), but ESP-IDF compiles every component in `components/`, so the firmware build also checks it compiles for the ESP32-S3.

## Deviation from the plan (recorded)

| Plan v1.1 | D1 as delivered | Reason |
|---|---|---|
| Unity vendored under `test/host/unity/` | Unity taken from `%IDF_PATH%\components\unity\unity` (override with `-DUNITY_DIR`) | ESP-IDF already ships Unity; no second copy to maintain |
| Generator part of `myems_profile_tool.py` (D3) | Separate `tools/profile/umd_gen.py` now | D1 needs the generated table; D3 will call it from the profile tool |

## Design decisions made in D1 (for review)

1. `seq` increments on every publish (even with the same value) and on every quality change.
2. `OUT_OF_RANGE` stores the decoded value for diagnostics but keeps the timestamp of the last GOOD sample.
3. `INVALID`, `STALE` and `COMM_LOST` keep the last value and its timestamp; consumers must check quality (only GOOD is usable).
4. A failed read turns only GOOD into STALE; INITIALIZING, INVALID and OUT_OF_RANGE are left unchanged.
5. Freshness: age exactly equal to the stale timeout is still GOOD; COMM_LOST is never changed by the freshness check.
6. Reading a dictionary parameter that the device does not declare returns NOT_SUPPORTED (seq 0); writing it is refused.
7. Non-finite values are refused by `ems_state_publish`; the decoder must call `ems_state_mark_invalid` instead.

## Steps

| # | Step | Command (ESP-IDF v6.1 PowerShell, project root) | Expected |
|---|---|---|---|
| 1 | Host compiler installed (one time) | see `test/host/README.md`; `gcc --version` | version printed |
| 2 | Generated code is current | `python tools/profile/umd_gen.py --check` | `OK: generated files match UMD 1.0.0 (14 parameters)` |
| 3 | Generator tests | `python -m unittest discover -s tools/profile/tests -v` | `Ran 17 tests ... OK` |
| 4 | Host C tests | `cmake -S test/host -B build_host -G Ninja -DCMAKE_C_COMPILER=gcc` then `cmake --build build_host` then `ctest --test-dir build_host --output-on-failure` | builds without warnings; `100% tests passed` (29 tests) |
| 5 | Firmware still builds with the new component | `.\tools\build_firmware.ps1 -Mode Production` | build succeeds |
| 6 | Commit (you) | `git add config components/ems_measurement tools/profile test/host docs/D1_CHECKLIST.md` then `git commit -m "D1: measurement dictionary, quality model, state store (host-tested)"` | |
| 7 | Tag (you) | `git tag -a dp-wp4-d1 -m "DP-WP4 D1 complete"` | |

## Test coverage of D1

| Test ID | Where | Status after my run |
|---|---|---|
| UMD-001 | host C + Python | PASS |
| UMD-002 | Python (`test_umd002_*`) | PASS |
| UMD-003 | host C (40-byte layout and offsets) | PASS |
| UMD-004 | host C + Python | PASS |
| UMD-005 | host C | PASS |
| UMD-008 (host part) | host C | PASS |
| UMD-009 | host C | PASS |
| Q-001 … Q-008 (host part) | host C | PASS |
| UMD-006/007, Q-009/010 | need acquisition (D6, HIL) | not in D1 |

Verified on Linux with gcc 13 and clang (`-Werror`), gcc AddressSanitizer + UBSan, line coverage 98 % / branch 100 % for `ems_state_store.c`, against a Unity-compatible stand-in. **Your run with the real Unity on Windows is the acceptance run.**

## Exit record

| Item | Result | Note |
|---|---|---|
| Step 2 `--check` | | |
| Step 3 generator tests | | |
| Step 4 host tests (29) | | |
| Step 5 firmware build | | |
| Commit / tag `dp-wp4-d1` | | |
