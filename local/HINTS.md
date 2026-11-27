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
| GNOME 45 and later | Shell extension `gnome-vram-hints/` | `./gnome-vram-hints/install.sh`, log in again |
| Hyprland, sway, niri | `vram-hintd/vram-hintd.c` (C, libc only) | `./vram-hintd/install.sh`, plus one line in the compositor's configuration (the script prints it) |
| X11: Xfce, Cinnamon, MATE, LXQt, i3, Openbox, any EWMH window manager | `vram-hintd` (with libxcb, xcb-res, xcb-randr: a build without them leaves X11 out) | `./vram-hintd/install.sh` (XDG autostart) |

All of them apply the same rules (below). The KWin script runs on the
author's desktop; the others pass the scenarios of
`tests/hint-writers-test.sh` on the real compositors run headless, with the
patched driver on a rented RTX 3060 (`--gpu`: the driver's own file, the
real Hyprland 0.57, and on each desktop a hidden window's process giving its
video memory to the focused one and getting it back when shown), but none
has yet run anyone's everyday session. The rest of this page is the protocol, for a writer on yet
another desktop.

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

## The writers

**KWin** (`kwin-vram-hints/main.qml`): the reference; window state straight
from KWin's scripting API, the table written through `XMLHttpRequest`.

**GNOME** (`gnome-vram-hints/extension.js`): runs inside gnome-shell, which
is the compositor. Windows from `global.get_window_actors()` (stacking
order), shown = not minimized, `showing_on_its_workspace()` and
`located_on_workspace()` of the active workspace; the covering test as
above, override-redirect Xwayland windows over a monitor as games. Writes
with a plain open and write (`Gio.File.append_to()`): `replace_contents()`
writes a temporary file and renames it, which `/proc` refuses. Writes an
empty table when disabled and on the shell's `shutdown` signal; stays enabled
on the lock screen, so locking does not bring everything back. The overview
shows no focused window: while it is open nobody is `f`.

**vram-hintd** (`vram-hintd/vram-hintd.c`, `make`): a daemon in C that
asks the compositor over its IPC on every event and every 250 ms (X11: 1 s, events cover more
there and each query costs more round trips), and writes the table when it
changed; picks the compositor from the environment
(`HYPRLAND_INSTANCE_SIGNATURE`, `SWAYSOCK`, `NIRI_SOCKET`, else `DISPLAY`).
On SIGTERM and when the compositor goes away it writes an empty table and
exits. `vram-hintd --once` prints the table it would write; `--debug` logs
each window's visible share.

- *Hyprland*: `j/monitors`, `j/clients`, `j/activewindow` and the event
  socket `.socket2.sock`; formats from 0.4x to 0.57 (workspaces by address,
  else id, else name; fullscreen as the old bool or the new mode, 2 =
  fullscreen, 1 = maximized). Shown = mapped, not `hidden` (a group's other
  tabs), on a monitor's active or shown special workspace. Stacking: tiled,
  floating, fullscreen, special workspace above. `decoration:active_opacity`,
  `inactive_opacity` and `fullscreen_opacity` below 1 make those windows
  translucent (they cover nothing; read every 5 s); per-window opacity rules
  are not in the IPC. From 0.57 the configuration is Lua: the autostart line
  is `hl.on("hyprland.start", function() hl.exec_cmd("~/.local/bin/vram-hintd") end)`
  in `hyprland.lua` (before 0.57: `exec-once = ...` in `hyprland.conf`;
  `install.sh` prints both with the full path).
- *sway*: `GET_TREE` and a subscription to window, workspace and output
  events. sway's own `visible` is already false on other workspaces, in the
  scratchpad, for unseen tabs and behind a fullscreen view; floating views
  get the covering test.
- *niri*: `Outputs`, `Workspaces`, `Windows` and `EventStream`. niri gives
  no position for tiled windows, so a tiled window on an active workspace
  counts as shown even when its column is scrolled out of view: a hidden
  window kept costs only memory not freed, a shown one moved out would make
  the user wait. A tiled window the size of its output that is its
  workspace's active window is fullscreen and covers the other tiled ones;
  floating windows have positions and get the covering test. X11 apps under
  xwayland-satellite are reported by niri with the satellite's pid, not
  their own: they get no hint and count as background processes.
- *X11*: EWMH (`_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`,
  `_NET_CURRENT_DESKTOP`, per window `_NET_WM_PID`, `_NET_WM_DESKTOP`,
  `_NET_WM_STATE`, `_NET_WM_WINDOW_TYPE`, `_NET_WM_WINDOW_OPACITY`), the
  stacking from the X server (the root's children), pids from the
  X-Resource extension when `_NET_WM_PID` is missing. Shown = viewable
  (window managers unmap minimized windows and those of other desktops; the
  unseen tabs of i3 are covered by the shown one). Override-redirect windows over a monitor
  are games, unless they belong to the window manager or compositor (its
  overlay window). Translucency through an ARGB visual alone is not seen.

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
scenarios with hints written by hand, no desktop needed;
`tests/hint-writers-test.sh` runs the writers on headless compositors, no
GPU needed; with `--gpu`, as root on a machine with the driver loaded and no
desktop of its own (the test VM, a rented one), against the driver itself.

## The numbers that follow the card

The focused window's headroom and the marks of the policy without hints are
a share of the card's memory, the values of a 12 GB card in proportion
(`vepScaleToCard()`): on 6 GB the headroom is 1 GB, on 24 GB 4 GB. The
background cushion (256 MB) and the free run compaction keeps (1 GB, less on
cards below 12 GB) are sized by what the monitors need. The kernel log line
`vidmem evict: auto ...` at the first allocation after loading lists them.
Fixed values: `NVreg_RegistryDwords="RmVidmemFgHeadroomMB=1536;RmVidmemBgCushionMB=256"`
(also `RmVidmemEvictLowMB`, `RmVidmemEvictHighMB`) in the module options.
