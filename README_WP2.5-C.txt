MyEMS WP2.5-C — Completion Queue Backpressure Patch

Baseline: myems_rs485_modbus_rtu_09_055(2).zip
Scope: completion capacity reservation at request admission.

Files (relative to project root):
  components/modbus_rtu_master/src/modbus_rtu_master.c
  components/modbus_rtu_master/include/modbus_rtu_master.h
  docs/concurrency_contract.md
  docs_architecture.md

Behavior:
- A counting semaphore is initialized with completion_queue_length tokens.
- modbus_master_submit() reserves one token before enqueueing.
- If no token is available, submit returns ESP_ERR_TIMEOUT and increments
  completion_backpressure_rejections. The request is not accepted.
- If request enqueue fails, the reserved token is returned.
- A token remains reserved while the request is queued/active/retrying and
  while its completion is waiting in the completion queue.
- modbus_master_get_completion() returns the token after receiving the result.
- completion_drops remains a diagnostic invariant-violation counter.

Important:
- This patch is WP2.5-C only. It does not implement WP2.5-B lifecycle safety,
  WP2.5-D queued-request cancellation, or shutdown draining.
- Do not run stop/close concurrently with active API calls; WP2.5-B remains open.
- No build or hardware test has been performed by this patch generation step.

Apply:
1. Back up the current project.
2. Extract this ZIP into the project root, preserving paths and overwriting the
   four listed files.
3. Rebuild using ESP-IDF v6.1 and your existing WP24 build configuration.

Recommended checks:
- Build production and WP24 configurations.
- Configure completion_queue_length=2; submit two requests without consuming
  completions; third submission must return ESP_ERR_TIMEOUT.
- Consume one completion; a new submit should then be accepted (subject to
  request queue capacity and stopping state).
- Force request queue full after reserving a token; confirm failed submission
  returns the token (repeat until the token budget is demonstrably unchanged).
- Run WP24 A-I regression.
- Verify completion_drops remains zero and backpressure rejection counter
  increases under saturation.
