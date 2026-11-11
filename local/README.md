# Local patches

Personal patches on top of NVIDIA open-gpu-kernel-modules for one desktop
(RTX 3080 Ti, Ubuntu, KDE Plasma on Wayland). Not intended for upstream.

## vidmem user reserve

When a game fills VRAM, kwin can no longer allocate its scanout buffers and the
whole desktop freezes (nvidia-drm only falls back to sysmem for NO_SCANOUT GEM
objects). The patch in `src/nvidia/src/kernel/mem_mgr/video_mem.c` keeps the
bottom `RmVidmemUserReserveMB` of vidmem for the compositor:

- Allocations of ordinary processes get a PMA address range above that block
  and fail with `NV_ERR_NO_MEMORY` once the free memory outside it is used up.
  A block rather than a byte count, because scanout buffers must be physically
  contiguous and a game churning at the limit fragments all free memory.
- The same range is applied in `pmaAllocatePages()` to every allocation made
  from an ordinary process's context, so RM's internal allocations for it (GR
  contexts, page tables) stay out of the block as well.
- `kwin_wayland` / `Xwayland` (by thread or process name) and kernel threads are
  exempt; their contiguous
  allocations stay bottom-up (into the block), their other allocations go
  top-down. Buffers nvkms allocates for a process's GBM/dumb-buffer ioctl count
  against that process (nvidia-drm falls back to sysmem for NO_SCANOUT ones).
- `NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE`, which the userspace driver turns into
  the Vulkan/DXGI memory budget, excludes the block's free part for ordinary
  processes, so games plan their streaming for what they can get. `nvidia-smi`
  run as a user shows that smaller free value too.

Unset or 0 disables all of it. Denials are logged as
`NVRM: ... vidmem user reserve: denied ...` (first 8, then every 1024th).

`/proc/driver/nvidia/gpus/<bdf>/vidmem_pma` (readable by everyone) prints, in
MB: free, largest contiguous free, block size, block free, block largest
contiguous free.

## vidmem census (read-only)

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
sudo ./local/trace/nvtrace.py record -d 60             # or: record -- ./local/nvt-game ... (runs as you)
sudo ./local/trace/nvtrace.py mark "stutter"           # while recording: a marker (bind to a hotkey)
./local/trace/nvtrace.py report local/trace-logs/<dir> # summary.txt + trace.json (open in ui.perfetto.dev)
sudo ./local/trace/nvtrace.py live                     # the events one line each, as they happen
sudo ./local/evict.sh trace-lock 500                   # lock waits/holds traced from 500 us
```

## Recording your own gaming (host)

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

## Real-life scenarios (test VM guest)

`local/scenario/scenario.py run death`: a game in the foreground, Alt+Tab to
Chromium, browse, back, N cycles, with background apps sized so the game is
short of VRAM (`--bg-fill MB`); report per phase: frames on screen, worst
frame and time to steady after each switch, the game's own frame times
(MangoHud), what the driver moved. `gs.py` drives the desktop (windows,
uinput mouse and keyboard, screenshots). Needs the KWin hints script in the
session. See HANDOFF.md "Real-life scenarios".

## Usage

```bash
./local/build.sh                         # build modules + vram-hog
sudo ./local/install.sh enroll-key       # once: MOK key for Secure Boot, then reboot and enroll
sudo ./local/install.sh install          # sign + install into updates/nvidia-local, then reboot
./local/build.sh tools                   # only vram-hog and gbm-hog
./local/vram-hog --hold 20               # fill VRAM; desktop must stay responsive
./local/stress.sh race                   # parallel fills under a watchdog
./local/stress.sh gbm                    # GBM / dumb buffers (nvkms path) under a watchdog
./local/stress.sh scanout                # can the compositor still get contiguous scanout buffers?
./local/stress.sh game                   # start before a game: logs its vidmem/sysmem and vidmem_pma
./local/compr-test --mode hold           # a compressed image with a known pattern, verified via the GPU
NVTEST_FORCE=1 ./local/bench-all.sh      # whole bench under watchdog.sh (killed if no progress for 8 min)
./local/watchdog.sh --stall 300 -- CMD   # any long run: WATCHDOG STALL/TIMEOUT instead of waiting forever
sudo ./local/safety.sh install           # watchdog + panic on lockups (before risky driver work)
sudo ./local/rollback.sh                 # back to stock modules, then reboot
```

No VRAM is reserved by default (the compositor gets room made instead);
`RESERVE_MB=768 sudo ./local/install.sh install` brings the old fixed reserve back.
If the system does not boot to a desktop, pick the previous kernel in GRUB
(it keeps Canonical's modules), then run `rollback.sh`.

## Updating to a new driver

```bash
git fetch upstream tag <new-version> --depth 1
git rebase --onto <new-version> 595.99.02 local/vidmem-desktop-reserve
./local/build.sh && sudo ./local/install.sh install
```
The kernel module version must match the installed `libnvidia-gl` userspace version.

## Test VM (no reboots)

The RTX 3080 Ti goes to a QEMU guest (VFIO), so a build that hangs or panics
only takes the guest down: `./local/vm/vm.sh reset` and it is back in seconds.
The host's desktop runs on the AMD iGPU (one monitor on the motherboard); the
other monitor stays on the 3080 Ti and shows the guest's own KDE Plasma
(Wayland, KWin on the NVIDIA GPU, as on bare metal), so games run there as
usual. The repository is shared into the guest at the same path (virtiofs).

```bash
sudo ./local/vm/setup.sh                 # once: sudoers rule + guest image (CachyOS via pacstrap)
./local/vm/vm.sh start --no-gpu          # optional: check the VM while the host still has the GPU
sudo ./local/vm/mode.sh vm               # host stops loading nvidia; move a monitor, reboot
./local/vm/vm.sh start && ./local/vm/vm.sh wait
./local/vm/vm.sh input on                # keyboard + mouse to the guest; both Ctrl switch back
./local/vm/vm.sh build                   # modules for the guest kernel, built in the guest
./local/vm/vm.sh reload                  # swap the guest's driver (SDDM restarts, autologin)
./local/vm/vm.sh install                 # the tree build becomes what the guest boots
./local/vm/vm.sh log                     # serial log: oopses survive a hung guest
./local/vm/vm.sh reset                   # reboot the guest (--force: hard reset of a dead one, may wedge the GPU until a host reboot)
./local/vm/vm.sh ssh $PWD/local/move-test.sh    # evict.sh move keeps the content (BACKLOG #16)
./local/vm/vm.sh ssh $PWD/local/contig-test.sh  # contiguous buffers in fragmented vidmem: moved inside vidmem
./local/vm/vm.sh ssh $PWD/local/fragchurn.sh    # fragment vidmem like a long session
./local/vm/vm.sh ssh $PWD/local/compact-test.sh # background compaction lengthens the free run
./local/vm/vm.sh ssh $PWD/local/contigloop-test.sh # a focused app remaking a contiguous buffer: no clearing loop (BACKLOG #15)
./local/vm/vm.sh ssh $PWD/local/compact-test.sh 120 4096 fg  # compaction out of the focused window's memory (evict.sh compactfg on|off)
./local/vm/vm.sh ssh $PWD/local/momc-probe.sh   # a probe: do access counters report accesses to vidmem (BACKLOG I5)
./local/vm/vm.sh ssh $PWD/local/fgown-test.sh   # the focused app gets an allocation nobody can make room for (BACKLOG #7)
./local/vm/fulltest.sh                          # all of the above + nvtest all + the Plasma session + a game, one verdict
./local/vm/vmtest.sh [all|focus|...]     # build on the host, reload in the guest, nvtest under watchdogs
sudo ./local/vm/mode.sh host             # back to the 3080 Ti on the host after a reboot
```

`vmtest.sh` is the one-command check of the working tree: it builds the
modules on the host (same kernel as the guest), loads them in the guest and
runs nvtest there with a watchdog in the guest and one on the host (a guest
that goes silent is reset); it ends with `VMTEST PASS|FAIL` and keeps a log
in `local/stress-logs/vmtest-*.log`. If a hard reset leaves the GPU wedged
(the guest hangs in its firmware; with `NVVM_ROMBAR=0 ./local/vm/vm.sh start`
it boots and its driver says "WPR2 already up"), only a host reboot (or a
PCI reset as root) brings the GPU back.

The guest pins its kernel and `nvidia-utils`; the tree must be the same
release as the guest's `nvidia-utils` (`guest.sh` refuses otherwise). Guest
uses CCD0 (3D V-Cache, CPUs 0-7,16-23) and 24 GB; the host is limited to CCD1
while it runs (`NVVM_ISOLATE=0` to skip, `NVVM_MEM=16G` for less memory).
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
