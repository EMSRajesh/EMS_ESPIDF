# MyEMS RS485 Modbus RTU — Corrected source package

This package contains the project source and configuration, without the stale `build/` directory.

## Corrections included

- `main/ems_poll_scheduler.c`
  - Creates the static feedback queue before starting the scheduler task.
  - Removes the duplicate scheduler task-creation path.
  - Uses consistent byte-based configuration and converts to FreeRTOS `StackType_t` words at task creation.
  - Validates queue length and stack capacity before creating the task.
- `main/ems_acquisition.c`
  - Uses byte-based stack-size configuration and converts it to FreeRTOS stack-depth words.
- `components/modbus_rtu_master/src/modbus_rtu_master.c`
  - Uses byte-based stack-size configuration and converts it to FreeRTOS stack-depth words.
- The existing RS-485 pin defaults remain TX GPIO17, RX GPIO18, RTS GPIO21, UART1, 9600 baud, 8E1.

## Build and flash on Windows (ESP-IDF v6.1)

Open the ESP-IDF PowerShell environment, then:

```powershell
Set-Location 'C:\Users\malli\OneDrive\Desktop\EMS_MVP\WorkESP\myems_rs485_modbus_rtu_08_static'
idf.py fullclean
idf.py build
idf.py -p COM11 flash
idf.py -p COM11 monitor
```

If extracting to a different directory, change `Set-Location` accordingly. `fullclean` removes generated build artifacts and is appropriate because this is a corrected source package.

## Important verification notes

- This package has not been compiled against the user's local ESP-IDF installation or hardware in this environment.
- After boot, confirm the log reports successful RS-485, Modbus master, scheduler, and acquisition startup.
- The sample meter profiles (slave IDs 1 and 2, FC03, start register 0, count 2) are placeholders. Configure them from each actual meter's register map before connecting production equipment.
- Defining OTA partitions does not by itself implement OTA update or rollback behavior.


## Runtime correction — RS-485 transport stack overflow

The runtime log showed `A stack overflow in task rs485_transport` immediately after the first Modbus request was scheduled. The transport task's static stack was increased from 1024 to 2048 `StackType_t` words (8192 bytes on ESP32-S3). The default byte-based config was updated to 8192 bytes, a bounds check was added, and a low-headroom watermark warning was added during bring-up. This is a targeted mitigation; verify with repeated polling and inspect watermark warnings under load.


## Transport stack overflow follow-up (v2)

The RS-485 transport task stack was increased to 16 KiB (4096 StackType_t words on ESP32-S3). Its TX request, UART event, RX chunk, and RX event scratch buffers are now held in the single static transport context rather than on the task stack. This reduces stack pressure but does not prove the original overflow root cause is eliminated; compile, flash, and validate with stack high-water logging enabled.
