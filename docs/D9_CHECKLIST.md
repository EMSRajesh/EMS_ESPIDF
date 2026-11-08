# DP-WP4 milestone D9 — Stabilisation

Plan v1.1 milestone D9: two-device tests, ratio site, soak, resource measurement, regression. Exit: PERF-*, REG-* pass; Test Spec acceptance gate (Section 23) met. Tag after acceptance: `dp-wp4-d9`. The gate itself is recorded in [`DP-WP4_ACCEPTANCE_RECORD.md`](DP-WP4_ACCEPTANCE_RECORD.md).

## Delivered

| Path | Change |
|---|---|
| `main/generated_sites/lab_bench_2dev/`, `lab_ratio_ct200/` (new, generated) | Firmware tables for the two-meter site (CRC 0xE3FF2DF7) and the CT 200 site (CRC 0x5A2CC286) |
| `main/Kconfig.projbuild`, `main/CMakeLists.txt` | Choice `MYEMS_SIM_SITE` (lab_bench / lab_bench_2dev / lab_ratio_ct200) selects the table folder; `main/generated` stays lab_bench |
| `sdkconfig.wp4_sim_2dev`, `sdkconfig.wp4_ratio` (new); `sdkconfig.wp4_sim`, `sdkconfig.wp4_esc` | Site lines; build modes `Wp4Sim2Dev`, `Wp4Ratio` in `tools/build_firmware.ps1` and `BUILD_MODES.md` |
| `tools/profile/myems_profile_tool.py` | "Regenerate:" header comment names `--out-dir` when it is not `main/generated` (copying the old comment would have overwritten lab_bench) |
| `test/host/generated/decoder_test/` | Regenerated: header comment only |
| `main/app_main.c` | `PERF heap_free_at_start=…` once; every 60 s `PERF heap_free=… heap_min=… \| acq_proc_us elec_last=… elec_max=… max=… \| master_outstanding_hwm=… completion_backlog_hwm=…`; STACK line for the `main` task |
| `main/ems_acquisition.c/.h` | Processing time per result (PERF-006); request-queue and completion-queue high-water marks derived from the master counters (PERF-004) |
| `tools/wp4/myems_wp4_runner.py` | Suites `two_dev`, `ratio`, `load`, `soak`; `--duration-min`; site chosen per suite; Ctrl+C keeps and analyses a long run; phase syntax `N=MODE`, `set …` |
| `test/host/test_ems_config_site.c`, `test_main_config_site.c` (new) | Executables `test_ems_config_site_2dev`, `_ct200`: CRC, firmware validation, activation in a sim build only, plan |
| `tools/wp4/tests/test_wp4_runner_d9.py` (new) | 34 tests of the D9 analyses; `tools/profile/tests` +1 |

## Suites

| Suite | Firmware | Site | Length | Tests |
|---|---|---|---|---|
| `two_dev` | `Wp4Sim2Dev` | lab_bench_2dev | ~3 min | DEV-003 (slave 2 unaffected, electrical every 5 s ± 3.2 s while slave 1 is offline), RTE-005 (no cross-talk), SCH-003 (no OVERLAP), DEV-002 |
| `ratio` | `Wp4Ratio` | lab_ratio_ct200 | ~1 min | CONV-004/005/006: l1 2.5 A → 500 A GOOD; l2 6.0 A → 1200 A OUT_OF_RANGE; 1 kW → 200 kW; 100.25 kWh → 20050 kWh (× CT, not × CT²); V, Hz unchanged |
| `load` | `Wp4Sim2Dev` | lab_bench_2dev | 60 min | PERF-001: measured load (transactions × controller-side transaction time, from the DIAG entry lines) within 0.5…1.5 × budget nominal (6.3 %); every request answered; no overlap; PERF-003/004/006 |
| `soak` | `Wp4Sim` | lab_bench | 60 min dry run, then 24 h | PERF-005: NORMAL → TIMEOUT → OFFLINE_THEN_RECOVER:300, 10 min each; one OFFLINE per TIMEOUT phase, one ONLINE ≤ 40 s after the simulator answers again, qualities right in every dump, no bus fault; PERF-002 heap change < 1 KB; PERF-003 all five tasks ≥ 1024 B; PERF-004 ≤ 4, no backpressure; PERF-006 ≤ 2 ms |

**PERF-001 measurement.** The budget assumes 50 ms device latency; the simulator answers faster. The runner therefore uses the time the controller actually spends per transaction (submit → result, per poll group), which is what occupies the bus from its point of view, and reports the per-group values. Expected ratio ≈ 0.6–0.7.

**PERF-004 measurement.** The COM master has no queue high-water API (COM unchanged). The firmware derives them every ≤ 100 ms: request queue + active = `accepted − completed`; completion queue = `completed − results consumed`.

## Run everything unattended (recommended)

```powershell
.\tools\d9\run_d9_all.ps1                      # all steps below, then 60 min dry run + 24 h soak
.\tools\d9\run_d9_all.ps1 -SkipSoak            # everything except the soak (about 2.5 h)
```

The script runs the steps of the next section one after the other (about 3.5 h before the soak), keeps the PC awake while it runs, carries on after a failed step and skips only the steps whose firmware failed to build or flash. The 24 h soak starts only if the 60 min dry run passed. Results: `tools\test_results\d9_all_<timestamp>\summary.txt` (one line per step with the evidence folder) and one log per step. Commit the D9 files first: REG-01 builds a fresh clone of the last commit.

## Run order (what the script does)

ESP-IDF v6.1 PowerShell, project root, ESP32 console on COM11, USB-RS485 simulator on COM12, **no live meter**.

```powershell
# PC tests
cmake -S test/host -B build_host -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build_host
ctest --test-dir build_host --output-on-failure                 # 10/10
python -m unittest discover -s tools/profile/tests              # 64 OK
python -m unittest discover -s tools/wp4/tests                  # 162 OK

# Two meters, then bus load (1 h)
.\tools\build_firmware.ps1 -Mode Wp4Sim2Dev -Port COM11 -Flash
python tools/wp4/myems_wp4_runner.py --suite two_dev --console-port COM11 --rs485-port COM12
python tools/wp4/myems_wp4_runner.py --suite load --console-port COM11 --rs485-port COM12

# CT 200
.\tools\build_firmware.ps1 -Mode Wp4Ratio -Port COM11 -Flash
python tools/wp4/myems_wp4_runner.py --suite ratio --console-port COM11 --rs485-port COM12

# Soak: 1 h dry run, then 24 h
.\tools\build_firmware.ps1 -Mode Wp4Sim -Port COM11 -Flash
python tools/wp4/myems_wp4_runner.py --suite soak --duration-min 60 --console-port COM11 --rs485-port COM12
python tools/wp4/myems_wp4_runner.py --suite soak --console-port COM11 --rs485-port COM12

# Regression on the same firmware (d6, d7) and COM (REG-01..05)
python tools/wp4/myems_wp4_runner.py --suite d6 --console-port COM11 --rs485-port COM12
python tools/wp4/myems_wp4_runner.py --suite d7 --console-port COM11 --rs485-port COM12
.\tools\d0\d0_verify_build.ps1                                                   # REG-01 fresh clone
.\tools\build_firmware.ps1 -Mode SelfTest -Port COM11 -Flash
python .\tools\myems_run_full_rtu_test_v5_1.py --console-port COM11 --rs485-port COM12   # REG-02
.\tools\build_wp24_all.ps1
python .\tools\myems_wp24_all_tests.py --esp-port COM11 --rs485-port COM12          # REG-03
git diff --stat com-09_057 HEAD -- partitions_myems_esp32s3_16mb.csv              # REG-05: no output
git diff com-09_057 HEAD -- sdkconfig.production | Select-String "PARTITION|FLASHSIZE|FLASHMODE"   # REG-05: no output
```

The soak keeps the PC busy for 24 h: disable sleep and Windows Update restarts for that time. If it has to stop, Ctrl+C ends it and the runner still writes the report (with the duration shortfall as a FAIL).

## Verified before delivery

- **Host tests:** 10 executables pass (gcc + AddressSanitizer/UBSan). Firmware sources compile against stub headers with ESP32 integer types for wp4_sim (all three sites), wp4_esc, production and self-test.
- **Python:** 64 profile-tool and 162 wp4 tests pass, including a scripted good run and deliberate faults for every D9 check.
- **Found and fixed while testing:** the runner's request grouping (5 s window, made for retries) merged 5 s polls, so the DEV-003 poll-interval check would have reported 10 s intervals; it now uses a 2 s window.
- **Not verified here:** ESP-IDF build of the new modes, real timings, the runs above.

## Exit criteria

| # | Criterion | Result |
|---|---|---|
| 1 | Host 10/10, Python 64 + 162 | |
| 2 | Builds: Production, SelfTest, Wp4Sim, Wp4Sim2Dev, Wp4Ratio, Wp4Esc | |
| 3 | `two_dev` PASS | |
| 4 | `ratio` PASS | |
| 5 | `load` (60 min) PASS — PERF-001, 003, 004, 006 | |
| 6 | `soak` 60 min dry run PASS | |
| 7 | `soak` 24 h PASS — PERF-002…006 | |
| 8 | `d6`, `d7` PASS on the D9 firmware | |
| 9 | REG-01, REG-02, REG-03, REG-05 PASS (REG-04 once COM-WP2.4 is closed; REG-06 not in 09_057) | |
| 10 | Acceptance record complete | |
