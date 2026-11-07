# MyEMS Modbus RTU — Stage 1 source audit and changes

Source baseline: uploaded `myems_rs485_modbus_rtu_09.zip`.

## Scope completed in this source patch

1. Corrected FC23 write quantity validation from the FC16 limit (123) to 121 registers.
   - FC23 carries both read and write ranges in one request ADU.
   - With 121 write registers, the request is 255 bytes including slave, function, address/count fields, byte count, payload and CRC.
   - 122 write registers would require 257 bytes and exceed the Modbus RTU 256-byte ADU limit and the 256-byte frame buffer.
2. Added `MODBUS_MAX_FC23_WRITE_REGISTERS` to the public Modbus header.
3. Extended the separate firmware self-test with local request-validation tests:
   - FC23 write quantity 122 rejected.
   - FC23 read quantity 126 rejected.
   - FC16 write quantity 124 rejected.
   - FC03 read quantity 126 rejected.
   These tests call `modbus_master_submit()` and expect `ESP_ERR_INVALID_ARG`; they should not put frames on the RS-485 bus.
4. Updated the self-test summary text to include request validation.

## Existing source behavior retained

- Separate self-test selected with `CONFIG_MYEMS_MODBUS_SELF_TEST`.
- Normal scheduler/acquisition are not started in self-test mode.
- FC01/02/03/04/05/06/15/16/23 request/response paths remain in the existing master.
- Write retry remains opt-in using `allow_write_retries`.
- Existing UART1/RS-485 board configuration and application architecture are unchanged.

## Validation performed in this environment

- `python -m py_compile tools/*.py`: PASS.
- `python tools/usb_rs485_modbus_slave_sim_all_fc.py --self-test`: PASS for simulator response generation and CRC for all nine function codes.
- Source checks confirm FC23 uses the new 121-register limit and the four validation cases are present.

## Not yet verified

- ESP-IDF v6.1 C compilation: NOT RUN in this environment (ESP-IDF toolchain is not available here).
- ESP32-S3 flashing and serial self-test: NOT RUN here.
- USB-RS485 end-to-end tests, fault injection, late-response isolation, queue saturation and long-duration stability: NOT RUN here.

A successful simulator self-test is not evidence that the ESP32 master has passed hardware testing. Build and run the separate self-test firmware on the board before accepting this patch as verified.

## Build instructions (Windows PowerShell, ESP-IDF v6.1)

From the project root, activate the ESP-IDF v6.1 environment, then:

```powershell
idf.py menuconfig
```

Enable:

`MyEMS Modbus RTU` → `Run Modbus RTU self-test instead of normal polling`

Then build and flash:

```powershell
idf.py build
idf.py -p COM11 flash monitor
```

Start the simulator in a separate PowerShell window:

```powershell
py -m pip install -r .\tools\requirements-usb-rs485.txt
py .\tools\usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1 --baud 9600 --parity E --stopbits 1
```

Do not connect a live meter for this diagnostic-write self-test. It writes only to the simulator's test register/coil map.
