# MyEMS host unit tests

Pure-C DP-WP4 code is tested on the PC, without the ESP32:

| Test executable | Component | Milestone | Tests |
|---|---|---|---|
| `test_ems_measurement` | `components/ems_measurement` (dictionary, quality, state store) | D1 | 29 |
| `test_ems_register_map` | `components/ems_register_map` (decode, convert, validate, counters) | D2 | 34 |

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

Expected: `100% tests passed, 0 tests failed out of 2`. For per-test output run `.\build_host\test_ems_measurement.exe` or `.\build_host\test_ems_register_map.exe`.

`build_host` is ignored by Git. To start clean, delete the folder.

## Generator tests (Python)

```powershell
python -m unittest discover -s tools/profile/tests -v
```

These include a check that the generated files in `components/ems_measurement` match `config/umd/umd_v1.json`.

## Options

| Option | Effect |
|---|---|
| `-DUNITY_DIR=<path>` | Use another Unity checkout instead of the one in ESP-IDF |
| `-DEMS_HOST_SANITIZE=ON` | AddressSanitizer + UBSan (Linux/macOS GCC or Clang; not supported by MinGW) |
| `-DEMS_HOST_COVERAGE=ON` | gcov instrumentation |

Warnings are errors (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, or `/W4 /WX` for MSVC).
