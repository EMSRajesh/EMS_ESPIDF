WP24-L Late Response Guard Patch
================================

Based on the uploaded components(4).zip (WP24-K source).

Test objective:
- FC03 A: slave 1, address 100, count 2. Simulator schedules first response for 1200 ms.
- A retry response is dropped.
- A should terminate TIMEOUT with retry_count=1 after recovery.
- B: FC03 address 200 must complete OK with [1200,1201].
- Delayed A response is sent asynchronously by the simulator while it continues processing requests.

Safety:
- Use ESP console COM11 and USB RS485 simulator COM12 only.
- Disconnect all real meters / field bus devices during fault injection.
- Do not treat a test failure as a firmware PASS.

Files:
- main/modbus_self_test.c: WP24-L selector branch (7)
- main/Kconfig.projbuild: selector range extended to 7
- sdkconfig.wp24_l: dedicated test config
- tools/build_wp24_l.ps1: build and flash
- tools/myems_wp24_l_late_response_test.py: runner
- tools/myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py: simulator with asynchronous delayed TX

Note:
This patch is generated from the source ZIP provided. Build/flash/runtime validation must be performed on your target and confirmed by logs. The test checks late response handling with a configured finite guard; it does not prove safety against arbitrarily late frames.


Runner/simulator assertion fix: this revision corrects L-specific summary matching, expects one delay then one drop (not two drops), queues delayed replies asynchronously, and records monitor host timestamps to check that the delayed response is emitted within the recovery guard.
