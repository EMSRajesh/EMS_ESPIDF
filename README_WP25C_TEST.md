# MyEMS WP2.5-C dedicated completion backpressure test

## Purpose
Adds a dedicated `CONFIG_MYEMS_WP25_C_TEST` self-test mode. It validates:
1. Completion reservation acquired before request acceptance.
2. Reservation returned when request queue is full.
3. A failed queue admission does not increment completion-backpressure rejection.
4. Three accepted/unconsumed requests exhaust completion capacity.
5. A further submission is rejected specifically by completion backpressure.
6. Consuming one completion releases exactly one reservation.
7. Four accepted requests produce four unique successful terminal completions.
8. `completion_drops == 0`.

## Safety
Use the USB-RS485 simulator only. Do not connect this self-test build to live meters. The test issues FC03 reads only (no writes).

## Apply
From the project root, copy the files in this patch while preserving relative paths. Do not overwrite your existing `sdkconfig.wp24_all`; this patch adds `sdkconfig.wp25_c`.

## Build
```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"

idf.py -B build_wp25_c -D SDKCONFIG=sdkconfig.wp25_c reconfigure
idf.py -B build_wp25_c build
idf.py -B build_wp25_c -D SDKCONFIG=sdkconfig.wp25_c -p COM11 flash
```

## Simulator
In a second PowerShell terminal:
```powershell
python .\tools\myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py `
  --port COM12 --slaves 1 --baud 9600 --parity E --stopbits 1
```
Keep the simulator connected only to the USB-RS485 adapter and the adapter connected to the controller's RS485 bus.

## Monitor
```powershell
idf.py -B build_wp25_c -D SDKCONFIG=sdkconfig.wp25_c -p COM11 monitor
```

Expected markers:
- `WP25_C_REQUEST_QUEUE_ROLLBACK_PASS`
- `WP25_C_RESERVATION_ROLLBACK_PASS`
- `WP25_C_COMPLETION_SATURATION_PASS`
- `WP25_C_SUMMARY: PASS accepted=4 completed=4 drops=0 backpressure_rejections=1`
- `MODE=SELF_TEST result=PASS`

## Interpretation
A build success is not a test pass. The hardware test must show all markers above and the final summary. Afterward, run the existing WP24 A-I build and campaign separately to confirm the production recovery baseline remains unchanged.

## Implementation notes
The worker pause API exists only when `CONFIG_MYEMS_WP25_C_TEST` is enabled. It is a deterministic test hook to fill the request queue without relying on scheduler timing. It does not exist in production configuration. The test uses request queue capacity 2 and completion capacity 3.
