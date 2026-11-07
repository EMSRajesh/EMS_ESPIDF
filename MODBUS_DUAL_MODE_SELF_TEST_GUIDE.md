# MyEMS Modbus RTU dual-mode firmware

## Modes

The firmware now has two mutually exclusive modes, selected at build time. The menu option is defined in `main/Kconfig.projbuild`, so ESP-IDF discovers it as part of the `main` component.

- **Normal running mode** (`CONFIG_MYEMS_MODBUS_SELF_TEST=n`, default): starts the existing asynchronous Modbus master, scheduler, and acquisition task. It polls slave 1 and slave 2 using FC03 as before.
- **Self-test mode** (`CONFIG_MYEMS_MODBUS_SELF_TEST=y`): starts the same RS-485 transport and Modbus master, but does NOT start normal scheduler/acquisition tasks. A single self-test routine owns the completion queue and sequentially tests FC01, FC02, FC03, FC04, FC05, FC06, FC15, FC16, and FC23, including write/readback checks.

This is a compile-time choice, so both modes cannot accidentally consume the same completion queue simultaneously.

## Select the mode in ESP-IDF v6.1

From the project directory in the ESP-IDF PowerShell:

```powershell
idf.py menuconfig
```

Open **MyEMS Modbus RTU** and set **Run Modbus RTU self-test instead of normal polling**.

Build and flash:

```powershell
idf.py fullclean
idf.py build
idf.py -p COM11 flash
idf.py -p COM11 monitor
```

Close the monitor with `Ctrl+]` before running any host script that needs COM11. Re-open it after flashing if needed.

To return to normal mode, set the option to `n` in `menuconfig`, then run `idf.py build flash monitor` again. Default is `n`.

## Hardware/test setup

- ESP32-S3-POE-ETH-8DI-8DO RS-485 pins as configured in firmware: TX GPIO17, RX GPIO18, RTS GPIO21.
- USB-RS485 adapter: COM12.
- ESP console: COM11 at 115200.
- Test bus: 9600 baud, 8 data bits, even parity, 1 stop bit (8E1).
- Run `tools/usb_rs485_modbus_slave_sim_all_fc.py` on COM12 as slave ID 1. The self-test writes test data to slave 1 coils 1..8 and holding registers 5..6. Do not connect a production meter for this test.

Start simulator (in a separate PowerShell window):

```powershell
cd <project>\tools
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1 --baud 9600 --parity E --stopbits 1
```

Then open the ESP monitor on COM11. Expect `SELF_TEST_SUMMARY: PASS all nine function codes and write readbacks` and `MODE=SELF_TEST result=PASS`.

## Positive test matrix

| Service | Operation | Expected check |
|---|---|---|
| FC01 | Read coils 0..7 | packed byte `0x55` |
| FC02 | Read discrete inputs 0..7 | packed byte `0xAA` |
| FC03 | Read holding registers 0..1 | `1000, 1001` |
| FC04 | Read input registers 0..1 | `1100, 1101` |
| FC05 | Write coil 1 ON | success response, then FC01 confirms ON |
| FC06 | Write holding register 5 = `0x1234` | success response, then FC03 confirms |
| FC15 | Write coils 1..8 = pattern `0x55` | success response, then FC01 confirms `0x55` |
| FC16 | Write holding registers 5..6 = `0x1234, 0xABCD` | success response, then FC03 confirms |
| FC23 | Write holding registers 5..6 = `0x2222, 0x3333`, read 0..1 | response registers `1000, 1001`; separate FC03 verifies written values |

## Fault and recovery test conditions

Use the existing `myems_modbus_auto_test.py` in **normal mode** to exercise FC03 timeout/retry and recovery:
- `drop`: no response; expected timeout and retry.
- `bad-crc`: response CRC corrupted; expected CRC rejection and retry.
- `exception` with code 2: valid Modbus exception; expected exception status, not a successful read.
- `delay`: delayed response; expected timeout/retry according to delay and configured timeout, then recovery.
- `wrong-slave` and `short-response`: run the simulator with these fault modes manually and verify no false success, followed by a no-fault recovery stage.

The existing runner's automatic summary is FC03-focused. It does not yet automate fault injection for all nine FCs. For write functions, do fault injection only against the test simulator because retrying writes can have application-level implications even though these test writes are idempotent assignments.

## Important validation notes

- The source package has been edited, but this environment does not contain your Windows ESP-IDF v6.1 toolchain or physical COM ports. The firmware has **not** been built or run here.
- Treat the first build as an integration check. If the compiler reports errors, share the complete first error and surrounding log; do not flash until build succeeds.
- Self-test writes only to slave 1 of the simulator. It is not intended to be run against a real meter.