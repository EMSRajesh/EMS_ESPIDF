# MyEMS RS-485 / Modbus RTU — Architecture and concurrency contracts

## Decision summary

- ESP-IDF managed UART ISR and driver-managed RX ring buffer/event queue.
- Raw RX chunks plus idle/error events are delivered by the transport; Modbus master assembles RTU frames.
- One transport instance and one Modbus master transaction engine per physical bus.
- A dedicated scheduler task submits periodic requests.
- A dedicated acquisition task is the sole consumer of terminal Modbus completions.
- Completion feedback is sent to the scheduler so it can clear the corresponding per-meter in-flight flag.
- Board direction-control implementation remains subject to verification of Waveshare hardware polarity and behavior.

## Task ownership

| Task | Owns | Must not do |
|---|---|---|
| RS-485 transport | UART event queue consumption, RX chunks, TX queue, direction-control sequence, transport diagnostics | Parse Modbus registers or schedule meter polls |
| Modbus master | Active request, transaction state, RX frame buffer, CRC/protocol validation, retry/timeout decisions | Directly manipulate UART registers or process EMS measurements |
| Poll scheduler | Per-meter deadlines, request IDs, pending-request slots, request submission | Wait synchronously for a meter response or consume Modbus completions |
| Acquisition | Completion queue consumption, result handling, measurement-decoding integration point | Access UART or modify master transaction state |
| app_main | Initialization and startup ordering | Perform periodic polling or response processing |

## Request lifecycle

1. Scheduler determines that a meter profile is due and has no request in flight.
2. Scheduler assigns a nonzero request ID and submits a copied request to the Modbus master.
3. The master serializes requests for the physical bus, transmits the RTU request and waits for a validated response or terminal failure.
4. The master places exactly one terminal result on its bounded completion queue for the request, with completion capacity reserved at request admission; shutdown cancellation/draining remains governed by the shutdown policy.
5. Acquisition consumes the result, logs it, and later will decode it into the EMS data model.
6. Acquisition sends the request ID to scheduler feedback. The scheduler releases the in-flight slot.
7. If a feedback event is lost, the scheduler has a watchdog deadline to recover a stuck slot. This is a defensive fallback, not a replacement for sizing and monitoring queues correctly.

## Scheduling behavior

The sample configures two profiles on the same physical bus. Each profile has its own period and initial offset. The scheduler skips missed polling periods instead of generating a catch-up burst. A profile with an in-flight request is not submitted again. A queue rejection causes the scheduler to retry after a short configurable delay.

The master request queue serializes transactions for the shared bus. Bus 2, if added later with its own transceiver/UART, should use an independent transport and master instance. The acquisition/data model may be shared at a higher layer.

## Buffer and queue rules

- All queues are bounded and their overflow/rejection behavior must be observable.
- TX bytes are copied into transport-owned storage before `rs485_submit_tx()` returns success.
- RX event data is copied into event-owned storage.
- Never queue pointers to task stack buffers for asynchronous use.
- UART event chunks are not Modbus frame boundaries.
- On RX loss, discard the current partial frame before continuing.
- Only one task consumes the Modbus completion queue.

## Timeout and late-response handling

Modbus RTU has no transaction identifier in the frame. After timeout, a delayed response can be ambiguous. The master validates slave address, function code, byte count, length and CRC, and applies a quiet recovery interval before retrying. These mitigations reduce but cannot mathematically eliminate ambiguity for all devices and bus conditions. Validate timing on the real meter and transceiver.

## Initial resource estimates

The transport and master defaults are initial engineering values, not proven production sizing. Measure task stack high-water marks, UART RX ring occupancy, event queue occupancy, request/completion queue occupancy, maximum frame timing, retries and error rates under representative bus load.

## Hardware assumptions to verify

- Waveshare ESP32-S3-POE-ETH-8DI-8DO onboard RS-485 mapping: TX GPIO17, RX GPIO18, direction/RTS GPIO21, UART_NUM_1.
- UART RTS half-duplex mode is enabled by default.
- Verify active polarity and turnaround behavior on the exact board revision before field use.
- Only one onboard physical RS-485 bus is assumed. A second independent bus needs external transceiver hardware and conflict-checked GPIO/UART resources.
