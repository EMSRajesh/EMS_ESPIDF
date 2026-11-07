# MyEMS Modbus RTU Reliability Runner V5

## Why V5 exists

V4 flashed the ESP32 before starting the simulator and monitor. Flashing resets the
board, so the self-test could run before the monitor was capturing output. Starting
the monitor could then trigger another reset. V5 changes the order:

1. Build firmware.
2. Flash firmware (this may cause an initial boot; it is not the measured test run).
3. Start the USB-RS485 simulator and wait for its serial port to open.
4. Start ESP-IDF monitor using `--no-reset`.
5. Wait for the runner prompt, then press the board's EN/RESET button exactly once.
6. Press Enter in PowerShell. The runner captures the fresh test boot until the
   `MODE=SELF_TEST result=...` line.

V5 also changes simulator `--reset-on-fc01` to reset each slave only once per
simulator process. The simulator adds `delay-first` fault support.

## Files

- `myems_run_full_rtu_test_v5.py` — test runner.
- `myems_usb_rs485_modbus_slave_sim_v5.py` — USB-RS485 Modbus slave simulator.

Copy both files into your project's `tools` directory. Keep the names unchanged.

## Prerequisites

- ESP-IDF v6.1 PowerShell environment.
- Python environment with `pyserial` installed.
- ESP32 console on COM11 (change with `--console-port` if needed).
- USB-RS485 adapter on COM12 (change with `--rs485-port` if needed).
- Disconnect all live meters. Self-test performs Modbus writes. Connect only the
  USB-RS485 simulator to the ESP32 RS485 interface.

## One baseline run

From the project root:

```powershell
python .\tools\myems_run_full_rtu_test_v5.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline
```

The runner builds and flashes by default. When this prompt appears:

```text
*** MONITOR IS ATTACHED. Press the ESP32 board EN/RESET button ONCE. ***
After pressing EN once, press ENTER here to begin/continue capture:
```

Press EN once, release it, then press Enter in PowerShell. Do not press EN again
while the test is running.

## Ten consecutive baseline runs

```powershell
python .\tools\myems_run_full_rtu_test_v5.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline `
  --repeat-baseline 10
```

Each iteration starts a fresh simulator process and monitor session. Press EN
exactly once at each prompt, then press Enter.

For faster reruns after a successful build/flash:

```powershell
python .\tools\myems_run_full_rtu_test_v5.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline `
  --repeat-baseline 10 `
  --skip-build `
  --skip-flash
```

Only use `--skip-build --skip-flash` if the intended self-test firmware is already
built and flashed.

## Full fault suite

```powershell
python .\tools\myems_run_full_rtu_test_v5.py `
  --console-port COM11 `
  --rs485-port COM12
```

Each test case requires one EN press after monitor attachment. For fault tests,
the runner checks the expected Modbus result status and four request validations.
`delay-first` success is not proof of same-slave/same-function stale-response
isolation; that needs a dedicated test.

## Output

Results are written under:

`tools/test_results/full_rtu_suite_v5_<timestamp>/`

Important files:
- `report.txt`
- `summary.json`
- `summary.csv`
- Per-test `esp32_console.log`
- Per-test `simulator.log`

## Safety and interpretation

- Never connect this self-test to a live meter or live control bus.
- The runner's syntax and simulator self-test are checked, but the complete V5
  runner has not yet been executed against the user's ESP32 hardware.
- If `idf.py monitor --no-reset` is not recognized by the installed IDF monitor,
  stop and share the exact error. Do not remove `--no-reset`; that would reintroduce
  the automatic-reset race.
- If monitor startup fails, inspect `runner_error.txt` and ensure COM11 is not
  open in another terminal or serial application.
- Baseline PASS requires all nine FC checks, all four validation checks, no crash,
  exactly one simulator reset marker, and first FC01 debug byte `0x55`.
