# MyEMS WP24-M recovery-guard boundary test patch

## Contents
- `firmware_wp24_m.patch`: patch for `main/Kconfig.projbuild` and `main/modbus_self_test.c`.
- `firmware_patch/`: patched source snapshot for review, including `sdkconfig.wp24_m1` and `sdkconfig.wp24_m2`.
- `myems_wp24_m_boundary_test.py`: Windows/ESP-IDF runner.
- `simulator_source.py`: copy of the simulator source supplied in this conversation.

## Safety
**Connect the ESP32 RS485 interface only to the USB RS485 simulator. Never connect a live meter or live control bus during fault injection.**

## Apply
1. Back up your working project.
2. Copy the two changed firmware files or apply `firmware_wp24_m.patch`.
3. Copy `sdkconfig.wp24_m1` and `sdkconfig.wp24_m2` to the project root.
4. Copy `myems_wp24_m_boundary_test.py` into `<project>/tools/`.
5. Ensure your simulator is at:
   `tools/myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py`
6. Run M1 and M2 separately from an ESP-IDF v6.1 PowerShell environment.

## Commands
```powershell
# M1: intended to land during the recovery guard
python tools/myems_wp24_m_boundary_test.py --mode M1 --build --flash

# M2: intended to land just after recovery guard release
python tools/myems_wp24_m_boundary_test.py --mode M2 --build --flash
```

Default simulator delays (1250 ms for M1, 1450 ms for M2) are **starting estimates**, not guaranteed guard-relative values. The simulator delay is measured from its receipt of the first targeted FC03 request. The runner captures ESP recovery-start/release timestamps and simulator late-response timestamp, then classifies the actual event. If a response is not in the intended timing window, adjust `--delay-ms` and rerun, e.g.:

```powershell
python tools/myems_wp24_m_boundary_test.py --mode M1 --delay-ms 1275
python tools/myems_wp24_m_boundary_test.py --mode M2 --delay-ms 1475
```

Do not report M1/M2 as validated until each runner report says PASS and the artifacts have been reviewed. No ESP-IDF build or hardware run was performed while preparing this patch.

## Interpretation
- M1 requires the delayed response to be captured between recovery-start and recovery-release; the first request should time out after its retry is dropped.
- M2 requires the delayed response to be captured after recovery-release. A stale FC03 response may be accepted during retry WAIT_RESPONSE because FC03 RTU responses do not carry a transaction ID or start address. Firmware accepts either timeout or stale-response acceptance for the first request, but requires the independent address-200 request to return `[1200,1201]`.
- This is a timing characterization of the configured guard. It is not proof against arbitrarily late frames or a substitute for protocol-level isolation.

## WP24-L regression
Keep the existing WP24-L test unchanged and rerun it after M1/M2. The patch does not modify the WP24-L test behavior.
