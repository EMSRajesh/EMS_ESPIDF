# MyEMS Modbus RTU Reliability Runner V5.1

## Purpose

V5.1 removes the need to press a physical EN/RESET button. It uses the ESP-IDF
monitor command interface to reset the ESP32 after the monitor has attached.

## Files

- `myems_run_full_rtu_test_v5_1.py` — V5.1 test runner.
- `myems_usb_rs485_modbus_slave_sim_v5.py` — V5 simulator with one-time state reset
  and `delay-first` fault support.

Copy both files into the project's `tools` directory. Keep the filenames unchanged.

## Prerequisites and safety

- ESP-IDF v6.1 PowerShell environment (`IDF_PATH` configured).
- Python environment with `pyserial` installed.
- ESP32 console connected on COM11 (or specify another port).
- USB-RS485 simulator connected on COM12 (or specify another port).
- Disconnect all live meters. The self-test firmware performs Modbus writes.
- Connect only the USB-RS485 simulator to the ESP32 RS485 interface.

## One baseline run

From the project root:

```powershell
python .\tools\myems_run_full_rtu_test_v5_1.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline
```

The runner will:

1. Build the self-test firmware unless `--skip-build` is supplied.
2. Flash the self-test firmware unless `--skip-flash` is supplied.
3. Start the simulator and wait for its serial port to open.
4. Start `idf.py monitor --no-reset`.
5. Wait for the monitor to initialize, then send the monitor's `reset` command
   through its stdin pipe. No physical EN/RESET button press is required.
6. Capture boot output and the self-test summary.

Expected runner message:

```text
[runner] Sent ESP-IDF monitor 'reset' command; capturing boot and self-test.
```

The test is accepted only if the full self-test passes, all four request
validations pass, the first FC01 diagnostic byte is `0x55`, and the simulator
records exactly one `RESET_ONCE slave=1` event.

## Ten consecutive baseline runs

After one successful build and flash:

```powershell
python .\tools\myems_run_full_rtu_test_v5_1.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline `
  --repeat-baseline 10 `
  --skip-build `
  --skip-flash
```

Each iteration starts a fresh simulator process and monitor session. The runner
sends one software reset command per iteration.

Only use `--skip-build --skip-flash` when the intended self-test firmware is
already built and flashed.

## Full fault-injection suite

```powershell
python .\tools\myems_run_full_rtu_test_v5_1.py `
  --console-port COM11 `
  --rs485-port COM12
```

## Optional timing controls

- `--monitor-start-delay 2.0` — seconds to wait after starting the monitor before
  sending the reset command.
- `--timeout 40` — seconds allowed for the self-test capture.
- `--delay-ms 1500` — injected response delay for delay tests.

## Results

Results are written under:

`tools\test_results\full_rtu_suite_v5_1_<timestamp>\`

Key files:

- `report.txt`
- `summary.json`
- `summary.csv`
- Per-test `esp32_console.log` and `simulator.log`

## Important limitations

- The runner uses the ESP-IDF monitor's `reset` command through stdin. This
  interface is expected from the ESP-IDF monitor's non-interactive command mode;
  verify the first hardware run from the saved console log.
- A successful `delay-first` case does not prove same-slave/same-function late
  response isolation. That requires a dedicated test where the stale and current
  response have the same slave ID and function code.
- Syntax checks and simulator self-test do not replace running the runner on the
  actual ESP32-S3 and USB-RS485 hardware.
