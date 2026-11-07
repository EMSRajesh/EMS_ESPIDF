# WP2.5-B2 Initial Lifecycle Test Patch

## Baseline
Prepared against the uploaded project archive `myems_rs485_modbus_rtu_09_055(6).zip`.
The patch adds a dedicated self-test selection; it does not modify the B2 master implementation.

## Files changed
- `main/Kconfig.projbuild`
- `main/modbus_self_test.c`

## Test coverage in this patch
This first deterministic hardware test covers:
- stop while one request is active and two are queued;
- one CANCELLED completion per accepted request;
- transition to `MODBUS_MASTER_DRAINING`;
- reading all completions after `modbus_master_stop()` returns;
- repeated `modbus_master_stop()` while DRAINING;
- rejection of a new submission after stop.

It combines B2-01, B2-02, B2-05 and B2-07 into one isolated run.

## Safety / setup
**Disconnect the USB-RS485 simulator and all live meters from the RS485 bus.**
This test deliberately submits an FC03 request with a 5-second timeout and expects
shutdown to cancel it. It does not require a responding slave.

## Configure
Create a separate configuration file so this test cannot overwrite the WP24 or WP25-D configurations:

```powershell
Copy-Item .\sdkconfig.wp25_d .\sdkconfig.wp25_b2
```

Edit `sdkconfig.wp25_b2`:
```ini
CONFIG_MYEMS_MODBUS_SELF_TEST=y
CONFIG_MYEMS_WP25_D_TEST=n
CONFIG_MYEMS_WP25_B2_TEST=y
```

Reconfigure and build:
```powershell
idf.py -B build_wp25_b2 -D SDKCONFIG=sdkconfig.wp25_b2 reconfigure
idf.py -B build_wp25_b2 -D SDKCONFIG=sdkconfig.wp25_b2 build
```

Flash and monitor:
```powershell
idf.py -B build_wp25_b2 -D SDKCONFIG=sdkconfig.wp25_b2 -p COM11 flash
idf.py -B build_wp25_b2 -D SDKCONFIG=sdkconfig.wp25_b2 -p COM11 monitor
```

## Expected result
```text
WP25_B2_SUMMARY: PASS accepted=3 cancelled=3 draining=1 second_stop=ESP_OK post_stop_submit=ESP_ERR_INVALID_STATE
MODE=SELF_TEST result=PASS
```
Exact formatting of `esp_err_to_name()` can vary by ESP-IDF version.

## Important limitations
- This patch is a test harness only. It does not change `modbus_rtu_master.c`.
- It has not been compiled or run on hardware in this environment.
- It does not yet test stop during retry delay, stop during recovery, concurrent
  submit/stop races, or the shutdown-acknowledgement timeout path.
- It verifies one active + two queued requests in this timing setup. The output
  reports the completion count and lifecycle state; it does not independently
  instrument the master task's internal active/queue ownership at the instant
  stop begins. The 100 ms delay is intended to let request 0 enter the active
  transaction before requests 1 and 2 are submitted.
- WP24-H and WP24-I scope limitations remain unchanged.
