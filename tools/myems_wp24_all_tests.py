#!/usr/bin/env python3
"""Run the unified WP24 A-I campaign on one dedicated firmware image.

Build/flash separately with tools/build_wp24_all.ps1. This runner never builds
or flashes. It starts one simulator with a deterministic FC03/address-100 fault
schedule, opens the ESP-IDF monitor (which resets the DUT), and evaluates both
firmware assertions and simulator fault-schedule evidence.
"""
from __future__ import annotations
import argparse, csv, datetime as dt, json, os, queue, re, subprocess, sys, threading, time
from pathlib import Path

FAULTS = ["drop", "delay", "bad-crc", "short-response", "wrong-slave", "exception", "drop", "none", "drop", "drop"]
EXPECTED_MARKERS = [f"WP24_{c}_PASS" for c in "ABCDEFGHI"]


def idf_cmd(args: list[str]) -> list[str]:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH is not set. Activate ESP-IDF v6.1 first.")
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
    py = next((x for x in candidates if x.is_file()), None)
    if py is None:
        raise RuntimeError("ESP-IDF Python interpreter not found")
    return [str(py), str(idf_py), *args]


def verify_wp24_build(project: Path, build_dir: str) -> None:
    """Fail closed unless the existing build is configured for WP24 A-I."""

    cache = project / build_dir / "CMakeCache.txt"

    if not cache.is_file():
        raise RuntimeError(
            f"WP24 build cache not found: {cache}. "
            "Run .\\tools\\build_wp24_all.ps1 first."
        )

    cache_text = cache.read_text(
        encoding="utf-8",
        errors="replace"
    )

    sdkconfig_match = re.search(
        r"^SDKCONFIG:[^=]*=(.*?)\s*$",
        cache_text,
        re.I | re.M
    )

    if not sdkconfig_match:
        raise RuntimeError(
            "SDKCONFIG entry not found in CMakeCache."
        )

    selected_config = sdkconfig_match.group(1).strip().strip('"')

    selected_filename = (
        selected_config.replace("\\", "/").rsplit("/", 1)[-1]
    )

    if selected_filename.lower() != "sdkconfig.wp24_all":
        raise RuntimeError(
            f"Wrong build configuration: SDKCONFIG={selected_config}. "
            "Expected sdkconfig.wp24_all."
        )

    config_header = project / build_dir / "config" / "sdkconfig.h"

    if not config_header.is_file():
        raise RuntimeError(
            f"Generated sdkconfig header not found: {config_header}"
        )

    header = config_header.read_text(
        encoding="utf-8",
        errors="replace"
    )

    required = [
        r"#define CONFIG_MYEMS_MODBUS_SELF_TEST 1",
        r"#define CONFIG_MYEMS_WP24_FAULT_INJECTION 1",
        r"#define CONFIG_MYEMS_WP24_EXTENDED_TEST 4",
    ]

    for pattern in required:
        if not re.search(pattern, header):
            raise RuntimeError(
                f"WP24 build configuration check failed: {pattern}"
            )


def stop_process(proc: subprocess.Popen | None) -> None:
    if not proc or proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait(timeout=2)


def capture_monitor(project: Path, build_dir: str, port: str, timeout_s: int, output: Path) -> tuple[str, bool]:
    cmd = idf_cmd([
        "-B", build_dir,
        "-D", "SDKCONFIG=sdkconfig.wp24_all",
        "-p", port,
        "monitor"
    ])
    print("Starting ESP-IDF monitor (this resets the ESP32): " + subprocess.list2cmdline(cmd), flush=True)
    proc = subprocess.Popen(cmd, cwd=project, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1)
    q: queue.Queue[str | None] = queue.Queue()
    lines: list[str] = []
    def reader() -> None:
        try:
            assert proc.stdout is not None
            for line in iter(proc.stdout.readline, ""):
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
                if proc.poll() is not None:
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
        if proc.poll() is None:
            try:
                if proc.stdin:
                    proc.stdin.write("exit\n"); proc.stdin.flush(); proc.wait(timeout=3)
            except Exception:
                stop_process(proc)
    text = "".join(lines)
    output.write_text(text, encoding="utf-8", errors="replace")
    return text, done


def main() -> int:
    ap = argparse.ArgumentParser(description="MyEMS unified WP24 A-I test campaign")
    ap.add_argument("--esp-port", default="COM11")
    ap.add_argument("--rs485-port", default="COM12")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--parity", choices=["N", "E", "O"], default="E")
    ap.add_argument("--stopbits", type=int, choices=[1, 2], default=1)
    ap.add_argument("--build-dir", default="build_wp24_all")
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()
    project = Path(__file__).resolve().parent.parent
    simulator = project / "tools" / "myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py"
    if not simulator.is_file():
        print(f"Missing simulator: {simulator}", file=sys.stderr); return 2
    try:
        import serial  # noqa: F401
        verify_wp24_build(Path(__file__).resolve().parent.parent, args.build_dir)
        monitor_cmd = idf_cmd(["-B", args.build_dir, "-D", "SDKCONFIG=sdkconfig.wp24_all",
                               "-p", args.esp_port, "monitor"])
    except Exception as exc:
        print(f"PRECHECK FAIL: {exc}", file=sys.stderr); return 2
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    results_dir = project / "tools" / "test_results" / f"wp24_all_{stamp}"
    results_dir.mkdir(parents=True, exist_ok=False)
    sim_log = results_dir / "simulator.log"
    sim_stdout = (results_dir / "simulator_process.log").open("w", encoding="utf-8")
    sim_cmd = [sys.executable, str(simulator), "--port", args.rs485_port, "--slaves", "1",
               "--baud", str(args.baud), "--parity", args.parity, "--stopbits", str(args.stopbits),
               "--fault-sequence", ",".join(FAULTS), "--fault-fc", "3", "--fault-start", "100",
               "--delay-ms", "1000", "--reset-on-fc01", "--log-file", str(sim_log)]
    print("MYEMS WP24 A-I CAMPAIGN")
    print("SAFETY: connect RS485 only to USB simulator; self-test writes simulator registers/coils 1..8.")
    print("Firmware is not built or flashed by this runner. Verified build: sdkconfig.wp24_all / build_wp24_all.")
    print("Simulator fault schedule: " + ",".join(FAULTS))
    sim = subprocess.Popen(sim_cmd, cwd=project, stdout=sim_stdout, stderr=subprocess.STDOUT)
    console = ""; monitor_done = False; error = None
    try:
        time.sleep(1.5)
        if sim.poll() is not None:
            raise RuntimeError("Simulator exited during startup; see simulator_process.log")
        console, monitor_done = capture_monitor(project, args.build_dir, args.esp_port,
                                                args.timeout, results_dir / "esp32_console.log")
    except Exception as exc:
        error = str(exc)
    finally:
        stop_process(sim); sim_stdout.close()
    sim_text = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
    crash = bool(re.search(r"Guru Meditation|stack overflow|assert failed|abort\(\)|Backtrace:", console, re.I))
    markers = {c: (f"WP24_{c}_PASS" in console) for c in "ABCDEFGHI"}
    seq_entries = re.findall(r"FAULT_SEQUENCE index=(\d+) selected=([a-z-]+)", sim_text)
    seq_ok = len(seq_entries) == len(FAULTS) and [x[1] for x in seq_entries] == FAULTS
    summary_ok = "WP24_A_TO_I_SUMMARY: PASS" in console and "SELF_TEST_SUMMARY: PASS WP24 A-I campaign" in console and "MODE=SELF_TEST result=PASS" in console
    # Retry ordering: recovery must be released before the master schedules the retry.
    g_start = console.find("WP24_CASE_START G")
    release_pos = console.find("RS485 recovery released; normal RX/TX resumed", g_start) if g_start >= 0 else -1
    retry_schedule_pos = console.find("recovery complete; retry scheduled", release_pos) if release_pos >= 0 else -1
    retry_order_ok = g_start >= 0 and release_pos > g_start and retry_schedule_pos > release_pos
    results = [{"test": f"WP24-{c}", "result": "PASS" if markers[c] else "FAIL"} for c in "ABCDEFGHI"]
    overall = bool(not error and monitor_done and all(markers.values()) and seq_ok and summary_ok and retry_order_ok and not crash)
    report = {
        "suite": "MyEMS WP24 A-I unified campaign", "result": "PASS" if overall else "FAIL",
        "firmware_config": "sdkconfig.wp24_all", "build_dir": args.build_dir,
        "esp_port": args.esp_port, "rs485_port": args.rs485_port, "fault_sequence": FAULTS,
        "simulator_sequence_verified": seq_ok, "firmware_summary_verified": summary_ok,
        "retry_recovery_release_order_verified": retry_order_ok, "monitor_completed": monitor_done,
        "crash_detected": crash, "runner_error": error, "cases": results,
        "limitations": ["WP24-H is software-injected RS485_EVT_RX_LOSS, not physical UART ring-buffer overflow.",
                        "WP24-I forces recovery activity in software; it is not an electrical-noise immunity test."],
    }
    (results_dir / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    with (results_dir / "summary.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["test", "result"]); w.writeheader(); w.writerows(results)
    text = ["MYEMS WP24 A-I CAMPAIGN: " + report["result"],
            f"EXECUTED: {sum(markers.values())}/9", f"SIMULATOR SEQUENCE: {'PASS' if seq_ok else 'FAIL'}",
            f"RETRY ORDER: {'PASS' if retry_order_ok else 'FAIL'}", f"FIRMWARE SUMMARY: {'PASS' if summary_ok else 'FAIL'}",
            "", *[f"{r['test']}: {r['result']}" for r in results], "",
            "H scope: software RX_LOSS injection only.", "I scope: software-forced recovery activity only."]
    (results_dir / "summary.txt").write_text("\n".join(text), encoding="utf-8")
    print("\n" + "\n".join(text)); print(f"Reports: {results_dir}")
    return 0 if overall else 1

if __name__ == "__main__":
    raise SystemExit(main())
