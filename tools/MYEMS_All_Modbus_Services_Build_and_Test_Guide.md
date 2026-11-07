# MyEMS Modbus RTU services: build and test guide

## Implemented in this source update
ESP32 master request/response support is added for FC01, FC02, FC03, FC04, FC05, FC06, FC15, FC16 and FC23. The existing EMS polling scheduler is still a read-polling scheduler configured for FC03 by default; direct write/bit requests are submitted through `modbus_master_submit()` by an application component. No write command is automatically issued on boot.

Request structure:
- FC01/02: `start_register`, `register_count`; result uses packed LSB-first `bits` and `bit_count`.
- FC03/04: `start_register`, `register_count`; result uses `registers`.
- FC05: `start_register`, `register_count=1`, `write_coils[0]=0/1`.
- FC06: `start_register`, `register_count=1`, `write_registers[0]=value`.
- FC15: `start_register`, `register_count`, packed LSB-first `write_coils`.
- FC16: `start_register`, `register_count`, `write_registers[]`.
- FC23: `start_register` and `register_count` describe the write range and `write_registers[]`; `read_start_register` and `read_register_count` describe the read range. FC23 write quantity is limited to 121 registers to keep the complete request ADU within the 256-byte Modbus RTU limit.

## Windows build
Use ESP-IDF v6.1 PowerShell environment in the project root:
```powershell
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```
This source package has not been built in the user's ESP-IDF environment. Fix any compile diagnostics before flashing.

## Python simulator self-test and run
```powershell
py -m pip install pyserial
py usb_rs485_modbus_slave_sim_all_fc.py --self-test
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1
```
The simulator only responds to requests. The ESP32 must initiate requests. Do not run the earlier Python master tester on this bus at the same time.

## Fault injection
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault drop
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault bad-crc --fault-every 3
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault exception --exception-code 2
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault wrong-slave
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault short-response
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault delay --delay-ms 1500
```

Important limitations:
- This update implements the common function-code engine, not every optional/vendor-specific Modbus function or broadcast behavior.
- FC05/06/15/16/23 require a caller to submit a properly populated request. The demo scheduler remains FC03 polling until profiles/application commands are extended.
- The existing Python simulator self-test validates its own frame generation, not the ESP32 binary.
- This ZIP has not been compiled or hardware-verified in this execution. Build with ESP-IDF v6.1, test each FC explicitly, then test timeout/retry, bad CRC, wrong slave, short response, and delayed stale response handling.
- Before production, harden completion queue saturation and shared-stat synchronization; review task-stop lifetime and RS-485 RTS polarity on the actual board.

## Build log follow-up (v2 package)
This revision fixes the compiler diagnostics shown in the uploaded ESP-IDF v6.1 log:
- Removed always-true uint16_t comparisons in FC05/FC06 request validation.
- Split adjacent single-line `if` statements to satisfy `-Werror=misleading-indentation`.
- Split `modbus_master_start()` argument validation and `*out = NULL` assignment.
The revision still needs to be rebuilt in the user's ESP-IDF environment; no successful ESP-IDF build is claimed here.

## Stage 1 source patch

See `STAGE1_SOURCE_AUDIT_AND_CHANGES.md` for the FC23 frame-size correction, added self-test request-validation cases, validation performed, and hardware build instructions.
