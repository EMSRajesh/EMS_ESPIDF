# DP-WP4 tools

| File | Purpose |
|---|---|
| `myems_ac_meter_sim.py` | AC meter simulator (Modbus RTU slave) for DP-WP4 HIL tests. **Simulation only.** |
| `tests/test_ac_meter_sim.py` | Simulator tests SIM-001…SIM-012 and more; no serial port needed |

The COM simulators in `tools/` (`myems_usb_rs485_modbus_slave_sim_*.py`) are unchanged and are still used for the COM regression tests (REG-02, REG-03).

## How it gets its register layout

The simulator does not contain a register map. It loads a site file and builds the layout with `tools/profile/myems_profile_tool.py`. The firmware tables are generated from the same JSON, so both always agree. The start line shows the configuration CRC; it must equal the CRC in the ESP32 boot banner (from D7 on).

## Run

ESP-IDF PowerShell, project root. `pyserial` is needed only for a real port (`python -m pip install -r tools/requirements-usb-rs485.txt`).

```powershell
# Check without hardware
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --self-test
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --dump

# Serve on the USB-RS485 adapter (9600 8E1 from the site file)
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench.site.json --port COM12 --log-file tools/test_results/sim.log

# Two meters, slave 1 not answering
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_bench_2dev.site.json --port COM12 --mode-for 1=TIMEOUT

# Test Spec 4.3 ratio vectors (meter-side values)
python tools/wp4/myems_ac_meter_sim.py --site config/site/lab_ratio_ct200.site.json --port COM12 `
    --set 1:ac.current.l1=2.5 --set 1:ac.current.l2=6.0 --set 1:ac.power.active.total=1.0
```

Stop with Ctrl+C; the last log line shows the counters (rx, tx, no_response, overlap, ...).

## Modes

`NAME[:ARG][@GROUP]`. With `@GROUP`, a fault mode only affects that poll group (e.g. `EXCEPTION:02@energy` for DEV-004).

| Mode | Behaviour |
|---|---|
| `NORMAL` | Test Spec 4.1 values, static (decoder map: Test Spec 4.2 vectors) |
| `LOAD_CHANGE[:STEP_S]` | Load steps every STEP_S s (default 10), same sequence for the same `--seed` |
| `IMPORT` | +12.5 kW; import counter +0.01 kWh every 2.88 s |
| `EXPORT` | −12.5 kW, PF −0.972; export counter increases |
| `TIMEOUT` | No response |
| `EXCEPTION[:CODE]` | Exception response, default code 02 |
| `BAD_LENGTH` | Byte count field 2 larger than the payload; CRC valid |
| `BAD_CRC` | Last CRC byte inverted |
| `OUT_OF_RANGE` | Frequency 30.00 Hz |
| `COUNTER_RESET` | Import counter set to 0 once, then counts up |
| `SLOW[:MS]` | Reply after MS ms (default 400) |
| `OFFLINE_THEN_RECOVER[:S]` | No response for S s (default 60), then NORMAL |

Always: writes, FC01 and FC02 → exception 01; addresses outside the map's poll groups → exception 02; quantity 0 or > 125 → exception 03. A request for a slave that still has a delayed reply pending is logged as `OVERLAP` (evidence for SCH-003).

## Runtime control

With `--control-file ctl.txt` the simulator rereads the file whenever it changes:

```text
mode 1 EXCEPTION:02@energy
mode * NORMAL
set 2 ac.frequency 49.5
```

## Tests

```powershell
python -m unittest discover -s tools/wp4/tests -v
```
