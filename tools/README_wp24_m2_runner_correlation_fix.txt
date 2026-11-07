WP24 M2 runner event-correlation fix
=====================================

File:
  myems_wp24_m_boundary_test_runner_m2_correlation_fix.py

Purpose:
  Fixes simulator event selection in the WP24-M1/M2 boundary runner. It scopes
  FAULT_MATCH and LATE_RESPONSE_SENT events to the latest FC01 address=0/count=8
  fault-sequence reset and associates the late response with the first
  FC03/start=100 request after that reset.

Evidence checked against the uploaded M2 artifact:
  - Latest reset: 2026-11-03T19:28:11.499703+00:00
  - First target FC03 request: 2026-11-03T19:28:12.222035+00:00
  - Selected delayed response: 2026-11-03T19:28:13.665457+00:00
  - Recovery start: 2026-11-03 19:28:12.578490+00:00
  - Recovery release: 2026-11-03 19:28:13.569872+00:00
  - Response is 1,086.967 ms after recovery start and 95.585 ms after release.

The patch changes only the Python test runner. It does not modify firmware,
sdkconfig, the simulator, or test timing. Do not rebuild/reflash for this
runner-only correction.

Install:
  1. Back up tools\myems_wp24_m_boundary_test.py.
  2. Copy this file into your project tools folder.
  3. Rename it to myems_wp24_m_boundary_test.py (replace the current runner).
  4. In ESP-IDF PowerShell, run:
       python -m py_compile .\tools\myems_wp24_m_boundary_test.py

Next:
  Run M2 WITHOUT --build or --flash, using the same COM ports and delay:
       python .\tools\myems_wp24_m_boundary_test.py `
         --mode M2 `
         --console-port COM11 `
         --rs485-port COM12 `
         --baud 9600 `
         --delay-ms 1450

Safety:
  USB RS485 simulator only. Never connect a live meter.
