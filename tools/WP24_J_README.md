# WP24-J: retry exhaustion

## Purpose
Validate a request with `max_retries=1` when the simulator drops both the
initial FC03 response and the one permitted retry. The expected Modbus request
result is TIMEOUT; the test itself passes if the retry limit is enforced.

## Safety
Use only the USB-RS485 simulator on COM12. Never connect the ESP32 RS485 port
to a live meter during fault injection.

## Build and flash
From the project root in ESP-IDF v6.1 PowerShell:

```powershell
.\tools\build_wp24_j.ps1
```

This uses `sdkconfig.wp24_j`, `build_wp24_j`, and flashes COM11.

## Run
```powershell
python .\tools\myems_wp24_j_retry_exhaustion_test.py `
  --esp-port COM11 `
  --rs485-port COM12 `
  --baud 9600 `
  --parity E `
  --stopbits 1
```

The simulator is configured to drop every FC03 request at start address 100.
WP24-J checks:
- final status is TIMEOUT
- retry_count is exactly 1 (one retry only)
- at least two recovery completions occurred
- simulator logs exactly two matching dropped response attempts
- self-test/monitor completed without crash

The runner builds/flashes only when explicitly passed `--build` / `--flash`;
otherwise the build must already be configured as WP24-J.
