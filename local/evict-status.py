#!/usr/bin/env python3
"""The driver's `status` of vidmem_evict, laid out for a terminal.

The driver prints plain lines that the tests and gamerec read with grep
(_vidmemEvictStatus() in src/nvidia/src/kernel/mem_mgr/vidmem_evict.c);
`evict.sh status` passes them through this when it prints to a terminal.
A line this does not know is shown as it came, at the end: nothing is lost
when the driver adds one.

    sudo ./evict.sh status                  # laid out on a terminal, plain lines into a pipe
    sudo ./evict.sh status --raw            # plain lines on a terminal too
    sudo ./evict.sh status --pretty | less -R
    sudo ./evict.sh status --watch [SEC]    # refreshed every SEC s (2), with what changed since it began
    ./evict-status.py [--prev FILE] [--rows N] [FILE]
                                            # a saved status (gamerec's evict-status.log); --prev: an
                                            # earlier one, the changes since it are shown; --rows: what
                                            # matters least is left out to fit N lines

Colours follow NO_COLOR and FORCE_COLOR; the width follows the terminal
(COLUMNS).
"""

import os
import re
import shutil
import sys
import textwrap

# ---- colours ---------------------------------------------------------------

USE_COLOR = False


def sgr(code, text):
    return f"\x1b[{code}m{text}\x1b[0m" if USE_COLOR and text else text


def bold(t):   return sgr("1", t)
def dim(t):    return sgr("2", t)
def red(t):    return sgr("31", t)
def green(t):  return sgr("32", t)
def yellow(t): return sgr("33", t)
def blue(t):   return sgr("34", t)
def magenta(t): return sgr("35", t)
def cyan(t):   return sgr("36", t)


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def vlen(s):
    return len(ANSI.sub("", s))


def pad(s, w, right=False):
    gap = " " * max(0, w - vlen(s))
    return gap + s if right else s + gap


# Window states: the bar's colour, the legend's name.
STATES = {
    "fg":      ("1;32", "focus"),
    "visible": ("36",   "visible"),
    "hidden":  ("34",   "hidden"),
    "-":       ("35",   "no window"),
}
DRIVER_CODE = "37"      # in use, held by no process shown (driver, kernel clients)
FREE_CODE = "2"
# Without colours the parts of a bar differ by their character.
PLAIN = {"1;32": "#", "36": "=", "34": "-", "35": "+", DRIVER_CODE: "%", FREE_CODE: "."}


def num(v, unit="", warn=None):
    """A count: dim when zero, `warn` (a colour function) when not."""
    s = f"{v}" + (f" {unit}" if unit else "")
    if v == 0:
        return dim(s)
    return warn(s) if warn else bold(s)


def mb(v, warn=None):
    if v >= 10240:
        s = f"{v / 1024:.1f} GB"
    else:
        s = f"{v} MB"
    if v == 0:
        return dim(s)
    return warn(s) if warn else bold(s)


def delta(cur, old, unit="", paren=True):
    """ (+N) / (-N): the change since the earlier status, nothing when none."""
    if old is None or cur == old:
        return ""
    d = cur - old
    s = f"{d:+d}{unit}"
    s = cyan(s) if d > 0 else blue(s)
    return (" " + dim("(") + s + dim(")")) if paren else s


# ---- parsing ---------------------------------------------------------------

N = r"(\d+)"
LINES = {
    "auto": rf"auto (.*?), evict below {N} MB, restore above {N} MB; free outside reserve {N} MB; "
            rf"in sysmem {N} allocs {N} MB$",
    "born": rf"created in sysmem instead of denied: {N} allocs {N} MB \({N} failed\); busiest pid {N}; "
            rf"window buffers (.*?); quiesce {N}$",
    "noreserve": rf"no reserve: largest contiguous free {N} MB; background cushion {N} MB; "
                 rf"room made for the compositor {N} times \({N} failed\), {N} MB moved, longest {N} us$",
    "compact": rf"windows cleared for contiguous allocations: {N} MB moved inside vidmem; "
               rf"background compaction: keeps a free run of {N} MB, {N} steps, {N} MB moved(; not the window in focus)?; "
               rf"allocations of up to {N} MB move \({N} MB of the window in focus\); "
               rf"with a game in focus a run of {N} MB is enough \(the biggest contiguous request of the last {N} min: {N} MB\)$",
    "saw": rf"compaction saw \(MB\): in use {N}; movable {N}; compositor {N}, foreground {N}, "
           rf"movable with a pause of the compositor {N}, bigger than a step {N}, refused {N}$",
    "frag": rf"fragmentation: free {N} MB of {N}; in whole 2 MB chunks {N} MB in {N} runs "
            rf"\({N} of 32 MB or more, {N} of 128 MB or more, longest {N} MB\); in holes {N} MB "
            rf"in {N} partly used chunks \({N} MB in use there\)$",
    "hot": rf"hot memory: (.*?); notifications {N} \(sysmem {N}, on evicted allocations {N}\), hot allocations restored {N}; "
           rf"probe: outside evicted allocations {N}, type cpu {N}, aperture vidmem {N}$",
    "hints": rf"window hints: (on|off) \((.*?)\); could give up {N} MB to visible apps, {N} MB to the foreground",
    "bounces": rf"bounces: {N} moves undone by the policy within {N} s, {N} MB copied for nothing",
    "sides": r"sides: (on|off); (.*)$",
    "walks": rf"walks: {N} processes walked, {N} taken unchanged from the last walk$",
    "gamegives": r"a visible game for a focused app: (.*)$",
    "reserve": r"reserve block .*$",
}
LINES = {k: re.compile(v) for k, v in LINES.items()}
HEADER = re.compile(r"pid\s+name\s+vram_mb\s+ram_mb\s+evict_mb\s+idle_s\s+samples\s+window\s+flags")
# "%-8u %-16.16s %7llu ...": the name may hold spaces, so it is read by position.
ROW_REST = re.compile(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+(\S+)\s+(\d+)\s+(\S+)\s*(.*)$")


def parse_row(line):
    if len(line) < 26 or not line[:8].strip().isdigit():
        return None
    m = ROW_REST.match(line[25:])
    if not m:
        return None
    vram, ram, ev, idle, samples, win, flags = m.groups()
    return dict(pid=int(line[:8]), name=line[9:25].strip(), vram=int(vram), ram=int(ram), ev=int(ev),
                idle=idle, samples=int(samples), win=win, flags=flags.split())


def ints(m):
    return [int(g) if g is not None and g.isdigit() else g for g in m.groups()]


def parse(lines):
    """One GPU's status: {key: values}, the processes, the lines not known."""
    st, procs, other = {}, [], []
    in_table = False
    for line in lines:
        line = line.rstrip("\n")
        if not line.strip():
            continue
        if HEADER.match(line):
            in_table = True
            continue
        if in_table:
            row = parse_row(line)
            if row:
                procs.append(row)
                continue
        for key, rx in LINES.items():
            m = rx.match(line)
            if m:
                st[key] = ints(m) if key != "reserve" else line
                break
        else:
            other.append(line)
    return st, procs, other


# ---- layout ----------------------------------------------------------------

def bar(parts, width):
    """A stacked bar: parts are (MB, colour code); every non-empty part gets a cell."""
    total = sum(v for v, _ in parts)
    if total <= 0 or width <= 0:
        return ""
    cells = [max(1, round(v * width / total)) if v > 0 else 0 for v, _ in parts]
    # Give back or take the rounding from the biggest part.
    big = max(range(len(cells)), key=lambda i: parts[i][0])
    cells[big] = max(0, cells[big] + width - sum(cells))
    out = []
    for (v, code), n in zip(parts, cells):
        if USE_COLOR:
            out.append(sgr(code, ("░" if code == FREE_CODE else "█") * n))
        else:
            out.append(PLAIN.get(code, "#") * n)
    return "".join(out)


class Out:
    def __init__(self, width):
        self.width = width
        self.lines = []
        self.tags = []          # the section of each line
        self.tag = None
        self.key_w = 16

    def add(self, s=""):
        self.lines.append(s)
        self.tags.append(self.tag)

    def section(self, title, note=""):
        self.tag = title
        if self.lines:
            self.add()
        self.add(bold(cyan(title)) + ("  " + note if note else ""))

    def fit(self, rows):
        """At most `rows` lines: the sections that matter least while watching go first."""
        dropped = []
        for title in ("Driver", "Policy", "Compaction", "Other lines"):
            if len(self.lines) <= rows:
                break
            if title in self.tags:
                keep = [i for i, t in enumerate(self.tags) if t != title]
                self.lines = [self.lines[i] for i in keep]
                self.tags = [self.tags[i] for i in keep]
                dropped.append(title)
        if dropped:
            self.tag = None
            self.add()
            self.add(dim(f"  not shown for the height: {', '.join(dropped)}"))
        return self.lines

    def field(self, key, parts):
        """`key  part · part · part`, wrapped under itself at the width."""
        indent = "  " + " " * self.key_w
        line, first = "  " + pad(dim(key), self.key_w), True
        cur = 0
        sep = dim(" · ")
        for p in parts:
            if p is None:
                continue
            piece = p if cur == 0 else sep + p
            if cur > 0 and vlen(line) + vlen(piece) > self.width:
                self.add(line)
                line, cur, piece = indent, 0, p
            line += piece
            cur += 1
        self.add(line)


def show(st, procs, other, width, prev=None, rows=None):
    o = Out(width)
    frag = st.get("frag")
    auto = st.get("auto")
    pst = prev[0] if prev else {}

    def was(key, i):
        v = pst.get(key)
        return v[i] if v and i < len(v) and isinstance(v[i], int) else None
    total = frag[1] if frag else None
    free = frag[0] if frag else (auto[3] if auto else None)

    # ---- the memory --------------------------------------------------------
    if total:
        used = total - free
        out_mb = auto[5] if auto else 0
        pfrag = pst.get("frag")
        pused = pfrag[1] - pfrag[0] if pfrag else None
        o.section("VRAM", f"{bold(f'{used / 1024:.1f}')} of {total / 1024:.1f} GB in use{delta(used, pused, ' MB')}, "
                  f"{100 * free // total}% free"
                  + (f", {yellow(f'{out_mb} MB')} moved out to system memory{delta(out_mb, was('auto', 5), ' MB')}"
                     if out_mb else ", nothing moved out"))
        by = {k: 0 for k in STATES}
        for p in procs:
            by[p["win"].rstrip("^") if p["win"].rstrip("^") in by else "-"] += p["vram"]
        driver = max(0, used - sum(by.values()))
        parts = [(by[k], STATES[k][0]) for k in STATES] + [(driver, DRIVER_CODE), (free, FREE_CODE)]
        bw = max(10, width - 4)
        o.add("  " + bar(parts, bw))
        legend = [sgr(STATES[k][0], "█") + " " + STATES[k][1] + " " + bold(str(by[k])) for k in STATES if by[k]]
        legend.append(sgr(DRIVER_CODE, "█") + " driver " + bold(str(driver)))
        legend.append(dim("░ free ") + bold(str(free)))
        o.field("by window, MB", legend)
        o.field("total", [f"{bold(str(total))} MB", f"{bold(str(used))} used",
                          f"{bold(str(free))} free ({100 * free // total}%)",
                          f"longest free run {mb(frag[6])}"])
        o.field("free runs", [f"{num(frag[3])} runs of whole 2 MB chunks ({mb(frag[2])})",
                              f"{num(frag[4])} of ≥32 MB", f"{num(frag[5])} of ≥128 MB",
                              f"holes {mb(frag[7])} in {num(frag[8])} chunks ({frag[9]} MB in use there)"])
    else:
        o.section("VRAM")
    if "reserve" in st:
        o.field("reserve", [st["reserve"]])
    if auto:
        o.field("in sysmem", [mb(auto[5], yellow) + delta(auto[5], was("auto", 5))
                              + f" in {num(auto[4], warn=yellow)} allocations",
                              f"free outside reserve {mb(auto[3])}"])
    born = st.get("born")
    if born:
        o.field("born in sysmem", [f"{num(born[0], warn=yellow)}{delta(born[0], was('born', 0))} allocations "
                                   f"({mb(born[1], yellow)}) created there instead of denied",
                                   f"{num(born[2], warn=red)}{delta(born[2], was('born', 2))} failed"])

    # ---- who is moved out, when it comes back ------------------------------
    o.section("Policy")
    if auto:
        mode = auto[0]
        o.field("auto", [bold(green(mode)) if mode != "off" else red(mode),
                         f"moves out below {bold(str(auto[1]))} MB free",
                         f"brings back above {bold(str(auto[2]))} MB"])
    h = st.get("hints")
    if h:
        o.field("window hints", [green("on") if h[0] == "on" else yellow("off"), dim(h[1]),
                                 f"could give {mb(h[2])} to visible apps, {mb(h[3])} to the focus"])
    g = st.get("gamegives")
    s = st.get("sides")
    if g or s:
        parts = []
        if g:
            parts.append(("a visible game for a focused app: " + (green(g[0]) if "inside vidmem" in g[0] else yellow(g[0]))))
        if s:
            parts.append(f"sides {green('on') if s[0] == 'on' else dim('off')}: {s[1]}")
        o.field("game", parts)
    b = st.get("bounces")
    if b:
        o.field("bounces", [f"{num(b[0], warn=red)}{delta(b[0], was('bounces', 0))} moves undone within {b[1]} s",
                            f"{mb(b[2], red)}{delta(b[2], was('bounces', 2), ' MB')} copied for nothing"])
    if born:
        o.field("window buffers", [born[4], f"quiesce {born[5]}", f"busiest pid {bold(str(born[3]))}"])

    # ---- compaction --------------------------------------------------------
    c = st.get("compact")
    nr = st.get("noreserve")
    saw = st.get("saw")
    if c or nr or saw:
        o.section("Compaction")
    if c:
        o.field("keeps free", [f"a run of {mb(c[1])}", f"{mb(c[7])} with a game in focus",
                               f"biggest contiguous request in {c[8]} min: {mb(c[9])}"])
        o.field("done", [f"{num(c[2])}{delta(c[2], was('compact', 2))} steps, "
                         f"{mb(c[3])}{delta(c[3], was('compact', 3), ' MB')} moved",
                         f"windows cleared for contiguous allocations: {mb(c[0])}{delta(c[0], was('compact', 0), ' MB')} moved"]
                + ([yellow("not the window in focus")] if c[4] else []))
        o.field("moves", [f"allocations up to {c[5]} MB", f"{c[6]} MB of the window in focus"])
    if nr:
        o.field("compositor", [f"room made {num(nr[2])}{delta(nr[2], was('noreserve', 2))} times "
                               f"({num(nr[3], warn=red)}{delta(nr[3], was('noreserve', 3))} failed)",
                               f"{mb(nr[4])} moved", f"longest {num(nr[5], 'us')}",
                               f"background cushion {nr[1]} MB"])
    if saw:
        o.field("saw (MB)", [f"in use {num(saw[0])}", f"movable {num(saw[1], warn=green)}",
                             f"compositor {num(saw[2])}", f"focus {num(saw[3])}",
                             f"with a compositor pause {num(saw[4])}", f"bigger than a step {num(saw[5])}",
                             f"refused {num(saw[6], warn=yellow)}"])

    # ---- the rest ----------------------------------------------------------
    hot = st.get("hot")
    w = st.get("walks")
    if hot or w:
        o.section("Driver")
    if hot:
        o.field("hot memory", [dim(hot[0]) if hot[0] == "off" else bold(hot[0])]
                + ([f"notifications {num(hot[1])} (sysmem {hot[2]}, evicted {hot[3]})",
                    f"restored {num(hot[4])}",
                    f"probe: elsewhere {hot[5]}, cpu {hot[6]}, vidmem {hot[7]}"] if hot[0] != "off" or hot[1] else []))
    if w:
        seen = w[0] + w[1]
        o.field("walks", [f"{num(w[0])} processes walked, {num(w[1])} unchanged"
                          + (f" ({100 * w[1] // seen}% from the cache)" if seen else "")])

    # ---- the processes -----------------------------------------------------
    if procs:
        o.section("Processes")
        show_procs(o, procs, width, prev[1] if prev else None)
    if other:
        o.section("Other lines")
        for line in other:
            o.add("  " + line)
    return o.fit(rows) if rows else o.lines


def show_procs(o, procs, width, pprocs=None):
    procs = sorted(procs, key=lambda p: (-(p["vram"] + p["ram"]), p["pid"]))
    before = {p["pid"]: p for p in pprocs} if pprocs is not None else None
    dw = 6 if before is not None else 0         # the change of vram + sysmem since then
    big = max(p["vram"] + p["ram"] for p in procs) or 1
    name_w = min(16, max(4, max(len(p["name"]) for p in procs)))
    fixed = 2 + 7 + 1 + name_w + 1 + 6 + 1 + (dw + 1 if dw else 0) + 6 + 1 + 8 + 1 + 6 + 1 + 8 + 1
    flags_w = max([vlen(" ".join(p["flags"])) for p in procs] + [5])
    bar_w = min(30, width - fixed - min(flags_w, 14))
    if bar_w < 6:
        bar_w = 0
    head = ("  " + pad("pid", 7) + " " + pad("name", name_w) + " " + pad("vram", 6, True) + " "
            + (pad("Δ", dw, True) + " " if dw else "")
            + (pad("", bar_w) + " " if bar_w else "")
            + pad("sysmem", 6, True) + " " + pad("movable", 8, True) + " " + pad("idle", 6, True) + " "
            + pad("window", 8) + " " + "flags")
    o.add(dim(head))
    for p in procs:
        win = p["win"]
        state = win.rstrip("^")
        code = STATES.get(state, STATES["-"])[0]
        cells_v = round(p["vram"] * bar_w / big) if bar_w else 0
        cells_r = round(p["ram"] * bar_w / big) if bar_w else 0
        if p["vram"] and not cells_v:
            cells_v = 1
        if p["ram"] and not cells_r:
            cells_r = 1
        pbar = sgr(code, "█" * cells_v) + sgr("33", "▒" * cells_r)
        pbar = pad(pbar, bar_w)
        if not USE_COLOR:
            pbar = pad("#" * cells_v + "~" * cells_r, bar_w)
        idle = p["idle"]
        if idle == "-":
            idle_s = dim("-")
        else:
            sec = int(idle)
            idle_s = (f"{sec}s" if sec < 120 else f"{sec // 60}m" if sec < 7200 else f"{sec // 3600}h")
            idle_s = green(idle_s) if sec == 0 else (idle_s if sec < 60 else dim(idle_s))
        win_s = (dim(state) if state == "-" else sgr(code, state)) + (dim("^") if win.endswith("^") else "")
        if state == "fg":
            win_s = bold(green("focus")) + (dim("^") if win.endswith("^") else "")
        flags = []
        for f in p["flags"]:
            if f == "busiest":
                flags.append(magenta(f))
            elif f == "never":
                flags.append(dim(f))
            elif f.startswith("bounces="):
                flags.append(red(f))
            else:
                flags.append(f)
        if dw:
            old = before.get(p["pid"])
            if old is None:
                d_s = green("new")
            else:
                d_s = delta(p["vram"] + p["ram"], old["vram"] + old["ram"], paren=False)
        name = p["name"][:name_w]
        name_s = bold(name) if state == "fg" else name
        o.add("  " + pad(str(p["pid"]), 7) + " " + pad(name_s, name_w) + " "
              + pad(mb_cell(p["vram"]), 6, True) + " "
              + (pad(d_s, dw, True) + " " if dw else "")
              + (pbar + " " if bar_w else "")
              + pad(mb_cell(p["ram"], yellow), 6, True) + " "
              + pad(mb_cell(p["ev"]), 8, True) + " " + pad(idle_s, 6, True) + " "
              + pad(win_s, 8) + " " + " ".join(flags))
    tv = sum(p["vram"] for p in procs)
    tr = sum(p["ram"] for p in procs)
    te = sum(p["ev"] for p in procs)
    o.add("  " + pad(dim(f"{len(procs)} processes"), 7 + 1 + name_w) + " " + pad(bold(str(tv)), 6, True) + " "
          + (pad("", dw) + " " if dw else "")
          + (pad("", bar_w) + " " if bar_w else "")
          + pad(mb_cell(tr, yellow), 6, True) + " " + pad(bold(str(te)), 8, True))
    if before is not None:
        now = {p["pid"] for p in procs}
        gone = [p for pid, p in sorted(before.items()) if pid not in now]
        if gone:
            o.field("gone since", [f"{p['name']} {dim(str(p['pid']))} ({p['vram'] + p['ram']} MB)" for p in gone])
    note = ("MB. sysmem: moved out to system memory; movable: what the policy may move out; "
            "^: the window state is its parent's" + ("; Δ: vram + sysmem since the earlier status" if dw else ""))
    for line in textwrap.wrap(note, width - 2):
        o.add(dim("  " + line))


def mb_cell(v, warn=None):
    if v == 0:
        return dim("0")
    return warn(str(v)) if warn else str(v)


# ---- main ------------------------------------------------------------------

def split_blocks(text):
    """Several GPUs: evict.sh puts "== FILE" before each; gamerec's log "== HH:MM:SS"."""
    blocks, cur, title = [], [], None
    for line in text:
        if line.startswith("== "):
            if cur or title is not None:
                blocks.append((title, cur))
            title, cur = line[3:].strip(), []
        else:
            cur.append(line)
    blocks.append((title, cur))
    return blocks


def main():
    global USE_COLOR
    args = sys.argv[1:]
    prev_text, rows = None, None
    while len(args) >= 2 and args[0] in ("--prev", "--rows"):
        if args[0] == "--rows":
            rows = int(args[1])
        else:
            try:
                with open(args[1]) as f:
                    prev_text = f.read().splitlines()
            except OSError:
                pass
        args = args[2:]
    src = open(args[0]) if args and args[0] != "-" else sys.stdin
    text = src.read().splitlines()
    tty = sys.stdout.isatty()
    if os.environ.get("NO_COLOR"):
        USE_COLOR = False
    elif os.environ.get("FORCE_COLOR") or os.environ.get("EVICT_STATUS_COLOR"):
        USE_COLOR = True
    else:
        USE_COLOR = tty
    width = shutil.get_terminal_size((100, 40)).columns
    width = max(60, min(width, 120))

    blocks = split_blocks(text)
    prevs = {}
    if prev_text is not None:
        for title, lines in split_blocks(prev_text):
            pst, pprocs, _ = parse(lines)
            if pst or pprocs:
                prevs[title] = (pst, pprocs)
    out = []
    for title, lines in blocks:
        if not any(l.strip() for l in lines):
            continue
        if title is not None:
            out.append(bold(f"━━ {title} " + "━" * max(0, width - vlen(title) - 4)))
        st, procs, other = parse(lines)
        if not st and not procs:
            out.extend(lines)       # not a status (a command's answer): as it came
        else:
            out.extend(show(st, procs, other, width, prevs.get(title), rows))
        out.append("")
    sys.stdout.write("\n".join(out).rstrip("\n") + "\n")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        pass
