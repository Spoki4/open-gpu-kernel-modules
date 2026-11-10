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
  ./winbench.py run wukong --label win-wk-apps --apps --bg-fill 1500
  ./winbench.py metrics DIR GAME       print metrics of a run (PresentMon CSV)

Superposition: the engine binary with the exact arguments the Linux launcher
gives it (bench.py's 8K Optimized performance run, OpenGL, captured from
/proc/PID/cmdline on 2026-11-04), so no launcher clicks. Wukong: Steam
-applaunch and the same clicks as bench.py at 3840x2160 (the settings file
is copied from the Linux Proton prefix, so the graphics settings match).
Frames: PresentMon's time between presents from the start; the stretch kept
is the one bench.py logs with MangoHud, placed by the end of loading (Windows
loads slower: a fixed delay caught the end of the loading screen): LOAD_OFFSET
after the last long frame of the loading, LOG_S long. LOAD_OFFSET is measured
on Linux (Superposition: GPU busy reaches 100% 8.7 s after the engine starts,
MangoHud logs from 12 s). Metrics are bench.py's own frame_metrics() and
stutters(). Logs: local/bench-logs/<label>-<date>/.

--apps and --bg-fill as in bench.py: Edge (Chromium) with the same three
tabs, then idle windowless processes holding VRAM (nvt-idle.exe, built in the
guest from local/vm/win/nvt-idle.cs) so the game is DEFICIT_MB short of what
it peaks at on Windows (WIN_VRAM_MB, from the clean runs' nvidia-smi log:
the game's own figure, as bench.py's VRAM_MB is Linux's). What happens then is
Windows' own (WDDM's video memory manager), not our eviction.
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
# Seconds from the end of loading to bench.py's logged stretch, per game (see
# above). Wukong's loading screen has no long frames: its stretch is placed
# like bench.py's, START_S after Confirm (where PresentMon starts), so None.
LOAD_OFFSET = {"superposition": 3.3, "wukong": None}
# The game's peak VRAM on Windows (memory.used over the clean run minus the
# desktop before it, nvidia-smi.csv), for --bg-fill.
WIN_VRAM_MB = {"superposition": 6773 - 555, "wukong": 5789 - 676}     # 2026-11-04, 617.42
APPS_URLS = ["https://www.youtube.com/watch?v=aqz-KE-bpKQ",
             "https://en.wikipedia.org/wiki/Graphics_processing_unit",
             "file:///C:/nvwin/nvt-webgl.html?mode=anim&tex=200"]
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"


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


def nvsmi_start():
    """VRAM used/free every 500 ms into C:\\nvwin\\out\\nvidia-smi.csv."""
    win(r"Start-Process -WindowStyle Hidden nvidia-smi.exe -ArgumentList "
        r"'--query-gpu=timestamp,memory.used,memory.free --format=csv,nounits -lms 500 "
        r"-f C:\nvwin\out\nvidia-smi.csv'")


def gpumem_start():
    """Dedicated VRAM per process (gpumem.ps1) into C:\\nvwin\\out\\gpumem.csv:
    which process WDDM moved out, which nvidia-smi's total cannot tell."""
    win(r"Start-Process -WindowStyle Hidden powershell -ArgumentList "
        r"'-NoProfile -ExecutionPolicy Bypass -File C:\nvwin\gpumem.ps1'")


def vram_free_mb():
    out = win("nvidia-smi.exe --query-gpu=memory.free --format=csv,noheader,nounits")
    return int(out.split()[0])


def vram_used_mb():
    out = win("nvidia-smi.exe --query-gpu=memory.used --format=csv,noheader,nounits")
    return int(out.split()[0])


def start_apps(log):
    win(r"Remove-Item -Recurse -Force C:\nvwin\edge -ErrorAction SilentlyContinue")
    args = ("--user-data-dir=C:\\nvwin\\edge --no-first-run --no-default-browser-check "
            "--autoplay-policy=no-user-gesture-required --start-maximized " + " ".join(f'"{u}"' for u in APPS_URLS))
    win(f"Start-Process '{EDGE}' -ArgumentList '{args}'")
    log("edge")
    time.sleep(8)


def start_bg(game, deficit, log):
    free = vram_free_mb()
    total = max(0, free - WIN_VRAM_MB[game] + deficit)
    n = max(1, (total + 899) // 900)
    sizes = [total // n + (1 if i < total % n else 0) for i in range(n)] if total else []
    for i, mb in enumerate(sizes):
        win(f"Start-Process -WindowStyle Hidden C:\\nvwin\\nvt-idle.exe "
            f"-ArgumentList '--mb {mb} --log C:\\nvwin\\out\\bg-{i}.log'")
        log("bg_app", mb=mb)
    time.sleep(8)
    got = win(r"Get-Content C:\nvwin\out\bg-*.log", check=False).split()
    log("bg_fill", free_mb=free, game_mb=WIN_VRAM_MB[game], deficit_mb=deficit, bg_total_mb=total,
        apps=len(sizes), allocated=" ".join(got), free_after_mb=vram_free_mb())
    time.sleep(40)     # as bench.py: background apps are old in real life


def stop_apps():
    win("Stop-Process -Name nvt-idle,msedge -Force -ErrorAction SilentlyContinue", check=False)


def fetch(remote, local):
    subprocess.run([VM, "win", "get", remote.replace("\\", "/"), local], check=True)


def run_superposition(d, log):
    g = bench.SuperpositionBench
    out = r"C:\nvwin\out\sp.csv"
    win(f"Start-Process -WorkingDirectory '{SP_DIR}\\bin' '{SP_DIR}\\bin\\superposition.exe' -ArgumentList '{SP_ARGS}'")
    t0 = time.time()
    log("engine_start")
    presentmon("superposition.exe", 0, g.WARMUP_S + g.LOG_S + 30, out)
    time.sleep(g.WARMUP_S + g.LOG_S + 35)
    while "running" in win("if (Get-Process PresentMon* -ErrorAction SilentlyContinue) { 'running' }", check=False):
        time.sleep(2)
    log("logged", s=round(time.time() - t0, 1))
    win("Stop-Process -Name superposition -Force -ErrorAction SilentlyContinue", check=False)
    fetch(out, os.path.join(d, "presentmon.csv"))


def run_wukong(d, log):
    g = bench.WukongBench
    out = r"C:\nvwin\out\wk.csv"
    before = vram_used_mb()
    win(f"Start-Process 'C:\\Program Files (x86)\\Steam\\steam.exe' -ArgumentList '-applaunch {g.APPID}'")
    log("applaunch")
    for _ in range(240):
        if "up" in win("if (Get-Process b1-Win64-Shipping -ErrorAction SilentlyContinue) { 'up' }", check=False):
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("Wukong did not start")
    log("engine_start")
    # Logo, "Press Any Button". The first start after the guest boots is
    # slower: at 25 s the key went to the loading screen (2026-11-04).
    time.sleep(40)
    key("e")
    time.sleep(4)
    click(*g.MENU_BENCHMARK)
    time.sleep(2)
    click(*g.CONFIRM)
    t0 = time.time()
    log("benchmark_confirmed")
    presentmon("b1-Win64-Shipping.exe", 0, g.START_S + g.LOG_S + 20, out)
    time.sleep(g.START_S)
    log("scene", vram_added_mb=vram_used_mb() - before)
    time.sleep(max(g.LOG_S + g.START_S, g.RUN_S) + 5 - g.START_S)
    log("engine_exit", s=round(time.time() - t0, 1))
    win("Stop-Process -Name b1-Win64-Shipping -Force -ErrorAction SilentlyContinue", check=False)
    fetch(out, os.path.join(d, "presentmon.csv"))


def frame_times(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    col = "MsBetweenPresents" if rows and "MsBetweenPresents" in rows[0] else "FrameTime"
    return [float(r[col]) for r in rows if r.get(col) not in (None, "", "NA")]


def logged_stretch(ft, offset, seconds, long_ms=250.0, search_s=60.0):
    """The frames of the stretch bench.py logs: from `offset` s after the end
    of the last frame longer than long_ms in the first search_s (the loading
    screen), `seconds` long. Returns (frames, start_s)."""
    t, load_end = 0.0, 0.0
    for x in ft:
        t += x / 1000
        if t > search_s:
            break
        if x > long_ms:
            load_end = t
    start = load_end + offset
    out, t = [], 0.0
    for x in ft:
        t += x / 1000
        if t > start and t <= start + seconds:
            out.append(x)
    return out, round(start, 2), round(load_end, 2)


def vram_peak(d):
    """Peak memory.used over the run and memory.used at its start (MB)."""
    p = os.path.join(d, "nvidia-smi.csv")
    if not os.path.exists(p):
        return None, None
    with open(p, newline="") as f:
        used = [int(r[1]) for r in csv.reader(f) if len(r) > 2 and r[1].strip().isdigit()]
    return (max(used), used[0]) if used else (None, None)


def metrics(d, game):
    allft = frame_times(os.path.join(d, "presentmon.csv"))
    g = bench.SuperpositionBench if game == "superposition" else bench.WukongBench
    if LOAD_OFFSET[game] is None:
        ft, start, load_end = logged_stretch(allft, g.START_S, g.LOG_S, search_s=0)
    else:
        ft, start, load_end = logged_stretch(allft, LOAD_OFFSET[game], g.LOG_S)
    m = bench.frame_metrics(ft)
    m["stutters"] = len(bench.stutters(ft))
    m["load_end_s"], m["window_start_s"] = load_end, start
    m["vram_peak_used_mb"], m["vram_start_used_mb"] = vram_peak(d)
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
    r.add_argument("--apps", action="store_true", help="Edge with YouTube, Wikipedia, the WebGL page")
    r.add_argument("--bg-fill", type=int, metavar="DEFICIT_MB", help="background apps so the game is this short")
    m = sub.add_parser("metrics")
    m.add_argument("dir")
    m.add_argument("game", choices=["superposition", "wukong"])
    a = ap.parse_args()

    if a.cmd == "setup":
        win(r"New-Item -ItemType Directory -Force C:\nvwin\dl | Out-Null")
        for f in sorted(os.listdir(DL)):
            if f.endswith(".exe"):
                subprocess.run([VM, "win", "put", os.path.join(DL, f), f"C:/nvwin/dl/{f}"], check=True)
                print("copied", f)
        subprocess.run([VM, "win", "put", os.path.join(LOCAL, "nvt-webgl.html"), "C:/nvwin/nvt-webgl.html"], check=True)
        for f in ("nvt-idle.cs", "gpumem.ps1"):
            subprocess.run([VM, "win", "put", os.path.join(LOCAL, "vm", "win", f), f"C:/nvwin/{f}"], check=True)
        print(win(r"& C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize "
                  r"/platform:x64 /out:C:\nvwin\nvt-idle.exe C:\nvwin\nvt-idle.cs"))
        # Edge's first-run pages would cover the tabs (a fresh profile every run).
        win(r"New-Item -Force HKLM:\SOFTWARE\Policies\Microsoft\Edge | Out-Null; "
            r"Set-ItemProperty HKLM:\SOFTWARE\Policies\Microsoft\Edge HideFirstRunExperience 1")
        print("built nvt-idle.exe")
        return
    if a.cmd == "metrics":
        print(json.dumps(metrics(a.dir, a.game), indent=1))
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
    if a.bg_fill is not None and WIN_VRAM_MB[a.game] is None:
        sys.exit(f"--bg-fill: WIN_VRAM_MB[{a.game!r}] is not measured yet (a clean run's vram_peak)")
    win(r"New-Item -ItemType Directory -Force C:\nvwin\out | Out-Null; "
        r"Remove-Item C:\nvwin\out\* -ErrorAction SilentlyContinue")
    nvsmi_start()
    gpumem_start()
    try:
        if a.apps:
            start_apps(log)
        if a.bg_fill is not None:
            start_bg(a.game, a.bg_fill, log)
        (run_superposition if a.game == "superposition" else run_wukong)(d, log)
    finally:
        stop_apps()
        win(r"Stop-Process -Name nvidia-smi -Force -ErrorAction SilentlyContinue; "
            r"New-Item C:\nvwin\out\gpumem.stop | Out-Null", check=False)
        time.sleep(4)
        for f in ("nvidia-smi.csv", "gpumem.csv"):
            try:
                fetch(f"C:\\nvwin\\out\\{f}", os.path.join(d, f))
            except subprocess.CalledProcessError:
                pass
    m = metrics(d, a.game)
    log("metrics", **m)
    if a.game == "wukong" and m["avg_fps"] > 150:
        sys.exit(f"{a.label}: {m['avg_fps']} fps is the menu, not the benchmark (the clicks missed)")
    print(f"{a.label}: avg {m['avg_fps']} fps, 1% low {m['low1_fps']}, 0.1% low {m['low01_fps']}, "
          f"P99 {m['p99_ms']} ms, stutters {m['stutters']}, VRAM peak {m['vram_peak_used_mb']} MB "
          f"(start {m['vram_start_used_mb']})  ({d})")


if __name__ == "__main__":
    main()
