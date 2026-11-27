# Window hints: running the policy on another desktop

The driver decides whose video memory moves to system memory by what the
user sees: the focused window keeps its memory and a headroom, the window
focused before it and visible ones come next, hidden ones give first. The
driver cannot see windows; the compositor can, and a hint writer reports
them. Without one, with the default `auto` mode (`hints`), the policy stays
off: the driver works like the stock one, apart from the allocation fixes
that do not depend on the policy.

| desktop | writer | install |
|---|---|---|
| KDE Plasma 6 | KWin script `kwin-vram-hints/` | `./kwin-vram-hints/install.sh`, log in again |

Only KDE has one so far. The rest of this page is the protocol, for a writer
on another desktop.

## The file

`/proc/driver/nvidia/vidmem_hints`, one line per process:

```
<pid> <state> [g]
```

| field | meaning |
|---|---|
| `pid` | the process that owns the window, as the host sees it (the PID namespace of `init`, not a sandbox's) |
| `f` | it owns the window in focus, and that window shows |
| `v` | it has a window the user can see (any monitor) |
| `h` | it has windows, and none of them shows: minimized, on another workspace or activity, or covered by opaque windows above (less than 2 % of it left) |
| `g` | optional second word: one of its shown windows is fullscreen |

- **A snapshot, not a diff.** What one `open()` of the file writes replaces
  the whole table; several `write()` calls of the same `open()` are joined
  (a shell's `printf` writes line by line). Write every window owner every
  time. At most 512 lines and 16 KB.
- **One state per process**: the highest of its windows' (`f` > `v` > `h`).
  Leave out processes without windows and the compositor itself.
- **Children inherit.** A process without a line takes the state of its
  nearest ancestor with one (up to 5 levels up): the GPU process of Chrome,
  Electron and CEF apps, a game started by a launcher. There is no need to
  find the process that holds the memory.
- **Only your own processes**: a line is taken only if the PID belongs to the
  writer's user (or the writer is root). The file is writable by everyone.
- **Off**: a write without a single valid line (`echo >` the file) turns the
  hints off. Do this when the writer stops (logout, the extension disabled,
  the compositor exiting): a stale table is worse than none, because the
  driver keeps acting on windows nobody reports. With the hints off the
  driver brings back what it had moved out, as room allows, and moves
  nothing more. `: >` the file writes nothing and changes nothing.
- **Reading** it back (`cat`) shows the table, its generation and how many
  writes there were.

```bash
printf '%s f\n%s v\n%s h g\n' 1234 2345 3456 > /proc/driver/nvidia/vidmem_hints
cat /proc/driver/nvidia/vidmem_hints
echo > /proc/driver/nvidia/vidmem_hints        # hints off
```

## When to write

- At once on a change of focus, workspace, activity or monitor
  configuration, and when a window is added, removed, minimized or restored.
  The driver reacts in a work item right after the write (makes room for the
  focus, brings back what became visible); the writer never waits for it.
- Covering changes with every move and resize. The KWin script also polls
  every 250 ms and writes only when the table differs from the last one. Do
  not write unchanged tables: every write costs the driver a walk over all
  clients under its locks.

## How the states are decided (as the KWin script does)

1. Per window: *shown* = mapped, not minimized, on the current workspace and
   activity of its monitor. A shown window's visible share is its area minus
   the opaque windows above it (normal or fullscreen windows with opacity 1;
   menus, tooltips, notifications and translucent windows cover nothing).
   Below 2 % it counts as hidden. Round geometry to whole pixels first:
   fractional scaling leaves slivers.
2. `f`: the active window, if it shows. If it does not (a fullscreen game on
   top took the screen without taking the focus, as an override-redirect X11
   window does), the topmost shown fullscreen window is the focus (an
   unmanaged window covering at least 90 % of a monitor counts as one).
3. `g`: the process has a shown window that is fullscreen, or an unmanaged
   (override-redirect) window covering 90 % of a monitor.

## What the driver does with them

| state | its memory |
|---|---|
| `f` | moves out only for the compositor, and last; a headroom is kept free for it (the card's memory / 6: 2 GB on 12 GB) and others move out to refill it; its own allocations get room made before they would fail |
| the window focused before the current one | kept for the headroom and for the background, so alt-tab and back costs nothing; gives what the focus needs right now (an allocation, its own memory coming back) |
| `v` | for the focus's headroom only after 10 s idle; for what the focus needs right now, yes (a game not for a focused app that is not one); comes back as soon as there is room |
| `h` | moves out first, for anybody, unless it keeps drawing while hidden; its new allocations below a 256 MB cushion are created in sysmem |
| no line, no ancestor with one | a background process: after 10 s idle under pressure, after 4 s for the focus |

A **game** is a process with `g` that keeps the GPU busy and has run for 5
s, or any process started by Steam (an ancestor named `reaper`, every Steam
game, native or Proton). A game in focus gets its headroom refilled in small
pieces so its own calls are not held up, and a game hidden less than 30 s ago
is kept like the previous focus.

The desktop's own processes never move. They are found by name, not by
hints (the kernel's process name, at most 15 characters):

- compositors, which also never wait for room and may keep evicted buffers
  shared (`_vepCompositors` in `src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c`):
  kwin_wayland, gnome-shell, mutter, Hyprland, sway, wayfire, labwc, niri,
  cosmic-comp, river, weston, gamescope, Xwayland, Xorg and the X11
  compositing managers kwin_x11, xfwm4, picom, compiz, marco, cinnamon,
  budgie-wm;
- shells, panels and login screens (`_vidmemEvictNeverNames`, same file):
  plasmashell, quickshell, ags, cosmic-panel, budgie-panel, lxqt-panel,
  xfce4-panel, mate-panel, sddm-greeter(-qt), lightdm-gtk-greeter,
  slick-greeter, regreet, gtkgreet.

A compositor or shell not in these lists is an ordinary app to the driver:
add its name there and rebuild. Check the name with `ps -o comm= -p <pid>`.

## The writer

**KWin** (`kwin-vram-hints/main.qml`): the reference; window state straight
from KWin's scripting API, the table written through `XMLHttpRequest`.

**Another compositor** needs a window list with the owners' pids. The
Wayland protocols that list toplevels to clients (wlr foreign toplevel,
ext-foreign-toplevel-list) carry no pid, so wayfire, labwc, river or COSMIC
need a plugin or IPC of their own (wayfire's IPC plugin, for one), or a
patch to the compositor. A writer inside a Flatpak or container sees other
pids than the host and must run on the host.

## Checking it

```bash
cat /proc/driver/nvidia/vidmem_hints               # the table the driver has
sudo ./local/evict.sh status                       # per process: vram, ram, window state as the driver sees it (inherited ones too)
journalctl -k -f | grep 'vidmem evict'             # what the policy does, and for whom
```

`status` should show `auto with window hints only` and, for the window in
focus, `f`. Then switch windows and watch hidden apps go out under pressure
and come back when shown. `tests/nvtest.sh` (see the README) runs the policy's
scenarios with hints written by hand, no desktop needed.

## The numbers that follow the card

The focused window's headroom and the marks of the policy without hints are
a share of the card's memory, the values of a 12 GB card in proportion
(`vepScaleToCard()`): on 6 GB the headroom is 1 GB, on 24 GB 4 GB. The
background cushion (256 MB) and the free run compaction keeps (1 GB, less on
cards below 12 GB) are sized by what the monitors need. The kernel log line
`vidmem evict: auto ...` at the first allocation after loading lists them.
Fixed values: `NVreg_RegistryDwords="RmVidmemFgHeadroomMB=1536;RmVidmemBgCushionMB=256"`
(also `RmVidmemEvictLowMB`, `RmVidmemEvictHighMB`) in the module options.
