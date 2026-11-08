# DP-WP4 milestone D8 — Bus recovery escalation

Plan v1.1 milestone D8 (Design 9.7, Test Spec Section 17). Tag after acceptance: `dp-wp4-d8`.
Review findings and the COM change request: [`D8_ESCALATION_REVIEW.md`](D8_ESCALATION_REVIEW.md).

## Outcome of the review

The 09_057 master fails closed correctly for the active request (terminal `TRANSPORT_ERROR`), but then stops taking requests forever, has no bus-health API and never retries recovery; the transport's `FAILED` state is terminal. Steps 2 and 3 of Design 9.7 are missing → **COM change request CR-D8-01**. The application side (steps 4–6) is implemented now; ESC-004/005 are BLOCKED against the CR.

Found on the bench (first `esc` run, 2026-11-08): the transport measures bus silence only per 120-byte UART FIFO event, so a continuously busy (babbling) bus looks silent and recovery never fails → CR-D8-01 (e). The `esc` suite therefore uses the test hook that Test Spec ESC-001 foresees, in the new build mode `Wp4Esc`.

## Delivered

| Path | Change |
|---|---|
| `main/ems_bus_health.c`, `main/include/ems_bus_health.h` (new) | Pure-C bus health: OK / RECOVERING / FAULT derived from transport recovery state and master counters (TRANSPORT_FAILED, RECOVERY_STUCK > 5 s, MASTER_STALLED > scheduler guard); FAULT latched without a master report; master report supported for CR-D8-01 |
| `main/ems_sched_core.c/.h` | `ems_sched_set_bus_hold()`: no submissions, all devices in probe mode, health flag `0x02`; release probes every OFFLINE device at once; `ems_sched_in_flight()` |
| `main/ems_poll_scheduler.c/.h` | Bus-hold message; on hold cancels what is still in the master (`modbus_master_cancel_pending`), so CANCELLED results release the slots; `in_flight`, `bus_hold`, `bus_cancelled` in the stats |
| `main/ems_acq_core.c/.h` | `ems_acq_core_force_offline()`: ONLINE → OFFLINE, all parameters COMM_LOST |
| `main/ems_acquisition.c/.h` | Bus check every loop: `EVT_BUS_FAULT` → scheduler hold → devices OFFLINE (`EVT_DEVICE_OFFLINE … bus fault`); `EVT_BUS_OK` → release; bus health in the stats; new config field `transport` |
| `main/app_main.c` | Passes the transport handle; STATS adds `in_flight`, `bus_holds`, `bus_cancelled`, `cancelled`; DIAG bus adds `health`, `reason`, `transport`, `bus_faults`, `bus_recoveries` |
| `main/CMakeLists.txt` | adds `ems_bus_health.c` |
| `main/Kconfig.projbuild` | `MYEMS_WP4_ESC_TEST` (simulation builds only; selects `MYEMS_WP24_FAULT_INJECTION`, whose dependency now allows it) |
| `main/app_main.c` (ESC build only) | `BUILD_VARIANT=WP4_ESC_TEST` and `rs485_test_force_recovery_activity(transport, true)` after `rs485_open` |
| `sdkconfig.wp4_esc` (new), `sdkconfig.wp4_sim` | ESC build configuration; `wp4_sim` gets the explicit "is not set" line |
| `tools/build_firmware.ps1`, `BUILD_MODES.md` | Mode `Wp4Esc` (build directory `build_wp4_esc`) |
| `tools/wp4/myems_ac_meter_sim.py` | Bus mode `BABBLE` / `QUIET` (`bus …` control command): continuous transmission, so recovery really fails |
| `tools/wp4/myems_wp4_runner.py` | Suite `esc` (normal → simulator TIMEOUT → fail-closed); checks `BUILD_VARIANT` (esc needs Wp4Esc, d6/d7 refuse it); BLOCK rows (listed, not failed); `RESULT: PASS, 2 BLOCKED` |
| `test/host/test_ems_bus_health.c`, `test_main_d8.c` (new), `CMakeLists.txt` | Executable `test_ems_d8` (14 tests) |
| `test/host/test_ems_sched_core.c`, `test_ems_acq_core.c` | +4 bus-hold tests, +2 force-offline tests |
| `tools/wp4/tests/test_ac_meter_sim.py`, `test_wp4_runner_d8.py` (new) | +5 BABBLE tests; 21 esc-analysis and build-variant tests |
| `docs/D8_ESCALATION_REVIEW.md`, `docs/D8_CHECKLIST.md`, READMEs | Review, CR-D8-01, this checklist |

COM components (`components/modbus_rtu_master`, `components/rs485_transport`): **unchanged**.

## New log lines

```
E ems_acquisition: EVT_BUS_FAULT bus=<id> reason=TRANSPORT_FAILED transport=FAILED outstanding=1: HEALTH_BUS_FAULT; polling held, ...
E ems_acquisition: bus <id> stays BUS_FAULT until the master reports BUS_OK; ... (COM change request CR-D8-01): restart the controller to recover
W ems_scheduler: BUS_HOLD: polling stopped, every device in probe mode; 1 request(s) in flight
W ems_scheduler: BUS_HOLD: cancelled 1 request(s) in the master; their CANCELLED results release the slots
W ems_acquisition: EVT_DEVICE_OFFLINE slot=0 GRID_INCOMER_01: bus fault; all parameters COMM_LOST
I myems_app: STATS sched: ... health=0x02 in_flight=0 bus_holds=1 bus_cancelled=1 | acq: ... cancelled=1 ...
I myems_app: DIAG bus ... | health=BUS_FAULT reason=TRANSPORT_FAILED transport=FAILED bus_faults=1 bus_recoveries=0
```

In normal operation the DIAG bus line shows `health=BUS_OK reason=NONE transport=IDLE bus_faults=0` (or `BUS_RECOVERING` right after a failed transaction).

## Verified before delivery

- **Host tests:** 8 executables (sched 27, acq 33, d8 14, others unchanged), passing with gcc + AddressSanitizer/UBSan, clang, and gcc with x87 maths. `ems_bus_health.c`: 99 % of lines, 98 % of branches.
- **Mutation testing:** 17 deliberate defects in the bus-health rules, bus hold and force-offline; 16 caught, 1 equivalent (stall check while nothing is outstanding: that case is already covered by the progress refresh). The run found one missing test (a long idle period followed by a new request must not count as a stall), which was added.
- **Python tests:** 128 wp4 tests (was 102: +5 simulator, +21 esc analysis) and 63 profile-tool tests pass.
- **Firmware sources:** compiled against stub headers with ESP32 integer types for wp4_sim, wp4_esc, production and self-test, `-Werror -Wformat=2 -Wconversion`.
- **Not verified here:** ESP-IDF build, real stack use and the HIL runs; those are the exit criteria below.

## Exit criteria

| # | Criterion | Command | Result |
|---|---|---|---|
| 1 | Host tests: 8 executables pass | `ctest` | PASS 2026-11-08 |
| 2 | Python tests pass (63 + 128) | `unittest` | PASS 2026-11-08 |
| 3 | Production, self-test, wp4_sim, wp4_esc build | `build_firmware.ps1` | PASS 2026-11-08 |
| 4 | HIL d7 and d6 still PASS on Wp4Sim (regression; no false bus fault) | runner `--suite d7`, `--suite d6` | PASS 2026-11-08 (`wp4_d7_20261108_144958`, `wp4_d6_20261108_145432`) |
| 5 | HIL suite `esc` on Wp4Esc: PASS, 2 BLOCKED (ESC-004/005, CR-D8-01) | runner `--suite esc` | PASS, 2 BLOCKED 2026-11-08 (`wp4_esc_20261108_151633`): BUS_FAULT 5.2 s after the fault, reason TRANSPORT_FAILED; 1 request cancelled, in_flight=0; 0 requests on the bus afterwards |
| 6 | CR-D8-01 handed to the COM track | `D8_ESCALATION_REVIEW.md` Section 4 | open (items a–e) |

`esc` needs the Wp4Esc firmware; flash Wp4Sim again afterwards for any other suite (the runner checks the build variant).

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | 2026-11-08 |
| Commit | |
| esc / d7 / d6 evidence folders | `wp4_esc_20261108_151633` / `wp4_d7_20261108_144958` / `wp4_d6_20261108_145432` |
| CR-D8-01 reference (COM track) | |
