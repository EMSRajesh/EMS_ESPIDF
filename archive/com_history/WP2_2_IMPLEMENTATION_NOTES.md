# MyEMS WP2.2 — RS485 Transport Recovery

## Scope

This change adds transport-owned asynchronous recovery primitives to the 09_05 RS485 transport component. It does **not** integrate recovery into the Modbus master; that is WP2.3.

## Files changed

- `components/rs485_transport/include/rs485_transport.h`
- `components/rs485_transport/src/rs485_transport.c`

## API

```c
esp_err_t rs485_recovery_begin(rs485_handle_t handle);
esp_err_t rs485_recovery_get_state(rs485_handle_t handle,
                                   rs485_recovery_state_t *state);
esp_err_t rs485_recovery_release(rs485_handle_t handle);
```

States:

- `RS485_RECOVERY_IDLE`: normal TX/RX operation.
- `RS485_RECOVERY_REQUESTED`: request accepted; transport task has not yet performed cleanup.
- `RS485_RECOVERY_ACTIVE`: transport task owns quarantine and discards RX.
- `RS485_RECOVERY_COMPLETE`: configured RX silence observed; state remains latched and RX is still discarded.
- `RS485_RECOVERY_FAILED`: UART cleanup failed; TX remains blocked. The transport continues draining/discarding RX to avoid driver-queue buildup. Requires higher-level fault handling/restart.

`rs485_recovery_begin()` and `rs485_recovery_release()` are asynchronous requests. The caller must poll `rs485_recovery_get_state()`. In particular, after calling release, do not transmit until the state returns to `IDLE`.

## Recovery behaviour

1. The transport task notices `REQUESTED`.
2. It discards pending TX requests (publishing TX errors), flushes UART RX, resets the UART driver event queue, and removes queued RX application events while retaining queued TX completion/error events.
3. During `ACTIVE` and `COMPLETE`, UART RX bytes and RX events are discarded; TX submissions are rejected.
4. RX activity restarts the bus-silence timer. The default `recovery_bus_idle_ms` is 5 ms.
5. Once silence is observed, state becomes `COMPLETE` but RX remains quarantined.
6. The master requests release only after its WP2.1 1000 ms late-response guard has expired. The transport task drains/checks UART data and event queues before transitioning to `IDLE`.

The transport component does not implement the 1000 ms late-response guard or 2000 ms recovery-operation deadline. Those belong to the Modbus master in WP2.3. A `FAILED` transport state deliberately blocks TX rather than silently resuming.

## Important integration contract for WP2.3

- After a failed attempt, the master calls `rs485_recovery_begin()` once.
- It must not submit TX while state is REQUESTED, ACTIVE, COMPLETE or FAILED.
- It must keep polling transport state and enforce the 1000 ms guard itself.
- If state becomes COMPLETE before the guard expires, the master continues waiting and does not release yet.
- Once both the 1000 ms guard and transport COMPLETE are satisfied, the master calls `rs485_recovery_release()`.
- It waits until transport state becomes IDLE before retrying or starting the next request.
- If state becomes FAILED or recovery exceeds the master-side 2000 ms deadline, the master marks the bus unavailable.

## Build and hardware validation

This package was source-edited in the provided environment, which does not contain the user's ESP-IDF v6.1 Windows toolchain. Therefore no ESP-IDF compile, flash, or hardware test is claimed.

On the Windows development machine, from the project root:

```powershell
idf.py -B build_selftest -D SDKCONFIG="$PWD\sdkconfig.selftest" build
```

Do not flash this WP2.2-only build for normal Modbus tests yet. The current Modbus master does not call the new recovery APIs, so WP2 stale-response isolation is not fixed until WP2.3 is implemented.

After a successful build, test the API from a temporary transport-level test task or the dedicated WP2.2 test harness before master integration. Required checks:

1. `begin()` changes state from IDLE to REQUESTED/ACTIVE.
2. RX data during recovery never appears as `RS485_EVT_RX_DATA` to the master.
3. TX submissions are rejected until state returns to IDLE.
4. A late frame while ACTIVE or COMPLETE resets the silence condition.
5. `release()` does not return the transport to IDLE until UART data and event queues are drained and silence is re-established.
6. A duplicate `begin()` and premature `release()` return `ESP_ERR_INVALID_STATE`.
7. UART cleanup failure leaves the transport in FAILED and TX blocked.

## Known limits / follow-up

- The existing stats getter remains a best-effort unsynchronized snapshot; a separate stats synchronization fix is still required.
- The 5 ms silence default is for the current 9600 8E1 configuration. Validate/derive timing for other UART formats before supporting them.
- The transport cannot guarantee protection against an arbitrarily late slave response arriving after the master-side guard expires. WP2.1's maximum supported response-latency contract remains mandatory. WP2.3 must also account for the maximum response-frame transmission time at the configured baud/framing: a response that begins just before the 1000 ms deadline can finish just after it. The master must not release quarantine until the guard plus a frame-drain margin and the transport bus-silence condition have both been satisfied.
- The existing transport task stack high-water mark must be checked on hardware after integration.
