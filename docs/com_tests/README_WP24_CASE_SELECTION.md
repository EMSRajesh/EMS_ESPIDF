
# WP24 isolated firmware case-selection patch (A-F)

## Purpose
This patch adds a firmware-side WP24 case selector for A-F. A selected case executes only its own faulted request plus the existing address-200 isolation/control request. It does not continue into unrelated WP24 cases. The default selection `0` retains the full WP24 A-I campaign.

## Files to apply
Copy these files into the project root, preserving paths and overwriting the matching files:
- `main/modbus_self_test.c`
- `main/Kconfig.projbuild`
- `sdkconfig.wp24_all` (adds explicit case selection `0` for the full campaign)
- `sdkconfig.wp24_case` (isolated case config; default A)
- `tools/myems_wp24_recovery_test.py` (recognizes new `WP24_X_PASS` firmware markers and validates the selected build config)
- `tools/build_wp24_case.ps1`

## Build and flash one isolated case
From the ESP-IDF v6.1 PowerShell environment, project root:

```powershell
.\tools\build_wp24_case.ps1 -Case A
```

Allowed cases: A, B, C, D, E, F. The script modifies only the dedicated `sdkconfig.wp24_case` selector, reconfigures/builds `build_wp24_case`, checks generated `sdkconfig.h`, then flashes COM11.

## Run only the selected case
For A:

```powershell
python .\tools\myems_wp24_recovery_test.py `
  --only WP24_A_drop_timeout `
  --console-port COM11 `
  --rs485-port COM12 `
  --baud 9600 `
  --build-dir build_wp24_case
```

For B-F, replace the case label with the exact label printed by `--list` and build/flash the same letter using `-Case B`, etc. The runner verifies that the compiled `CONFIG_MYEMS_WP24_CASE_SELECTION` matches the requested case and refuses to run a mismatched image.

## Safety
- Connect the ESP32 RS485 only to the USB-RS485 simulator, never to a live meter.
- The baseline self-test performs write operations to simulator registers/coils 1..8.
- Do not run the isolated-case runner while the A-I full-campaign firmware is flashed.

## Scope / validation status
This is a source patch generated against project archive `myems_rs485_modbus_rtu_09_055(7).zip`. It has not been compiled or hardware-tested in this environment. The user must build, flash, and run the isolated case before marking the patch validated.

The isolated selection covers WP24-A through WP24-F. WP24-G/H/I remain available through the existing full A-I campaign; this patch does not add isolated selection for G/H/I.


### Corrected packaging notes
- PowerShell script starts with `param(...)` as required by PowerShell (no leading backslash).
- Kconfig `depends on` uses boolean symbols only; the `MYEMS_WP24_EXTENDED_TEST == 4` comparison is intentionally removed because Kconfig dependency expressions do not accept that comparison syntax. The C preprocessor still gates case-selection code on `CONFIG_MYEMS_WP24_EXTENDED_TEST == 4`.
