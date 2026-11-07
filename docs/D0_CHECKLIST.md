# DP-WP4 milestone D0 — Baseline & housekeeping

Reference: DP-WP4 Development Plan v1.1, Sections 4.1 and 10. Exit criteria: **clean build of both modes from a fresh clone; REG-01 to REG-03 pass.**

## Deviation from the plan (recorded)

| Plan v1.1 item | D0 as executed | Reason |
|---|---|---|
| 4.1-4: move `sdkconfig.*` into `configs/` | **Kept in the project root** | `tools/build_wp24_*.ps1`, `build_firmware.ps1` and the WP24 runners load them by name from the root. Moving them now would break COM-WP2.4 work in progress. Move after COM-WP2.4 is closed, together with the script updates. |
| 4.1-1: optionally shorten the project path | Not done | Optional. Builds work at the current path; `core.longpaths` is enabled in the repository. |

## Steps

Run in **Windows PowerShell** from the project root
`D:\EMS_MVP\myEMS\myems_rs485_modbus_rtu_09\myems_rs485_modbus_rtu_09_057`.
Close VS Code's ESP-IDF monitor and any serial terminals first, so no build folder is locked.

| # | Step | Command | Expected |
|---|---|---|---|
| 1 | Back up the project folder (zip or copy to another drive) | – | Backup exists |
| 2 | Dry run | `powershell -ExecutionPolicy Bypass -File .\tools\d0\d0_housekeeping.ps1` | Lists git init, about 47 moves, 1 de-duplication, the VS Code fix, 13 build folders with sizes. **No changes made.** |
| 3 | Apply | `powershell -ExecutionPolicy Bypass -File .\tools\d0\d0_housekeeping.ps1 -Apply` | Two commits (`Baseline: COM 09_057 as received`, tag `com-09_057`; then `D0: housekeeping`). Build folders deleted; free space reported. |
| 4 | Check | `git log --oneline --decorate -n 3` and `git status` | Two commits, tag shown, working tree clean |
| 5 | **REG-01** — fresh-clone build, both modes (open the **ESP-IDF v6.1 PowerShell**) | `.\tools\d0\d0_verify_build.ps1` | `REG-01 RESULT: PASS`; report in `tools\test_results\d0_reg01_<time>\` |
| 6 | Commit sdkconfig drift (only if step 5 reports it) | Build once in the project (`.\tools\build_firmware.ps1 -Mode Production` and `-Mode SelfTest`), then `git diff sdkconfig.production sdkconfig.selftest`; if the changes are only ESP-IDF defaults: `git commit -am "D0: sdkconfig defaults written by ESP-IDF v6.1"` | Working tree clean after both builds |
| 7 | **REG-02** — COM self-test, all nine function codes (ESP32 on COM11, USB-RS485 simulator on COM12, **no live meter**) | `python .\tools\myems_run_full_rtu_test_v5_1.py --console-port COM11 --rs485-port COM12` | All cases PASS; results in `tools\test_results\` |
| 8 | **REG-03** — WP24 A–I campaign | `.\tools\build_wp24_all.ps1` then `python .\tools\myems_wp24_all_tests.py --esp-port COM11 --rs485-port COM12` | Summary PASS |
| 9 | Tag the milestone | `git tag -a dp-wp4-d0 -m "DP-WP4 D0 complete"` | Tag exists |

If step 3 stops with an error, nothing after that point has run; fix the cause and re-run `-Apply`. The script is safe to re-run: moved files are skipped and an existing `com-09_057` repository is reused.

To undo steps 2–5 of the script completely: `git reset --hard com-09_057` (build folders cannot be restored, but every build script recreates them).

## What moves where

| Destination | Files |
|---|---|
| `docs/` | `docs_architecture.md`, `STATIC_MEMORY_AND_MODBUS_SERVICES.md`, project report `.docx`, newer `concurrency_contract.md` (replaces the older copy in `docs/`) |
| `docs/com_tests/` | Self-test and WP24/WP25 test guides (11 files) |
| `archive/com_history/` | Change notes and patch manifests (17 files) and the older concurrency contract |
| `archive/patches/` | 7 `.patch` / `.diff` files |
| `archive/snapshots/` | `backup_before_wp24_m/`, `wp24_m_package/`, root copies of `simulator_source.py` and `myems_wp24_m_boundary_test.py` |
| `archive/test_logs/` | 3 `wp22_*.log` files from `tools/` |
| Removed | `concurrency_contract - Copy.md` (only if byte-identical to the current contract; otherwise archived) |
| Deleted (not in Git) | `build_wp24_*`, `build_wp25_*` folders |
| Unchanged | `README.md`, `BUILD_MODES.md`, all `sdkconfig.*`, `CMakeLists.txt`, partition table, `components/`, `main/`, `tools/` scripts |

## Exit record

| Item | Result | Evidence / note |
|---|---|---|
| Baseline commit and tag `com-09_057` | | commit hash: |
| D0 housekeeping commit | | commit hash: |
| Free disk space before / after | | |
| REG-01 Production build from clone | | report path: |
| REG-01 SelfTest build from clone | | report path: |
| REG-02 COM self-test | | results path: |
| REG-03 WP24 A–I | | results path: |
| Tag `dp-wp4-d0` | | |
| Date / by | | |
