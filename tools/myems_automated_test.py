#!/usr/bin/env python3
"""Fixed MyEMS RTU smoke test. Requires: python -m pip install pyserial."""
import argparse, csv, json, re, subprocess, sys, threading, time
from pathlib import Path
from datetime import datetime
from collections import Counter
import serial

SUCCESS = re.compile(r"ems_acquisition:\s*(\S+)\s+request=(\d+)\s+slave=(\d+)\s+FC(\d+)\s+OK\s+registers=(\d+)\s+retries=(\d+)", re.I)
REG = re.compile(r"ems_acquisition:\s*(\S+)\s+register\[(\d+)\]=(-?\d+)\s+\((0x[0-9a-f]+)\)", re.I)
SCHED = re.compile(r"ems_scheduler:\s*scheduled\s+(\S+):\s*request=(\d+)\s+slave=(\d+)\s+FC(\d+)", re.I)
ERR = {
 "timeout": re.compile(r"\b(timeout|timed out|response timeout)\b", re.I),
 "crc_error": re.compile(r"\b(bad[- ]CRC|CRC error|invalid CRC|CRC mismatch)\b", re.I),
 "stack_overflow": re.compile(r"stack overflow|stack canary|stack smashing", re.I),
 "panic": re.compile(r"assert failed|Guru Meditation|abort\(\) was called", re.I),
 "modbus_exception": re.compile(r"exception response|Modbus exception|exception code", re.I),
}
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--project",type=Path,default=Path.cwd())
    ap.add_argument("--simulator",default="tools/usb_rs485_modbus_slave_sim_all_fc.py")
    ap.add_argument("--console-port",default="COM11")
    ap.add_argument("--console-baud",type=int,default=115200)
    ap.add_argument("--rs485-port",default="COM12")
    ap.add_argument("--baud",type=int,default=9600)
    ap.add_argument("--duration",type=int,default=300)
    ap.add_argument("--startup-wait",type=float,default=2)
    ap.add_argument("--output",type=Path,default=Path("test_results"))
    a=ap.parse_args()
    project=a.project.resolve(); simulator=(project/a.simulator).resolve()
    if not simulator.is_file():
        print(f"ERROR: simulator not found: {simulator}"); return 2
    if a.duration<5: print("ERROR: duration must be >= 5 seconds"); return 2
    # Probe first: never start simulator if console cannot be opened.
    try:
        probe=serial.Serial(a.console_port,a.console_baud,timeout=.1); probe.close()
    except Exception as e:
        print(f"ERROR: cannot access {a.console_port}: {e}")
        print("Close ESP-IDF Serial Monitor/other serial terminal. Simulator was NOT started.")
        return 2
    stamp=datetime.now().strftime("%Y%m%d_%H%M%S")
    out=(a.output if a.output.is_absolute() else project/a.output)/f"myems_rtu_{stamp}"
    out.mkdir(parents=True,exist_ok=True)
    stop=threading.Event(); lock=threading.Lock()
    lines=[]; successes=[]; registers=[]; scheduled=[]; errors=Counter(); error_events=[]
    def capture():
        try:
            with serial.Serial(a.console_port,a.console_baud,timeout=.25) as ser, (out/"esp32_console.log").open("w",encoding="utf-8",errors="replace") as f:
                while not stop.is_set():
                    raw=ser.readline()
                    if not raw: continue
                    line=raw.decode("utf-8",errors="replace").rstrip()
                    f.write(line+"\n"); f.flush()
                    ts=datetime.now().astimezone().isoformat(timespec="milliseconds")
                    with lock:
                        lines.append(line)
                        for name,pat in ERR.items():
                            if pat.search(line):
                                errors[name]+=1; error_events.append({"timestamp":ts,"type":name,"line":line})
                        m=SUCCESS.search(line)
                        if m:
                            x=m.groups(); successes.append({"timestamp":ts,"meter":x[0],"request":int(x[1]),"slave":int(x[2]),"fc":int(x[3]),"registers":int(x[4]),"retries":int(x[5])})
                        m=REG.search(line)
                        if m:
                            x=m.groups(); registers.append({"timestamp":ts,"meter":x[0],"index":int(x[1]),"value":int(x[2]),"hex":x[3]})
                        m=SCHED.search(line)
                        if m:
                            x=m.groups(); scheduled.append({"timestamp":ts,"meter":x[0],"request":int(x[1]),"slave":int(x[2]),"fc":int(x[3])})
        except Exception as e:
            with lock: error_events.append({"type":"console_capture_error","line":str(e)})
    t=threading.Thread(target=capture,daemon=True); t.start(); time.sleep(a.startup_wait)
    cmd=[sys.executable,str(simulator),"--port",a.rs485_port,"--slaves","1,2","--baud",str(a.baud),"--parity","E","--stopbits","1","--fault","none"]
    print(f"Project: {project}\nConsole: {a.console_port} @ {a.console_baud}\nRS485: {a.rs485_port} @ {a.baud}, 8E1\nOutput: {out}")
    proc=None; start=time.monotonic()
    try:
        with (out/"simulator.log").open("w",encoding="utf-8",errors="replace") as sf:
            proc=subprocess.Popen(cmd,cwd=project,stdout=sf,stderr=subprocess.STDOUT)
            while time.monotonic()-start<a.duration:
                if proc.poll() is not None: print(f"WARNING simulator exited: {proc.returncode}"); break
                time.sleep(.2)
    except KeyboardInterrupt: print("Interrupted; saving partial results.")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=4)
            except subprocess.TimeoutExpired: proc.kill()
        stop.set(); t.join(timeout=2)
    with lock:
        ss=list(successes); rr=list(registers); qq=list(scheduled); ee=dict(errors); ev=list(error_events); ll=list(lines)
    # Register log records have meter name, not slave ID: map by meter name.
    values={}
    for r in rr: values.setdefault(r["meter"],[]).append(r["value"])
    grid=1000 in values.get("grid_meter",[]); load=2000 in values.get("load_meter",[])
    byslave=Counter(str(x["slave"]) for x in ss)
    console_problem=not ll or any(x.get("type")=="console_capture_error" for x in ev)
    critical=bool(ee.get("stack_overflow") or ee.get("panic"))
    if console_problem: result="INCONCLUSIVE"
    elif critical: result="FAIL"
    elif byslave["1"] and byslave["2"] and grid and load: result="PASS"
    else: result="INCONCLUSIVE"
    summary={"result":result,"start":stamp,"duration_seconds":round(time.monotonic()-start,2),"console_lines":len(ll),"successful_polls":len(ss),"scheduled_requests":len(qq),"successes_by_slave":dict(byslave),"grid_value_1000_seen":grid,"load_value_2000_seen":load,"detected_errors":ee,"note":"Register records are associated by meter name; their log format does not include slave ID."}
    (out/"summary.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
    (out/"error_events.json").write_text(json.dumps(ev,indent=2),encoding="utf-8")
    def savecsv(name,rows,fields):
        with (out/name).open("w",newline="",encoding="utf-8") as f:
            w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)
    savecsv("successful_polls.csv",ss,["timestamp","meter","request","slave","fc","registers","retries"])
    savecsv("decoded_registers.csv",rr,["timestamp","meter","index","value","hex"])
    savecsv("scheduled_requests.csv",qq,["timestamp","meter","request","slave","fc"])
    report=[f"Result: {result}",f"Elapsed: {summary['duration_seconds']}s",f"Console lines: {len(ll)}",f"Successful polls: {len(ss)}",f"Scheduled requests: {len(qq)}",f"Slave 1 polls: {byslave['1']}",f"Slave 2 polls: {byslave['2']}",f"Grid value 1000 observed: {grid}",f"Load value 2000 observed: {load}",f"Detected errors: {ee or 'none'}","",f"Results: {out}"]
    (out/"report.txt").write_text("\n".join(report)+"\n",encoding="utf-8")
    print("\n"+"\n".join(report)); return 0 if result=="PASS" else (1 if result=="FAIL" else 2)
if __name__=="__main__": raise SystemExit(main())
