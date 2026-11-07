#!/usr/bin/env python3
"""
MyEMS Modbus RTU fault-injection test runner.

Runs the existing simulator in separate scenarios and captures ESP32 UART logs.
Each scenario gets its own simulator log and console log. Results are written to
CSV, JSON, and a human-readable report.

Run from the tools directory:
  python .\\myems_fault_injection_test.py --project .. --duration-per-test 45

Prerequisites:
  python -m pip install pyserial
  Close ESP-IDF Serial Monitor so this script can open COM11.
  Ensure COM12 is not used by another process.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
import threading
import time
from collections import Counter
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    print("Missing dependency. Install with: python -m pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


DEFAULT_SIMULATOR = "tools/usb_rs485_modbus_slave_sim_all_fc.py"

SUCCESS_RE = re.compile(
    r"ems_acquisition:\s*(?P<meter>\S+)\s+request=(?P<request>\d+)\s+"
    r"slave=(?P<slave>\d+)\s+FC(?P<fc>\d+)\s+OK\s+"
    r"registers=(?P<registers>\d+)\s+retries=(?P<retries>\d+)",
    re.I,
)
SCHEDULE_RE = re.compile(
    r"ems_scheduler:\s*scheduled\s+(?P<meter>\S+):\s*request=(?P<request>\d+)\s+"
    r"slave=(?P<slave>\d+)\s+FC(?P<fc>\d+)",
    re.I,
)
REG_RE = re.compile(
    r"ems_acquisition:\s*(?P<meter>\S+)\s+register\[(?P<index>\d+)\]="
    r"(?P<value>-?\d+)\s+\((?P<hex>0x[0-9A-Fa-f]+)\)", re.I
)
FAILURE_RE = re.compile(
    r"ems_acquisition:\s*(?P<meter>\S+)\s+request=(?P<request>\d+)\s+"
    r"failed\s+status=(?P<status>\d+)\s+exception=0x(?P<exception>[0-9A-Fa-f]+)\s+"
    r"retries=(?P<retries>\d+)", re.I
)
RETRY_RE = re.compile(
    r"modbus_master:\s*request\s+(?P<request>\d+)\s+retry\s+"
    r"(?P<attempt>\d+)/(?P<maximum>\d+)\s+after\s+status\s+(?P<status>\d+)", re.I
)

PATTERNS = {
    "request_failed": re.compile(r"ems_acquisition:.*failed status=", re.I),
    "retry": RETRY_RE,
    "timeout": re.compile(r"\b(timeout|timed out|response timeout)\b", re.I),
    "crc_error": re.compile(r"\b(bad[- ]CRC|CRC error|invalid CRC|CRC mismatch)\b", re.I),
    "transport_error": re.compile(r"\b(transport error|UART error|ESP_FAIL)\b", re.I),
    "stack_overflow": re.compile(r"stack overflow|stack canary|Stack smashing", re.I),
    "panic": re.compile(r"assert failed|Guru Meditation|abort\(\) was called", re.I),
    "modbus_exception": re.compile(r"exception response|Modbus exception|exception code", re.I),
    "wrong_slave": re.compile(r"wrong slave|slave mismatch|unexpected slave", re.I),
    "malformed": re.compile(r"malformed|short response|invalid frame|invalid response", re.I),
    "recovery": re.compile(r"recovered|recovery|resuming|resume normal", re.I),
}


class Capture:
    def __init__(self):
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.lines = []
        self.successes = []
        self.scheduled = []
        self.registers = []
        self.failures = []
        self.retries = []
        self.errors = Counter()
        self.error_events = []

    def add(self, line):
        now = datetime.now().astimezone().isoformat(timespec="milliseconds")
        with self.lock:
            self.lines.append(line)
            for name, pat in PATTERNS.items():
                if pat.search(line):
                    self.errors[name] += 1
                    self.error_events.append({"timestamp": now, "type": name, "line": line})
            for pat, target in ((SUCCESS_RE, self.successes), (SCHEDULE_RE, self.scheduled),
                                (REG_RE, self.registers), (FAILURE_RE, self.failures),
                                (RETRY_RE, self.retries)):
                m = pat.search(line)
                if not m:
                    continue
                d = m.groupdict()
                d["timestamp"] = now
                for key in ("request", "slave", "fc", "registers", "retries", "index",
                            "value", "status", "exception", "attempt", "maximum"):
                    if key in d and d[key] is not None:
                        d[key] = int(d[key], 16) if key == "exception" else int(d[key])
                target.append(d)


def console_reader(port, baud, cap, path):
    try:
        with serial.Serial(port, baudrate=baud, timeout=0.25) as ser, path.open(
            "a", encoding="utf-8", errors="replace"
        ) as f:
            f.write(f"# console opened {port} {baud}\n")
            f.flush()
            while not cap.stop.is_set():
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                f.write(f"{datetime.now().astimezone().isoformat(timespec='milliseconds')} {line}\n")
                f.flush()
                cap.add(line)
    except Exception as e:
        with cap.lock:
            cap.error_events.append({"timestamp": datetime.now().astimezone().isoformat(),
                                     "type": "console_open_error", "line": str(e)})
        print(f"[ERROR] Console {port}: {e}", file=sys.stderr)


def simulator_reader(pipe, path):
    with path.open("a", encoding="utf-8", errors="replace") as f:
        for line in iter(pipe.readline, ""):
            f.write(f"{datetime.now().astimezone().isoformat(timespec='milliseconds')} {line.rstrip()}\n")
            f.flush()
            print("[SIM] " + line.rstrip())


def write_csv(path, rows, fields):
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description="MyEMS Modbus RTU fault-injection runner")
    ap.add_argument("--project", type=Path, default=Path.cwd().parent,
                    help="Project root; default is parent of current directory")
    ap.add_argument("--simulator", default=DEFAULT_SIMULATOR,
                    help="Simulator path relative to project root")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--console-baud", type=int, default=115200)
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--duration-per-test", type=int, default=45)
    ap.add_argument("--startup-wait", type=float, default=2.0)
    ap.add_argument("--delay-ms", type=int, default=1500)
    ap.add_argument("--fault-every", type=int, default=1,
                    help="Inject fault every Nth matching request (simulator-supported behavior)")
    ap.add_argument("--output", type=Path, default=Path("test_results"))
    ap.add_argument("--scenarios", default="none,drop,bad-crc,delay,exception,wrong-slave,short-response",
                    help="Comma-separated subset of supported simulator fault modes")
    args = ap.parse_args()

    project = args.project.resolve()
    simscript = (project / args.simulator).resolve()
    if not simscript.is_file():
        print(f"Simulator not found: {simscript}", file=sys.stderr)
        print("Use --simulator with the path found by Get-ChildItem.", file=sys.stderr)
        return 2
    if args.duration_per_test < 5:
        print("--duration-per-test must be at least 5 seconds.", file=sys.stderr)
        return 2

    supported = {"none", "drop", "bad-crc", "delay", "exception", "wrong-slave", "short-response"}
    scenarios = [x.strip() for x in args.scenarios.split(",") if x.strip()]
    if not scenarios or any(x not in supported for x in scenarios):
        print(f"Unsupported scenario. Choose from: {', '.join(sorted(supported))}", file=sys.stderr)
        return 2

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    outdir = (args.output if args.output.is_absolute() else project / args.output) / f"fault_test_{stamp}"
    outdir.mkdir(parents=True, exist_ok=True)
    cap = Capture()
    console_path = outdir / "esp32_console.log"
    console_thread = threading.Thread(
        target=console_reader,
        args=(args.console_port, args.console_baud, cap, console_path), daemon=True
    )
    console_thread.start()
    time.sleep(args.startup_wait)

    scenario_rows = []
    print(f"Project: {project}")
    print(f"Simulator: {simscript}")
    print(f"ESP32 console: {args.console_port} @ {args.console_baud}")
    print(f"RS-485: {args.rs485_port} @ {args.baud}, 8E1")
    print(f"Output: {outdir}")
    print("Do not open a second serial monitor or simulator while this runs.")

    try:
        for index, fault in enumerate(scenarios, start=1):
            before_success = len(cap.successes)
            before_scheduled = len(cap.scheduled)
            before_failures = len(cap.failures)
            before_retries = len(cap.retries)
            before_errors = Counter(cap.errors)
            sim_log = outdir / f"simulator_{index:02d}_{fault}.log"
            cmd = [
                sys.executable, str(simscript),
                "--port", args.rs485_port,
                "--slaves", "1,2",
                "--baud", str(args.baud),
                "--parity", "E",
                "--stopbits", "1",
                "--fault", fault,
                "--fault-every", str(args.fault_every),
                "--delay-ms", str(args.delay_ms),
            ]
            print(f"\n[{index}/{len(scenarios)}] Fault mode: {fault}")
            with sim_log.open("w", encoding="utf-8", errors="replace") as simout:
                proc = subprocess.Popen(
                    cmd, cwd=str(project), stdout=simout, stderr=subprocess.STDOUT
                )
                start = time.monotonic()
                try:
                    while time.monotonic() - start < args.duration_per_test:
                        if proc.poll() is not None:
                            print(f"WARNING: simulator exited early ({proc.returncode})")
                            break
                        time.sleep(0.2)
                except KeyboardInterrupt:
                    proc.terminate()
                    raise
                finally:
                    if proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait(timeout=2)
            # Give UART reader a moment to collect buffered log lines.
            time.sleep(0.5)
            with cap.lock:
                new_success = cap.successes[before_success:]
                new_scheduled = cap.scheduled[before_scheduled:]
                new_failures = cap.failures[before_failures:]
                new_retries = cap.retries[before_retries:]
                delta_errors = {k: cap.errors[k] - before_errors[k] for k in cap.errors
                                if cap.errors[k] - before_errors[k] > 0}
            success_slaves = sorted({int(r["slave"]) for r in new_success if "slave" in r})
            scenario_rows.append({
                "scenario": fault,
                "duration_seconds": args.duration_per_test,
                "scheduled_requests": len(new_scheduled),
                "successful_polls": len(new_success),
                "failed_requests": len(new_failures),
                "retry_events": len(new_retries),
                "failure_statuses": ",".join(str(x["status"]) for x in new_failures),
                "successful_slave_ids": ",".join(map(str, success_slaves)),
                "detected_log_errors": json.dumps(delta_errors, sort_keys=True),
                "stack_overflow_or_panic": bool(delta_errors.get("stack_overflow", 0)
                                                or delta_errors.get("panic", 0)),
                "result": "OBSERVED" if new_scheduled or new_success else "NO_FIRMWARE_ACTIVITY_OBSERVED",
            })
            print(f"  scheduled={len(new_scheduled)} successful={len(new_success)} "
                  f"failed={len(new_failures)} retries={len(new_retries)} "
                  f"failure_statuses={[x['status'] for x in new_failures]} "
                  f"slaves={success_slaves or '-'} errors={delta_errors or 'none'}")
    except KeyboardInterrupt:
        print("\nInterrupted; saving partial results...")
    finally:
        cap.stop.set()
        console_thread.join(timeout=2)

    with cap.lock:
        successes = list(cap.successes)
        scheduled = list(cap.scheduled)
        registers = list(cap.registers)
        failures = list(cap.failures)
        retries = list(cap.retries)
        errors = dict(cap.errors)
        error_events = list(cap.error_events)
        console_lines = list(cap.lines)

    write_csv(outdir / "scenario_results.csv", scenario_rows, [
        "scenario", "duration_seconds", "scheduled_requests", "successful_polls",
        "failed_requests", "retry_events", "failure_statuses", "successful_slave_ids",
        "detected_log_errors", "stack_overflow_or_panic", "result"
    ])
    write_csv(outdir / "successful_polls.csv", successes,
              ["timestamp", "meter", "request", "slave", "fc", "registers", "retries"])
    write_csv(outdir / "failed_requests.csv", failures,
              ["timestamp", "meter", "request", "status", "exception", "retries"])
    write_csv(outdir / "retry_events.csv", retries,
              ["timestamp", "request", "attempt", "maximum", "status"])
    write_csv(outdir / "scheduled_requests.csv", scheduled,
              ["timestamp", "meter", "request", "slave", "fc"])
    write_csv(outdir / "decoded_registers.csv", registers,
              ["timestamp", "meter", "index", "value", "hex"])
    (outdir / "error_events.json").write_text(json.dumps(error_events, indent=2), encoding="utf-8")
    summary = {
        "test": "MyEMS Modbus RTU fault injection",
        "start_local": stamp,
        "scenarios_requested": scenarios,
        "duration_per_test_seconds": args.duration_per_test,
        "console_port": args.console_port,
        "rs485_port": args.rs485_port,
        "baud": args.baud,
        "format": "8E1",
        "scenario_results": scenario_rows,
        "total_successful_polls": len(successes),
        "total_failed_requests": len(failures),
        "total_retry_events": len(retries),
        "total_scheduled_requests": len(scheduled),
        "detected_log_errors": errors,
        "console_line_count": len(console_lines),
        "error_events": error_events,
        "interpretation": (
            "Scenario results are observations, not automatic proof of correct handling. "
            "Compare each scenario with firmware timeout/error logs and confirm the unaffected "
            "meter continues polling. A missing log pattern is not proof a fault was rejected."
        ),
    }
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    lines = [
        "MyEMS Modbus RTU Fault-Injection Report",
        "=" * 40,
        f"Start: {stamp}",
        f"Requested scenarios: {', '.join(scenarios)}",
        f"Duration per scenario: {args.duration_per_test}s",
        f"Total successful polls: {len(successes)}",
        f"Total failed requests: {len(failures)}",
        f"Total retry events: {len(retries)}",
        f"Total scheduled requests: {len(scheduled)}",
        f"Detected errors: {errors or 'none detected by configured patterns'}",
        "",
        "Scenario observations:",
    ]
    for row in scenario_rows:
        lines.append(
            f"- {row['scenario']}: {row['result']}; scheduled={row['scheduled_requests']}; "
            f"successful={row['successful_polls']}; failed={row['failed_requests']}; "
            f"retries={row['retry_events']}; statuses={row['failure_statuses'] or '-'}; "
            f"slaves={row['successful_slave_ids'] or '-'}; "
            f"errors={row['detected_log_errors'] or '{}'}"
        )
    lines += [
        "",
        "Important: this runner reports observations. Review raw logs and confirm the unaffected",
        "meter continues polling. Missing matching error text does not prove correct fault handling.",
        "",
        "Files: scenario_results.csv, summary.json, successful_polls.csv, scheduled_requests.csv,",
        "decoded_registers.csv, failed_requests.csv, retry_events.csv, error_events.json,",
        "esp32_console.log, simulator_*.log",
    ]
    (outdir / "report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n" + "\n".join(lines))
    print(f"\nResults saved to: {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
