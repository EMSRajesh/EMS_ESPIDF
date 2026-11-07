#!/usr/bin/env python3
"""MyEMS WP24-M1/M2 recovery-guard boundary characterization runner.

Run M1 and M2 separately. The delayed frame is classified using ESP console
timestamps (recovery start/release), not by assuming the simulator delay equals
the guard-relative delay. USB RS485 simulator only; never connect a live meter.
"""
from __future__ import annotations
import argparse, datetime as dt, os, queue, re, subprocess, sys, threading, time
from pathlib import Path


def idf_cmd(args):
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH is not set. Open ESP-IDF v6.1 PowerShell environment.")
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.is_file():
        raise RuntimeError(f"idf.py not found: {idf_py}")
    candidates = []
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path:
        candidates.append(Path(env_path) / "Scripts" / "python.exe")
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    if tools_path:
        candidates.extend(Path(tools_path).glob("python/*/venv/Scripts/python.exe"))
    candidates.append(Path(sys.executable))
    py = next((p for p in candidates if p.is_file()), None)
    if py is None:
        raise RuntimeError("Could not locate ESP-IDF Python interpreter")
    return [str(py), str(idf_py), *map(str, args)]


def run_logged(cmd, cwd, logfile):
    print("\\n> " + subprocess.list2cmdline(map(str, cmd)), flush=True)
    p = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, errors="replace")
    logfile.write_text(p.stdout or "", encoding="utf-8", errors="replace")
    print((p.stdout or "")[-6000:], end="\\n", flush=True)
    if p.returncode:
        raise RuntimeError(f"Command failed ({p.returncode}); see {logfile}")


def stop_process(p):
    if not p or p.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait(timeout=2)


def verify_config(project, build_dir, selector):
    header = project / build_dir / "config" / "sdkconfig.h"
    if not header.is_file():
        raise RuntimeError(f"Generated sdkconfig header missing: {header}. Build first.")
    cfg = header.read_text(encoding="utf-8", errors="replace")
    expected = [
        r"#define CONFIG_MYEMS_MODBUS_SELF_TEST 1",
        r"#define CONFIG_MYEMS_WP24_FAULT_INJECTION 1",
        rf"#define CONFIG_MYEMS_WP24_EXTENDED_TEST {selector}",
    ]
    for pattern in expected:
        if not re.search(pattern, cfg):
            raise RuntimeError(f"WP24-M config mismatch: expected {pattern}")


def start_simulator(simulator, project, port, baud, delay_ms, root):
    sim_log = root / "simulator.log"
    proc_log = root / "simulator_process.log"
    cmd = [sys.executable, str(simulator), "--port", port, "--slaves", "1",
           "--baud", str(baud), "--parity", "E", "--stopbits", "1",
           "--fault-sequence", "delay,drop", "--fault-fc", "3", "--fault-start", "100",
           "--delay-ms", str(delay_ms), "--reset-on-fc01", "--log-file", str(sim_log)]
    f = proc_log.open("w", encoding="utf-8", errors="replace")
    p = subprocess.Popen(cmd, cwd=project, stdout=f, stderr=subprocess.STDOUT)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 8:
        if p.poll() is not None:
            f.close()
            detail = sim_log.read_text(errors="replace") if sim_log.exists() else ""
            raise RuntimeError(f"Simulator exited: {detail or proc_log}")
        detail = sim_log.read_text(errors="replace") if sim_log.exists() else ""
        if "ERROR" in detail:
            stop_process(p); f.close()
            raise RuntimeError(f"Simulator error: {detail}")
        if "START port=" in detail and time.monotonic() - t0 > 1:
            return p, sim_log, proc_log, f
        time.sleep(.2)
    stop_process(p); f.close()
    raise RuntimeError("Simulator startup timeout")


def capture_monitor(project, build_dir, sdkconfig, console_port, timeout_s, outfile, timelinefile):
    cmd = idf_cmd(["-B", build_dir, "-D", f"SDKCONFIG={sdkconfig}",
                   "-p", console_port, "monitor"])
    print("\\nStarting ESP-IDF monitor: " + subprocess.list2cmdline(cmd), flush=True)
    p = subprocess.Popen(cmd, cwd=project, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                         errors="replace", bufsize=1)
    q = queue.Queue(); lines = []; timeline = []
    def reader():
        try:
            for line in iter(p.stdout.readline, ""):
                q.put(line)
        finally:
            q.put(None)
    threading.Thread(target=reader, daemon=True).start()
    start = time.monotonic(); done = False
    try:
        while time.monotonic() - start < timeout_s:
            try:
                line = q.get(timeout=.25)
            except queue.Empty:
                if p.poll() is not None: break
                continue
            if line is None: break
            lines.append(line)
            timeline.append(f"{dt.datetime.now(dt.timezone.utc).isoformat()}\\t{line.rstrip()}\\n")
            print(line, end="", flush=True)
            if "MODE=SELF_TEST result=" in line:
                done = True
                time.sleep(.4)
                break
    finally:
        if p.poll() is None:
            try:
                if p.stdin:
                    p.stdin.write("exit\\n"); p.stdin.flush(); p.wait(timeout=3)
            except Exception:
                stop_process(p)
    text = "".join(lines)
    outfile.write_text(text, encoding="utf-8", errors="replace")
    timelinefile.write_text("".join(timeline), encoding="utf-8", errors="replace")
    return text, done


def main():
    ap = argparse.ArgumentParser(description="WP24-M recovery guard boundary characterization")
    ap.add_argument("--mode", choices=("M1", "M2"), required=True)
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--delay-ms", type=int, help="Simulator delayed-response time from request RX")
    ap.add_argument("--build-dir", help="Default build_wp24_m1 or build_wp24_m2")
    ap.add_argument("--timeout", type=int, default=50)
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--flash", action="store_true")
    args = ap.parse_args()

    selector = 8 if args.mode == "M1" else 9
    sdkconfig = f"sdkconfig.wp24_{args.mode.lower()}"
    build_dir = args.build_dir or f"build_wp24_{args.mode.lower()}"
    # Initial estimates only. The runner always reports measured timing.
    delay_ms = args.delay_ms if args.delay_ms is not None else (1250 if args.mode == "M1" else 1450)
    if delay_ms < 0:
        raise SystemExit("--delay-ms must be non-negative")

    project = Path(__file__).resolve().parent.parent
    simulator = project / "tools" / "myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py"
    if not simulator.is_file():
        raise SystemExit(f"Missing simulator: {simulator}")
    try:
        import serial  # noqa
    except ImportError:
        raise SystemExit("pyserial missing: python -m pip install pyserial")
    if not (project / sdkconfig).is_file():
        raise SystemExit(f"Missing config {sdkconfig}; copy generated WP24-M configs into project root.")

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    root = project / "tools" / "test_results" / f"wp24_{args.mode.lower()}_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    try:
        if args.build:
            run_logged(idf_cmd(["-B", build_dir, "-D", f"SDKCONFIG={sdkconfig}", "reconfigure"]),
                       project, root / "reconfigure.log")
            run_logged(idf_cmd(["-B", build_dir, "-D", f"SDKCONFIG={sdkconfig}", "build"]),
                       project, root / "build.log")
        if args.build or args.flash:
            verify_config(project, build_dir, selector)
        if args.flash:
            run_logged(idf_cmd(["-B", build_dir, "-D", f"SDKCONFIG={sdkconfig}",
                                "-p", args.console_port, "flash"]), project, root / "flash.log")
        if not args.build and not args.flash:
            verify_config(project, build_dir, selector)
    except Exception as e:
        (root / "setup_error.txt").write_text(str(e), encoding="utf-8")
        print(f"SETUP FAILED: {e}")
        return 2

    print(f"WP24-{args.mode} RECOVERY-GUARD BOUNDARY TEST")
    print(f"Mode={args.mode}; selector={selector}; simulator delay={delay_ms} ms")
    print(f"Console={args.console_port}; RS485 simulator={args.rs485_port} @ {args.baud} 8E1")
    print("SAFETY: ESP RS485 to USB simulator only; never a live meter.")
    print("Delay is an initial estimate from simulator request RX. Use measured ESP timestamps to tune.")

    sim = None; sim_log = root / "simulator.log"; proc_file = None
    console = ""; monitor_done = False; error = None
    try:
        sim, sim_log, proc_log, proc_file = start_simulator(
            simulator, project, args.rs485_port, args.baud, delay_ms, root)
        console, monitor_done = capture_monitor(
            project, build_dir, sdkconfig, args.console_port, args.timeout,
            root / "esp32_console.log", root / "esp32_console_timeline.tsv")
    except Exception as e:
        error = str(e)
        (root / "runner_error.txt").write_text(error, encoding="utf-8")
        print(f"RUNNER ERROR: {error}")
    finally:
        stop_process(sim)
        if proc_file: proc_file.close()

    sim_text = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
    timeline = root / "esp32_console_timeline.tsv"
    timeline_text = timeline.read_text(encoding="utf-8", errors="replace") if timeline.exists() else ""

    def esp_time(pattern):
        for row in timeline_text.splitlines():
            if re.search(pattern, row):
                try: return dt.datetime.fromisoformat(row.split("\\t", 1)[0])
                except (ValueError, IndexError): return None
        return None

    recovery_start = esp_time(r"RS485 recovery requested; late-response guard=1000 ms")
    recovery_release = esp_time(r"RS485 recovery released; normal RX/TX resumed")
    sim_late_time = None
    for row in sim_text.splitlines():
        if "LATE_RESPONSE_SENT" in row:
            try: sim_late_time = dt.datetime.fromisoformat(row.split(" ", 1)[0])
            except ValueError: pass
            break

    delta_start_ms = ((sim_late_time - recovery_start).total_seconds() * 1000
                      if sim_late_time and recovery_start else None)
    delta_release_ms = ((sim_late_time - recovery_release).total_seconds() * 1000
                         if sim_late_time and recovery_release else None)
    timing_valid = bool(delta_start_ms is not None and delta_release_ms is not None)
    if args.mode == "M1":
        timing_classified = timing_valid and delta_start_ms >= 0 and delta_release_ms <= 0
        timing_label = "LATE_RESPONSE_DURING_GUARD"
    else:
        timing_classified = timing_valid and delta_release_ms > 0
        timing_label = "LATE_RESPONSE_AFTER_GUARD_RELEASE"

    # M2 can legitimately expose a stale response accepted during retry WAIT_RESPONSE.
    # The firmware reports the observed first request outcome and validates the
    # independent start=200 request regardless.
    summary_ok = bool(re.search(
        r"WP24_EXTENDED_SUMMARY: PASS mode=(RECOVERY_GUARD_PRE_BOUNDARY|POST_GUARD_STALE_RESPONSE_ACCEPTED_CHARACTERIZED|RECOVERY_GUARD_POST_BOUNDARY)",
        console))
    checks = {
        "WP24_M_mode_pass_marker": bool(re.search(rf"WP24-{args.mode} (PASS|OBSERVED):", console)),
        "extended_summary_pass": summary_ok,
        "self_test_summary_pass": "SELF_TEST_SUMMARY: PASS baseline FC/validation and selected WP2.4 extended test" in console,
        "monitor_completed": monitor_done and "MODE=SELF_TEST result=PASS" in console,
        "fault_sequence_delay_then_drop": bool(
            re.search(r"FAULT_SEQUENCE index=1 selected=delay", sim_text)
            and re.search(r"FAULT_SEQUENCE index=2 selected=drop", sim_text)
            and len(re.findall(r"FAULT drop response", sim_text)) == 1),
        "two_target_fc03_start100_requests": len(re.findall(r"FAULT_MATCH .*fc=0x03 .*start=0x0064", sim_text, re.I)) == 2,
        "late_response_timestamp_available": sim_late_time is not None,
        "late_response_timing_classified": timing_classified,
        "next_request_isolation": bool(re.search(r"next=.*\\[1200,1201\\]|next=\\[1200,1201\\]", console)) or
                                  "next=[1200,1201]" in console or "next=[1200,1201]" in console,
        "no_crash": not bool(re.search(r"Guru Meditation|stack overflow|assert failed|abort\\(\\)|Backtrace:", console, re.I)),
        "runner_no_error": error is None,
    }
    report = [f"WP24-{args.mode} RESULT: " + ("PASS" if all(checks.values()) else "FAIL"),
              "", f"Configured simulator delay: {delay_ms} ms",
              f"Recovery start: {recovery_start}", f"Recovery release: {recovery_release}",
              f"Late response sent: {sim_late_time}",
              f"Late response - guard start: {delta_start_ms} ms",
              f"Late response - guard release: {delta_release_ms} ms",
              f"Timing classification: {timing_label if timing_classified else 'NOT CLASSIFIED'}",
              "", "CHECKS:"]
    report += [f"{'PASS' if ok else 'FAIL':4} {name}" for name, ok in checks.items()]
    report += ["", f"Artifacts: {root}"]
    (root / "report.txt").write_text("\\n".join(report) + "\\n", encoding="utf-8")
    print("\\n" + "\\n".join(report))
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
