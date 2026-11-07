#!/usr/bin/env python3
"""MyEMS WP2.4 recovery-path test runner (Windows / ESP-IDF v6.1).

Runs the existing ESP32 self-test against the targeted USB-RS485 simulator.
Each case faults only the WP2-A request (slave 1, FC03, start 100), so the
nine function-code checks and validation checks execute before fault injection.

This runner does not modify, build, or flash firmware unless explicitly asked.
It independently evaluates WP2-A's exact completion status, recovery evidence,
and WP2-B's exact [1200, 1201] result. Firmware WP2-A assertion accepts the
supported non-OK fault classes; this runner remains responsible for checking
the exact status expected for each selected simulator fault.

Not covered by this host-only script: retry-enabled request (current WP2-A has
max_retries=0), UART RX overflow/RX_LOSS injection, forced recovery-operation
timeout, and sustained-noise/bus-never-idle testing. Those need dedicated DUT
hooks or a more capable electrical/firmware fault injector.
"""
from __future__ import annotations
import argparse, csv, datetime as dt, json, os, queue, re, subprocess, sys
import threading, time
from pathlib import Path

CASES = [
    # label, simulator fault, expected A status, recovery expected, delay_ms
    ("WP24_A_drop_timeout", "drop", 1, True, 0),
    ("WP24_B_late_response_quarantine", "delay", 1, True, 1000),
    ("WP24_C_bad_crc_recovery", "bad-crc", 2, True, 0),
    ("WP24_D_short_frame_recovery", "short-response", None, True, 0),
    ("WP24_E_wrong_slave_recovery", "wrong-slave", 1, True, 0),
    ("WP24_F_valid_exception_no_recovery", "exception", 4, False, 0),
]

def timestamp(): return dt.datetime.now().strftime("%Y%m%d_%H%M%S")

def idf_cmd(args):
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path: raise RuntimeError("IDF_PATH is not set. Use the ESP-IDF v6.1 PowerShell environment.")
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.is_file(): raise RuntimeError(f"idf.py not found: {idf_py}")
    candidates=[]
    env_path=os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path: candidates.append(Path(env_path)/"Scripts"/"python.exe")
    tools_path=os.environ.get("IDF_TOOLS_PATH")
    if tools_path: candidates.extend(Path(tools_path).glob("python/*/venv/Scripts/python.exe"))
    candidates.append(Path(sys.executable))
    py=next((p for p in candidates if p.is_file()),None)
    if py is None: raise RuntimeError("Could not locate ESP-IDF Python interpreter")
    return [str(py),str(idf_py),*map(str,args)]

def run_logged(cmd,cwd,path):
    print("\n> "+subprocess.list2cmdline(map(str,cmd)),flush=True)
    p=subprocess.run(cmd,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,errors="replace")
    path.write_text(p.stdout or "",encoding="utf-8",errors="replace")
    print((p.stdout or "")[-5000:],end="\n",flush=True)
    if p.returncode: raise RuntimeError(f"Command failed ({p.returncode}); see {path}")

def stop_process(p):
    if not p or p.poll() is not None: return
    if os.name=="nt": subprocess.run(["taskkill","/PID",str(p.pid),"/T","/F"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    else: p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=2)

def start_simulator(simulator,case_dir,port,baud,fault,delay_ms):
    sim_log=case_dir/"simulator.log"
    proc_log=case_dir/"simulator_process.log"
    cmd=[sys.executable,str(simulator),"--port",port,"--slaves","1","--baud",str(baud),"--parity","E","--stopbits","1",
         "--fault",fault,"--fault-fc","3","--fault-start","100","--fault-once","--reset-on-fc01","--log-file",str(sim_log)]
    if fault=="delay": cmd += ["--delay-ms",str(delay_ms)]
    f=proc_log.open("w",encoding="utf-8",errors="replace")
    start_time = time.monotonic()
    p=subprocess.Popen(cmd,cwd=simulator.parent.parent,stdout=f,stderr=subprocess.STDOUT)
    # Wait for the simulator to initialize its serial port. A live process alone
    # is not sufficient: pyserial can fail to open a busy/missing COM port.
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if p.poll() is not None:
            f.close()
            detail = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
            raise RuntimeError(f"Simulator exited during startup; {detail or 'see ' + str(proc_log)}")
        detail = sim_log.read_text(encoding="utf-8", errors="replace") if sim_log.exists() else ""
        if "ERROR" in detail:
            stop_process(p); f.close()
            raise RuntimeError(f"Simulator could not initialize COM port; {detail}")
        if "START port=" in detail and time.monotonic() - start_time >= 1.0:
            break
        time.sleep(0.2)
    else:
        stop_process(p); f.close()
        raise RuntimeError(f"Simulator startup timed out; see {sim_log} and {proc_log}")
    return p,sim_log,proc_log,f

def capture_monitor(project,build_dir,console_port,timeout_s,outfile):
    cmd=idf_cmd(["-B",build_dir,"-p",console_port,"monitor"])
    print("\nStarting monitor: "+subprocess.list2cmdline(cmd),flush=True)
    p=subprocess.Popen(cmd,cwd=project,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                       text=True,encoding="utf-8",errors="replace",bufsize=1)
    q=queue.Queue(); lines=[]
    def reader():
        try:
            for line in iter(p.stdout.readline,""): q.put(line)
        finally: q.put(None)
    threading.Thread(target=reader,daemon=True).start()
    start=time.monotonic(); summary_seen=False
    try:
        while time.monotonic()-start<timeout_s:
            try: line=q.get(timeout=.25)
            except queue.Empty:
                if p.poll() is not None: break
                continue
            if line is None: break
            lines.append(line); print(line,end="",flush=True)
            if "MODE=SELF_TEST result=" in line:
                summary_seen=True; time.sleep(.35); break
    finally:
        if p.poll() is None:
            try:
                if p.stdin: p.stdin.write("exit\n"); p.stdin.flush(); p.wait(timeout=3)
            except Exception: stop_process(p)
    text="".join(lines); outfile.write_text(text,encoding="utf-8",errors="replace")
    return text,summary_seen

def evaluate(case,console,sim_text,summary_seen):
    label,fault,expected,recovery_expected,delay_ms=case
    case_letter = label.split("_")[1]
    selected_marker = re.search(
        rf"WP24_{case_letter}_PASS A_status=(\d+) B=\[(\d+),(\d+)\] recovery=(completed|not-needed)",
        console)
    # New isolated WP24 firmware emits WP24_X_PASS; older WP2 firmware emitted
    # WP2-A/WP2-B markers. Support both, but do not let a missing marker pass.
    a=re.search(r"WP2-A completion:.*?status=(\d+)",console)
    b=re.search(r"WP2-B request B result: status=(\d+) registers=\[(\d+),(\d+)\] expected=\[1200,1201\]",console)
    recovery_requested="RS485 recovery requested" in console
    recovery_active="RS485 recovery active" in console
    recovery_released="RS485 recovery released" in console
    crash=bool(re.search(r"Guru Meditation|stack overflow|assert failed|abort\(\)|Backtrace:",console,re.I))
    fc_passes=len(re.findall(r"FC(?:01|02|03|04|05|06|15|16|23).*?: PASS",console))
    validations=len(re.findall(r"VALIDATE .*?: PASS",console))
    fault_seen=("FAULT_MATCH" in sim_text and f"FAULT applied={fault}" in sim_text) or (fault=="drop" and "FAULT drop response" in sim_text)
    if selected_marker:
        a_status=int(selected_marker.group(1))
        b_ok=(int(selected_marker.group(2)),int(selected_marker.group(3)))==(1200,1201)
        marker_recovery=selected_marker.group(4)
        marker_recovery_ok=(marker_recovery=="completed") if recovery_expected else (marker_recovery=="not-needed")
    else:
        a_status=int(a.group(1)) if a else None
        b_ok=bool(b and int(b.group(1))==0 and (int(b.group(2)),int(b.group(3)))==(1200,1201))
        marker_recovery_ok=True
    if expected is None: a_ok=(a_status in (1,2,3,5))
    else: a_ok=(a_status==expected)
    recovery_ok=(recovery_requested and recovery_active and recovery_released) if recovery_expected else (not recovery_requested)
    firmware_selftest_pass = "SELF_TEST_SUMMARY: PASS" in console
    # The new WP24 isolated-case assertion itself proves B and recovery counters.
    if selected_marker:
        recovery_ok = recovery_ok and marker_recovery_ok
    passed=bool(fault_seen and a_ok and b_ok and recovery_ok and not crash and
                fc_passes>=9 and validations==4 and summary_seen and firmware_selftest_pass and
                (selected_marker is not None or (a is not None and b is not None)))
    return {"test":label,"fault":fault,"result":"PASS" if passed else "FAIL","A_status":a_status,
            "A_status_expected":expected if expected is not None else "non-OK failure status",
            "B_correct_1200_1201":b_ok,"recovery_expected":recovery_expected,
            "recovery_requested":recovery_requested,"recovery_active":recovery_active,
            "recovery_released":recovery_released,"fault_injected":fault_seen,
            "function_code_pass_lines":fc_passes,"validation_passes":validations,
            "firmware_summary_pass":firmware_selftest_pass,
            "monitor_summary_seen":summary_seen,"crash":crash,
            "firmware_case_marker":bool(selected_marker),
            "note":"Isolated WP24 firmware marker is checked when present; legacy WP2 markers are also supported."}

def verify_case_build(project, build_dir, case_label):
    case_letter = case_label.split("_")[1]
    case_value = ord(case_letter) - ord("A") + 1
    header = project / build_dir / "config" / "sdkconfig.h"
    if not header.is_file():
        raise RuntimeError(f"Generated sdkconfig header missing: {header}. Build isolated case firmware first.")
    text = header.read_text(encoding="utf-8", errors="replace")
    required = [
        r"#define CONFIG_MYEMS_MODBUS_SELF_TEST 1",
        r"#define CONFIG_MYEMS_WP24_FAULT_INJECTION 1",
        r"#define CONFIG_MYEMS_WP24_EXTENDED_TEST 4",
        rf"#define CONFIG_MYEMS_WP24_CASE_SELECTION {case_value}",
    ]
    for pattern in required:
        if not re.search(pattern, text):
            raise RuntimeError(f"Isolated WP24 build configuration mismatch: expected {pattern}. Rebuild using tools\build_wp24_case.ps1 -Case {case_letter}")

def main():
    ap=argparse.ArgumentParser(description="MyEMS WP2.4 recovery-path test runner")
    ap.add_argument("--console-port",default="COM11"); ap.add_argument("--rs485-port",default="COM12")
    ap.add_argument("--baud",type=int,default=9600); ap.add_argument("--build-dir",default="build_wp24_case")
    ap.add_argument("--timeout",type=int,default=45); ap.add_argument("--release-wait",type=float,default=1.5)
    ap.add_argument("--only",choices=[c[0] for c in CASES],help="Run one isolated WP24 case; firmware must be built with matching WP24_CASE_SELECTION"); ap.add_argument("--build",action="store_true")
    ap.add_argument("--flash",action="store_true",help="Explicitly flash self-test firmware before test cases")
    ap.add_argument("--skip",nargs="*",choices=["retry","rx_loss","recovery_timeout"],default=["retry","rx_loss","recovery_timeout"])
    ap.add_argument("--list",action="store_true",help="List WP24-A through WP24-F and exit")
    args=ap.parse_args()
    if args.list:
        print("WP24-A through WP24-F:")
        for case in CASES: print("  " + case[0])
        return 0
    project=Path(__file__).resolve().parent.parent
    simulator=project/"tools"/"myems_usb_rs485_modbus_slave_sim_all_fc_reset_targeted.py"
    if not simulator.is_file(): raise SystemExit(f"Missing simulator: {simulator}")
    try: import serial  # noqa
    except ImportError: raise SystemExit("pyserial missing: python -m pip install pyserial")
    run_id=timestamp(); root=project/"tools"/"test_results"/f"wp24_{run_id}"; root.mkdir(parents=True,exist_ok=True)
    print("MYEMS WP2.4 RECOVERY TEST SUITE")
    print(f"Project: {project}\nConsole: {args.console_port} @115200\nRS485: {args.rs485_port}, {args.baud} 8E1\nResults: {root}")
    print("SAFETY: connect ESP32 RS485 only to the USB simulator. Existing self-test performs write operations to simulator registers/coils 1..8.")
    print("No build/flash unless --build and/or --flash is explicitly supplied.")
    sdkconfig_name = "sdkconfig.wp24_case" if args.only else "sdkconfig.selftest"
    try:
        if args.build: run_logged(idf_cmd(["-B",args.build_dir,"-D",f"SDKCONFIG={project/sdkconfig_name}","build"]),project,root/"build.log")
        if args.flash: run_logged(idf_cmd(["-B",args.build_dir,"-D",f"SDKCONFIG={project/sdkconfig_name}","-p",args.console_port,"flash"]),project,root/"flash.log")
        if args.only: verify_case_build(project, args.build_dir, args.only)
    except Exception as e:
        (root/"setup_error.txt").write_text(str(e),encoding="utf-8"); print(f"SETUP FAILED: {e}"); return 2
    selected=[c for c in CASES if not args.only or c[0] == args.only]
    results=[]
    for i,case in enumerate(selected,1):
        label,fault,expected,recovery_expected,delay_ms=case; d=root/f"{i:02d}_{label}"; d.mkdir()
        sim=None; sim_log=d/"simulator.log"; sim_proc_log=d/"simulator_process.log"; proc_file=None; console=""; sim_text=""; summary=False
        try:
            sim,sim_log,sim_proc_log,proc_file=start_simulator(simulator,d,args.rs485_port,args.baud,fault,delay_ms)
            print(f"\n=== {label}: simulator running; starting DUT self-test ===",flush=True)
            console,summary=capture_monitor(project,args.build_dir,args.console_port,args.timeout,d/"esp32_console.log")
        except Exception as e:
            (d/"runner_error.txt").write_text(str(e),encoding="utf-8"); print(f"RUNNER ERROR {label}: {e}")
        finally:
            stop_process(sim)
            if proc_file: proc_file.close()
            time.sleep(args.release_wait)
            if sim_log.exists(): sim_text=sim_log.read_text(encoding="utf-8",errors="replace")
        if (d/"runner_error.txt").exists(): r={"test":label,"fault":fault,"result":"NOT EXECUTED","details":(d/"runner_error.txt").read_text(encoding="utf-8")}
        else: r=evaluate(case,console,sim_text,summary)
        results.append(r); print(f"\nRESULT {label}: {r['result']} | A={r.get('A_status')} B_correct={r.get('B_correct_1200_1201')} recovery={r.get('recovery_requested')}/{r.get('recovery_released')}",flush=True)
    blocked=[{"test":f"WP24_{x.upper()}","result":"NOT EXECUTED","reason":{
        "retry":"Current WP2-A test request sets max_retries=0; retry scenario needs a test firmware option with max_retries>=1.",
        "rx_loss":"Current simulator cannot force UART driver RX ring overflow or a genuine RS485_EVT_RX_LOSS; requires dedicated noise/overflow injector or DUT test hook.",
        "recovery_timeout":"Current simulator cannot keep RX activity continuous or force transport recovery failure; requires a transport test hook or controlled noise source."
    }[x]} for x in args.skip]
    counts={s:sum(r["result"]==s for r in results) for s in ("PASS","FAIL","NOT EXECUTED")}
    if args.only:
        a_to_f = "SELECTED_CASE_PASS" if len(results)==1 and counts["PASS"]==1 else "SELECTED_CASE_FAIL"
        overall = "PASS" if a_to_f == "SELECTED_CASE_PASS" else "FAIL"
    else:
        a_to_f = "PASS" if len(results)==6 and counts["PASS"]==6 else "FAIL" if counts["FAIL"] else "PARTIAL"
        overall="PASS" if a_to_f=="PASS" and not blocked else "PARTIAL" if counts["FAIL"]==0 else "FAIL"
    extended = "PARTIAL" if blocked else "NOT ASSESSED"
    summary={"suite":"MyEMS WP2.4 recovery-path suite","overall":("WP24_"+args.only+"_"+overall if args.only else "WP24_A_TO_F_"+a_to_f),
             "wp24_a_to_f_status":a_to_f,"extended_recovery_coverage":extended,"started":run_id,
             "console_port":args.console_port,"rs485_port":args.rs485_port,"results":results,"not_executed_capabilities":blocked,
             "counts":counts,"qualification":"A PASS means the selected simulator fault was injected, A had the expected non-OK status, required recovery behavior was observed (or correctly absent for exception), B returned [1200,1201], baseline FC/validation checks passed, and no crash was detected. It is not a production qualification of retry, RX-loss or recovery-timeout behavior."}
    (root/"summary.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
    with (root/"summary.csv").open("w",newline="",encoding="utf-8") as f:
        fields=["test","fault","result","A_status","A_status_expected","B_correct_1200_1201","recovery_expected","recovery_requested","recovery_active","recovery_released","fault_injected","function_code_pass_lines","validation_passes","firmware_summary_pass","note"]
        w=csv.DictWriter(f,fieldnames=fields); w.writeheader();
        for r in results: w.writerow({k:r.get(k,"") for k in fields})
    lines=[f"WP24-A TO WP24-F: {a_to_f}",f"EXECUTED CASES: {len(results)}",f"PASS: {counts['PASS']}",f"FAIL: {counts['FAIL']}",f"NOT EXECUTED: {counts['NOT EXECUTED']}",f"EXTENDED RECOVERY COVERAGE: {extended}",f"OVERALL WP2.4 STATUS: {overall}","","CASES:"]
    lines += [f"{r['result']:12} {r['test']} A={r.get('A_status')} B_correct={r.get('B_correct_1200_1201')} recovery={r.get('recovery_requested')}/{r.get('recovery_released')}" for r in results]
    lines += ["","NOT EXECUTED / REQUIRES FURTHER TEST SUPPORT:"]+[f"{r['test']}: {r['reason']}" for r in blocked]
    (root/"report.txt").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print("\n"+"\n".join(lines)); print(f"\nArtifacts: {root}")
    return 0 if overall=="PASS" else 1
if __name__=="__main__": raise SystemExit(main())
