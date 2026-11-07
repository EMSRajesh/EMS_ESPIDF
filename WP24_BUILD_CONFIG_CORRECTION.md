# WP24 unified build configuration correction

The previous `build_wp24_all.ps1` used `SDKCONFIG=... reconfigure`, which is
POSIX shell syntax and is not valid PowerShell environment assignment. The
commands also did not consistently pass the CMake cache variable selecting
`sdkconfig.wp24_all`.

The corrected script now uses `idf.py -B build_wp24_all -D
SDKCONFIG=sdkconfig.wp24_all ...` for reconfigure, build, and flash, and checks
the CMake cache before flashing.

The Python campaign runner now checks that `build_wp24_all/CMakeCache.txt`
selects `sdkconfig.wp24_all` and that the generated sdkconfig header enables:
- CONFIG_MYEMS_MODBUS_SELF_TEST
- CONFIG_MYEMS_WP24_FAULT_INJECTION
- CONFIG_MYEMS_WP24_EXTENDED_TEST=4

It also passes the dedicated SDKCONFIG selection when opening the monitor.
If the build directory is configured for the default config or a G/H/I-only
configuration, the runner exits before starting the simulator.

## Required sequence

1. Activate ESP-IDF v6.1 in PowerShell.
2. Run `.\\tools\\build_wp24_all.ps1` once to reconfigure, build, and flash.
3. Run `python .\\tools\\myems_wp24_all_tests.py ...` for the campaign.

This correction addresses the configuration mismatch visible in the
2026-11-01 execution log. It does not establish an ESP-IDF build or hardware
PASS; those still require execution on the user's development PC and ESP32-S3.
