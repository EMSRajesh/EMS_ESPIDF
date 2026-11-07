# MyEMS common Modbus RTU function-code test harness

## Supported simulated slave function codes
- FC01 Read Coils
- FC02 Read Discrete Inputs
- FC03 Read Holding Registers
- FC04 Read Input Registers
- FC05 Write Single Coil
- FC06 Write Single Register
- FC15 / 0x0F Write Multiple Coils
- FC16 / 0x10 Write Multiple Registers
- FC23 / 0x17 Read/Write Multiple Registers

## Install and self-test
```powershell
py -m pip install pyserial
py usb_rs485_modbus_slave_sim_all_fc.py --self-test
```

## Run against ESP32 master
Use the same serial settings as the ESP32 firmware. COM12 is the PC USB-RS485 adapter; COM11 may be the ESP-IDF USB serial console:
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1
```
The simulator supports the above function codes, but the existing MyEMS C master currently accepts FC03/FC04 only. The C API must be extended before the ESP32 can submit write/coil/FC23 requests; the simulator alone does not add those functions to firmware.

## Fault injection examples
Drop every response (timeout/retry test):
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault drop
```
Bad CRC on every third response:
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault bad-crc --fault-every 3
```
Delay each response 1500 ms:
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault delay --delay-ms 1500
```
Return exception code 2 (Illegal Data Address):
```powershell
py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --fault exception --exception-code 2
```
Other options: `wrong-slave`, `short-response`. Ctrl+C stops the simulator.

## Important
This is a simulated test device, not a certified Modbus implementation and not a real meter. Test writes only on a disconnected test setup. Do not use this script on a live control bus. Broadcast writes are not acknowledged by Modbus RTU; verify application behavior without expecting a response.
