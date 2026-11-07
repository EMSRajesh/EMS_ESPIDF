# WP24 A-I Unified Implementation Notes

## Changes in this package

- Added `sdkconfig.wp24_all` with self-test and test-only fault injection enabled and selection value `4` for the complete campaign.
- Extended `MYEMS_WP24_EXTENDED_TEST` Kconfig range to include `4`.
- Added a firmware campaign path that executes A-F stale-response/recovery pairs, then G retry, H software RX-loss injection, and I recovery-timeout/fail-closed last.
- A-F now validate exact result status, request completion identity, B response values `[1200,1201]`, and recovery-success counter behavior (no recovery for valid exception F).
- G validates retry count and returned values. The host runner additionally verifies a recovery-release log precedes retry scheduling.
- H waits on a deliberately dropped response before injecting the software RX-loss event, avoiding the previous race where a normal response could complete before injection.
- I remains last and checks recovery timeout count, `RS485_RECOVERY_FAILED`, and rejected TX.
- Added simulator `--fault-sequence` support and a deterministic ten-entry fault schedule.
- Added `tools/myems_wp24_all_tests.py`, build/flash helper, and user guide.
- Production `sdkconfig.production` and the WP2.3 recovery implementation are not intentionally changed.

## Static validation performed in this environment

- Python syntax compilation passed for the unified runner and simulator.
- Simulator's built-in FC01/02/03/04/05/06/15/16/23 response-generation and CRC self-test passed.
- The unified sdkconfig contains `CONFIG_MYEMS_MODBUS_SELF_TEST=y`, `CONFIG_MYEMS_WP24_FAULT_INJECTION=y`, and `CONFIG_MYEMS_WP24_EXTENDED_TEST=4`.

## Not yet validated

- ESP-IDF v6.1 C compilation was not run in this environment.
- No ESP32-S3 firmware was flashed or executed here.
- No physical COM11/COM12 campaign was run.

Therefore this package is an implementation candidate pending ESP-IDF build review and hardware execution; it is not a WP24 A-I PASS result. Review any build errors before flashing. Run only with the USB-RS485 simulator, never a live meter.
