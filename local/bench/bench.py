#!/usr/bin/env python3
"""Frame-time benchmarks with a full system trace, for seeing and explaining
stutters (test VM guest; the desktop must be logged in).

Every run records, on one clock:
  - the game's frame times (MangoHud, every frame; logging is started by its
    hotkey at a trace marker, so frame i ends at marker + elapsed_i);
  - a Perfetto system trace (perfetto.cfg): scheduler with the kernel
    function a blocked thread sleeps in, page flips, the patched driver's
    events (moves, pauses, make-room waits, reserve denials, RM lock waits),
    Xids, and counters written as trace markers: VRAM free outside the
    reserve, reserve block free, evicted MB, PCIe traffic, GPU load.

  ./bench.py run superposition --label clean
  ./bench.py run superposition --label apps-evict --apps --bg-fill 1500 --auto hints
  ./bench.py run superposition --label apps-noevict --apps --bg-fill 1500 --auto off
  ./bench.py analyze DIR                 metrics.json + stutters.csv (with causes)
  ./bench.py compare DIR... -o FILE.html frame times, distributions, metrics, stutters

Metrics (as CapFrameX / reviewers use them): average FPS (frames / time),
1% and 0.1% low (average FPS of the slowest 1% / 0.1% of frames), P99 and
P99.9 frame time, frame-time standard deviation, and stutters: frames longer
than 2.5x the median of the 31 frames around them and at least 8 ms more.
Run under ../watchdog.sh; logs in local/bench-logs/<label>-<date>/.
"""

import argparse
import csv
import datetime
import glob
import io
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LOCAL, "scenario"))
sys.path.insert(0, os.path.join(LOCAL, "trace"))

PERFETTO = os.path.join(LOCAL, ".perfetto", "v58.2", "linux-amd64")
TRACEBOX = os.path.join(PERFETTO, "tracebox")
TP = os.path.join(PERFETTO, "trace_processor_shell")
EVICT = os.path.join(LOCAL, "evict.sh")
MARKER = "/sys/kernel/tracing/trace_marker"
HOTKEY = "rightctrl+rightshift+scrolllock"     # MangoHud toggle_logging, see scenario.py


def mono():
    return time.clock_gettime(time.CLOCK_MONOTONIC)


def patched():
    """The patched driver (vidmem reserve, eviction, tracepoints) is loaded;
    otherwise the stock one: runs work the same, minus the driver's own data."""
    return bool(glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma"))


def free_mb():
    """Free vidmem the game can get: outside the reserve block when patched,
    NVML's free memory with the stock driver."""
    if patched():
        import scenario as sc
        return sc.free_outside_mb()
    out = subprocess.run(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
                         capture_output=True, text=True).stdout
    return int(out.split()[0])


# --------------------------------------------------------------------------
# recording
# --------------------------------------------------------------------------

def sampler_main(stop_file):
    """Root helper: counters into the trace (trace_marker "C|pid|name|value")
    every 250 ms until stop_file appears. NVML once a second: its calls take
    the RM API lock themselves."""
    import nvtrace
    pma = (glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma") or [None])[0]
    try:
        nvml = nvtrace.Nvml()
    except OSError:
        nvml = None
    fd = os.open(MARKER, os.O_WRONLY)

    def c(name, v):
        os.write(fd, f"C|0|{name}|{v}".encode())
    i = 0
    while not os.path.exists(stop_file):
        if pma:
            v = [int(x) for x in open(pma).read().split()]
            c("VRAM free outside reserve MB", v[0] - v[3])
            c("reserve block free MB", v[3])
            c("reserve block largest MB", v[4])
        elif nvml is not None and i % 4 == 0:
            m = nvtrace._Mem()
            if nvml.lib.nvmlDeviceGetMemoryInfo(nvml.dev, nvtrace.ctypes.byref(m)) == 0:
                c("VRAM free MB (stock driver)", m.free >> 20)
        if nvml is not None and i % 4 == 0:
            s = nvml.sample()
            for k, name in (("util_gpu", "GPU busy %"), ("pcie_rx_mbs", "PCIe host->GPU MB/s"),
                            ("pcie_tx_mbs", "PCIe GPU->host MB/s"), ("sm_mhz", "GPU clock MHz")):
                if k in s:
                    c(name, s[k])
        i += 1
        time.sleep(0.25)
    os.close(fd)


class Recorder:
    def __init__(self, d):
        self.d = d
        self.trace = os.path.join(d, "trace.pftrace")
        out = subprocess.run(["sudo", "-n", TRACEBOX, "--txt", "-c", os.path.join(HERE, "perfetto.cfg"),
                              "-o", self.trace, "--background-wait"], capture_output=True, text=True)
        self.pid = int(out.stdout.split()[-1])
        self.stop_file = os.path.join(d, ".sampler-stop")
        self.sampler = subprocess.Popen(["sudo", "-n", sys.executable, os.path.abspath(__file__), "_sampler",
                                         self.stop_file])

    def mark(self, text):
        subprocess.run(["sudo", "-n", "sh", "-c", f"printf '%s' '{text}' > {MARKER}"])

    def stop(self):
        open(self.stop_file, "w").close()
        self.sampler.wait(10)
        os.unlink(self.stop_file)
        subprocess.run(["sudo", "-n", "kill", "-INT", str(self.pid)])
        for _ in range(60):
            if subprocess.run(["sudo", "-n", "kill", "-0", str(self.pid)], capture_output=True).returncode:
                break
            time.sleep(0.5)
        subprocess.run(["sudo", "-n", "chown", f"{os.getuid()}:{os.getgid()}", self.trace])


class SuperpositionBench:
    """The free performance run of Unigine Superposition, preset 8K
    Optimized (OpenGL, ~7.8 GB VRAM, fullscreen): a fixed camera path, so
    runs compare frame by frame. MangoHud logs a fixed stretch of it."""
    DIR = os.path.expanduser("~/bench/superposition")
    BENCH_TAB = (415, 229)
    PERFORMANCE = (558, 175)
    PRESET_BOX = (894, 176)
    PRESET_8K = (865, 316)
    RUN = (922, 618)
    VRAM_MB = 6700       # peak, from the clean run's trace (10859 -> 4178 MB free)
    WARMUP_S = 12        # from the engine's start to the first logged frame
    LOG_S = 90           # logged stretch (the run lasts ~2 min)

    def __init__(self, d, inp, rec):
        self.d, self.inp, self.rec = d, inp, rec

    def run(self, log):
        import gs
        subprocess.run(["pkill", "-x", "launcher"])
        mh_dir = os.path.join(self.d, "mangohud")
        os.makedirs(mh_dir, exist_ok=True)
        env = dict(gs.env(), MANGOHUD="1",
                   # Logging starts by itself WARMUP_S after each process starts (no
                   # hotkey: with other windows open the keys went to Chromium, not
                   # the game); analyze() aligns the frames with the trace's flips.
                   # autostart_log does nothing with no_display (MangoHud 0.8.4): the HUD
                   # is on but invisible (alpha 0, only the FPS item).
                   MANGOHUD_CONFIG=(f"fps_only,alpha=0.0,background_alpha=0.0,gpu_stats=0,output_folder={mh_dir},"
                                    f"log_interval=0,autostart_log={self.WARMUP_S},log_duration={self.LOG_S}"))
        subprocess.Popen(["mangohud", "./Superposition"], cwd=self.DIR, env=env, start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            w = [x for x in gs.windows() if x["caption"].startswith("UNIGINE Superposition")]
            if w:
                w = w[0]
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("launcher did not open")
        gs.activate(w["pid"])
        time.sleep(2)
        s = gs.screen()[0]["scale"]
        click = lambda p: self.inp.click(w["x"] * s + p[0], w["y"] * s + p[1])   # noqa: E731
        for p in (self.BENCH_TAB, self.PERFORMANCE, self.PRESET_BOX, self.PRESET_8K):
            click(p)
            time.sleep(0.7)
        gs.shot(os.path.join(self.d, "launcher.png"))
        click(self.RUN)
        t_click = mono()
        pid = None
        for _ in range(120):
            r = subprocess.run(["pgrep", "-n", "-x", "superposition"], capture_output=True, text=True)
            if r.stdout.strip():
                pid = int(r.stdout.split()[0])
                break
            time.sleep(0.25)
        if pid is None:
            raise RuntimeError("benchmark did not start")
        log("engine_start", pid=pid, s_after_click=round(mono() - t_click, 2))
        self.rec.mark("bench: engine start")
        time.sleep(self.WARMUP_S)
        # The launcher starts a short-lived superposition first (a check),
        # then the engine: the engine is whichever runs after the warm-up.
        r = subprocess.run(["pgrep", "-n", "-x", "superposition"], capture_output=True, text=True)
        if not r.stdout.strip():
            raise RuntimeError("no benchmark running after the warm-up")
        pid = int(r.stdout.split()[0])
        log("engine", pid=pid)
        t0 = mono()
        while subprocess.run(["pgrep", "-x", "superposition"], capture_output=True).returncode == 0 \
                and mono() - t0 < 400:
            time.sleep(0.5)
        log("engine_exit", s=round(mono() - t0 + self.WARMUP_S, 1))
        self.rec.mark("bench: engine exit")
        time.sleep(2)
        gs.shot(os.path.join(self.d, "results.png"))
        subprocess.run(["pkill", "-x", "launcher"])


GAMES = {"superposition": SuperpositionBench}


def cmd_run(a):
    import gs
    import scenario as sc
    if patched():
        sc.check_setup()
    d = os.path.join(LOCAL, "bench-logs", f"{a.label}-{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(d)
    ev = open(os.path.join(d, "events.jsonl"), "w")

    def log(kind, **kw):
        rec = {"t": round(mono(), 3), "kind": kind, **kw}
        ev.write(json.dumps(rec) + "\n")
        ev.flush()
        print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {kind} " + " ".join(f"{k}={v}" for k, v in kw.items()),
              flush=True)

    meta = {"label": a.label, "game": a.game, "apps": a.apps, "bg_fill": a.bg_fill, "auto": a.auto,
            "driver_kind": "patched" if patched() else "stock",
            "start": datetime.datetime.now().isoformat(timespec="seconds"),
            "driver": open("/proc/driver/nvidia/version").readline().strip()}
    if a.auto and patched():
        subprocess.run(["sudo", "-n", EVICT, "auto", a.auto], capture_output=True)
    inp = gs.Input()
    chrome = None
    bg = []
    rec = None
    try:
        if a.apps:
            chrome = sc.Chromium(["https://www.youtube.com/watch?v=aqz-KE-bpKQ",
                                  "https://en.wikipedia.org/wiki/Graphics_processing_unit",
                                  f"file://{LOCAL}/nvt-webgl.html?mode=anim&tex=200"])
            log("chromium", pid=chrome.pid)
            time.sleep(8)
        if a.bg_fill is not None:
            free = free_mb()
            total = max(0, free - GAMES[a.game].VRAM_MB + a.bg_fill)
            n = max(1, (total + 899) // 900)
            sizes = [total // n + (1 if i < total % n else 0) for i in range(n)] if total else []

            import types
            bg = sc.start_bg(types.SimpleNamespace(dir=d, log=log), sizes)
            log("bg_fill", free_outside_mb=free, game_mb=GAMES[a.game].VRAM_MB, deficit_mb=a.bg_fill,
                bg_total_mb=total, apps=len(sizes))
            time.sleep(40)     # background apps are old in real life (VIDMEM_EVICT_NEW_PROC_SEC)
        rec = Recorder(d)
        time.sleep(2)
        rec.mark("bench: start " + a.label)
        GAMES[a.game](d, inp, rec).run(log)
        rec.mark("bench: end")
        time.sleep(1)
    finally:
        if rec:
            rec.stop()
        inp.close()
        if chrome:
            chrome.close()
        for p, f in bg:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
            f.close()
        if a.auto and patched():
            subprocess.run(["sudo", "-n", EVICT, "auto", "hints"], capture_output=True)
        meta["end"] = datetime.datetime.now().isoformat(timespec="seconds")
        json.dump(meta, open(os.path.join(d, "meta.json"), "w"), indent=1)
    res = analyze(d)
    print(format_metrics(res), flush=True)
    print(f"bench: {d}", flush=True)


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------

def tp_query(trace, sql):
    """CSV results of each statement (trace_processor_shell prints them
    separated by a blank line; its log goes to stderr)."""
    r = subprocess.run([TP, "query", "-f", "/dev/stdin", trace], input=sql, capture_output=True, text=True)
    tables = []
    for block in r.stdout.strip().split("\n\n"):
        if block.strip():
            tables.append(list(csv.DictReader(io.StringIO(block.strip()))))
    if r.returncode != 0:
        raise RuntimeError(f"trace_processor: {r.stderr.strip()[-300:]}")
    return tables


def frame_metrics(ft):
    """ft: frame times in ms, in order."""
    n = len(ft)
    tot = sum(ft) / 1000
    s = sorted(ft)

    def low(frac):
        k = max(1, int(n * frac))
        worst = s[-k:]
        return 1000 / (sum(worst) / k)

    def pct(p):
        return s[min(n - 1, int(n * p))]
    return {"frames": n, "seconds": round(tot, 2), "avg_fps": round(n / tot, 2),
            "low1_fps": round(low(0.01), 2), "low01_fps": round(low(0.001), 2),
            "p99_ms": round(pct(0.99), 2), "p999_ms": round(pct(0.999), 2), "max_ms": round(s[-1], 2),
            "median_ms": round(statistics.median(ft), 2), "stdev_ms": round(statistics.pstdev(ft), 2)}


def stutters(ft, win=15, factor=2.5, min_extra=8.0):
    """Indices of frames longer than factor x the median of the frames around
    them (win on each side) and at least min_extra ms more."""
    out = []
    for i, x in enumerate(ft):
        lo, hi = max(0, i - win), min(len(ft), i + win + 1)
        med = statistics.median(ft[lo:i] + ft[i + 1:hi]) if hi - lo > 1 else x
        if x > factor * med and x - med >= min_extra:
            out.append((i, x, med))
    return out


SQL_EVENTS = """
select e.ts, e.name, t.tid, t.name as thread, p.pid, p.name as proc,
       (select group_concat(a.key || '=' || coalesce(a.int_value, a.string_value), ' ')
          from args a where a.arg_set_id = e.arg_set_id) as fields
from ftrace_event e left join thread t using(utid) left join process p using(upid)
where e.name like 'vidmem_%' or e.name in ('rm_lock', 'nvidia_dev_xid')
order by e.ts;
"""

SQL_MARKS = """
select e.ts, (select string_value from args a where a.arg_set_id = e.arg_set_id and a.key like '%buf%') as buf
from ftrace_event e where e.name = 'print' order by e.ts;
"""


def sql_game_states(pid):
    return f"""
select ts.ts, ts.dur, ts.state, ts.blocked_function, t.tid, t.name as thread
from thread_state ts join thread t using(utid) join process p using(upid)
where p.pid = {pid} and ts.dur > 0 order by ts.ts;
select t.tid, t.name, sum(ts.dur) as busy from thread_state ts join thread t using(utid) join process p using(upid)
where p.pid = {pid} and ts.state = 'Running' group by t.tid order by busy desc limit 1;
"""


def align_frames(trace, el, engine_ts, warmup_s, res):
    """Trace timestamp of MangoHud's logging start. Coarse: the engine-start
    mark + autostart delay. Exact: a fullscreen game's frames are page flips,
    so the offset that puts the most frame ends just before a flip (it reaches
    the screen at the next refresh, up to 8 ms later at 144 Hz) wins."""
    if engine_ts is None:
        return None
    guess = engine_ts + int(warmup_s * 1e9)
    flips = [int(r["ts"]) for r in tp_query(trace, f"""
select ts from ftrace_event where name = 'drm_vblank_event_delivered'
  and ts between {guess - 3_000_000_000} and {guess + el[-1] + 3_000_000_000} order by ts;""")[0]]
    if len(flips) < 50:
        res["alignment"] = "coarse (no page flips in the trace)"
        return guess
    # Flips at every refresh (the compositor redraws another window, e.g. a
    # visible Chromium) match any offset: then only the coarse estimate.
    if len(flips) > 1.3 * len(el) * (el[-1] + 6e9) / el[-1]:
        res["alignment"] = f"coarse ({len(flips)} flips for {len(el)} frames: the compositor flips for other windows too)"
        return guess
    import bisect
    sample = el[:: max(1, len(el) // 400)]

    def score(off):
        hit = 0
        for e in sample:
            t = off + e
            k = bisect.bisect_left(flips, t)
            if k < len(flips) and flips[k] <= t + 8_000_000:
                hit += 1
        return hit
    best = max(range(guess - 2_000_000_000, guess + 2_000_000_000, 1_000_000), key=score)
    fine = max(range(best - 1_000_000, best + 1_000_000, 100_000), key=score)
    res["alignment"] = f"page flips: {score(fine)}/{len(sample)} frames reach the screen within 8 ms, {(fine - guess) / 1e6:+.0f} ms from the guess"
    return fine


def load_mangohud(d):
    files = sorted((f for f in glob.glob(os.path.join(d, "mangohud", "*.csv")) if not f.endswith("_summary.csv")),
                   key=os.path.getsize)
    if not files:
        return [], []
    rows = open(files[-1]).read().splitlines()     # the engine's (the launcher may log too)
    hdr = rows[2].split(",")
    i_ft, i_el = hdr.index("frametime"), hdr.index("elapsed")
    ft, el = [], []
    for line in rows[3:]:
        p = line.split(",")
        try:
            ft.append(float(p[i_ft]))
            el.append(int(p[i_el]))
        except (IndexError, ValueError):
            pass
    return ft, el


def analyze(d):
    events = [json.loads(l) for l in open(os.path.join(d, "events.jsonl"))]
    ft, el = load_mangohud(d)
    res = {"dir": d, "meta": json.load(open(os.path.join(d, "meta.json")))}
    if not ft:
        res["error"] = "no MangoHud frames"
        json.dump(res, open(os.path.join(d, "metrics.json"), "w"), indent=1)
        return res
    res["frames"] = frame_metrics(ft)
    st = stutters(ft)
    res["frames"]["stutters"] = len(st)
    res["frames"]["stutter_ms_total"] = round(sum(x - m for _, x, m in st), 1)
    trace = os.path.join(d, "trace.pftrace")
    marks = {}
    for r in tp_query(trace, SQL_MARKS)[0]:
        b = (r.get("buf") or "").strip()
        if b.startswith("bench:"):
            marks[b] = int(r["ts"])
    t0 = align_frames(trace, el, marks.get("bench: engine start"), SuperpositionBench.WARMUP_S, res)
    pid = next((e["pid"] for e in reversed(events) if e["kind"] in ("engine", "engine_start")), None)
    drv = tp_query(trace, SQL_EVENTS)[0]
    states, main = tp_query(trace, sql_game_states(pid))[:2] if pid else ([], [])
    main_tid = int(main[0]["tid"]) if main else None
    res["main_thread"] = main[0] if main else None
    # Driver activity during the logged stretch.
    if t0:
        t_end = t0 + el[-1]
        in_run = [r for r in drv if t0 <= int(r["ts"]) <= t_end]
        res["driver"] = {k: sum(1 for r in in_run if r["name"] == k)
                         for k in ("vidmem_move", "vidmem_pause", "vidmem_makeroom", "vidmem_reserve", "rm_lock")}
        res["driver"]["moved_mb"] = sum(int(dict(kv.split("=", 1) for kv in (r["fields"] or "").split())
                                            .get("kb", 0)) >> 10 for r in in_run if r["name"] == "vidmem_move")
    # Each stutter: what the game's main thread did and what the driver did.
    rows = []
    for i, x, med in st:
        if t0 is None:
            break
        end = t0 + el[i]
        beg = end - int(x * 1e6)
        m = {}
        blocked = {}
        for s in states:
            if int(s["tid"]) != main_tid:
                continue
            a, b = int(s["ts"]), int(s["ts"]) + int(s["dur"])
            ov = min(b, end) - max(a, beg)
            if ov > 0:
                m[s["state"]] = m.get(s["state"], 0) + ov / 1e6
                if s["blocked_function"] not in ("", "[NULL]", None):
                    blocked[s["blocked_function"]] = blocked.get(s["blocked_function"], 0) + ov / 1e6
        dv = [r for r in drv if beg - 5e6 <= int(r["ts"]) <= end + 5e6]
        rows.append({"t_s": round((end - t0) / 1e9, 3), "frame_ms": round(x, 1), "median_ms": round(med, 1),
                     "main_thread": " ".join(f"{k} {v:.1f}" for k, v in sorted(m.items(), key=lambda kv: -kv[1])),
                     "blocked_in": " ".join(f"{k} {v:.1f}" for k, v in sorted(blocked.items(), key=lambda kv: -kv[1])[:2]),
                     "driver": "; ".join(f"{r['name']}({r['proc']}) {r['fields']}"[:160] for r in dv[:4])})
    with open(os.path.join(d, "stutters.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["t_s", "frame_ms", "median_ms", "main_thread", "blocked_in", "driver"])
        w.writeheader()
        w.writerows(rows)
    res["stutter_rows"] = rows[:50]
    res["frame_times_ms"] = ft
    res["frame_end_s"] = [e / 1e9 for e in el]
    # Counters over the run (for the report's lower panels).
    if t0:
        cs = tp_query(trace, f"""
select t.name, c.ts, c.value from counter c join counter_track t on c.track_id = t.id
where t.name in ('VRAM free outside reserve MB', 'VRAM free MB (stock driver)', 'PCIe host->GPU MB/s',
                 'PCIe GPU->host MB/s', 'GPU busy %')
  and c.ts between {t0} and {t0 + el[-1]} order by c.ts;""")[0]
        ser = {}
        for r in cs:
            ser.setdefault(r["name"], ([], []))
            ser[r["name"]][0].append((int(r["ts"]) - t0) / 1e9)
            ser[r["name"]][1].append(float(r["value"]))
        res["counters"] = ser
    json.dump(res, open(os.path.join(d, "metrics.json"), "w"), indent=1)
    return res


def format_metrics(res):
    if "error" in res:
        return f"{res['dir']}: {res['error']}"
    f = res["frames"]
    s = (f"{res['meta']['label']}: avg {f['avg_fps']} fps, 1% low {f['low1_fps']}, 0.1% low {f['low01_fps']}, "
         f"P99 {f['p99_ms']} ms, max {f['max_ms']} ms, stdev {f['stdev_ms']} ms, stutters {f['stutters']} "
         f"({f['stutter_ms_total']} ms lost), {f['frames']} frames in {f['seconds']} s")
    if res.get("alignment"):
        s += f"\n  frames aligned with the trace: {res['alignment']}"
    if res.get("driver"):
        d = res["driver"]
        s += (f"\n  driver: {d['vidmem_move']} moves ({d['moved_mb']} MB), {d['vidmem_pause']} pauses, "
              f"{d['vidmem_makeroom']} make-room, {d['vidmem_reserve']} reserve denials, {d['rm_lock']} long lock waits/holds")
    return s


def cmd_compare(a):
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots
    runs = []
    for d in a.dirs:
        p = os.path.join(d, "metrics.json")
        runs.append(json.load(open(p)) if os.path.exists(p) else analyze(d))
    runs = [r for r in runs if "frames" in r]
    fig = make_subplots(rows=3, cols=1, shared_xaxes=True, row_heights=[0.55, 0.25, 0.2], vertical_spacing=0.04,
                        subplot_titles=("Frame time (ms), every frame", "VRAM free (MB; patched: outside the reserve)",
                                        "PCIe host->GPU (MB/s)"))
    for r in runs:
        lab = r["meta"]["label"]
        fig.add_trace(go.Scattergl(x=r["frame_end_s"], y=r["frame_times_ms"], mode="lines", name=lab,
                                   line={"width": 1}), row=1, col=1)
        st = r.get("stutter_rows", [])
        if st:
            fig.add_trace(go.Scatter(x=[s["t_s"] for s in st], y=[s["frame_ms"] for s in st], mode="markers",
                                     name=lab + " stutters", marker={"size": 7, "symbol": "x"},
                                     text=[f"{s['main_thread']}<br>{s['blocked_in']}<br>{s['driver']}" for s in st],
                                     hoverinfo="text+y"), row=1, col=1)
        c = r.get("counters", {})
        vk = "VRAM free outside reserve MB" if "VRAM free outside reserve MB" in c else "VRAM free MB (stock driver)"
        if vk in c:
            x, y = c[vk]
            fig.add_trace(go.Scatter(x=x, y=y, mode="lines", name=lab + " VRAM free", showlegend=False), row=2, col=1)
        if "PCIe host->GPU MB/s" in c:
            x, y = c["PCIe host->GPU MB/s"]
            fig.add_trace(go.Scatter(x=x, y=y, mode="lines", name=lab + " PCIe", showlegend=False), row=3, col=1)
    fig.update_layout(height=900, title="Frame times: same benchmark, different conditions", hovermode="x unified")
    fig.update_xaxes(title_text="seconds into the logged stretch", row=3, col=1)

    dist = go.Figure()
    for r in runs:
        s = sorted(r["frame_times_ms"])
        n = len(s)
        dist.add_trace(go.Scatter(x=[100 * i / (n - 1) for i in range(n)], y=s, mode="lines", name=r["meta"]["label"]))
    dist.update_layout(title="Frame-time percentiles (flat and low = smooth; the right end = worst frames)",
                       xaxis_title="percentile of frames", yaxis_title="frame time (ms)", height=450)
    dist.update_xaxes(range=[90, 100])

    cols = ["avg_fps", "low1_fps", "low01_fps", "p99_ms", "p999_ms", "max_ms", "stdev_ms", "stutters", "stutter_ms_total"]
    th = "".join(f"<th>{c}</th>" for c in ["run"] + cols + ["driver"])
    tr = ""
    for r in runs:
        d = r.get("driver") or {}
        drv = (f"{d.get('vidmem_move', 0)} moves / {d.get('moved_mb', 0)} MB, {d.get('vidmem_pause', 0)} pauses, "
               f"{d.get('vidmem_reserve', 0)} denials, {d.get('rm_lock', 0)} lock events") if d else ""
        tr += "<tr>" + f"<td>{r['meta']['label']}</td>" + "".join(f"<td>{r['frames'][c]}</td>" for c in cols) + f"<td>{drv}</td></tr>"
    st_html = ""
    for r in runs:
        rows = r.get("stutter_rows", [])
        st_html += f"<h3>{r['meta']['label']}: {len(rows)} stutters</h3>"
        if rows:
            st_html += ("<table><tr><th>t (s)</th><th>frame ms</th><th>median ms</th><th>main thread (ms per state)</th>"
                        "<th>blocked in</th><th>driver around it</th></tr>" +
                        "".join(f"<tr><td>{s['t_s']}</td><td>{s['frame_ms']}</td><td>{s['median_ms']}</td>"
                                f"<td>{s['main_thread']}</td><td>{s['blocked_in']}</td><td>{s['driver']}</td></tr>"
                                for s in rows[:30]) + "</table>")
    html = f"""<!doctype html><html><head><meta charset="utf-8"><title>Frame-time comparison</title>
<style>body{{font-family:system-ui,sans-serif;margin:16px;max-width:1400px}}table{{border-collapse:collapse;font-size:13px}}
td,th{{border:1px solid #ccc;padding:3px 6px;text-align:right}}td:first-child,th:first-child{{text-align:left}}</style></head><body>
<h1>Frame-time comparison</h1>
<p>{', '.join(r['meta']['label'] + ' (' + r['meta']['start'] + ')' for r in runs)}. Stutter: a frame longer than 2.5x the
median of the 30 frames around it and at least 8 ms more. 1% / 0.1% low: average FPS of the slowest 1% / 0.1% of frames.</p>
<table><tr>{th}</tr>{tr}</table>
{fig.to_html(full_html=False, include_plotlyjs=True)}
{dist.to_html(full_html=False, include_plotlyjs=False)}
<h2>Stutters and what overlapped them</h2>
<p>main thread: time the game's busiest thread spent Running / Runnable (waiting for a CPU) / S (sleeping, e.g. waiting
for the GPU) / D (uninterruptible, in the kernel) inside that frame; blocked in: the kernel function of D sleeps.</p>
{st_html}</body></html>"""
    with open(a.out, "w") as f:
        f.write(html)
    for r in runs:
        print(format_metrics(r))
    print(f"report: {a.out}")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "_sampler":
        return sampler_main(sys.argv[2])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="sub", required=True)
    r = sub.add_parser("run")
    r.add_argument("game", choices=sorted(GAMES))
    r.add_argument("--label", required=True)
    r.add_argument("--apps", action="store_true", help="Chromium with YouTube, Wikipedia, a WebGL page")
    r.add_argument("--bg-fill", type=int, metavar="DEFICIT_MB", help="background apps so the game is this short")
    r.add_argument("--auto", choices=["on", "off", "hints"], help="evict.sh auto mode for this run")
    an = sub.add_parser("analyze")
    an.add_argument("dir")
    c = sub.add_parser("compare")
    c.add_argument("dirs", nargs="+")
    c.add_argument("-o", "--out", default="compare.html")
    a = ap.parse_args()
    if a.sub == "run":
        cmd_run(a)
    elif a.sub == "analyze":
        print(format_metrics(analyze(a.dir)))
    else:
        cmd_compare(a)


if __name__ == "__main__":
    main()
