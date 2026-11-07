# MyEMS RS485 / Modbus RTU project review

## Scope reviewed
- `components/rs485_transport`
- `components/modbus_rtu_master`
- `main/ems_poll_scheduler.c`
- `main/ems_acquisition.c`
- `main/app_main.c`
- USB-RS485 simulator and automated test scripts

## Corrections applied

1. **Removed duplicate static transport stack declaration.** The duplicate definition in `rs485_transport.c` was a C compile blocker.
2. **Honored the configured RS485 task stack size.** `task_stack_size` is interpreted as bytes and converted to FreeRTOS `StackType_t` words for `xTaskCreateStatic`.
3. **Made retry semantics explicit.**
   - `max_retries == 0`: no retry.
   - `max_retries == MODBUS_RETRIES_USE_DEFAULT`: use the master's configured default.
   - Values 1 through `MODBUS_MAX_RETRIES` are explicit retry counts.
   - Values above the supported limit are rejected.
4. **Protected write operations from automatic retries by default.** FC05, FC06, FC15, FC16 and FC23 are not retried unless `allow_write_retries` is explicitly set. This avoids silently repeating operations whose first execution may have succeeded even if the response was lost. Only opt in after confirming the device operation is idempotent.
5. **Propagated UART receive-loss and receive-error events into the active Modbus transaction.** They now terminate the current attempt as a transport error and use the configured retry policy rather than silently clearing the partial response and waiting for timeout.
6. **Added unexpected-slave response diagnostics** to the Modbus master statistics and log.
7. **Added RX event length validation** before consuming the copied event payload.
8. **Improved partial RTOS-object cleanup** when Modbus master startup fails.
9. **Improved scheduler pending-deadline arithmetic** to use 64-bit milliseconds and include the configured retry quiet-time budget.
10. **Fixed the fault-injection parser.** It now records `ems_acquisition ... failed status=...` and master retry records, and exports `failed_requests.csv` and `retry_events.csv`. This addresses the previous false impression that a drop scenario had no errors.

## Important design limitations retained

- Modbus RTU has no transaction identifier in the response. A sufficiently late response from an earlier attempt can still be indistinguishable from the response to a retry if it arrives during the later attempt. The quiet interval reduces risk but cannot mathematically eliminate it. Set response timeouts and retry policy using the slowest supported meter's documented response latency. Consider disabling retries for devices with highly variable response times.
- `timestamp_us` is the time the transport task handles an event, not a hardware timestamp of the first received bit.
- Application event queue overflow remains non-blocking by design. It is counted in `app_event_drops`; expose this statistic in the device diagnostic telemetry and treat nonzero growth as a transport health fault.
- The code uses ESP-IDF's UART driver, which allocates its own RX/TX ring buffers and UART event queue. MyEMS-owned task stacks, queues, and semaphores are statically allocated; this does not mean every ESP-IDF driver allocation is static.
- Write retries should remain disabled unless the device's command semantics have been reviewed. FC23 is considered a write operation for this policy.

## Validation performed in this package

- Python syntax compilation of the test scripts.
- Fault parser fixture test against representative retry and failed-request log lines.
- Static source checks for the duplicate stack declaration and API consistency.
- Archive excludes stale generated `build/` output so ESP-IDF regenerates it from the corrected sources.

## Validation not performed

A firmware build and hardware-in-the-loop test were not run in this environment because the ESP-IDF v6.1 Windows toolchain and the Waveshare ESP32-S3 board/USB-RS485 setup are not available here. Therefore this package is a source-corrected candidate, not a claim of a successful target build or validated physical timing.

## Build and test on Windows

From the project root in an ESP-IDF v6.1 PowerShell environment:

```powershell
idf.py fullclean
idf.py set-target esp32s3
idf.py build
idf.py -p COM11 flash monitor
```

Use the separate USB-RS485 adapter on COM12 for the simulator. Close any serial monitor that owns COM11 before running a capture script.

Baseline smoke test:

```powershell
python -m pip install -r .\tools\requirements-usb-rs485.txt
python .\tools\myems_automated_test.py --project . --console-port COM11 --rs485-port COM12 --duration 60
```

Fault-injection test (begin with a single scenario):

```powershell
python .\tools\myems_fault_injection_test.py --project . --console-port COM11 --rs485-port COM12 --duration-per-test 30 --scenarios drop
```

For the current status enum, `status=1` is `MODBUS_RESULT_TIMEOUT`. Confirm that the failed request is logged, that retries match the configured count, and that normal polling resumes after the simulator is returned to `--fault none`.
