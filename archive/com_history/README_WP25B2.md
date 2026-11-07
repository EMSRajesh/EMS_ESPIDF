# WP2.5-B2 — Active transaction shutdown coordination

## Baseline
Generated from the user-uploaded project archive:
`myems_rs485_modbus_rtu_09_055(5).zip`

This patch is based on the current WP2.5-B1 source and preserves the existing
WP2.5-A atomic statistics, WP2.5-C completion reservation/backpressure,
WP2.5-D cancellation, and WP2.5-E1 static working-set optimizations.

## Files changed
- `components/modbus_rtu_master/src/modbus_rtu_master.c`

## Changes
1. Reworks the master task loop so shutdown is checked before normal work,
   after bounded event waits, before event decoding, and before retry scheduling.
2. If a request was dequeued just as shutdown was requested, it remains the
   active request and is completed as CANCELLED by the task's shutdown drain;
   it is not intentionally transmitted after the task observes shutdown.
3. Stops processing RX events after shutdown is observed, preventing a late
   response from being decoded as a successful completion after shutdown wins.
4. Keeps the master API admission mutex held through the bounded stop wait and
   phase-1 resource transition. This serializes concurrent stop callers so
   they cannot consume the same stopped semaphore or delete the same resources.
5. Preserves Option A: request queue/task control resources are released after
   task exit, while completion queue and completion reservation semaphore stay
   alive for application draining in DRAINING.

## Important boundary / not yet covered
- This patch does not add B3 deinitialization or restart.
- It does not yet prove physical bus quarantine / late-response safety across
  a future transport restart. B3/B4 must define and enforce bus recovery before
  any restarted master can transmit.
- It does not add the dedicated WP2.5-B2 hardware test harness. B2 test cases
  must be added/validated before marking B2 complete.
- This patch has not been compiled or run on hardware by the assistant.

## Apply
Replace only:
`components/modbus_rtu_master/src/modbus_rtu_master.c`

Do not replace the header or other files.

## Validation sequence
1. Build WP2.5-D:
   `idf.py -B build_wp25_d -D SDKCONFIG=sdkconfig.wp25_d build`
2. Flash and run the dedicated WP2.5-D cancellation test.
3. Build and flash WP24:
   `idf.py -B build_wp24_all -D SDKCONFIG=sdkconfig.wp24_all build`
   `idf.py -B build_wp24_all -D SDKCONFIG=sdkconfig.wp24_all -p COM11 flash`
4. Run `tools/myems_wp24_all_tests.py` with the USB RS485 simulator on COM12.
5. Review stop-during-active, stop-during-recovery, and concurrent-stop behavior
   with a dedicated B2 test before accepting this work package.

Safety: WP24 self-tests write test registers/coils 1..8. Connect only the USB
RS485 simulator, never a live meter.
