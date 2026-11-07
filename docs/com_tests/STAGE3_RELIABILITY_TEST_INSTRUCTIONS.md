# MyEMS RS485 Modbus RTU — Stage 3 Reliability Validation Instructions

**Source baseline:** `myems_rs485_modbus_rtu_09_02` uploaded project  
**Target:** Waveshare ESP32-S3-POE-ETH-8DI-8DO  
**Toolchain:** ESP-IDF v6.1, Windows PowerShell  
**Serial ports:** ESP32 console `COM11` at 115200; USB-RS485 simulator `COM12` at 9600, 8E1

---

## 1. Purpose and scope

This stage validates four remaining reliability areas:

1. Deterministic baseline startup and first-response synchronization.
2. Same-slave, same-function late-response isolation.
3. Parser robustness and memory safety for malformed/boundary-length frames.
4. Long-duration operation and queue saturation/recovery.

The existing nine-function-code self-test and its request-validation checks remain the baseline. Do not change firmware behaviour merely to make a test pass. First reproduce and isolate a defect, then make a targeted change and rerun the relevant tests.

## 2. Safety rules — read before connecting

- **Disconnect all live meters and field devices** from the RS485 bus before running self-test or fault-injection tests.
- Connect the ESP32 RS485 interface only to the USB-RS485 simulator for these tests.
- The self-test issues Modbus write requests (FC05, FC06, FC15, FC16 and FC23). Never run it against a production meter or actuator.
- Confirm COM11 and COM12 in Windows Device Manager before running.
- Close serial terminals, IDE monitors and other programs that may have either COM port open.
- Stop the simulator before starting another simulator process.
- Do not flash production firmware for these tests. Use the `sdkconfig.selftest` configuration.
- Keep the original project ZIP as an untouched backup.

## 3. Clean project contents

This clean source package intentionally excludes generated or reproducible material:

- `build/`, `build_selftest/`, `build_production/` and their compiler/CMake/Ninja outputs.
- `test_results/` folders and historical run captures.
- Temporary diagnostic `.log`, `.csv`, `.json` output files under `tools/`.
- Python `__pycache__/` and `.pyc` files.
- `sdkconfig.old` and text search dumps.

It retains the firmware source, headers, component CMake files, partition CSV, build-mode configurations, build helper, test scripts, simulators and project documentation.

**Important:** deleting build directories does not delete firmware source or partition definitions. ESP-IDF recreates build output on the next build. Test reports should be copied elsewhere before cleaning a working project if they are needed for traceability.

## 4. Restore the development environment

1. Extract the clean ZIP to a short local path, for example:

   `C:\MyEMS\myems_rs485_modbus_rtu_09_02`

2. Open the ESP-IDF v6.1 PowerShell environment.
3. Change to the project root:

   ```powershell
   cd C:\MyEMS\myems_rs485_modbus_rtu_09_02
   ```

4. Confirm the toolchain:

   ```powershell
   $env:IDF_PATH
   python --version
   ```

5. Install the USB serial dependency if needed:

   ```powershell
   python -m pip install -r .\tools\requirements-usb-rs485.txt
   ```

6. Confirm the board and serial port assignments:
   - COM11 = ESP32-S3 USB/UART console and flashing port.
   - COM12 = USB-RS485 adapter connected to the board's RS485 bus.

The V4 runner locates `idf.py` through `IDF_PATH` and prefers the ESP-IDF Python environment when available.

## 5. Work package 1 — deterministic baseline

### Objective

Prove that each run begins from a known simulator state, that the console monitor captures the current boot, and that the first FC01 request receives the expected initial value.

### Current baseline symptom

In the supplied V4 report, the simulator's first FC01 response showed `0x55`, while the ESP32 self-test assertion reported `0xAB`. This is not yet explained. Do not assume the cause; establish event ordering with synchronized logs.

1. Open ESP-IDF PowerShell : Open your ESP-IDF v6.1 terminal and navigate to your project:
cd "C:\Users\malli\OneDrive\Desktop\EMS_MVP\WorkESP\myems_rs485_modbus_rtu_09_03"

2. Build Self-Test firmware
idf.py -B build_selftest `
  -D SDKCONFIG="$PWD\sdkconfig.selftest" `
  build

3. Flash firmware to ESP32
idf.py -B build_selftest `
  -D SDKCONFIG="$PWD\sdkconfig.selftest" `
  -p COM11 flash

 4. Monitor serial output
 idf.py -B build_selftest `
  -D SDKCONFIG="$PWD\sdkconfig.selftest" `
  -p COM11 monitor

5. Build Production firmware (when needed)
idf.py -B build_production `
  -D SDKCONFIG="$PWD\sdkconfig.production" `
  build
6. Run the baseline only

Ensure the bus has only the simulator connected. Then run:

```powershell
python .\tools\myems_run_full_rtu_test.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline
```

The runner builds and flashes self-test firmware unless `--skip-build` or `--skip-flash` is specified. It starts the simulator and writes a timestamped result folder under:

`tools\test_results\full_rtu_suite_v4_<timestamp>\`

### Collect and inspect

Retain these files from the run:

- `01_baseline\esp32_console.log`
- `01_baseline\simulator.log`
- `01_baseline\simulator_process_output.log`
- `report.txt`
- `summary.json`

Check the timeline in order:

1. Simulator process started and COM12 opened.
2. Simulator initial-state reset occurred exactly once before the first valid request.
3. ESP32 reset/boot banner appeared after console capture began.
4. `BUILD_MODE=SELF_TEST` appeared.
5. First Modbus request was FC01, slave 1, start address 0, quantity 8.
6. Simulator transmitted the initial coil byte `0x55`.
7. ESP32 accepted the matching response and reported `bits[0]=0x55`.

Do not accept baseline determinism until repeated clean runs pass consistently (recommended minimum: 10 consecutive runs).

## 6. Work package 2 — same-slave, same-function late response

### Why this needs a dedicated test

Modbus RTU has no transaction identifier. A response frame contains a slave address and function code, but not the master's request sequence number. Consequently, a delayed response for request A can be indistinguishable from a response to request B if both use the same slave and function code and the response data/length are otherwise plausible.

### Test design

Use two consecutive requests with the same:
- slave ID,
- function code,
- response byte count / requested quantity.

Use different requested register addresses and configure the simulator to return distinguishable data for A and B.

Test sequence:

1. Master sends request A.
2. Simulator delays response A beyond the master's response timeout.
3. Master times out A and enters its configured retry/recovery path.
4. Master sends request B with the same slave and function code.
5. Simulator sends the delayed response A while B is outstanding.
6. Simulator then sends the genuine response B.
7. Verify that response A is not delivered as B's successful completion.

### Pass criteria

- No stale response is reported as a valid completion for B.
- B completes only with its own expected response data, or fails safely with a clearly diagnosed transport/protocol result.
- No parser overrun, crash, watchdog reset or uncontrolled queue growth.
- Logs identify request A, request B, response arrival order and final completion status.

**Important limitation:** a quiet period and RX flush reduce the chance of stale-frame acceptance but cannot mathematically identify an old response when the wire frame is indistinguishable from the expected new response. If the test demonstrates this ambiguity, the recovery policy must be designed explicitly (for example, bus quarantine/quiet-time policy, no immediate same-signature retry, or marking the transaction/bus unsynchronized until a safe boundary). Do not claim perfect isolation from a test that uses different function codes or different response lengths.

This package's existing `delay-first` test is not sufficient to prove same-slave/same-function isolation.

## 7. Work package 3 — parser robustness and memory safety

### Frame classes

Exercise the RX parser with at least:

- zero bytes / empty event;
- one-byte and two-byte fragments;
- every truncated length of representative response frames;
- valid frame with bad CRC;
- valid CRC but wrong slave;
- valid CRC but wrong function;
- exception frame;
- incorrect byte count;
- extra trailing bytes;
- maximum legal response sizes for supported functions;
- one byte below, at, and above each configured buffer boundary;
- back-to-back frames and fragmented delivery;
- noise before a valid frame;
- repeated UART overflow / frame / parity errors.

For Modbus RTU, keep protocol maximums distinct from transport buffer capacities. A transport buffer being larger does not make an oversized protocol frame valid.

### Evidence and pass criteria

- Every input length is checked before indexing or copying.
- No read/write exceeds the destination buffer.
- No invalid frame produces a successful transaction completion.
- No crash, watchdog reset, stack overflow or heap corruption.
- RX loss and malformed-frame counters behave as specified.
- Valid traffic works again after malformed traffic stops.

Use host-side parser unit tests where feasible, then hardware-in-loop tests for UART fragmentation, overflow and timing. For memory-safety assurance, compile/run host tests with AddressSanitizer/UndefinedBehaviorSanitizer where the parser can be built independently. Hardware stability alone is not proof of memory safety.

## 8. Work package 4 — endurance and queue saturation

### Endurance sequence

Run production polling against the simulator (not live meters) at the intended polling rate. Begin with:

- 1 hour smoke endurance;
- 8 hour extended endurance;
- 24 hour qualification run.

Record at startup and periodically:

- uptime;
- free heap and minimum-ever free heap;
- task count and task stack high-water marks;
- request queue occupancy / high-water mark;
- completion queue occupancy / high-water mark;
- UART event queue pressure and application event drops;
- RX overflow, frame, parity and break errors;
- CRC, timeout, protocol and transport error counts;
- successful polls per slave and poll latency.

### Queue saturation tests

Test each queue separately where possible:

1. Fill the Modbus request queue faster than the master can service it.
2. Stop consuming completions until the completion queue reaches capacity.
3. Inject bursts of UART events / RX chunks.
4. Force repeated RX loss and transport error events.
5. Restore normal traffic and verify recovery.

### Pass criteria

- Queue-full behaviour is deterministic and documented (reject, drop, or back-pressure).
- No unbounded memory allocation or blocking in ISR context.
- Queue drops and overflow counters are visible to diagnostics.
- No deadlock, crash, watchdog reset or stack overflow.
- Memory usage remains bounded; no monotonic unexplained heap decline.
- After load is removed, normal polling recovers within a defined recovery interval.
- Stack high-water marks retain a documented safety margin.

The previous project history included a `rs485_transport` stack-overflow event in production polling. Specifically inspect that task's stack high-water mark and do not declare endurance PASS until the earlier failure mode has been addressed and retested.

## 9. Existing V4 suite commands

Run the complete nine-case self-test suite:

```powershell
python .\tools\myems_run_full_rtu_test.py `
  --console-port COM11 `
  --rs485-port COM12
```

Run selected cases:

```powershell
python .\tools\myems_run_full_rtu_test.py `
  --console-port COM11 `
  --rs485-port COM12 `
  --only baseline FI_06_delay_first FI_07_recovery
```

The accepted case labels are:

- `baseline`
- `FI_01_drop`
- `FI_02_bad_crc`
- `FI_03_exception`
- `FI_04_wrong_slave`
- `FI_05_short_response`
- `FI_06_delay`
- `FI_06_delay_first`
- `FI_07_recovery`

Do not use `--skip-flash` unless the board already has the intended self-test firmware. Do not use `--skip-build` unless the build output is known to correspond to the current source and self-test configuration.

## 10. Reporting template

For every test run, retain:

- source package/version identifier;
- firmware build mode and build result;
- board and toolchain details;
- COM port and UART settings;
- exact command and test parameters;
- raw ESP32 console log;
- raw simulator log;
- runner report and summary;
- PASS / FAIL / INCONCLUSIVE verdict;
- observed failure and reproducible steps.

A test is **INCONCLUSIVE** if required logs are missing, the simulator did not start cleanly, the monitor attached too late, or the expected stimulus was not confirmed.

## 11. Recommended execution order

1. Baseline determinism — 10 consecutive clean runs.
2. Same-slave/same-function late-response isolation.
3. Parser boundary and malformed-frame tests.
4. Queue saturation tests.
5. 1-hour endurance.
6. 8-hour endurance.
7. 24-hour endurance and final report.

Do not proceed to endurance qualification while baseline determinism or parser safety is unresolved.
