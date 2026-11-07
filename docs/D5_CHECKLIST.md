# DP-WP4 milestone D5 — Poll scheduler

Plan v1.1 milestone D5 (Design 9.1, 9.2, 9.5, 9.6). Tag after acceptance: `dp-wp4-d5`.

## Delivered

| Path | Change |
|---|---|
| `main/ems_sched_core.c`, `main/include/ems_sched_core.h` | **New.** Pure-C scheduler core: plan (poll entries from the generated tables), request ID `(entry << 24) \| seq24`, one in-flight per device, earliest deadline first, skip missed periods, rejected submission retry, probe mode, guard rule, T_wc |
| `main/ems_poll_scheduler.c`, `main/include/ems_poll_scheduler.h` | **Rewritten** as a FreeRTOS wrapper around the core. One message queue for completion feedback and device-mode changes. **Guard expiry no longer releases the slot.** Stats snapshot API |
| `main/ems_acquisition.c`, `main/include/ems_acquisition.h` | Routes results by request ID (no more lookup by slave address); checks slave, FC and count against the entry (`routing_errors`); feedback for every result; one log line per result. Decoding comes in D6 |
| `main/app_main.c` | Demo meter profiles removed. Polling path: config banner, CRC check, build-target check (V-13), plan build; bus settings from the generated bus table; counters every 60 s. **Self-test path unchanged** |
| `main/Kconfig.projbuild` | `MYEMS_CONFIG_TARGET_SIM` |
| `main/CMakeLists.txt` | Adds `ems_sched_core.c`, `generated/ems_config_gen.c`; requires `ems_profile`, `ems_measurement`, `ems_register_map` |
| `sdkconfig.wp4_sim` | **New** build mode = production + `CONFIG_MYEMS_CONFIG_TARGET_SIM=y` |
| `tools/build_firmware.ps1`, `BUILD_MODES.md` | Mode `Wp4Sim` (build dir `build_wp4_sim`) |
| `tools/profile/myems_profile_tool.py` | `export-sim` also writes `offset_ms` and `stale_timeout_ms` per poll group |
| `tools/wp4/wp4_sched_check.py` | **New.** Checks SCH-001/002/003 from the simulator log |
| `test/host/test_ems_sched_core.c`, `test_main_sched.c`, `CMakeLists.txt`, `README.md` | 22 host tests |
| `tools/wp4/tests/test_sched_check.py` | 12 tests for the checker |

## Behaviour changes to be aware of

1. **Production build no longer polls.** Only simulation configurations exist, so `-Mode Production` boots, prints `CONFIG REJECTED … in a CONFIG_TARGET=prod build` and starts nothing. That is the intended V-13 behaviour (Plan Section 11). Use `-Mode Wp4Sim` with the simulator.
2. **What gets polled:** the demo polls (slaves 1 and 2, registers 0+2) are gone. The `lab_bench` configuration polls slave 1: electrical 0+14 every 5 s, energy 14+4 every 10 s, status 18+1 every 30 s.
3. **Guard expiry:** after T_wc + 5 s (8.1 s) without feedback, the scheduler logs `EVT_SCHED_GUARD_EXPIRED` once and raises `HEALTH_SCHED_FEEDBACK`. The device stays blocked until matching feedback arrives. In 09_057 the slot was released, which could put two requests for one meter in flight.
4. **Device-mode messages** (ONLINE/OFFLINE) are already handled by the scheduler. They are sent only from D7 on, when acquisition tracks the device state.

## Test coverage

| Test | Host (core, simulated time) | HIL (simulator log) |
|---|---|---|
| SCH-001 intervals and offsets | exact times for 120 s | `wp4_sched_check.py`, ±100 ms |
| SCH-002 missed periods skipped | device busy 12 s: one poll after release, then back on the grid | no gap < ½ interval |
| SCH-003 one in-flight per device | 2 devices, random reply times, 10 min | `OVERLAP` count = 0 (run with `SLOW:400`) |
| SCH-004 two devices due together | tie order and earliest deadline first | D9 (two-device site) |
| SCH-005 rejected submission | retried after 250 ms, counted | – |
| SCH-006 OFFLINE probe mode | only status, first after 30 s, then every 30 s; probe interval ≠ group interval | D7 |
| SCH-007 back ONLINE | all groups at once, one after the other | D7 |
| SCH-008 guard expiry | not released, counted once, health flag, other device unaffected; expires at exactly T_wc + 5 s | needs a test build that drops feedback (D7) |
| SCH-009 late feedback | released, flag cleared; flag stays while another device is stuck | D7 |
| SCH-010 non-matching feedback | ignored, counted, slot stays pending | – |
| RTE-001 request IDs | entry in bits 31–24, seq wraps 0xFFFFFF → 1, never 0 | – |

Verified before delivery:
- **Host:** the host tests pass with gcc, clang, gcc with x87 maths and AddressSanitizer/UBSan.
- **Mutation testing:** 18 deliberate defects in the core and 7 in the checker were all caught.
- **Firmware sources:** compiled against FreeRTOS/ESP-IDF stub headers with ESP32-style integer types (`uint32_t` = `unsigned long`) for the production, wp4_sim and self-test variants, `-Wall -Wextra -Wformat=2 -Werror`. This finds format and type errors, but it does **not** replace the real ESP-IDF build below.

## Exit criteria

| # | Criterion | Command | Result |
|---|---|---|---|
| 1 | Host tests: 5 executables pass | `ctest` | |
| 2 | Python tests pass (63 + 56) | `unittest` | |
| 3 | Production, self-test and wp4_sim firmware build | `build_firmware.ps1` | |
| 4 | Production firmware refuses the simulation config | boot log `CONFIG REJECTED` | |
| 5 | wp4_sim firmware polls the simulator; SCH-001/002/003 PASS | `wp4_sched_check.py` | |
| 6 | No routing errors, no guard expiry in 2 min | `STATS` line in the ESP32 log | |
| 7 | COM regression unchanged (REG-03, optional for D5) | `myems_wp24_all_tests.py` | |

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | |
| Commit | |
| Firmware CRC in boot log | 0x4B8B5924 (expected) |
| wp4_sched_check result | |
