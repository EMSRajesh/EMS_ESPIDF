# MyEMS firmware build modes

MyEMS uses compile-time selection through the existing Kconfig option
`CONFIG_MYEMS_MODBUS_SELF_TEST`. The option selects exactly one application
path in `main/app_main.c`:

- `sdkconfig.selftest`: `CONFIG_MYEMS_MODBUS_SELF_TEST=y`; runs the isolated
  Modbus RTU self-test and does not start the EMS polling scheduler or
  acquisition task.
- `sdkconfig.production`: `# CONFIG_MYEMS_MODBUS_SELF_TEST is not set`; starts
  normal meter polling and acquisition.

The two modes use independent ESP-IDF build directories. Do not reuse one
build directory for both modes.

## Build self-test firmware

Open the ESP-IDF v6.1 PowerShell environment at the project root:

```powershell
.\tools\build_firmware.ps1 -Mode SelfTest
```

Build and flash to the ESP32 console port:

```powershell
.\tools\build_firmware.ps1 -Mode SelfTest -Port COM11 -Flash
```

Build, flash and open the serial monitor:

```powershell
.\tools\build_firmware.ps1 -Mode SelfTest -Port COM11 -Monitor
```

The self-test performs write operations. Connect the USB-RS485 simulator,
not a live field meter, before running it.

## Build production firmware

```powershell
.\tools\build_firmware.ps1 -Mode Production
```

Build and flash:

```powershell
.\tools\build_firmware.ps1 -Mode Production -Port COM11 -Flash
```

## Build DP-WP4 simulation firmware

`sdkconfig.wp4_sim` is `sdkconfig.production` plus
`CONFIG_MYEMS_CONFIG_TARGET_SIM=y`. It polls the devices of the generated
configuration (`main/generated/`, made with `--target sim`) and must only be
connected to the MyEMS AC meter simulator (`tools/wp4/myems_ac_meter_sim.py`).

```powershell
.\tools\build_firmware.ps1 -Mode Wp4Sim -Port COM11 -Monitor
```

## Build DP-WP4 simulation firmware for other sites (D9)

`sdkconfig.wp4_sim_2dev` and `sdkconfig.wp4_ratio` are `sdkconfig.wp4_sim` with
another `CONFIG_MYEMS_SIM_SITE_DIR` value (DP-WP5 M1; it replaced the D9
choice `MYEMS_SIM_SITE_*`): the firmware is built with the tables
in `main/generated_sites/lab_bench_2dev/` (two meters, slaves 1 and 2) or
`main/generated_sites/lab_ratio_ct200/` (CT 200). The HIL runner starts the
simulator with the matching site and checks that the configuration CRCs agree.

```powershell
.\tools\build_firmware.ps1 -Mode Wp4Sim2Dev -Port COM11 -Flash   # suites two_dev, load
.\tools\build_firmware.ps1 -Mode Wp4Ratio -Port COM11 -Flash     # suite ratio
```

## Build DP-WP5 DC meter firmware

DP-WP5 adds a simulated DC meter as data only (profile, map, site files and
generated tables). The build modes differ from `Wp4Sim` / `Wp4Esc` only in
`CONFIG_MYEMS_SIM_SITE_DIR`:

| Mode | sdkconfig | Site (tables) | Config CRC |
|---|---|---|---|
| `Wp5Dc` | `sdkconfig.wp5_dc` | `lab_dc_bench`: DC meter, slave 3 | 0xCC22DC64 |
| `Wp5Mixed` | `sdkconfig.wp5_mixed` | `lab_mixed`: AC meter slave 1 + DC meter slave 3 | 0xC04114E4 |
| `Wp5MixedEsc` | `sdkconfig.wp5_mixed_esc` | `lab_mixed`, plus the escalation test hook of `Wp4Esc` | 0xC04114E4 |

The boot banner shows the site ID and the configuration CRC; the simulator must
be started with the same site file.

```powershell
.\tools\build_firmware.ps1 -Mode Wp5Dc -Port COM11 -Flash
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_dc_bench.site.json --port COM12
```

## Build DP-WP4 escalation test firmware (D8)

`sdkconfig.wp4_esc` is `sdkconfig.wp4_sim` plus `CONFIG_MYEMS_WP4_ESC_TEST=y`
(which selects `CONFIG_MYEMS_WP24_FAULT_INJECTION`). At start-up it arms the
transport test hook that makes the next RS-485 recovery fail, so the first
failed transaction takes the bus to BUS_FAULT (Test Spec ESC-001..005). It is
used only for the HIL suite `esc`; flash `Wp4Sim` again for every other suite.
The boot log shows `BUILD_VARIANT=WP4_ESC_TEST`.

```powershell
.\tools\build_firmware.ps1 -Mode Wp4Esc -Port COM11 -Flash
python tools/wp4/myems_wp4_runner.py --suite esc --console-port COM11 --rs485-port COM12
```

A production build refuses a simulation configuration (and the other way
round): the boot log then shows `CONFIG REJECTED` and nothing is polled. Since
DP-WP4 D5 this is the expected result for `-Mode Production`, because only
simulation configurations exist so far.

## Direct ESP-IDF commands

The PowerShell helper is a convenience wrapper. Equivalent direct commands:

```powershell
idf.py -B build_selftest -D SDKCONFIG=sdkconfig.selftest build
idf.py -B build_production -D SDKCONFIG=sdkconfig.production build
idf.py -B build_wp4_sim -D SDKCONFIG=sdkconfig.wp4_sim build
```

Flash a previously built mode using its matching build directory:

```powershell
idf.py -B build_selftest -p COM11 flash
idf.py -B build_production -p COM11 flash
```

## Configuration safety

- Keep the two sdkconfig files under source control or backed up with the
  project; they are build inputs, not interchangeable runtime settings.
- Never flash an image from `build_selftest` onto a controller connected to
  live field meters. The self-test contains diagnostic write operations.
- `sdkconfig` at the project root is not used by the helper; the selected
  named config file is passed explicitly to CMake.
- Confirm the startup log reports `MODE=SELF_TEST` or the production startup
  message before connecting the controller to field equipment.

## Validation status

The configuration files and helper script are prepared. Actual ESP-IDF v6.1
builds and hardware flashing must still be run in the user's ESP-IDF Windows
environment; they are not claimed as verified here.
