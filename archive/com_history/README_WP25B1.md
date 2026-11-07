# WP2.5-B1 — Lifecycle state and synchronized request admission

## Baseline

This patch is based on the latest supplied project archive `myems_rs485_modbus_rtu_09_055(4).zip`, with the WP2.5-E1 master implementation applied as the source baseline. It retains the WP2.5-A atomic statistics, WP2.5-C completion reservation/backpressure, WP2.5-D cancellation behavior, and WP2.5-E1 static master workspaces/stack instrumentation.

## Scope implemented

1. Adds the public lifecycle state enum:
   - STOPPED
   - STARTING
   - RUNNING
   - QUIESCING
   - DRAINING
   - STOPPING (reserved for later B2/B3 sequencing)
   - FAILED
2. Adds a statically allocated lifecycle/admission mutex. It is retained for process lifetime and is not deleted during stop.
3. Serializes `modbus_master_submit()` state validation, completion-slot reservation, request enqueue, and accepted-counter update against the shutdown transition.
4. `modbus_master_stop()` closes admission by transitioning RUNNING -> QUIESCING while holding the same mutex.
5. Converts cross-task `stopping` reads/writes to atomic loads/stores.
6. Serializes `modbus_master_cancel_pending()` against shutdown by retaining the admission mutex through its bounded acknowledgement wait.
7. Adds `modbus_master_get_lifecycle_state()`.
8. Implements Option A phase-1 behavior: after the master task exits, stop leaves the completion queue and completion-slot semaphore alive and sets lifecycle state to DRAINING. `modbus_master_get_completion()` can continue draining results.

## Deliberately deferred

- B2: fully formalized active transaction termination and stop/cancel interaction beyond the existing stop-drain path.
- B3: explicit deinit API, consumer-drain verification, and resource destruction after drain.
- B4: restart-after-deinit, repeated/concurrent lifecycle calls, and comprehensive failure cleanup.
- WP2.5-F lifecycle stress tests and WP2.5-G integrated acceptance.

Until B3/B4 is delivered, the single master remains allocated in DRAINING after `modbus_master_stop()`. Do not expect `modbus_master_start()` to succeed again in the same boot after stop. This is intentional for this staged patch because the completion queue is preserved.

## Important usage constraint for this intermediate patch

The API admission lock protects submit/cancel admission against the RUNNING -> QUIESCING transition. Completion consumers are safe to continue draining after stop because the completion queue is retained. Full concurrent deinit/consumer lifetime protection is not implemented until B3/B4; do not manually delete or reuse the master resources.

## Files

- `components/modbus_rtu_master/src/modbus_rtu_master.c`
- `components/modbus_rtu_master/include/modbus_rtu_master.h`

## Validation procedure

Use separate build directories/configurations as appropriate. First build the existing WP2.5-D test configuration, then run its cancellation self-test. Verify the lifecycle state is RUNNING before stop and DRAINING after stop when a dedicated lifecycle test is added. Re-run the WP24 A-I campaign with `sdkconfig.wp24_all` to verify the existing E1 regression baseline.

This patch has not been built or run on hardware by the patch author. Treat it as a source change requiring ESP-IDF v6.1 build and board validation.
