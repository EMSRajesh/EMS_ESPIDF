# DP-WP4 milestone D4 — AC meter simulator

Plan v1.1 milestone D4. Tag after acceptance: `dp-wp4-d4`.

## Delivered

| Path | Content |
|---|---|
| `tools/wp4/myems_ac_meter_sim.py` | Modbus RTU slave for DP-WP4: layout from the site file via the profile tool, 12 modes, runtime control file, request/response log |
| `tools/wp4/tests/test_ac_meter_sim.py` | 44 tests: SIM-001…SIM-012, request checks, value overrides, serial loop with an in-memory port and simulated clock, CLI |
| `tools/wp4/README.md` | Usage |

## Decision: new tool, COM simulators unchanged

The Plan says "simulator extended in D4". The COM regression runners (REG-02, REG-03) start `myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py` and rely on its writable register space and fault options. Changing that file would put COM regression at risk, so the AC meter simulator is a separate script. It uses the same RTU framing and partial-frame rule (50 ms gap) as the COM simulators.

## Exit criteria

| # | Criterion | How checked | Result |
|---|---|---|---|
| 1 | SIM-001…SIM-012 pass | `unittest` (44 tests) | |
| 2 | Register image equals Test Spec 4.1 and 4.2 word for word | `--dump` and SIM-001 | |
| 3 | Simulator layout = firmware configuration | CRC on the START line = `EMS_CONFIG_GEN_CRC32` (0x4B8B5924 for `lab_bench`) | |
| 4 | Earlier tests still pass | profile tool and host C tests | |
| 5 | (optional) Opens COM12 | `--port COM12`, then Ctrl+C | |

Verified before delivery: 15 deliberate defects in the simulator, all caught by the tests (word orders, export sign, out-of-range value, energy rate, seed use, read-only check, bad length, slow delay, group targeting, recovery time, frame gap, address check, counter reset, overlap detection).

## Notes for later milestones

- **Values are meter-side.** With `MYEMS_APPLIES` the simulator serves secondary values; the firmware multiplies by CT/PT. For the 4.3 vectors use `--set` (see README).
- **Counters never go backwards** except in `COUNTER_RESET`; switching modes keeps the counter value.
- **SLOW** replies are sent from the main loop, so the simulator keeps receiving while a reply is pending (same technique as the COM late-response tests).
- The HIL runner (D6) will use `--control-file` to change modes without restarting the simulator.

## Exit record (fill in)

| Item | Value |
|---|---|
| Date | |
| Commit | |
| Result | |
