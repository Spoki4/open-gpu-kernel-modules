# VRAM eviction for the NVIDIA open kernel modules

Local patches on top of NVIDIA's open-gpu-kernel-modules for one desktop: an
RTX 3080 Ti (12 GB), two monitors, KDE Plasma on Wayland, CachyOS. Not meant
for upstream. Driver release: see `version.mk` (branch `local/vidmem-<release>`).

## The problem

On Linux the NVIDIA driver never moves video memory out. When a game and the
desktop together want more than the card has, an allocation simply fails:
the compositor cannot make a buffer for a window and the whole desktop
freezes, a browser tab's GPU process dies, a new application cannot create
its GPU context. Windows (WDDM) solves this in the OS: it moves the memory of
windows nobody looks at to system memory and brings it back when needed.
These patches do the same inside the driver.

## What the driver does now

- **Moves a process's vidmem to system memory and back** while the process
  runs (`vidmem_evict.c`): its channels are stopped for the copy, the pages
  are copied by the GPU's copy engine, the GPU mappings are redirected. The
  process keeps working from sysmem, slower; contents are verified by the
  tests on every move.
- **Decides who moves by what the user sees** (`vidmem_evict_policy.c`). A
  KWin script (`kwin-vram-hints/`) tells the driver which processes have the
  window in focus, a visible window, or only hidden ones. The order is: focus
  > the window focused before it > visible > hidden; a fullscreen window that
  keeps the GPU busy is a game. Without the script's hints the policy stays
  off (`evict.sh auto on` runs it on GPU activity alone).
- **Makes room inside an allocation**: when a visible process (or the
  compositor, or a new GPU context) finds no memory, hidden processes are
  moved out first and the allocation is retried instead of failing. The
  focused window may take what it needs from a visible game; a focused app
  that is not a game gets its allocation even when nobody can make room, and
  as much of its own older memory goes to sysmem.
- **Brings memory back** when its window is shown again and there is room,
  the focused window first; nothing returns to others while the focus still
  has memory in sysmem.
- **Fights fragmentation.** Scanout and some browser buffers must be
  physically contiguous, and a long session leaves gigabytes free in pieces
  of a few MB. A contiguous request gets a window cleared for it by moving
  other memory to other vidmem pages (no sysmem), and a background step
  compacts vidmem a little at a time so the longest free run stays long. A
  step moves whole allocations: of the window in focus those up to 96 MB (a
  pause of about 9 ms), of the others up to 256 MB, and 256 MB in all (96 MB
  while a game is in focus).
- **Does not keep a fixed reserve** for the compositor any more (the first
  approach, `RmVidmemUserReserveMB`, still there and off by default).
- Fixes found on the way in NVIDIA's own code: a channel group leaked on GSP
  after a failed creation (new GPU apps could not start until reboot), slow
  copies of memory on 64 KB pages, new contexts failing in fragmented vidmem.

Everything is switchable at run time, see `evict.sh` below.

## Layout

| where | what |
|---|---|
| `src/nvidia/src/kernel/mem_mgr/vidmem_evict.c` | moving memory, make-room, windows for contiguous requests, compaction, procfs commands |
| `src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c`, `.h` | the policy: who moves, when it comes back; also built by the simulator |
| `src/nvidia/src/kernel/mem_mgr/video_mem.c` | hooks in the allocation path, the optional reserve |
| `kernel-open/nvidia/nv-procfs.c`, `nv-vidmem-trace.[ch]` | `/proc/driver/nvidia/...` files, tracepoints |
| `local/evict.sh`, `census.sh` | control and inspection from a shell |
| `local/kwin-vram-hints/` | the KWin script that reports window state |
| `local/sim/` | policy simulator, no GPU needed |
| `local/tests/` | test programs and scenarios for a real GPU |
| `local/vm/` | a QEMU guest that owns the GPU, to test builds without rebooting the host |
| `local/trace/`, `gamerec/`, `gamerec.sh` | tracing and session recording |
| `local/bench/`, `scenario/` | game benchmarks and scripted desktop sessions (Linux and a Windows reference guest) |
| `local/cloud/` | setting up a rented GPU machine (Ubuntu) for the tests |

`git diff <release tag> -- . ':!local'` shows every change to NVIDIA's code
(about 10 thousand lines, 7 thousand of them in the three `vidmem_evict*` files).

## Build and install (Arch / CachyOS)

The modules must be the same release as the installed `nvidia-utils`.

```bash
./local/build.sh                         # modules for the running kernel + the test tools
sudo ./local/install.sh                  # they load from the next boot; then reboot
./local/kwin-vram-hints/install.sh       # the KWin script, per user; then log in again
sudo ./local/install.sh stock            # back to the stock modules; then reboot
./local/install.sh status                # which build runs, which one boots
```

`RESERVE_MB=768 sudo ./local/install.sh` brings the old fixed reserve back.

## Control: evict.sh

```bash
sudo ./local/evict.sh status             # the policy's state, fragmentation, per process: vram, ram, window state
sudo ./local/evict.sh dry chrome         # what of a process could move, and why the rest cannot
sudo ./local/evict.sh evict|restore|move <pid|chrome|name>   # by hand: to sysmem, back, to other vidmem pages
sudo ./local/evict.sh auto on|off|hints  # the automatic policy (hints, the default: only with window hints)
sudo ./local/evict.sh compact MB         # background compaction keeps a free run of MB (0: off)
sudo ./local/evict.sh compactfg on|off   # ... also out of the focused window's memory
sudo ./local/evict.sh compactbig on|off  # ... in allocations of up to 256 MB (96 MB of the focus); off: 32 MB
sudo ./local/evict.sh moveshared on|off  # window buffers shared with the compositor may move inside vidmem
sudo ./local/evict.sh fgown on|off       # the focused app pays for its allocation with its own older memory
```

The full list is in the script's header. If the desktop stutters, switch off
`moveshared`, then `compactbig`, then `compactfg`, and look at `status`.

Per GPU there are also `/proc/driver/nvidia/gpus/<bdf>/vidmem_pma` (free
memory, longest free run; readable by everyone) and the kernel log lines
`NVRM: vidmem evict: ...` for every decision the policy makes.

## Census: what a process holds

`/proc/driver/nvidia/gpus/<bdf>/vidmem_census` (root only): write a PID, then
read the list of that process's vidmem allocations with kind, dup count,
CPU/GPU mapping counts and page size, plus a summary that sorts them into
`candidate` (private, uncompressed, not CPU-mapped: what an eviction patch
could move to sysmem), `compressed`, `cpu-mapped` and `shared`, and totals the
process's GPU-visible system memory (`sysmem`).
Code: `rm_vidmem_census` in `src/nvidia/arch/nvalloc/unix/src/osapi.c`, procfs
glue in `kernel-open/nvidia/nv-procfs.c`. RM-internal buffers (GR context,
page tables) are not listed.

```bash
sudo ./local/census.sh chrome --summary  # Chrome's GPU process, totals only
sudo ./local/census.sh 1234              # any PID, full list
```

## Tracing: why does it stutter?

The driver emits tracepoints (system `nvidia_vidmem`, free while off;
`kernel-open/nvidia/nv-vidmem-trace.h`):

| event | what | who waited |
|---|---|---|
| `vidmem_move` | one evict/restore batch: why, MB, phases (quiesce, sysmem alloc, copy, remap, pma) | the process moved, and whoever waits for the RM locks |
| `vidmem_pause` | a process's channels were off: its GPU work stood still for `paused_us` | that process |
| `vidmem_makeroom` | an allocation of a visible process moved others out first | the allocating thread (`us`) |
| `vidmem_reserve` | the reserve turned an allocation away: denied, room made, or created in sysmem | the allocating process |
| `vidmem_tick` | one policy step (API + GPU lock held throughout), time queued for the lock | every RM call meanwhile |
| `vidmem_proc` | per process at each step: vram, sysmem, eligible, window state, idle | - |
| `vidmem_hint` | a window state change from the KWin script | - |
| `vidmem_frag` | how fragmented free vidmem is: free in whole 2 MB chunks and in holes, longest free run, runs of 32/128 MB or more; every ~10 s (`at=tick`) and after each move batch with the longest run before it (`evict.sh status` prints the same line) | - |
| `rm_lock` | an RM API/GPU lock wait or hold over `trace-lock` us (default 1000), where it was taken (symbol), who held it | the waiting thread |

`local/trace/nvtrace.py` records them together with Xids (`nvidia:nvidia_dev_xid`),
the compositor's page flips (`drm:drm_vblank_event_delivered`: a gap in
continuous motion = a dropped frame), the kernel log, and samples of VRAM
(`vidmem_pma`), GPU load, PCIe traffic and per-process SM load (NVML), all on
CLOCK_MONOTONIC. The report lists the longest frame gaps with the driver
events that overlapped them, and writes a timeline for ui.perfetto.dev.

```bash
sudo ./local/trace/nvtrace.py record -d 60             # or: record -- ./local/tests/nvt-game ... (runs as you)
sudo ./local/trace/nvtrace.py mark "stutter"           # while recording: a marker (bind to a hotkey)
./local/trace/nvtrace.py report local/trace-logs/<dir> # summary.txt + trace.json (open in ui.perfetto.dev)
sudo ./local/trace/nvtrace.py live                     # the events one line each, as they happen
sudo ./local/evict.sh trace-lock 500                   # lock waits/holds traced from 500 us
```

## Recording a gaming session

`local/gamerec.sh` records a gaming session: everything above (nvtrace for
the whole session), the system journal, RAM/swap/PSI, free VRAM, power
profile and V-Cache mode every second, the KWin hints, `evict.sh status`
every 10 s, MangoHud frame times, and a Perfetto flight recorder that always
holds the last minute (scheduler, page flips, fences, driver events). A mark
(hotkey Meta+Ctrl+M, or `gamerec.sh mark "text"`) saves that minute plus a
screenshot, evict status, nvidia-smi and ps; journal lines that look like
trouble (Xid, failed channel allocs, eviction failures, coredumps, OOM kills)
make a mark on their own. A summary at the stop.

The root part is a service (`gamerec.service`, a root-owned copy in
/usr/local/lib/gamerec, idle until asked); `~/.local/bin/game-mode-hook`
starts a recording when GameMode switches on and stops it when it goes off.
Without the service, `record` runs the root part through sudo.

```bash
sudo ./local/gamerec.sh install         # once, and again after changing gamerec.py
./local/gamerec.sh hotkey               # once: KDE shortcuts Meta+Ctrl+M (mark), Meta+Ctrl+H (HUD)
./local/gamerec.sh steam                # restart Steam so Vulkan games log frame times
./local/gamerec.sh record               # by hand (without GameMode); Ctrl-C to stop
./local/gamerec.sh mark "stutter after alt-tab"   # or the hotkey
./local/gamerec.sh status               # service, running recording
./local/gamerec.sh report               # summary of the last recording
```

Output: `local/game-logs/<date_time>/` (frame times, screenshots, summary)
with `system/` -> `/var/lib/gamerec/<date_time>/` (root-owned, readable by
you: everything else). Frame times need MangoHud in the game: `gamerec.sh
steam` (Steam with `MANGOHUD=1`, Vulkan games: Proton/DXVK/VKD3D), or per game
in the launch options `MANGOHUD_CONFIGFILE=~/.config/MangoHud/gamerec.conf
mangohud %command%` (also OpenGL). Bottles games (WoW through Battle.net in
the `games` bottle): switch on MangoHud in the bottle's settings once; gamerec
writes the sandbox's own MangoHud config and moves its logs into the recording
at the stop. Auto start needs the game to switch GameMode on (Steam:
`gamemoderun %command%`; Bottles: the bottle's GameMode switch). The overlay
is invisible unless `record --hud`; Meta+Ctrl+H (`gamerec.sh hud`) shows /
hides it in games that are already running (MangoHud reloads its config file;
the frame log goes on in a new file). Disk: nvtrace ~100 MB per hour, a mark
~20-100 MB.

## Policy simulator (no GPU)

`local/sim/` runs the eviction policy (the driver's own
`vidmem_evict_policy.c`) against a memory model: on recorded sessions and on
hand-written scenarios, in well under a second. Details in
`local/sim/README.md`.

```
cd local/sim && ./run-tests.sh                 # all scenarios, known bugs marked
./replay.py 20261106_112014 -o /tmp/cod.scn    # a gamerec recording -> scenario
./sim -cmp 1684253 /tmp/cod.scn                # recorded vs simulated for one process
./sim -v /tmp/cod.scn | less                   # every move and why
```

## Tests on a GPU

`local/tests/` (run them where the test driver is loaded: the test VM guest
or a spare machine, not the desktop you work on). Each prints PASS/FAIL and
checks the kernel log for Xids.

```bash
cd local/tests
./nvtest.sh all                # 15 scenarios: content after evict/restore (idle, rendering, Chrome WebGL/WebGPU),
                               # window hints, focus priority, a "game" filling VRAM, the compositor and new
                               # contexts with vidmem full; nvtest.sh's header lists them
./move-test.sh                 # moves inside vidmem keep the content
./contig-test.sh               # contiguous buffers in fragmented vidmem: a window is cleared
./contigloop-test.sh           # a focused app remaking a contiguous buffer: no clearing loop
./compact-test.sh [120 4096 fg]  # background compaction lengthens the free run (fg: out of the focused window too;
                                 # CHUNK_MB=64|128: with blocks the size a game holds)
./fgown-test.sh                # the focused app gets an allocation nobody can make room for
./fragchurn.sh                 # fragments vidmem like a long session
./momc-probe.sh                # a probe: do the GPU's access counters report accesses to vidmem?
../watchdog.sh --stall 300 -- CMD   # any long run: WATCHDOG STALL/TIMEOUT instead of waiting forever
```

`nvt-game` is a synthetic workload that fills vidmem with a pattern and
checks it on the GPU every frame (also run under other names as background
apps); `gbm-hog` allocates window buffers the way a compositor does;
`compr-test` holds a compressible image; `nvt-webgl.html` / `nvt-webgpu.html`
check their own pixels in a real Chrome on a headless KWin (`nvt-session.sh`).

## Test VM (no host reboots)

The RTX 3080 Ti goes to a QEMU guest (VFIO), so a build that hangs or panics
only takes the guest down: `./local/vm/vm.sh reset` and it is back in seconds.
The host's desktop runs on the integrated GPU (one monitor on the
motherboard); the other monitor stays on the 3080 Ti and shows the guest's
own KDE Plasma, so games run there as on bare metal. The repository is shared
into the guest at the same path (virtiofs). `vm.sh` carries this PC's device
names (keyboard, mouse, PCI addresses, CPU sets): adjust them for another one.

```bash
sudo ./local/vm/setup.sh                 # once: sudoers rule + guest image (CachyOS via pacstrap)
sudo ./local/vm/mode.sh vm               # the host stops loading nvidia; move a monitor, reboot
./local/vm/vm.sh start && ./local/vm/vm.sh wait
./local/vm/vm.sh input on                # keyboard + mouse to the guest; both Ctrl switch back
./local/vm/vm.sh build                   # modules for the guest kernel, built in the guest
./local/vm/vm.sh reload                  # swap the guest's driver without a reboot (~4 s)
./local/vm/vm.sh ssh CMD                 # anything in the guest
./local/vm/vm.sh log                     # serial log: oopses survive a hung guest
./local/vm/vm.sh reset                   # reboot the guest (--force: hard reset of a dead one)
./local/vm/vmtest.sh [all|focus|...]     # build, reload in the guest, nvtest under two watchdogs: VMTEST PASS|FAIL
./local/vm/fulltest.sh                   # every test above + the Plasma session + a game from Steam: one table
sudo ./local/vm/mode.sh host             # the 3080 Ti back to the host after a reboot
```

A hard reset (`reset --force`) while the guest's driver is running can leave
the GPU wedged (GSP stays up through the reset: the guest then hangs in its
firmware); only a host power cycle clears it. So a guest is hard-reset only
when it does not answer at all, and the test drivers first stop the test's
processes over a new ssh session.

Timings differ a little from bare metal (IOMMU, nested paging): confirm final
numbers without the VM.

### Windows reference guest

The same GPU under Windows, for comparisons (never together with the Linux guest):

```bash
local/vm/vm.sh stop && local/vm/vm.sh win start && local/vm/vm.sh win wait
python3 local/bench/winbench.py run superposition --label win-clean
python3 local/bench/winbench.py run wukong --label win-wk-clean
python3 local/bench/winbench.py run wukong --label win-wk-apps --apps --bg-fill 1500   # Edge + background VRAM
local/vm/vm.sh win stop && local/vm/vm.sh start
```
`local/vm/vm.sh win input on` hands keyboard and mouse to Windows (both Ctrl switch back).

## A rented GPU machine

`local/cloud/setup.sh` and `setup-desktop.sh` prepare an Ubuntu machine with
a passed-through NVIDIA GPU (build dependencies, the modules, NVIDIA's
userspace of the same release, a headless KWin and Chrome) so that
`tests/nvtest.sh` and the memory tests run there over ssh.

## Results so far

The simulator replays recorded sessions with the old and the new policy:

| recording | what | before -> after |
|---|---|---|
| CoD, 1 h 40 min | the game's memory in sysmem while it is focused | 371k -> 59k MB*s (-84 %) |
| | moves / volume | 466 / 76 GB -> 96 / 27 GB |
| | out-and-back within 4 s (all processes) | 332 -> 20 |
| | the game waited inside its own allocations | 1372 -> 91 ms |
| WoW, 2.5 h | the game waited inside its own allocations | 227 -> 0 ms |

On the GPU (2026-11-11): the six memory tests pass on a rented Quadro RTX
5000 and `nvtest all` gives 13 of 15 there (`active` stalls on that machine
with any build; `fgprio` see idea I15 below). On the 3080 Ti the build before
compaction ran two hours under a game with no Xid; the current one was
installed on the host on 2026-11-11.

The first evening on the host (2026-11-11, five hours of WoW with Chrome on
the second monitor): no Xid, no crash, 68 moves out and 31 back without a
failure, single moves of 1-36 ms. Compaction stood still next to the game
(problem #17), and Chrome was denied 8 of its 17 contiguous buffers.

Not verified yet: `vm/fulltest.sh` on the 3080 Ti (its `desktop` and `game`
stages have never run), moving buffers shared with the compositor under a
real Plasma session, the access-counter probe on Ampere. The fix for problem
#17 has run in the simulator only (`sim/scenarios/compact-game.scn`); on a
GPU the stages `compactbig` and `compactbigfg` of `fulltest.sh` are for it.

## Numbers cited in the code

Comments and tests refer to the problems that led to a piece of code.

| # | problem | seen as |
|---|---|---|
| 1 | a policy step held the RM locks ~20 ms every 2 s | WebGPU micro-stutter |
| 2 | the recorder read the driver's status every 10 s and stalled it | stutter in every recording |
| 3 | channel group leaked on GSP after a failed creation | plasmashell crash; no new GPU apps until reboot |
| 4 | "can't update VA space" after an evict and restore in one step | errors in the kernel log |
| 5 | slow copies on eviction (0.4 GB/s instead of ~10) | a game frozen for 0.9-1.6 s |
| 6 | out and back in circles next to a game | 30-40 ms hitches |
| 7 | the focused app is refused while a background game holds gigabytes | a WebGPU page died next to a game |
| 8 | make-room tried on every small allocation | 4-11 ms under the locks |
| 9 | an app moved out and back in the same step | hundreds of useless moves |
| 10 | the focused game's memory stuck in sysmem for minutes | low fps until settings were lowered |
| 11 | a failed restore first moved others out | useless moves, log noise |
| 12 | room for the compositor's 32 MB took 284 MB from the game | a 0.5 s freeze |
| 13 | new contexts failed in fragmented vidmem | xdg-desktop-portal crash |
| 15 | Chrome's contiguous buffers: windows cleared at the game's cost, in a loop | freezes up to 1.5 s, 75 GB of copies in an evening |
| 16 | fragmentation: 7 GB free, the longest piece under 1 GB | all of the above, later in a session |
| 17 | compaction moved 32 MB allocations, a game holds blocks of 64-256 MB | 107 idle steps in five hours, Chrome denied contiguous buffers with 1-2 GB free |

Rules (R): R1 focus > previously focused > visible > hidden, and nothing
returns to visible windows while the focus lacks memory; R2 the focus may
take from a visible game, only what it needs; R3 victims by how long ago
they were focused; R4 "game" is a separate flag (fullscreen and busy, or
started by Steam/GameMode), not the same as focus; R5 when nothing else
helps, the game matters more than the desktop app.

Ideas (I) cited: I5 use the GPU's access counters to move cold memory first
(probe: `tests/momc-probe.sh`); I7 shrink the reported budget of background
apps while a game is focused; I10 buffers shared between applications; I11
suspend with evicted memory (not verified); I12 scanout allocations made by
nvkms; I15 the last 6-12 MB of the focused window stay in sysmem when vidmem
is 100 % committed.

## Moving to a new driver release

```bash
git fetch upstream tag <new> --no-tags
git checkout -b local/vidmem-<new> local/vidmem-<old>
git rebase --onto <new> <old>            # <old>, <new>: NVIDIA's release tags
./local/build.sh && (cd local/sim && ./run-tests.sh)
```

then the GPU tests (`vm/vmtest.sh all`) before installing. NVIDIA publishes
one commit per release, so the rebase replays the local commits on the new
tree; conflicts so far were only in files both sides touch lightly
(`os-interface.c`, `osapi.c`, `regmap.c`, `mem.c`).
