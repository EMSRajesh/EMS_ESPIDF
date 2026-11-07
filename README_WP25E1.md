# WP2.5-E1 — Master task stack working-set reduction

## Basis
Generated against the user's latest uploaded source archive:
`myems_rs485_modbus_rtu_09_055(4).zip`

## Files
- `components/modbus_rtu_master/src/modbus_rtu_master.c` — complete replacement source
- `WP25E1_SOURCE.diff` — unified diff against the uploaded source
- `PATCH_MANIFEST.txt` — source hash and scope

## Changes
1. Moves the master transaction context out of `master_task()` automatic storage into a singleton static workspace.
2. Moves completion-result and decode-result workspaces out of `publish()` and `decode()` automatic storage.
3. Removes the 256-byte RX temporary buffer; decode now consumes the transaction RX buffer directly.
4. Moves the queued-request scratch object and RS485 event scratch object out of the master task stack.
5. Removes temporary `transaction_t` objects from queued-request cancellation and stop-drain paths by publishing cancellation directly from the queued request.
6. Adds a second stack watermark sample after event/decode/recovery processing each loop.

## Ownership and invariants
- The existing design is a single-master singleton. All new workspaces are file-static and must remain exclusively owned by the master task.
- Completion queue publication still copies the result into the FreeRTOS queue before the workspace is reused.
- The decode workspace and publish workspace are distinct to avoid aliasing while a result is copied.
- Request IDs, cancellation status/counters, completion-slot reservation, retries, recovery, and atomic statistics are retained.

## Apply
Back up the current file, then replace:
`components/modbus_rtu_master/src/modbus_rtu_master.c`

Do not apply this to a different source revision without reviewing the diff.

## Validation required
This is a source patch, not a build- or hardware-validated release.
1. Build using the existing WP25-D test configuration.
2. Inspect generated `sdkconfig.h` and confirm WP25-D test symbols.
3. Flash and run WP25-D cancellation test.
4. Run WP24 A-I regression.
5. Compare `modbus_master` `min_ever_free` against the previous 740-byte result. Capture both after WP25-D cancellation and after WP24 A-I traffic.
6. Repeat stack tests with compiler optimization and production configuration identical to intended release.

A larger stack margin is a target, not an assumed outcome. Do not declare stack acceptance solely because this patch builds or because one test passes.
