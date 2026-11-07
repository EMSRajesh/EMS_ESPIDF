# USB-to-RS485 Modbus RTU test tool

This script acts as a Modbus RTU **master** and reads FC03/FC04 registers from a real Modbus **slave** (such as a meter) using a USB-to-RS485 adapter. It validates CRC, slave address, function code, byte count, and response length.

## Windows / PowerShell

1. Connect the USB-to-RS485 adapter. Find its COM port in Device Manager.
2. Install the dependency:
   ```powershell
   py -m pip install -r requirements-usb-rs485.txt
   ```
3. Run the offline protocol tests:
   ```powershell
   py usb_rs485_modbus_test.py --self-test
   ```
4. Example only — replace COM5, slave ID, serial settings, and register map with your meter's documented values:
   ```powershell
   py usb_rs485_modbus_test.py --port COM5 --slave 1 --fc 3 --start 0 --count 10 --baud 9600 --parity E --stopbits 1
   ```
5. For repeated reads and CSV logging:
   ```powershell
   py usb_rs485_modbus_test.py --port COM5 --slave 1 --fc 3 --start 0 --count 10 --interval 2 --iterations 10 --csv readings.csv
   ```
   Use `--iterations 0` for continuous polling until Ctrl+C.

## Wiring and safety

- The USB adapter connects to the **RS-485 A/B differential pair**, not directly to ESP32 UART pins.
- Verify A/B labeling in both manuals; naming conventions can be reversed across vendors.
- Confirm slave address, baud rate, parity, stop bits, and valid register addresses from the meter manual.
- Some manuals show register `40001` while the PDU address is `0`; conventions vary, so verify.
- Follow equipment isolation/grounding guidance. Add termination only at the physical bus ends when required.
- Do not run this PC script and the ESP32 Modbus master as simultaneous transmitters on the same bus. Use a real slave for a PC-master test, or use a suitable passive capture arrangement to observe ESP32-master traffic.
- The script prints raw unsigned 16-bit values. Scaling, signedness, word order, and units are device-specific.

## Common failures

- **Timeout:** check power, COM port, wiring, serial format, slave ID, and function/register range.
- **CRC error:** check serial format, wiring, electrical noise, grounding, and termination.
- **Exception 0x02:** the slave responded but the register address/range is invalid.
- **Port open error:** close terminal applications holding the COM port.
