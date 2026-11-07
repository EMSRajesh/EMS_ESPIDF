#!/usr/bin/env python3
"""MyEMS Modbus RTU reliability test runner V5.1 (ESP-IDF v6.1 / Windows).

V5.1 removes the physical EN/RESET-button requirement. It starts idf_monitor
with --no-reset, waits for the monitor process to initialize, then sends the
monitor's documented `reset` command through stdin. The reader thread is
already capturing output before reset is requested.
"""
from __future__ import annotations
import argparse, csv, datetime as dt, json, os, queue, re, subprocess
import sys, threading, time
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
    raise RuntimeError("Cannot find ESP-IDF Python interpreter.")

def idf_cmd(*args):
    py, idf_py = resolve_idf()
    return [py, idf_py, *map(str, args)]

def run_command(cmd, cwd, log_path):
    print("\n> " + subprocess.list2cmdline([str(x) for x in cmd]), flush=True)
    p = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, errors="replace")
    out = p.stdout or ""
    log_path.write_text(out, encoding="utf-8", errors="replace")
    print(out[-6000:], end="" if out.endswith("\n") else "\n", flush=True)
    if p.returncode:
        raise RuntimeError(f"Command returned {p.returncode}; see {log_path}")
    return out

def stop_process(proc):
    if proc is None or proc.poll() is not None:
        return
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
        with process_log.open("a", encoding="utf-8", errors="replace") as out:
            out.write(f"\n===== START ATTEMPT {attempt}/{startup_attempts} =====\n")
            out.flush()
            cmd = [sys.executable, str(simulator), "--port", port, "--slaves", "1",
                   "--baud", str(baud), "--parity", "E", "--stopbits", "1",
                   "--fault", fault, "--reset-on-fc01", "--log-file", str(sim_log)]
            if fault in ("delay", "delay-first"):
                cmd += ["--delay-ms", str(delay_ms)]
            proc = subprocess.Popen(cmd, cwd=simulator.parent.parent,
                                    stdout=out, stderr=subprocess.STDOUT)
            time.sleep(1.5)
            if proc.poll() is None:
                return proc, sim_log, process_log, attempt
            out.write(f"SIMULATOR_EXITED returncode={proc.returncode}\n")
            out.flush()
        time.sleep(release_wait * attempt)
    detail = process_log.read_text(encoding="utf-8", errors="replace") if process_log.exists() else ""
    raise RuntimeError(f"Simulator could not acquire {port}. {detail[-2500:]}")

def capture_one_monitor(project, build_dir, console_port, timeout_s, console_file,
                        monitor_start_delay):
    # --no-reset prevents monitor startup itself from causing an unobserved boot.
    cmd = idf_cmd("-B", build_dir, "-p", console_port, "monitor", "--no-reset")
    print("\nStarting ESP-IDF monitor without automatic reset:")
    print(subprocess.list2cmdline(cmd), flush=True)
    proc = subprocess.Popen(cmd, cwd=project, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", bufsize=1)
    q = queue.Queue()
    captured = []

    def reader():
        try:
            for line in iter(proc.stdout.readline, ""):
                q.put(line)
        finally:
            q.put(None)

    threading.Thread(target=reader, daemon=True).start()
    summary_seen = False
    reset_sent = False
    try:
        # idf_monitor prints its startup banner and opens COM before accepting commands.
        time.sleep(monitor_start_delay)
        if proc.poll() is not None:
            raise RuntimeError("ESP-IDF monitor exited before reset command. Check monitor startup output.")
        if proc.stdin is None:
            raise RuntimeError("ESP-IDF monitor stdin is unavailable; cannot issue reset command.")
        try:
            proc.stdin.write("reset\n")
            proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise RuntimeError(f"Could not send monitor reset command: {exc}") from exc
        reset_sent = True
        print("[runner] Sent ESP-IDF monitor 'reset' command; capturing boot and self-test.", flush=True)

        start = time.monotonic()
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
                summary_seen = True
                # Allow trailing serial output to drain before closing monitor.
                drain_until = time.monotonic() + 0.4
                while time.monotonic() < drain_until:
                    try:
                        extra = q.get(timeout=0.05)
                    except queue.Empty:
                        continue
                    if extra is None:
                        break
                    captured.append(extra)
                    print(extra, end="", flush=True)
                break
    finally:
        stop_process(proc)
    text = "".join(captured)
    console_file.write_text(text, encoding="utf-8", errors="replace")
    if not reset_sent:
        raise RuntimeError("Monitor reset command was not sent.")
    return text, summary_seen

def evaluate(label, fault, expected, console, sim_text, summary_seen):
    statuses = [int(x) for x in re.findall(r": FAIL status=(\d+)", console)]
    validation_count = len(re.findall(r"VALIDATE .*: PASS", console))
    result_line = re.search(r"MODE=SELF_TEST result=(PASS|FAIL)", console)
    summary_line = re.search(r"SELF_TEST_SUMMARY:\s*(PASS|FAIL)", console)
    crash = bool(re.search(r"Guru Meditation|stack overflow|assert failed|abort\(\)|Backtrace:", console, re.I))
    sim_started = bool(re.search(r"START port=", sim_text))
    sim_port_error = bool(re.search(r"ERROR .*COM\d+|Access is denied|PermissionError", sim_text, re.I))
    rx_count = len(re.findall(r"(?:^|\s)RX ", sim_text, re.M))
    if not sim_started or sim_port_error:
        state, details = "NOT EXECUTED", "Simulator failed to open its serial port."
    elif label in ("baseline", "FI_07_recovery"):
        first_fc01 = re.search(r"FC01 DEBUG:.*bits\[0\]=0x([0-9A-Fa-f]{2})", console)
        deterministic = bool(first_fc01 and first_fc01.group(1).upper() == "55")
        reset_once_count = sim_text.count("RESET_ONCE slave=1")
        passed = bool(summary_line and summary_line.group(1) == "PASS"
                      and result_line and result_line.group(1) == "PASS"
                      and validation_count == 4 and not crash and summary_seen
                      and deterministic and reset_once_count == 1 and rx_count > 0)
        state = "PASS" if passed else "FAIL"
        details = (f"full_self_test={result_line.group(1) if result_line else 'missing'}; "
                   f"first_fc01={first_fc01.group(1) if first_fc01 else 'missing'}; "
                   f"reset_once_count={reset_once_count}; simulator_rx={rx_count}; "
                   f"validation_passes={validation_count}; crash={crash}")
    elif label == "FI_06_delay_first":
        failed = len(re.findall(r": FAIL status=\d+", console))
        passed = bool(summary_line and summary_line.group(1) == "PASS"
                      and result_line and result_line.group(1) == "PASS"
                      and validation_count == 4 and failed == 0 and not crash
                      and summary_seen
                      and "FAULT delay-first: subsequent response sent immediately" in sim_text)
        state = "PASS" if passed else "FAIL"
        details = f"delay-first recovery; failed_assertions={failed}; validations={validation_count}"
    else:
        passed = bool(statuses) and all(s == expected for s in statuses)
        passed = passed and validation_count == 4 and not crash and summary_seen
        state = "PASS" if passed else "FAIL"
        details = f"expected_status={expected}; observed={statuses}; validations={validation_count}; crash={crash}"
    return {"test": label, "fault": fault, "result": state, "observed_statuses": statuses,
            "validation_passes": validation_count, "simulator_rx": rx_count,
            "firmware_result": result_line.group(1) if result_line else None,
            "details": details}

def main():
    ap = argparse.ArgumentParser(description="MyEMS Modbus RTU reliability runner V5.1")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--delay-ms", type=int, default=1500)
    ap.add_argument("--build-dir", default="build_selftest")
    ap.add_argument("--timeout", type=int, default=40)
    ap.add_argument("--startup-attempts", type=int, default=5)
    ap.add_argument("--release-wait", type=float, default=1.5)
    ap.add_argument("--monitor-start-delay", type=float, default=2.0)
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-flash", action="store_true")
    ap.add_argument("--only", nargs="*", choices=[c[0] for c in CASES])
    ap.add_argument("--repeat-baseline", type=int, default=1)
    args = ap.parse_args()

    project = Path(__file__).resolve().parent.parent
    tools = Path(__file__).resolve().parent
    simulator = tools / "myems_usb_rs485_modbus_slave_sim_v5.py"
    if not simulator.is_file():
        raise SystemExit(f"Simulator not found: {simulator}")
    try:
        import serial  # noqa: F401
    except ImportError:
        raise SystemExit("pyserial missing. Install: python -m pip install pyserial")
    if args.repeat_baseline < 1:
        raise SystemExit("--repeat-baseline must be >= 1")

    tag = now_id()
    result_root = tools / "test_results" / f"full_rtu_suite_v5_1_{tag}"
    result_root.mkdir(parents=True, exist_ok=True)
    print("MYEMS MODBUS RTU RELIABILITY TEST RUNNER — V5.1")
    print(f"Project: {project}\nConsole: {args.console_port} @115200\nRS485: {args.rs485_port} @ {args.baud}, 8E1")
    print(f"Results: {result_root}")
    print("\nSAFETY: disconnect all live meters. Connect only USB-RS485 simulator; self-test performs writes.")

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
        print(f"SETUP FAILED: {exc}")
        return 1

    selected = [c for c in CASES if not args.only or c[0] in args.only]
    if args.repeat_baseline > 1:
        selected = [("baseline", "none", None)] * args.repeat_baseline
    results = []
    for idx, (label, fault, expected) in enumerate(selected, 1):
        case_label = f"{label}_{idx:02d}" if args.repeat_baseline > 1 else label
        case_dir = result_root / f"{idx:02d}_{case_label}"
        case_dir.mkdir()
        sim_proc = None
        console = ""
        summary_seen = False
        sim_text = ""
        try:
            sim_proc, sim_log, sim_proc_log, attempt = start_simulator(
                simulator, case_dir, args.rs485_port, args.baud, fault,
                args.delay_ms, args.startup_attempts, args.release_wait)
            print(f"\n[{case_label}] Simulator ready (attempt {attempt}).")
            console, summary_seen = capture_one_monitor(
                project, args.build_dir, args.console_port, args.timeout,
                case_dir / "esp32_console.log", args.monitor_start_delay)
        except Exception as exc:
            (case_dir / "runner_error.txt").write_text(str(exc), encoding="utf-8")
            print(f"RUNNER ERROR: {exc}")
        finally:
            stop_process(sim_proc)
            time.sleep(args.release_wait)
            sim_log_path = case_dir / "simulator.log"
            if sim_log_path.exists():
                sim_text = sim_log_path.read_text(encoding="utf-8", errors="replace")
            proc_log_path = case_dir / "simulator_process_output.log"
            if proc_log_path.exists():
                (case_dir / "simulator_process_capture.log").write_text(
                    proc_log_path.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
        if (case_dir / "runner_error.txt").exists():
            res = {"test": case_label, "fault": fault, "result": "NOT EXECUTED",
                   "observed_statuses": [], "validation_passes": 0, "simulator_rx": 0,
                   "details": (case_dir / "runner_error.txt").read_text(encoding="utf-8")}
        else:
            res = evaluate(label, fault, expected, console, sim_text, summary_seen)
            res["test"] = case_label
        results.append(res)
        print(f"\n>>> {case_label}: {res['result']} — {res['details']}", flush=True)

    counts = {s: sum(r["result"] == s for r in results) for s in ("PASS", "FAIL", "NOT EXECUTED")}
    overall = "PASS" if counts["FAIL"] == 0 and counts["NOT EXECUTED"] == 0 else "FAIL"
    summary = {"suite": "MyEMS Modbus RTU V5.1", "overall": overall, "total": len(results),
               **counts, "started": tag, "console_port": args.console_port,
               "rs485_port": args.rs485_port, "results": results,
               "notes": ["Monitor starts with --no-reset; runner sends its reset command over monitor stdin.",
                         "Monitor output reader is active before reset is sent.",
                         "Simulator reset-on-fc01 resets each slave only once per simulator process.",
                         "A passing delay-first test does not prove same-slave/same-function stale-frame isolation."]}
    (result_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    fields = ["test", "fault", "result", "observed_statuses", "validation_passes", "simulator_rx", "firmware_result", "details"]
    with (result_root / "summary.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields); w.writeheader()
        for r in results:
            row = dict(r); row["observed_statuses"] = ",".join(map(str, row.get("observed_statuses", [])))
            w.writerow({k: row.get(k, "") for k in fields})
    report = [f"OVERALL: {overall}", f"TOTAL: {len(results)}", f"PASS: {counts['PASS']}",
              f"FAIL: {counts['FAIL']}", f"NOT EXECUTED: {counts['NOT EXECUTED']}", "", "TEST RESULTS:"]
    report += [f"{r['result']:12} {r['test']}: {r['details']}" for r in results]
    (result_root / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n" + "\n".join(report))
    print(f"\nREPORT: {result_root / 'report.txt'}")
    return 0 if overall == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
