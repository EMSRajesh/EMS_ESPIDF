#!/usr/bin/env python3
"""
MyEMS automated RS-485/Modbus RTU test runner.

Runs the Python slave simulator on the USB-RS485 adapter (e.g. COM12),
captures the ESP32 UART log (e.g. COM11), prints all output, and writes a
timestamped combined log plus a JSON summary.

IMPORTANT:
- Close ESP-IDF Monitor before running; this script must own COM11.
- ESP32 firmware must already be flashed and running as Modbus RTU MASTER.
- USB-RS485 adapter A/B must be connected to the board's RS-485 A/B, with
  common reference/ground as required by the hardware. Do not connect to a live bus.
- Current firmware scheduler polls FC03. This script tests FC03 stability and
  fault/recovery behavior; it cannot prove FC01/02/04/05/06/15/16/23 until the
  firmware application actually submits those function codes.
"""
from __future__ import annotations
import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None

SUCCESS_RE = re.compile(r'\bFC\s*0?3\b.*\bOK\b|\bFC3\b.*\bOK\b', re.I)
FAIL_RE = re.compile(r'\bfailed status=|\bTIMEOUT\b|\bCRC_ERROR\b|\bPROTOCOL_ERROR\b', re.I)
RETRY_RE = re.compile(r'\bretry\s+\d+/\d+', re.I)
REG_RE = re.compile(r'register\[(\d+)\]=(\d+)')
REQUEST_RE = re.compile(r'scheduled\s+(\w+):\s+request=(\d+)\s+slave=(\d+)\s+FC(\d+)', re.I)

class Logger:
    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.Lock()
        self.fp = path.open("a", encoding="utf-8", buffering=1)
    def write(self, source: str, text: str):
        line = f"{datetime.now().astimezone().isoformat(timespec='milliseconds')} [{source}] {text.rstrip()}"
        with self.lock:
            print(line, flush=True)
            self.fp.write(line + "\n")
    def close(self):
        self.fp.close()

class TestRunner:
    def __init__(self, args):
        self.a = args
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.log_dir = Path(args.log_dir).resolve()
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.logger = Logger(self.log_dir / f"myems_modbus_test_{stamp}.log")
        self.summary_path = self.log_dir / f"myems_modbus_summary_{stamp}.json"
        self.sim = None
        self.stop_event = threading.Event()
        self.stats = {
            "esp_lines": 0, "fc03_successes": 0, "request_failures": 0,
            "retries": 0, "grid_successes": 0, "load_successes": 0,
            "slaves_seen": set(), "stages": [], "sim_errors": 0
        }
        self.current_stage = "startup"
        self.stage_lock = threading.Lock()
        self.esp_thread = None
        self.sim_thread = None

    def esp_reader(self):
        try:
            with serial.Serial(self.a.esp_port, self.a.esp_baud, timeout=0.25) as ser:
                self.logger.write("RUNNER", f"Opened ESP log port {self.a.esp_port} at {self.a.esp_baud}; ESP may reset when port opens.")
                # Capture boot banner as well as application logs.
                while not self.stop_event.is_set():
                    try:
                        raw = ser.readline()
                    except Exception as e:
                        self.logger.write("ESP-ERROR", str(e)); break
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip()
                    if not line:
                        continue
                    self.stats["esp_lines"] += 1
                    if SUCCESS_RE.search(line):
                        self.stats["fc03_successes"] += 1
                        if "grid_meter" in line: self.stats["grid_successes"] += 1
                        if "load_meter" in line: self.stats["load_successes"] += 1
                    if FAIL_RE.search(line): self.stats["request_failures"] += 1
                    if RETRY_RE.search(line): self.stats["retries"] += 1
                    m = REQUEST_RE.search(line)
                    if m: self.stats["slaves_seen"].add(int(m.group(3)))
                    self.logger.write("ESP", line)
        except Exception as e:
            self.logger.write("ESP-ERROR", f"Could not open {self.a.esp_port}: {e}")
            self.logger.write("ESP-ERROR", "Close ESP-IDF Monitor/other serial applications and retry.")
            self.stop_event.set()

    def simulator_reader(self, proc, stage):
        try:
            for line in iter(proc.stdout.readline, ''):
                if not line:
                    break
                if line.startswith("ERROR"):
                    self.stats["sim_errors"] += 1
                self.logger.write(f"SIM:{stage}", line.rstrip())
        except Exception as e:
            self.logger.write("SIM-ERROR", str(e))

    def stop_simulator(self):
        proc = self.sim
        self.sim = None
        if proc and proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=2)
        if self.sim_thread and self.sim_thread.is_alive():
            self.sim_thread.join(timeout=1)
        self.sim_thread = None

    def start_simulator(self, stage, fault="none", delay_ms=None, exception_code=None):
        self.stop_simulator()
        cmd = [
            sys.executable, str(Path(self.a.simulator).resolve()),
            "--port", self.a.rs485_port, "--slaves", self.a.slaves,
            "--baud", str(self.a.baud), "--parity", self.a.parity,
            "--stopbits", str(self.a.stopbits), "--fault", fault,
        ]
        if fault == "delay":
            cmd += ["--delay-ms", str(delay_ms if delay_ms is not None else self.a.delay_ms)]
        if fault == "exception":
            cmd += ["--exception-code", str(exception_code if exception_code is not None else 2)]
        if self.a.fault_every > 1 and fault not in ("none", "drop"):
            cmd += ["--fault-every", str(self.a.fault_every)]
        # Append simulator output to our own reader, so console + file both get it.
        self.logger.write("RUNNER", f"Starting simulator stage={stage}: {' '.join(cmd)}")
        self.sim = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, bufsize=1, encoding="utf-8", errors="replace")
        self.sim_thread = threading.Thread(target=self.simulator_reader, args=(self.sim, stage), daemon=True)
        self.sim_thread.start()
        time.sleep(self.a.sim_start_wait)
        if self.sim.poll() is not None:
            raise RuntimeError(f"Simulator exited during stage {stage}, return code {self.sim.returncode}. Check COM port and settings.")

    def run_stage(self, name, duration, fault="none", delay_ms=None, exception_code=None):
        if self.stop_event.is_set():
            raise RuntimeError("ESP serial reader stopped; check COM port ownership.")
        before = {k: self.stats[k] for k in ("fc03_successes", "request_failures", "retries", "grid_successes", "load_successes")}
        self.current_stage = name
        self.logger.write("RUNNER", f"STAGE START {name}: duration={duration}s fault={fault}")
        self.start_simulator(name, fault, delay_ms, exception_code)
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if self.stop_event.wait(min(0.5, max(0, end-time.monotonic()))):
                break
            if self.sim and self.sim.poll() is not None:
                raise RuntimeError(f"Simulator exited unexpectedly in stage {name}")
        self.stop_simulator()
        after = {k: self.stats[k] for k in before}
        delta = {k: after[k] - before[k] for k in before}
        result = {"stage": name, "fault": fault, "duration_seconds": duration, "counts": delta}
        self.stats["stages"].append(result)
        self.logger.write("RUNNER", f"STAGE END {name}: {json.dumps(result, sort_keys=True)}")
        return result

    def run(self):
        self.logger.write("RUNNER", f"Log file: {self.logger.path}")
        self.logger.write("RUNNER", f"Summary JSON: {self.summary_path}")
        self.logger.write("RUNNER", "Close ESP-IDF Monitor first. Starting ESP UART capture.")
        self.esp_thread = threading.Thread(target=self.esp_reader, daemon=True)
        self.esp_thread.start()
        # Wait until serial opened or errors surface.
        time.sleep(1.0)
        if self.stop_event.is_set():
            raise RuntimeError("Could not start ESP serial reader.")
        try:
            plan = [
                ("baseline_5min", self.a.baseline_seconds, "none", None, None),
                ("drop_response", self.a.fault_seconds, "drop", None, None),
                ("recovery_after_drop", self.a.recovery_seconds, "none", None, None),
                ("bad_crc", self.a.fault_seconds, "bad-crc", None, None),
                ("recovery_after_crc", self.a.recovery_seconds, "none", None, None),
                ("exception_response", self.a.fault_seconds, "exception", None, self.a.exception_code),
                ("recovery_after_exception", self.a.recovery_seconds, "none", None, None),
                ("delayed_response", self.a.fault_seconds, "delay", self.a.delay_ms, None),
                ("final_recovery", self.a.recovery_seconds, "none", None, None),
            ]
            for name, dur, fault, delay, ex in plan:
                self.run_stage(name, dur, fault, delay, ex)
                if self.stop_event.is_set(): break
        finally:
            self.stop_simulator()
            self.stop_event.set()
            if self.esp_thread and self.esp_thread.is_alive(): self.esp_thread.join(timeout=2)
            summary = {
                "started_at": self.logger.path.name,
                "esp_port": self.a.esp_port, "rs485_port": self.a.rs485_port,
                "baud": self.a.baud, "parity": self.a.parity, "stopbits": self.a.stopbits,
                "baseline_expected_seconds": self.a.baseline_seconds,
                "fault_seconds": self.a.fault_seconds, "recovery_seconds": self.a.recovery_seconds,
                "counts": {k: sorted(v) if isinstance(v, set) else v for k,v in self.stats.items() if k != "stages"},
                "stages": self.stats["stages"],
                "notes": [
                    "Current firmware shown in prior logs polls FC03; this runner does not prove other FC implementations.",
                    "ESP console parsing is heuristic; review raw log for exact results.",
                    "A recovery stage passes only if successful responses resume in the captured log; inspect stage counts."
                ]
            }
            self.summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
            self.logger.write("SUMMARY", json.dumps(summary, indent=2))
            self.logger.close()

def parse_args():
    p = argparse.ArgumentParser(description="Automated MyEMS Modbus RTU stability/fault/recovery test")
    p.add_argument("--esp-port", default="COM11", help="ESP32 UART console port (default COM11)")
    p.add_argument("--esp-baud", type=int, default=115200)
    p.add_argument("--rs485-port", default="COM12", help="USB-RS485 adapter port (default COM12)")
    p.add_argument("--simulator", default="usb_rs485_modbus_slave_sim_all_fc.py")
    p.add_argument("--slaves", default="1,2")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--parity", choices=("N","E","O"), default="E")
    p.add_argument("--stopbits", type=int, choices=(1,2), default=1)
    p.add_argument("--baseline-seconds", type=int, default=300, help="Normal-mode baseline duration; default 5 minutes")
    p.add_argument("--fault-seconds", type=int, default=25, help="Duration per fault stage")
    p.add_argument("--recovery-seconds", type=int, default=30, help="Normal-mode recovery duration after each fault")
    p.add_argument("--delay-ms", type=int, default=1500)
    p.add_argument("--exception-code", type=int, default=2)
    p.add_argument("--fault-every", type=int, default=0, help="For supported periodic faults, 0 means every request")
    p.add_argument("--sim-start-wait", type=float, default=1.0)
    p.add_argument("--log-dir", default="test_logs")
    a = p.parse_args()
    if a.baseline_seconds < 1 or a.fault_seconds < 1 or a.recovery_seconds < 1:
        p.error("stage durations must be >= 1 second")
    if not Path(a.simulator).exists():
        # Resolve relative to script directory as a convenience.
        candidate = Path(__file__).resolve().parent / a.simulator
        if candidate.exists(): a.simulator = str(candidate)
        else: p.error(f"Simulator not found: {a.simulator}")
    return a

def main():
    if serial is None:
        print("ERROR: pyserial not installed. Run: py -m pip install pyserial", file=sys.stderr)
        return 2
    args = parse_args()
    runner = TestRunner(args)
    try:
        runner.run()
    except KeyboardInterrupt:
        runner.logger.write("RUNNER", "Interrupted by user")
        runner.stop_simulator()
        runner.stop_event.set()
        runner.logger.close()
        return 130
    except Exception as e:
        runner.logger.write("RUNNER-ERROR", str(e))
        runner.stop_simulator()
        runner.stop_event.set()
        runner.logger.close()
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
