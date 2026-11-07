# WP2.5-D — Pending request cancellation

## Scope
This patch adds `modbus_master_cancel_pending()`. Cancellation is executed by
the master task, which remains the sole owner of the active transaction and
request queue. It publishes exactly one `MODBUS_RESULT_CANCELLED` completion
for the active request (if any) and every request queued at the cancellation
service point.

The API does not stop the master. The application must consume the cancellation
completions and may then call `modbus_master_stop()`.

## Important concurrency boundary
Until WP2.5-B is complete, callers MUST stop submitting requests before calling
`modbus_master_cancel_pending()`. Concurrent submit/cancel/stop admission is
not yet made atomic. The API serializes concurrent cancellation calls and
waits for the master task acknowledgement with a caller-supplied timeout.

## Build configuration
Create `sdkconfig.wp25_d` from the existing WP25-C or WP24 self-test sdkconfig.
Enable:
- `CONFIG_MYEMS_MODBUS_SELF_TEST=y`
- `CONFIG_MYEMS_WP25_D_TEST=y`
Disable:
- `CONFIG_MYEMS_WP24_FAULT_INJECTION`
- `CONFIG_MYEMS_WP24_EXTENDED_TEST`

Use a separate build directory:
```powershell
idf.py -B build_wp25_d -D SDKCONFIG=sdkconfig.wp25_d reconfigure
idf.py -B build_wp25_d build
idf.py -B build_wp25_d -D SDKCONFIG=sdkconfig.wp25_d -p COM11 flash
idf.py -B build_wp25_d -D SDKCONFIG=sdkconfig.wp25_d -p COM11 monitor
```

## Hardware setup
Do NOT connect a live meter or USB-RS485 simulator for this test. The firmware
submits three FC03 requests and cancels them before a response is expected.
The RS485 transceiver can remain physically connected to the controller, but
the bus must be isolated from field devices.

## Expected output
```text
WP25_D_SUMMARY: PASS accepted=3 cancelled=3 active=1 queued=2 drops=0
MODE=SELF_TEST result=PASS
```

The test checks:
- three accepted requests with unique IDs;
- one active request and two queued requests cancelled by the master task;
- three distinct terminal completion records, all CANCELLED;
- accepted == completed == cancelled;
- active/queued cancellation counters are 1/2;
- completion_drops == 0.

## Validation status
Source patch only. Build and target execution must be performed by the project
owner. WP2.5-B lifecycle admission protection remains open.
