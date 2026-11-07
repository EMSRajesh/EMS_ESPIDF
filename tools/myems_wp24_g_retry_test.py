#!/usr/bin/env python3
"""MyEMS WP24-G retry-after-recovery test runner (Windows / ESP-IDF v6.1).

Place this file in <project>/tools/. It uses sdkconfig.wp24_g and build_wp24_g.
Build and flash are opt-in via --build and --flash. RS485 must connect only to
USB simulator, never to a live meter.
"""
from __future__ import annotations
import argparse, datetime as dt, os, queue, re, subprocess, sys, threading, time
from pathlib import Path


def idf_cmd(args):
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH is not set. Open the ESP-IDF v6.1 PowerShell environment.")
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
    print("\n> " + subprocess.list2cmdline(map(str, cmd)), flush=True)
    p = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, errors="replace")
    logfile.write_text(p.stdout or "", encoding="utf-8", errors="replace")
    print((p.stdout or "")[-6000:], end="\n", flush=True)
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


def verify_g_build(project, build_dir):
    header = project / build_dir / "config" / "sdkconfig.h"
    if not header.is_file():
        raise RuntimeError(f"Generated sdkconfig header missing: {header}. Build WP24-G first.")
    cfg = header.read_text(encoding="utf-8", errors="replace")
    expected = [
        r"#define CONFIG_MYEMS_MODBUS_SELF_TEST 1",
        r"#define CONFIG_MYEMS_WP24_FAULT_INJECTION 1",
        r"#define CONFIG_MYEMS_WP24_EXTENDED_TEST 1",
    ]
    for pattern in expected:
        if not re.search(pattern, cfg):
            raise RuntimeError(f"WP24-G build config mismatch: expected {pattern}. Use sdkconfig.wp24_g.")


def start_simulator(simulator, case_dir, port, baud):
    sim_log = case_dir / "simulator.log"
    proc_log = case_dir / "simulator_process.log"
    cmd = [sys.executable, str(simulator), "--port", port, "--slaves", "1",
           "--baud", str(baud), "--parity", "E", "--stopbits", "1",
           "--fault", "drop", "--fault-fc", "3", "--fault-start", "100",
           "--fault-once", "--reset-on-fc01", "--log-file", str(sim_log)]
    f = proc_log.open("w", encoding="utf-8", errors="replace")
    p = subprocess.Popen(cmd, cwd=simulator.parent.parent,
                         stdout=f, stderr=subprocess.STDOUT)
    start = time.monotonic(); deadline = start + 8.0
    while time.monotonic() < deadline:
        if p.poll() is not None:
            f.close()
            detail = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
            raise RuntimeError(f"Simulator exited during startup: {detail or proc_log}")
        detail = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
        if "ERROR" in detail:
            stop_process(p); f.close()
            raise RuntimeError(f"Simulator COM initialization failed: {detail}")
        if "START port=" in detail and time.monotonic() - start >= 1.0:
            return p, sim_log, proc_log, f
        time.sleep(0.2)
    stop_process(p); f.close()
    raise RuntimeError(f"Simulator startup timeout; see {sim_log} / {proc_log}")


def capture_monitor(project, build_dir, console_port, timeout_s, outfile):
    cmd = idf_cmd(["-B", build_dir, "-D", "SDKCONFIG=sdkconfig.wp24_g",
                   "-p", console_port, "monitor"])
    print("\nStarting ESP-IDF monitor (resets ESP32): " + subprocess.list2cmdline(cmd), flush=True)
    p = subprocess.Popen(cmd, cwd=project, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                         errors="replace", bufsize=1)
    q = queue.Queue(); lines = []
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
                line = q.get(timeout=0.25)
            except queue.Empty:
                if p.poll() is not None:
                    break
                continue
            if line is None:
                break
            lines.append(line); print(line, end="", flush=True)
            if "MODE=SELF_TEST result=" in line:
                done = True
                time.sleep(0.4)
                break
    finally:
        if p.poll() is None:
            try:
                if p.stdin:
                    p.stdin.write("exit\n"); p.stdin.flush(); p.wait(timeout=3)
            except Exception:
                stop_process(p)
    text = "".join(lines)
    outfile.write_text(text, encoding="utf-8", errors="replace")
    return text, done


def main():
    ap = argparse.ArgumentParser(description="MyEMS WP24-G retry-after-recovery test")
    ap.add_argument("--console-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--build-dir", default="build_wp24_g")
    ap.add_argument("--timeout", type=int, default=45)
    ap.add_argument("--build", action="store_true", help="Explicitly build WP24-G firmware")
    ap.add_argument("--flash", action="store_true", help="Explicitly flash WP24-G firmware")
    args = ap.parse_args()

    project = Path(__file__).resolve().parent.parent
    simulator = project / "tools" / "myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py"
    if not simulator.is_file():
        raise SystemExit(f"Missing simulator: {simulator}")
    try:
        import serial  # noqa: F401
    except ImportError:
        raise SystemExit("pyserial missing: python -m pip install pyserial")

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    root = project / "tools" / "test_results" / f"wp24_g_{stamp}"
    root.mkdir(parents=True, exist_ok=False)
    sdkconfig = project / "sdkconfig.wp24_g"
    if not sdkconfig.is_file():
        raise SystemExit(f"Missing config: {sdkconfig}")

    try:
        if args.build:
            run_logged(idf_cmd(["-B", args.build_dir, "-D", "SDKCONFIG=sdkconfig.wp24_g", "build"]),
                       project, root / "build.log")
        if args.build or args.flash:
            verify_g_build(project, args.build_dir)
        if args.flash:
            run_logged(idf_cmd(["-B", args.build_dir, "-D", "SDKCONFIG=sdkconfig.wp24_g",
                                "-p", args.console_port, "flash"]), project, root / "flash.log")
        if not args.build and not args.flash:
            verify_g_build(project, args.build_dir)
    except Exception as exc:
        (root / "setup_error.txt").write_text(str(exc), encoding="utf-8")
        print(f"SETUP FAILED: {exc}")
        return 2

    print("MYEMS WP24-G RETRY-AFTER-RECOVERY TEST")
    print(f"Project: {project}\nBuild: {args.build_dir} / sdkconfig.wp24_g")
    print(f"Console: {args.console_port} @115200; RS485 simulator: {args.rs485_port} @ {args.baud} 8E1")
    print("SAFETY: connect ESP32 RS485 only to the USB simulator, never a live meter.")
    print("This test uses one-shot drop on FC03/start=100; retry should receive [1100,1101].")

    sim = None; sim_log = root / "simulator.log"; proc_log = root / "simulator_process.log"; proc_file = None
    console = ""; monitor_done = False; error = None
    try:
        sim, sim_log, proc_log, proc_file = start_simulator(simulator, root, args.rs485_port, args.baud)
        print("Simulator ready; starting DUT self-test.", flush=True)
        console, monitor_done = capture_monitor(project, args.build_dir, args.console_port,
                                                args.timeout, root / "esp32_console.log")
    except Exception as exc:
        error = str(exc)
        (root / "runner_error.txt").write_text(error, encoding="utf-8")
        print(f"RUNNER ERROR: {error}")
    finally:
        stop_process(sim)
        if proc_file:
            proc_file.close()
        time.sleep(1.5)

    sim_text = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
    checks = {
        "WP24_G_PASS_marker": bool(re.search(r"WP24-G PASS: retry_count=1; successful retry returned \[1100,1101\]", console)),
        "extended_summary_pass": "WP24_EXTENDED_SUMMARY: PASS mode=RETRY" in console,
        "self_test_summary_pass": "SELF_TEST_SUMMARY: PASS baseline FC/validation and selected WP2.4 extended test" in console,
        "monitor_completed": monitor_done and "MODE=SELF_TEST result=PASS" in console,
        "one_shot_drop_logged": ("FAULT applied=drop" in sim_text) or ("FAULT drop response" in sim_text),
        "no_crash": not bool(re.search(r"Guru Meditation|stack overflow|assert failed|abort\(\)|Backtrace:", console, re.I)),
    }
    passed = not error and all(checks.values())
    report = ["WP24-G RESULT: " + ("PASS" if passed else "FAIL"), "", "CHECKS:"]
    report += [f"{'PASS' if ok else 'FAIL':4} {name}" for name, ok in checks.items()]
    report += ["", f"Artifacts: {root}"]
    (root / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n" + "\n".join(report))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
