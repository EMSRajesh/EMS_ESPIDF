# DP-WP4 milestone D6 — Acquisition: decode, publish, quality

Plan v1.1 milestone D6 (Design 7.3, 8.5, 9.1, 9.3). Tag after acceptance: `dp-wp4-d6`.

## Delivered

| Path | Change |
|---|---|
| `main/ems_acq_core.c`, `main/include/ems_acq_core.h` | **New.** Pure-C acquisition core: state-store layout from the generated tables, routing check, decoding with the D2 engine (scale, ratio, range), Design 9.3 result → quality table, energy counters, freshness, per-device counters |
| `main/ems_acquisition.c`, `main/include/ems_acquisition.h` | Uses the core. State store with a short critical-section lock. Freshness check on every wake-up (≤ 100 ms). Events in the log. Value dump (`VALUES` / `VAL` lines). Device stats API. Stack 4096 → **6144 B** |
| `main/app_main.c` | Acquisition stack 6144 B; value dump every 5 s (sim build) / 60 s (production); `stale=` in the STATS line |
| `main/CMakeLists.txt` | Adds `ems_acq_core.c` |
| `test/host/test_ems_acq_core.c`, `test_main_acq.c`, `CMakeLists.txt`, `README.md` | 22 host tests on the real generated `lab_bench` tables |
| `tools/wp4/myems_wp4_runner.py` | **New.** HIL runner: simulator + console capture + 9 phases + automatic checks + evidence folder |
| `tools/wp4/tests/test_wp4_runner.py` | 18 tests of the runner analysis with a scripted firmware |

## Result → quality (as implemented, Design 9.3)

| Terminal result | Comm failure counter | Parameters of the group | Log |
|---|---|---|---|
| OK, decoded | reset to 0 | GOOD; OUT_OF_RANGE (value kept, timestamp not moved) or INVALID per mapping | warning if any OUT_OF_RANGE/INVALID (rate-limited) |
| OK, wrong register count | reset to 0 | INVALID; `routing_errors` +1 | `EVT_RESPONSE_MISMATCH` |
| EXCEPTION | reset to 0 | INVALID | `EVT_MODBUS_EXCEPTION code=…` |
| TIMEOUT, CRC, PROTOCOL, TRANSPORT | +1 | GOOD → STALE; others unchanged (INITIALIZING stays INITIALIZING) | warning, max 1 per device per 10 s |
| CANCELLED | unchanged | unchanged (record bytes identical) | info |
| Unknown entry, slave or FC mismatch | – | nothing; `routing_errors` +1 | warning |

The freshness check turns every GOOD value older than its group's stale timeout into STALE. For `lab_bench` that is 11 s (electrical), 21 s (energy) and 61 s (status). Every result, including routing errors, is still fed back to the scheduler.

**Counters:**
- Updated only on a GOOD read; an OUT_OF_RANGE or failed read does not move the baseline.
- A decrease or reset gives `EVT_COUNTER_DISCONTINUITY`: the value is still published GOOD, but there is no delta.
- The plausibility limit is the profile's `counter_max_rate_per_hour` × the mapping's CT/PT factor (decided in D3).

**Decision (RTE-003 vs Design 9.3):** an OK result with the wrong register count is counted as a routing error, and the group is set INVALID. That satisfies both texts: the data is discarded and the consumer sees INVALID instead of an old GOOD.

## Value dump format (parsed by the runner)

```
I (123456) ems_acquisition: VALUES slot=0 GRID_INCOMER_01 count=14
I (123456) ems_acquisition: VAL slot=0 ac.voltage.l1_n=230 V q=GOOD age_ms=1234 seq=57
```

## HIL runner phases (suite d6, ~5 min)

| Phase | Simulator | Checks |
|---|---|---|
| normal 40 s | NORMAL | UMD-001 (14 parameters), Q-002 (all GOOD = Test Spec 4.1), UMD-004 (units), UMD-008 (age) |
| import 40 s | IMPORT | UMD-006: +12.5 kW GOOD, import counter rises |
| export 40 s | EXPORT | UMD-007 / CONV-008: −12.5 kW, PF −0.972, export counter rises |
| timeout 30 s | TIMEOUT | Q-003: electrical + energy STALE, last GOOD value kept (−12.5 kW); Q-009: never GOOD after the loss |
| recover 25 s | NORMAL | Q-007: GOOD again with a new timestamp |
| out_of_range 20 s | OUT_OF_RANGE | Q-006 / SIM-009: frequency 30 Hz OUT_OF_RANGE, rest of the group GOOD |
| exception_energy 30 s | EXCEPTION:02@energy | Q-005: energy INVALID, electrical GOOD, `EVT_MODBUS_EXCEPTION code=0x02` |
| counter_reset 25 s | COUNTER_RESET | ENG-008: exactly one `EVT_COUNTER_DISCONTINUITY`, import counter GOOD from the new baseline |
| final 15 s | NORMAL | – |
| whole run | | boot banner + CRC = simulator CRC; no crash/reboot; last STATS `routing_errors=0 guard_expired=0`; acquisition stack ≥ 1024 B free |

Device OFFLINE / COMM_LOST (DEV-*) is D7. In D6 a dead device only produces STALE.

## Verified before delivery

- **Host tests:** 6 executables pass with gcc, clang, gcc with x87 maths, and AddressSanitizer/UBSan.
- **Mutation testing:** 22 deliberate defects in the acquisition core and 12 in the runner analysis. All core defects were caught. Two runner defects survived at first: one was a weak test fixture (now fixed), the other is harmless (removing the colour-code stripping changes nothing).
- **Firmware sources:** compiled against stub headers with ESP32 integer types for the production, wp4_sim and self-test variants, `-Werror`. The real ESP-IDF build is step 2 below.

## Exit criteria

| # | Criterion | Command | Result |
|---|---|---|---|
| 1 | Host tests: 6 executables pass | `ctest` | |
| 2 | Python tests pass (63 + 76) | `unittest` | |
| 3 | Production, self-test and wp4_sim firmware build | `build_firmware.ps1` | |
| 4 | HIL suite d6 PASS | `myems_wp4_runner.py` | |
| 5 | Acquisition stack margin ≥ 1024 B | runner `stack` line | |

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | |
| Commit | |
| Runner evidence folder | |
| Acquisition min free stack | |
