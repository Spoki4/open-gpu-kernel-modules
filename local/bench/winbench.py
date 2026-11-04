#!/usr/bin/env python3
"""The same benchmarks as bench.py, in the Windows guest (local/vm/win.sh)
on NVIDIA's Windows driver: a reference for what the hardware does without
our Linux driver, Proton or MangoHud in the way. Runs on the host; drives
the guest over ssh (its job runner for the desktop session) and QMP (clicks
and keys, as a USB tablet/keyboard).

  ./winbench.py setup                  copy PresentMon, Superposition, Steam,
                                       the driver installer into C:\\nvwin\\dl
  ./winbench.py run superposition --label win-clean
  ./winbench.py run wukong --label win-wk-clean
  ./winbench.py metrics DIR            print metrics of a run (PresentMon CSV)

Superposition: the engine binary with the exact arguments the Linux launcher
gives it (bench.py's 8K Optimized performance run, OpenGL, captured from
/proc/PID/cmdline on 2026-11-04), so no launcher clicks. Wukong: Steam
-applaunch and the same clicks as bench.py at 3840x2160 (the settings file
is copied from the Linux Proton prefix, so the graphics settings match).
Frames: PresentMon's time between presents for the same stretch bench.py
logs with MangoHud (WARMUP_S/START_S after start, LOG_S long); metrics are
bench.py's own frame_metrics() and stutters(). Logs: local/bench-logs/<label>-<date>/.
"""

import argparse
import csv
import datetime
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.path.dirname(HERE)
VM = os.path.join(LOCAL, "vm", "vm.sh")
DL = os.path.expanduser("~/.local/share/nvwin/dl")
PRESENTMON = "PresentMon-2.6.0-x64.exe"
sys.path.insert(0, HERE)
import bench  # noqa: E402  (frame_metrics, stutters, the games' timings)

SP_DIR = r"C:\Program Files\Unigine\Superposition Benchmark"
SP_ARGS = ("-preset 6 -video_app opengl -video_vsync 0 -video_mode -1 "
           "-console_command \"world_load superposition/superposition && render_manager_create_textures 1\" "
           "-project_name Superposition -video_fullscreen 1 -video_width 3840 -video_height 2160 "
           "-virtual_width 7680 -virtual_height 4320 -extern_plugin GPUMonitor -mode 0 -sound 1 -tooltips 1")
SCREEN = (3840, 2160)


def win(ps, check=True):
    """PowerShell in the guest's desktop session; returns its output."""
    r = subprocess.run([VM, "win", "run", ps], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"guest: rc {r.returncode}: {(r.stdout + r.stderr).strip()[-400:]}")
    return r.stdout


def qmp(cmd):
    r = subprocess.run([VM, "win", "qmp", json.dumps(cmd)], capture_output=True, text=True)
    if '"error"' in r.stdout:
        raise RuntimeError(f"qmp: {r.stdout.strip()}")


def click(x, y):
    """Left click at screen pixel x, y (usb-tablet: 0..32767 over the screen)."""
    ax, ay = round(x * 32767 / (SCREEN[0] - 1)), round(y * 32767 / (SCREEN[1] - 1))
    move = [{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}]
    qmp({"execute": "input-send-event", "arguments": {"events": move}})
    time.sleep(0.1)
    for down in (True, False):
        qmp({"execute": "input-send-event",
             "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
        time.sleep(0.08)


def key(qcode, hold=0.1):
    for down in (True, False):
        qmp({"execute": "input-send-event",
             "arguments": {"events": [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]}})
        time.sleep(hold)


def presentmon(process, delay, seconds, out):
    """Starts PresentMon in the guest for `seconds` after `delay`, on `process`."""
    win(f"Start-Process -WindowStyle Hidden C:\\nvwin\\dl\\{PRESENTMON} -ArgumentList "
        f"'--process_name {process} --output_file {out} --delay {delay} --timed {seconds} "
        f"--terminate_after_timed --no_console_stats --stop_existing_session --session_name nvwin'")


def fetch(remote, local):
    subprocess.run([VM, "win", "get", remote.replace("\\", "/"), local], check=True)


def run_superposition(d, log):
    g = bench.SuperpositionBench
    out = r"C:\nvwin\out\sp.csv"
    win(r"New-Item -ItemType Directory -Force C:\nvwin\out | Out-Null; Remove-Item C:\nvwin\out\* -ErrorAction SilentlyContinue")
    win(f"Start-Process -WorkingDirectory '{SP_DIR}\\bin' '{SP_DIR}\\bin\\superposition.exe' -ArgumentList '{SP_ARGS}'")
    t0 = time.time()
    log("engine_start")
    presentmon("superposition.exe", g.WARMUP_S, g.LOG_S, out)
    time.sleep(g.WARMUP_S + g.LOG_S + 5)
    while "running" in win("if (Get-Process PresentMon* -ErrorAction SilentlyContinue) { 'running' }", check=False):
        time.sleep(2)
    log("logged", s=round(time.time() - t0, 1))
    win("Stop-Process -Name superposition -Force -ErrorAction SilentlyContinue", check=False)
    fetch(out, os.path.join(d, "presentmon.csv"))


def run_wukong(d, log):
    g = bench.WukongBench
    out = r"C:\nvwin\out\wk.csv"
    win(r"New-Item -ItemType Directory -Force C:\nvwin\out | Out-Null; Remove-Item C:\nvwin\out\* -ErrorAction SilentlyContinue")
    win(f"Start-Process 'C:\\Program Files (x86)\\Steam\\steam.exe' -ArgumentList '-applaunch {g.APPID}'")
    log("applaunch")
    for _ in range(240):
        if "up" in win("if (Get-Process b1-Win64-Shipping -ErrorAction SilentlyContinue) { 'up' }", check=False):
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("Wukong did not start")
    log("engine_start")
    time.sleep(25)                       # logo, "Press Any Button"
    key("e")
    time.sleep(4)
    click(*g.MENU_BENCHMARK)
    time.sleep(2)
    click(*g.CONFIRM)
    t0 = time.time()
    log("benchmark_confirmed")
    presentmon("b1-Win64-Shipping.exe", g.START_S, g.LOG_S, out)
    time.sleep(max(g.LOG_S + g.START_S, g.RUN_S) + 5)
    log("engine_exit", s=round(time.time() - t0, 1))
    win("Stop-Process -Name b1-Win64-Shipping -Force -ErrorAction SilentlyContinue", check=False)
    fetch(out, os.path.join(d, "presentmon.csv"))


def frame_times(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    col = "MsBetweenPresents" if rows and "MsBetweenPresents" in rows[0] else "FrameTime"
    return [float(r[col]) for r in rows if r.get(col) not in (None, "", "NA")]


def metrics(d):
    ft = frame_times(os.path.join(d, "presentmon.csv"))
    m = bench.frame_metrics(ft)
    m["stutters"] = len(bench.stutters(ft))
    with open(os.path.join(d, "metrics.json"), "w") as f:
        json.dump(m, f, indent=1)
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup")
    r = sub.add_parser("run")
    r.add_argument("game", choices=["superposition", "wukong"])
    r.add_argument("--label", required=True)
    m = sub.add_parser("metrics")
    m.add_argument("dir")
    a = ap.parse_args()

    if a.cmd == "setup":
        win(r"New-Item -ItemType Directory -Force C:\nvwin\dl | Out-Null")
        for f in sorted(os.listdir(DL)):
            if f.endswith(".exe"):
                subprocess.run([VM, "win", "put", os.path.join(DL, f), f"C:/nvwin/dl/{f}"], check=True)
                print("copied", f)
        return
    if a.cmd == "metrics":
        print(json.dumps(metrics(a.dir), indent=1))
        return

    d = os.path.join(LOCAL, "bench-logs", f"{a.label}-{datetime.datetime.now():%Y%m%d_%H%M%S}")
    os.makedirs(d)
    ev = open(os.path.join(d, "events.jsonl"), "w")

    def log(what, **kw):
        ev.write(json.dumps(dict(t=round(time.time(), 3), ev=what, **kw)) + "\n")
        ev.flush()
        print(what, kw if kw else "", flush=True)
    log("start", game=a.game, driver=win("(Get-CimInstance Win32_VideoController | "
                                         "Where-Object Name -like '*NVIDIA*').DriverVersion", check=False).strip())
    (run_superposition if a.game == "superposition" else run_wukong)(d, log)
    m = metrics(d)
    log("metrics", **m)
    print(f"{a.label}: avg {m['avg_fps']} fps, 1% low {m['low1_fps']}, 0.1% low {m['low01_fps']}, "
          f"P99 {m['p99_ms']} ms, stutters {m['stutters']}  ({d})")


if __name__ == "__main__":
    main()
