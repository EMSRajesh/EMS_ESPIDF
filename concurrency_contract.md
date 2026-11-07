# MyEMS RS-485 / Modbus RTU Concurrency Contract

Target: ESP32-S3, ESP-IDF v6.1, FreeRTOS. This document specifies the intended contract for the scheduler, Modbus master, RS-485 transport, and acquisition task.

## 1. Component ownership

| Component | Owns | Must not do |
|---|---|---|
| Poll scheduler | Per-meter poll deadline, pending flag, request ID allocation | Parse UART data or decide whether a Modbus response is valid |
| Modbus master | Request lifecycle, active transaction, CRC/response validation, retry and timeout policy | Access UART hardware directly |
| RS-485 transport | UART driver, UART event queue consumption, TX/RX buffers, direction-control implementation | Interpret Modbus register meanings or schedule meters |
| EMS acquisition | Consume terminal results, decode profile values, maintain measurement quality, notify scheduler | Manipulate UART or change master transaction state |

The ESP-IDF UART driver owns its internal ISR and UART event queue. The transport task consumes UART events in task context.

## 2. Queue map

| Channel | Producer -> consumer | Payload | Contract |
|---|---|---|---|
| Modbus request queue | Scheduler -> master | `modbus_request_t` | Submission copies request; queue-full means rejected and not accepted |
| Transport TX queue | Master -> transport | copied bytes + TX ID | Acceptance means queued, not physically transmitted |
| UART driver event queue | ESP-IDF driver -> transport | UART event metadata | Driver-owned; transport is the sole consumer |
| Transport event queue | Transport -> master | RX chunk, TX completion, error/loss event | RX payload lifetime must be explicit; no stack pointers may escape |
| Modbus completion queue | Master -> acquisition | `modbus_result_t` | One terminal result per accepted request under normal operation |
| Scheduler feedback queue | Acquisition -> scheduler | terminal `request_id` | Only matching request ID clears that meter's pending slot |

Queue payloads should be fixed-size copies unless a documented ownership-transfer protocol is used. A queue notification does not replace the data payload.

## 3. Request lifecycle

1. Scheduler decides a meter is due and submits a request.
2. If submission fails, request was not accepted; scheduler keeps/retries the poll according to its backoff policy.
3. Once accepted, the master owns the request copy and serializes it with other requests on that physical bus.
4. Retries remain part of the same request and preserve the same `request_id`.
5. The master emits one terminal result: success, Modbus exception, timeout, transport error, protocol error, or cancellation.
6. Acquisition consumes the result, updates data quality and sends the matching request ID to scheduler feedback.
7. Scheduler clears the pending slot only when the feedback ID matches the slot's current pending request ID.

The master owns transaction deadlines. The scheduler must not infer that a transaction succeeded or failed merely because its own guard timer expired. A scheduler guard expiry is an internal fault/reconciliation condition, not a substitute for a terminal result.

## 4. Completion delivery and backpressure

The master is the sole producer of its completion queue. The acquisition task is the sole consumer.

The implementation uses a counting-semaphore reservation tied to the configured completion queue capacity:

1. `modbus_master_submit()` takes one completion-slot token before it enqueues the request.
2. If no token is available, submission returns `ESP_ERR_TIMEOUT`; the request is not accepted and `completion_backpressure_rejections` increments.
3. If the request queue is full after reservation, the token is returned and submission is rejected.
4. An accepted request retains its token while queued, active, retrying, and after its terminal result is published.
5. `modbus_master_get_completion()` returns the token only after it has copied a completion result to the caller.
6. Since outstanding accepted requests cannot exceed completion queue capacity, and there is one completion producer, a terminal result has a reserved queue slot.

This policy deliberately applies backpressure at request submission rather than allowing the master to dequeue work and later discover that no completion slot is available. It bounds the total number of accepted-but-not-consumed requests, including the active request.

`completion_drops` remains a diagnostic assertion counter. A nonzero value indicates an invariant violation, queue misuse, or an implementation defect; it is not an expected overload outcome. The master must not add a second completion producer without revisiting the reservation design.

The reservation token is held until the consumer removes the result. Consequently, a stalled or stopped consumer eventually causes new submissions to be rejected rather than causing terminal results to be lost. Shutdown cancellation/draining of all queued requests is handled separately by WP2.5-D and lifecycle behavior by WP2.5-B.

## 5. Timeout and retry policy

- Request timeout is measured by the master from the defined transaction phase (normally after TX completion and bus turnaround, for response timeout).
- TX completion has its own bounded deadline.
- Retries do not create new request IDs or new application completions.
- After retry exhaustion, emit one terminal result.
- Because Modbus RTU has no transaction ID on the wire, late responses must be handled with response validation and a quiet-time/recovery policy before a new request is accepted as active.
- RX loss/overflow invalidates the partial candidate frame; the master must not validate a frame assembled across a known byte-loss boundary.

## 6. Synchronization rules

- Only the transport task consumes the UART event queue and performs transport-level UART operations.
- Only the master task mutates active transaction state.
- Only the scheduler task mutates per-meter scheduling state; external tasks communicate through feedback queues.
- `volatile` alone is not synchronization. Stop state must use a FreeRTOS notification/event group, a critical section, or a suitable atomic primitive with defined memory ordering.
- Statistics read from another task require a consistent snapshot protected by a mutex/critical section or copied through an owner-task API.
- Never hold a mutex while waiting for UART data, a Modbus response, or a queue that can depend on another task needing that mutex.

## 7. Shutdown protocol

Use separate request-stop and wait/destroy phases, or an equivalent protocol. Do not free queues while another task can still access them.

Recommended order:

1. Scheduler enters STOPPING and stops creating new requests, but retains its feedback queue.
2. Master stops accepting new requests and resolves the active and queued accepted requests according to policy (finish or publish cancellation results).
3. Acquisition continues consuming all terminal results and reporting scheduler feedback until the master has stopped publishing.
4. Stop and join acquisition; only then destroy scheduler feedback resources and scheduler context.
5. Destroy master queues/context only after acquisition has stopped consuming the completion queue.
6. Close transport only after the master has stopped using transport APIs; then destroy transport queues and context.

If a stop timeout occurs, do not free the context anyway. Return the timeout and leave resources valid so the caller can retry or diagnose the failed shutdown.

## 8. Error and queue-full semantics

- Submission queue full: reject synchronously; request was not accepted.
- Transport TX queue full: return an explicit error to the master; it must produce/retry according to the transaction policy.
- UART RX overflow or transport RX event loss: emit a loss/error event if possible, invalidate the current partial frame, increment diagnostics.
- Completion queue full: should be prevented by reservation/backpressure. If it occurs despite the invariant, latch a fatal/internal-consistency fault and do not claim the result was delivered.
- Scheduler feedback queue full: treat as a scheduler coordination fault; do not silently clear or reuse a pending slot. Recovery must reconcile request IDs.

## 9. Code review status for the bundled implementation

The current source includes the queue topology and request IDs, but it is not yet fully compliant with every contract above:

- `publish_result()` currently uses a zero-wait completion send and can drop a terminal result.
- Several task stop flags are `volatile bool`; `volatile` alone is not synchronization.
- Scheduler/acquisition/master stop APIs combine requesting stop and destroying resources, so shutdown order must be corrected before runtime teardown is used.
- Statistics snapshots need synchronization if read concurrently with updates.

These are known remediation items, not claims that the current code already implements the full contract. The source should be built and stress-tested with ESP-IDF v6.1 after fixes. Do not use shutdown APIs concurrently with active traffic until the lifecycle fix is implemented and verified.

## 10. Minimum verification tests

1. Fill request queue and confirm submit rejects without claiming acceptance.
2. Delay acquisition while polling and confirm completion backpressure prevents result loss.
3. Disconnect each meter and confirm one final failure result per accepted request after retry exhaustion.
4. Inject malformed CRC and RX overflow; confirm partial frame invalidation.
5. Exercise request-ID matching with delayed and out-of-order scheduler feedback.
6. Request shutdown while idle, while waiting for response, during retry delay, and with queued requests.
7. Fill scheduler feedback queue and verify the fault is surfaced rather than silently releasing the wrong slot.
8. Run sustained polling on both independent buses when second-bus hardware is available.
