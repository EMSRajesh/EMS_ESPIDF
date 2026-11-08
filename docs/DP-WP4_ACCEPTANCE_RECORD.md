# DP-WP4 — Acceptance record

DP-WP4 Verification & Test Specification v1.1, Section 23 (acceptance gate) and Section 24 (execution record). Fill in on the commit that is to be tagged `dp-wp4-d9`; every result must come from that commit.

**Statement (gate item 7):** all DP-WP4 tests use the synthetic simulation profile `AC_METER_3PH_SIM`, synthetic register maps and the USB-RS485 simulator. **No real meter and no vendor register map has been validated.**

## Run identification (Section 24)

| Field | Value |
|---|---|
| Firmware commit | |
| ESP-IDF version | v6.1 |
| Target board / revision | Waveshare ESP32-S3-POE-ETH-8DI-8DO, ESP32-S3 (QFN56) rev v0.2 |
| Config CRC — lab_bench / lab_bench_2dev / lab_ratio_ct200 | 0x4B8B5924 / 0xE3FF2DF7 / 0x5A2CC286 |
| Inputs | profile AC_METER_3PH_SIM 1.1.0, map MYEMS_SIM_ACMETER_3PH 1.1.0, UMD 1.0.0 |
| Simulator | `tools/wp4/myems_ac_meter_sim.py` 1.0.0, seed 1 |
| COM-WP2.4 status | open (WP24-M1/M2): hardware results below are recorded but become acceptance evidence only after it is closed (Plan Section 2) |

## Gate (Section 23)

| # | Condition | Evidence | Status |
|---|---|---|---|
| 1 | All L1 and L2 tests pass; D2 coverage targets met | `ctest` 10/10; profile tool 64; wp4 162; D2 coverage report | |
| 2 | Mandatory L3–L5 pass with COM-WP2.4 closed; ESC passed or BLOCKED against an accepted COM CR | Table below; ESC-004/005 BLOCKED against CR-D8-01 | |
| 3 | PERF-001…PERF-006 pass, including the 24 h soak | `load`, `soak` 24 h reports | |
| 4 | No open critical or high-severity defects | Defect log below | |
| 5 | Simulation-only / test-only labels in profiles, maps, generated files, boot banners | `classification SIMULATION_ONLY` in `main/generated*/`, `TEST_ONLY` decoder fixture; boot banner `CONFIG … classification=SIMULATION_ONLY`; production build rejects them (CFG-018) | |
| 6 | Evidence archived with firmware commit and config CRC | `tools/test_results/wp4_*` folders (ignored by Git: archive them with this record) | |
| 7 | Statement that no real meter or vendor map is validated | Top of this record | done |

## Results by test group

| Group | Level | Where | Evidence | Result |
|---|---|---|---|---|
| UMD, Q (host) | L1 | `test_ems_measurement`, `test_ems_acq_core` | ctest | |
| DEC, CONV, ENG | L1 | `test_ems_register_map`, `test_ems_config_*` | ctest | |
| GEN, CFG-001…016 | L2 | `tools/profile/tests` | unittest | |
| SIM-001…012 | L3 | `tools/wp4/tests/test_ac_meter_sim.py` | unittest | |
| SCH-001…010 | L1 + L4 | `test_ems_sched_core`; HIL d5/d7 | | |
| RTE, Q, UMD-006/007, ENG-008 | L4 | HIL `d6` | `wp4_d6_20261108_145432` (D8 firmware) | PASS |
| DEV, DIAG, CFG-019 | L4 | HIL `d7` | `wp4_d7_20261108_144958` (D8 firmware) | PASS |
| CFG-018 | L4 | HIL `reject` (Production) | `wp4_reject_20261108_135330` | PASS |
| ESC-001…003 | L4 | HIL `esc` (Wp4Esc) | `wp4_esc_20261108_151633` | PASS |
| ESC-004, ESC-005 | L4 | — | CR-D8-01 (`docs/D8_ESCALATION_REVIEW.md`) | BLOCKED |
| DEV-003, RTE-005, SCH-003 | L4 | HIL `two_dev` (Wp4Sim2Dev) | | |
| CONV-004…006 on the board | L4 | HIL `ratio` (Wp4Ratio) | | |
| PERF-001 | L6 | HIL `load` 60 min | | |
| PERF-002…006 | L6 | HIL `soak` 24 h | | |
| REG-01 | L5 | `tools/d0/d0_verify_build.ps1` | | |
| REG-02 | L5 | `myems_run_full_rtu_test_v5_1.py` | | |
| REG-03 | L5 | `myems_wp24_all_tests.py` | | |
| REG-04 | L5 | WP24-L, M1/M2 runners | — | pending COM-WP2.4 |
| REG-05 | L5 | `git diff com-09_057` partition file / flash settings | | |
| REG-06 | L5 | — | not in 09_057 (Plan Section 2) | n/a |

The d6/d7/reject/esc results above are from the D8 firmware; re-run d6 and d7 on the D9 commit (D9 checklist) and replace them.

## Defect log

| ID | Severity | Found | Description | Fix commit | Retest |
|---|---|---|---|---|---|
| CR-D8-01 | (COM change request) | D8 | Master: no bus health API, no recovery retry, transport FAILED terminal, requests held after fail-closed; transport: bus silence measured per 120-byte FIFO event | COM track | ESC-004/005 |
| | | | | | |

## Sign-off

| Role | Name | Date |
|---|---|---|
| Developer | | |
| Reviewer | | |
