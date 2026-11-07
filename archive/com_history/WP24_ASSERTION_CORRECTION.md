# WP2.4 firmware self-test assertion correction

## Change

`main/modbus_self_test.c` no longer hard-codes WP2-A to require
`MODBUS_RESULT_TIMEOUT`.

WP2-A now requires:
- successful submission and completion retrieval;
- the completion request ID to match request A;
- a supported non-OK fault result (`TIMEOUT`, `CRC_ERROR`,
  `PROTOCOL_ERROR`, `EXCEPTION`, or `TRANSPORT_ERROR`).

The host runner `tools/myems_wp24_recovery_test.py` remains responsible for
checking the exact expected status for each simulator fault:
- A/drop: timeout;
- B/delayed response: timeout;
- C/bad CRC: CRC error;
- D/short response: expected non-OK failure status;
- E/wrong slave: timeout;
- F/valid exception: exception, with no recovery.

The runner now also requires `SELF_TEST_SUMMARY: PASS`. This avoids reporting a
case as passed solely because the host-side parser saw the expected wire-level
status while the firmware's aggregate self-test reported an assertion failure.

## Deliberately not changed

- Modbus master recovery state machine and transport recovery implementation.
- Late-response guard (1000 ms) and bus-silence requirement (5 ms).
- Retry policy and write retry policy.
- UART driver and transport queues.
- Production configuration.

## WP2.4 extended coverage

Retry, genuine UART RX-loss/overflow, and recovery-timeout/fail-closed tests
remain not implemented by this patch. The current simulator cannot produce
those conditions reliably. They require dedicated, compile-time-gated test
support and must not be simulated by weakening production recovery behaviour.

## Build and test

From the ESP-IDF v6.1 PowerShell environment, in the project root:

```powershell
idf.py -B build_selftest -D SDKCONFIG=sdkconfig.selftest reconfigure
idf.py -B build_selftest build
```

Then connect the ESP32 RS485 port only to the USB-RS485 simulator (not a live
meter) and run:

```powershell
python .\tools\myems_wp24_recovery_test.py --build-dir build_selftest
```

The runner does not flash unless `--flash` is explicitly supplied. To flash
after reviewing the build output:

```powershell
python .\tools\myems_wp24_recovery_test.py --build-dir build_selftest --flash
```

Expected qualification remains `OVERALL WP2.4 STATUS: PARTIAL` until the
retry, RX-loss, and recovery-timeout tests are implemented and executed.
