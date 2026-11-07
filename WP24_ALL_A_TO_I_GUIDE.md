# MyEMS WP24 A-I Unified Test Firmware

## Purpose

This is a dedicated test image. It is separate from the production configuration and must only be connected to the USB-RS485 simulator. It executes the baseline Modbus function-code and validation checks, then WP24-A through WP24-I sequentially.

## Safety

- ESP32-S3 console: COM11 at 115200.
- USB-RS485 simulator: COM12 at 9600, 8E1.
- Do not connect a live meter or operational RS485 network. The baseline self-test writes coils/registers 1..8.
- WP24-H is a software-injected `RS485_EVT_RX_LOSS`; it does not prove physical UART ring-buffer overflow.
- WP24-I forces recovery activity in software; it does not prove immunity to real electrical noise.
- WP24-I intentionally leaves the transport in `RS485_RECOVERY_FAILED`; it runs last.

## Build and flash once

From the project root in PowerShell:

```powershell
. .\tools\build_wp24_all.ps1
```

This uses `sdkconfig.wp24_all` and `build_wp24_all`. It does not use or overwrite `sdkconfig.production`.

## Run all tests with one command

After the build/flash completes and the simulator is connected:

```powershell
python .\tools\myems_wp24_all_tests.py `
  --esp-port COM11 `
  --rs485-port COM12 `
  --baud 9600 `
  --parity E `
  --stopbits 1
```

The runner starts one simulator process with a deterministic fault schedule, starts ESP-IDF monitor (which resets the ESP32), captures the full run, and writes TXT/JSON/CSV reports under `tools/test_results/`.

## Fault schedule

The simulator applies faults only to FC03 requests at start address 100. The ten scheduled matching attempts are:

1. A: drop
2. B: delay 1000 ms
3. C: bad CRC
4. D: short response
5. E: wrong slave
6. F: valid exception
7. G first attempt: drop
8. G retry: no fault
9. H: drop (allows software RX-loss injection while waiting)
10. I: drop (allows forced recovery-timeout path)

The control requests at address 200 are unmodified and must return `[1200,1201]`.

## Acceptance

The campaign passes only if:
- The firmware emits individual `WP24_A_PASS` through `WP24_I_PASS` markers.
- A-E complete with their expected non-OK status and the subsequent B request returns `[1200,1201]` after recovery.
- F returns a valid Modbus exception without unnecessary recovery.
- G succeeds on retry and monitor logs show recovery release before retry scheduling.
- H classifies the injected event as `MODBUS_RESULT_TRANSPORT_ERROR` and recovery completes.
- I records a recovery timeout, remains in `RS485_RECOVERY_FAILED`, and rejects TX with `ESP_ERR_INVALID_STATE`.
- The simulator confirms the complete ten-entry fault schedule and firmware emits `WP24_A_TO_I_SUMMARY: PASS` plus `MODE=SELF_TEST result=PASS`.

## Validation status

This package is source-modified and statically checked. ESP-IDF compilation and hardware execution must still be performed in the user's ESP-IDF v6.1 Windows environment. Do not treat source checks as a firmware build or hardware PASS.
