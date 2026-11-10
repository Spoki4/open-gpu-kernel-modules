#!/usr/bin/env python3
"""Records a gaming session on the host so problems can be studied later.

Start a recording (by hand, or automatically when GameMode switches on),
mark problems with a hotkey while playing, stop. The root part (driver
events, journal, tracefs, procfs) runs in the gamerec service, or in the
terminal through sudo if the service is not installed. Each recording has
two halves:

local/game-logs/<id>/            yours: frame times, screenshots, summary
  mangohud/        frame times of every game started with MangoHud
  shots/           the screenshots taken with marks
  summary.txt      `gamerec.sh report`
  system ->        /var/lib/gamerec/<id>/, written by root, readable by you:
    nvtrace/         the patched driver's events, RM lock waits, page flips,
                     kernel log, VRAM / GPU load / PCIe samples, the whole
                     session (local/trace/nvtrace.py record; with the stock
                     driver only samples.jsonl, NVML)
    journal.log      the whole system journal (kwin, plasmashell, coredumps)
    sys.jsonl        once a second: RAM, swap, PSI, free VRAM, power profile,
                     V-Cache mode, CPU energy preference
    hints.log        what the KWin script told the driver, on every change
    evict-status.log evict.sh status at the start and the end (per process
                     VRAM / RAM; the per-step numbers are in nvtrace's
                     vidmem_proc events: reading the status holds the RM
                     locks 9-14 ms, a stutter every 10 s when it ran that often)
    problems.log     journal lines that look like trouble (Xid, failed channel
                     allocs, eviction failures, coredumps, OOM)
    marks/NN_.../    one per mark (hotkey, `gamerec.sh mark`, or automatic on a
                     problem line): flight.pftrace (Perfetto: the last ~minute
                     of scheduler, page flips, fences, driver events; open at
                     https://ui.perfetto.dev), evict status, nvidia-smi, ps,
                     PSI, coredump info
    flight-end.pftrace  the last ~minute before the stop

  sudo ./gamerec.sh install         the service (once, and after changing this file)
  sudo ./gamerec.sh uninstall
  ./gamerec.sh record [--hud]       until Ctrl-C (through the service, or sudo)
  ./gamerec.sh auto start|end       GameMode's hook: a recording while it is on
  ./gamerec.sh mark [TEXT]          mark a problem (the hotkey runs this)
  ./gamerec.sh stop                 stop the running recording
  ./gamerec.sh status
  ./gamerec.sh hotkey [remove]      KDE shortcuts: Meta+Ctrl+M mark, Meta+Ctrl+H HUD
  ./gamerec.sh hotkey mark|hud KEY  another key for one of them
  ./gamerec.sh hud [on|off]         show / hide MangoHud (toggle), also in running games
  ./gamerec.sh steam                restart Steam so every Vulkan game gets MangoHud
  ./gamerec.sh report [DIR]         summary: marks, problems, frame times

MangoHud logs only games that load it: either `gamerec.sh steam` once (Steam
started with MANGOHUD=1 and our config file), or per game in Steam launch
options:  MANGOHUD_CONFIGFILE=~/.config/MangoHud/gamerec.conf mangohud %command%
Bottles: the bottle's MangoHud switch. The config logs only while a
recording runs; otherwise it does nothing visible.

The service runs a root-owned copy (/usr/local/lib/gamerec), never this
checkout: root must not run code that the user's processes can change. It
takes requests only from that user's inbox (/run/gamerec/inbox, 0700) and
writes only into root-owned /var/lib/gamerec.
"""

import argparse
import datetime
import glob
import json
import os
import queue
import re
import shutil
import signal
import stat
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = os.path.dirname(HERE)             # the checkout, or the service's copy
LOGS = os.path.join(LOCAL, "game-logs")
CURRENT = os.path.join(LOGS, "current")
WRAPPER = os.path.join(LOCAL, "gamerec.sh")
TRACEBOX = os.path.join(LOCAL, ".perfetto", "v58.2", "linux-amd64", "tracebox")
FLIGHT_CFG = os.path.join(HERE, "flight.cfg")
NVTRACE = os.path.join(LOCAL, "trace", "nvtrace.py")
EVICT = os.path.join(LOCAL, "evict.sh")
# What the service copy needs, relative to LOCAL.
INSTALL_FILES = ["gamerec/gamerec.py", "gamerec/flight.cfg", "trace/nvtrace.py", "evict.sh",
                 ".perfetto/v58.2/linux-amd64/tracebox"]
INSTALL_DIR = "/usr/local/lib/gamerec"
UNIT = "/etc/systemd/system/gamerec.service"
SYS_LOGS = "/var/lib/gamerec"
RUN = "/run/gamerec"
INBOX = RUN + "/inbox"                   # the user's requests
STATE = RUN + "/state.json"              # the running recording
LAST = RUN + "/last.json"                # the last finished one
DAEMON_PID = RUN + "/daemon.pid"
SOCK_DIR = RUN + "/pf"                   # short: unix socket paths are limited to 108 bytes
ID_RE = re.compile(r"^\d{8}_\d{6}$")
TRACEFS = "/sys/kernel/tracing"
NVTRACE_MARKER = TRACEFS + "/instances/nvtrace/trace_marker"
HINTS = "/proc/driver/nvidia/vidmem_hints"
MH_CONF = os.path.expanduser("~/.config/MangoHud/gamerec.conf")
# Bottles (flatpak) sees only its own ~/.var/app dir and ~/Games: MangoHud there
# (the bottle's MangoHud switch) reads the app's own config and logs into a
# folder the sandbox can write; the stop moves those logs into the recording.
BOTTLES = os.path.expanduser("~/.var/app/com.usebottles.bottles")
BOTTLES_MH_CONF = os.path.join(BOTTLES, "config", "MangoHud", "MangoHud.conf")
BOTTLES_MH_OUT = os.path.join(BOTTLES, "data", "gamerec-mangohud")
MH_HEADER = "# Written by local/gamerec.sh; rewritten on every record and stop."
APPS = os.path.expanduser("~/.local/share/applications")     # the hotkeys' .desktop files
DEFAULT_KEY = "Meta+Ctrl+M"

# Journal lines worth a mark of their own (the flight recorder holds only the
# last minute, so it is cloned right away). A broken state can repeat a line
# for hours, so per key the next auto mark waits 30 s, then 5 min, then 30 min;
# at most AUTO_MARKS_MAX auto marks per recording (all lines go to problems.log).
TRIGGERS = [
    ("xid", re.compile(r"NVRM: Xid|nvidia_dev_xid")),
    ("gsp-alloc", re.compile(r"GspRmAlloc failed")),
    ("rm-no-memory", re.compile(r"ctxBufPoolReserve: Failed|NV_ERR_NO_MEMORY")),
    ("va-map", re.compile(r"can't update VA space for mapping")),
    ("evict-fail", re.compile(r"vidmem evict: .*(failed [1-9]|failed: 0x|INVALID|moved 0x)")),
    ("nvidia-drm", re.compile(r"\[nvidia-drm\].*ERROR|nvidia-modeset: ERROR")),
    ("coredump", re.compile(r"systemd-coredump\[\d+\]: Process (\d+) \((.*?)\) of user \d+ (dumped core|terminated abnormally)")),
    ("gpu-reset", re.compile(r"GPU reset|graphics reset|GL_CONTEXT_LOST|ring \S+ timeout|amdgpu: .*hang", re.I)),
    ("oom-kill", re.compile(r"Out of memory: Killed process|oom-kill:")),
    ("gpu-hang", re.compile(r"NVRM: .*(GPU has fallen off the bus|RmInitAdapter failed|Lockup)")),
]
AUTO_BACKOFF_S = (30, 300, 1800)
AUTO_MARKS_MAX = 20


def now_s():
    return datetime.datetime.now().strftime("%H:%M:%S")


def rfile(path):
    with open(path) as f:
        return f.read()


def wjson(path, obj):
    with open(path + ".tmp", "w") as f:
        json.dump(obj, f)
    os.replace(path + ".tmp", path)


def rjson(path):
    try:
        return json.loads(rfile(path))
    except (OSError, ValueError):
        return None


def patched():
    return bool(glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma"))


def notify(text):
    subprocess.run(["notify-send", "-a", "GameRec", "-t", "3000", "GameRec", text],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def pid_alive(path):
    try:
        os.kill(int(rfile(path)), 0)
    except PermissionError:     # root's process, alive
        return True
    except (OSError, ValueError):
        return False
    return True


# --------------------------------------------------------------------------
# MangoHud
# --------------------------------------------------------------------------

def write_mangohud_conf(out_dir=None, hud=False):
    """With out_dir: log every frame there from 1 s after the game starts
    (games already running reload the file and start a new log). Without:
    no log. The HUD stays invisible unless `hud` (no_display would switch
    logging off too)."""
    lines = [MH_HEADER]
    if hud:
        lines += ["fps", "frametime", "frame_timing", "vram", "ram", "gpu_stats",
                  "position=top-left", "font_size=18", "background_alpha=0.3"]
    else:
        lines += ["fps_only", "alpha=0.0", "background_alpha=0.0", "gpu_stats=0"]
    log = ["autostart_log=1", "log_interval=0"]
    targets = [(MH_CONF, lines + (log + [f"output_folder={out_dir}"] if out_dir else []))]
    if os.path.isdir(BOTTLES):
        os.makedirs(BOTTLES_MH_OUT, exist_ok=True)
        try:
            mine = rfile(BOTTLES_MH_CONF).startswith(MH_HEADER)
        except OSError:
            mine = True
        if mine:
            targets.append((BOTTLES_MH_CONF, lines + (log + [f"output_folder={BOTTLES_MH_OUT}"] if out_dir else [])))
        else:
            print(f"gamerec: {BOTTLES_MH_CONF} is not ours, left alone: Bottles games log no frame times")
    for path, ls in targets:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".tmp", "w") as f:
            f.write("\n".join(ls) + "\n")
        os.replace(path + ".tmp", path)


def collect_bottles_logs(d):
    """Moves the MangoHud logs Bottles games wrote during the recording."""
    for p in glob.glob(os.path.join(BOTTLES_MH_OUT, "*.csv")):
        shutil.move(p, os.path.join(d, "mangohud", os.path.basename(p)))


def cmd_hud(a):
    """Shows or hides the overlay in running games: MangoHud watches its
    config file and reloads it (the frame log restarts in a new file)."""
    try:
        conf = rfile(MH_CONF)
    except OSError:
        conf = ""
    on = conf.startswith(MH_HEADER) and "\nfps_only" not in conf
    want = {"on": True, "off": False}.get(a.state, not on)
    m = re.search(r"^output_folder=(.*)$", conf, re.M)
    write_mangohud_conf(m.group(1) if m and "autostart_log" in conf else None, want)
    notify("HUD " + ("on" if want else "off"))
    print(f"gamerec: MangoHud overlay {'on' if want else 'off'}")


def steam_has_mangohud():
    for p in subprocess.run(["pgrep", "-x", "steam"], capture_output=True, text=True).stdout.split():
        try:
            if f"MANGOHUD_CONFIGFILE={MH_CONF}".encode() in open(f"/proc/{p}/environ", "rb").read():
                return True
        except OSError:
            pass
    return False


def cmd_steam(a):
    if steam_has_mangohud():
        print("gamerec: Steam already runs with MangoHud (gamerec.conf)")
        return
    if subprocess.run(["pgrep", "-x", "steam"], capture_output=True).stdout:
        print("gamerec: closing Steam ...")
        subprocess.run(["steam", "-shutdown"], capture_output=True)
        for _ in range(60):
            if not subprocess.run(["pgrep", "-x", "steam"], capture_output=True).stdout:
                break
            time.sleep(1)
        else:
            sys.exit("gamerec: Steam did not exit in 60 s; close it and run this again")
    if not os.path.exists(MH_CONF):
        write_mangohud_conf()
    env = dict(os.environ, MANGOHUD="1", MANGOHUD_CONFIGFILE=MH_CONF)
    subprocess.Popen(["steam", "-silent"], env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("gamerec: Steam started with MANGOHUD=1: Vulkan games (Proton/DXVK/VKD3D) log frame "
          "times while a recording runs. OpenGL games need `mangohud %command%` in their launch options.")


# --------------------------------------------------------------------------
# hotkeys (KDE global shortcuts that run `gamerec.sh mark` / `hud`)
# --------------------------------------------------------------------------

QT_MODS = {"meta": 0x10000000, "ctrl": 0x04000000, "alt": 0x08000000, "shift": 0x02000000}

# action -> (desktop file / kglobalaccel component, default key, what it runs, description)
HOTKEYS = {
    "mark": ("gamerec-mark.desktop", DEFAULT_KEY, "mark", "Mark a problem"),
    "hud": ("gamerec-hud.desktop", "Meta+Ctrl+H", "hud", "Show / hide MangoHud"),
}


def qt_key(spec):
    code = 0
    *mods, key = spec.split("+")
    for m in mods:
        code |= QT_MODS[m.strip().lower()]
    key = key.strip().upper()
    if len(key) == 1 and (key.isalnum()):
        code |= ord(key)
    elif re.fullmatch(r"F([1-9]|[12][0-9]|3[0-5])", key):
        code |= 0x01000030 + int(key[1:]) - 1
    else:
        sys.exit(f"gamerec: key '{spec}' not understood (use e.g. Meta+Ctrl+M or Ctrl+Alt+F10)")
    return code


def key_name(code):
    """A Qt key code back to text, e.g. Meta+Ctrl+M."""
    mods = [m.capitalize() for m in ("meta", "ctrl", "alt", "shift") if code & QT_MODS[m]]
    key = code & 0x01ffffff
    if 0x01000030 <= key <= 0x01000052:
        name = f"F{key - 0x01000030 + 1}"
    elif 0x20 < key < 0x7f:
        name = chr(key).upper()
    else:
        name = hex(key)
    return "+".join(mods + [name])


def hotkey_text(action):
    """'Meta+Ctrl+M' (or several, joined), '' if no key is bound."""
    return " or ".join(key_name(k) for k in hotkey_keys(action))


def kglobalaccel():
    import dbus
    bus = dbus.SessionBus()
    return dbus, dbus.Interface(bus.get_object("org.kde.kglobalaccel", "/kglobalaccel"), "org.kde.KGlobalAccel")


def hotkey_keys(action="mark"):
    """The keys bound to an action now, as Qt key codes ([] if none)."""
    try:
        dbus, ka = kglobalaccel()
        keys = ka.shortcutKeys([HOTKEYS[action][0], "_launch", "", ""])
        return [int(k[0][0]) for k in keys if len(k[0])]
    except Exception:
        return []


def set_hotkey(dbus, ka, action, key):
    comp, _, cmd, desc = HOTKEYS[action]
    aid = [comp, "_launch", "GameRec", desc]
    desktop = os.path.join(APPS, comp)
    if key == "remove":
        ka.setShortcutKeys(aid, dbus.Array([], signature="(ai)"), dbus.UInt32(4))
        ka.unRegister(aid)
        if os.path.exists(desktop):
            os.unlink(desktop)
        print(f"gamerec: {action} hotkey removed")
        return
    code = qt_key(key)
    os.makedirs(os.path.dirname(desktop), exist_ok=True)
    with open(desktop, "w") as f:
        f.write(f"[Desktop Entry]\nType=Application\nName=GameRec: {desc.lower()}\n"
                f"Exec={WRAPPER} {cmd}\nNoDisplay=true\nX-KDE-GlobalAccel-CommandShortcut=true\n")
    subprocess.run(["kbuildsycoca6"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    others = [s for s in ka.globalShortcutsByKey(dbus.Struct((dbus.Array([code], signature="i"),), signature=None),
                                                 dbus.Struct((dbus.Int32(0),), signature=None))
              if s[0] != "_launch" or s[2] != comp]
    if others:
        sys.exit(f"gamerec: {key} is taken by {others[0][3]} / {others[0][1]}; pick another key")
    ka.doRegister(aid)
    ka.setShortcutKeys(aid, dbus.Array([dbus.Struct((dbus.Array([code], signature="i"),), signature=None)],
                                       signature="(ai)"), dbus.UInt32(4))     # NoAutoloading: set it as given
    if code not in hotkey_keys(action):
        sys.exit("gamerec: KDE did not take the shortcut; set it by hand: System Settings > Keyboard > "
                 f"Shortcuts > Add New > Command: {WRAPPER} {cmd}")
    print(f"gamerec: {key}: {desc.lower()} (runs `{WRAPPER} {cmd}`)")


def cmd_hotkey(a):
    """hotkey: both with default keys; hotkey remove; hotkey mark|hud [KEY|remove]."""
    dbus, ka = kglobalaccel()
    if a.args and a.args[0] in HOTKEYS:
        set_hotkey(dbus, ka, a.args[0], a.args[1] if len(a.args) > 1 else HOTKEYS[a.args[0]][1])
        return
    if a.args and a.args[0] != "remove":
        sys.exit("usage: gamerec.sh hotkey [remove] | hotkey mark|hud [KEY|remove]")
    for action, (_, default, _, _) in HOTKEYS.items():
        set_hotkey(dbus, ka, action, "remove" if a.args else default)


# --------------------------------------------------------------------------
# user side: talking to the root part
# --------------------------------------------------------------------------

def daemon_running():
    return pid_alive(DAEMON_PID)


def state():
    """The running recording ({"id", "auto", "stopping", ...}) or None."""
    return rjson(STATE) if daemon_running() else None


def send(kind, **req):
    req["kind"] = kind
    path = os.path.join(INBOX, f"{time.time_ns()}-{kind}.json")
    wjson(path, req)


def wait_for(cond, timeout, step=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return None


def prepare_user_dir(rid, hud):
    d = os.path.join(LOGS, rid)
    for sub in ("mangohud", "shots"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    if not os.path.lexists(os.path.join(d, "system")):
        os.symlink(os.path.join(SYS_LOGS, rid), os.path.join(d, "system"))
    if os.path.islink(CURRENT):
        os.unlink(CURRENT)
    os.symlink(rid, CURRENT)
    write_mangohud_conf(os.path.join(d, "mangohud"), hud)
    return d


def finish_user_dir(d):
    write_mangohud_conf(None, False)
    collect_bottles_logs(d)
    if os.path.islink(CURRENT) and os.path.realpath(CURRENT) == os.path.realpath(d):
        os.unlink(CURRENT)
    return report(d, quiet=True)


def new_id():
    return datetime.datetime.now().strftime("%Y%m%d_%H%M%S")


def print_hints(d):
    print(f"gamerec: recording into {d}")
    mark_key, hud_key = hotkey_text("mark"), hotkey_text("hud")
    print("  mark a problem: " + (mark_key or f"`{WRAPPER} mark` (no hotkey yet: `{WRAPPER} hotkey`)"))
    print("  show the HUD:   " + (hud_key or f"`{WRAPPER} hud`") + " (switches in running games too)")
    print("  frame times:    " + ("Steam runs with MangoHud" if steam_has_mangohud() else
                                  f"games started with MangoHud only (`{WRAPPER} steam`, or launch options)"))
    for y in glob.glob(os.path.join(BOTTLES, "data", "bottles", "bottles", "*", "bottle.yml")):
        on = re.search(r"^\s*mangohud:\s*true", rfile(y), re.M)
        print(f"  bottle {os.path.basename(os.path.dirname(y))}: " +
              ("MangoHud on, frame times logged" if on else "MangoHud off (Bottles: bottle settings, MangoHud)"))
    print("  stop:           Ctrl-C here (or `gamerec.sh stop`)\n")


def cmd_record(a):
    if os.geteuid() == 0:
        sys.exit("gamerec: run as yourself (the root part is the service, or sudo)")
    st = state()
    if st:
        sys.exit(f"gamerec: already recording ({st['id']}); `gamerec.sh stop` first")
    rid = new_id()
    d = prepare_user_dir(rid, a.hud)
    print_hints(d)
    opts = {"no_perfetto": a.no_perfetto, "evict_trace": a.evict_trace}
    if daemon_running():
        send("start", id=rid, auto=False, **opts)
        if not wait_for(lambda: (state() or {}).get("id") == rid, 15):
            finish_user_dir(d)
            sys.exit("gamerec: the service did not start the recording (journalctl -u gamerec)")
        stop = []
        old = signal.signal(signal.SIGINT, lambda *_: stop.append(1))
        log, pos = os.path.join(SYS_LOGS, rid, "gamerec.log"), 0
        try:
            while True:
                try:
                    with open(log) as f:
                        f.seek(pos)
                        out = f.read()
                        pos = f.tell()
                    print(out, end="", flush=True)
                except OSError:
                    pass
                if (rjson(LAST) or {}).get("id") == rid:
                    break
                if stop and stop[0] == 1:
                    send("stop")
                    stop.append(2)
                time.sleep(0.5)
        finally:
            signal.signal(signal.SIGINT, old)
    else:
        print("gamerec: no service (sudo ./local/gamerec.sh install), running the root part through sudo")
        cmd = ["sudo", sys.executable, os.path.abspath(__file__), "_daemon", "--uid", str(os.getuid()),
               "--gid", str(os.getgid()), "--session", rid]
        cmd += ["--no-perfetto"] if a.no_perfetto else []
        cmd += ["--evict-trace"] if a.evict_trace else []
        # Ctrl-C reaches it (same process group); this side waits for it.
        old = signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            subprocess.run(cmd)
        finally:
            signal.signal(signal.SIGINT, old)
    print(finish_user_dir(d), end="")


def cmd_auto(a):
    """GameMode's hook: start when a game turns it on, stop when the last
    one turns it off; only recordings started here are stopped here."""
    if a.what == "start":
        if not daemon_running():
            notify("gamerec service not installed: no recording")
            return
        st = state()
        if st and st.get("stopping"):         # the previous game's recording is still being written
            wait_for(lambda: not state(), 120, 0.5)
            st = state()
        if st:
            return
        rid = new_id()
        d = prepare_user_dir(rid, False)
        send("start", id=rid, auto=True)
        if wait_for(lambda: (state() or {}).get("id") == rid, 15):
            notify("recording started")
        else:
            finish_user_dir(d)
            notify("recording did not start (journalctl -u gamerec)")
    else:
        st = state()
        if not st or not st.get("auto"):
            return
        send("stop")
        wait_for(lambda: (rjson(LAST) or {}).get("id") == st["id"], 600, 0.5)
        summary = finish_user_dir(os.path.join(LOGS, st["id"]))
        m = re.search(r"^marks: (\d+)", summary, re.M)
        notify(f"recording stopped, {m.group(1) if m else 0} marks: local/game-logs/{st['id']}")


def cmd_mark(a):
    st = state()
    # Every call, also without a recording: shows whether the hotkey fires.
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, "mark-calls.log"), "a") as f:
        f.write(f"{datetime.datetime.now().isoformat(timespec='seconds')} "
                f"{st['id'] if st else 'not recording'} {' '.join(a.text)}\n")
    if st is None:
        notify("not recording")
        sys.exit("gamerec: no recording is running")
    req = {"text": " ".join(a.text) or "problem (hotkey)", "t": time.time(), "shot": None}
    if not a.no_shot:
        shots = os.path.join(LOGS, st["id"], "shots")
        os.makedirs(shots, exist_ok=True)
        shot = os.path.join(shots, datetime.datetime.now().strftime("%H%M%S") + ".png")
        # Every monitor (the page that "broke" was on the other one, BACKLOG #15), no window, no notification.
        if subprocess.run(["spectacle", "-b", "-n", "-f", "-o", shot], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, timeout=15).returncode == 0 and os.path.exists(shot):
            req["shot"] = "shots/" + os.path.basename(shot)
    send("mark", **req)
    notify("mark saved")
    print(f"gamerec: mark sent to {st['id']}")


def cmd_stop(a):
    st = state()
    if st is None:
        sys.exit("gamerec: no recording is running")
    send("stop")
    print(f"gamerec: asked {st['id']} to stop")


def cmd_status(a):
    print("service: " + ("running" if daemon_running() else "not running"))
    st = state()
    print(f"recording: {st['id']} ({'auto' if st.get('auto') else 'by hand'}, since {st.get('started')})"
          if st else "recording: none")
    last = rjson(LAST)
    if last:
        print(f"last: {last.get('id')}")


# --------------------------------------------------------------------------
# root side: the collector
# --------------------------------------------------------------------------

class Collector:
    """One recording into SYS_LOGS/<id> (root-owned; group = the user's,
    readable). Marks arrive on self.marks, the stop on self.stop."""

    def __init__(self, rid, uid, gid, opts):
        self.rid, self.uid, self.gid, self.opts = rid, uid, gid, opts
        os.makedirs(SYS_LOGS, mode=0o755, exist_ok=True)
        self.d = os.path.join(SYS_LOGS, rid)
        os.mkdir(self.d, 0o750)
        os.mkdir(self.path("marks"), 0o750)
        self.stop = threading.Event()
        self.marks = queue.Queue()
        self.nmarks = 0
        self.nproblems = 0
        self.nauto = 0
        self.last_trigger = {}         # key -> (monotonic time of the last auto mark, auto marks so far)
        self.parts = {}
        self.procs = []
        self.patched = patched()
        self.t0 = time.time()
        self.power_profile = None
        self.log_f = open(self.path("gamerec.log"), "a")
        self.share(self.d)
        env = {k: v for k, v in os.environ.items() if not k.startswith("SUDO_")}
        self.env = env                 # nvtrace would hand its output to SUDO_UID
        self.pf_env = dict(env, PERFETTO_PRODUCER_SOCK_NAME=SOCK_DIR + "/p",
                           PERFETTO_CONSUMER_SOCK_NAME=SOCK_DIR + "/c")
        self.pf_pid = None

    def log(self, text):
        line = f"{now_s()} {text}"
        print(line, flush=True)
        self.log_f.write(line + "\n")
        self.log_f.flush()

    def path(self, *p):
        return os.path.join(self.d, *p)

    def share(self, top):
        """root:<user's group>, dirs 0750, files 0640: the user reads, only root
        writes (nothing here is ever a symlink: only root creates entries)."""
        for r, ds, fs in os.walk(top):
            for n in ds + fs:
                p = os.path.join(r, n)
                try:
                    st = os.lstat(p)
                    if stat.S_ISLNK(st.st_mode):
                        continue
                    os.lchown(p, 0, self.gid)
                    os.chmod(p, 0o750 if stat.S_ISDIR(st.st_mode) else 0o640)
                except OSError:
                    pass
        os.lchown(top, 0, self.gid)
        os.chmod(top, 0o750)

    def marker(self, text):
        for m in (NVTRACE_MARKER, TRACEFS + "/trace_marker"):
            try:
                with open(m, "w") as f:
                    f.write(text)
            except OSError:
                pass

    def run_to(self, path, cmd, timeout=20, mode="w"):
        with open(path, mode) as f:
            try:
                subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, env=self.env)
            except subprocess.TimeoutExpired:
                f.write(f"\n[gamerec: timed out after {timeout} s]\n")
            except OSError as e:
                f.write(f"[gamerec: {e}]\n")

    def spawn(self, name, cmd, **kw):
        out = open(self.path(name + ".out"), "w")
        p = subprocess.Popen(cmd, stdout=kw.pop("stdout", out), stderr=out, start_new_session=True,
                             env=kw.pop("env", self.env), **kw)
        self.procs.append((name, p))
        return p

    # -- parts ------------------------------------------------------------

    def start_perfetto(self):
        os.makedirs(SOCK_DIR, mode=0o700, exist_ok=True)
        for s in ("p", "c"):
            if os.path.lexists(f"{SOCK_DIR}/{s}"):
                os.unlink(f"{SOCK_DIR}/{s}")
        self.spawn("perfetto-traced", [TRACEBOX, "traced"], env=self.pf_env)
        self.spawn("perfetto-probes", [TRACEBOX, "traced_probes"], env=self.pf_env)
        for _ in range(50):
            if os.path.exists(SOCK_DIR + "/c") and os.path.exists(SOCK_DIR + "/p"):
                break
            time.sleep(0.1)
        time.sleep(0.5)          # traced_probes registers its data sources
        out = subprocess.run([TRACEBOX, "perfetto", "--txt", "-c", FLIGHT_CFG, "-o", self.path("flight-end.pftrace"),
                              "--background"], env=self.pf_env, capture_output=True, text=True, timeout=30)
        self.pf_pid = int(out.stdout.split()[-1])

    def clone_flight(self, path):
        if self.pf_pid is None:
            return
        try:
            out = subprocess.run([TRACEBOX, "perfetto", "--clone-by-name", "gamerec", "-o", path],
                                 env=self.pf_env, capture_output=True, text=True, timeout=120)
            if out.returncode != 0:
                self.log(f"perfetto clone failed: {out.stderr.strip()[-300:]}")
        except subprocess.TimeoutExpired:
            self.log("perfetto clone timed out")

    def start_nvtrace(self):
        if self.patched:
            self.spawn("nvtrace", [sys.executable, NVTRACE, "record", "-o", self.path("nvtrace"),
                                   "--interval", "250", "--nvml-ms", "1000"])
        else:
            # Stock driver: no driver events, NVML samples only.
            sys.path.insert(0, os.path.dirname(NVTRACE))
            import nvtrace
            f = open(self.path("samples.jsonl"), "w")
            threading.Thread(target=nvtrace.sampler, args=(f, self.stop, 0.25, 4), daemon=True).start()

    def start_journal(self):
        jf = open(self.path("journal.log"), "w")
        p = self.spawn("journal", ["journalctl", "-f", "-n", "0", "-o", "short-iso-precise", "--no-hostname"],
                       stdout=subprocess.PIPE, text=True, errors="replace")

        def reader():
            for line in p.stdout:
                jf.write(line)
                jf.flush()
                self.check_line(line.rstrip("\n"))
        threading.Thread(target=reader, daemon=True).start()

    def check_line(self, line):
        for key, rx in TRIGGERS:
            m = rx.search(line)
            if not m:
                continue
            self.nproblems += 1
            with open(self.path("problems.log"), "a") as f:
                f.write(f"{key}\t{line}\n")
            t = time.monotonic()
            last, count = self.last_trigger.get(key, (-1e9, 0))
            wait = AUTO_BACKOFF_S[min(count, len(AUTO_BACKOFF_S)) - 1] if count else 0
            if t - last >= wait and self.nauto < AUTO_MARKS_MAX:
                self.last_trigger[key] = (t, count + 1)
                self.nauto += 1
                extra = {"coredump_pid": m.group(1), "comm": m.group(2)} if key == "coredump" else {}
                self.log(f"problem: {key}: {line[:160]}")
                self.marks.put(dict(text=f"auto {key}: {line[:300]}", source="auto", **extra))
            return

    # -- marks ------------------------------------------------------------

    def mark_worker(self):
        while True:
            m = self.marks.get()
            if m is None:
                return
            try:
                self.do_mark(m)
            except Exception as e:          # a broken mark must not end the recording
                self.log(f"mark failed: {e!r}")

    def do_mark(self, m):
        self.nmarks += 1
        n = self.nmarks
        t = datetime.datetime.fromtimestamp(m.get("t", time.time()))
        name = f"{n:02d}_{t.strftime('%H%M%S')}_{m['source']}"
        md = self.path("marks", name)
        os.makedirs(md, exist_ok=True)
        self.marker(f"gamerec: mark {n} {m['text'][:200]}")
        self.clone_flight(os.path.join(md, "flight.pftrace"))
        with open(os.path.join(md, "note.txt"), "w") as f:
            f.write(f"{t.isoformat(timespec='seconds')} mark {n} ({m['source']})\n{m['text']}\n")
            if m.get("shot"):
                f.write(f"screenshot: local/game-logs/{self.rid}/{m['shot']}\n")
        if self.patched:
            self.run_to(os.path.join(md, "evict-status.txt"), [EVICT, "status"])
        self.run_to(os.path.join(md, "nvidia-smi.txt"), ["nvidia-smi"])
        self.run_to(os.path.join(md, "nvidia-smi.txt"),
                    ["nvidia-smi", "-q", "-d", "MEMORY,UTILIZATION,PERFORMANCE,CLOCK,POWER,TEMPERATURE"], mode="a")
        self.run_to(os.path.join(md, "ps.txt"),
                    ["ps", "-eo", "pid,ppid,pcpu,pmem,rss,stat,lstart,comm,args", "--sort=-pcpu"])
        with open(os.path.join(md, "sys.txt"), "w") as f:
            for p in ["/proc/meminfo", "/proc/pressure/cpu", "/proc/pressure/memory", "/proc/pressure/io", HINTS] + \
                     glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma"):
                try:
                    f.write(f"== {p}\n{rfile(p)}\n")
                except OSError:
                    pass
        if m.get("coredump_pid"):
            # systemd-coredump writes the stack trace a few seconds later.
            def later(pid=m["coredump_pid"], comm=m.get("comm", "x")):
                time.sleep(20)
                self.run_to(os.path.join(md, f"coredump-{re.sub(r'[^A-Za-z0-9_.-]', '_', comm)}.txt"),
                            ["coredumpctl", "info", "--no-pager", pid], timeout=60)
                self.share(md)
            threading.Thread(target=later, daemon=True).start()
        self.share(md)
        self.log(f"mark {n}: {m['text'][:120]} -> marks/{name}")

    # -- the loop ---------------------------------------------------------

    def sample_sys(self, f, i):
        rec = {"t": round(time.time(), 3)}
        try:
            mi = {}
            for ln in rfile("/proc/meminfo").splitlines():
                k, v = ln.split(":", 1)
                mi[k] = int(v.split()[0])
            rec["mem_avail_mb"] = mi["MemAvailable"] >> 10
            rec["swap_used_mb"] = (mi["SwapTotal"] - mi["SwapFree"]) >> 10
        except (OSError, KeyError, ValueError):
            pass
        for res in ("cpu", "memory", "io"):
            try:
                for ln in rfile(f"/proc/pressure/{res}").splitlines():
                    kind, *kv = ln.split()
                    rec[f"psi_{res}_{kind}"] = float(dict(x.split("=") for x in kv)["avg10"])
            except (OSError, KeyError, ValueError):
                pass
        pma = glob.glob("/proc/driver/nvidia/gpus/*/vidmem_pma")
        if pma:
            try:
                rec["vram_free_mb"] = int(rfile(pma[0]).split()[0])
            except (OSError, ValueError, IndexError):
                pass
        # What GameMode's hook switches: the power profile (every 10 s, a
        # D-Bus call), the V-Cache mode and the CPUs' energy preference.
        if i % 20 == 0:
            try:
                self.power_profile = subprocess.run(["powerprofilesctl", "get"], capture_output=True,
                                                    text=True, timeout=5).stdout.strip() or None
            except (OSError, subprocess.TimeoutExpired):
                pass
        if self.power_profile:
            rec["power_profile"] = self.power_profile
        for key, pat in (("x3d_mode", "/sys/bus/platform/drivers/amd_x3d_vcache/*/amd_x3d_mode"),
                         ("epp", "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference")):
            for p in glob.glob(pat)[:1]:
                try:
                    rec[key] = rfile(p).strip()
                except OSError:
                    pass
        f.write(json.dumps(rec) + "\n")
        f.flush()
        return rec

    def loop(self):
        sysf = open(self.path("sys.jsonl"), "w")
        last_hints = None
        rec = {}
        i = 0
        while not self.stop.is_set():
            t = time.monotonic()
            if i % 2 == 0:
                rec = self.sample_sys(sysf, i)
                try:
                    h = rfile(HINTS).split("\n", 1)
                    body = h[1] if len(h) > 1 else ""
                    if body != last_hints:
                        last_hints = body
                        with open(self.path("hints.log"), "a") as f:
                            f.write(f"== {now_s()}\n{body}")
                except OSError:
                    pass
            if self.patched and i == 0:
                self.evict_status()
            if i % 120 == 0 and i:
                mins = (time.time() - self.t0) / 60
                vram = f", VRAM free {rec['vram_free_mb']} MB" if "vram_free_mb" in rec else ""
                size = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(self.d) for n in fs)
                self.log(f"{mins:.0f} min{vram}, marks {self.nmarks}, problem lines {self.nproblems}, "
                         f"{size >> 20} MB on disk")
            for name, p in self.procs:
                if p.poll() is not None and name not in self.parts.get("dead", []):
                    self.parts.setdefault("dead", []).append(name)
                    self.log(f"{name} exited with {p.returncode} (see {name}.out)")
            i += 1
            self.stop.wait(max(0.0, 0.5 - (time.monotonic() - t)))
        if self.patched:
            self.evict_status()

    # The driver's per-process status once (start, end; marks keep their own).
    def evict_status(self):
        with open(self.path("evict-status.log"), "a") as f:
            f.write(f"== {now_s()}\n")
        self.run_to(self.path("evict-status.log"), [EVICT, "status"], mode="a")

    def run(self):
        meta = {"id": self.rid, "start": datetime.datetime.now().isoformat(timespec="seconds"),
                "kernel": os.uname().release, "patched": self.patched, "opts": self.opts}
        try:
            meta["driver"] = rfile("/proc/driver/nvidia/version").splitlines()[0]
        except OSError:
            meta["driver"] = "not loaded"
        meta["packages"] = subprocess.run(["pacman", "-Q", "nvidia-utils", "kwin", "plasma-workspace", "qt6-base",
                                           "mangohud", "mesa", "gamemode"],
                                          capture_output=True, text=True).stdout.split("\n")
        if self.opts.get("evict_trace") and self.patched:
            subprocess.run([EVICT, "trace", "on"], capture_output=True)

        for name, fn in (("journal", self.start_journal), ("nvtrace", self.start_nvtrace),
                         ("perfetto", None if self.opts.get("no_perfetto") else self.start_perfetto)):
            if fn is None:
                continue
            try:
                fn()
                self.parts[name] = "on"
            except Exception as e:
                self.parts[name] = f"failed: {e!r}"
                self.log(f"{name} not started: {e!r}")
        worker = threading.Thread(target=self.mark_worker)
        worker.start()
        self.share(self.d)
        self.marker("gamerec: start")
        self.log(f"recording {self.rid} ({', '.join(k for k, v in self.parts.items() if v == 'on')}); "
                 f"driver: {meta['driver'][:90]}")
        try:
            self.loop()
        finally:
            self.log("stopping ...")
            self.marker("gamerec: stop")
            time.sleep(1)                       # the journal's last lines
            self.marks.put(None)
            worker.join(300)
            for name, p in self.procs:
                if name in ("nvtrace", "journal") and p.poll() is None:
                    p.send_signal(signal.SIGTERM)
            if self.pf_pid is not None:
                try:
                    os.kill(self.pf_pid, signal.SIGINT)     # writes flight-end.pftrace
                    for _ in range(240):
                        os.kill(self.pf_pid, 0)
                        time.sleep(0.5)
                except ProcessLookupError:
                    pass
            for name, p in self.procs:
                if p.poll() is None:
                    p.send_signal(signal.SIGTERM)
                try:
                    p.wait(60)
                except subprocess.TimeoutExpired:
                    p.kill()
            if self.opts.get("evict_trace") and self.patched:
                subprocess.run([EVICT, "trace", "off"], capture_output=True)
            self.run_to(self.path("coredumps.txt"), ["coredumpctl", "list", "--no-pager", "--since",
                        datetime.datetime.fromtimestamp(self.t0).strftime("%Y-%m-%d %H:%M:%S")])
            meta.update(end=datetime.datetime.now().isoformat(timespec="seconds"), parts=self.parts,
                        marks=self.nmarks, problem_lines=self.nproblems)
            with open(self.path("meta.json"), "w") as f:
                json.dump(meta, f, indent=1)
            self.log(f"done: {self.d}")
            self.share(self.d)


# --------------------------------------------------------------------------
# root side: the daemon (the service, or one recording through sudo)
# --------------------------------------------------------------------------

def read_request(path):
    """A request file from the user's inbox, without following links or
    reading anything but a small regular file."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_size > 65536:
            return None
        return json.loads(os.read(fd, 65536))
    except (OSError, ValueError):
        return None
    finally:
        os.close(fd)


def setup_run(uid, gid):
    os.makedirs(RUN, mode=0o755, exist_ok=True)
    os.chmod(RUN, 0o755)
    st = os.lstat(INBOX) if os.path.lexists(INBOX) else None
    if st is None or not stat.S_ISDIR(st.st_mode) or st.st_uid != uid:
        if st is not None:
            shutil.rmtree(INBOX) if stat.S_ISDIR(st.st_mode) else os.unlink(INBOX)
        os.mkdir(INBOX, 0o700)
        os.chown(INBOX, uid, gid)
    for n in os.listdir(INBOX):            # stale requests from before
        try:
            os.unlink(os.path.join(INBOX, n))
        except OSError:
            pass


def cmd_daemon(a):
    if os.geteuid() != 0:
        sys.exit("gamerec: _daemon runs as root (the service, or `gamerec.sh record` through sudo)")
    if pid_alive(DAEMON_PID):
        sys.exit("gamerec: the service already runs (`gamerec.sh record` uses it)")
    setup_run(a.uid, a.gid)
    with open(DAEMON_PID, "w") as f:
        f.write(str(os.getpid()))
    quit_ = threading.Event()
    cur = {}                               # "c": Collector, "t": its thread, "st": state

    def on_signal(*_):
        quit_.set()
        if "c" in cur:
            cur["c"].stop.set()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    def start(rid, auto, opts):
        if "c" in cur:
            print(f"gamerec: start {rid} ignored, {cur['st']['id']} is running", flush=True)
            return
        if not ID_RE.match(rid or "") or os.path.lexists(os.path.join(SYS_LOGS, rid)):
            print(f"gamerec: bad recording id {rid!r}", flush=True)
            return
        c = Collector(rid, a.uid, a.gid, {k: bool(opts.get(k)) for k in ("no_perfetto", "evict_trace")})
        cur["c"] = c
        cur["st"] = {"id": rid, "auto": bool(auto), "started": now_s(), "stopping": False}
        wjson(STATE, cur["st"])
        cur["t"] = threading.Thread(target=c.run)
        cur["t"].start()

    if a.session:
        start(a.session, False, {"no_perfetto": a.no_perfetto, "evict_trace": a.evict_trace})
    try:
        while True:
            for n in sorted(os.listdir(INBOX)):
                if n.endswith(".tmp"):
                    continue
                p = os.path.join(INBOX, n)
                req = read_request(p)
                try:
                    os.unlink(p)
                except OSError:
                    pass
                if not isinstance(req, dict):
                    continue
                kind = req.get("kind")
                if kind == "start":
                    start(str(req.get("id", "")), req.get("auto"), req)
                elif kind == "stop" and "c" in cur:
                    cur["c"].stop.set()
                    cur["st"]["stopping"] = True
                    wjson(STATE, cur["st"])
                elif kind == "mark" and "c" in cur:
                    shot = req.get("shot")
                    cur["c"].marks.put(dict(text=str(req.get("text", ""))[:500], source="user",
                                            t=float(req.get("t") or time.time()),
                                            shot=shot if isinstance(shot, str) and re.fullmatch(r"shots/[\w.-]+", shot)
                                            else None))
            if "t" in cur and not cur["t"].is_alive():
                wjson(LAST, {"id": cur["st"]["id"], "dir": cur["c"].d})
                os.unlink(STATE)
                cur.clear()
                if a.session:
                    break
            if quit_.is_set() and "c" not in cur:
                break
            time.sleep(0.3)
    finally:
        if "t" in cur:
            cur["c"].stop.set()
            cur["t"].join()
            wjson(LAST, {"id": cur["st"]["id"], "dir": cur["c"].d})
        for p in (STATE, DAEMON_PID):
            if os.path.exists(p):
                os.unlink(p)


# --------------------------------------------------------------------------
# install (root)
# --------------------------------------------------------------------------

def cmd_install(a):
    if os.geteuid() != 0 or "SUDO_UID" not in os.environ:
        sys.exit("gamerec: sudo ./local/gamerec.sh install")
    uid, gid = int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"])
    if os.path.exists(UNIT):
        subprocess.run(["systemctl", "stop", "gamerec.service"])
    tmp = INSTALL_DIR + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    for rel in INSTALL_FILES:
        dst = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
        shutil.copy2(os.path.join(LOCAL, rel), dst)
    for r, ds, fs in os.walk(tmp):
        for n in ds + fs:
            p = os.path.join(r, n)
            os.lchown(p, 0, 0)
            os.chmod(p, 0o755 if os.path.isdir(p) or os.access(p, os.X_OK) else 0o644)
    os.chmod(tmp, 0o755)
    shutil.rmtree(INSTALL_DIR, ignore_errors=True)
    os.rename(tmp, INSTALL_DIR)
    with open(UNIT, "w") as f:
        f.write(f"""[Unit]
Description=gamerec: records gaming sessions on request (local/gamerec.sh in {LOCAL})

[Service]
ExecStart=/usr/bin/python3 {INSTALL_DIR}/gamerec/gamerec.py _daemon --uid {uid} --gid {gid}
TimeoutStopSec=300

[Install]
WantedBy=multi-user.target
""")
    subprocess.run(["systemctl", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "enable", "--now", "gamerec.service"], check=True)
    time.sleep(1)
    print(f"gamerec: service installed and running ({INSTALL_DIR}, {UNIT}); recordings in {SYS_LOGS}")


def cmd_uninstall(a):
    if os.geteuid() != 0:
        sys.exit("gamerec: sudo ./local/gamerec.sh uninstall")
    subprocess.run(["systemctl", "disable", "--now", "gamerec.service"])
    for p in (UNIT,):
        if os.path.exists(p):
            os.unlink(p)
    shutil.rmtree(INSTALL_DIR, ignore_errors=True)
    subprocess.run(["systemctl", "daemon-reload"])
    print(f"gamerec: service removed (recordings stay in {SYS_LOGS})")


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def load_mangohud(path):
    """(start datetime from the file name, [(elapsed s, frametime ms)])."""
    m = re.search(r"_(\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d)\.csv$", path)
    start = datetime.datetime.strptime(m.group(1), "%Y-%m-%d_%H-%M-%S") if m else None
    frames, cols = [], None
    with open(path, errors="replace") as f:
        for ln in f:
            parts = ln.rstrip("\n").split(",")
            if cols is None:
                if "frametime" in parts and "elapsed" in parts:
                    cols = (parts.index("frametime"), parts.index("elapsed"))
                continue
            try:
                frames.append((int(parts[cols[1]]) / 1e9, float(parts[cols[0]])))
            except (ValueError, IndexError):
                pass
    return start, frames


def report(d, quiet=False):
    sysd = os.path.join(d, "system") if os.path.isdir(os.path.join(d, "system")) else d
    out = []
    meta = rjson(os.path.join(sysd, "meta.json"))
    if meta:
        out.append(f"{d}\n{meta['start']} .. {meta.get('end', '?')}, {meta.get('driver', '?')[:80]}")
        out.append(f"parts: {meta.get('parts')}")
    else:
        out.append(f"{d} (no meta.json: still recording, or the root part died)")
    marks = sorted(glob.glob(os.path.join(sysd, "marks", "*", "note.txt")))
    out.append(f"\nmarks: {len(marks)}")
    for n in marks:
        lines = rfile(n).splitlines()
        out.append(f"  {os.path.basename(os.path.dirname(n))}: {(lines[1] if len(lines) > 1 else '')[:150]}")
    try:
        counts = {}
        for ln in open(os.path.join(sysd, "problems.log")):
            k = ln.split("\t", 1)[0]
            counts[k] = counts.get(k, 0) + 1
        out.append("problem lines: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    except OSError:
        out.append("problem lines: none")
    out.append("\nframe times (MangoHud):")
    files = sorted(f for f in glob.glob(os.path.join(d, "mangohud", "*.csv")) if not f.endswith("_summary.csv"))
    if not files:
        out.append("  none (no game ran with MangoHud)")
    for p in files:
        start, fr = load_mangohud(p)
        if len(fr) < 20:
            continue
        ft = [x[1] for x in fr]
        med = statistics.median(ft)
        srt = sorted(ft)
        p99 = srt[int(len(srt) * 0.99)]
        low1 = 1000.0 / statistics.mean(srt[int(len(srt) * 0.99):])
        spikes = [x for x in fr if x[1] > max(2.5 * med, med + 8)]
        dur = fr[-1][0] - fr[0][0]
        out.append(f"  {os.path.basename(p)}: {dur / 60:.1f} min, {len(ft)} frames, "
                   f"avg {len(ft) / max(dur, 1e-6):.1f} fps, median {med:.1f} ms, p99 {p99:.1f} ms, "
                   f"1% low {low1:.1f} fps, spikes {len(spikes)}")
        for el, v in sorted(spikes, key=lambda x: -x[1])[:5]:
            at = (start + datetime.timedelta(seconds=el)).strftime("%H:%M:%S") if start else f"+{el:.0f}s"
            out.append(f"      {at}  {v:.0f} ms")
    text = "\n".join(out) + "\n"
    try:
        with open(os.path.join(d, "summary.txt"), "w") as f:
            f.write(text)
    except OSError:
        pass
    if not quiet:
        print("\n" + text, end="")
    return "\n" + text


def cmd_report(a):
    d = a.dir
    if d is None:
        ds = sorted(x for x in glob.glob(os.path.join(LOGS, "2*")) if os.path.isdir(x))
        if not ds:
            sys.exit("gamerec: no recordings in " + LOGS)
        d = ds[-1]
    report(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--hud", action="store_true", help="show MangoHud's fps / frame time / VRAM")
    r.add_argument("--no-perfetto", action="store_true", help="no flight recorder (marks without flight.pftrace)")
    r.add_argument("--evict-trace", action="store_true", help="log every eviction remap with its VA range")
    au = sub.add_parser("auto")
    au.add_argument("what", choices=["start", "end"])
    m = sub.add_parser("mark")
    m.add_argument("--no-shot", action="store_true", help="no screenshot")
    m.add_argument("text", nargs="*")
    sub.add_parser("stop")
    sub.add_parser("status")
    h = sub.add_parser("hotkey")
    h.add_argument("args", nargs="*")
    hd = sub.add_parser("hud")
    hd.add_argument("state", nargs="?", choices=["on", "off"], help="default: toggle")
    sub.add_parser("steam")
    rp = sub.add_parser("report")
    rp.add_argument("dir", nargs="?")
    sub.add_parser("install")
    sub.add_parser("uninstall")
    dm = sub.add_parser("_daemon")
    dm.add_argument("--uid", type=int, required=True)
    dm.add_argument("--gid", type=int, required=True)
    dm.add_argument("--session", help="start this recording at once and exit after it (sudo path)")
    dm.add_argument("--no-perfetto", action="store_true")
    dm.add_argument("--evict-trace", action="store_true")
    a = ap.parse_args()
    {"record": cmd_record, "auto": cmd_auto, "mark": cmd_mark, "stop": cmd_stop, "status": cmd_status,
     "hotkey": cmd_hotkey, "hud": cmd_hud, "steam": cmd_steam, "report": cmd_report,
     "install": cmd_install, "uninstall": cmd_uninstall, "_daemon": cmd_daemon}[a.cmd](a)


if __name__ == "__main__":
    main()
