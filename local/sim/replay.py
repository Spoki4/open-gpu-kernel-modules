#!/usr/bin/env python3
"""A gamerec recording -> a simulator scenario (sim.c).

Every policy step the driver traced (vidmem_tick, timer or window hints) is
replayed: per process the window hint, the GPU activity (idle seconds, engine
samples) and the memory it held (vidmem + sysmem, how much of it movable),
then a step. The simulator then runs the policy of the current tree on the
same input: what it would have moved, and what sat in sysmem.

The recorded per-process vram/ram go along as `rec` lines, so
`sim -cmp PID` prints recorded against simulated side by side (once a
minute). Marks of the recording become `mark` lines.

  replay.py RECORDING [--from HH:MM[:SS]] [--to HH:MM[:SS]] [-o FILE]
            [--expect "HH:MM:SS METRIC PID OP VALUE"]...

--expect adds a check at that wall-clock time (sim.c `expect`): a clip of a
recording with the behaviour it should have had becomes a test.

RECORDING: a gamerec id (20261106_112014), local/game-logs/<id> or
/var/lib/gamerec/<id>. Needs read access to the root-owned half (the user's
group has it).
"""
import argparse
import datetime as dt
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SYS_LOGS = "/var/lib/gamerec"

PROC = re.compile(r"\s(\d+\.\d+): vidmem_proc: pid=(\d+) comm=(.*?) vram_mb=(\d+) ram_mb=(\d+) "
                  r"eligible_mb=(\d+) hint=(\S) inherited=(\d+) idle_s=(\d+) samples=(\d+) "
                  r"submit_s=(\d+) never=(\d)")
MAKEROOM = re.compile(r"\s(\d+\.\d+): vidmem_makeroom: pid=(\d+) comm=(.*?) kb=(\d+) .*?level=3")
TICK = re.compile(r"\s(\d+\.\d+): vidmem_tick: .*?avail_mb=(\d+) .*?trigger=(\w+)")


def find_recording(arg):
    for d in (arg, os.path.join(SYS_LOGS, arg), os.path.join(HERE, "..", "game-logs", arg, "system")):
        if os.path.isfile(os.path.join(d, "nvtrace", "trace.txt")):
            return os.path.realpath(d)
    sys.exit(f"replay.py: no nvtrace/trace.txt for {arg}")


def parse_hm(s, day):
    parts = [int(x) for x in s.split(":")]
    while len(parts) < 3:
        parts.append(0)
    return dt.datetime.combine(day, dt.time(*parts)).timestamp()


def ticks(trace):
    """(boottime, avail_mb, trigger, [proc dicts]) per policy step."""
    procs = []
    with open(trace, errors="replace") as f:
        for line in f:
            if "vidmem_proc:" in line:
                m = PROC.search(line)
                if m:
                    procs.append(dict(pid=int(m[2]), comm=m[3].replace(" ", "_") or "?", vram=int(m[4]),
                                      ram=int(m[5]), elig=int(m[6]), hint=m[7], inh=int(m[8]),
                                      idle=int(m[9]), samples=int(m[10]), never=int(m[12])))
            elif "vidmem_makeroom:" in line:
                m = MAKEROOM.search(line)
                if m:
                    yield float(m[1]), None, "comp", dict(pid=int(m[2]), comm=m[3], kb=int(m[4]))
            elif "vidmem_tick:" in line:
                m = TICK.search(line)
                if m:
                    yield float(m[1]), int(m[2]), m[3], procs
                procs = []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recording")
    ap.add_argument("--from", dest="frm")
    ap.add_argument("--to")
    ap.add_argument("-o", "--out")
    ap.add_argument("--expect", action="append", default=[])
    a = ap.parse_args()

    rec = find_recording(a.recording)
    meta = json.load(open(os.path.join(rec, "nvtrace", "meta.json")))
    start_wall = dt.datetime.fromisoformat(meta["start"])
    boot2unix = start_wall.timestamp() - meta["start_mono"]
    lo = parse_hm(a.frm, start_wall.date()) - boot2unix if a.frm else 0
    hi = parse_hm(a.to, start_wall.date()) - boot2unix if a.to else float("inf")

    events = [t for t in ticks(os.path.join(rec, "nvtrace", "trace.txt")) if lo <= t[0] <= hi]
    steps = [e for e in events if e[2] != "comp"]
    comps = [e for e in events if e[2] == "comp"]
    if not steps:
        sys.exit("replay.py: no policy steps in that window")
    cap = statistics.median(avail + sum(p["vram"] for p in procs) for _, avail, _, procs in steps)
    t0 = steps[0][0]

    marks = []
    mdir = os.path.join(rec, "marks")
    if os.path.isdir(mdir):
        for name in sorted(os.listdir(mdir)):
            m = re.match(r"(\d+)_(\d\d)(\d\d)(\d\d)_(\w+)", name)
            if m:
                w = parse_hm(f"{m[2]}:{m[3]}:{m[4]}", start_wall.date()) - boot2unix
                if steps[0][0] <= w <= steps[-1][0]:
                    marks.append((w, f"gamerec mark {m[1]} ({m[5]})", None))

    cur_cap = steps[0][1] + sum(p["vram"] for p in steps[0][3])
    for e in a.expect:
        when, rest = e.split(None, 1)
        marks.append((parse_hm(when, start_wall.date()) - boot2unix, None, rest))
    marks = sorted(marks, key=lambda m: m[0])
    out = open(a.out, "w") if a.out else sys.stdout
    w = out.write
    w(f"# replay of gamerec recording {os.path.basename(rec)}, "
      f"{dt.datetime.fromtimestamp(t0 + boot2unix):%H:%M:%S} .. "
      f"{dt.datetime.fromtimestamp(steps[-1][0] + boot2unix):%H:%M:%S}, {len(steps)} policy steps\n")
    w(f"# generated by local/sim/replay.py; cap (free + per-process vidmem) per step, median {cap:.0f} MB\n")
    w(f"cap {cur_cap}\nset autotick 0\nset hints 1\nclock {t0 + boot2unix:.0f}\n")

    known, last_total, last_in = {}, {}, {}

    def declare(p, old):
        if p["pid"] not in known:
            flags = (" old" if old else "") + (" never" if p["never"] else "")
            w(f"proc {p['pid']} {p['comm']}{flags}\n")
            known[p["pid"]] = p["comm"]

    def elig(p):
        return (p["elig"] / p["vram"]) if p["vram"] else 0.0

    for i, (t, avail, trig, procs) in enumerate(steps):
        tracked = {p["pid"] for p in procs}
        while comps and comps[0][0] <= t:
            ct, _, _, c = comps.pop(0)
            # kwin's own allocations are in its footprint already.
            if c["pid"] not in tracked or c["comm"] not in ("kwin_wayland", "Xwayland"):
                w(f"at {max(ct - t0, 0):.3f}\ncomp {c['kb'] / 1024:.0f}  # {c['comm']}({c['pid']})\n")
        while marks and marks[0][0] <= t:
            mt, text, check = marks.pop(0)
            w(f"at {mt - t0:.3f}\n" + (f"mark {text}\n" if text else f"expect {check}\n"))
        w(f"at {t - t0:.3f}\n")
        # The inputs of this step: window hints and GPU activity.
        for p in procs:
            pid = p["pid"]
            declare(p, i == 0)
            st = (p["hint"] + (" inh" if p["inh"] & 1 else "") + (" other" if p["inh"] & 2 else "") +
                  (" full" if p["inh"] & 4 else "") + (" launched" if p["inh"] & 8 else ""))
            idle = "-" if p["idle"] >= 4294967295 else str(p["idle"])
            last = last_in.get(pid, {})
            if last.get("st") != st:
                w(f"hint {pid} {st}\n")
            if (last.get("idle"), last.get("samples")) != (idle, p["samples"]):
                w(f"act {pid} {idle} {p['samples']}\n")
            last_in[pid] = dict(st=st, idle=idle, samples=p["samples"])
            if i == 0:
                w(f"place {pid} {p['vram']} {p['ram']} {elig(p):.2f}\n")
                last_total[pid] = p["vram"] + p["ram"]
        for pid in [x for x in last_in if x not in {p["pid"] for p in procs}]:
            w(f"hint {pid} -\n")
            del last_in[pid]
        w("step\n")
        for p in procs:
            w(f"rec {p['pid']} {p['vram']} {p['ram']}\n")
        # What each process allocates or frees until the next step, spread over
        # it; the driver's own memory (scanout buffers, contexts) as the
        # capacity left for processes at the next step.
        if i + 1 < len(steps):
            t1, avail1, _, nxt = steps[i + 1]
            cap1 = avail1 + sum(p["vram"] for p in nxt)
            if abs(cap1 - cur_cap) >= 8:
                w(f"cap {cap1}\n")
                cur_cap = cap1
            spread = t1 - t
            for p in nxt:
                declare(p, False)
                total = p["vram"] + p["ram"]
                if abs(last_total.get(p["pid"], 0) - total) >= 1:
                    w(f"target {p['pid']} {total} {elig(p):.2f} spread={spread:.3f}\n")
                    last_total[p["pid"]] = total
            for pid in [x for x in last_total if x not in {p["pid"] for p in nxt}]:
                if last_total[pid]:
                    w(f"target {pid} 0 0 spread={spread:.3f}\n")
                del last_total[pid]
    if out is not sys.stdout:
        out.close()
        print(f"{a.out}: {len(steps)} steps, cap {cap:.0f} MB, {len(known)} processes")


if __name__ == "__main__":
    main()
