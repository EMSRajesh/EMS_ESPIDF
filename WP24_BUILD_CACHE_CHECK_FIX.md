# WP24 build-cache validation fix

The 2026-11-01 16:30 execution log confirms that ESP-IDF selected
`sdkconfig.wp24_all` (CMake configure output and bootloader configure output),
and the full 1087-step firmware build completed. The script nevertheless
refused to flash because its post-build check assumed the CMake cache entry
would have the exact type `SDKCONFIG:FILEPATH`. CMake may store the entry with
a different cache type, so that check was too strict.

The PowerShell build script now accepts any SDKCONFIG cache type and validates
the selected filename. The Python runner applies the same flexible check.
The firmware configuration itself is still verified through generated
`sdkconfig.h` macros before the runner opens the monitor.

No firmware source or recovery logic was changed in this correction.
This is a build-script/runner validation fix only. The build and hardware
campaign must be rerun to establish results.
