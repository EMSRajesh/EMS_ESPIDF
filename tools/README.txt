MyEMS Modbus RTU full self-test runner — V4

Contents
- myems_run_full_rtu_test.py: V4 automated suite runner.
- myems_usb_rs485_modbus_slave_sim_all_fc_reset_delay_first.py: simulator with delay-first mode and reset-on-fc01 performed only once.

V4 changes from V3
1. FI-06 delay-first is evaluated as a recovery PASS when the first FC01 response is delayed beyond timeout, the retry response is immediate, and the complete self-test passes. V3 incorrectly expected all fault cases to end in a failed transaction.
2. --reset-on-fc01 now resets the simulator only on the first matching FC01 request, not every matching FC01 request. This prevents later FC01 reads/readbacks from resetting simulator state.
3. Simulator RX counter regex corrected to recognize whitespace before RX records.
4. Results are written under tools/test_results/full_rtu_suite_v4_<timestamp>/.

Run
1. Extract both Python files into your project's tools folder, replacing the previous V3 runner/simulator names if desired.
2. Ensure pyserial is installed in the Python environment used to run the simulator.
3. Open ESP-IDF v6.1 PowerShell environment and connect ONLY the USB-RS485 simulator to COM12. Keep live meters disconnected.
4. From the project root:
   python .\tools\myems_run_full_rtu_test.py --console-port COM11 --rs485-port COM12

Optional:
   python .\tools\myems_run_full_rtu_test.py --only baseline FI_06_delay_first FI_07_recovery
   python .\tools\myems_run_full_rtu_test.py --skip-build --skip-flash

Safety
Self-test firmware performs Modbus writes. Never connect it to live meters or operational equipment.
FI-06 delay-first confirms retry recovery for the tested sequence. It does NOT prove stale-response isolation for a late response with the same slave ID and same function code. That requires a dedicated test scenario.
