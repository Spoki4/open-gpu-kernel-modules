#!/usr/bin/env python3
"""The player's view of a gamerec recording: what of the driver's work the
player of the focused game could have noticed.

The goal of every change of the eviction policy is that the player notices
nothing. Measured from what a recording holds:
  - frames: the game's frame times (MangoHud);
  - the driver's work in the game's way (nvtrace): the game's channels
    stopped by the driver (vidmem_pause), the game's threads waiting for an
    RM lock (rm_lock, waits of 1 ms or more), the game waiting inside its
    own allocations (vidmem_makeroom);
  - the game's memory in sysmem and its window state, per policy step
    (vidmem_proc).

A long frame (twice the median of the frames before it, and 8 ms more: a
missed refresh at 60 Hz, what a player sees as a hitch) is the driver's when the driver's work in the game's way overlaps it
by at least half of what it was too long (2 ms at least); the others are
the game's own (loading, shader compilation, its own streaming). Only
frames while the game is focused count; for the time it is only visible (the
user in another window, the game on the other monitor) its memory in sysmem
and its frame times are given apart.

The verdict "unnoticed": no frame of the driver's, nothing of the game in
sysmem, and the driver in the game's way under 1 ms per frame in every
second.

Clocks: MangoHud names its log by the second it started and counts frames
from there; the trace runs on CLOCK_MONOTONIC. The offset between them is
found to a few ms from the driver's long pauses of the game, which show as
long frames; without such pauses it stays the file name's (about 1 s).

Usage: player.py RECORDING_DIR    (also part of `gamerec.sh report`)
"""
import bisect
import datetime
import glob
import json
import os
import re
import sys

LINE_RE = re.compile(r"^\s*(?P<task>.+?)-(?P<tid>\d+)\s+(?:\(\s*(?P<tgid>[\d-]+)\)\s+)?\[\d+\]\s+\S+\s+"
                     r"(?P<ts>\d+\.\d+): (?P<ev>\w+): (?P<body>.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")
EVENTS = (": vidmem_proc:", ": vidmem_pause:", ": rm_lock:", ": vidmem_move:", ": vidmem_makeroom:",
          ": vidmem_alloc:")
PHASES = (("check_us", "before PMA"), ("pma_us", "PMA"), ("room_us", "make-room and retry"),
          ("lockwait_us", "waiting for the GPU lock"), ("rest_us", "the rest (descriptor, GSP, mappings)"))
# Threads of a game under Wine/Proton that are not named after it.
GAME_THREADS = ("dxvk-", "vkd3d", "wine_")
LONG_RATIO, LONG_MS = 2.0, 8.0
WINDOW = 120            # frames the median is taken over
OFFSET_SPAN = 1.2       # s searched around the file name's clock
ANCHOR_MS = 15.0        # a pause this long shows as a long frame


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


def merge(iv):
    """Intervals (a, b) -> sorted disjoint ones."""
    out = []
    for a, b in sorted(iv):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def overlap(iv, starts, a, b):
    """Seconds of the disjoint sorted intervals iv (starts: their starts) inside [a, b]."""
    i = max(bisect.bisect_right(starts, a) - 1, 0)
    s = 0.0
    while i < len(iv) and iv[i][0] < b:
        s += max(0.0, min(b, iv[i][1]) - max(a, iv[i][0]))
        i += 1
    return s


class Trace:
    def __init__(self, path):
        self.proc = {}          # pid -> [(ts, comm, ram_mb, vram_mb, hint)]
        self.pause = []         # (end ts, pid, paused s)
        self.waits = []         # (end ts, task, tgid or None, wait s, lock, holder tid, where the holder took it)
        self.moves = []         # (ts, pid, op, why, mb, total s)
        self.makeroom = []      # (ts, pid, s)
        self.allocs = []        # (end ts, pid, comm, kb, contig, total s, {phase: s}): allocations that took long
        self.names = {}         # tid -> task name
        self.has_tgid = False
        with open(path, errors="replace") as f:
            for ln in f:
                if not any(e in ln for e in EVENTS):
                    continue
                m = LINE_RE.match(ln)
                if not m:
                    continue
                ts, ev, kv = float(m.group("ts")), m.group("ev"), dict(KV_RE.findall(m.group("body")))
                tgid = m.group("tgid")
                if tgid is not None and tgid.strip("-"):
                    self.has_tgid = True
                self.names[int(m.group("tid"))] = m.group("task").strip()
                try:
                    if ev == "vidmem_proc":
                        self.proc.setdefault(int(kv["pid"]), []).append(
                            (ts, kv["comm"], int(kv["ram_mb"]), int(kv["vram_mb"]), kv["hint"]))
                    elif ev == "vidmem_pause":
                        self.pause.append((ts, int(kv["pid"]), int(kv["paused_us"]) / 1e6))
                    elif ev == "rm_lock" and int(kv["wait_us"]) > 0:
                        self.waits.append((ts, m.group("task").strip(),
                                           int(tgid) if (tgid and tgid.strip("-")) else None,
                                           int(kv["wait_us"]) / 1e6, kv.get("lock", "?"), int(kv.get("holder", 0)),
                                           kv.get("holder_at", "")))
                    elif ev == "vidmem_move":
                        self.moves.append((ts, int(kv["pid"]), kv.get("op"), kv.get("why"),
                                           int(kv.get("kb", 0)) >> 10, int(kv.get("total_us", 0)) / 1e6))
                    elif ev == "vidmem_alloc":
                        self.allocs.append((ts, int(kv["pid"]), kv["comm"], int(kv["kb"]), int(kv["contig"]),
                                            int(kv["total_us"]) / 1e6,
                                            {k: int(kv[k]) / 1e6 for k, _ in PHASES}))
                    elif ev == "vidmem_makeroom":
                        self.makeroom.append((ts, int(kv["pid"]), int(kv.get("us", 0)) / 1e6))
                except (KeyError, ValueError):
                    pass


def holder(tr, w):
    """Who held the lock a wait was for, and in what: a work item of the driver, or a call of a process."""
    name = tr.names.get(w[5], str(w[5])) if w[5] else "?"
    at = w[6]
    if at.startswith("workItemLocksAcquire"):
        return f"{name}, a work item of the driver (policy step, headroom)"
    if at.startswith("serverTopLock_Prologue"):
        return f"{name}, an RM call of it"
    if at.startswith("RmUnixRmApiPrologue"):
        return f"{name}, a procfs command or window hints"
    return name


def find_game(tr, name, t0, t1):
    """The pid whose name the MangoHud log carries, the biggest in vidmem while it ran."""
    key = name.lower()
    best = None
    for pid, rows in tr.proc.items():
        comm = rows[-1][1].lower()
        if not (comm.startswith(key) or key.startswith(comm.split(".")[0])):
            continue
        vram = max((r[3] for r in rows if t0 <= r[0] <= t1), default=-1)
        if vram >= 0 and (best is None or vram > best[1]):
            best = (pid, vram)
    return best[0] if best else None


def analyse(d):
    sysd = os.path.join(d, "system") if os.path.isdir(os.path.join(d, "system")) else d
    tpath = os.path.join(sysd, "nvtrace", "trace.txt")
    mpath = os.path.join(sysd, "nvtrace", "meta.json")
    if not (os.path.exists(tpath) and os.path.exists(mpath)):
        return ["player: no driver trace in this recording"]
    meta = json.load(open(mpath))
    mono0, wall0 = meta["start_mono"], meta["start_wall"]
    files = sorted(f for f in glob.glob(os.path.join(d, "mangohud", "*.csv")) if not f.endswith("_summary.csv"))
    logs = []
    for p in files:
        start, fr = load_mangohud(p)
        if start is not None and len(fr) >= 600:
            logs.append((p, start, fr))
    if not logs:
        return ["player: no MangoHud frame log of a game in this recording"]
    tr = Trace(tpath)

    def clock(mono):
        return datetime.datetime.fromtimestamp(wall0 + (mono - mono0)).strftime("%H:%M:%S.%f")[:-4]

    out = []
    for p, start, fr in logs:
        name = os.path.basename(p).rsplit("_", 2)[0]
        base = mono0 + (start.timestamp() - wall0)
        game = find_game(tr, name, base, base + fr[-1][0])
        if game is None:
            out.append(f"player ({name}): no process of that name in the driver's trace")
            continue
        rows = tr.proc[game]
        comm = rows[-1][1]
        steps = [r[0] for r in rows]

        def state(t):
            i = bisect.bisect_right(steps, t) - 1
            return rows[i] if i >= 0 else None

        def focused(t):
            r = state(t)
            return r is not None and r[4] == "f" and t - r[0] < 6

        # The driver's work in the game's way.
        threads = (lambda w: w[2] == game) if tr.has_tgid else \
                  (lambda w: w[1] == comm[:15] or w[1].startswith(GAME_THREADS))
        pauses = [(t - s, t) for t, pid, s in tr.pause if pid == game]
        waits = [(t - s, t, w) for w in tr.waits if threads(w) for t, s in [(w[0], w[3])]]
        rooms = [(t - s, t) for t, pid, s in tr.makeroom if pid == game]
        t_end = base + fr[-1][0] + OFFSET_SPAN
        pauses = [x for x in pauses if base - OFFSET_SPAN <= x[1] <= t_end]
        waits = [x for x in waits if base - OFFSET_SPAN <= x[1] <= t_end]
        rooms = [x for x in rooms if base - OFFSET_SPAN <= x[1] <= t_end]
        iv = merge([(a, b) for a, b in pauses] + [(a, b) for a, b, _ in waits] + rooms)
        ivs = [x[0] for x in iv]

        # Clocks: the offset that puts the driver's long pauses of the game on long frames.
        ends, fts = [], []
        for el, ft in fr:
            ends.append(base + el)
            fts.append(ft)
        anchors = [(a, b) for a, b in pauses if (b - a) * 1000 >= ANCHOR_MS]
        offset, matched = 0.0, 0
        if anchors:
            med_all = sorted(fts)[len(fts) // 2]
            best = (0, 0.0)
            for k in range(-int(OFFSET_SPAN * 500), int(OFFSET_SPAN * 500) + 1):
                o = k / 500.0
                n = 0
                for a, b in anchors:
                    i = bisect.bisect_left(ends, a - o)
                    while i < len(ends) and ends[i] + o - fts[i] / 1000 < b:
                        if fts[i] >= med_all + 0.6 * (b - a) * 1000:
                            n += 1
                            break
                        i += 1
                if n > best[0] or (n == best[0] and abs(o) < abs(best[1])):
                    best = (n, o)
            if best[0] >= max(2, len(anchors) // 2):
                matched, offset = best
        # Frames while focused.
        win, win_sorted = [], []
        long_frames, driver_frames, game_frames = 0, [], 0
        n_focus, ms_sys, n_sys, ms_nosys, n_nosys = 0, 0.0, 0, 0.0, 0
        vis = {"n": 0, "ms_sys": 0.0, "n_sys": 0, "ms_no": 0.0, "n_no": 0}
        for i, (e, ft) in enumerate(zip(ends, fts)):
            e += offset
            if ft <= 0 or ft > 5000:
                continue
            if len(win) == WINDOW:
                old = win.pop(0)
                del win_sorted[bisect.bisect_left(win_sorted, old)]
            med = win_sorted[len(win_sorted) // 2] if win_sorted else ft
            win.append(ft)
            bisect.insort(win_sorted, ft)
            if not focused(e):
                r = state(e)
                if r is not None and r[4] == "v" and e - r[0] < 6:
                    vis["n"] += 1
                    k = "sys" if r[2] > 0 else "no"
                    vis["ms_" + k] += ft
                    vis["n_" + k] += 1
                continue
            n_focus += 1
            r = state(e)
            if r[2] > 0:
                ms_sys += ft
                n_sys += 1
            else:
                ms_nosys += ft
                n_nosys += 1
            if len(win) < 30 or ft <= max(LONG_RATIO * med, med + LONG_MS):
                continue
            long_frames += 1
            excess = ft - med
            ov = overlap(iv, ivs, e - ft / 1000, e) * 1000
            if ov >= max(2.0, 0.5 * excess):
                driver_frames.append((e, ft, med, ov))
            else:
                game_frames += 1
        # Memory in sysmem and focus time, per policy step.
        focus_s, sys_mbs, sys_max, sys_s = 0.0, 0.0, 0, 0.0
        vis_s, vsys_mbs, vsys_max, vsys_s = 0.0, 0.0, 0, 0.0
        for j, r in enumerate(rows):
            if r[4] not in ("f", "v") or r[0] < base or r[0] > ends[-1] + offset:
                continue
            dt = min((rows[j + 1][0] if j + 1 < len(rows) else r[0] + 2) - r[0], 6.0)
            if r[4] == "f":
                focus_s += dt
                if r[2] > 0:
                    sys_mbs += r[2] * dt
                    sys_max = max(sys_max, r[2])
                    sys_s += dt
            else:
                vis_s += dt
                if r[2] > 0:
                    vsys_mbs += r[2] * dt
                    vsys_max = max(vsys_max, r[2])
                    vsys_s += dt
        # The driver in the game's way while focused: in all, and the worst second.
        in_way, worst_sec, per_sec = 0.0, (0.0, 0), {}
        for a, b in iv:
            if focused(b):
                in_way += b - a
                per_sec[int(b)] = per_sec.get(int(b), 0.0) + (b - a)
        fps_per_sec = {}
        for e in ends:
            fps_per_sec[int(e + offset)] = fps_per_sec.get(int(e + offset), 0) + 1
        over = [s for s, v in per_sec.items() if v * 1000 > max(fps_per_sec.get(s, 60), 1)]
        if per_sec:
            s = max(per_sec, key=per_sec.get)
            worst_sec = (per_sec[s], s)
        hours = max(focus_s / 3600, 1e-9)
        verdict = "unnoticed" if (not driver_frames and sys_max == 0 and not over) else "noticed"

        if focus_s < 60:
            out.append(f"player ({comm} pid {game}, {os.path.basename(p)}): focused under a minute, nothing to say")
            continue
        out.append(f"player ({comm} pid {game}): focused {focus_s / 60:.0f} min, {n_focus} frames; "
                   + (f"clocks matched by {matched} of {len(anchors)} long pauses (offset {offset * 1000:+.0f} ms)"
                      if matched else "clocks from the log's name (about 1 s)")
                   + ("" if tr.has_tgid else "; the game's threads by name"))
        out.append(f"  verdict: {verdict}")
        if driver_frames:
            w = max(driver_frames, key=lambda x: x[1])
            out.append(f"  long frames caused by the driver: {len(driver_frames)} ({len(driver_frames) / hours:.1f} an hour), "
                       f"worst {w[1]:.0f} ms at {clock(w[0])}")
        else:
            out.append("  long frames caused by the driver: none")
        out.append(f"  the game's own long frames: {game_frames} (loading, shaders, its own streaming)")
        if sys_max:
            out.append(f"  the game's memory in sysmem while focused: up to {sys_max} MB, {sys_s:.0f} s "
                       f"({sys_mbs:.0f} MB*s); its frames meanwhile {ms_sys / max(n_sys, 1):.1f} ms, "
                       f"{ms_nosys / max(n_nosys, 1):.1f} ms otherwise")
        else:
            out.append("  the game's memory in sysmem while focused: none")
        out.append(f"  the driver in the game's way: {in_way:.1f} s in all ({in_way * 1000 / max(focus_s / 60, 1e-9):.1f} ms a minute: "
                   f"of it pauses {sum(b - a for a, b in merge(pauses) if focused(b)):.1f} s, "
                   f"lock waits {sum(b - a for a, b in merge([(a, b) for a, b, _ in waits]) if focused(b)):.1f} s); "
                   f"over 1 ms a frame in {len(over)} seconds"
                   + (f", worst {worst_sec[0] * 1000:.0f} ms at {clock(worst_sec[1])[:8]}" if per_sec else ""))
        if vis_s >= 10:
            out.append(f"  only visible (another window in focus) {vis_s / 60:.0f} min: "
                       + (f"its memory in sysmem up to {vsys_max} MB for {vsys_s:.0f} s ({vsys_mbs:.0f} MB*s), "
                          f"its frames meanwhile {vis['ms_sys'] / max(vis['n_sys'], 1):.1f} ms, "
                          f"{vis['ms_no'] / max(vis['n_no'], 1):.1f} ms otherwise" if vsys_max else
                          "none of its memory in sysmem"))
        sa = slow_allocs(tr, base - OFFSET_SPAN, t_end)
        if sa:
            out.append("  allocations that held the API lock long (vidmem_alloc), by phase in ms:")
            out.extend(sa)
        if driver_frames:
            by = {}
            for e, ft, med, ov in driver_frames:
                k = "pauses of the game"
                for wa, wb, w in waits:
                    if wb > e - ft / 1000 and wa < e:
                        k = "lock held by " + holder(tr, w)
                        break
                by[k] = by.get(k, 0) + 1
            out.append("  driver frames by cause: " + ", ".join(f"{k} {v}" for k, v in sorted(by.items(), key=lambda x: -x[1])))
            out.append("  worst frames caused by the driver:")
            slow = [x for x in tr.allocs if base - OFFSET_SPAN <= x[0] <= t_end]
            for e, ft, med, ov in sorted(driver_frames, key=lambda x: -x[1])[:8]:
                a = e - ft / 1000
                why = []
                for pa, pb in pauses:
                    if pb > a and pa < e:
                        mv = [m for m in tr.moves if m[1] == game and abs(m[0] - pb) < 0.05]
                        why.append(f"paused {(pb - pa) * 1000:.0f} ms"
                                   + (f" ({mv[0][2]} of {mv[0][4]} MB of its memory, why={mv[0][3]})" if mv else ""))
                wt = {}
                for wa, wb, w in waits:
                    if wb > a and wa < e:
                        k = (w[4], holder(tr, w))
                        wt[k] = wt.get(k, 0.0) + (min(wb, e) - max(wa, a))
                for (lock, who), s in sorted(wt.items(), key=lambda x: -x[1])[:2]:
                    why.append(f"waited {s * 1000:.0f} ms for the {lock} lock (held by {who})")
                for x in slow:
                    if a - 0.1 <= x[0] - x[5] and x[0] - x[5] <= e and x[1] != game:
                        ph = max(x[6], key=x[6].get)
                        why.append(f"{x[2]} allocating {x[3] >> 10} MB{' contiguous' if x[4] else ''} took "
                                   f"{x[5] * 1000:.0f} ms, most in {dict(PHASES)[ph]} ({x[6][ph] * 1000:.0f} ms)")
                        break
                out.append(f"    {clock(e)}  {ft:.0f} ms (median {med:.1f}): " + "; ".join(why))
    return out


def slow_allocs(tr, t0, t1):
    """Allocations that took the lock-trace threshold or more, per process and size: phases on average."""
    groups = {}
    for x in tr.allocs:
        if t0 <= x[0] <= t1:
            groups.setdefault((x[2], x[3] >> 10, x[4]), []).append(x)
    lines = []
    for (comm, mb, contig), xs in sorted(groups.items(), key=lambda g: -sum(x[5] for x in g[1]))[:6]:
        avg = {k: sum(x[6][k] for x in xs) / len(xs) for k, _ in PHASES}
        lines.append(f"    {comm} {mb} MB{' contiguous' if contig else ''}: {len(xs)} times, "
                     f"{sum(x[5] for x in xs) / len(xs) * 1000:.1f} ms on average: "
                     + ", ".join(f"{name} {avg[k] * 1000:.1f}" for k, name in PHASES))
    return lines


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    print("\n".join(analyse(sys.argv[1])))


if __name__ == "__main__":
    main()
