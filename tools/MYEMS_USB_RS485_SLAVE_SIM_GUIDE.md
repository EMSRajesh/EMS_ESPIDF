# MyEMS ESP32 Modbus RTU end-to-end test using USB-RS485

## Correct test arrangement

The ESP32-S3 firmware is the Modbus RTU **master**. Use this Python utility as a simulated Modbus RTU **slave**. The PC sends a response only after it receives a valid request from the ESP32.

1. Connect the ESP32 board's RS-485 A/B terminals and the USB-to-RS485 adapter's A/B terminals in parallel on the same two-wire bus.
2. Do not connect the USB adapter to ESP32 UART GPIO pins; connect it to the RS-485 differential terminals.
3. Verify A/B polarity against both device manuals. Vendor A/B naming may differ.
4. Keep any meter disconnected for this simulation, or ensure it cannot transmit on the same bus.
5. Run the Python slave simulator on the PC. Start the ESP32 firmware and observe both serial consoles.

## Windows setup

```powershell
py -m pip install pyserial
py usb_rs485_modbus_slave_sim.py --self-test
py usb_rs485_modbus_slave_sim.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1
```

Use the same baud/parity/stop-bit settings configured in the ESP32 firmware. COM12 is an example based on the user's current adapter port.

## Expected behavior

If the ESP32 sends FC03 for slave 1, start address 0, count 10, the PC console should show the received request and send a response. Register values are deterministic simulated raw values:
- FC03: `1000 * slave_id + register_address`
- FC04: `1000 * slave_id + 100 + register_address`

For example, slave 1 FC03 address 0 returns 1000 (`0x03E8`), address 1 returns 1001 (`0x03E9`).

The script supports FC03 and FC04 only. It is a protocol/transport test double, not a real meter model and not a substitute for testing the real meter's register map/scaling.

## If no request appears

- Confirm ESP32 is transmitting on the board's RS-485 port, not only logging requests to USB serial.
- Confirm board wiring uses the onboard RS-485 interface and the adapter is on its A/B bus.
- Confirm the ESP32's configured slave IDs and serial settings match the simulator.
- Check the board's RS-485 direction-enable behavior and adapter auto-direction support.
- Verify A/B polarity and common reference/grounding requirements.
- Ensure no other Modbus master is on the bus.
