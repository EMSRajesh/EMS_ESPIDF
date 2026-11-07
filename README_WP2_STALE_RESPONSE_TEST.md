# MyEMS WP2 — Same-slave / same-function stale-response test

## Purpose
Reproduce the Modbus RTU ambiguity where a delayed FC03 response to request A
may be accepted as the response to request B. Both requests use slave 1, FC03,
quantity 2, but different start addresses (100 and 200). FC03 response frames
do not echo the start address.

## Safety
- Use only the USB-RS485 simulator on COM12.
- Disconnect all live meters from the test RS485 bus.
- This self-test only performs FC03 reads in the WP2 scenario.
- The package is a source/test patch; it has NOT been built or flashed on your
  ESP32-S3 in this environment.

## Changes
1. `main/modbus_self_test.c`: adds request A timeout and request B stale-frame
   assertion after the existing self-test checks.
2. `tools/myems_usb_rs485_modbus_slave_sim_v5.py`: adds `--fault stale-fc03`.
   It schedules A's response at 1000 ms after A and B's genuine response at
   1200 ms after B, while continuing to receive requests.

## Expected timing
- A timeout: 350 ms, no retry.
- A delayed response: 1000 ms after A request.
- B starts after A timeout.
- Stale A arrives roughly 650 ms into B's response window.
- Genuine B response arrives 1200 ms after B request.
- B timeout: 1600 ms.

## Run
Open ESP-IDF PowerShell in the project root.

Terminal 1 — simulator:
```powershell
python .\tools\myems_usb_rs485_modbus_slave_sim_v5.py `
  --port COM12 --slaves 1 --baud 9600 --parity E --stopbits 1 `
  --fault stale-fc03 --log-file .\tools\wp2_stale_fc03_sim.log
```

Terminal 2 — build and flash self-test:
```powershell
idf.py -B build_selftest -D SDKCONFIG="$PWD\sdkconfig.selftest" build
idf.py -B build_selftest -D SDKCONFIG="$PWD\sdkconfig.selftest" -p COM11 flash
idf.py -B build_selftest -D SDKCONFIG="$PWD\sdkconfig.selftest" -p COM11 monitor
```

The existing V5.1 monitor automation may also be used, but do not run a second
simulator instance or a second monitor against the same COM ports.

## Interpret the result
- `WP2-B ... registers=[1100,1101]` followed by FAIL means stale response A
  was accepted as B. This is the suspected failure.
- `registers=[1200,1201]` and PASS means the stale frame did not satisfy B in
  this timing run.
- B timeout/transport error is a safe failure for this test, but investigate
  why the genuine response was not accepted.
- Simulator log must show `FAULT scheduled stale response A`, `FAULT scheduled
  genuine response B`, then `TX STALE_A` before `TX GENUINE_B`.

## Important limitations
This is one deterministic timing scenario, not proof of complete RTU recovery.
Repeat with timing offsets after the first run. Do not change production
transaction logic until the current firmware behavior has been observed.
