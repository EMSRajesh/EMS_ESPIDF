# MyEMS ESP32-S3 static-memory and Modbus RTU update

## Scope
This revision converts MyEMS-owned runtime objects in the RS-485 transport, Modbus RTU master,
poll scheduler, and acquisition service to static storage. It preserves the 16 MB partition CSV,
UART1 pins TX=17/RX=18/RTS=21, and the existing FC01/02/03/04/05/06/15/16/23 protocol implementation.

## Static storage strategy
- RS-485 transport context, TX queue, application event queue, stop semaphore, task TCB and stack:
  static storage; one transport instance (one physical bus).
- Modbus master context, request/completion queues, stop semaphore, task TCB and stack:
  static storage; one master instance per physical bus.
- Poll scheduler context, feedback queue, stop semaphore, task TCB and stack: static storage.
- Acquisition context, stop semaphore, task TCB and stack: static storage.
- Queues are bounded. Modbus request and completion queue limits are 16 each; scheduler feedback queue
  limit is 16. Attempts to configure larger queues are rejected.
- Stacks are fixed: transport 1024 StackType_t words (4096 bytes on ESP32-S3),
  master 1536 words (6144 bytes), scheduler 1024 words (4096 bytes), acquisition 1024 words
  (4096 bytes). Configuration `task_stack_size` is treated as a compatibility/upper-bound check;
  actual static stack depths are these compile-time constants.

## Modbus RTU services
- FC01 Read Coils
- FC02 Read Discrete Inputs
- FC03 Read Holding Registers
- FC04 Read Input Registers
- FC05 Write Single Coil
- FC06 Write Single Register
- FC15 Write Multiple Coils
- FC16 Write Multiple Registers
- FC23 Read/Write Multiple Registers
- Request validation, CRC16, response validation, exception response handling, bounded request and
  completion queues, response timeout, retry delay, and counters are retained in the existing master.
- Only one transaction is active on the shared half-duplex RS-485 bus at a time.

## Important allocation limitation
This change removes explicit `malloc`, `calloc`, `realloc`, and `free` usage from the MyEMS-owned
transport/master/scheduler/acquisition C files. It does **not** guarantee a globally heap-free ESP-IDF
system. The stock ESP-IDF UART driver (`uart_driver_install`) allocates its internal ring buffers,
event queue, and driver resources dynamically. ESP-IDF and logging/runtime components may also use
heap internally. These allocations must be measured/documented or replaced with a custom UART ISR
driver if a system-wide no-dynamic-allocation requirement is mandatory.

`CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y` enables static FreeRTOS object APIs; it does not select
FreeRTOS `heap_1` and does not disable ESP-IDF's multi-heap allocator. Do not disable ESP-IDF dynamic
allocation globally without auditing all required ESP-IDF components and drivers.

## Build and validation (run on Windows with ESP-IDF v6.1)
From the project root in an ESP-IDF PowerShell:
```powershell
idf.py reconfigure
idf.py build
idf.py size
idf.py flash monitor
```
Then test with the USB-RS485 simulator on COM12 at 9600 8E1. Self-test mode must be selected in
`sdkconfig` for the FC01/02/03/04/05/06/15/16/23 test to own the completion queue. Production polling
mode polls slave IDs 1 and 2; configure both IDs in the simulator, or test one configured meter at a
time. Never assume a meter's register map without its manual.

## Acceptance checklist
- [ ] `idf.py build` succeeds with ESP-IDF v6.1.
- [ ] Boot log reports SPI Flash Size: 16MB and the intended partition map.
- [ ] Static self-test passes all nine supported function codes and write readbacks.
- [ ] Production mode receives valid FC03/FC04 responses from each configured slave.
- [ ] Test timeout, bad CRC, Modbus exception, full request queue and full completion queue.
- [ ] Inspect stack high-water marks under load and verify no task stack overflow.
- [ ] Inspect ESP-IDF UART driver's internal allocation behavior and document its heap budget.
