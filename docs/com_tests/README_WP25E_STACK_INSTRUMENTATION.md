# WP2.5-E — Task stack instrumentation

## Scope
Adds a shared, header-only diagnostics component and samples five task stacks: `rs485_transport`, `modbus_master`, `ems_scheduler`, `ems_acquisition`, and `myems_self_test`. No Modbus request, retry, recovery, queue, or cancellation behavior is changed.

## Measurements
ESP-IDF FreeRTOS `uxTaskGetStackHighWaterMark(NULL)` is treated as bytes. Each task reports configured/created stack allocation, current free headroom, and minimum-ever free headroom observed since task start. Samples occur in the task loop; periodic informational reports are limited to once per 10 seconds.

## Thresholds
- Warning: free stack <= max(15% of allocated stack, 512 bytes).
- Critical: free stack <= max(5% of allocated stack, 256 bytes).
- A warning/critical transition is logged once per task lifetime.

These are bring-up thresholds, not final product safety limits. Establish final margins from worst-case stress measurements and compiler/build configuration. The minimum-ever value is only as complete as the executed workload and sampling frequency.

## Expected log format
`STACK task=... allocated=...B current_free=...B min_ever_free=...B`
`STACK LOW ...` or `STACK CRITICAL ...` when thresholds are crossed.

## Build
Use the same `sdkconfig` as the intended build mode. Reconfigure and build normally. This patch adds the `ems_stack_diag` component dependency.

## Validation required
1. Build with ESP-IDF v6.1.
2. Flash and boot.
3. Run production polling with a responsive simulator for at least 10 minutes.
4. Exercise timeout/recovery and the WP24 A-I regression.
5. Capture all `STACK` logs and check that no critical threshold is crossed.
6. Record allocated stack and minimum-ever free bytes for each task.

Build and target validation are not included in this source patch.
