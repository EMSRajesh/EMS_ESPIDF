#!/usr/bin/env python3
"""
MyEMS Modbus RTU full self-test runner v3 (Windows / ESP-IDF v6.1).

Fixes over v2:
- Starts simulator with --reset-on-fc01 for deterministic coil/register state.
- Retries simulator startup when Windows has not released COM12.
- Captures simulator stdout/stderr and preserves the exact startup exception.
- Waits for simulator to remain alive before starting ESP-IDF monitor.
- Stops and waits for every child process before proceeding.
- Uses PASS / FAIL / NOT EXECUTED as distinct test states.
- Parses timestamp-prefixed RX records correctly.
- Starts a fresh ESP-IDF monitor per test; monitor output is saved verbatim.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

CASES = [
    ("baseline", "none", None),
    ("FI_01_drop", "drop", 1),
    ("FI_02_bad_crc", "bad-crc", 2),
    ("FI_03_exception", "exception", 4),
    ("FI_04_wrong_slave", "wrong-slave", 1),
    ("FI_05_short_response", "short-response", 1),
    ("FI_06_delay", "delay", 1),
    ("FI_06_delay_first", "delay-first", 1),
    ("FI_07_recovery", "none", None),
]

def now_id():
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")

def resolve_idf():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH is not set. Open the ESP-IDF v6.1 PowerShell environment.")
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.is_file():
        raise RuntimeError(f"Cannot find {idf_py}")
    candidates = []
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path:
        candidates.append(Path(env_path) / "Scripts" / "python.exe")
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    if tools_path:
        candidates.extend(Path(tools_path).glob("python/*/venv/Scripts/python.exe"))
    candidates.append(Path(sys.executable))
    for exe in candidates:
        if exe.is_file():
            return str(exe), str(idf_py)
    raise RuntimeError("Cannot find the ESP-IDF Python interpreter.")

def idf_cmd(*args):
    py, idf_py = resolve_idf()
    return [py, idf_py, *map(str, args)]

def run_command(cmd, cwd, log_file):
    print("\n> " + subprocess.list2cmdline([str(x) for x in cmd]), flush=True)
    p = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, errors="replace")
    output = p.stdout or ""
    Path(log_file).write_text(output, encoding="utf-8", errors="replace")
    print(output[-6000:], end="" if output.endswith("\n") else "\n", flush=True)
    if p.returncode:
        raise RuntimeError(f"Command returned {p.returncode}; see {log_file}")
    return output

def stop_process(proc, graceful=False):
    if proc is None or proc.poll() is not None:
        return
    if graceful and proc.stdin:
        try:
            proc.stdin.write("exit\n")
            proc.stdin.flush()
            proc.wait(timeout=3)
            return
        except Exception:
            pass
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)

def start_simulator(simulator, case_dir, port, baud, fault, delay_ms,
                    startup_attempts, release_wait):
    sim_log = case_dir / "simulator.log"
    process_log = case_dir / "simulator_process_output.log"
    for attempt in range(1, startup_attempts + 1):
        # Preserve all attempts in a single process log.
        with process_log.open("a", encoding="utf-8", errors="replace") as output:
            output.write(f"\n===== START ATTEMPT {attempt}/{startup_attempts} =====\n")
            output.flush()
            cmd = [
                sys.executable, str(simulator),
                "--port", port, "--slaves", "1", "--baud", str(baud),
                "--parity", "E", "--stopbits", "1",
                "--fault", fault, "--reset-on-fc01",
                "--log-file", str(sim_log),
            ]
            if fault in ("delay", "delay-first"):
                cmd += ["--delay-ms", str(delay_ms)]
            proc = subprocess.Popen(
                cmd, cwd=simulator.parent.parent, stdout=output,
                stderr=subprocess.STDOUT
            )
            time.sleep(1.5)
            if proc.poll() is None:
                # A running process after this interval has passed serial.Serial open.
                return proc, sim_log, process_log, attempt
            rc = proc.returncode
            output.write(f"SIMULATOR_EXITED returncode={rc}\n")
            output.flush()
        time.sleep(release_wait * attempt)
    detail = process_log.read_text(encoding="utf-8", errors="replace") if process_log.exists() else ""
    raise RuntimeError(
        f"Simulator could not acquire {port} after {startup_attempts} attempts. "
        f"See {process_log}. Last output:\n{detail[-2500:]}"
    )

def capture_monitor(project, build_dir, console_port, timeout_s, console_file):
    cmd = idf_cmd("-B", build_dir, "-p", console_port, "monitor")
    print("\nStarting ESP-IDF monitor: " + subprocess.list2cmdline(cmd), flush=True)
    proc = subprocess.Popen(
        cmd, cwd=project, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8",
        errors="replace", bufsize=1
    )
    q = queue.Queue()
    captured = []

    def reader():
        try:
            for line in iter(proc.stdout.readline, ""):
                q.put(line)
        finally:
            q.put(None)

    threading.Thread(target=reader, daemon=True).start()
    start = time.monotonic()
    got_summary = False
    try:
        while time.monotonic() - start < timeout_s:
            try:
                line = q.get(timeout=0.25)
            except queue.Empty:
                if proc.poll() is not None:
                    break
                continue
            if line is None:
                break
            captured.append(line)
            print(line, end="", flush=True)
            if "MODE=SELF_TEST result=" in line:
                got_summary = True
                # Let monitor flush final output before requesting graceful exit.
                time.sleep(0.25)
                break
    finally:
        stop_process(proc, graceful=True)
    text = "".join(captured)
    console_file.write_text(text, encoding="utf-8", errors="replace")
    return text, got_summary

def evaluate(label, fault, expected, console, sim_text, summary_seen):
    statuses = [int(x) for x in re.findall(r": FAIL status=(\d+)", console)]
    validation_count = len(re.findall(r"VALIDATE .*: PASS", console))
    result_line = re.search(r"MODE=SELF_TEST result=(PASS|FAIL)", console)
    summary_line = re.search(r"SELF_TEST_SUMMARY:\s*(PASS|FAIL)", console)
    crash = bool(re.search(
        r"Guru Meditation|stack overflow|assert failed|abort\(\)|Backtrace:", console, re.I
    ))
    # A missing simulator START/serial-open log is an execution failure, not a DUT failure.
    sim_started = bool(re.search(r"START port=", sim_text))
    sim_port_error = bool(re.search(r"ERROR .*COM\d+|Access is denied|PermissionError", sim_text, re.I))

    if not sim_started or sim_port_error:
        return {"test": label, "fault": fault, "result": "NOT EXECUTED",
                "observed_statuses": statuses, "validation_passes": validation_count,
                "simulator_rx": len(re.findall(r"(?:^|\s)RX ", sim_text, re.M)),
                "details": "Simulator did not successfully start/open its serial port."}

    if label in ("baseline", "FI_07_recovery"):
        passed = bool(summary_line and summary_line.group(1) == "PASS"
                      and result_line and result_line.group(1) == "PASS"
                      and validation_count == 4 and not crash)
        details = "all nine FC checks, write readbacks and four validations passed" if passed else (
            f"firmware={result_line.group(1) if result_line else 'missing'}; "
            f"summary={summary_line.group(1) if summary_line else 'missing'}; "
            f"validation_passes={validation_count}; crash={crash}"
        )
    elif label == "FI_06_delay_first":
        # The first response is delayed beyond timeout; retry response is immediate.
        # The firmware should recover and complete the full self-test successfully.
        failed_assertions = len(re.findall(r": FAIL status=\d+", console))
        passed = bool(summary_line and summary_line.group(1) == "PASS"
                      and result_line and result_line.group(1) == "PASS"
                      and validation_count == 4 and failed_assertions == 0
                      and not crash and summary_seen
                      and re.search(r"FAULT delay-first: subsequent response sent immediately", sim_text))
        details = (f"expected full self-test recovery after first delayed response; "
                   f"failed_assertions={failed_assertions}; validation_passes={validation_count}; "
                   f"firmware={result_line.group(1) if result_line else 'missing'}")
    else:
        passed = bool(statuses) and all(s == expected for s in statuses)
        passed = passed and validation_count == 4 and not crash and summary_seen
        details = (f"expected_status={expected}; observed={statuses}; "
                   f"validation_passes={validation_count}; "
                   f"firmware={result_line.group(1) if result_line else 'missing'}")
    return {
        "test": label, "fault": fault, "result": "PASS" if passed else "FAIL",
        "observed_statuses": statuses, "validation_passes": validation_count,
        "simulator_rx": len(re.findall(r"(?:^|\s)RX ", sim_text, re.M)),
        "firmware_result": result_line.group(1) if result_line else None,
        "details": details
    }

def main():
    ap = argparse.ArgumentParser(description="MyEMS complete Modbus RTU self-test suite v3")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--delay-ms", type=int, default=1500)
    ap.add_argument("--build-dir", default="build_selftest")
    ap.add_argument("--timeout", type=int, default=35)
    ap.add_argument("--startup-attempts", type=int, default=5)
    ap.add_argument("--release-wait", type=float, default=1.5)
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-flash", action="store_true")
    ap.add_argument("--only", nargs="*", choices=[c[0] for c in CASES])
    args = ap.parse_args()

    project = Path(__file__).resolve().parent.parent
    tools = Path(__file__).resolve().parent
    simulator = tools / "myems_usb_rs485_modbus_slave_sim_all_fc_reset_delay_first.py"
    if not simulator.is_file():
        raise SystemExit(f"Simulator file not found: {simulator}")
    try:
        import serial  # noqa: F401
    except ImportError:
        raise SystemExit("pyserial missing. Install once: python -m pip install pyserial")

    run_tag = now_id()
    result_root = tools / "test_results" / f"full_rtu_suite_v4_{run_tag}"
    result_root.mkdir(parents=True, exist_ok=True)
    print("MYEMS FULL MODBUS RTU TEST SUITE — V4")
    print(f"Project: {project}")
    print(f"ESP32 console: {args.console_port} @115200")
    print(f"USB-RS485 simulator: {args.rs485_port} @ {args.baud}, 8E1")
    print(f"Results: {result_root}")
    print("\nSAFETY: connect RS485 to the USB simulator only. Self-test performs writes.")

    try:
        resolve_idf()
        if not args.skip_build:
            run_command(idf_cmd("-B", args.build_dir, "-D",
                                 f"SDKCONFIG={project / 'sdkconfig.selftest'}", "build"),
                        project, result_root / "build.log")
        if not args.skip_flash:
            run_command(idf_cmd("-B", args.build_dir, "-p", args.console_port, "flash"),
                        project, result_root / "flash.log")
    except Exception as exc:
        (result_root / "setup_error.txt").write_text(str(exc), encoding="utf-8")
        print(f"\nSETUP FAILED: {exc}\nSee {result_root / 'setup_error.txt'}")
        return 1

    selected = [c for c in CASES if not args.only or c[0] in args.only]
    results = []
    for idx, (label, fault, expected) in enumerate(selected, 1):
        case_dir = result_root / f"{idx:02d}_{label}"
        case_dir.mkdir()
        console_file = case_dir / "esp32_console.log"
        sim_proc = None
        sim_log = case_dir / "simulator.log"
        sim_process_log = case_dir / "simulator_process_output.log"
        console = ""
        summary_seen = False
        sim_text = ""
        try:
            sim_proc, sim_log, sim_process_log, attempt = start_simulator(
                simulator, case_dir, args.rs485_port, args.baud, fault,
                args.delay_ms, args.startup_attempts, args.release_wait
            )
            print(f"\n[{label}] simulator acquired {args.rs485_port} "
                  f"(startup attempt {attempt}); resetting/running ESP32 test.")
            console, summary_seen = capture_monitor(
                project, args.build_dir, args.console_port, args.timeout, console_file
            )
        except Exception as exc:
            (case_dir / "runner_error.txt").write_text(str(exc), encoding="utf-8")
            print(f"\n[{label}] RUNNER ERROR: {exc}")
        finally:
            stop_process(sim_proc)
            # Give Windows USB serial driver time to release COM12 before next case.
            time.sleep(args.release_wait)
            if sim_log.exists():
                sim_text = sim_log.read_text(encoding="utf-8", errors="replace")
            if sim_process_log.exists():
                (case_dir / "simulator_process_capture.log").write_text(
                    sim_process_log.read_text(encoding="utf-8", errors="replace"),
                    encoding="utf-8"
                )

        if (case_dir / "runner_error.txt").exists():
            res = {"test": label, "fault": fault, "result": "NOT EXECUTED",
                   "observed_statuses": [], "validation_passes": 0,
                   "simulator_rx": len(re.findall(r"(?:^|\s)RX ", sim_text, re.M)),
                   "details": (case_dir / "runner_error.txt").read_text(encoding="utf-8")}
        else:
            res = evaluate(label, fault, expected, console, sim_text, summary_seen)
        results.append(res)
        print(f"\n>>> {label}: {res['result']} — {res['details']}\n", flush=True)

    counts = {state: sum(r["result"] == state for r in results)
              for state in ("PASS", "FAIL", "NOT EXECUTED")}
    overall = "PASS" if counts["FAIL"] == 0 and counts["NOT EXECUTED"] == 0 else "FAIL"
    summary = {
        "suite": "MyEMS Modbus RTU self-test v4", "overall": overall,
        "total": len(results), **counts, "started": run_tag,
        "console_port": args.console_port, "rs485_port": args.rs485_port,
        "results": results,
        "notes": [
            "Simulator reset-on-fc01 is enabled for deterministic initial state.",
            "NOT EXECUTED means the test runner/simulator could not start the test; it is not a firmware failure.",
            "FI-06 delay-first does not prove complete same-slave/same-function stale-response isolation."
        ]
    }
    (result_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    csv_fields = ["test", "fault", "result", "observed_statuses",
                  "validation_passes", "simulator_rx", "firmware_result", "details"]
    with (result_root / "summary.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=csv_fields)
        writer.writeheader()
        for r in results:
            row = dict(r)
            row["observed_statuses"] = ",".join(map(str, row.get("observed_statuses", [])))
            writer.writerow({k: row.get(k, "") for k in csv_fields})
    report = [
        f"OVERALL: {overall}", f"TOTAL: {len(results)}",
        f"PASS: {counts['PASS']}", f"FAIL: {counts['FAIL']}",
        f"NOT EXECUTED: {counts['NOT EXECUTED']}", "", "TEST RESULTS:"
    ]
    report += [f"{r['result']:12} {r['test']}: {r['details']}" for r in results]
    report += ["", "See each case directory for ESP32 console and simulator logs."]
    (result_root / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n" + "\n".join(report))
    print(f"\nREPORT: {result_root / 'report.txt'}")
    return 0 if overall == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
