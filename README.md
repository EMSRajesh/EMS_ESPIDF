# MyEMS RS485 / Modbus RTU

> **Corrected source package:** review the `PROJECT_REVIEW_AND_CHANGELOG.md` before building.
> Generated ESP-IDF `build/` output was intentionally removed; run `idf.py build` to regenerate it.

# MyEMS RS-485 + Modbus RTU — ESP-IDF v6.1

A commented, layered firmware foundation for the Waveshare ESP32-S3-POE-ETH-8DI-8DO board. This revision separates polling, Modbus transactions, result processing and UART transport into independent FreeRTOS tasks.

## Architecture

```text
app_main (initialization only)
  ├─ RS-485 transport task
  │    └─ ESP-IDF UART driver / managed ISR / RX ring buffer / UART event queue
  ├─ Modbus master task
  │    ├─ request queue
  │    ├─ one active transaction per physical bus
  │    ├─ RTU frame assembly + CRC + timeout/retry handling
  │    └─ completion queue
  ├─ EMS polling scheduler task
  │    └─ submits periodic requests for grid meter and load meter
  └─ EMS acquisition task
       ├─ sole consumer of the Modbus completion queue
       ├─ logs/processes completed results
       └─ sends request-completion feedback to scheduler
```

The two example meter profiles share the same physical RS-485 bus. Modbus RTU requests are serialized by the single master task for that bus. The grid and load meter polling deadlines are offset so they do not normally become due at the same time. If a meter has a request in flight, the scheduler skips a polling occurrence rather than accumulating duplicate requests.

## Included components

- `components/rs485_transport`: UART driver integration, managed ISR, RX ring buffer, UART event processing, copied RX chunks, TX queue, and transport/error events. It does not parse Modbus frames.
- `components/modbus_rtu_master`: asynchronous FC03/FC04 master, bounded request/completion queues, frame assembly, CRC validation, response matching, timeouts and retries.
- `main/ems_poll_scheduler.c`: dedicated periodic scheduler with per-meter deadlines, request IDs, in-flight suppression and completion feedback.
- `main/ems_acquisition.c`: dedicated completion consumer that logs results and provides the integration point for typed EMS measurements.
- `main/app_main.c`: initializes components and starts tasks; it does not contain a polling loop.

## Waveshare board mapping

The default transport configuration uses the published Waveshare RS-485 pin mapping:

- TX: GPIO17
- RX: GPIO18
- Direction / RTS: GPIO21
- UART peripheral: UART_NUM_1

The board documentation describes one onboard physical RS-485 interface. A second independent bus requires an external RS-485 transceiver and a separately verified UART/GPIO assignment.

The code currently configures `UART_MODE_RS485_HALF_DUPLEX`, using RTS for direction control. The official product page identifies GPIO21 as the direction/RTS signal, but public documentation may not establish the transceiver's exact part, active polarity, or every electrical detail. Verify the board revision and direction behavior before connecting live field equipment. Do not combine independent GPIO direction toggling with UART-managed RTS unless the implementation is intentionally redesigned and validated.

## Build and flash (Windows PowerShell)

Open an ESP-IDF v6.1 terminal, then run from this project directory:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the actual serial port. If a prior build exists for another target, `idf.py set-target esp32s3` will reconfigure the project.

## Configure actual meter profiles

Edit the `s_meter_profiles` array in `main/app_main.c`. The sample uses:

- Grid meter slave address: 1
- Load meter slave address: 2
- Function code: FC03
- Start register: 0
- Register count: 2
- Poll interval: 5 seconds per meter
- Timeout: 500 ms
- Maximum retries: 1

These values are demonstration settings only. Configure slave IDs, function codes, register addresses/counts, baud rate, parity and stop bits from each meter's manual. Slave addresses on the same multidrop bus must be unique. Register address zero does not universally mean voltage, and the returned 16-bit words may need device-specific scaling, word order, signedness and data-type decoding.

## Concurrency and ownership contracts

1. Only the RS-485 transport task owns UART event consumption and UART TX submission.
2. Only the Modbus master task owns active transaction state and RTU frame validation.
3. Only the EMS acquisition task consumes the Modbus completion queue.
4. Only the scheduler decides when to submit periodic meter requests; it does not wait for responses.
5. The acquisition task reports terminal request IDs back to the scheduler. The scheduler prevents duplicate in-flight requests for the same meter profile.
6. Queues are bounded. A queue-full condition is reported; no queue can guarantee delivery if its consumer is stopped indefinitely.
7. Transport event payloads are copied. No asynchronous event points at a temporary stack buffer.
8. RX chunks are not equivalent to Modbus frames. The master assembles the byte stream and validates frame length, address, function code and CRC.
9. On a UART overflow or stream-loss event, the partial candidate frame is invalidated.
10. Modbus RTU has no on-wire transaction ID. A late response after timeout is inherently ambiguous; the master uses validation and a quiet recovery interval to reduce misattribution risk.

## Notes and limitations

- This is an engineering foundation, not a certified production release. It has not been compiled on your local ESP-IDF installation or verified on the exact board/transceiver revision.
- The example supports FC03 and FC04 reads. Writes, broadcast operations, multiple physical bus instances, profile-file loading, full typed measurement conversion, OTA, and advanced bus recovery are outside this baseline.
- Queue lengths, task priorities and stack sizes are initial values. Measure stack high-water marks, queue occupancy, transaction latency and UART error counters under load.
- The completion queue is designed for one consumer. Do not add a second independent consumer without changing the API and routing model.
- During orderly shutdown, stop the scheduler first, then the acquisition task, then the Modbus master, and finally the RS-485 transport.

## Project layout

```text
myems_rs485_modbus_rtu/
├── CMakeLists.txt
├── sdkconfig.defaults
├── components/
│   ├── rs485_transport/
│   └── modbus_rtu_master/
├── main/
│   ├── app_main.c
│   ├── ems_poll_scheduler.c
│   ├── ems_acquisition.c
│   ├── include/
│   │   ├── ems_poll_scheduler.h
│   │   └── ems_acquisition.h
│   └── CMakeLists.txt
├── README.md
└── docs_architecture.md
```

## Concurrency contract and known remediation items

See [`docs/concurrency_contract.md`](docs/concurrency_contract.md) for the queue ownership map, request lifecycle, backpressure rules, synchronization requirements, shutdown sequence, and verification plan. The document explicitly identifies concurrency items that remain to be fixed in the source; it is not a claim that those fixes have already been implemented.
