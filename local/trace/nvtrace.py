#!/usr/bin/env python3
"""Trace recorder and report for the vidmem patch: why does it stutter?

Records the driver's own trace events (tracepoints nvidia_vidmem:*, see
kernel-open/nvidia/nv-vidmem-trace.h), RM lock waits/holds, Xids, the
compositor's page flips (drm:drm_vblank_event_delivered: a gap = a dropped
frame on that output), the kernel log, and samples of VRAM / GPU load /
PCIe traffic, all on one clock (CLOCK_MONOTONIC). The report lines up every
frame gap with what the driver did at that moment, and writes a timeline
for https://ui.perfetto.dev (open trace.json there; it stays in the browser).

  sudo ./nvtrace.py record [-o DIR] [-d SEC] [--lock-us N] [--sched] [-- CMD ...]
        until Ctrl-C, -d seconds, or CMD exits (CMD runs as the sudo user);
        DIR defaults to local/trace-logs/<date_time>
  sudo ./nvtrace.py mark [TEXT]      marker in a running recording ("stutter
                                     now", bind it to a hotkey)
  sudo ./nvtrace.py live [--lock-us N]
        the same events as one line each, as they happen, plus VRAM every 2 s
  ./nvtrace.py report DIR [--gap-ms N] [--top N]
        summary.txt (printed) + trace.json (Perfetto)
  ./nvtrace.py map DIR|TRACE [-o FILE] [--from T] [--to T] [--mangohud CSV]...
        map.html: where each process's memory lay in vidmem at any moment of
        the recording, and every move (to sysmem, back, compaction) with the
        places it left and took; open it in a browser. T: seconds from the
        start or HH:MM[:SS]. Needs a driver with the vidmem_ext events (the
        recording starts and ends with a snapshot, `evict.sh map`); the
        simulator writes the same trace (local/sim, -map FILE).

Event fields (key=value) are documented in nv-vidmem-trace.h. Timestamps in
the trace are seconds of CLOCK_MONOTONIC (trace_clock "mono").
"""

import argparse
import array
import bisect
import collections
import ctypes
import datetime
import glob
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import threading
import time

TRACEFS = "/sys/kernel/tracing"
INSTANCE = TRACEFS + "/instances/nvtrace"
PIDFILE = "/run/nvtrace.pid"             # who records into INSTANCE
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_LOCAL = os.path.dirname(HERE)

EVENTS = [
    "nvidia_vidmem",                     # all of ours
    "nvidia/nvidia_dev_xid",
    "drm/drm_vblank_event_delivered",    # page flip done (compositor / direct scanout)
]
SCHED_EVENTS = ["sched/sched_switch", "sched/sched_wakeup"]


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def wfile(path, text):
    with open(path, "w") as f:
        f.write(text)


def rfile(path):
    with open(path) as f:
        return f.read()


def gpu_proc(name):
    files = glob.glob(f"/proc/driver/nvidia/gpus/*/{name}")
    return files[0] if files else None


def evict_cmd(cmd):
    """One command to procfs vidmem_evict (root); returns its result text."""
    path = gpu_proc("vidmem_evict")
    if path is None:
        return ""
    try:
        wfile(path, cmd + "\n")
        return rfile(path)
    except OSError as e:
        return f"{cmd}: {e}\n"


# --------------------------------------------------------------------------
# NVML (ctypes, no Python package needed)
# --------------------------------------------------------------------------

class _Util(ctypes.Structure):
    _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]


class _Mem(ctypes.Structure):
    _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong), ("used", ctypes.c_ulonglong)]


class _ProcUtil(ctypes.Structure):
    _fields_ = [("pid", ctypes.c_uint), ("timeStamp", ctypes.c_ulonglong), ("smUtil", ctypes.c_uint),
                ("memUtil", ctypes.c_uint), ("encUtil", ctypes.c_uint), ("decUtil", ctypes.c_uint)]


class Nvml:
    def __init__(self):
        self.lib = ctypes.CDLL("libnvidia-ml.so.1")
        if self.lib.nvmlInit_v2() != 0:
            raise OSError("nvmlInit failed")
        self.dev = ctypes.c_void_p()
        if self.lib.nvmlDeviceGetHandleByIndex_v2(0, ctypes.byref(self.dev)) != 0:
            raise OSError("no GPU 0")
        self.last_proc_ts = 0

    def sample(self):
        lib, dev, out = self.lib, self.dev, {}
        u = _Util()
        if lib.nvmlDeviceGetUtilizationRates(dev, ctypes.byref(u)) == 0:
            out["util_gpu"], out["util_mem"] = u.gpu, u.memory
        m = _Mem()
        if lib.nvmlDeviceGetMemoryInfo(dev, ctypes.byref(m)) == 0:
            out["used_mb"] = m.used >> 20
        for name, counter in (("pcie_tx_mbs", 0), ("pcie_rx_mbs", 1)):   # KB/s, 20 ms sample each
            v = ctypes.c_uint()
            if lib.nvmlDeviceGetPcieThroughput(dev, counter, ctypes.byref(v)) == 0:
                out[name] = v.value // 1024
        v = ctypes.c_uint()
        if lib.nvmlDeviceGetClockInfo(dev, 1, ctypes.byref(v)) == 0:   # NVML_CLOCK_SM
            out["sm_mhz"] = v.value
        if lib.nvmlDeviceGetPowerUsage(dev, ctypes.byref(v)) == 0:
            out["power_w"] = v.value // 1000
        return out

    def proc_util(self):
        """{pid: sm%} since the last call (GSP samples)."""
        lib, dev = self.lib, self.dev
        n = ctypes.c_uint(0)
        lib.nvmlDeviceGetProcessUtilization(dev, None, ctypes.byref(n), ctypes.c_ulonglong(self.last_proc_ts))
        if n.value == 0:
            return {}
        buf = (_ProcUtil * (n.value + 8))()
        n = ctypes.c_uint(n.value + 8)
        if lib.nvmlDeviceGetProcessUtilization(dev, buf, ctypes.byref(n), ctypes.c_ulonglong(self.last_proc_ts)) != 0:
            return {}
        res = {}
        for s in buf[:n.value]:
            res[s.pid] = max(res.get(s.pid, 0), s.smUtil)
            self.last_proc_ts = max(self.last_proc_ts, s.timeStamp)
        return res


# --------------------------------------------------------------------------
# record / live
# --------------------------------------------------------------------------

class Tracer:
    def __init__(self, sched=False, buf_kb=8192):
        if not os.path.isdir(TRACEFS + "/instances"):
            sys.exit("nvtrace: tracefs not mounted at " + TRACEFS)
        if not os.path.isdir(TRACEFS + "/events/nvidia_vidmem"):
            sys.exit("nvtrace: no nvidia_vidmem events: is the patched module (with tracepoints) loaded?")
        if os.path.isdir(INSTANCE):
            # A recorder that was killed leaves its instance behind, and every
            # recording after it then had no driver events (gamerec, 2026-11-14:
            # three sessions in a row). It is stale unless its recorder lives.
            owner = self.owner()
            if owner:
                sys.exit(f"nvtrace: another recording is running (pid {owner})")
            print(f"nvtrace: removing the stale {INSTANCE}", file=sys.stderr)
            try:
                wfile(INSTANCE + "/tracing_on", "0")
                wfile(INSTANCE + "/events/enable", "0")
                os.rmdir(INSTANCE)
            except OSError as e:
                sys.exit(f"nvtrace: {INSTANCE} is left from a recording that is gone and cannot be removed: {e}")
        os.mkdir(INSTANCE)
        wfile(PIDFILE, str(os.getpid()))
        wfile(INSTANCE + "/trace_clock", "mono")
        wfile(INSTANCE + "/buffer_size_kb", str(buf_kb))
        # The process of each thread: a game's lock waits come from threads of many names (Wine, dxvk).
        try:
            wfile(INSTANCE + "/options/record-tgid", "1")
        except OSError:
            pass
        self.enabled = []
        for ev in EVENTS + (SCHED_EVENTS if sched else []):
            path = f"{INSTANCE}/events/{ev}/enable"
            if os.path.exists(path):
                wfile(path, "1")
                self.enabled.append(ev)
            else:
                print(f"nvtrace: event {ev} not available, skipped", file=sys.stderr)
        wfile(INSTANCE + "/tracing_on", "1")

    @staticmethod
    def owner():
        """pid of the live recorder that holds INSTANCE, or 0."""
        try:
            pid = int(rfile(PIDFILE))
            return pid if b"nvtrace" in open(f"/proc/{pid}/cmdline", "rb").read() else 0
        except (OSError, ValueError):
            return 0

    def marker(self, text):
        try:
            wfile(INSTANCE + "/trace_marker", text)
        except OSError:
            pass

    def close(self):
        try:
            os.unlink(PIDFILE)
        except OSError:
            pass
        try:
            wfile(INSTANCE + "/tracing_on", "0")
            for ev in self.enabled:
                wfile(f"{INSTANCE}/events/{ev}/enable", "0")
            os.rmdir(INSTANCE)
        except OSError as e:
            print(f"nvtrace: cleaning up {INSTANCE}: {e}", file=sys.stderr)


def pipe_reader(path, out, stop, line_cb=None):
    """Copies trace_pipe to `out` until stop is set (trace_pipe blocks, so
    it is opened non-blocking and polled)."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    pending = b""
    try:
        while True:
            try:
                chunk = os.read(fd, 1 << 16)
            except BlockingIOError:
                chunk = b""
            if chunk:
                if out is not None:
                    out.write(chunk)
                    out.flush()     # readable while recording
                if line_cb is not None:
                    pending += chunk
                    *lines, pending = pending.split(b"\n")
                    for ln in lines:
                        line_cb(ln.decode(errors="replace"))
            elif stop.is_set():
                break
            else:
                time.sleep(0.05)
    finally:
        os.close(fd)


def kmsg_reader(out, stop):
    """Kernel log records from now on; /dev/kmsg times are local_clock (us),
    a sync record ties them to CLOCK_MONOTONIC (offset written to meta)."""
    fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
    os.lseek(fd, 0, os.SEEK_END)
    try:
        while True:
            try:
                rec = os.read(fd, 8192)
            except BlockingIOError:
                if stop.is_set():
                    break
                time.sleep(0.1)
                continue
            except OSError:          # EPIPE: records overwritten, go on
                continue
            out.write(rec.decode(errors="replace").rstrip("\n") + "\n")
            out.flush()
            if stop.is_set():
                break
    finally:
        os.close(fd)


def sampler(out, stop, interval, nvml_every):
    """vidmem_pma every `interval` s, NVML every `nvml_every` samples,
    per-process SM% once a second. JSON lines with t = monotonic s."""
    pma = gpu_proc("vidmem_pma")
    try:
        nvml = Nvml()
    except OSError as e:
        print(f"nvtrace: NVML unavailable ({e}), only vidmem_pma is sampled", file=sys.stderr)
        nvml = None
    i = 0
    last_proc = 0.0
    while not stop.is_set():
        t0 = time.monotonic()
        rec = {"t": round(t0, 4)}
        if pma is not None:
            try:
                v = [int(x) for x in rfile(pma).split()]
                rec["pma"] = v   # free, largest contiguous free, block, block free, block largest (MB)
            except (OSError, ValueError):
                pass
        if nvml is not None and i % nvml_every == 0:
            rec.update(nvml.sample())
        if nvml is not None and t0 - last_proc >= 1.0:
            rec["proc_sm"] = nvml.proc_util()
            last_proc = t0
        out.write(json.dumps(rec) + "\n")
        out.flush()
        i += 1
        stop.wait(max(0.0, interval - (time.monotonic() - t0)))


def cmd_record(a):
    if os.geteuid() != 0:
        sys.exit("nvtrace: record needs root (tracefs, procfs)")
    outdir = a.out or os.path.join(REPO_LOCAL, "trace-logs", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    uid = int(os.environ.get("SUDO_UID", "0"))
    gid = int(os.environ.get("SUDO_GID", "0"))

    evict_cmd(f"trace-lock {a.lock_us}")
    tr = Tracer(sched=a.sched, buf_kb=a.buf_kb)
    stop = threading.Event()
    files = {n: open(os.path.join(outdir, n), "wb" if n == "trace.txt" else "w")
             for n in ("trace.txt", "kernel.log", "samples.jsonl")}
    threads = [
        threading.Thread(target=pipe_reader, args=(INSTANCE + "/trace_pipe", files["trace.txt"], stop)),
        threading.Thread(target=kmsg_reader, args=(files["kernel.log"], stop)),
        threading.Thread(target=sampler, args=(files["samples.jsonl"], stop, a.interval / 1000.0,
                                               max(1, round(a.nvml_ms / a.interval)))),
    ]
    for t in threads:
        t.start()

    # Tie /dev/kmsg's clock to CLOCK_MONOTONIC.
    sync_mono = mono_ns()
    try:
        wfile("/dev/kmsg", f"nvtrace: sync {sync_mono}\n")
    except OSError:
        pass
    wfile(os.path.join(outdir, "status-start.txt"), evict_cmd("status"))
    # The map of vidmem: a snapshot now, every change after it as it happens
    # (vidmem_ext), a snapshot at the end to check the events against.
    map_start = evict_cmd("map").strip()
    meta = {"start_mono": sync_mono / 1e9, "start": datetime.datetime.now().isoformat(timespec="seconds"),
            "start_wall": time.time() - (mono_ns() - sync_mono) / 1e9, "map": map_start,
            "kernel": os.uname().release, "events": tr.enabled, "lock_us": a.lock_us,
            "cmd": a.cmd, "driver": rfile("/proc/driver/nvidia/version").splitlines()[0]
            if os.path.exists("/proc/driver/nvidia/version") else "?"}
    # Now already: a recording that is still running can be read (map, report).
    with open(os.path.join(outdir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    tr.marker("nvtrace: start")
    print(f"nvtrace: recording to {outdir} ({', '.join(tr.enabled)}); "
          f"{'until ' + ' '.join(a.cmd) + ' exits' if a.cmd else ('%d s' % a.duration if a.duration else 'Ctrl-C to stop')}",
          file=sys.stderr)

    interrupted = []
    signal.signal(signal.SIGINT, lambda *_: interrupted.append(1))
    signal.signal(signal.SIGTERM, lambda *_: interrupted.append(1))
    proc = None
    if a.cmd:
        env = dict(os.environ)
        if uid:
            # The user's own environment, not root's (Chromium with HOME=/root
            # as uid 1000 dies on a breakpoint trap).
            import pwd
            pw = pwd.getpwuid(uid)
            env.update(HOME=pw.pw_dir, USER=pw.pw_name, LOGNAME=pw.pw_name,
                       XDG_RUNTIME_DIR=f"/run/user/{uid}",
                       DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{uid}/bus")
            for k in ("SUDO_COMMAND", "SUDO_USER", "SUDO_UID", "SUDO_GID", "MAIL"):
                env.pop(k, None)

        def demote():
            if uid:
                os.setgroups(os.getgrouplist(env["USER"], gid))
                os.setgid(gid)
                os.setuid(uid)
        log = open(os.path.join(outdir, "cmd.log"), "w")
        tr.marker("nvtrace: cmd start " + " ".join(a.cmd))
        # Own session: on stop the whole tree (test compositor, browsers) goes.
        proc = subprocess.Popen(a.cmd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=demote, env=env,
                                start_new_session=True)
    end = time.monotonic() + a.duration if a.duration else None
    while not interrupted:
        if proc is not None and proc.poll() is not None:
            tr.marker(f"nvtrace: cmd exit {proc.returncode}")
            meta["cmd_rc"] = proc.returncode
            break
        if end is not None and time.monotonic() >= end:
            break
        time.sleep(0.2)
    if proc is not None and proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)

    meta["map_end"] = evict_cmd("map").strip()
    tr.marker("nvtrace: stop")
    meta["end_mono"] = mono_ns() / 1e9
    wfile(os.path.join(outdir, "status-end.txt"), evict_cmd("status"))
    time.sleep(0.3)
    stop.set()
    for t in threads:
        t.join()
    for f in files.values():
        f.close()
    tr.close()
    # Overruns: events lost because the buffer filled faster than it was read.
    with open(os.path.join(outdir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    if uid:
        for root, dirs, fs in os.walk(outdir):
            for n in dirs + fs:
                os.chown(os.path.join(root, n), uid, gid)
        os.chown(outdir, uid, gid)
        parent = os.path.dirname(os.path.abspath(outdir))
        if os.path.basename(parent) == "trace-logs" and os.stat(parent).st_uid == 0:
            os.chown(parent, uid, gid)
    print(f"nvtrace: done, {outdir}\n  ./nvtrace.py report {outdir}", file=sys.stderr)


def cmd_mark(a):
    path = INSTANCE + "/trace_marker"
    if not os.path.exists(path):
        sys.exit("nvtrace: no recording is running")
    wfile(path, "nvtrace: mark " + (" ".join(a.text) or "stutter"))


def cmd_live(a):
    if os.geteuid() != 0:
        sys.exit("nvtrace: live needs root")
    evict_cmd(f"trace-lock {a.lock_us}")
    tr = Tracer(buf_kb=2048)
    stop = threading.Event()

    def show(line):
        ev = parse_line(line)
        if ev is None or ev["event"] == "vidmem_proc" or ev["event"] == "vidmem_tick" and \
                int(ev["f"].get("us", 0)) < 5000:
            return
        print(f"{ev['ts']:12.3f} {ev['task']}-{ev['tid']:<7} {describe(ev)}", flush=True)

    t = threading.Thread(target=pipe_reader, args=(INSTANCE + "/trace_pipe", None, stop, show))
    t.start()
    pma = gpu_proc("vidmem_pma")
    try:
        while True:
            if pma:
                v = rfile(pma).split()
                print(f"{time.monotonic():12.3f} -- vram free {v[0]} MB (largest {v[1]}), "
                      f"reserve block free {v[3]}/{v[2]} MB (largest {v[4]})", flush=True)
            time.sleep(2)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        t.join()
        tr.close()


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------

LINE_RE = re.compile(r"^\s*(?P<task>.+?)-(?P<tid>\d+)\s+(?:\(\s*[\d-]+\)\s+)?\[(?P<cpu>\d+)\]\s+"
                     r"(?:\S{4,5}\s+)?(?P<ts>\d+\.\d+):\s+(?P<event>\w+):\s*(?P<body>.*)$")
KV_RE = re.compile(r"(\w+)=((?:\S+?)(?: \[\w+\])?)(?=,? \w+=|,?$)")


def parse_line(line):
    m = LINE_RE.match(line)
    if not m:
        return None
    body = m.group("body")
    f = {k: v.rstrip(",") for k, v in KV_RE.findall(body)}
    return {"task": m.group("task").strip(), "tid": int(m.group("tid")), "ts": float(m.group("ts")),
            "event": m.group("event"), "body": body, "f": f}


def fi(f, k, d=0):
    try:
        return int(f.get(k, d), 0)
    except ValueError:
        return d


def describe(ev):
    f, e = ev["f"], ev["event"]
    if e == "vidmem_move":
        return (f"{f.get('op')} {f.get('comm')}({f.get('pid')}) {fi(f, 'kb') >> 10} MB why={f.get('why')} "
                f"in {fi(f, 'total_us') / 1000:.1f} ms (quiesce {fi(f, 'quiesce_us') / 1000:.1f}, "
                f"copy {fi(f, 'copy_us') / 1000:.1f}, remap {fi(f, 'remap_us') / 1000:.1f})"
                + (f" FAILED {f.get('failed')}" if fi(f, "failed") else ""))
    if e == "vidmem_pause":
        return (f"paused {f.get('comm')}({f.get('pid')}) {fi(f, 'paused_us') / 1000:.1f} ms "
                f"(stop {fi(f, 'stop_us') / 1000:.1f} ms, {f.get('channels')} channels)")
    if e == "vidmem_reserve":
        return (f"reserve {f.get('outcome')}: {f.get('comm')}({f.get('pid')}) {fi(f, 'kb') >> 10} MB, "
                f"free {f.get('free_mb')} MB ({f.get('block_free_mb')} in block)")
    if e == "vidmem_makeroom":
        return (f"makeroom for {f.get('comm')}({f.get('pid')}) {fi(f, 'kb') >> 10} MB: waited "
                f"{fi(f, 'us') / 1000:.1f} ms, ok={f.get('ok')}, avail {f.get('avail_mb')} MB")
    if e == "vidmem_tick":
        return (f"policy step {fi(f, 'us') / 1000:.1f} ms (queued {fi(f, 'queue_us') / 1000:.1f} ms), "
                f"avail {f.get('avail_mb')} MB, evicted {f.get('evicted_mb')} MB")
    if e == "vidmem_hint":
        return f"window {f.get('comm')}({f.get('pid')}) {f.get('old')} -> {f.get('new')}"
    if e == "vidmem_frag":
        tail = ""
        if f.get("at") != "tick":
            tail = (f" after {f.get('at')} {f.get('comm')}({f.get('pid')}) why={f.get('why')}: longest "
                    f"{f.get('longest_before_mb')} -> {f.get('longest_mb')} MB, in chunks "
                    f"{f.get('chunk_before_mb')} -> {f.get('chunk_mb')} MB")
        return (f"vidmem free {f.get('free_mb')} MB: {f.get('chunk_mb')} in 2 MB chunks ({f.get('runs')} runs, "
                f"{f.get('runs128')} >= 128 MB, longest {f.get('longest_mb')}), {f.get('holes_mb')} in holes"
                + tail)
    if e == "rm_lock":
        if fi(f, "wait_us"):
            return (f"waited {fi(f, 'wait_us') / 1000:.1f} ms for {f.get('lock')} lock ({f.get('mode')}) at "
                    f"{f.get('at')}; holder {f.get('holder')} at {f.get('holder_at')}")
        return f"held {f.get('lock')} lock ({f.get('mode')}) {fi(f, 'hold_us') / 1000:.1f} ms, taken at {f.get('at')}"
    return f"{e}: {ev['body']}"


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def load_dir(d):
    events = []
    with open(os.path.join(d, "trace.txt"), errors="replace") as f:
        for line in f:
            ev = parse_line(line)
            if ev is not None:
                events.append(ev)
    samples = []
    p = os.path.join(d, "samples.jsonl")
    if os.path.exists(p):
        with open(p) as f:
            for line in f:
                try:
                    samples.append(json.loads(line))
                except ValueError:
                    pass
    meta = {}
    p = os.path.join(d, "meta.json")
    if os.path.exists(p):
        with open(p) as f:
            meta = json.load(f)
    klog = []
    p = os.path.join(d, "kernel.log")
    if os.path.exists(p):
        offset = None
        recs = []
        with open(p, errors="replace") as f:
            for line in f:
                m = re.match(r"^\d+,\d+,(\d+),[^;]*;(.*)$", line.rstrip("\n"))
                if not m:
                    continue
                ts_us, msg = int(m.group(1)), m.group(2)
                sm = re.match(r"nvtrace: sync (\d+)", msg)
                if sm:
                    offset = int(sm.group(1)) / 1e9 - ts_us / 1e6
                recs.append((ts_us / 1e6, msg))
        for ts, msg in recs:
            klog.append((ts + (offset or 0.0), msg))
    return events, samples, meta, klog


def interval(ev):
    """(start, end) in s for events that describe a span ending at ts."""
    f, e, ts = ev["f"], ev["event"], ev["ts"]
    if e == "vidmem_move":
        return ts - fi(f, "total_us") / 1e6, ts
    if e == "vidmem_pause":
        return ts - fi(f, "paused_us") / 1e6, ts
    if e == "vidmem_makeroom":
        return ts - fi(f, "us") / 1e6, ts
    if e == "vidmem_tick":
        return ts - fi(f, "us") / 1e6, ts
    if e == "rm_lock":
        d = max(fi(f, "wait_us"), fi(f, "hold_us")) / 1e6
        return ts - d, ts
    return ts, ts


def pct(vals, p):
    if not vals:
        return 0
    vals = sorted(vals)
    return vals[min(len(vals) - 1, int(len(vals) * p / 100))]


def build_report(d, gap_ms, top):
    events, samples, meta, klog = load_dir(d)
    out = []
    w = out.append
    t0 = meta.get("start_mono") or (events[0]["ts"] if events else 0)
    t1 = meta.get("end_mono") or (events[-1]["ts"] if events else t0)
    by = {}
    for ev in events:
        by.setdefault(ev["event"], []).append(ev)

    w(f"nvtrace report: {d}")
    w(f"  {meta.get('driver', '?')}")
    w(f"  {meta.get('start', '?')}, {t1 - t0:.1f} s, lock trace from {meta.get('lock_us', '?')} us, "
      f"cmd: {' '.join(meta.get('cmd') or []) or '-'}")
    w("  events: " + ", ".join(f"{k} {len(v)}" for k, v in sorted(by.items())))
    w("")

    # --- frames ------------------------------------------------------------
    gaps = []
    flips = {}
    for ev in by.get("drm_vblank_event_delivered", []):
        m = re.search(r"crtc=(\d+)", ev["body"])
        flips.setdefault(int(m.group(1)) if m else 0, []).append(ev["ts"])
    w("== Frames (page flips delivered to the compositor, per output; a gap counts only")
    w("   inside continuous motion: 8 normal frames before it, 3 after)")
    if not flips:
        w("  none recorded (no flips: nothing on screen changed, or drm events unavailable)")
    for crtc, ts in sorted(flips.items()):
        iv = [b - a for a, b in zip(ts, ts[1:])]
        if not iv:
            continue
        med = statistics.median(iv)
        limit = max(gap_ms / 1000.0, 1.8 * med)
        # A gap only inside continuous motion (8 normal intervals before, 3
        # after): the compositor does not flip at all while nothing changes,
        # and a blinking cursor makes short bursts of flips.
        g = [(ts[i], ts[i + 1]) for i in range(8, len(iv) - 3)
             if iv[i] > limit and max(iv[i - 8:i]) <= limit and max(iv[i + 1:i + 4]) <= limit]
        # Over a second without a frame: idle with a periodic animation as
        # often as a freeze; listed, not counted (the game's own frame times,
        # MangoHud, decide).
        long_ = [(a, b) for a, b in g if b - a >= 1.0]
        g = [(a, b) for a, b in g if b - a < 1.0]
        gaps += [(crtc, a, b) for a, b in g]
        w(f"  crtc {crtc}: {len(ts)} flips, median {med * 1000:.2f} ms ({1 / med if med else 0:.0f} Hz), "
          f"p99 {pct(iv, 99) * 1000:.1f} ms, max {max(iv) * 1000:.1f} ms, gaps > {limit * 1000:.1f} ms: {len(g)}"
          + (f"; pauses >= 1 s (idle or freeze): {len(long_)} at "
             + ", ".join(f"{a - t0:.1f}s" for a, _ in long_[:6]) if long_ else ""))
    w("")

    # --- what the driver did ----------------------------------------------
    moves = by.get("vidmem_move", [])
    w(f"== Moves (evict/restore batches): {len(moves)}")
    agg = {}
    for ev in moves:
        f = ev["f"]
        k = (f.get("op"), f.get("why"))
        a = agg.setdefault(k, [0, 0, 0, 0])
        a[0] += 1
        a[1] += fi(f, "kb") >> 10
        a[2] += fi(f, "total_us")
        a[3] = max(a[3], fi(f, "total_us"))
    for (op, why), (n, mb, tot, mx) in sorted(agg.items(), key=lambda x: -x[1][2]):
        w(f"  {op:8} {why:10} {n:5}x {mb:7} MB, total {tot / 1000:8.1f} ms, max {mx / 1000:7.1f} ms")
    for ev in sorted(moves, key=lambda e: -fi(e["f"], "total_us"))[:top]:
        w(f"    {ev['ts'] - t0:9.3f}s  {describe(ev)}")
    w("")

    pauses = by.get("vidmem_pause", [])
    w(f"== Paused processes (channels off: their GPU work stood still): {len(pauses)}")
    for ev in sorted(pauses, key=lambda e: -fi(e["f"], "paused_us"))[:top]:
        w(f"    {ev['ts'] - t0:9.3f}s  {describe(ev)}")
    w("")

    mr = by.get("vidmem_makeroom", [])
    w(f"== Make room (the allocating thread waited while others moved out): {len(mr)}")
    per = {}
    for ev in mr:
        f = ev["f"]
        p = per.setdefault(f"{f.get('comm')}({f.get('pid')})", [0, 0, 0])
        p[0] += 1
        p[1] += fi(f, "us")
        p[2] = max(p[2], fi(f, "us"))
    for k, (n, tot, mx) in sorted(per.items(), key=lambda x: -x[1][1]):
        w(f"  {k:30} {n:5}x total {tot / 1000:8.1f} ms, max {mx / 1000:7.1f} ms")
    w("")

    res = by.get("vidmem_reserve", [])
    w(f"== Reserve turned allocations away: {len(res)}")
    per = {}
    for ev in res:
        f = ev["f"]
        p = per.setdefault((f"{f.get('comm')}({f.get('pid')})", f.get("outcome")), [0, 0])
        p[0] += 1
        p[1] += fi(f, "kb") >> 10
    for (k, oc), (n, mb) in sorted(per.items(), key=lambda x: -x[1][0]):
        w(f"  {k:30} {oc:7} {n:6}x {mb:7} MB")
    w("")

    ticks = by.get("vidmem_tick", [])
    if ticks:
        us = [fi(e["f"], "us") for e in ticks]
        q = [fi(e["f"], "queue_us") for e in ticks]
        w(f"== Policy steps (API + GPU lock held for all of each): {len(ticks)}, "
          f"p50 {pct(us, 50) / 1000:.1f} ms, p99 {pct(us, 99) / 1000:.1f} ms, max {max(us) / 1000:.1f} ms; "
          f"queued max {max(q) / 1000:.1f} ms")
        w("")

    frag_report(by.get("vidmem_frag", []), t0, w)

    locks = by.get("rm_lock", [])
    w(f"== RM locks (waits/holds >= {meta.get('lock_us', '?')} us): {len(locks)}")
    waits = [e for e in locks if fi(e["f"], "wait_us")]
    holds = [e for e in locks if fi(e["f"], "hold_us")]
    per = {}
    for ev in waits:
        p = per.setdefault((ev["task"], ev["f"].get("lock")), [0, 0, 0])
        p[0] += 1
        p[1] += fi(ev["f"], "wait_us")
        p[2] = max(p[2], fi(ev["f"], "wait_us"))
    w("  waits by thread:")
    for (task, lock), (n, tot, mx) in sorted(per.items(), key=lambda x: -x[1][1])[:top]:
        w(f"    {task:24} {lock:3} {n:6}x total {tot / 1000:8.1f} ms, max {mx / 1000:7.1f} ms")
    per = {}
    for ev in holds:
        p = per.setdefault((ev["f"].get("lock"), ev["f"].get("mode"), ev["f"].get("at")), [0, 0, 0, set()])
        p[0] += 1
        p[1] += fi(ev["f"], "hold_us")
        p[2] = max(p[2], fi(ev["f"], "hold_us"))
        p[3].add(ev["task"])
    w("  holds by place:")
    for (lock, mode, at), (n, tot, mx, tasks) in sorted(per.items(), key=lambda x: -x[1][1])[:top]:
        w(f"    {lock}/{mode} {at:50} {n:6}x total {tot / 1000:8.1f} ms, max {mx / 1000:7.1f} ms "
          f"({', '.join(sorted(tasks)[:3])})")
    w("")

    xids = by.get("nvidia_dev_xid", [])
    nvrm = [(ts, m) for ts, m in klog if "NVRM" in m or "Xid" in m or "nvidia" in m.lower()]
    w(f"== Xid: {len(xids)}; NVRM kernel log lines: {len(nvrm)}")
    for ev in xids[:top]:
        w(f"    {ev['ts'] - t0:9.3f}s  {ev['body']}")
    for ts, m in nvrm[:top]:
        w(f"    {ts - t0:9.3f}s  {m[:160]}")
    w("")

    # --- frame gaps vs. driver activity -------------------------------------
    spans = []
    for ev in events:
        if ev["event"] in ("vidmem_move", "vidmem_pause", "vidmem_makeroom", "vidmem_tick", "rm_lock",
                           "vidmem_reserve", "nvidia_dev_xid", "vidmem_hint", "tracing_mark_write"):
            a, b = interval(ev)
            spans.append((a, b, ev))
    spans.sort(key=lambda x: x[0])
    w(f"== Frame gaps and what overlapped them ({len(gaps)} gaps; window +-5 ms)")
    explained = 0
    for crtc, a, b in sorted(gaps, key=lambda g: -(g[2] - g[1]))[:top * 2]:
        hits = [ev for s, e, ev in spans if s <= b + 0.005 and e >= a - 0.005]
        if hits:
            explained += 1
        w(f"  {a - t0:9.3f}s crtc {crtc}: {(b - a) * 1000:6.1f} ms without a frame"
          + ("" if hits else "  (no driver event: look at GPU load / the app itself)"))
        for ev in hits[:6]:
            w(f"      {ev['ts'] - t0:9.3f}s {ev['task']}-{ev['tid']}: {describe(ev)}")
    if gaps:
        w(f"  {explained} of the {min(len(gaps), top * 2)} longest gaps overlap a driver event")
    w("")

    # --- samples -----------------------------------------------------------
    if samples:
        pma = [s["pma"] for s in samples if "pma" in s]
        if pma:
            w(f"== VRAM: free min {min(p[0] for p in pma)} MB, reserve block free min "
              f"{min(p[3] for p in pma)} MB, block largest contiguous min {min(p[4] for p in pma)} MB")
        rx = [s["pcie_rx_mbs"] for s in samples if "pcie_rx_mbs" in s]
        tx = [s["pcie_tx_mbs"] for s in samples if "pcie_tx_mbs" in s]
        if rx:
            w(f"   PCIe host->GPU max {max(rx)} MB/s (p50 {pct(rx, 50)}), GPU->host max {max(tx)} MB/s (p50 {pct(tx, 50)})")
        ug = [s["util_gpu"] for s in samples if "util_gpu" in s]
        if ug:
            w(f"   GPU busy p50 {pct(ug, 50)}%, max {max(ug)}%")
    return "\n".join(out) + "\n", events, samples, klog, t0


def frag_report(frags, t0, w):
    """problem #16: how fragmented free vidmem is over time and what moves did to it."""
    if not frags:
        return
    ticks = [e["f"] for e in frags if e["f"].get("at") == "tick"]
    moves = [e for e in frags if e["f"].get("at") != "tick"]
    scan = [fi(e["f"], "scan_us") for e in frags]
    w(f"== Fragmentation of free vidmem ({len(ticks)} samples every ~10 s, {len(moves)} after moves; "
      f"scan p50 {pct(scan, 50)} us, max {max(scan)} us)")
    w("   free = in whole 2 MB chunks + in holes (chunks partly in use); runs: free chunks in a row")
    if ticks:
        def col(k):
            return [fi(f, k) for f in ticks]
        for k, name in (("free_mb", "free MB"), ("chunk_mb", "in chunks MB"), ("holes_mb", "in holes MB"),
                        ("longest_mb", "longest run MB"), ("runs", "runs"), ("runs128", "runs >= 128 MB"),
                        ("partial", "partly used chunks"), ("partial_used_mb", "used in them MB")):
            v = col(k)
            w(f"  {name:20} min {min(v):6} p50 {pct(v, 50):6} max {max(v):6}  first {v[0]:6} last {v[-1]:6}")
        tk = [e for e in frags if e["f"].get("at") == "tick"]
        step = max(1, len(tk) // 24)
        w("  over time (t, free, in chunks, in holes, longest, runs >= 128 MB):")
        for e in tk[::step]:
            f = e["f"]
            w(f"    {e['ts'] - t0:9.1f}s {fi(f, 'free_mb'):6} {fi(f, 'chunk_mb'):6} {fi(f, 'holes_mb'):6} "
              f"{fi(f, 'longest_mb'):6} {fi(f, 'runs128'):3}")
    if moves:
        agg = {}
        for e in moves:
            f = e["f"]
            d = fi(f, "longest_mb") - fi(f, "longest_before_mb")
            a = agg.setdefault((f.get("at"), f.get("why"), f.get("comm")), [0, 0, 0, 0, 0])
            a[0] += 1
            a[1] += d
            a[2] += d < 0
            a[3] += d > 0
            a[4] += fi(f, "chunk_mb") - fi(f, "chunk_before_mb")
        w("  what move batches did to the longest free run (per op, why, process):")
        for (at, why, comm), (n, dl, worse, better, dc) in sorted(agg.items(), key=lambda x: x[1][1]):
            w(f"    {at:8} {why:10} {comm:16} {n:5}x  longest {dl:+7} MB in all, shorter {worse:4}x, "
              f"longer {better:4}x; free chunks {dc:+7} MB")
    w("")


def perfetto(events, samples, klog, t0, path):
    """Chrome JSON trace (opens in ui.perfetto.dev): one track per process
    and per lock-waiting thread, counters for VRAM / PCIe / GPU load."""
    tr = []
    names = {}
    PID_DRV, PID_GPU, PID_DISP, PID_LOCK, PID_LOG = 1000001, 1000002, 1000003, 1000004, 1000005

    def us(t):
        return round((t - t0) * 1e6, 1)

    def proc_name(pid, name):
        if pid not in names:
            names[pid] = name
            tr.append({"ph": "M", "name": "process_name", "pid": pid, "args": {"name": name}})

    proc_name(PID_DRV, "driver: policy")
    proc_name(PID_GPU, "GPU")
    proc_name(PID_DISP, "display: frames")
    proc_name(PID_LOCK, "RM locks")
    proc_name(PID_LOG, "kernel log")
    flip_last = {}
    lock_threads = set()
    for ev in events:
        f, e = ev["f"], ev["event"]
        a, b = interval(ev)
        if e in ("vidmem_move", "vidmem_pause", "vidmem_makeroom", "vidmem_reserve", "vidmem_hint", "vidmem_proc"):
            pid = fi(f, "pid")
            proc_name(pid, f"{f.get('comm')} ({pid})")
        if e == "vidmem_move":
            tr.append({"ph": "X", "name": f"{f.get('op')} ({f.get('why')}) {fi(f, 'kb') >> 10} MB",
                       "pid": fi(f, "pid"), "tid": 1, "ts": us(a), "dur": us(b) - us(a), "args": f})
        elif e == "vidmem_pause":
            tr.append({"ph": "X", "name": "paused (channels off)", "pid": fi(f, "pid"), "tid": 2,
                       "ts": us(a), "dur": us(b) - us(a), "args": f})
        elif e == "vidmem_makeroom":
            tr.append({"ph": "X", "name": f"waits: make room {fi(f, 'kb') >> 10} MB", "pid": fi(f, "pid"),
                       "tid": 3, "ts": us(a), "dur": us(b) - us(a), "args": f})
        elif e == "vidmem_reserve":
            tr.append({"ph": "i", "s": "t", "name": f"reserve: {f.get('outcome')} {fi(f, 'kb') >> 10} MB",
                       "pid": fi(f, "pid"), "tid": 3, "ts": us(a), "args": f})
        elif e == "vidmem_hint":
            tr.append({"ph": "i", "s": "p", "name": f"window {f.get('old')} -> {f.get('new')}",
                       "pid": fi(f, "pid"), "tid": 4, "ts": us(a), "args": f})
        elif e == "vidmem_proc":
            tr.append({"ph": "C", "name": "memory MB", "pid": fi(f, "pid"), "ts": us(a),
                       "args": {"vram": fi(f, "vram_mb"), "sysmem (evicted)": fi(f, "ram_mb")}})
        elif e == "vidmem_frag":
            tr.append({"ph": "C", "name": "free vidmem MB", "pid": PID_DRV, "ts": us(a),
                       "args": {"in 2 MB chunks": fi(f, "chunk_mb"), "in holes": fi(f, "holes_mb"),
                                "longest run": fi(f, "longest_mb")}})
        elif e == "vidmem_tick":
            tr.append({"ph": "X", "name": f"policy step ({f.get('trigger')})", "pid": PID_DRV, "tid": 1,
                       "ts": us(a), "dur": us(b) - us(a), "args": f})
            tr.append({"ph": "C", "name": "evicted MB", "pid": PID_DRV, "ts": us(b),
                       "args": {"evicted": fi(f, "evicted_mb")}})
        elif e == "rm_lock":
            what = "wait" if fi(f, "wait_us") else "hold"
            tr.append({"ph": "X", "name": f"{what} {f.get('lock')}/{f.get('mode')}", "pid": PID_LOCK,
                       "tid": ev["tid"], "ts": us(a), "dur": us(b) - us(a), "args": f})
            if ev["tid"] not in lock_threads:
                lock_threads.add(ev["tid"])
                tr.append({"ph": "M", "name": "thread_name", "pid": PID_LOCK, "tid": ev["tid"],
                           "args": {"name": f"{ev['task']}-{ev['tid']}"}})
        elif e == "drm_vblank_event_delivered":
            m = re.search(r"crtc=(\d+)", ev["body"])
            crtc = int(m.group(1)) if m else 0
            if crtc in flip_last:
                tr.append({"ph": "C", "name": f"frame interval ms crtc {crtc}", "pid": PID_DISP,
                           "ts": us(ev["ts"]), "args": {"ms": round((ev["ts"] - flip_last[crtc]) * 1000, 2)}})
            flip_last[crtc] = ev["ts"]
        elif e == "nvidia_dev_xid":
            tr.append({"ph": "i", "s": "g", "name": "Xid " + ev["body"][:80], "pid": PID_LOG, "tid": 1,
                       "ts": us(a)})
        elif e == "tracing_mark_write":
            tr.append({"ph": "i", "s": "g", "name": ev["body"][:80], "pid": PID_LOG, "tid": 2, "ts": us(a)})
    for ts, msg in klog:
        if "NVRM" in msg or "nvidia" in msg.lower() or "Xid" in msg:
            tr.append({"ph": "i", "s": "t", "name": msg[:100], "pid": PID_LOG, "tid": 3, "ts": us(ts)})
    for s in samples:
        t = us(s["t"])
        if "pma" in s:
            p = s["pma"]
            tr.append({"ph": "C", "name": "VRAM free MB", "pid": PID_GPU, "ts": t,
                       "args": {"free": p[0], "largest contiguous": p[1]}})
            tr.append({"ph": "C", "name": "reserve block free MB", "pid": PID_GPU, "ts": t,
                       "args": {"free": p[3], "largest contiguous": p[4]}})
        if "pcie_rx_mbs" in s:
            tr.append({"ph": "C", "name": "PCIe MB/s", "pid": PID_GPU, "ts": t,
                       "args": {"host->GPU": s["pcie_rx_mbs"], "GPU->host": s.get("pcie_tx_mbs", 0)}})
        if "util_gpu" in s:
            tr.append({"ph": "C", "name": "GPU busy %", "pid": PID_GPU, "ts": t, "args": {"gpu": s["util_gpu"]}})
        for pid, sm in (s.get("proc_sm") or {}).items():
            tr.append({"ph": "C", "name": "SM %", "pid": int(pid), "ts": t, "args": {"sm": sm}})
    with open(path, "w") as f:
        json.dump({"traceEvents": tr, "displayTimeUnit": "ms"}, f)

# --------------------------------------------------------------------------
# map: where memory lay in vidmem, and what each move took from where to where
# --------------------------------------------------------------------------

MAP_WHY = ["manual", "auto", "pressure", "alloc", "restore", "room", "idle-back", "hot", "access", "born",
           "compact", "-"]
MAP_MODE = {"-": 0, "evict": 1, "restore": 2, "move": 3}
RUNS_RE = re.compile(r"runs=\{([^}]*)\}")
COMM_RE = re.compile(r"\bcomm=(.*?) (?:n|why|old|kb|mode)=")      # a name may have spaces ("Isolated Web Co")


def load_mangohud(path):
    """(unix time of the log's start, from its name; [(elapsed s, frame time ms)])."""
    m = re.search(r"_(\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d)\.csv$", path)
    if not m:
        return None, []
    start = datetime.datetime.strptime(m.group(1), "%Y-%m-%d_%H-%M-%S").timestamp()
    rows, cols = [], None
    with open(path, errors="replace") as f:
        for ln in f:
            parts = ln.rstrip("\n").split(",")
            if cols is None:
                if "frametime" in parts and "elapsed" in parts:
                    cols = (parts.index("frametime"), parts.index("elapsed"))
                continue
            try:
                rows.append((int(parts[cols[1]]) / 1e9, float(parts[cols[0]])))
            except (ValueError, IndexError):
                pass
    return start, rows


def map_time(text, wall0):
    """--from/--to: seconds from the start, or HH:MM[:SS] of the recording's day."""
    if text is None:
        return None
    if ":" not in text:
        return float(text)
    if wall0 is None:
        sys.exit("nvtrace: this trace has no wall clock, give --from/--to in seconds from the start")
    day = datetime.datetime.fromtimestamp(wall0)
    h, m, *sec = (int(x) for x in text.split(":"))
    t = day.replace(hour=h, minute=m, second=sec[0] if sec else 0, microsecond=0)
    if t.timestamp() < wall0 - 60:
        t += datetime.timedelta(days=1)          # a recording across midnight
    return t.timestamp() - wall0


MAP_UNKNOWN = 0xffffffff       # "pid" of frames nothing is known about (a trace without a snapshot)


def map_infer(trace):
    """The map at the start of a trace that has no snapshot, from its own
    events: a frame whose first event frees it was held by that process, one
    whose first event allocates it was free; frames with no event stay not
    known. Returns (time of the first event, frames, [(pid, comm, start, n)])."""
    first, held, seen, top = None, [], bytearray(), 0

    def untouched(s, end):
        k = s
        while k < end:
            if seen[k]:
                k += 1
                continue
            z = k
            while z < end and not seen[z]:
                z += 1
            yield k, z - k
            k = z

    with open(trace, errors="replace") as f:
        for line in f:
            if "vidmem_ext" not in line or ("op=free" not in line and "op=alloc" not in line):
                continue
            e = parse_line(line)
            m = RUNS_RE.search(e["body"]) if e else None
            if not m:
                continue
            if first is None:
                first = e["ts"]
            c = COMM_RE.search(e["body"])
            runs = [int(x, 16) for x in m.group(1).split(",") if x]
            for j in range(0, len(runs) - 1, 2):
                s, n = runs[j], runs[j + 1]
                if s + n > top:
                    top = s + n
                    seen.extend(bytes(top - len(seen)))
                if e["f"].get("op") == "free":
                    for k, cnt in untouched(s, s + n):
                        held.append((fi(e["f"], "pid"), c.group(1) if c else "-", k, cnt))
                seen[s:s + n] = b"\x01" * n
    if first is None:
        return None
    frames = (top + 31) & ~31                   # whole 2 MB chunks: the size itself is not in the trace
    seen.extend(bytes(frames - len(seen)))
    for k, cnt in untouched(0, frames):
        held.append((MAP_UNKNOWN, "not known", k, cnt))
    return first, frames, held


def build_map(trace, meta, title, t_from=None, t_to=None, mangohud=()):
    """The data of map.html from a trace: the first snapshot (or the state at
    t_from) as one event, then every change. Later snapshots are compared
    with what the events built: a difference means lost events (`resync`)."""
    procs = [{"pid": 0, "comm": "free", "peak": 0}, {"pid": 1, "comm": "driver", "peak": 0}]
    index = {0: 0, 1: 1}
    counts = [0, 0]

    def proc(pid, comm):
        i = index.get(pid)
        if i is None:
            i = index[pid] = len(procs)
            procs.append({"pid": pid, "comm": comm or "-", "peak": 0})
            counts.append(0)
        elif comm and comm != "-" and i > 1:
            procs[i]["comm"] = comm
        return i

    def why_i(name):
        if name not in MAP_WHY:
            MAP_WHY.append(name)
        return MAP_WHY.index(name)

    own = None                 # owner (index into procs) of every frame
    frames = base_frame = 0
    snap = None                # a snapshot being read: (ts, owners, changes that arrived meanwhile, pins or None)
    # What compaction could not move at the last snapshot (op=pin): per frame
    # an index into `pins` + 1, 0 once the frame has changed hands since.
    pin = None
    pins = []                  # [kind, the move engine's reason or ""]
    start_mono = meta.get("start_mono")
    wall_at = (lambda ts: meta["start_wall"] + ts - start_mono) if "start_wall" in meta and start_mono else None
    base = None                # trace time of t = 0: the first snapshot
    t_out = None               # output starts here (base, or base + t_from)
    ev = {"t": [], "k": [], "p": [], "m": [], "w": [], "r": []}
    used = {"t": [], "mb": []}
    frag = {"t": [], "longest": []}
    moves, hints, notes, resync = [], [], [], []
    before = 0                 # changes seen before the first snapshot
    frame_mb = 1.0 / 16
    last_ts = 0.0

    def ms(ts):
        return round((ts - base) * 1000, 3)

    def emit(ts, k, p, m, w, r):
        ev["t"].append(ms(ts)); ev["k"].append(k); ev["p"].append(p); ev["m"].append(m); ev["w"].append(w)
        ev["r"].append(r)

    def pin_i(kind, reason):
        key = [kind or "-", reason if kind == "refused" and reason and reason != "-" else ""]
        if key not in pins:
            pins.append(key)
        return pins.index(key) + 1

    def pins_whole():
        """The pins as one event: (kind, start, count) per stretch."""
        r, start, cur = [], 0, pin[0] if frames else 0
        for f in range(1, frames + 1):
            o = pin[f] if f < frames else -1
            if o != cur:
                if cur:
                    r += [cur, start, f - start]
                start, cur = f, o
        return r

    def emit_state(ts):
        emit(ts, 2, 0, 0, why_i("-"), whole())
        if pin is not None:
            emit(ts, 6, 0, 0, why_i("-"), pins_whole())

    def whole():
        """The state as one event: (owner, start, count) per stretch."""
        r, start, cur = [], 0, own[0] if frames else 0
        for f in range(1, frames + 1):
            o = own[f] if f < frames else -1
            if o != cur:
                if cur:
                    r += [cur, start, f - start]
                start, cur = f, o
        return r

    def change(arr, cnt, k, p, runs):
        for j in range(0, len(runs), 2):
            s, n = runs[j], runs[j + 1]
            if s >= frames:
                continue
            n = min(n, frames - s)
            if cnt is not None:
                for o, c in collections.Counter(arr[s:s + n]).items():
                    cnt[o] -= c
                cnt[p if k else 0] += n
            arr[s:s + n] = array.array("H", [p if k else 0]) * n

    def unpin(arr, runs):
        for j in range(0, len(runs), 2):
            s, n = runs[j], min(runs[j + 1], max(0, frames - runs[j]))
            arr[s:s + n] = array.array("B", [0]) * n

    def note_used(ts):
        if t_out is None or ts < t_out:
            return
        t = ms(ts)
        mb = round((frames - counts[0]) * frame_mb, 1)
        if used["t"] and t - used["t"][-1] < 20:
            used["mb"][-1] = mb
        else:
            used["t"].append(t); used["mb"].append(mb)

    inferred = False
    with open(trace, errors="replace") as f:
        has_snap = any("op=snapend" in line for line in f)
    if not has_snap:
        # No snapshot (the driver refused it, or the recording was cut): the
        # start is what the events themselves tell.
        got = map_infer(trace)
        if got is not None:
            inferred = True
            base, frames, held = got
            t_out = base + (t_from or 0)
            own = array.array("H", [0]) * frames
            for pid, comm, s, n in held:
                change(own, None, 1, proc(pid, comm), [s, n])
            counts[:] = [0] * len(procs)
            for o, c in collections.Counter(own).items():
                counts[o] = c
            for i, c in enumerate(counts):
                procs[i]["peak"] = max(procs[i]["peak"], c)

    with open(trace, errors="replace") as f:
        for line in f:
            if "vidmem_" not in line and "tracing_mark_write" not in line:
                continue
            e = parse_line(line)
            if e is None:
                continue
            ts, name, fl = e["ts"], e["event"], e["f"]
            m = COMM_RE.search(e["body"])
            if m:
                fl["comm"] = m.group(1)
            last_ts = max(last_ts, ts)
            if base is not None and t_to is not None and ts - base > t_to:
                break
            if name == "vidmem_ext":
                m = RUNS_RE.search(e["body"])
                runs = [int(x, 16) for x in m.group(1).split(",") if x] if m else []
                op, pid = fl.get("op"), fi(fl, "pid")
                if op == "snap":
                    if len(runs) < 2 or runs[1] == 0:
                        continue
                    if own is not None and runs[1] != frames:
                        sys.exit("nvtrace: the size of vidmem changed inside the trace (two GPUs?)")
                    base_frame, frames = runs[0], runs[1]
                    snap = [ts, array.array("H", [0]) * frames, [], None]
                elif op == "have" and snap is not None:
                    change(snap[1], None, 1, proc(pid, fl.get("comm")), runs)
                elif op == "pin" and snap is not None:
                    if snap[3] is None:
                        snap[3] = array.array("B", [0]) * frames
                    code = pin_i(fl.get("mode"), fl.get("comm"))
                    for j in range(0, len(runs), 2):
                        s, n = runs[j], min(runs[j + 1], max(0, frames - runs[j]))
                        snap[3][s:s + n] = array.array("B", [code]) * n
                elif op == "snapend" and snap is not None:
                    sts, arr, late, parr = snap
                    snap = None
                    for k, p, r in late:        # what changed while the snapshot was taken
                        change(arr, None, k, p, r)
                        if parr is not None:
                            unpin(parr, r)
                    if own is None:
                        own = arr
                        base = sts
                        t_out = base + (t_from or 0)
                    else:
                        diff = sum(1 for a, b in zip(own, arr) if a != b)
                        if t_out is not None and sts >= t_out and ev["t"]:
                            resync.append([ms(sts), diff])
                        if diff:
                            own = arr
                            if ev["t"]:
                                emit(ts, 2, 0, 0, why_i("-"), whole())
                    # A snapshot of a driver that tells no pins leaves the old ones (frames since changed are out of them).
                    if parr is not None:
                        pin = parr
                    counts[:] = [0] * len(procs)
                    for o, c in collections.Counter(own).items():
                        counts[o] = c
                    for i, c in enumerate(counts):
                        procs[i]["peak"] = max(procs[i]["peak"], c)
                    if not ev["t"] and ts >= t_out:
                        emit_state(max(ts, t_out))
                    elif parr is not None and ev["t"]:
                        emit(ts, 6, 0, 0, why_i("-"), pins_whole())
                    note_used(ts)
                elif op in ("free", "alloc"):
                    if own is None:
                        before += 1
                        continue
                    k, p = int(op == "alloc"), proc(pid, fl.get("comm"))
                    if snap is not None:
                        snap[2].append((k, p, runs))
                    if ts >= t_out and not ev["t"]:
                        emit_state(t_out)
                    change(own, counts, k, p, runs)
                    if pin is not None:
                        unpin(pin, runs)
                    procs[p]["peak"] = max(procs[p]["peak"], counts[p])
                    if ts >= t_out:
                        emit(ts, k, p, MAP_MODE.get(fl.get("mode"), 0), why_i(fl.get("why", "-")), runs)
                    note_used(ts)
                elif op == "window" and own is not None and ts >= t_out and ev["t"]:
                    emit(ts, 5, proc(pid, fl.get("comm")) if pid > 1 else 0, 3, why_i(fl.get("why", "-")), runs)
                continue
            if base is None or ts < t_out:
                continue
            if name == "vidmem_move":
                op = {"evict": 1, "restore": 2, "move": 3}.get(fl.get("op"), 0)
                if op:
                    moves.append([ms(ts), round(fi(fl, "total_us") / 1000, 2), proc(fi(fl, "pid"), fl.get("comm")), op,
                                  why_i(fl.get("why", "-")), fi(fl, "kb"), fi(fl, "allocs"), fi(fl, "failed")])
            elif name == "vidmem_hint":
                hints.append([ms(ts), proc(fi(fl, "pid"), fl.get("comm")), fl.get("new", "-")])
            elif name == "vidmem_frag":
                if not frag["t"] or ms(ts) - frag["t"][-1] >= 20:
                    frag["t"].append(ms(ts)); frag["longest"].append(fi(fl, "longest_mb"))
            elif name == "vidmem_makeroom":
                if fi(fl, "us") >= 5000 or not fi(fl, "ok"):
                    notes.append([ms(ts), "room", f"{fl.get('comm')} waited {fi(fl, 'us') / 1000:.0f} ms for room "
                                  f"for {fi(fl, 'kb') >> 10} MB" + ("" if fi(fl, "ok") else ": none made")])
            elif name == "vidmem_reserve":
                if fl.get("outcome") == "denied":
                    notes.append([ms(ts), "denied", f"{fl.get('comm')} was denied {fi(fl, 'kb') >> 10} MB"])
            elif name == "tracing_mark_write":
                m = re.search(r"gamerec: mark (\d+) ?(.*)", e["body"])
                if m:
                    notes.append([ms(ts), "mark", f"mark {m.group(1)}: {m.group(2)}".rstrip(": ")])

    if own is None:
        sys.exit("nvtrace: no snapshot of vidmem in this trace (vidmem_ext events): recorded with a driver "
                 "older than the map, or the driver had no allocation yet")
    if not ev["t"]:
        if base + (t_from or 0) > last_ts:
            sys.exit("nvtrace: --from is past the end of the trace")
        emit_state(t_out)
    wall0 = wall_at(base) if wall_at else None
    ft = {"t": [], "ms": []}
    if wall0 is not None:
        for start, frames_list in mangohud:      # (unix time of the log's start, [(elapsed s, ms)])
            for el, dur in frames_list:
                t = (start + el - wall0) * 1000
                if ev["t"][0] <= t <= ms(last_ts):
                    ft["t"].append(round(t, 1)); ft["ms"].append(round(dur, 2))
        order = sorted(range(len(ft["t"])), key=ft["t"].__getitem__)
        ft = {"t": [ft["t"][i] for i in order], "ms": [ft["ms"][i] for i in order]}
    return {"title": title, "frames": frames, "base_frame": base_frame, "frame_kb": 64, "procs": procs,
            "why": MAP_WHY, "ev": ev, "moves": moves, "hints": hints, "notes": sorted(notes), "used": used,
            "frag": frag, "frametimes": ft, "resync": resync, "t_first": ev["t"][0], "t_last": ms(last_ts),
            "wall0": wall0, "before_first_snapshot": before, "inferred": inferred, "pins": pins}


def write_map(data, out):
    html = rfile(os.path.join(HERE, "map.html"))
    blob = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    wfile(out, html.replace("__DATA__", blob))
    lost = sum(r[1] for r in data["resync"])
    # A move whose frames are in the trace: a change made by the move engine for the same process inside it.
    ev, placed, kinds = data["ev"], 0, [0, 0, 0, 0]
    for t, dur, p, op, *_ in data["moves"]:
        kinds[op] += 1
        i = bisect.bisect_left(ev["t"], t - dur - 2)
        while i < len(ev["t"]) and ev["t"][i] <= t + 0.5:
            if ev["k"][i] in (0, 1) and ev["m"][i] and ev["p"][i] == p:
                placed += 1
                break
            i += 1
    print(f"map: {out} ({os.path.getsize(out) >> 10} KB): {data['frames'] >> 4} MB of vidmem, "
          f"{len(ev['t'])} changes, {len(data['procs']) - 2} processes\n"
          f"map: moves {len(data['moves'])} (to sysmem {kinds[1]}, back {kinds[2]}, inside vidmem {kinds[3]}), "
          f"{placed} with their frames, {ev['k'].count(5)} windows\n"
          + (f"map: what compaction could not move is told at {ev['k'].count(6)} snapshot(s): "
             + ", ".join(sorted({k for k, _ in data["pins"]})) + "\n" if data["pins"] else
             "map: the snapshots do not tell what compaction could not move (an older driver)\n")
          + ("map: no snapshot in the trace: the start is inferred from the events, frames with no event are "
             "shown as not known\n" if data.get("inferred") else "")
          + (f"map: {len(data['resync'])} later snapshot(s), {lost} frames differed from the events"
             if data["resync"] else "map: no later snapshot to check the events against"))
    return lost


def make_map(path, out=None, title=None, t_from=None, t_to=None, mangohud=()):
    """map.html for a recording's directory or a trace file; returns the
    frames that later snapshots found different from the events."""
    trace = os.path.join(path, "trace.txt") if os.path.isdir(path) else path
    if not os.path.exists(trace):
        sys.exit(f"nvtrace: no trace at {trace}")
    d = os.path.dirname(os.path.abspath(trace))
    meta = {}
    if os.path.exists(os.path.join(d, "meta.json")):
        with open(os.path.join(d, "meta.json")) as f:
            meta = json.load(f)
    wall0 = meta.get("start_wall")
    logs = [x for x in (load_mangohud(p) for p in mangohud) if x[0] is not None]
    data = build_map(trace, meta, title or os.path.basename(d), map_time(t_from, wall0), map_time(t_to, wall0), logs)
    return write_map(data, out or os.path.join(d, "map.html"))


def cmd_map(a):
    make_map(a.path, a.out, a.title, a.t_from, a.t_to, a.mangohud or [])


def cmd_report(a):
    text, events, samples, klog, t0 = build_report(a.dir, a.gap_ms, a.top)
    wfile(os.path.join(a.dir, "summary.txt"), text)
    perfetto(events, samples, klog, t0, os.path.join(a.dir, "trace.json"))
    sys.stdout.write(text)
    print(f"timeline: {os.path.join(a.dir, 'trace.json')} (open in https://ui.perfetto.dev)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="sub", required=True)
    r = sub.add_parser("record")
    r.add_argument("-o", "--out")
    r.add_argument("-d", "--duration", type=float, default=0)
    r.add_argument("--lock-us", type=int, default=1000, help="trace RM lock waits/holds from this long (us)")
    r.add_argument("--interval", type=int, default=100, help="vidmem_pma sample interval (ms)")
    r.add_argument("--nvml-ms", type=int, default=500, help="NVML sample interval (ms)")
    r.add_argument("--buf-kb", type=int, default=8192, help="trace buffer per CPU (KB)")
    r.add_argument("--sched", action="store_true", help="also sched_switch/wakeup (big)")
    r.add_argument("cmd", nargs="*")
    m = sub.add_parser("mark")
    m.add_argument("text", nargs="*")
    lv = sub.add_parser("live")
    lv.add_argument("--lock-us", type=int, default=2000)
    rp = sub.add_parser("report")
    rp.add_argument("dir")
    rp.add_argument("--gap-ms", type=float, default=25, help="a frame interval longer than this is a gap")
    rp.add_argument("--top", type=int, default=12)
    mp = sub.add_parser("map")
    mp.add_argument("path", help="a recording's directory, or a trace file")
    mp.add_argument("-o", "--out", help="default: map.html next to the trace")
    mp.add_argument("--from", dest="t_from", help="seconds from the start, or HH:MM[:SS]")
    mp.add_argument("--to", dest="t_to")
    mp.add_argument("--mangohud", action="append", help="a MangoHud log of the session (frame times on the timeline)")
    mp.add_argument("--title")
    a = ap.parse_args()
    {"record": cmd_record, "mark": cmd_mark, "live": cmd_live, "report": cmd_report, "map": cmd_map}[a.sub](a)


if __name__ == "__main__":
    main()
