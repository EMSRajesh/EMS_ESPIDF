# MyEMS host unit tests

Pure-C DP-WP4 code is tested on the PC, without the ESP32:

| Test executable | Component | Milestone | Tests |
|---|---|---|---|
| `test_ems_measurement` | `components/ems_measurement` (dictionary, quality, state store) | D1 | 29 |
| `test_ems_register_map` | `components/ems_register_map` (decode, convert, validate, counters) | D2 | 34 |
| `test_ems_config_lab` | `components/ems_profile` + `main/generated` (site `lab_bench`): CRC, firmware validation, Test Spec 4.1/4.3 vectors | D3 | 9 |
| `test_ems_config_decoder` | `components/ems_profile` + `test/host/generated/decoder_test`: CRC, decoder vectors D1–D11 | D3 | 6 |
| `test_ems_sched_core` | `main/ems_sched_core.c` (poll entries, request IDs, one in-flight per device, probe mode, guard rule, latency, bus hold) | D5, D7, D8 | 27 |
| `test_ems_acq_core` | `main/ems_acq_core.c` + `main/generated` (routing, result → quality, freshness, ratios and counters through the full path, device ONLINE/OFFLINE, bus-fault OFFLINE) | D6, D7, D8 | 33 |
| `test_ems_d7` | `components/ems_profile` activation checks on corrupted copies of `main/generated`; `components/ems_diag` event ring | D7 | 20 |
| `test_ems_d8` | `main/ems_bus_health.c` (bus health derived from the transport and master counters, latched fault, master health report) | D8 | 14 |
| `test_ems_config_site_2dev`, `test_ems_config_site_ct200` | `main/generated_sites/<site>` (firmware tables of build modes Wp4Sim2Dev, Wp4Ratio): CRC, firmware validation, activation, plan | D9 | 8 each |
| `test_ems_config_site_dc`, `test_ems_config_site_mixed` | `main/generated_sites/lab_dc_bench`, `lab_mixed` (build modes Wp5Dc, Wp5Mixed): the same table checks, the DC map layout, and the DC meter through `main/ems_acq_core.c` (Test Spec 2.1 and edge vectors, FC04 routing, counters, timeout, freshness; mixed site: AC and DC independent) | DP-WP5 M2, M3 | 16 / 17 |

## One-time setup (Windows)

ESP-IDF already provides CMake, Ninja and the Unity test framework (`%IDF_PATH%\components\unity\unity`). Only a **host C compiler** is missing:

1. Install MSYS2: `winget install MSYS2.MSYS2`
2. In the **MSYS2 UCRT64** shell: `pacman -S --needed mingw-w64-ucrt-x86_64-gcc`
3. Add `C:\msys64\ucrt64\bin` to the Windows `PATH` (System Properties → Environment Variables), then open a new terminal.
4. Check: `gcc --version`

(MSVC also works: build from a "Developer PowerShell for VS" and drop `-DCMAKE_C_COMPILER=gcc`.)

## Run (ESP-IDF v6.1 PowerShell, project root)

```powershell
cmake -S test/host -B build_host -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build_host
ctest --test-dir build_host --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 10`. For per-test output run any of the `.\build_host\test_*.exe` files.

`build_host` is ignored by Git. To start clean, delete the folder.

## Generator tests (Python)

```powershell
python -m unittest discover -s tools/profile/tests -v
```

These include checks that the generated files are up to date: `components/ems_measurement` against `config/umd/umd_v1.json`, and `main/generated` plus `test/host/generated/decoder_test` against `config/`. If you change anything in `config/`, regenerate:

```powershell
python tools/profile/myems_profile_tool.py generate --site config/site/lab_bench.site.json --target sim
python tools/profile/myems_profile_tool.py generate --site config/site/lab_decoder_test.site.json --target sim --out-dir test/host/generated/decoder_test
python tools/profile/myems_profile_tool.py generate --site config/site/lab_bench_2dev.site.json --target sim --out-dir main/generated_sites/lab_bench_2dev
python tools/profile/myems_profile_tool.py generate --site config/site/lab_ratio_ct200.site.json --target sim --out-dir main/generated_sites/lab_ratio_ct200
```

## Options

| Option | Effect |
|---|---|
| `-DUNITY_DIR=<path>` | Use another Unity checkout instead of the one in ESP-IDF |
| `-DEMS_HOST_SANITIZE=ON` | AddressSanitizer + UBSan (Linux/macOS GCC or Clang; not supported by MinGW) |
| `-DEMS_HOST_COVERAGE=ON` | gcov instrumentation |

Warnings are errors (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, or `/W4 /WX` for MSVC).
