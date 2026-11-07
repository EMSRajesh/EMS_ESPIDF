# MyEMS WP2.4 Extended Recovery Tests (G/H/I)

## Safety and scope

- Use only the ESP32-S3 and the USB-RS485 simulator on COM12. Do not connect a live meter.
- ESP32 console: COM11 @ 115200. RS485 simulator: COM12 @ 9600 8E1.
- These are dedicated self-test builds. `MYEMS_WP24_FAULT_INJECTION` must remain disabled in production.
- WP24-H injects a software `RS485_EVT_RX_LOSS` event. It verifies master handling of that event, **not** a physical UART ring-buffer overflow.
- WP24-I forces the transport's recovery activity timestamp to remain active. It tests the recovery timeout/fail-closed branch, **not** real electrical bus noise.
- Existing WP2.3 recovery algorithm is otherwise unchanged.

## Build each dedicated image

Run from the project root in an ESP-IDF v6.1 PowerShell environment:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"

idf.py -B build_wp24_g -D SDKCONFIG=sdkconfig.wp24_g reconfigure
idf.py -B build_wp24_g build
idf.py -B build_wp24_g -p COM11 flash

idf.py -B build_wp24_h -D SDKCONFIG=sdkconfig.wp24_h reconfigure
idf.py -B build_wp24_h build
idf.py -B build_wp24_h -p COM11 flash

idf.py -B build_wp24_i -D SDKCONFIG=sdkconfig.wp24_i reconfigure
idf.py -B build_wp24_i build
idf.py -B build_wp24_i -p COM11 flash
```

Each build uses its own sdkconfig and build directory. Flash one image at a time, then run its corresponding test below.

## Simulator command

For each test, start the simulator in a separate PowerShell window before resetting/monitoring the ESP32:

```powershell
python .\tools\myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py `
  --port COM12 --slaves 1 --baud 9600 --parity E --stopbits 1 `
  --fault drop --fault-fc 3 --fault-start 100 --fault-once --reset-on-fc01 `
  --log-file .\tools\wp24_GHI_simulator.log
```

The first matching FC03 request at address 100 is dropped; the subsequent retry (G) or recovery-test request path can receive a normal simulator response. For H/I the DUT test uses the injected RX-loss or forced recovery-activity hook in addition to the dropped response.

## Run each test

In another PowerShell window, after starting the simulator:

```powershell
idf.py -B build_wp24_g -p COM11 monitor
```

Expected G evidence:

```text
request ... failed status=1; entering RS485 recovery (retry=1/1)
RS485 recovery requested
RS485 recovery released
recovery complete; retry scheduled
WP24-G PASS: retry_count=1; successful retry returned [1100,1101]
WP24_EXTENDED_SUMMARY: PASS mode=RETRY
```

For H, use `idf.py -B build_wp24_h -p COM11 monitor`. Expected evidence:

```text
WP24-H RX-loss test: injecting a software RS485_EVT_RX_LOSS
receive-side transport fault
RS485 recovery requested
RS485 recovery released
WP24-H PASS: injected RX-loss classified as transport error; recovery completed
WP24_EXTENDED_SUMMARY: PASS mode=RX_LOSS_SOFTWARE_INJECTION
```

For I, use `idf.py -B build_wp24_i -p COM11 monitor`. Expected evidence:

```text
RS485 recovery timed out
RS485 bus unavailable; recovery failed
WP24-I PASS: ... state=FAILED; TX rejected while bus unavailable
WP24_EXTENDED_SUMMARY: PASS mode=RECOVERY_TIMEOUT_FAIL_CLOSED
```

WP24-I must **not** show `RS485 recovery released`.

## Acceptance rules

- G passes only if A completes successfully with `retry_count=1`, and logs prove recovery was released before the retry.
- H passes only if the injected RX-loss is classified as `MODBUS_RESULT_TRANSPORT_ERROR` and recovery completes. This does not qualify real UART overrun/ring-full behaviour.
- I passes only if the recovery timeout counter increments, recovery state remains `RS485_RECOVERY_FAILED`, the master reports transport error, and an attempted transport TX is rejected with `ESP_ERR_INVALID_STATE`.
- After G/H/I, rebuild and rerun the unchanged A-F baseline using `sdkconfig.selftest` to check regression.

## Current validation status

Source changes and test instructions are prepared. This package has not been built with ESP-IDF or run on hardware in the current environment. Do not mark G/H/I passed until the serial logs above have been captured and reviewed.
