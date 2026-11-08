# DP-WP4 D8 — Bus recovery escalation review (Design 9.7)

Plan v1.1 milestone D8: review the COM master's behaviour when RS-485 recovery fails, implement the application side of Design 9.7, and raise a COM change request for what the master is missing. Code baseline: `myems_rs485_modbus_rtu_09_057` at tag `dp-wp4-d7`. The COM components (`components/modbus_rtu_master`, `components/rs485_transport`) are **not changed** in DP-WP4 (Plan Section 5).

## 1. What happens today when recovery fails

Every failed transaction (timeout, CRC, protocol or transport error) puts the master into RS-485 recovery: the transport drains RX and waits for 5 ms of bus silence, then the master waits for the 1000 ms late-response guard and releases the transport.

| Step | Code (09_057) | Behaviour |
|---|---|---|
| Recovery fails | `rs485_transport.c` ~l.283: no 5 ms silence within `recovery_timeout_ms` (2000 ms) → `RS485_RECOVERY_FAILED`, log "bus remains blocked". `modbus_rtu_master.c` `service_recovery()` l.234: transport `FAILED`, release error, or its own 2000 ms start deadline (`RECOVERY_START_RETRY_MS`, l.15) | `recovery_fail_closed()` (l.224) |
| Fail closed | `recovery_fail_closed()` l.224–232 | Publishes **one terminal `TRANSPORT_ERROR`** for the active request, clears it and sets `TXN_BUS_UNAVAILABLE` |
| After that | master task loop l.467: `if (!t->active && t->state != TXN_BUS_UNAVAILABLE)` | **The master never takes another request from its queue.** Nothing leaves `TXN_BUS_UNAVAILABLE` except a stop. |
| New submissions | `modbus_master_submit()` l.639 checks only `lifecycle_state == RUNNING` (still RUNNING) | Accepted and queued (each holds a completion slot) but **never completed**. After 16, submit returns `ESP_ERR_TIMEOUT`. |
| Transport state | `rs485_recovery_begin()` l.790 needs `IDLE`; `rs485_recovery_release()` l.821 needs `COMPLETE`; transport task l.432 parks in `FAILED` | **`FAILED` is terminal**: no API leaves it. Only `rs485_close()` + `rs485_open()`, which needs the WP2.5-B3/B4 de-initialisation (not available). |
| Master gives up first | master deadline (l.262) can expire while the transport is still `ACTIVE`; if the bus then goes quiet the transport reaches `COMPLETE` and stays latched there (nobody releases it) | Same dead master, but the transport does **not** show `FAILED` |
| Cancellation | `service_cancel_pending()` l.395 runs on every loop, also in `TXN_BUS_UNAVAILABLE` | `modbus_master_cancel_pending()` (l.671) still completes every queued request with `CANCELLED` — **usable by the application** |
| Health | `modbus_master_stats_t` has no bus field; lifecycle stays `RUNNING` | **No way to read bus health** from the master |
| Silence detection | `rs485_transport.c` `recovery_drain_uart_rx()` / `handle_recovery_uart_event()` update `last_rx_activity_us` only when bytes reach the driver; no `uart_set_rx_full_threshold()` call, so the default RX-FIFO-full threshold (120 bytes) applies | **A continuously busy bus looks silent.** At 9600 8E1, 120 bytes take 137 ms; between two FIFO events the transport sees > 5 ms "silence" and completes recovery. HIL 2026-11-08 (`wp4_esc_20261108_145957`): a babbling simulator (0x55 at 879 B/s, line rate) gave "recovery complete; silence=5 ms" every ~137 ms; recovery never failed. A babbling device therefore never reaches the fail-closed path: each transaction fails, devices go OFFLINE through the normal path (Design 9.4) and keep being probed. |

Without D8, the DP-WP4 application on top of this would: see one `TRANSPORT_ERROR` (one failed transaction, device still ONLINE with threshold 2), submit the next poll into a master that never answers, then hit the scheduler guard (T_wc + 5 s) and keep the slot forever (Design 9.5). Values would go STALE by age, the device would **never be marked OFFLINE**, no bus event would be raised, and nothing would say why.

## 2. Design 9.7 against the 09_057 master

| Step | Owner | Required | 09_057 | Status |
|---|---|---|---|---|
| 1 | Master | Terminal `TRANSPORT_ERROR` for the active request; completion and scheduler slots released | Active request: yes. Requests accepted afterwards: held forever | **Partly met.** Application works around it with `modbus_master_cancel_pending()` (Section 3) |
| 2 | Master | Bus health `BUS_OK` / `BUS_RECOVERING` / `BUS_FAULT` via an API | None | **Not met** → CR-D8-01 (a). Application derives health (Section 3) |
| 3 | Master | Recovery retry with backoff 2 s, 5 s, then every 30 s; requests during `BUS_FAULT` completed at once with `TRANSPORT_ERROR` | No retry; transport `FAILED` terminal; requests queued and never completed | **Not met** → CR-D8-01 (b), (c) |
| 4 | Acquisition | On `BUS_FAULT`: every device on the bus OFFLINE, parameters COMM_LOST, `EVT_BUS_FAULT` | — | **Implemented (D8)** |
| 5 | Scheduler | All devices in probe mode; first successful probe after `BUS_OK` restores them | — | **Implemented (D8)**; the release path waits for a master that can report `BUS_OK` |
| 6 | Health | `HEALTH_BUS_FAULT` raised until the bus is OK | No health task in 09_057 | **Implemented (D8)** as the bus health state in diagnostics + scheduler flag `0x02`; event-log persistence comes with the storage work |

## 3. Application side (implemented in D8)

| Part | File | What it does |
|---|---|---|
| Bus health core | `main/ems_bus_health.c/.h` (pure C, host-tested) | Derives health every ≤ 100 ms from what COM exposes. **BUS_FAULT** when: transport recovery `FAILED` (`TRANSPORT_FAILED`); transport in recovery > 5 s, i.e. latched (`RECOVERY_STUCK`); or the master holds requests and completes none for longer than the largest scheduler guard, 8.1 s with `lab_bench` (`MASTER_STALLED`). Normal recoveries (≤ ~1.1 s) show as `BUS_RECOVERING` only. Without a master health report **BUS_FAULT is latched** (fail closed). With a report (after the CR) the report decides and FAULT ends only on `OK`. |
| Acquisition | `main/ems_acquisition.c` | Reads `rs485_recovery_get_state()` and `modbus_master_get_stats()` each loop. On FAULT: `EVT_BUS_FAULT bus=… reason=…`, tells the scheduler to hold, and forces every device on the bus OFFLINE (`ems_acq_core_force_offline`: all parameters COMM_LOST, last value and time kept, `EVT_DEVICE_OFFLINE`). On OK: `EVT_BUS_OK`, scheduler released. |
| Scheduler | `main/ems_sched_core.c`, `main/ems_poll_scheduler.c` | `ems_sched_set_bus_hold()`: nothing is submitted, every device in probe mode, health flag `EMS_SCHED_HEALTH_BUS_HOLD` (0x02). The task then calls `modbus_master_cancel_pending()` — allowed because the scheduler is the only submitter and has stopped — so every request stuck in the master returns `CANCELLED` and **releases its slot through normal feedback** (the guard rule of Design 9.5 is unchanged). On release, every OFFLINE device is probed at once. |
| Diagnostics | `main/app_main.c` | `STATS … health=0x02 in_flight=0 bus_holds=1 bus_cancelled=…` and `DIAG bus … | health=BUS_FAULT reason=… transport=… bus_faults=… bus_recoveries=…` every 60 s |

Fault sequence on the bench (HIL suite `esc`, `Wp4Esc` build, `lab_bench`: timeout 500 ms, 1 retry):

1. The simulator stops answering. The next poll (≤ 5 s) times out at 0.5 s and the master enters recovery.
2. The recovery sees no silence for 2 s (test hook), so it FAILS and the master publishes `TRANSPORT_ERROR`.
3. Within 100 ms the firmware sees transport `FAILED` and emits `EVT_BUS_FAULT reason=TRANSPORT_FAILED`.
4. The scheduler holds and cancels the queued poll; its `CANCELLED` result releases the slot (`in_flight=0`).
5. The device goes OFFLINE with every parameter COMM_LOST (`EVT_DEVICE_OFFLINE … bus fault`).
6. Nothing more is sent on the bus until reboot.

## 4. COM change request CR-D8-01

**Title:** Modbus master — bus health API and bounded recovery retry after RS-485 recovery FAILED
**Raised by:** DP-WP4 D8 (Design 9.7 steps 1–3). **Owner:** COM track. **Depends on:** WP2.5-B3/B4 for (c) option 2.

| # | Change | Detail |
|---|---|---|
| (a) | Bus health API | `esp_err_t modbus_master_get_bus_health(modbus_master_handle_t h, modbus_bus_health_t *out)` with `state` (`MODBUS_BUS_OK`, `MODBUS_BUS_RECOVERING`, `MODBUS_BUS_FAULT`), `fault_count`, `last_fault_reason` (transport FAILED / release failed / start deadline), `next_retry_ms`. Atomic snapshot like `get_stats`. |
| (b) | No silent queueing while faulted | While the bus is unavailable, complete every queued request **and** every new submission at once with `MODBUS_RESULT_TRANSPORT_ERROR` (consumes its completion slot as usual). Never hold an accepted request without a terminal result. |
| (c) | Bounded recovery retry | From `TXN_BUS_UNAVAILABLE`, retry recovery after 2 s, 5 s, then every 30 s. Needs a way out of transport `FAILED`: **option 1** a transport `rs485_recovery_reset()` (FAILED → IDLE after flushing UART and queues; transport task only), or **option 2** close and reopen the transport (WP2.5-B3/B4). On success: state `BUS_OK`, normal dequeuing resumes. |
| (d) | Test support | Fault injection to force transport `FAILED` **and to let the next retry succeed**, so ESC-004/005 can be checked. (Forcing FAILED already exists: `rs485_test_force_recovery_activity`, used by the D8 `Wp4Esc` build.) |
| (e) | Silence detection during recovery | Measure bus silence at byte resolution: e.g. `uart_set_rx_full_threshold()` of 1–8 bytes while recovering (and back afterwards), or poll `uart_get_buffered_data_len()` / the UART RX status each loop, so that a continuously busy bus (babbling device) keeps recovery ACTIVE and ends in FAILED after `recovery_timeout_ms`. Today such a bus is treated as silent every ~137 ms (see Section 1). |

**Application integration once (a)–(c) exist:** one line in `bus_check()` (`main/ems_acquisition.c`): set `in.master_report` from `modbus_master_get_bus_health()`. The bus health core already handles reports (host tests `test_ESC005_master_report_drives_recovery`, `test_master_report_ok_after_fault_is_change_ok`): FAULT ends only on `OK`, then `EVT_BUS_OK`, the scheduler probes and devices return ONLINE on their first answer.

**Acceptance:** extend HIL suite `esc` with a phase after the babble ends: `EVT_BUS_OK` within one backoff step (≤ 30 s + 2 s), probe ≤ 1 s later, `EVT_DEVICE_ONLINE`, values GOOD; attempt times ≈ 2 s, 5 s, 30 s from the master log (ESC-004).

## 5. Test status

| Test | Host | HIL suite `esc` |
|---|---|---|
| ESC-001 active request completes with TRANSPORT_ERROR; no stuck slot | `test_ESC001_hold_stops_polling_and_keeps_stuck_slot`, `test_master_stalled_after_guard` | master log, DIAG transport ≥ 1, BUS_HOLD, cancelled = CANCELLED consumed, `in_flight=0` |
| ESC-002 BUS_FAULT in diagnostics, EVT_BUS_FAULT | `test_ESC002_transport_failed_is_fault_once` + 6 rule tests | 1 `EVT_BUS_FAULT` ≤ 12 s after the jam, scheduler flag 0x02, DIAG `health=BUS_FAULT`, 0 requests on the bus afterwards |
| ESC-003 all devices OFFLINE, COMM_LOST | `test_ESC003_bus_fault_forces_offline_comm_lost` | `EVT_DEVICE_OFFLINE`, VALUES mode OFFLINE, all COMM_LOST, last value kept |
| ESC-004 backoff 2 s / 5 s / 30 s | — | **BLOCKED** (CR-D8-01 c) |
| ESC-005 recovery → EVT_BUS_OK, devices via probing | report path + scheduler release (`test_ESC005_*`) | **BLOCKED** (CR-D8-01 a–c); checked instead: BUS_FAULT stays latched, no `EVT_BUS_OK` |

**How the fault is made:** Test Spec ESC-001 asks for a test hook. Build mode `Wp4Esc` (`sdkconfig.wp4_esc` = `wp4_sim` + `CONFIG_MYEMS_WP4_ESC_TEST`, which selects `CONFIG_MYEMS_WP24_FAULT_INJECTION`) arms the existing transport hook `rs485_test_force_recovery_activity()` at start-up: during a recovery the transport always sees RX activity, so the recovery fails after 2 s. The runner then makes the simulator stop answering (`TIMEOUT`); the first failed transaction's recovery fails and everything after the transport — the master's fail-closed path, bus health, scheduler hold, device state — is the normal code. The boot log shows `BUILD_VARIANT=WP4_ESC_TEST`; the runner refuses `esc` on another build and `d6`/`d7` on this one.

The first attempt (bus mode `BABBLE` in the simulator: 0x55 without a gap) did not make recovery fail, which led to finding (e). The mode stays in the simulator as a "babbling device" scenario; with the 09_057 transport its expected result is per-device OFFLINE, not BUS_FAULT.

## 6. Limitations

- With the 09_057 master a bus fault is permanent until reboot. This is fail-closed by design (no request reaches a master that cannot answer); recovery needs CR-D8-01.
- One RS-485 bus (one master, bus 0). The board has a single RS-485 port; the bus-fault handling covers the devices with `bus_index` 0.
- `MASTER_STALLED` uses the largest device guard (T_wc + 5 s) as the limit, so it reacts in ≤ ~8.2 s with `lab_bench`. The scheduler guard event (Design 9.5) for the stuck device may come at the same time.
