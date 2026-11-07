# MyEMS Modbus RTU — All Nine Function Codes Test Plan

Target: Waveshare ESP32-S3-POE-ETH-8DI-8DO, ESP-IDF v6.1, RS-485 9600 8E1, ESP console COM11, USB-RS485 simulator COM12.

## Important test boundary

The Python simulator's response-generation self-test passes for FC01/02/03/04/05/06/15/16/23. This is **not yet an ESP32 end-to-end pass**. The current application scheduler submits FC03 polling requests; it will not automatically exercise all nine functions. Add/use a dedicated firmware self-test mode that submits each request through `modbus_master_submit()` and reads each result from `modbus_master_get_completion()`. Do not run the normal acquisition task as a second consumer of that completion queue during this test.

## 1. Test environment

- COM11: ESP32 serial log, 115200 8N1.
- COM12: USB-RS485 adapter connected to the board's RS-485 A/B pair.
- Bus settings: 9600 baud, even parity, 1 stop bit.
- Simulator: `py usb_rs485_modbus_slave_sim_all_fc.py --port COM12 --slaves 1,2 --baud 9600 --parity E --stopbits 1`
- Close ESP-IDF Monitor before any runner that opens COM11 directly.
- Test writes only against this isolated simulator; never run write tests against a live control bus.

## 2. Deterministic simulator data

For slave 1: holding registers `1000 + address`; input registers `1100 + address`; coil at address 0 is ON; discrete input at address 0 is OFF.

For slave 2: holding registers `2000 + address`; input registers `2100 + address`; coil at address 0 is OFF; discrete input at address 0 is ON.

Bits are packed LSB-first in the result structure. Modbus protocol addresses in these tests are zero-based PDU addresses.

## 3. Positive tests — run each on slave 1 unless stated otherwise

Use `timeout_ms=500`, `max_retries=1` for baseline tests. Ensure only one request is outstanding at a time, wait for its matching `request_id`, and assert `status == MODBUS_RESULT_OK`.

| ID | FC | Request parameters | Expected result / assertion |
|---|---|---|---|
| P01 | FC01 Read Coils | start=0, count=8 | `bit_count=8`; first bits for slave 1 are 1,0,1,0,1,0,1,0. |
| P02 | FC02 Read Discrete Inputs | start=0, count=8 | `bit_count=8`; first bits for slave 1 are 0,1,0,1,0,1,0,1. |
| P03 | FC03 Read Holding Registers | start=0, count=2 | `register_count=2`; values 1000, 1001. |
| P04 | FC04 Read Input Registers | start=0, count=2 | `register_count=2`; values 1100, 1101. |
| P05 | FC05 Write Single Coil | start=10, count=1, `write_coils[0]=1` | Echo response succeeds; then FC01 start=10,count=1 returns ON. Repeat with value 0 and assert OFF. |
| P06 | FC06 Write Single Register | start=10, count=1, `write_registers[0]=0x1234` | Echo response succeeds; then FC03 start=10,count=1 returns `0x1234`. |
| P07 | FC15 Write Multiple Coils | start=20, count=4, packed `write_coils[0]=0x0D` | Echo response succeeds; then FC01 start=20,count=4 returns 1,0,1,1. |
| P08 | FC16 Write Multiple Registers | start=30, count=3, values `0x1111,0x2222,0x3333` | Echo response succeeds; then FC03 start=30,count=3 returns those exact values. |
| P09 | FC23 Read/Write Multiple Registers | write start=40,count=2, values `0xA55A,0x5AA5`; read start=40,count=2 | Successful response with `register_count=2` and values `0xA55A,0x5AA5`. |
| P10 | Multi-slave isolation | FC03 slave 2 start=0,count=2; FC04 slave 2 start=0,count=2 | Holding values 2000,2001; input values 2100,2101. |

For FC05, the request structure's `write_coils[0]` is a boolean value 0 or 1. For FC15 it is a packed bit field, with bit 0 corresponding to the first coil.

## 4. Request validation tests (should be rejected locally)

For each invalid request, assert `modbus_master_submit()` returns an error and no request is transmitted:

- Unknown function code, e.g. FC07.
- Slave address 0 or 248.
- Read count zero for FC01/02/03/04.
- FC03/04 count above 125.
- FC01/02 count above 2000.
- FC15 count zero or above 1968.
- FC16 count zero or above 123.
- FC23 read count zero or above 125; write count zero or above 123.
- Address-range overflow, e.g. start=65535,count=2.
- FC05 with `write_coils[0]` not 0 or 1.

Note: review the current implementation's exact validation for each field before treating all cases as passed; in particular, the current FC05 validation checks only the first coil byte.

## 5. Fault-injection tests

Run each fault as a separate short stage with the simulator restarted between stages. Submit one or more requests of the listed FC, then restart the simulator with `--fault none` and verify a successful request afterward.

| ID | Fault condition | Simulator command option | Expected ESP32 behavior |
|---|---|---|---|
| F01 | No response / timeout | `--fault drop` | Timeout result after configured retry budget; no false success. |
| F02 | Bad response CRC | `--fault bad-crc` | CRC error and retry; corrupted payload must not be accepted. |
| F03 | Modbus exception, Illegal Data Address | `--fault exception --exception-code 2` | `MODBUS_RESULT_EXCEPTION`, exception code `0x02`; no successful data result. |
| F04 | Wrong slave ID | `--fault wrong-slave` | Response must not be accepted for the active request; bounded timeout/protocol failure. |
| F05 | Truncated/short response | `--fault short-response` | Protocol/timeout failure; no success. |
| F06 | Delayed response below timeout | `--fault delay --delay-ms 150` | Expected success if timing and RTU inter-frame constraints allow. |
| F07 | Delayed response above timeout | `--fault delay --delay-ms 1500` | Timeout/retry; any late response must not be misattributed to the next request. |
| F08 | Recovery after each fault | `--fault none` | Next normal request succeeds with correct values. |
| F09 | Intermittent corruption | `--fault bad-crc --fault-every 3` | Good responses succeed; corrupted responses are rejected and retried/failed as configured. |

Repeat F01–F09 against at least FC03 and one variable-length response function (FC01 or FC23). For write functions, verify the data after a successful write by issuing a read request. Exception/fault stages should be run only against the simulator.

## 6. Timing, queue, and robustness tests

- **Single request:** each positive test passes 10/10 times.
- **Repeated suite:** run P01–P10 100 cycles; no hangs, stale completions, wrong request IDs, or corrupted data.
- **Bus serialization:** submit several requests quickly; simulator must observe complete non-overlapping RTU frames and each completion must match its request ID.
- **Completion queue:** deliberately delay the completion consumer; verify queue-full behavior is counted/logged and results are not silently treated as success.
- **Retry count:** for each injected timeout/CRC fault, observed retries must not exceed configured `max_retries`.
- **Recovery:** after each fault stage, at least 5 consecutive normal requests must pass.
- **Soak:** five-minute baseline with no injected faults; target zero failures after startup. Record any intermittent failure rather than masking it.

## 7. Acceptance criteria

A function code passes only when all of these are true:

1. Correct request bytes and CRC are observed by the simulator.
2. Simulator sends a valid response.
3. ESP32 returns the expected result status, FC, slave ID, request ID, count, and data.
4. Writes are verified with a subsequent read-back.
5. Invalid frames and injected faults are rejected, retries stay bounded, and normal communication recovers.
6. The suite completes without completion-queue loss, stale-response cross-matching, task watchdog reset, or memory errors.

## 8. Current status

- Simulator self-test: **PASS** for response generation/CRC of all nine function codes.
- ESP32 end-to-end FC03 fault-injection run: **provisional pass**, based on the previously supplied logs.
- ESP32 end-to-end FC01/02/04/05/06/15/16/23: **NOT RUN** until firmware self-test mode submits those requests.
- Production readiness: **not established** by simulator tests alone.
