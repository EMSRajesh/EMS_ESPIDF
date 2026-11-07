# MyEMS ESP32-S3 memory policy (ESP-IDF v6.1)

## Partition layout

`partitions_myems_esp32s3_16mb.csv` is copied from the user-supplied CSV without changing offsets or sizes. It reserves `ota_0` and `ota_1` as 4 MiB app slots, plus `ems_cfg`, `ems_snap`, `ems_logs`, and `coredump`. The CSV ends at 0xC90000; the remaining 16 MiB flash is intentionally unassigned for future design. OTA/rollback is NOT implemented in this phase.

## Important current-code status

The source project currently uses dynamic allocation in MyEMS-owned components (`calloc`, `xQueueCreate`, `xTaskCreate`, and binary semaphore creation). Therefore this source package is **not yet compliant** with a strict static-allocation-only requirement. Do not treat the static-allocation requirement as implemented until those call sites are converted to static context storage, `xQueueCreateStatic`, `xTaskCreateStatic`, and static semaphore APIs, and an allocation audit confirms no application-owned `malloc/calloc/free` calls remain.

The ESP-IDF UART driver (`uart_driver_install`) also allocates driver-owned buffers, event queue and internal state. Retaining the standard UART driver means these internal allocations remain and must be accounted for. Eliminating every allocation, including driver internals, would require a custom UART/ISR driver, which is outside this selected Option A.

## FreeRTOS heap_1 clarification

ESP-IDF v6.1 does not offer a simple project setting that replaces its capability-based heap implementation with the vanilla FreeRTOS `heap_1.c` allocator. `CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y` enables static FreeRTOS object creation; it does not force all ESP-IDF components to stop allocating dynamically. Static application objects and explicit allocation audits are the appropriate approach for Option A.

## Flash size

The device boot log detected 16 MB flash while the current image header reported 2 MB. `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` is added to defaults to correct the image header on reconfiguration. Confirm the generated `sdkconfig` and boot log after rebuilding.

## Required validation before flash

1. Run `idf.py fullclean` only after closing serial monitors and build processes.
2. Run `idf.py reconfigure`; verify `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_myems_esp32s3_16mb.csv"`, and `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` in `sdkconfig`.
3. Run `idf.py partition-table` and inspect the generated partition table.
4. Run `idf.py build`. Do not flash if the partition table or build fails.
5. Check the boot log says 16 MB and displays the intended partition offsets.
