#!/usr/bin/env python3
"""Real-life scenarios on the test VM guest's desktop: a game in the
foreground, the user switches to Chromium (died, waiting for a respawn),
browses, comes back. Measures what the user feels:

  - the compositor's frames (page flips) in every phase: smoothness of the
    game and of Chromium while it scrolls or plays a video;
  - each switch: how long until the shown app draws, the worst frame in the
    first seconds, time until the frame rate is steady again;
  - Chromium's own frames (requestAnimationFrame) while browsing;
  - the game's own frame times (MangoHud, whole run);
  - what the driver did meanwhile (nvtrace: moves, pauses, lock waits).

  ./scenario.py list
  ./scenario.py run NAME [--cycles N] [--game GAME] [--auto on|off|hints]
  ./scenario.py report DIR
  ./scenario.py stop                  a hung or interrupted run and all it started

Runs in the guest as the desktop user (vm.sh ssh), the desktop must be
logged in; uses sudo for nvtrace and evict.sh (NOPASSWD in the guest).
Logs: local/scenario-logs/<name>-<date>/. Run it under ../watchdog.sh.
"""

import argparse
import datetime
import glob
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(LOCAL, "trace"))
import gs          # noqa: E402
import nvtrace     # noqa: E402

NVTRACE = os.path.join(LOCAL, "trace", "nvtrace.py")
EVICT = os.path.join(LOCAL, "evict.sh")
MANGOHUD_DIR = os.path.expanduser("~/.cache/scenario-mangohud")
# GPU stats off: MangoHud's NVML thread takes the RM API lock every poll
# (2026-11-04: kwin waited 41 ms for it); nvtrace samples the GPU itself.
# The logging hotkey must not mean anything to the game (Shift+F2, the
# default, toggled Superposition's wireframe view).
MANGOHUD_CONFIG = (f"no_display,gpu_stats=0,output_folder={MANGOHUD_DIR},autostart_log=1,log_interval=0,"
                   "toggle_logging=Control_R+Shift_R+Scroll_Lock")


def mono():
    return time.clock_gettime(time.CLOCK_MONOTONIC)


class Run:
    def __init__(self, name):
        self.dir = os.path.join(LOCAL, "scenario-logs",
                                f"{name}-{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}")
        os.makedirs(self.dir)
        self.events = open(os.path.join(self.dir, "events.jsonl"), "w")

    def log(self, kind, **kw):
        rec = {"t": round(mono(), 4), "kind": kind, **kw}
        self.events.write(json.dumps(rec) + "\n")
        self.events.flush()
        print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {kind} "
              + " ".join(f"{k}={v}" if not isinstance(v, list) else f"{k}=[{len(v)} values]"
                         for k, v in kw.items()), flush=True)

    def shot(self, name):
        """Screenshot at the end of a phase (what was on screen)."""
        try:
            gs.shot(os.path.join(self.dir, f"shot-{name}.png"))
        except Exception as e:
            self.log("shot_failed", name=name, error=str(e)[:100])

    def phase(self, name):
        """Phase boundary, also as a marker in the trace."""
        subprocess.run(["sudo", "-n", NVTRACE, "mark", "phase " + name], capture_output=True)
        self.log("phase", name=name)


# --------------------------------------------------------------------------
# Chromium over the DevTools protocol
# --------------------------------------------------------------------------

class Chromium:
    PORT = 9401
    PROFILE = "/tmp/gs-chromium"
    # Counts frames of the page; read and reset by frames().
    # Also: when the page last became visible and how long until its first
    # frame after that (__gsVis), the switch latency as the page sees it.
    PROBE = """
(() => { if (window.__gsProbe) return; window.__gsProbe = 1; window.__gsF = []; window.__gsVis = null;
  let last = performance.now(), visAt = null;
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') visAt = performance.now(); });
  const f = t => { window.__gsF.push(t - last); last = t;
    if (visAt !== null) { window.__gsVis = t - visAt; visAt = null; }
    requestAnimationFrame(f); };
  requestAnimationFrame(f); })();
"""

    def __init__(self, urls):
        shutil.rmtree(self.PROFILE, ignore_errors=True)
        cmd = [shutil.which("google-chrome") or "chromium", "--ozone-platform=wayland",
               f"--user-data-dir={self.PROFILE}", f"--remote-debugging-port={self.PORT}",
               "--no-first-run", "--no-default-browser-check", "--password-store=basic",
               "--autoplay-policy=no-user-gesture-required", "--start-maximized"] + urls[:1]
        self.proc = subprocess.Popen(cmd, env=gs.env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        for _ in range(100):
            try:
                self._http("/json/version")
                break
            except OSError:
                time.sleep(0.1)
        time.sleep(2)
        for u in urls[1:]:
            self.open(u)
        self.pid = self.proc.pid
        time.sleep(2)
        for t in self.tabs():
            try:
                self.probe(t)
            except Exception:
                pass

    def _http(self, path, method="GET"):
        req = urllib.request.Request(f"http://127.0.0.1:{self.PORT}{path}", method=method)
        with urllib.request.urlopen(req, timeout=5) as r:
            body = r.read().decode()
        try:
            return json.loads(body or "null")
        except ValueError:          # /json/activate answers "Target activated"
            return body

    def tabs(self):
        return [t for t in self._http("/json/list") if t["type"] == "page"]

    def open(self, url):
        t = self._http("/json/new?" + urllib.request.quote(url, safe=":/?=&"), method="PUT")
        time.sleep(1)
        return t

    def activate(self, idx):
        t = self.tabs()[idx]
        self._http("/json/activate/" + t["id"])
        return t

    def eval(self, js, tab=None, await_promise=False, timeout=10):
        import websocket
        t = tab or self.tabs()[0]
        ws = websocket.create_connection(t["webSocketDebuggerUrl"], timeout=timeout,
                                         suppress_origin=True)
        try:
            ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                                "params": {"expression": js, "returnByValue": True,
                                           "awaitPromise": await_promise}}))
            while True:
                m = json.loads(ws.recv())
                if m.get("id") == 1:
                    return m.get("result", {}).get("result", {}).get("value")
        finally:
            ws.close()

    def visible_tab(self):
        for t in self.tabs():
            try:
                if self.eval("document.visibilityState", t) == "visible":
                    return t
            except Exception:
                pass
        return self.tabs()[0]

    def probe(self, tab):
        self.eval(self.PROBE, tab)

    def frames(self, tab):
        """Frame intervals (ms) since the last call."""
        v = self.eval("(() => { const f = window.__gsF || []; window.__gsF = []; return f; })()", tab)
        return v or []

    def vis_to_frame_ms(self, tab, wait=3.0):
        """Visible -> first frame of the page after the last switch (ms), or None."""
        t_end = time.time() + wait
        while time.time() < t_end:
            v = self.eval("window.__gsVis", tab)
            if v is not None:
                self.eval("window.__gsVis = null", tab)
                return round(v, 1)
            time.sleep(0.1)
        return None

    def first_frame_ms(self, tab):
        """Waits for the page's next two frames: how long until it draws."""
        return self.eval("new Promise(r => { const t0 = performance.now(); requestAnimationFrame(() => "
                         "requestAnimationFrame(() => r(performance.now() - t0))); })", tab,
                         await_promise=True, timeout=15)

    def scroll(self, tab, px, steps=20, pause=0.05):
        for _ in range(steps):
            self.eval(f"window.scrollBy(0, {px // steps})", tab)
            time.sleep(pause)

    def close(self):
        try:
            os.killpg(self.proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


# --------------------------------------------------------------------------
# games
# --------------------------------------------------------------------------

class Superposition:
    """Unigine Superposition (OpenGL, native). The free edition starts only
    from its launcher, and only the performance run and the "game" (an
    endless walk-through scene: the stand-in for a game that keeps running)
    are free; STRESS needs the paid edition. Its fullscreen mode is an
    unmanaged X11 window: always on top, keyboard grabbed, Alt+Tab cannot
    leave it. So the game runs as a borderless 4K window (preset Custom),
    like a game in "windowed fullscreen". Offsets in physical pixels inside
    the launcher window (1024x681 at scale 1.45), from screenshots.
    duration: MangoHud's log_duration, longer than the run."""
    DIR = os.path.expanduser("~/bench/superposition")
    GAME_TAB = (413, 307)
    # (box to open, item to pick) per setting, GAME tab, preset Custom.
    CUSTOM = [((822, 176), (790, 196)),     # Preset: Custom
              ((818, 254), (805, 314)),     # Fullscreen: Borderless Window
              ((818, 281), (810, 460)),     # Resolution: 3840x2160
              ((830, 332), (806, 412)),     # Shaders: Extreme
              ((830, 358), (782, 418))]     # Textures: High
    RUN = (922, 618)
    VRAM_MB = 5600      # what it holds at these settings (vidmem_pma, 2026-11-04)

    def __init__(self, run, inp, duration=3600):
        self.run, self.inp = run, inp
        self.duration = duration

    def _launcher(self):
        for w in gs.windows():
            if w["cls"].startswith("UNIGINE Superposition") or w["caption"].startswith("UNIGINE Superposition"):
                return w
        return None

    def start(self):
        subprocess.run(["pkill", "-x", "launcher"])
        env = dict(gs.env(), MANGOHUD="1",
                   MANGOHUD_CONFIG=MANGOHUD_CONFIG + f",log_duration={self.duration}")
        subprocess.Popen(["mangohud", "./Superposition"], cwd=self.DIR, env=env, start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            w = self._launcher()
            if w:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Superposition launcher did not open")
        gs.activate(w["pid"])     # a click into an unfocused window may only focus it
        time.sleep(2)
        s = gs.screen()[0]["scale"]
        ox, oy = w["x"] * s, w["y"] * s
        click = lambda p: self.inp.click(ox + p[0], oy + p[1])   # noqa: E731
        click(self.GAME_TAB)
        time.sleep(0.8)
        for box, item in self.CUSTOM:
            click(box)
            time.sleep(0.6)
            click(item)
            time.sleep(0.6)
        gs.shot(os.path.join(self.run.dir, "launcher.png"))
        click(self.RUN)
        self.run.log("game_start", game="superposition", mode="game, custom: borderless 3840x2160, shaders extreme, textures high")
        for _ in range(120):
            p = self.pid()
            if p:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Superposition did not start")
        time.sleep(15)    # loading
        gs.minimize("^UNIGINE Superposition v")   # like a game's launcher hiding itself
        gs.activate(p)
        return p

    def pid(self):
        # By process name: a -f pattern also matches command lines that only
        # mention it (watchdog.sh --on-kill "pkill -f bin/superposition").
        r = subprocess.run(["pgrep", "-n", "-x", "superposition"], capture_output=True, text=True)
        p = [int(x) for x in r.stdout.split()]
        return p[0] if p else None

    def focus(self):
        p = self.pid()
        return gs.activate(p) if p else None

    def stop(self):
        p = self.pid()
        if p:
            os.kill(p, signal.SIGTERM)
        time.sleep(3)
        subprocess.run(["pkill", "-x", "launcher"])


GAMES = {"superposition": Superposition}


# --------------------------------------------------------------------------
# scenarios
# --------------------------------------------------------------------------

def browse(run, chrome, inp, seconds, video_tab=None):
    """What a user does while waiting: scroll the page, switch tabs, watch a
    video for a while."""
    t_end = mono() + seconds
    tabs = chrome.tabs()
    i = 0
    while mono() < t_end:
        t = chrome.activate(i % len(tabs))
        chrome.probe(t)
        run.log("chrome_tab", url=t["url"][:80])
        if video_tab is not None and i % len(tabs) == video_tab:
            time.sleep(min(10, max(0, t_end - mono())))
        else:
            chrome.scroll(t, 3000, steps=30)
            chrome.scroll(t, -1500, steps=15)
        run.log("chrome_frames", url=t["url"][:80], frames=chrome.frames(t))
        i += 1


def free_outside_mb():
    """Free vidmem outside the compositor's reserve block (MB)."""
    v = [int(x) for x in open(glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma")[0]).read().split()]
    return v[0] - v[3]


def start_bg(run, sizes):
    """Background "apps" holding VRAM, no window, idle (Discord, Steam,
    Telegram on the real desktop): the memory the driver can take for the
    game. nvt-game checks their content (errors= at the end)."""
    procs = []
    for i, mb in enumerate(sizes):
        name = os.path.join(LOCAL, "nvt-idle-a" if i % 2 == 0 else "nvt-idle-b")
        f = open(os.path.join(run.dir, f"bg-{i}.log"), "w")
        procs.append((subprocess.Popen([name, "--mb", str(mb), "--idle", "--verify-s", "30"], stdout=f,
                                       stderr=subprocess.STDOUT, env=gs.env(), start_new_session=True), f))
        run.log("bg_app", name=os.path.basename(name), mb=int(mb), pid=procs[-1][0].pid)
    if procs:
        time.sleep(8)
    return procs


def switch_to(run, inp, pid, name, tries=4):
    """Alt+Tab as the user does (KWin switches to the previously used window);
    checks which window became active and tabs on if needed. A KWin script
    activation does not raise a window over a fullscreen game (2026-11-04:
    Chromium "shown" while the game stayed on screen)."""
    t0 = mono()
    for k in range(1, tries + 1):
        # Alt held, Tab pressed k times: the k-th most recently used window.
        inp.key_down("alt")
        for _ in range(k):
            inp.key("tab", hold=0.05)
            time.sleep(0.1)
        inp.key_up("alt")
        time.sleep(0.4)
        act = gs.active()
        if act and act.get("pid") == pid:
            run.log("switched", to=name, tabs=k, s=round(mono() - t0, 2))
            return True
    gs.activate(pid)
    time.sleep(0.4)
    act = gs.active()
    ok = bool(act and act.get("pid") == pid)
    run.log("switched", to=name, tabs=tries, fallback="kwin-activate", ok=ok,
            active=(act or {}).get("caption", "?")[:40])
    return ok


def mangohud_toggle(run, inp, why):
    """MangoHud's logging hotkey (see MANGOHUD_CONFIG): stopping writes that stretch to
    a CSV, so every game phase gets its own frame-time file."""
    inp.key("rightctrl+rightshift+scrolllock", hold=0.1)
    run.log("mangohud_toggle", why=why)


def scenario_death(run, a, inp):
    """Play, die, alt-tab to Chromium, browse until the respawn, back to the
    game. N cycles."""
    urls = ["https://en.wikipedia.org/wiki/Graphics_processing_unit",
            "https://www.youtube.com/watch?v=aqz-KE-bpKQ",
            f"file://{LOCAL}/nvt-webgl.html?mode=anim&tex=200"]
    chrome = Chromium(urls)
    run.log("chrome_start", pid=chrome.pid)
    time.sleep(5)
    if a.bg_fill is not None:
        # Fill VRAM before the game starts, so that the game finds bg_fill MB
        # less than it needs: the driver has to move the others out.
        free = free_outside_mb()
        total = max(0, free - GAMES[a.game].VRAM_MB + a.bg_fill)
        run.log("bg_fill", free_outside_mb=free, game_mb=GAMES[a.game].VRAM_MB, deficit_mb=a.bg_fill, bg_total_mb=total)
        # Several apps of up to 900 MB, like Discord, Telegram, Spotify, Steam:
        # a windowless process with >= 1 GB counts as a game for the policy
        # (VIDMEM_EVICT_GAME_BYTES) and is never moved out.
        n = (total + 899) // 900
        a.bg.extend(start_bg(run, [total // n + (1 if i < total % n else 0) for i in range(n)] if total else []))
        # Real background apps are old; the policy leaves processes alone in
        # their first 30 s (VIDMEM_EVICT_NEW_PROC_SEC in vidmem_evict.c).
        run.log("bg_age_wait", s=a.bg_age)
        time.sleep(a.bg_age)
    game = GAMES[a.game](run, inp, duration=a.cycles * (a.play + a.browse + 30) + 120)
    gpid = game.start()
    run.log("game_running", pid=gpid)
    try:
        game.focus()
        time.sleep(1)
        mangohud_toggle(run, inp, "stop the loading part")    # autostart_log began at launch
        for c in range(a.cycles):
            run.phase(f"game-{c}")
            mangohud_toggle(run, inp, f"start game-{c}")
            time.sleep(a.play)
            mangohud_toggle(run, inp, f"end game-{c}")
            run.shot(f"game-{c}")

            run.phase(f"switch-to-chrome-{c}")
            switch_to(run, inp, chrome.pid, "chromium")
            run.phase(f"chrome-{c}")
            browse(run, chrome, inp, a.browse, video_tab=1)
            run.shot(f"chrome-{c}")

            run.phase(f"switch-to-game-{c}")
            switch_to(run, inp, game.pid(), "game")
        run.phase(f"game-end")
        mangohud_toggle(run, inp, "start game-end")
        time.sleep(a.play)
        mangohud_toggle(run, inp, "end game-end")
        run.shot("game-end")
    finally:
        run.phase("stop")
        # nvt-webgl.html checks its own pixels: bad=N means GPU memory came back wrong.
        try:
            for t in chrome.tabs():
                if t["title"].startswith("NVT"):
                    run.log("webgl_check", title=t["title"])
        except Exception:
            pass
        game.stop()
        chrome.close()


SCENARIOS = {"death": scenario_death}


# What a run leaves behind if it is killed: matched in /proc/*/cmdline.
LEFTOVERS = ["scenario/scenario.py run", "bench/bench.py run", "user-data-dir=/tmp/gs-chromium", "/nvt-idle-",
             "trace/nvtrace.py record"]


def cmd_stop(a):
    """Stops a run and everything it started; never its own caller (a
    pgrep -f pattern matches the command line that contains it)."""
    skip = set()
    p = os.getpid()
    while p > 1:
        skip.add(p)
        try:
            p = int(open(f"/proc/{p}/stat").read().rsplit(")", 1)[1].split()[1])
        except (OSError, IndexError, ValueError):
            break
    found = []
    for d in os.listdir("/proc"):
        if not d.isdigit() or int(d) in skip:
            continue
        try:
            cmd = open(f"/proc/{d}/cmdline", "rb").read().replace(b"\0", b" ").decode(errors="replace")
            comm = open(f"/proc/{d}/comm").read().strip()
        except OSError:
            continue
        if comm in ("superposition", "launcher") or any(x in cmd for x in LEFTOVERS):
            found.append((int(d), comm, cmd[:80]))
    for pid, comm, cmd in found:
        sig = signal.SIGINT if ("scenario.py" in cmd or "nvtrace.py" in cmd or "bench.py" in cmd) else signal.SIGTERM
        try:
            os.kill(pid, sig)
        except PermissionError:
            subprocess.run(["sudo", "-n", "kill", f"-{int(sig)}", str(pid)])
        except ProcessLookupError:
            pass
        print(f"stop: {pid} {comm} {cmd}")
    time.sleep(5)
    for pid, comm, cmd in found:
        if os.path.exists(f"/proc/{pid}"):
            subprocess.run(["sudo", "-n", "kill", "-9", str(pid)])
            print(f"stop: killed {pid} {comm}")


def check_setup():
    """What a run needs from the desktop, checked first: a run without it
    measures nothing (2026-11-04: the KWin hints script was missing after a
    guest reboot, the policy was off, the game was simply denied memory)."""
    try:
        head = open("/proc/driver/nvidia/vidmem_hints").readline()
    except OSError:
        sys.exit("scenario: /proc/driver/nvidia/vidmem_hints missing: patched driver not loaded?")
    m = re.search(r"generation (\d+)", head)
    if not m or int(m.group(1)) == 0:
        sys.exit("scenario: the KWin window-hints script reports nothing (local/kwin-vram-hints/install.sh, "
                 "then restart the session): the eviction policy is off without it")


def cmd_run(a):
    check_setup()
    run = Run(a.name)
    print(f"scenario {a.name}: logs in {run.dir}", flush=True)
    if a.auto:
        subprocess.run(["sudo", "-n", EVICT, "auto", a.auto], capture_output=True)
        run.log("auto", mode=a.auto)
    os.makedirs(MANGOHUD_DIR, exist_ok=True)
    before = set(os.listdir(MANGOHUD_DIR))
    total = a.cycles * (a.play + a.browse + 40) + a.play + 180
    trace = subprocess.Popen(["sudo", "-n", NVTRACE, "record", "-d", str(total), "-o", os.path.join(run.dir, "trace")],
                             stdout=open(os.path.join(run.dir, "nvtrace.log"), "w"), stderr=subprocess.STDOUT)
    time.sleep(2)
    inp = gs.Input()
    a.bg = start_bg(run, [int(x) for x in (a.bg_mb or "").split(",") if x])
    bg = a.bg
    try:
        SCENARIOS[a.name](run, a, inp)
    finally:
        inp.close()
        for p, f in bg:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
            f.close()
        subprocess.run(["sudo", "-n", "pkill", "-INT", "-f", "nvtrace.py record -d"], capture_output=True)
        trace.wait(timeout=60)
        time.sleep(1)
        for f in set(os.listdir(MANGOHUD_DIR)) - before:
            shutil.move(os.path.join(MANGOHUD_DIR, f), os.path.join(run.dir, f))
        run.log("end")
    print(report(run.dir), flush=True)


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def stats_ms(iv):
    if not iv:
        return "no frames"
    med = statistics.median(iv)
    s = sorted(iv)
    p99 = s[min(len(s) - 1, int(len(s) * 0.99))]
    slow = sum(1 for x in iv if x > max(25, 2 * med))
    return (f"{len(iv):5} frames, {1000 / med if med else 0:5.0f} fps median, p99 {p99:6.1f} ms, "
            f"max {max(iv):7.1f} ms, slow {slow}")


def steady_after(ts, t0, med):
    """Seconds after t0 until 30 frames in a row are within 1.5x the median."""
    run = 0
    for i in range(1, len(ts)):
        if ts[i] < t0:
            continue
        if ts[i] - ts[i - 1] <= 1.5 * med:
            run += 1
            if run >= 30:
                return ts[i - 29] - t0
        else:
            run = 0
    return None


def report(d):
    out = []
    w = out.append
    events = [json.loads(l) for l in open(os.path.join(d, "events.jsonl"))]
    tdir = os.path.join(d, "trace")
    tr = []
    if os.path.exists(os.path.join(tdir, "trace.txt")):
        tr, samples, meta, klog = nvtrace.load_dir(tdir)
    flips = [e["ts"] for e in tr if e["event"] == "drm_vblank_event_delivered"]
    phases = [(e["t"], e["name"]) for e in events if e["kind"] == "phase"]
    end = events[-1]["t"]
    w(f"scenario report: {d}")
    for k in ("auto", "game_start"):
        for e in events:
            if e["kind"] == k:
                w(f"  {k}: " + " ".join(f"{a}={b}" for a, b in e.items() if a not in ("t", "kind")))
    w("")
    w("== Frames on screen (page flips) per phase")
    game_iv = [b - a for a, b in zip(flips, flips[1:])]
    game_med = statistics.median(game_iv) if game_iv else 0.016
    for i, (t, name) in enumerate(phases):
        t1 = phases[i + 1][0] if i + 1 < len(phases) else end
        f = [x for x in flips if t <= x < t1]
        iv = [(b - a) * 1000 for a, b in zip(f, f[1:])]
        drv = [e for e in tr if t <= e["ts"] < t1 and e["event"] in ("vidmem_move", "vidmem_pause", "vidmem_makeroom")]
        waits = [e for e in tr if t <= e["ts"] < t1 and e["event"] == "rm_lock" and nvtrace.fi(e["f"], "wait_us") >= 5000
                 and e["task"].startswith(("kwin", "Xwayland", "superposition", "chrom"))]
        w(f"  {name:22} {t1 - t:6.1f} s  {stats_ms(iv)}"
          + (f"; driver: {len(drv)} moves/pauses" if drv else "")
          + (f"; {len(waits)} lock waits >= 5 ms (max {max(nvtrace.fi(e['f'], 'wait_us') for e in waits) / 1000:.0f} ms)" if waits else ""))
        if name.startswith("switch-to-game") or name.startswith("switch-to-chrome"):
            win = [x for x in flips if t <= x < t + 3]
            wiv = [(b - a) * 1000 for a, b in zip(win, win[1:])]
            st = steady_after(flips, t, statistics.median(iv) / 1000 if iv else game_med)
            w(f"      first 3 s after the switch: worst frame {max(wiv) if wiv else 0:.1f} ms, "
              f"steady after {st:.2f} s" if st is not None else "      first 3 s: no steady frame rate")
            for e in drv[:4]:
                w(f"      driver: {nvtrace.describe(e)}")
    w("")
    moves = [e for e in tr if e["event"] == "vidmem_move"]
    pauses = [e for e in tr if e["event"] == "vidmem_pause"]
    mrs = [e for e in tr if e["event"] == "vidmem_makeroom"]
    w("== Eviction (driver)")
    per = {}
    for e in moves:
        k = (e["f"].get("comm"), e["f"].get("op"))
        x = per.setdefault(k, [0, 0, 0])
        x[0] += 1
        x[1] += nvtrace.fi(e["f"], "kb") >> 10
        x[2] += nvtrace.fi(e["f"], "failed")
    for (comm, op), (n, mb, failed) in sorted(per.items()):
        w(f"  {op:8} {comm:16} {n:3}x {mb:6} MB" + (f", FAILED {failed}" if failed else ""))
    if pauses:
        w(f"  paused: {len(pauses)}x, longest {max(nvtrace.fi(e['f'], 'paused_us') for e in pauses) / 1000:.1f} ms")
    if mrs:
        w(f"  allocations that waited for room: {len(mrs)}x, longest {max(nvtrace.fi(e['f'], 'us') for e in mrs) / 1000:.1f} ms")
    pressure = any(e["kind"] == "bg_fill" for e in events)
    if not moves:
        w("  no evictions" + ("  <-- INVALID: a pressure run must make the driver move memory" if pressure else ""))
    w("")
    w("== Switches")
    for e in events:
        if e["kind"] in ("switched",):
            w("  " + e["kind"] + ": " + " ".join(f"{a}={b}" for a, b in e.items() if a not in ("t", "kind")))
    w("")
    w("== Chromium's own frames while browsing (requestAnimationFrame)")
    per = {}
    for e in events:
        if e["kind"] == "chrome_frames":
            per.setdefault(e["url"][:50], []).extend(e["frames"])
    for u, iv in per.items():
        w(f"  {u:50} {stats_ms(iv[1:])}")
    w("")
    for e in events:
        if e["kind"] == "webgl_check":
            bad = re.search(r"bad=(\d+)", e["title"])
            w(f"== WebGL page self-check: {e['title']}" + ("  <-- CORRUPTION" if bad and int(bad.group(1)) else ""))
    for f in sorted(glob.glob(os.path.join(d, "bg-*.log"))):
        lines = open(f).read().splitlines()
        res = [l for l in lines if l.startswith("RESULT") or "errors=" in l]
        w(f"== Background app {os.path.basename(f)}: " + (res[-1][:200] if res else "no result line"))
    # MangoHud: one CSV per toggle stretch; the first is the loading part,
    # then game-0, game-1, ..., game-end (scenario_death's toggles).
    mh_files = sorted(f for f in glob.glob(os.path.join(d, "*.csv")) if not f.endswith("_summary.csv"))
    game_phases = [n for _, n in phases if n.startswith("game-")]
    if mh_files:
        w("== The game's own frames per phase (MangoHud; the first seconds after a switch back are")
        w("   in the phase that follows it)")
        for name, f in zip(["loading"] + game_phases, mh_files):
            ft = []
            for line in open(f).read().splitlines()[3:]:
                parts = line.split(",")
                try:
                    ft.append(float(parts[1]))
                except (IndexError, ValueError):
                    pass
            first = ft[:int(3 * 1000 / statistics.median(ft))] if ft else []
            w(f"  {name:12} {stats_ms(ft)}"
              + (f"; first 3 s: worst {max(first):.1f} ms" if first else ""))
        w("")
    mh = sorted(glob.glob(os.path.join(d, "*_summary.csv")))
    if mh:
        w("== Game frame times (MangoHud, whole run)")
        for f in mh:
            rows = open(f).read().splitlines()
            if len(rows) >= 2:
                hdr, val = rows[0].split(","), rows[1].split(",")
                w("  " + os.path.basename(f) + ": " + ", ".join(f"{h} {v}" for h, v in list(zip(hdr, val))[:7]))
    if tr:
        w("")
        w(f"Driver detail: ../trace/nvtrace.py report {tdir}  (timeline: {tdir}/trace.json in ui.perfetto.dev)")
    text = "\n".join(out) + "\n"
    with open(os.path.join(d, "report.txt"), "w") as f:
        f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="sub", required=True)
    sub.add_parser("list")
    sub.add_parser("stop")
    r = sub.add_parser("run")
    r.add_argument("name", choices=sorted(SCENARIOS))
    r.add_argument("--cycles", type=int, default=3)
    r.add_argument("--play", type=int, default=40, help="seconds in the game per cycle")
    r.add_argument("--browse", type=int, default=25, help="seconds in Chromium per cycle")
    r.add_argument("--game", default="superposition", choices=sorted(GAMES))
    r.add_argument("--auto", choices=["on", "off", "hints"], help="evict.sh auto mode for this run")
    r.add_argument("--bg-mb", help="background apps holding VRAM, MB each, e.g. 1500,1000")
    r.add_argument("--bg-fill", type=int, metavar="DEFICIT_MB",
                   help="size background apps so the game is this many MB short: eviction must happen")
    r.add_argument("--bg-age", type=int, default=40,
                   help="seconds the background apps run before the game starts (default 40)")
    rp = sub.add_parser("report")
    rp.add_argument("dir")
    a = ap.parse_args()
    if a.sub == "list":
        for k, f in SCENARIOS.items():
            print(f"{k:10} {f.__doc__.strip().splitlines()[0]}")
    elif a.sub == "run":
        cmd_run(a)
    elif a.sub == "stop":
        cmd_stop(a)
    else:
        print(report(a.dir))


if __name__ == "__main__":
    main()
