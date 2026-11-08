# DP-WP4 milestone D7 — Device state, diagnostics, events, activation

Plan v1.1 milestone D7 (Design 5.3, 9.4, 9.5, 9.9). Tag after acceptance: `dp-wp4-d7`.

## Delivered

| Path | Change |
|---|---|
| `components/ems_diag/` (new) | `ems_event.h/.c`: 64-entry event ring, 16 B per event, sequence numbers, overwrite-oldest with drop count, injected lock |
| `components/ems_profile/include/ems_profile.h`, `src/ems_profile_activate.c` (new) | Firmware activation checks: counts/pointers, CRC, V-13 target/classification, structural rules V-02..V-12; report with rule ID and index |
| `components/ems_profile/CMakeLists.txt` | adds activation; requires `ems_measurement` |
| `main/ems_acq_core.c/.h` | Device state ONLINE/OFFLINE/DISABLED: OFFLINE after `comm_lost_threshold` consecutive failures (all parameters COMM_LOST); ONLINE on the first OK or EXCEPTION; transition counters |
| `main/ems_sched_core.c/.h` | Per-entry latency (last/max, submit → feedback) |
| `main/ems_poll_scheduler.c/.h` | `EVT_SCHED_GUARD_EXPIRED` to the event ring; message-queue high-water mark |
| `main/ems_acquisition.c/.h` | Sends ONLINE/OFFLINE to the scheduler (probe mode); `EVT_DEVICE_OFFLINE/ONLINE`, exception, mismatch and counter events to the ring; `mode=` in the VALUES line |
| `main/app_main.c` | Event ring initialised first; activation via `ems_profile_activate` → `EVT_CONFIG_ACTIVATED` / `EVT_CONFIG_REJECTED rule=…`; DIAG block every 60 s |
| `main/CMakeLists.txt` | requires `ems_diag` |
| `test/host/test_ems_profile_activate.c`, `test_ems_event.c`, `test_main_d7.c` | 20 tests (new executable `test_ems_d7`) |
| `test/host/test_ems_acq_core.c`, `test_ems_sched_core.c` | +9 device-state / DIAG tests, +1 latency test |
| `tools/wp4/myems_wp4_runner.py` | Suites `d6`, `d7`, `reject`; simulator-log analysis (probe timing); wall-clock phase times |
| `tools/wp4/tests/test_wp4_runner_d7.py` | 21 tests of the d7/reject analysis |

## Behaviour

**Device state (Design 9.4).** With the `lab_bench` settings (threshold 2, probe group `status`, probe every 30 s):

| Event | Effect |
|---|---|
| 1st failed transaction | that group's GOOD values → STALE |
| 2nd consecutive failure | `EVT_DEVICE_OFFLINE`; **all 14 parameters COMM_LOST** (last value and timestamp kept); scheduler polls only `status`, first probe 30 s later |
| Probe fails | stays OFFLINE; next probe 30 s later |
| Any answer (OK or exception) | `EVT_DEVICE_ONLINE`; all groups polled at once; each group becomes GOOD on its own next read |
| EXCEPTION | counts as "device answered": resets the failure count, never takes a device OFFLINE |
| CANCELLED | does not change the failure count |
| `enabled: false` | DISABLED: never polled, parameters NOT_SUPPORTED |

**Activation order (fixed during D7):**
1. Counts and pointers (V-15, V-01).
2. CRC.
3. Target and classification (V-13).
4. Maps (V-02/V-05..V-09), dictionary (V-04), references (V-03), buses and devices (V-10), ratio rule (V-12), poll-entry limit (V-15).

Checking counts *before* the CRC matters. AddressSanitizer showed that a corrupted count in the generated file would otherwise make the CRC computation read past a table.

**Log lines** (every 60 s):

```
STATS sched: ... guard_expired=0 ... | acq: ... routing_errors=0 stale=..
DIAG slot=0 GRID_INCOMER_01 mode=ONLINE ok=.. timeouts=.. crc=.. ... offline=.. online=.. last_ok_age_ms=..
DIAG entry=0 slot=0 grp=0 submitted=.. skipped=.. rejected=.. latency_ms=.. max_latency_ms=..
DIAG bus accepted=.. completed=.. timeouts=.. ... | sched_queue_hwm=.. sched_queue_full=.. | events=.. event_drops=..
```

## Test coverage

| Test | Where |
|---|---|
| CFG-017 altered byte → CRC | host (`test_ems_d7`), on copies of the generated tables |
| CFG-018 sim config in production build | host + **HIL suite `reject`** |
| CFG-019 valid config activates | host + HIL suite `d7` (`EVT_CONFIG_ACTIVATED`) |
| CFG-020 mapping outside its group | host (CRC recomputed, V-06) |
| DEV-001 OFFLINE, COMM_LOST | host + HIL d7 (`OFFLINE_THEN_RECOVER:65`) |
| DEV-002 probe → ONLINE, groups recover separately | host + HIL d7 (twice: after OFFLINE_THEN_RECOVER and after BAD_CRC) |
| DEV-003 one of two devices offline | D9 (two-device site) |
| DEV-004 exceptions keep device ONLINE | host + HIL d7 |
| DEV-005 disabled device | host |
| DEV-006 BAD_CRC → OFFLINE | host + HIL d7 |
| SCH-006 only probe group, every 30 s ± 1 s | host (D5) + HIL d7 (simulator log) |
| SCH-007 all groups at once after ONLINE | host (D5) + HIL d7 (simulator log) |
| DIAG-001/002 counters, last-OK time | host |
| DIAG-003 event ring overflow | host (burst of 100) + HIL `event_drops=0` |
| DIAG-004 ≤ 1 warning per device per 10 s | HIL d7 |
| DIAG-005 stack and queue high-water in diagnostics | HIL d7 |
| SCH-008/009 guard expiry | host (D5); a firmware test build that drops feedback is not part of DP-WP4 D7 |

**Verified before delivery:**
- **Host tests:** 7 executables pass with gcc, clang, gcc with x87 maths, and AddressSanitizer/UBSan.
- **Python tests:** 63 profile-tool + 100 wp4 tests pass.
- **Mutation testing:** deliberate defects in the activation checks, device state and d7 analysis were all caught except one harmless case: a d7 runner check that is duplicated by another check in the same test.
- **Firmware sources:** compiled against stub headers with ESP32 integer types for production, wp4_sim and self-test, `-Werror`.

## Exit criteria

| # | Criterion | Command | Result |
|---|---|---|---|
| 1 | Host tests: 7 executables pass | `ctest` | |
| 2 | Python tests pass (63 + 100) | `unittest` | |
| 3 | Production, self-test, wp4_sim build | `build_firmware.ps1` | |
| 4 | HIL suite d7 PASS | runner `--suite d7` | |
| 5 | HIL suite reject PASS (production firmware) | runner `--suite reject` | |
| 6 | HIL suite d6 still PASS on D7 firmware (regression) | runner `--suite d6` | |

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | |
| Commit | |
| d7 / reject / d6 evidence folders | |
