# Handoff: VRAM eviction project

Context for a new Claude Code session that continues this work from another
machine (MacBook) over SSH. Written 2026-11-01 at the end of the first session.
The user writes in Russian; answer in Russian.

## Test VM — the current way to test (CachyOS host, set up 2026-11-04)

Read this first. The PC now boots CachyOS (kernel 7.3.1-1-cachyos, stock
`linux-cachyos-nvidia-open` + `nvidia-utils` 615.78.08, Limine with Secure
Boot via sbctl). Ubuntu is still in the Limine menu; the sections further down
were written there (`install.sh`, `nvtest-root.sh`, `build.sh` tools and the
MOK signing are Ubuntu-specific: dpkg, `/usr/lib/x86_64-linux-gnu`). On
CachyOS, driver builds are tested in a QEMU guest that owns the RTX 3080 Ti
(VFIO), so a hang or panic costs a ~11 s guest reset instead of a host reboot.
User docs: "Test VM" in `local/README.md`; scripts in `local/vm/`, each with
a usage header.

Since 2026-11-07 23:15 the host is in host mode and runs the patched driver
itself (build d294cce4 in updates/nvidia-local via `sudo local/vm/guest.sh
install` on the host, same kernel as the guest, no reserve; the KWin hints
script and gamerec reinstalled; before that 805435d3 from 2026-11-04): the test VM needs `sudo local/vm/mode.sh vm`
and a reboot again before it can have the card. `sudo local/vm/guest.sh
install stock` on the host goes back to the stock modules.

Layout (vm mode):
- Host in "vm mode" (`local/vm/mode.sh status`): the host never loads nvidia,
  `vfio-pci` owns 01:00.0/01:00.1, the host desktop runs on the AMD iGPU (one
  monitor on the motherboard, HDMI-A-2). The other monitor stays on the 3080 Ti
  (DP) and shows the guest's own Plasma. Switching modes (`mode.sh vm|host`)
  needs sudo and a reboot: the user does both.
- Guest: CachyOS image `/var/lib/nvidia-vm/guest.img` (built by `mkimage.sh`
  via pacstrap), hostname `nvvm`, user `spoki4` (uid 1000, no password,
  NOPASSWD sudo), Plasma Wayland with SDDM autologin, Steam, Chromium (not
  google-chrome: `nvt-session.sh` calls `google-chrome`, untested in the guest).
  Kernel, headers, `linux-cachyos-nvidia-open` and `nvidia-utils` are pinned
  (`IgnorePkg`) at 7.3.3-1 / 620.70.06 (since 2026-11-10; before: 7.3.1 /
  615.78.08, the old pacman.conf is /etc/pacman.conf.pin-615); update them
  together on purpose only.
- The repo is shared into the guest at the same path (virtiofs), so edit on the
  host and build in the guest. QEMU runs as root in `nvvm.service` (transient,
  `nvvm.slice`), virtiofsd in `nvvm-virtiofsd.service`.
- Guest gets CCD0 (CPUs 0-7,16-23, the V-Cache CCD) and 24 GB (pinned by
  VFIO); while it runs the host is limited to CCD1 (`AllowedCPUs` on
  user/system slice, restored on stop). The user's other VM `vsos-winbuild`
  uses 16 GB and port 2222, hence guest ssh on 127.0.0.1:2230.

How an agent drives it (no password needed: `/etc/sudoers.d/nvidia-local-vm`
lets spoki4 run `local/vm/vm.sh` as root with SETENV; the script re-runs itself
with `sudo -n` for root subcommands):
```bash
local/vm/vm.sh status                # running?, 3080 Ti driver, guest's driver
local/vm/vm.sh start && local/vm/vm.sh wait
local/vm/vm.sh build                 # make modules in the guest, for the guest kernel
local/vm/vm.sh reload [tree|installed|stock]   # ~4 s; SDDM restarts and autologs in
local/vm/vm.sh install [stock]       # what the guest boots (updates/nvidia-local + depmod override)
local/vm/vm.sh ssh 'CMD'             # anything in the guest; `sudo` works without a password
local/vm/vm.sh log                   # serial log, /var/lib/nvidia-vm/serial.log (oopses survive a hang)
local/vm/vm.sh reset                 # hung guest: back in ~11 s, the GPU resets fine
local/vm/vm.sh console               # root shell on the guest's serial port
local/vm/vm.sh qmp '{"execute":...}' # raw QMP; gdbstub on 127.0.0.1:1234
local/vm/vm.sh stop
```
`guest.sh` refuses a tree build whose version differs from the guest's
`nvidia-utils` or whose vermagic is not the guest kernel. Everything else that
needs root (`setup.sh`, `mkimage.sh`, `mode.sh`) the user runs in a terminal
(Claude: open a Terminal panel tab with the command, the user types the
password there).

Rules and pitfalls:
- Long runs only under `local/watchdog.sh` (user's rule, 2026-11-04, after a
  bench sat hung for 30 min while the agent waited for "done"): in the guest
  `./watchdog.sh --stall 420 --watch stress-logs --ignore dmon.log --on-kill
  "./nvt-session.sh stop" -- ./nvtest.sh ...` (or `bench-all.sh`, which does
  that), and on the host around `vm.sh ssh` too (a hung guest goes silent).
  Monitors must report "no progress" and every failure, not only the end.
  dmon.log keeps growing while a test hangs: it does not count as progress.
- Never run `vm.sh input on` on your own: it takes the user's real keyboard and
  mouse away from the host (both Ctrl switch back). The user does it.
  To test the input path without hands: a uinput keyboard (python-evdev is
  installed, `/dev/uinput` has an ACL for spoki4) attached via `vm.sh qmp`
  `object-add input-linux` worked on 2026-11-04.
- Keyboard: keys come on the NKRO interface (`by-id/...Gaming_Keyboard-event-if01`)
  as well as the boot one (`-event-kbd`); vm.sh grabs both. Mouse:
  `wheel-debounce.service` grabs the G703's node and re-emits it as
  "Logitech G703 LS (debounced)" (no by-id link); vm.sh finds it by name.
- pacstrap runs the host's `/etc/pacman.d/hooks`, where limine's initcpio hook
  shadows the guest's mkinitcpio hook; `mkimage.sh` creates the presets itself.
- QEMU here has no `virtio-vga` module; `start --no-gpu` boots without a display
  (enough to test ssh, virtiofs, builds while the host still owns the GPU).
- 2026-11-06 23:22: a `vm.sh reset` (the host watchdog of vmtest.sh, a
  test that left Chrome filling vidmem) left the 3080 Ti wedged: the guest
  then hangs in the firmware with the GPU (fine with --no-gpu); reset,
  kill + start did not help. Recovery needs root (PCI reset) or a host
  reboot. local/vm/vmtest.sh (build on the host, reload in the guest,
  nvtest under two watchdogs) is how agents check a build now.
  With `NVVM_ROMBAR=0 local/vm/vm.sh start` the guest boots (OVMF skips the
  GPU's ROM), but its driver fails: "_kgspBootGspRm: unexpected WPR2 already
  up" (Xid 62). The host's vfio resets the device on start and on a reset
  from inside the guest ("vfio-pci 0000:01:00.0: reset done"), yet WPR2
  stays up: GSP survives FLR / that reset. It needs a secondary bus reset of
  the host's root port or a power cycle (root: e.g. remove the device and
  rescan, or `echo 1 > .../reset` with reset_method bus on the host, else a
  host reboot). Also tried without root (2026-11-07 ~01:40): QEMU has no
  option to skip FLR; a reset of both functions at once (00:13:08, the bus
  reset vfio does) left WPR2 up as well; the GPU reports NoSoftRst+, so a
  D3hot -> D0 cycle from the guest does not reset it either. Only a power
  cycle (host reboot) clears it. Why it happened: nvtest focus (new) left
  under `set -e` without stopping its Chrome sessions (wait_ram returned 1),
  so a game page kept vidmem full and the ssh session open; the host
  watchdog of vmtest.sh saw no output for 600 s and hard-reset a guest that
  was in fact alive, with GSP busy. Now nvtest.sh stops its sessions on any
  exit (trap) and vmtest.sh's watchdog first stops the test's processes over
  a new ssh session, resetting only a guest that does not answer.
- Timings (fps, eviction throughput) differ a little from bare metal (IOMMU,
  nested paging): confirm final numbers outside the VM.
- Verified 2026-11-04: PCIe 4.0 x16 and the full 16 GB BAR1 in the guest,
  `reload stock` 4 s, `reset` 11 s, Plasma back each time, input switching.

Rebased 2026-11-04 (user's go-ahead): `local/vidmem-615` now sits on tag
`615.78.08`, no conflicts (upstream touched os-interface.c, osapi.c, regmap.c,
mem.c only cosmetically; its os-interface.c also fixes the dmem cgroup build on
7.3). The pre-rebase branch is kept as `local/vidmem-615.71.09-backup`; the
rebase is not pushed (origin still has the 615.71.09 one, needs a force push).
Building for CachyOS: the kernel is clang + ThinLTO, so kernel-open needs
`LD=ld.lld` (kernel-open/Makefile passes LD=ld otherwise; `vm.sh build` does
it). The host runs the same kernel and gcc as the guest, so a tree built on the
host (`make modules SYSSRC=/lib/modules/$(uname -r)/build LD=ld.lld
IGNORE_CC_MISMATCH=1`) loads in the guest too. The guest needs `vm.sh install`
once (writes the reserve modprobe conf; without it the reserve is 0).
Bench in the guest: `NVTEST_FORCE=1 ./nvtest.sh all` via `vm.sh ssh` (the guest
is always logged in, hence FORCE; nvt-session.sh gives its test KWin a private
DBus then, chromium is used when google-chrome is missing).

Tracing (2026-11-04): tracepoints `nvidia_vidmem:*` + `local/trace/nvtrace.py`
(record / report / live / mark), see "Tracing" in README.md. Use it to explain
any stutter before changing the policy: the report lines up frame gaps (page
flips) with moves, pauses, make-room waits, policy steps and RM lock waits.

## Where things are (set up 2026-11-01)

- PC: repo at `~/projects/open-gpu-kernel-modules` (branch
  `local/vidmem-desktop-reserve`). Its `kernel-open/*.ko` are already built
  from commit 461f1a2b (stage 0 + stage 1) and installed (running since the
  2026-11-01 17:35 boot).
- Mac: clone at `~/projects/open-gpu-kernel-modules` (shallow, depth 50;
  fetch over HTTPS, push URL is SSH). `ssh spoki4-pc` reaches the PC over
  Tailscale with `~/.ssh/id_ed25519`; password login on the PC is disabled.
- Mac: pushing to GitHub does not work yet — `~/.ssh/config` points
  github.com at a missing `~/.ssh/id_rsa` and `gh` is not logged in. Either
  fix that (user action) or commit and push from the PC repo.
- Mac login shell is fish: run remote scripts as `ssh host 'bash -ls' <<'EOF'`,
  and use `ssh -n` for nested ssh calls inside such a heredoc.
- tmux is installed on both machines; run builds and tests on the PC inside
  `tmux` so they survive a dropped SSH connection.

## Goal

Stop the desktop from freezing when a game fills VRAM, and eventually free VRAM
held by idle processes (Chrome, Electron) by moving it to system RAM — a
personal patch on NVIDIA open-gpu-kernel-modules, rebased by hand every few
months on driver updates.

## The machine being patched ("the PC")

- Ubuntu 26.04, kernel 7.0.0-42-generic, KDE Plasma on Wayland (kwin_wayland).
- RTX 3080 Ti (GA102, Ampere, GSP firmware), 12 GB; monitors (2x 4K@120) on the
  NVIDIA card. AMD iGPU (Ryzen 9 9950X3D) is present but drives nothing.
- Driver 595.99.02, `nvidia-driver-595-open`; userspace `libnvidia-gl-595`
  595.99.02. Kernel module version must match userspace exactly.
- Secure Boot enabled. Own MOK key `CN=spoki4-pc local NVIDIA module signing`
  is enrolled; key files in `/var/lib/nvidia-local-mok` (root only).
- Kernel 7.0.0-38-generic is still installed with untouched Canonical modules:
  the GRUB fallback if a patched module breaks boot.
- Persistent journal, kdump-tools installed. Reachable over Tailscale (ask the
  user for the address; this repo is public). openssh-server + tmux were being
  installed at handoff.
- `sudo` is sudo-rs (prompt `[sudo: authenticate]`); the user types passwords.
- Existing modprobe options: `NVreg_PreserveVideoMemoryAllocations=1`,
  `NVreg_TemporaryFilePath=/var`, `NVreg_UseKernelSuspendNotifiers=1`.

## What happened (root cause)

2026-10-31: Call of Duty (Proton/vkd3d, DX12) plus desktop apps filled VRAM
(11.9/12 GB; Chrome alone ~2 GB). kernel log: `pmaAllocatePages NV_ERR_NO_MEMORY`,
`ctxBufPoolReserve failed`, thousands of `Failed to allocate NVKMS memory for
GEM object`; kwin: `Failed to allocate an egl gbm swapchain graphics buffer`,
`Key repeat discarded`. Desktop froze → hard reset. No Xid.

Why: the 595 "sysmem fallback" is only in nvidia-drm
(`kernel-open/nvidia-drm/nvidia-drm-gem-nvkms-memory.c:588`) and only for
`NV_GEM_ALLOC_NO_SCANOUT`; kwin's scanout buffers get no fallback.

## Branch `local/vidmem-desktop-reserve` (fork Spoki4/open-gpu-kernel-modules)

Based on upstream tag `595.99.02`. Remotes: `origin` = fork, `upstream` = NVIDIA.

### Stage 0 — vidmem reserve for the compositor: DONE, installed, verified

- `src/nvidia/src/kernel/mem_mgr/video_mem.c`: `_vidmemCheckUserReserve()`,
  called before `_vidmemPmaAllocate()` in `vidmemConstruct_IMPL`. User-mode
  allocations fail with `NV_ERR_NO_MEMORY` if free PMA memory would drop below
  `RmVidmemUserReserveMB`. Exempt: processes named `kwin_wayland`/`Xwayland`
  (RM client name or thread comm), and privLevel >= KERNEL only from kernel
  threads (since 2026-11-01 18:07; before, all nvkms allocations were exempt,
  see "Stage 0 stress tests"). Since 18:40 the reserve is also a place: the
  bottom RmVidmemUserReserveMB of vidmem is off-limits to reserve-bound
  allocations, so kwin can still get contiguous scanout buffers. Since 19:45
  that block is the whole reserve (no separate byte check), and HEAP_FREE /
  the Vulkan budget of ordinary processes excludes its free part.
- Enabled via `/etc/modprobe.d/nvidia-vidmem-reserve.conf`:
  `options nvidia NVreg_RegistryDwords="RmVidmemUserReserveMB=768"`.
- Verified 2026-11-01 with `local/vram-hog --hold 40`: hog got 8 GB, then one
  denial at 945 MB free (`VK_ERROR_OUT_OF_DEVICE_MEMORY`); kwin, Chrome GPU
  process unaffected, zero compositor errors, memory returned after.
- NOT yet tested in the actual game.

### Stage 1 — read-only vidmem census: DONE, installed 2026-11-01, works

- `rm_vidmem_census()` in `src/nvidia/arch/nvalloc/unix/src/osapi.c` (declared
  in both `nv.h` copies, listed in `src/nvidia/exports_link_command.txt`).
- `/proc/driver/nvidia/gpus/<bdf>/vidmem_census` in `kernel-open/nvidia/nv-procfs.c`
  (root only; write a PID, read the result; 1 MB buffer via `single_open_size`).
- Buckets: `shared` (DupCount>1) > `compressed` > `cpu-mapped` > `candidate`.
- kwin_wayland's `/proc/<pid>/fd` is unreadable for the user (capabilities),
  so a "scan fds for /dev/nvidia" loop misses it: census it by `pgrep -x`.
- Per-process totals double-count shared buffers (kwin imports everyone's
  window buffers), so they do not add up to `nvidia-smi` used.

First measurement, 2026-11-01 17:40, fresh login, light desktop (Chrome with
usual tabs, Claude desktop, Telegram, ghostty; 1.7 GB VRAM used in total):

| process        | total | candidate | compressed | shared |
|----------------|------:|----------:|-----------:|-------:|
| kwin_wayland   | 1356  |         7 |         16 |   1331 |
| plasmashell    |  642  |         7 |        223 |    408 |
| Chrome GPU     |  633  |        26 |        183 |    420 |
| Claude desktop |  326  |        18 |        169 |    136 |
| ghostty        |  100  |         5 |         32 |     60 |
| Telegram       |   94  |         3 |         10 |     80 |
| ~10 small KDE  | 3 each|         3 |          0 |      0 |

(MB.) Chrome detail: shared = 6x 66 MB (dup 5; 4K fp16 swapchain/window
buffers exported to kwin) + 4x 6 MB; compressed = kind 0x8, 2–32 MB each
(render targets); candidates are all <= 4 MB.

Second measurement, 2026-11-01 ~17:50, usual apps open (Chrome with its
normal tabs, Claude desktop, Steam, Spotify, Telegram, ghostty; 4.8 GB used):

| process          | total | candidate | compressed | shared |
|------------------|------:|----------:|-----------:|-------:|
| Chrome GPU       |  3473 |        67 |       1757 |   1640 |
| kwin_wayland     |  2552 |        10 |        141 |   2398 |
| plasmashell      |   678 |        11 |        231 |    432 |
| Claude desktop   |   364 |        18 |        207 |    136 |
| Steam webhelper  |   348 |        34 |        227 |     81 |
| Spotify          |   270 |        31 |        140 |     96 |
| Xwayland         |   244 |         3 |          0 |    239 |
| ghostty/Telegram | ~95 each |     ~4 |     10–31 |  60–80 |

Chrome detail: shared 18x 66 MB (kind 0x6, dup 5: window buffers held by
kwin, ~4K x 8 bytes/px although HDR and WCG are off) + 7x 56 MB (kind 0x8,
compressed, dup 2–3); compressed 40x 18 MB + a few 50–220 MB buffers;
candidates 67 MB, all <= 4 MB each. Clients: one holds 2.4 GB.

Conclusion: the `candidate` bucket is tiny (~50–180 MB across all apps), so
stage 2 as planned (move only private, uncompressed, unmapped allocations)
is not worth it. The memory sits in `compressed` and `shared`. Open:
- re-measure with Chrome "heavy" (~2 GB, as during the 2026-10-31 freeze),
  and on the game itself, before deciding anything;
- moving `compressed` would need a decompressing copy and a PTE kind change
  to the uncompressed equivalent (unverified that this is safe);
- moving `shared` would need every importer's mappings (kwin) rewritten.

### Tooling in `local/`

`build.sh` (modules + vram-hog), `install.sh enroll-key|install` (strip, sign
with MOK, install to `/lib/modules/$KVER/updates/nvidia-local`, depmod,
update-initramfs), `rollback.sh`, `census.sh`, `vram-hog.c` (refuses to run
unless the reserve is configured). See `local/README.md`.

Gotchas already hit:
- `set -o pipefail` + `cmd | grep -q` fails on SIGPIPE: capture output first.
- `mokutil --test-key` wording differs by version: match the SHA1 fingerprint.
- The MOK Manager blue screen timed out before the 4K monitors woke up; fixed
  with `mokutil --timeout -1`.
- Ubuntu's prebuilt package ships RM as `nv-kernel.o_binary`; we build the full
  tree from source (`make modules`, ~5 min with -j32).

## Stage 0 stress tests (2026-11-01)

Tools: `local/stress.sh race|gbm|run NAME CMD` (watchdog: free-memory
sampler with a kill switch at 200 MB, kernel + user journal, a `spectacle -b`
screenshot every 3 s as the kwin liveness probe, census of the hogs, PASS/FAIL
summary; logs in `local/stress-logs/`), `vram-hog --threads/--churn/--load/
--start-at/--floor-mb`, `gbm-hog [--scanout|--dumb]`. Build: `./local/build.sh tools`.
NVML "free" equals PMA free (calibrated: both 820 MB at the same denial).

- **Race (check vs PMA alloc, RO API lock, no GPU lock): not reproduced.**
  10 runs, 8x64 MB / 16x32 MB threads and 4 processes x 2 threads started at
  the same instant: hog-side minimum free 786 MB (reserve 768), every run
  PASS. Stays a theoretical overshoot of (threads - 1) x chunk.
  nvidia-smi saw 755–763 MB: that is kwin's own ~31 MB screenshot buffer,
  allowed (kwin is exempt).
- **GBM / dumb buffers bypass the reserve: confirmed.** They are allocated by
  nvkms (kernel privilege) on behalf of the calling process, and census of
  `gbm-hog` shows 0 allocations of its own.
  - GBM render (NO_SCANOUT): went to 405 MB free, stopped only by the hog's
    safety floor (would continue; nvidia-drm would then fall back to sysmem).
  - GBM scanout: stopped at 693 MB free with `Failed to allocate NVKMS memory
    for GEM object` — not a reserve denial. Scanout allocations fail with
    ~700 MB still free (contiguity or some nvkms constraint?). Open question:
    can kwin's own scanout allocations fail the same way with the reserve
    intact?
  - Dumb buffers (primary node): stopped at 610 MB free, ENOMEM.
  - Chrome's large buffers are in Chrome's own RM client (census), i.e.
    covered by the reserve; which real apps allocate through GBM is unknown
    (Xwayland/glamor does, but is exempt).
  - No app GPU process restarted; apps released ~600 MB of caches under the
    pressure.
- **Fix (2026-11-01 18:07, installed, verified):** `_vidmemCheckUserReserve`
  exempts kernel privilege only in kernel threads
  (`osGetCurrentProcessFlags() & OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD`).
  nvkms allocations made inside a user process's ioctl now count against
  that process unless it is kwin/Xwayland; denials say `kernel client`. The
  nvidia-drm KAPI RM client is named `(udev-worker)` (created by udev).
  Re-run after the fix (`stress-logs/gbm-fix`, `race-fix`), all PASS:
  GBM render denied at 778 MB free and nvidia-drm fell back to sysmem
  (gbm-hog now stops after 5 buffers that take no vidmem); GBM scanout
  stopped at 790 MB, dumb at 774 MB; race runs unchanged (min 779 MB). No
  NVIDIA errors at boot/login, no app GPU process restarted.
- **Scanout needs contiguous memory; the byte reserve did not protect it.**
  nvkms allocates scanout surfaces `PHYSICALITY_CONTIGUOUS` + ISO
  (`nvkms-kapi.c:835`); kwin's 32 MB swapchain buffers are contiguous too
  (census `contig` 1). New `/proc/driver/nvidia/gpus/<bdf>/vidmem_pma`
  (world-readable, `rm_vidmem_pma_info()` in osapi.c) prints
  `<free MB> <largest contiguous free MB>`. `stress.sh scanout` fills vidmem
  to the reserve with vram-hog (plain or `--churn`), then a copy of gbm-hog
  named `kwin_wayland` (exempt by name, like the real compositor) asks for 6
  scanout buffers of 3840x2160:

  | build (2026-11-01)           | plain fill | churn 4x64, 30 s      | churn 8x16, 60 s |
  |------------------------------|------------|-----------------------|------------------|
  | bottom-up (stock placement)  | 6/6        | **0/6** (25 MB contig of 770 free) | 5/6 (186 MB) |
  | reserve-bound top-down       | 6/6        | **0/6** (11 MB)       | 6/6 (134 MB)     |
  | bottom block partition       | 6/6        | 6/6 (166 MB)          | 6/6 (166 MB)     |
  | + exempt non-contig top-down | 6/6        | 6/6 (286 MB)          | 6/6 (286 MB)     |

  This is very likely the 2026-10-31 freeze mechanism: a game streaming
  textures at the limit fragments free vidmem, kwin's swapchain allocation
  fails although hundreds of MB are free.
- **Fix (installed 2026-11-01 18:40, all stress tests PASS):** reserve-bound
  allocations get `PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE` above the bottom
  `RmVidmemUserReserveMB` of PMA region 0 (`_vidmemReserveBoundRange`; this
  GPU has one region, 0x5200000-0x2eb8effff, ISO). Exempt callers keep the
  bottom-up placement for contiguous allocations, and their non-contiguous
  ones go top-down (`PMA_ALLOCATE_REVERSE_ALLOC`), so the bottom block is
  left to scanout buffers. Side effect: non-exempt GEM/dumb scanout requests
  are now refused with ~1.1 GB free (their range is full, the bottom block is
  not theirs); Chrome logs `Failed to allocate NVKMS memory for GEM object`
  under pressure and carries on (GPU process never restarted).
- Still open: the bottom block holds only ~290 MB contiguous after login (the
  rest is boot/kernel-thread allocations and contiguous kwin buffers):
  enough for ~9 x 32 MB buffers. If kwin ever needs more at once (mode change
  on both 4K outputs at the limit), raise `RESERVE_MB`. T3 (GPU load +
  compositor churn), T4 (lock screen, krunner, panel at the limit), T5
  (Chrome after denials, WebGPU), the game.

## Game test (2026-11-01 18:49–18:58, build 3a3a16cc): PASS

Call of Duty MW4 (`sp26-cod.exe`, Proton) with the usual apps open, as a
stress test: ultra settings + ray tracing (7 fps, unplayable, but that is the
GPU), then restarted by the user once (18:54:54, not a crash). The game ran
into the reserve: first logged denial 248 MB at 934 MB free (18:56:58), then
1000–2000 more denials within ~25 s; it kept running. The user reports the
desktop stayed smooth the whole time. Journal: zero kwin gbm/egl/swapchain
errors, no `Key repeat discarded`, no Xid / pmaAllocatePages /
ctxBufPoolReserve; 9 refused scanout GEM requests from some non-exempt
process at 18:54:38. Afterwards 8645 MB free, 2244 MB contiguous. The
user's normal game settings play fine. No vidmem_pma sampling ran during
the game (next time: `stress.sh run game sleep 1800` before starting it).

## Budget, game memory and the block-only reserve (2026-11-01 19:00–19:50)

- **The Vulkan/DXGI budget ignored the reserve.** The NVIDIA userspace driver
  reports `heapBudget = own usage + HEAP_FREE` (vram-hog `BUDGET` lines): at
  the reserve a process still saw ~780 MB of budget it could not get, so a
  game planned its streaming for it and hit thousands of denials. Fixed in
  `_kmemsysGetFbInfos` (kern_mem_sys_ctrl.c): reserve-bound callers get
  `vidmemUserReserveReportedFree()`. NVML (`nvidia-smi`) shows the same
  reduced free memory to ordinary processes; the test tools read the real
  PMA free from `vidmem_pma` instead (`real_free_mb()` in nvml-lite.h).
- `vidmem_pma` now prints 5 numbers: free, largest contiguous free, block
  size, block free, block largest contiguous free (MB); API lock taken for
  reading only. Census adds `sysmem N allocs X MB (+ os-descriptor ...)`:
  GPU-visible system memory of the process.
- **Game session** (`stress.sh game`, logs `stress-logs/game-1`): CoD always
  fills whatever budget it gets (streaming cache) and keeps the rest in
  system memory itself:

  | settings     | other apps | game vidmem | game sysmem | fps   |
  |--------------|------------|-------------|-------------|-------|
  | ultra + RT   | open       | 8.1 GB      | 2.3–10 GB   | 5–7   |
  | ultra, no RT | closed     | 9.9 GB      | 4.8 GB      | ~60   |
  | ultra + RT   | closed     | 9.9 GB      | 9.6 GB      | 10–13 |

  The in-game menu shows "12 GB available, other 0": it reads the static
  adapter size, not the budget. 1.8 GB more vidmem doubled RT fps, so every
  hundred MB the reserve idles matters when a game is VRAM-bound.
- **Block-only reserve (installed 19:45, all stress tests PASS).** With the
  partition in place, the old byte check ("keep 768 MB free") only idled
  ~400 MB above the block. Now reserve-bound allocations fail only when the
  free memory outside the bottom block cannot hold them, and HEAP_FREE for
  them is total free minus the block's free part. Denial point moved from
  ~825 MB free to ~341 MB free (of which ~325 MB in the block). The block
  holds ~380–440 MB of exempt/kernel allocations after login; ~325 MB free,
  282 MB contiguous at rest, never below ~100 MB while the scanout probe
  holds 6 x 32 MB. Ordinary apps' GEM scanout requests (Claude, Chrome) are
  refused under pressure and logged as `Failed to allocate NVKMS memory`.
- Game with the block-only reserve (19:58–20:01, `stress-logs/game-2`),
  ultra + RT with the usual apps open: **~12 fps (was 5–7)**; game vidmem
  7.7–8.1 GB, sysmem 9.2–9.5 GB, total free down to ~280 MB of which ~275 MB
  in the block (block largest contiguous stayed 272 MB). Still many denials
  (e.g. a 590 MB request at 468 MB free): the game sometimes asks past its
  budget. Zero kwin errors, no NVKMS failures, no Xid; app GPU processes
  survived. Not a controlled A/B (the apps used less vidmem after this boot),
  so the gain is budget honesty + ~400 MB more vidmem + noise.
- Open: the block lost ~80 MB during an earlier game (kwin window textures or
  RM-internal allocations, which bypass vidmemConstruct).

## Chrome renders at scale 2 (2026-11-01 20:10, reverted on purpose)

Chrome's launchers carry `--disable-features=WaylandFractionalScaleV1`
(`~/.local/share/applications/{google-chrome,com.google.Chrome}.desktop`,
`~/.config/autostart/local-chrome.desktop`). Without the fractional-scale
protocol Chrome takes the integer output scale: `devicePixelRatio` 2 at KDE
scale 1.4, kwin then downsamples. That is why its window buffers were 66 MB
(5486x3086x4 rounded to 2 MB pages = 67584 KB) instead of 33 MB.
Without the flag: Chrome 2758 -> 1507 MB right after restart (window buffers
1318 -> 439 MB, tiles 1404 -> 1029 MB), but a 1 px see-through line appears
between the toolbar and the page, at KDE scale 1.4 and at 1.5 alike, and
`--disable-features=WaylandOverlayDelegation` does not remove it (probably
Chrome rounding a fractional toolbar height). The user chose to keep the
flag: no line, no savings. Do not suggest this again unless Chrome fixes it.

## RM-internal allocations ate the block (2026-11-01 20:05–20:35, fixed)

Game run 3 (ultra, no RT, usual apps open, `stress-logs/game-3`): **30–40 fps**
(60 with the apps closed), game vidmem 8.2 GB, sysmem ~5 GB. And the block
shrank before and during it: 359 MB free / 272 MB contiguous at 20:05, three
Chrome restarts -> 236 / 102 MB, during the game 97 / **34 MB** (one 4K
buffer left; the stress.sh kill switch fired at <100 MB free). Cause: RM's
internal allocations for a process (GR context buffers, page tables, channel
memory) call PMA directly, not vidmemConstruct, and PMA places them
bottom-up, i.e. into the block; exiting processes leave holes.

Fix (installed 20:25): `pmaAllocatePages()` is now a wrapper around the
original (`_pmaAllocatePagesImpl`) that adds the above-the-block address range
to every allocation without one, unless the calling thread is a kernel thread
or belongs to kwin_wayland/Xwayland by thread name or process
(thread-group-leader) name (`vidmemUserReservePmaRange()`;
`os_get_current_process_leader_name()` added to kernel-open). The exemption
check in vidmemConstruct also looks at the process name now.
After login the block is 569 MB free / 530 MB contiguous (was ~387 / 378);
all stress tests PASS, block never below 284 MB free / 228 MB contiguous
while the probe holds 6 x 32 MB. Trade-off: those internal allocations now
come out of the games' share (denial point ~510 MB free instead of ~341);
the block could be shrunk (e.g. 640 MB) if kwin's ~260 MB own use stays put.

## Stage 2 preparation (2026-11-01 20:50, nothing installed or run as root)

Goal (user): 60 fps at ultra without RT with the usual apps open (30–40 now;
60 with apps closed: the game lacks ~1.7 GB). Not wanted: software rendering
for apps, Chrome fractional scale (see above). Killing Chrome's GPU process
does not help (1593 -> 1845 MB right after the restart). Chosen direction:
variant A of stage 2: move idle apps' vidmem to sysmem while they keep
running; compressed allocations are copied through a GPU VA with their own
kind (L2 decompresses) and remapped with the uncompressed kind. Findings:
GA102 uses one-to-one comptags (`bOneToOneComptagLineAllocation`,
kern_mem_sys.c:422) and requires VA-based CE ops for that
(`memmgrUseVasForCeMemoryOps`, :448); RM already maps compressible memory with
`memmgrGetUncompressedKind_HAL` when out of comptags
(virt_mem_allocator_gm107.c:2326). Unknown: whether usermode drivers depend
on the compressed kind (e.g. tag-only fast clears) after a remap.

Ready:
- `local/safety.sh install|remove|status` (root): sp5100_tco watchdog fed by
  systemd (RuntimeWatchdogSec=30s), panic on soft/hard lockup and oops,
  reboot 10 s after a panic; efi_pstore keeps the log. Not run yet. kdump is
  inactive (no crashkernel= on the command line); netconsole needs the Mac on
  the same LAN (Tailscale's tun device cannot do netpoll).
  **Installed 2026-11-01 20:54 (re-run 20:57):** /dev/watchdog0 "SP5100 TCO
  timer" fed by systemd every 30 s, panic=10, panic_on_oops, soft/hard
  lockup panic on. Undo: `sudo ./local/safety.sh remove`.
- Restore point: `/.snapshots/2026-11-01_2056-pre-stage2` (read-only btrfs
  snapshot of / incl. /home, made by `local/snapshot.sh`; already contains the
  safety.sh config). Delete it when stage 2 is done, it pins disk space.
- `local/compr-test` (built): 2048x2048 RGBA8 colour-attachment image in its
  own 16 MB allocation, kind 0x8 "compressed" per census, 2 MB pages; mode
  `write` (full clear + rect clears + data block each iteration) or `hold`
  (write once, then only read back); prints OK / MISMATCH per iteration.
- Not written yet: the eviction code in the driver.

## Stage 2 eviction: WORKS (2026-11-02, build 66a96d54, installed)

`src/nvidia/src/kernel/mem_mgr/vidmem_evict.c` (header comment explains the
design). Per process, with all its channels disabled+preempted
(FIFO_DISABLE_CHANNELS RPC via `pGpu->hInternalClient/hInternalSubdevice`):
sysmem in chunks of the vidmem page size (contiguous allocs: of the largest
mapping page) -> copy with `pMemoryManager->pCeUtils` (virtual mode on GA102:
reads vidmem through an FB alias with kind GENERIC_MEMORY_COMPRESSIBLE_
DISABLE_PLC, i.e. decompressing) -> for each GPU mapping `dmaFreeMap` +
`dmaAllocMap` at the same VA (DMA_OFFSET_FIXED, forced page size) with a sysmem
sub-memdesc and the uncompressed kind -> drop the static BAR1 reference (like
vidmemDestruct) -> `pmaFreePages`. Restore: new PMA pages (contiguous allocs:
contiguous, aligned to the largest mapping page), retake static BAR1 ref,
copy back, remap with the saved per-mapping kinds.
Hooks: `MEMORY_DESCRIPTOR.pLocalEvictState` (generated/g_mem_desc_nvoc.h);
`memIsReady_IMPL` restores on any RM use (dup, controls, GPU/CPU map,
memGetByHandle*); `memmgrFree` frees the sysmem copy instead of PMA pages.
Interface: `/proc/driver/nvidia/gpus/<bdf>/vidmem_evict` (root), wrapper
`sudo local/evict.sh dry|evict|restore <pid|chrome|name>` (in sudoers since
2026-11-02, file /etc/sudoers.d/nvidia-local-evict).

Gotchas found:
- `CeUtils.bUseVasForCeCopy` is never set (upstream bug, also in
  mem_utils.c:101); the real flag is `pCeUtils->pChannel->bUseVasForCeCopy`.
- Static BAR1 is enabled on this GPU: every vidmem alloc holds
  `staticBar1MappingRefCount` 1 (2 MB-page allocs; others fail with
  NOT_SUPPORTED and keep 0).
- Right after boot PMA hands out contiguous blocks even when not required:
  ~2/3 of Chrome's private vidmem is `bContig`.
- pgrep for a Chrome profile: `type=gpu-process.*<user-data-dir>` (the type
  comes first in the command line).

Tests (all PASS, no Xid / NVRM errors, no GPU process restarts):
- compr-test hold (16 MB kind 0x8 image): evict 24 ms, reads OK from sysmem,
  restore 41 ms, 0 MISMATCH. write mode (fast clears + rendering into the
  evicted image): 0 MISMATCH; re-evict; process killed while evicted ->
  sysmem freed, evicted total back to 0.
- Throwaway Chrome profile + WebGL aquarium: 166 MB evicted in 134 ms, still
  rendering at ~113 fps from sysmem, restore clean (one alloc freed by Chrome
  while evicted, free path worked).
- Real Chrome (old build, reserved-VA mappings only): 155 MB evicted for
  ~5 min while the user browsed; "works well".
- Full desktop after login (build 66a96d54), PMA free 8373 -> 9722 MB
  (+1349 MB) in ~1.1 s total:
  | process        | evicted | not eligible (main reason)         |
  |----------------|--------:|------------------------------------|
  | Chrome GPU     |  913 MB | 1370 MB (not-owner 1140, pma-info 168) |
  | Claude desktop |  180 MB | 139 MB (not-owner)                  |
  | Spotify        |  156 MB |  98 MB (not-owner)                  |
  | ghostty        |   97 MB | 398 MB (not-owner 264, pma-info 132)|
  | Telegram       |    3 MB |  20 MB                              |
  kwin/Xwayland/plasmashell not touched (the desktop itself).

Not done yet:
- Automatic trigger (who to evict, when): a small daemon, e.g. evict all
  non-desktop apps when a fullscreen game starts / when PMA free above the
  reserve block drops below N, restore when the game exits.
- `not-owner` (buffers imported from another process: window buffers, GEM)
  and `pma-info` (refCount != 1) are the remaining big buckets.
- Restore of a contiguous alloc needs a contiguous block; if VRAM is
  fragmented the restore (and the RM call that triggered it) fails with
  NV_ERR_NO_MEMORY and the memory stays in sysmem.
- Untested: suspend/resume with evicted memory; the game while apps are
  evicted.

## Automatic eviction policy (2026-11-02, build b07db76c, installed)

In the driver, no daemon (`vidmem_evict.c`, bottom half). The 1 Hz RM callback
queues a work item (API lock RW) every 2 s: GPUMON perfmon samples from GSP
(`PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2`: pid on GR/NVDEC/NVENC, ~7/s) give
per-process last-activity; avail = PMA free outside the reserve block.
avail < 1024 MB: evict whole processes idle >= 10 s, least recently active
first, 2 per tick, never the busiest process nor kwin/Xwayland/plasmashell.
Restore: active process as soon as it fits above low+512 MB; idle ones after
30 s without pressure and only if 2560 MB stay free. Registry:
RmVidmemAutoEvict, RmVidmemEvictLowMB, RmVidmemEvictHighMB. `evict.sh status`
(per process vram/ram/evictable/idle + reserve block low-water marks),
`evict.sh auto on|off`, `evict.sh shared <pid>`. Restores from the worker
(kernel thread) pass the above-the-block range explicitly.

Tests: `vram-hog --load --hold 40`: evicted steam, spotify, steamwebhelper,
chrome (1.2 GB), restored all after exit. `vram-hog --load --churn 40` (a game
at the limit): hog grew into the freed memory (held 9013 MB vs 7424 filled),
1.4 GB evicted, no Xid; Chrome got reserve denials for new SharedImages
("could not create backing") and dropped evicted tiles itself, GPU process
survived; 8 TSG allocs of some process failed with NO_MEMORY at 1 MB avail
(reserve working as designed). Active procs restored right after, idle ones
30 s later.

Research (2026-11-02):
- `pma-info` = PMA_ALLOC_INFO.refCount > 1, bumped by heapReference() on dup:
  the owner side of shared buffers. Same class as `not-owner`.
- Chrome's 1.6 GB shared vidmem (`evict.sh shared chrome`): 1188 MB = 18
  window buffers, 66 MB each, kind 0x6 (uncompressed), contiguous, owned by
  the nvidia-drm kernel client "(udev-worker)", dup'd twice into kwin (one
  with 1 GPU mapping) and twice into Chrome (one with 1 GPU mapping); memdesc
  RefCount 7 = 1 + 2 mapping children + 4 dups, i.e. no display/ctxdma
  reference while not scanned out. 392 MB = 7 compressed (0x8) 56 MB buffers
  dup'd between Chrome's own RM clients only. 60 MB = 12 small window buffers.
  => Evictable by generalising the engine to all Memory objects sharing a
  memdesc (pause kwin's channels too during the remap; kwin does not
  composite hidden windows while a game is fullscreen). Restore of these
  needs 66 MB contiguous blocks -> item 5 (restore non-contiguous) matters.
- Reserve block (768 MB): lowest free since boot 427 MB (kwin), i.e. ~340 MB
  used at the peak so far; needs a day of data before shrinking.

## Shared buffers, Xid incident, "evicted stays usable" (2026-11-02 01:30-02:05)

- Build 712f44a5: buffers shared only with the compositor (window buffers:
  nvidia-drm kernel client owns, kwin + app hold) are evicted too, two-phase:
  copy + the app's remaps with only the app paused, then kwin paused just for
  its remaps + PMA free. Holders found by scanning all clients for Memory
  objects with the same memdesc; free of an evicted shared buffer follows the
  PMA refCount (one per holder). Chrome eligible: 1497 of ~1506 MB; manual
  evict 1491 MB in 1.04 s, window still rendered fine (kwin composites from
  sysmem).
- INCIDENT, `vram-hog --churn` with auto on (01:47): Xid 31 FAULT_PTE in
  Chrome (x3, GPU process restarted, WebGPU then disabled until Chrome
  restart) and once in kwin; windows flickered. Timeline: restore-on-access
  of Chrome's 56 MB internal shared buffer failed (no vidmem) -> 4x
  `RmExportObject ... DupObject failed 0x51` -> 7 s later Xid. The new Chrome
  GPU process was evicted 2 s after it started -> Xid in it and in kwin.
  ghostty looped evict -> access -> restore -> evict; restores failed ~1000/s
  under pressure. No remap failure was ever logged by our code.
- Fixes, build 268b0c41 (auto OFF by default since c9de3262):
  * memIsReady() no longer restores. Dups/exports/controls/memIsDuplicate run
    on the evicted memdesc; memGetMemInterMapParams() maps the sysmem copy
    (`vidmemEvictGetSysMemDesc`); vidmemCopyConstruct skips the static BAR1
    ref while evicted (counted in the state, taken on restore). Restore only
    in memCreateKernelMapping, memGetByHandle* and
    memGetMemoryMappingDescriptor (CPU maps, ctxdma/scanout, channel buffers),
    falling back to the reserve block if needed.
  * policy: no eviction in a process's first 30 s nor 60 s after it needed a
    restore on access; remaps never defer the TLB invalidate.
  * `evict.sh trace on|off` logs every remap with its VA range (match against
    Xid addresses); `evict.sh` serialized with flock (a `watch evict.sh
    status` overwrote results).
- Test 02:02 (throwaway Chrome profile + WebGL aquarium, churn 60 s, manual
  evict every 5 s, 12 times): no Xid, no restore on access, no export
  failures, GPU process survived, 337 remaps traced, restore 241 MB / 218 ms.
- Full-desktop churn test 02:10 (auto on at runtime, trace on, churn 60 s):
  PASS. No Xid, no restore on access, no export failures; Chrome GPU process
  and kwin unchanged. Evicted up to 2.38 GB (steam, spotify, steamwebhelper,
  chrome 1.4 GB, ghostty, Telegram, claude-desktop); the hog grew to 9783 MB
  (filled 7168 at first, i.e. +2.6 GB for the "game"). All restored within
  ~40 s after the hog exited.
- Step 2, builds cfd0ff6f + 04bfb834 (installed, auto ON by default again):
  an allocation the reserve would deny is created in sysmem instead when the
  caller is idle on the GPU >= 10 s, not new (30 s) and not the desktop; no
  contiguous/ISO/fixed-address/>512 MB requests; it is allocated anywhere
  (explicit whole PMA range, may sit in the block for a moment), then
  evicted at the end of vidmemConstruct (`vidmemEvictBornAllocation`, no
  mappings yet, scrubbed content). Restores into the block keep >= 256 MB
  free there.
  Test 02:13 (first rule "not the busiest"): vram-hog's fill load is on the
  copy engine, invisible in GR samples, so the hog counted as background and
  got 4.7 GB in sysmem (filled 11.8 GB); compr-test write mode rendered fine
  into its sysmem allocations; no Xid. Hence the idle rule.
  Test 02:17 (idle rule, churn 50 s): no Xid, Chrome/kwin unchanged, 2.0 GB
  evicted, 5 allocs / 139 MB created in sysmem, hog held 9764 MB, all back
  within ~30 s. No restore failed for lack of (contiguous) vidmem all
  session -> item 5 (non-contiguous restore of contiguous allocs) not needed
  so far. Reserve block lowest free since boot: 501 MB.
- CoD test 1 (02:20, build 04bfb834): played OK at first, then ghostty on
  the 2nd monitor showed garbage (likely reserve denials: it was active, so
  no sysmem for it), the game froze and crashed. Journal: evicting
  steamwebhelper (26 MB) took 3.3 s with Xid 109 CTX SWITCH TIMEOUT in the
  game: FIFO_DISABLE_CHANNELS with preemption forced the game's context off
  GR. Then the hung game looked idle and the policy evicted it (7.4 GB).
  Fixes: a2f64b27 never requests preemption beyond the process's own TSGs
  (scheduling off, per-TSG NVA06C_CTRL_CMD_PREEMPT, which GSP refuses with
  0x57 OBJECT_NOT_FOUND for these handles -> 20 ms drain wait instead);
  812d33ad: busiest process of the last 2 min is never evicted / never given
  sysmem; sysmem-instead-of-denial for every other non-desktop process.
  compr-test write mode, 15 evict/restore cycles without preemption: 0
  MISMATCH. CeUtils uses an async (non-GRCE) copy engine, so copies do not
  touch GR.
- CoD test 2 (02:33, build a2f64b27, auto on): no freezes, no Xid, no
  errors; evicted 2.5 GB (Chrome 1627, ghostty 338, Claude 281, Spotify 189,
  steamwebhelper 167, ...); ghostty/Claude got new allocations in sysmem;
  all restored ~40 s after the game exited. But 55-60 fps vs ~70 in test 1:
  Chrome (busiest before the game started) stayed protected by the 2-minute
  rule and was evicted only at 02:35:29, near the end. Fix 5f91ad3d
  (installed, not yet rebooted into): the previous busiest loses protection
  once another process has been the busiest for 15 s in a row.
- CoD test 3 (02:43, 5f91ad3d): Chrome evicted 4 s after pressure began,
  but the game got a 200 MB allocation in sysmem (Steam overlay's
  steamwebhelper often topped the GR samples, so the game lost protection):
  18 fps. Fix df0c9531: active process with >= 1 GB vidmem never gets sysmem.
- CoD test 4 (02:49, df0c9531): game again got 128 MB in sysmem 40 s in
  (its vidmem not measured yet by any tick; fix 75e7406c measures the
  caller's client at the denial). Each stop of a small process took ~300 ms
  (TSG preempt waiting its 200 ms timeout while the game holds GR; fix
  c9288817: 20 ms). Then < 1 fps and macro freezes: visible on the 2nd 4K@120
  monitor were Chrome fullscreen + ghostty (window buffers evicted, kwin reads
  them every frame over PCIe) and Steam playing a trailer got its new
  allocations in sysmem (the "everyone but the game" rule from 812d33ad).
  GR samples looked unreliable: "busiest" was Chrome/Claude most of the game.
- Fix (built, installed, not yet booted): window buffers stay in vidmem by
  default (`evict.sh windows on|off`), sysmem-instead-of-denial only for
  idle processes again (plus the >= 1 GB and busiest guards), status shows
  per-process GR sample counts.
- Results so far: test 1 ~70 fps (aggressive, before the crash), test 2
  55-60 fps (Chrome late), test 3 18 fps, test 4 < 1 fps.
- CoD test 5 (10:02, 3ae8eaaa; Chrome fullscreen + ghostty on the 2nd
  monitor, Steam playing a trailer): baseline with auto off 25-30 fps; GR
  samples do show the game as busiest (11 vs 2-4). Auto on: ~1.3 GB evicted
  (window buffers kept), then ~1 fps; steamwebhelper (trailer) evicted 7x in
  a minute. Manual restores at 10:06:40 -> 10:06:41 Xid 13 "Shader Program
  Header" in Chrome; 10:08:59 the same in claude-desktop (also restored at
  10:06:40). Re-enabled auto: Xid 31 in the game (CE3 FAULT_PDE) and Chrome
  (UNSUPPORTED_KIND write), then the hung game was evicted (7.2 GB).
  Auto off again, everything restored; build edd34fb7 (installed, not yet
  booted): auto OFF by default, >= 1 GB processes never evicted.

## Stage 2 status and conclusions (2026-11-02 10:15)

Works: manual eviction/restore of idle apps (compr-test, test Chrome, the
desktop under vram-hog), sysmem-backed allocations, the reserve block.
Not safe yet for automatic use during a game, four open problems:
1. Quiescing a process. FIFO_DISABLE_CHANNELS with preemption hung the game
   once (Xid 109); without preemption (scheduling off + 20 ms) a process
   that was still running lost writes (Xid 13 shader corruption in Chrome
   and Claude). The targeted NVA06C_CTRL_CMD_PREEMPT via RPC returns 0x57
   OBJECT_NOT_FOUND for every TSG tried (implicit TSGs are allocated with
   RMAPI_ALLOC_FLAGS_SKIP_RPC, kernel_channel.c:379; explicit ones unclear).
   Options: find the TSG handle GSP knows; wait for GP_GET == GP_PUT on each
   channel's USERD before copying.
2. Holders in other processes. Since evicted memory stays usable, others can
   dup and map it (the game imports Steam overlay buffers); a restore remaps
   those mappings while only the owner (and kwin) are paused -> likely the
   Xid 31 FAULT_PDE in the game. Fix: pause every holder with a mapping, or
   restore on dup into a non-compositor process.
3. Hotness. GR samples miss light-duty apps: Chrome fullscreen, Claude, a
   Steam trailer look "idle" while rendering; from sysmem their GR
   timeslices stretch and the game starves (1 fps). Needs real access data:
   HW access counters (NVC365 AccessCounterBuffer, Volta+) count GPU accesses
   to sysmem per region and notify; to research: usable for graphics VA
   spaces, controllable through GSP from RM?
4. Game identity: busiest-by-samples flips (overlay, Chrome); size-based
   (>= 1 GB) protection now used for both eviction and sysmem allocations.
Stage 0 (reserve for kwin) is unaffected and stays on.

## Controlled experiment: why eviction lowers fps (2026-11-02 10:20)

Auto off, manual evictions one at a time while playing CoD (Chrome fullscreen
+ ghostty on the 2nd 4K@120 monitor, Steam playing a trailer); 1 s
`nvidia-smi dmon -s ut` + `pmon -s u` (local/stress-logs/evict-experiment-1).
- Baseline: 30-40 fps; SM 99% (game 86-87%, kwin 14%); PCIe rx 2-8 GB/s,
  tx 1-4 GB/s: the game itself already streams from its ~5-10 GB in sysmem.
- Spotify (idle, 148 MB): fps 20-25 for a few seconds, then 30. Cause: the
  game grabbed the freed memory and uploaded textures (rx burst 12.5 GB/s 8 s
  later), not the operation itself.
- Steam webhelper (trailer, 172 MB): 25-30 fps; webhelper rose to 25% SM
  (was not even visible in pmon), game 87 -> 76%; PCIe +1 GB/s each way.
  Later Xid 13 "Illegal Instruction Encoding" in it (evicted while drawing).
- Chrome (548 MB, window buffers kept): 0 fps. Chrome 75% SM, game 27%;
  PCIe GPU->host 14-26 GB/s, host->GPU up to 19 GB/s (link saturated).
  Restored -> Chrome rendered garbage (evicted/restored while drawing, no
  preemption: lost writes).
Conclusion: an app that keeps rendering must never be moved to sysmem; its
render targets/tiles then cross PCIe every frame, saturate the link and
stretch its GR time slices, starving the game. Eviction only pays for apps
that do not touch the GPU at all, and the quiesce must be safe (preemption
of the process's own TSGs), which we do not have yet.

## Agreed direction (2026-11-02 10:30): a driver that does it right

The user wants a proper driver solution (no userspace workarounds such as
Chrome GPU-memory flags or VKD3D settings; those were rejected), possibly
with small WM (KWin) help for hints.

Why the current automatic policy fails (see experiment above):
1. Wrong activity signal: GSP GR samples miss light-duty renderers (Chrome
   animations, a trailer), and those must never be evicted.
2. Unsafe quiesce: global preemption hangs the game (Xid 109), no preemption
   corrupts the evicted app (Xid 13, garbage in Chrome).
3. No notion of what the user sees: a visible window must stay in vidmem.
4. No feedback when evicted memory turns out to be hot.
5. Other processes holding the buffer (game importing Steam overlay
   buffers) are not paused during a remap.

Architecture:
A. KWin hints. A small KWin C++ plugin reports per process: `foreground`
   (active window / fullscreen game), `visible` (at least partly shown),
   `hidden` (minimized, occluded, other desktop) through a procfs file
   writable by the session user. Driver: evict only `hidden`; `foreground`
   is the game (no sample heuristics); when a window becomes visible,
   restore its memory proactively before it draws from sysmem. KDE's
   plasma-foreground-booster (dmem.low on app-*.scope cgroups) could be a
   second source.
B. Real activity = work submission. Every channel's USERD has GP_PUT; RM
   knows each channel's USERD and can check once a second whether GP_PUT
   moved. Any submission in the last N s = active (catches Chrome
   animations and video, unlike the GR samples).
C. Safe quiesce. Preempt only the process's own TSGs. The RPC
   NVA06C_CTRL_CMD_PREEMPT returns 0x57 OBJECT_NOT_FOUND today: implicit
   TSGs are created with RMAPI_ALLOC_FLAGS_SKIP_RPC (kernel_channel.c:379),
   so GSP likely knows them under another handle; find how RM itself
   addresses them. Fallback: after disabling scheduling, wait until GP_GET
   catches up with GP_PUT on every channel (work really drained) instead of
   a blind 20 ms.
D. Access counters (later): NVC365 AccessCounterBuffer counts GPU accesses
   to sysmem per region; restore evicted memory that turns out to be hot.
E. Pause every holder with a mapping during evict/restore, not only the
   owner and kwin.

Order of work:
1. C (blocker: any eviction risks corruption without it). Research first:
   which TSG handle GSP knows; whether GP_GET/GP_PUT can be read from RM.
2. B (activity by GP_PUT), fixes the main selection error quickly.
3. A (KWin plugin + hint interface + proactive restore on show).
4. E, then D.

State at this point: build edd34fb7 installed and booted, automatic policy
OFF by default (`evict.sh auto on` enables it at runtime), window buffers
kept in vidmem by default, manual evict/restore/status/shared/trace work,
stage 0 reserve on. After the experiment Chrome showed garbage (needs
chrome://restart) and steamwebhelper had an Xid 13.

- Remaining also: block size decision (needs a day of data); then step 2: allocations the reserve would deny are created
  already evicted (in sysmem) instead of failing (WDDM-like), first for
  background apps, then games.

## Test bench and first results (2026-11-02 10:35-11:20)

Goal set by the user: real-time switching between apps, fast; Claude tests
without the user. Built (commits 3afaaa5c, f884fa70, 1d8e46b4):
- `local/nvt-game` (Vulkan): holds N MB with a position+generation pattern,
  frames check it (compute, or `--gfx` full-screen draw), `--write` rewrites
  with the next generation every frame (lost writes are caught), `--idle`
  apps, fps/frame-time/errors per second, RESULT line. Named by symlink.
- `local/nvtest.sh integrity|active|quiesce|game`: synthetic scenarios with
  kernel-log checks and a verdict line; runs from ssh at the SDDM greeter
  (no login needed). `game` only when nobody is on seat0.
- `local/nvtest-root.sh stage|reload|promote|status`: candidate slot
  `/var/lib/nvidia-local/candidate` loaded only by hot reload (stop SDDM,
  rmmod, insmod, start SDDM); a hang resets via watchdog into the installed
  build. Needs the one-time `local/nvtest-setup.sh` (watchdog, sudoers rule,
  slot dir, groups render+video) run by the user:
  `ssh -t spoki4-pc "sudo ~/projects/open-gpu-kernel-modules/local/nvtest-setup.sh"`.
  NOT RUN YET at 11:20.
- Driver knob `evict.sh quiesce 0..4` (stop method) and per-evict report:
  quiesce stats (TSGs, implicit, preempt ok/refused, idle status/time) and
  phase times (quiesce, sysmem alloc, copy, remap, pma, sysmem free).

Findings:
- Speed: CeUtils copied page by page (4K for sysmem: GPFIFO entry + doorbell
  each, ~3.4 us) -> 1.1 GB/s. Fixed in ce_utils.c (_ceutilsContigRun merges
  PTE-contiguous pages): 1 GB evict 806 -> 119 ms, copy ~20 GB/s (PCIe line
  rate). Left: 20 ms blind drain (mode 0) and ~40 ms/GB sysmem allocation
  (-> preallocated pool).
- GSP refuses NVA06C_CTRL_CMD_PREEMPT (0x57) for ALL TSGs of a Vulkan app,
  explicit ones too. Implicit TSGs: physical RM clears hPhysChannelGroup
  (kernel_channel.c:215) and names them itself.
- Quiesce 2 (scheduling off + IDLE_CHANNELS RPC) stops in 1-4 ms; modes 0
  and 2 kept content correct for compute and graphics apps that rewrite
  their memory every frame (6-8 cycles each). The 10:46 hang of a 50 ms/frame
  app (desktop session active) did not reproduce at the greeter.
- Chrome/Spotify Xid 13 (Misaligned Register = bad shader code) from the
  10:20 experiment is NOT reproduced by the synthetic apps: next is a real
  Chrome in a headless compositor with screenshot comparison. Headless
  Chrome has no GPU (gl=none); kwin_wayland --virtual picks the AMD iGPU
  unless its nodes are hidden (bwrap --bind /dev/null /dev/dri/renderD129
  ... card0) and needs the render group without a seat0 session.
- A heavy graphics app running from sysmem holds GR with long draws: a
  60 fps neighbour drops to 12 fps (81 ms frames). Same starvation as in the
  CoD tests: evicting an app that keeps drawing is never acceptable; the
  activity signal (B) and KWin hints (A) stay required.

Next: sysmem pool; Chrome headless test; quiesce 2 as default after real
apps pass; KWin hint plugin; proactive restore on show.

## Real-time switching with KWin window hints (2026-11-02 11:40-12:40, build de730738, installed)

Goal (user): the system switches between apps in real time and fast, not
just evicts without breaking things. Done, all on the bench first.

Pieces:
- `local/kwin-vram-hints/` KWin QML script (no build; `install.sh` per user,
  no root; needs QML_XHR_ALLOW_FILE_WRITE=1 in KWin's env, set via
  ~/.config/plasma-workspace/env/ and environment.d; takes effect at the
  next login). Per window owner pid: f (active), v (visible), h (minimized,
  other desktop/activity, or fully covered by an opaque window above, e.g.
  behind a fullscreen game). Publishes on activation/add/remove, polls every
  250 ms, writes `/proc/driver/nvidia/vidmem_hints` (0666; one open = one
  snapshot; only pids of the writer's uid).
- Driver: os_get_vidmem_hint() walks parents (Chrome/CEF GPU processes take
  the browser's state; bit "other app" when the parent has another name, a
  game started by Steam gets a 2-minute grace while it has no window).
  A write runs the policy at once in a kworker (rm_vidmem_evict mode 9).
- Policy with hints (auto default "hints": runs only while hints exist):
  f/v never moved out; f/v with memory in sysmem restored at once, hidden
  ones making room; under pressure (< 256 MB free outside the reserve)
  hidden ones out (idle first, still-drawing ones later, windowless idle
  last; hidden longest first). Partial eviction: only what is missing
  (+64 MB) moves out of a victim.
- Budget: a visible process's HEAP_FREE (its Vulkan/DXGI budget) includes
  what hidden processes could give up. Key finding: GL/Vulkan drivers and
  games never allocate beyond the reported budget (they spill to sysmem
  themselves, as CoD did at 2-8 GB/s), so without this nothing would ever
  ask for the hidden apps' memory.
- vidmemEvictMakeRoom(): an allocation of a visible process the reserve
  would deny moves hidden processes out inside vidmemConstruct (takes the
  device GPU lock itself; PMA allocates before vidmemConstruct takes it).
- Speed: CeUtils page merging (1.1 -> ~20 GB/s copies), quiesce 1 (1-3 ms),
  one L2 writeback/evict per process batch with deferred sysmem frees.
- Quiesce 1 (FIFO_DISABLE_CHANNELS with preemption) is the default: modes
  0/2/3/4 let a running draw see its memory move (Xid 31 on the bench).
  The 02:21 Xid 109 in CoD with mode 1 was not reproduced by the bench.

Bench results (`nvtest.sh all`, all PASS): integrity (evict 12.4 GB/s,
restore 17 GB/s), active (writes in flight, no Xid), chrome anim/idle
(pixel-exact), switch (fullscreen 10 GB "game" Chrome B + hidden Chrome A:
alt-tab both ways ~0.09-0.10 s from the KWin command until the shown app
is fully in vidmem, driver ~60 ms; B 60 fps), game (foreground got 9531 MB
vs 7936 MB without eviction).

Known limits / watch in the real test:
- A hidden app that keeps drawing while moved out holds GR with slow draws
  (bench: neighbour frames up to ~250 ms). Such apps are picked last, but
  X11 apps (Steam) do not stop drawing when covered.
- Window buffers (shared with kwin) stay in vidmem (windows off).
- If the game's window pid is not the process holding its memory, it is
  "unknown": no extra budget, but it is never moved out while active.
- Check after login: `local/kwin-vram-hints/install.sh status` (kwin env,
  what the driver received), `sudo local/evict.sh status` (window column),
  `journalctl -k | grep "vidmem evict: hints"`. Off switch:
  `sudo local/evict.sh auto off`; script: `install.sh remove`.

## Foreground priority, WebGPU, 615 port (2026-11-02 12:40-13:10)

- CoD on the real desktop with hints (12:39): the game was recognised as
  foreground (window pid = sp26-cod.exe = the memory holder), hidden apps
  moved out inside its allocations (Spotify, Claude, Steam: 212 MB, 3-18 ms
  each), no Xid; but 30 fps with Chrome/ghostty/Steam visible on monitor 2,
  60 fps once closed: visible windows were never touched.
- Fix (6deefd7b, installed): victim levels. For the foreground process,
  visible apps idle on the GPU >= 10 s give up memory too (budget,
  allocations, and pressure while a >= 1 GB foreground game fills vidmem;
  GL apps never ask beyond the budget, so pressure must act); visible ones
  still drawing last, only for the foreground's allocations/restore. A
  visible app moved out for the foreground returns when activated, or after
  60 s if there is room without moving anything.
- Bench: `nvtest.sh fgprio` (two virtual monitors) and `nvtest.sh webgpu`
  (nvt-webgpu.html, Dawn/Vulkan, 512 MB checked+rewritten per frame; a new
  tab must still get an adapter). Full `nvtest.sh all`: 8/8 PASS.
- Branch local/vidmem-615 (worktree ~/projects/ogkm-615 on the PC): the
  patch on 615.71.09 (Ubuntu ships nvidia-driver-615-open 615.71.09),
  builds; 2 conflicts resolved (HEAP_FREE via memmgrGetHeapFreeBytes, static
  BAR1 refcount under kbusShouldRefcountConstruct); upstream build fix for
  Linux 7.0 (struct nv_cxl under NV_CXL_INIT_SUPPORTED). Not installed: the
  615 userspace must come first (`sudo apt install nvidia-driver-615-open`,
  by the user), then switch the main checkout to local/vidmem-615 (sudoers
  paths point there), build, install.sh install, reboot, run the bench.
- User asked about moving to CachyOS: answered (possible later; tooling is
  Ubuntu-specific, the patch and KWin script are not); no decision yet.

## CoD 1 fps, GP_PUT activity, hot memory start (2026-11-02 13:15-13:50)

- CoD test 13:15 (build 6deefd7b): 1 fps. Chrome on monitor 2 was drawing
  (busiest) but the game's allocations took 831 MB of it and 186 MB of the
  Steam window (rule "visible drawing last" at level FG): they drew from
  sysmem, PCIe 21 GB/s, SM 100%. Also a quiesce of ghostty took 294 ms.
- Fixes (4fc2f2d0): activity also from GP_PUT changes per process (USERD
  0x8C read every policy step; the GPUMON samples name one process per
  sample, so a light Chrome next to a game never shows); drawing processes
  are never taken for streaming (only to restore the foreground's own
  memory); hidden ones only if not drawing now (4 s, after they were
  hidden); a moved-out process drawing again comes back first. KWin script:
  union occlusion (Steam under several windows).
- User idea: Chrome tabs / hot vs cold inside a process. Plan: access
  counters (NVC365). Module in vidmem_evict.c, knob `evict.sh hot on|off`
  (default off). Findings on 595/GA102: the buffer can be created inside RM
  (internal client, 4096 entries, MIMC 2 MB, threshold 256) and ~3000
  notifications arrived within seconds, with aperture 0 = VIDMEM regions
  (so hot/cold of resident memory is visible, the base for per-tab /
  per-allocation decisions); BUT kfifoConvertInstToKernelChannel fails for
  every entry (under GSP on 595 the instance blocks are GSP's; 615 moved
  RAMFC/instance setup to CPU-RM, retry there), and after the first
  RESET_COUNTERS no further notifications came, none for sysmem.
  Next: decode MIMC vs MOMC (which one sees sysmem), buffer full/overflow
  handling, inst->channel on 615 (or match by VA alone + owner pid), then
  per-allocation cold eviction for visible processes.

## Outside research (2026-11-02, from another session, partly unverified)

Nobody published a kwin reserve or cross-process eviction for NVIDIA.
- Kai Krakow's fork https://github.com/kakra/ogkm-experimental (branch
  ogkm/615.78.08): scanout GEM alloc retried without compression on
  fragmentation (c43c0bfc), per-client exponential back-off for repeated
  failing scanout requests (207e7530, Chrome retried every frame).
- NVIDIA 610.43+/615 charges vidmem to the caller's dmem cgroup
  (memacct.c, region nvidia/<pci>/vidmem; only dmem.min honoured, dmem.max
  hard limit, no kernel eviction). Natural integration point on a rebase.
- Mainline (Linux 7.3, TTM only): dmem protection lets protected allocations
  evict unprotected ones; KDE plasma-foreground-booster sets dmem.low on the
  focused app-*.scope.
- WDDM: background budget shrunk and announced before eviction; residency
  priorities inside a process.
Ideas: identify apps by cgroup (app-*.scope) and honour dmem.low so KDE's
booster drives the policy; shrink background budgets (HEAP_FREE) before
evicting; xdg_toplevel "suspended" as idle signal; back-off for repeated
failing scanout requests. Without code: VKD3D_CONFIG=no_upload_hvv for CoD.

## Next steps (written 2026-11-01, after the census)

In priority order. Steps 1–3 need no kernel work and decide whether stage 2
is needed at all.

1. **Stage 0 in the real game.** DONE 2026-11-01, see "Game test" above.
   Repeat with vidmem_pma sampling if a freeze is ever seen again.
   - Open the usual apps (≈4.8 GB used), start the game, play until VRAM is
     near full. On the PC: `journalctl -kf | grep -E 'vidmem user reserve|NVRM'`
     and `nvidia-smi -l 2`; census the game process (`census.sh <pid> --summary`).
   - Pass: desktop stays responsive (cursor, alt-tab, kwin repaints), no
     `Failed to allocate NVKMS memory` / kwin gbm errors, no hard reset.
   - Also record what the game does when denied: vkd3d may crash on
     `VK_ERROR_OUT_OF_DEVICE_MEMORY` or just stream lower textures. A crashing
     game is acceptable (better than a frozen desktop) but worth knowing.
   - If kwin still starves, raise the reserve: `RESERVE_MB=1024`.
2. **Why each Chrome window buffer is 66 MB** (18 of them = 1.2 GB). 4K RGBA8
   would be ~33 MB; HDR and WCG are off, scale 1.4 on both outputs.
   - Count Chrome windows: 18 = windows × buffers per window?
   - Measure with one window, then minimize it, then resize it: census
     before/after shows whether buffers follow window size and are freed on
     minimize.
   - Find format/modifier: `chrome://gpu` (buffer formats, Wayland/Vulkan
     info), or `WAYLAND_DEBUG=1` on Chrome and look at
     `zwp_linux_buffer_params_v1.add/create` (fourcc, modifier, width, height).
   - Suspects: an fp16/10-bit format chosen by Chrome, block-linear
     alignment, or a buffer sized for the whole 2-monitor area.
3. **Chrome-side knobs** (keep HW acceleration and WebGPU). Unverified flags
   to try, one at a time, with census before/after:
   `--force-gpu-mem-available-mb=<N>`,
   `--force-gpu-mem-discardable-limit-mb=<N>`; tab discarding
   (`chrome://discards`). Goal: shrink the 1.8 GB of compressed render
   targets (raster tiles?) without patching the driver.
4. **Better census** (small, read-only, safe): for `shared` allocations print
   which other clients/PIDs hold the dups (to tell "exported to kwin" from
   "duped inside Chrome"); decode `kind` names; count compression tags. Helps
   steps 2 and 5.
5. **Decide on stage 2.** Only if 1–3 leave a real problem. The candidate
   bucket is too small (~180 MB total); the only worthwhile target is
   `compressed` (1.8 GB in Chrome, single process, no kwin involvement).
   Desk research first, no code:
   - Does a CE copy through a compressed-kind mapping return decompressed
     data on GA102 (including ZBC fast-cleared tiles)? Look at how RM itself
     decompresses: `memmgrGetUncompressedKind_HAL`, CeUtils, the
     sysmem-compression checks around `bSysmemCompressionSupportDef`.
   - After switching the app's PTEs to the uncompressed kind, is anything in
     userspace (Vulkan/GL driver) still assuming compression (fast clears,
     decompress-in-place, comptag-based ops)? If yes, eviction breaks
     rendering.
   - Restore can be uncompressed (lose bandwidth, keep correctness).
   Window buffers (`shared`, owned through nvidia-drm/nvkms, mapped by kwin)
   are a later, harder target: only for minimized windows, if at all.
6. **Safety net before any stage 2 code** (it will hang the PC):
   netconsole to the Mac; hardware watchdog (`sp5100_tco`) plus
   `kernel.panic=10`, `kernel.softlockup_panic=1`, `kernel.hung_task_panic=1`
   so a hang reboots by itself; check kdump actually captures; a remote boot
   fallback (e.g. `grub-reboot` one-shot into the patched entry with the
   stock kernel as default) so a module that breaks boot does not need
   someone at the keyboard.

## Stage 2/3 plan (not started) — technical template, see "Next steps" first

Evict a chosen idle process: disable its channels (pattern:
`nvGpuOpsDisableVaSpaceChannels`, `src/nvidia/src/kernel/rmapi/nv_gpu_ops.c:929`,
RPC to GSP), L2 flush, for each candidate allocation: alloc sysmem in 64K/2M
chunks, CeUtils copy, rewrite PTEs with `dmaUpdateVASpace_HAL` +
`DMA_UPDATE_VASPACE_FLAGS_ALLOW_REMAP` (template:
`src/nvidia/src/kernel/gpu/bus/arch/turing/kern_bus_tu102.c:805`), swap memdescs
in `Memory` and `CLI_DMA_MAPPING_INFO`, free PMA pages, TLB invalidate, re-enable.
Restore explicitly (graphics page faults are fatal — no lazy fault-in).
Constraints: GA102 cannot keep compressed data in sysmem
(`bSysmemCompressionSupportDef`); skip shared, CPU-mapped, USERD/notifiers,
and the GSP-owned VA window 4–4.5 GB (`SPLIT_VAS_SERVER_RM_MANAGED_VA_*`).
Test on vkcube first, then a throwaway Chrome profile.

Stage 2 will hang the PC during development: work from the Mac, keep
netconsole (to be set up) streaming kernel logs to it, use tmux on the PC.

## Rules the user set

- Patch lives on the fork branch; commit with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `/etc/sudoers.d/nvidia-local` (2026-11-01) lets `spoki4` run without a
  password: `local/install.sh install`, `local/rollback.sh`,
  `local/census.sh`, `/usr/bin/systemctl reboot` — call them with absolute
  paths and `sudo -n`. Claude installs, reboots and tests on its own without
  asking ("доделывай до конца"). Anything else needing sudo (incl.
  `enroll-key`) is run by the user in a real terminal: `!` in Claude Code has
  no TTY, so sudo-rs cannot prompt there.
- After a reboot the PC sits at the SDDM greeter; the user logs in (no
  autologin: it would leave KWallet locked, which Chrome/Discord need).
  `~/.config/autostart/local-*.desktop` then start Chrome
  (`--restore-last-session`), Telegram (`-startintray`; `-autostart` makes it
  quit when its own autostart setting is off), Spotify, Steam (`-silent`),
  Discord (`--start-minimized`), ghostty; Claude has its own entry. Wait
  ~1 min after plasmashell appears before measuring.
- Keep Chrome hardware acceleration on (user needs WebGPU). Discord's HW
  acceleration was disabled (backup `~/.config/discord/settings.json.bak-before-hwaccel`).

## Real-life scenarios in the VM guest (2026-11-04, local/scenario/)

User's goal: test like real life (play, die, Alt+Tab to Chrome, browse, back)
and with real game benchmarks. In the guest: Steam logged in by the user,
installed Black Myth: Wukong Benchmark Tool (3132990) and The Alters
Benchmark Tool (3787490) (MH Wilds benchmark 3424520: "No licenses"),
Unigine Superposition in ~/bench/superposition, mangohud, python-evdev,
spectacle, ydotool (unused: its absolute moves do not work under KWin).

```bash
local/vm/vm.sh ssh 'cd ~/projects/open-gpu-kernel-modules/local && ./watchdog.sh --stall 180 \
  --on-kill "python3 scenario/scenario.py stop" -- python3 scenario/scenario.py run death \
  --cycles 2 --play 30 --browse 20 --bg-fill 1500'
```

Pitfalls found the hard way:
- The KWin hints script must be installed in the guest session
  (kwin-vram-hints/install.sh + `sudo systemctl restart sddm`); without it the
  policy is off and a game is just denied memory. scenario.py checks it.
- Superposition free edition: only the performance run and "game" mode; its
  fullscreen is an unmanaged X11 window (always on top, grabs the keyboard,
  Alt+Tab cannot leave it; F2 = wireframe). Run as borderless 4K window.
- MangoHud writes a log only when logging stops (log_duration or the hotkey,
  here Ctrl_R+Shift_R+Scroll_Lock); its NVML thread takes the RM API lock
  (kwin waited 41 ms for it): gpu_stats=0.
- `pgrep -f`/`pkill -f` patterns match the command line that contains them
  (killed my own ssh twice, found the watchdog instead of the game): use
  `scenario.py stop` / `pgrep -x`.
- A pressure run is valid only with evictions in the report ("INVALID"
  otherwise); a windowless background process with >= 1 GB counts as a game
  (VIDMEM_EVICT_GAME_BYTES) and is never moved out, so --bg-fill uses apps of
  <= 900 MB, idle (memory check every 30 s, idle_s must reach 10).

Bug found and fixed (4b2a1bc2): restored PMA pages were unpinned -> the next
allocation over them called the NULL UVM eviction callback, guest oops.

Policy findings of the first valid run (scenario-logs/death-20261104_140517),
not fixed yet:
1. A minimized launcher "drawing again" ping-pongs (6 evictions, 5
   restores) and its restore takes room from the game while the game is
   hidden: 241 MB of the game moved out at the switch back, 215 ms frame,
   one failed restore of the game.
2. Windowless >= 1 GB processes are never evicted (an LLM server would starve
   a game: the game ran at 0.8-8 fps next to 5.8 GB of idle memory).
3. RM lock waits up to 192 ms during evictions (policy step holds the API and
   GPU locks for the whole batch).

## Frame-time benchmarks and the eviction goal (2026-11-04, local/bench/)

User's goal: with other apps and eviction, the game within 10% of the clean
FPS. Tooling: `local/bench/bench.py run|analyze|compare` (MangoHud every
frame + Perfetto v58.2 system trace, local/.perfetto/, user-approved;
report: compare-*.html). Superposition performance run, 8K Optimized
(~6.7 GB), 90 s logged stretch; "apps" = Chromium (YouTube, Wikipedia,
WebGL) + background apps sized so the game is --bg-fill MB short.

| run | fps | 1% low | stutters |
|---|---|---|---|
| clean (patched) | 52.3 / 52.0 | 34.7 / 34.3 | 0 |
| stock-clean | 52.1 | 34.0 | 0 |
| apps-only (no deficit) | 50.0 | 34.7 | 0 |
| apps, 1.5 GB short, stock driver | 5.8 | 1.2 | 113 |
| apps, 1.5 GB short, before 9adf94bb | 7.7-8.8 | 1.3 | 124-167 |
| apps, 1.5 GB short, 9adf94bb | 49.9 / 49.9 | 34.8 / 33.9 | 0 / 1 |

What it took (9adf94bb): KWin hints recognise fullscreen unmanaged X11
windows; windowless idle apps are victims for the foreground when not
drawing; foreground headroom (RmVidmemFgHeadroomMB 2048): GL places
textures by the vidmem actually free at load time, not by HEAP_FREE.
nvtest all: 10/10 PASS on this build.

MangoHud: autostart_log does nothing with no_display (invisible HUD
instead); with Chromium visible the compositor flips every refresh, so the
frame/trace alignment is coarse (engine start + autostart delay).
Next: the minimized launcher ping-pong (one 42 ms stutter in the second
apps-evict run), lock holds of whole eviction batches, Wukong / The Alters
(DX12 via Proton) in bench.py, final numbers on bare metal.

Wukong (bench.py run wukong, 68b6dace; DX12 via Proton, 4K Very High, DLSS
50, ~5.6 GB), same conditions (Chromium + background apps 1.5 GB short):

| run | fps | 1% low | notes |
|---|---|---|---|
| wk-clean | 52.4 | 41.2 | |
| wk-apps-evict | 50.45 | 38.0 | -3.7%; 3.2 GB moved out by the foreground headroom while it loaded |
| wk-apps-noevict | 28.8 | 14.2 | 273 reserve denials |
| wk-stock-apps | 1.7 | 0.8 | stock driver: hung at the loading screen, ctxBufPoolReserve NO_MEMORY (the 2026-10-31 freeze signature) |

The stutter at ~67 s of the logged stretch is in the scene (clean too).
Wukong's first start (shader compilation, language, brightness, a data
collection prompt: declined) was done once by hand.

## No reserve: priority for what the user works with (2026-11-04 evening)

User's decision: drop the fixed VRAM reserve (768 MB) and protect the desktop
by priority instead: the compositor gets memory when it needs it, the
processes the user interacts with come first, everything else is slowed
down carefully (moved to sysmem), not denied. Default now (guest.sh /
install.sh `RESERVE_MB=0`); `RESERVE_MB=768` brings the old block back (that
code path is unchanged).

How it works (video_mem.c header comment, vidmem_evict.c):
- Placement: without the reserve every non-contiguous PMA allocation goes
  top-down (`vidmemPmaTopDown()`, hook in `pmaAllocatePages()`), contiguous
  ones (scanout buffers, cursors) stay bottom-up. Nothing is held back.
- The compositor (kwin_wayland, Xwayland, kernel threads; nvkms allocations
  inside kwin's ioctl count as kwin by thread name) is never turned away:
  when PMA fails its allocation, `vidmemEvictMakeRoomCompositor()` moves
  others out (`VE_LEVEL_COMPOSITOR`: hidden idle first ... the foreground
  game last) and the allocation is retried. Accounting is in free 2 MB
  pages (physically 2 MB aligned: PMA region 0 starts at 0x37c0000), not
  free bytes: 64 KB holes are useless to a 2 MB-page allocation. A
  contiguous one (scanout: nvkms-kapi asks CONTIGUOUS + ISO) gets a window
  cleared: `_vidmemEvictMakeContig()` marks the frames every victim could
  give up, picks the aligned window with the least to move, and evicts only
  the allocations in it (`_vidmemEvictRangeLo/Hi` filter in
  `_vidmemEvictPidLocked()`).
- Background processes (hidden, or windowless and idle, not the game, not
  >= 1 GB, not the foreground of the last 30 s) may not take the last
  `RmVidmemBgCushionMB` (256): below it their new allocations are created in
  sysmem; their HEAP_FREE (Vulkan/DXGI budget) is that much lower too. If
  sysmem is impossible for the allocation (contiguous, too big, drawing),
  it goes to vidmem as before.
- Visible / foreground processes: room made as with the reserve
  (vidmemEvictMakeRoom, foreground headroom 2048 MB).
- "System UI in focus" needs nothing extra: plasmashell / KRunner popups
  are the active window, so the KWin script reports them as the foreground;
  kwin's own UI (Alt+Tab, overview) allocates as the compositor.

Ping-pong fix (the minimized launcher, death-20261104_140517):
- "Drawing again" restores only a process that has drawn at every policy
  step for `VIDMEM_EVICT_STEADY_SEC` (6 s), not one frame now and then.
- A hidden one is restored only into memory that is free anyway; a visible
  one gets room made by idle hidden processes (margin 64 MB, not 256).
- A process that was the foreground in the last 30 s
  (`VIDMEM_EVICT_RECENT_FG_SEC`) is not a victim at `VE_LEVEL_HIDDEN` (for
  pressure, other visible apps, hot restores): the game the user just left
  or is switching back to keeps its memory (241 MB of the game had moved
  out for the launcher's 21 MB at the switch back, a 215 ms frame).
- Order of a policy step: foreground restore, visible restore, then
  "drawing again", then hot allocations, then pressure.

Status line (`evict.sh status`): "no reserve: largest contiguous free,
background cushion, room made for the compositor N times (failed), MB
moved, longest".

Results without the reserve (build a32843c5+, same conditions as above:
Chromium + background apps sized so the game is 1.5 GB short):

| run | fps | 1% low | stutters | with the 768 MB reserve |
|---|---|---|---|---|
| nr-clean (Superposition) | 52.35 | 33.8 | 0 | 52.3 / 52.0 |
| nr-apps-evict | 50.02 (-4.4%) | 34.5 | 0 | 49.9 |
| wk-nr-clean (Wukong) | 52.7 | 41.0 | 1 (the scene, 67 s) | 52.4 |
| wk-nr-apps-evict | 50.57 (-4.0%) | 37.2 | 2 (scene + 46 ms at 5.7 s, no driver activity) | 50.45 |

stress.sh scanout without the reserve: 5/5 PASS, 6/6 4K scanout buffers
for a "kwin_wayland" probe with vidmem filled to 2-50 MB free, also while a
hog re-allocates at the limit with 4/8 threads. The bugs it found on the
way: free bytes in 64 KB holes are no help to 2 MB pages; PMA region 0 is
not 2 MB aligned (0x37c0000); gbm-hog's "sysmem fallback" heuristic
mistook room being made for a fallback (off for --scanout now).

Death scenario (death-20261104_185602, 2 cycles, alt-tab to Chromium):
launcher 1 evict / 1 restore (was 6 / 5), the game was never moved out at
a switch back (was 241 MB). The second switch back still has one 285 ms
flip gap with no driver activity in that phase; the reserve build had
291.7 ms there too (death-20261104_135315): the scenario, not the policy.

## Windows reference on the same GPU (2026-11-04 evening, vm.sh win)

`local/vm/win.sh` (through `vm.sh win ...`, same sudoers rule): Windows 11
Pro 26300 on the same 3080 Ti, 16 vCPUs on CCD0, 24 GB; never at the same
time as the Linux guest. Disk /var/lib/nvidia-vm/win/disk.img (sparse, 200
GB), local user `bench` with autologon, password and ssh key in
~/.local/share/nvwin (installers cached in dl/). Installed: NVIDIA 617.42
(no 615 on Windows), Superposition 1.1, Steam (the user's account,
invisible), Wukong Benchmark Tool + DirectX June 2010 and VC++ 2022 redist
(the tool refused to start without the DirectX runtime). The firewall prompt
of b1 was cancelled. Pitfalls: Setup reads autounattend.xml only from
removable media (usb-storage removable=on); `win run` = the job runner in
the desktop session (ssh is a service session); Windows ignores the ACPI
power button: stop shuts down from inside; the emulated VGA exists only
during `create` (screendump), afterwards the monitor is the only screen.

`local/bench/winbench.py run superposition|wukong --label L`: Superposition's
engine with the exact Linux arguments (OpenGL, 8K Optimized, 4K output);
Wukong with the Linux settings files copied in, the same clicks. Frames:
PresentMon (time between presents), the same metric functions as bench.py;
the stretch is placed like bench.py's (Superposition: 3.3 s after the
loading ends, measured on Linux by GPU busy; Wukong: 20 s after Confirm).
Monitor at 120 Hz there (144 on Linux): no effect at ~50-60 fps, vsync off.

| clean run | Linux (patched, no reserve) | Windows 617.42 |
|---|---|---|
| Superposition (OpenGL) avg fps | 52.35 | 51.41 / 51.39 |
| Superposition 1% low / P99 | 33.8 / 27.8 ms | 43.3 / 22.5 ms |
| Wukong (DX12; Linux: Proton/vkd3d) avg fps | 52.7 | 61.08 / 60.83 |
| Wukong 1% low / P99 | 41.0 / 22.5 ms | 42.9-48.0 / 19.5 ms |

Superposition: the same median frame (19.3 vs 19.4 ms); Linux has a tail
(2% of frames > 25 ms vs 0.06%), hence the lower 1% low: not looked into
yet (MangoHud vs PresentMon measure differently; or GL frame pacing).
Wukong: Windows ~16% faster, the vkd3d-proton translation cost, not the
driver patch (the patched clean run equals the stock one, 52.1 vs 52.3).

### Apps + 1.5 GB deficit on Windows (2026-11-04 late evening)

`winbench.py run GAME --apps --bg-fill 1500`, bench.py's scenario: Edge
(Chromium) with the same three tabs (YouTube, Wikipedia, nvt-webgl), then
idle windowless processes holding VRAM (nvt-idle.exe: local/vm/win/nvt-idle.cs,
Vulkan through P/Invoke, built in the guest with .NET Framework's csc; it fills
its buffers once) so the game is 1.5 GB short of its own peak on Windows
(WIN_VRAM_MB: Superposition 6218 MB, Wukong 5113 MB from the clean runs'
nvidia-smi log; Linux: 6700 / 5600). Background total 5.5-5.7 GB / 6.7-6.8 GB,
as on Linux (5.5 / 6.4). Every run now logs nvidia-smi (nvidia-smi.csv) and
dedicated/shared VRAM per process (gpumem.csv: vm/win/gpumem.ps1, Windows'
"GPU Process Memory" counters).

| avg fps (1% low) | Linux clean | Linux apps | | Windows clean | Windows apps | |
|---|---|---|---|---|---|---|
| Superposition | 52.35 (33.8) | 50.02 (34.5) | -4.4% | 51.38 (43.3/42.9) | 50.47 / 50.40 (42.3/42.1) | -1.8% |
| Wukong | 52.7 (41.0) | 50.57 (37.2) | -4.0% | 61.07 (42.9-49.9) | 59.07 / 59.16 (39.6/40.5) | -3.2% |

Windows clean = 3 runs (51.41/51.39/51.33; 61.08/60.83/61.31); Linux 1 run
each. Wukong on Windows: run 1 had 3 stutters (max 122 ms), run 2 none (max
34 ms); Linux 2 stutters (max 95 ms).

What Windows did with the memory (gpumem.csv, dedicated MB before -> during):
- Edge gave up most of its VRAM by itself once the fullscreen game covered
  it: 1361 -> 198 (Superposition), 1300 -> 497 (Wukong). Chromium on Windows
  drops its GPU resources when its window is occluded; nothing on our Linux
  desktop tells Chromium that (Wayland has no occlusion signal it uses).
- WDDM moved the idle processes out only as far as needed: 5725 -> 5149
  (Superposition, at load), 6805 -> 5941 (Wukong, in 32-64 MB steps during
  the run; their "shared" grew by the same amount). The game itself stayed
  in VRAM: 5.6 GB dedicated / ~50 MB shared (Superposition), 4.7-4.9 GB /
  150-330 MB (Wukong).
- So the same policy as ours (the background pays, the foreground keeps its
  memory), plus a browser that cooperates. Linux loses 2-3 points more in
  Superposition; in Wukong the two are within a point (and within run noise).
Not looked into: Steam took ~130 s from -applaunch to the game's process
with Edge and the fill running (18 s clean); it does not touch the measured
stretch.
Pitfalls: Start-Process -RedirectStandardOutput gives the child the job
runner's own output handle (the job is never cleaned up): nvt-idle writes
--log itself. Wukong's first start after the guest boots is slower: the key
at 25 s went to the loading screen and the run measured the menu (299 fps,
kept as bench-logs/win-wk-clean2-*-INVALID-menu); 40 s now, and a run over
150 fps is refused.

## Where we stand and what to improve (2026-11-04 night)

Against the stock Linux driver: under pressure (apps, 1.5 GB short) ~50 fps
instead of 5.8 fps / a hung Wukong; KWin never denied, now without a reserve;
no ping-pong; nothing lost in a clean run. Against Windows 617.42 under the
same pressure: -4.4% vs -1.8% (Superposition), -4.0% vs -3.2% (Wukong, within
noise). Ideas, most useful first:

1. Covered windows should stop drawing (KWin / the hints script, not the
   driver). Likely the whole Linux loss: apps-only with no deficit already
   gave 50.0 fps (the same as apps + deficit), so it is Chromium running
   YouTube and WebGL behind the game, not eviction; KWin flipped ~150/s for
   the game's 50 in nr-apps-evict (13805 flips / 4501 frames). On Windows,
   Edge covered by the fullscreen game stops and gives up its VRAM. Check
   first: count Chromium's frames during the run (Chromium.frames(), the
   page probe in scenario.py) and run with Chromium minimized; then look at
   whether KWin keeps sending frame callbacks to a fully covered window.
   Expected: 2-3 points, close to Windows.
2. Move out only what is needed, gradually, like WDDM (32-64 MB steps as the
   game grows) instead of the 2 GB foreground headroom at load (3-4 GB of
   background moved where Windows moved 0.6-0.9 GB). The game's fps does
   not suffer now; the gain is less PCIe traffic and faster returns to the
   background apps.
3. The Superposition frame tail (1% low 34 vs 43 on Windows, the same with
   the stock driver: not the patch). Start with whether KWin scans the
   fullscreen X11 game out directly or composites it.
4. Final numbers on bare metal, more than one run per case under pressure.

## plasmashell crash on the host: a leaked GSP channel group (2026-11-05)

Reported by another session ("Краши plasmashell"); the user said not to go
back to stock for now. First host boot with the patched driver (boot
1675525d, 2026-11-04 23:47, build 12995825, no reserve, KWin hints). On
2026-11-05 11:32:37 plasmashell got SIGSEGV in QSGRenderThread: a call through
a null pointer inside libnvidia-eglcore.so.615.78.08 under QRhi::endFrame
(`coredumpctl debug 1617`). It came ~100 ms after a failed channel group /
channel alloc in the kernel. The session had just lost its outputs ("There
are no outputs - creating placeholder screen" in every client, kwin
"drmModeListLessees() failed: Permission denied"; VT switch or DPMS wake,
not established); the screen had been locked since 10:38. kscreenlocker_greet
logged "eglSwapBuffers failed with 0x3000" at the same time.

What the kernel log shows (`journalctl -k -b <boot> | grep NVRM`):
- 23:56:01, during sp26-cod.exe with VRAM full (the user reserve denied it at
  27 MB free a few minutes earlier): 3x "ctxBufPoolReserve: Failed to reserve
  memory" (NV_ERR_NO_MEMORY from PMA) in kchangrpapiConstruct, then
  GspRmAlloc of class 0xa06c (KEPLER_CHANNEL_GROUP_A) for hClient 0xc1d000e8,
  hObject 0xcef80002 failing with 0x19 NV_ERR_INSERT_DUPLICATE_NAME: GSP
  already had that object.
- The same client and handle got 0x19 again at 01:26, 04:30, 04:42.
- From 10:27 on, other clients (0xc1d00204, 0x...33, 0x...214, 0x...21c)
  get 0x63 NV_ERR_STATE_IN_USE for both 0xa06c and 0xc56f
  (AMPERE_CHANNEL_GPFIFO_A, its auto-created TSG), in bursts: 10:27 (28),
  10:29, 10:38 (screen lock), 11:32:37 (the crash), 11:32:39. Each burst is
  followed by nvidia-drm "nv_drm_semsurf_fence_create_ioctl: Failed to
  lookup gem object for fence context: 0x0" (the userspace fence of the
  context that was never created).

Cause (stock code, read in the source; the GSP side is a hypothesis that
fits every line above):
- `kchangrpapiConstruct_IMPL` (gpu/fifo/kernel_channel_group_api.c) sends
  the alloc RPC to GSP first (~line 460) and reserves the context buffer pool
  only at the end, after dropping the GPU lock (`ctxBufPoolReserve`, ~line
  589). When that fails it goes to `failed:`, which destroys the kernel-side
  TSG and pools but sends no RPC_FREE. `bRpcFree` is set, but a failed
  constructor leaves `pResource` NULL in `clientAllocResource`
  (resserv/rs_client.c `fail:`), so `serverFreeResourceRpcUnderLock` never
  runs either. GSP keeps the channel group: the handle (hence 0x19 when the
  same client reuses it) and its TSG ID.
- Kernel RM tells GSP which TSG ID to use (`_TSG_ID_VALID`, and
  `_INTERNAL_TSG_ID` for channels with an RM-made TSG), and it has freed
  that ID on its side. Whenever its allocator hands out a leaked ID again,
  GSP refuses with STATE_IN_USE. Clients creating contexts at that moment
  (the lock screen, plasmashell recreating its scene graph after the outputs
  went away) fail. Mesa-style recovery is missing in the NVIDIA EGL
  userspace: plasmashell's render thread called through a null pointer.
- It lasts until the driver is reloaded (reboot): ~3 TSGs leaked at 23:56.

Our part: the patch does not touch this path (no change in fifo/ or
resserv), and eviction never logged a failure this boot. But it lets VRAM
reach ~0 MB free under a game (no reserve; the game takes what we evict),
and the stock driver bug needs exactly that: a channel group created while
PMA has nothing left. Stock can hit it too with a full card, but less often.
Not compared with stock on the host: the previous boot was vm mode.

Not explained: "dmaAllocMapping_GM107: can't update VA space for mapping"
with "Check failed: 0 == (pMapNode->gpuMask & gpuMask) @ gpu_vaspace.c:4502"
(a mapping inserted where the VA space already has one for this GPU) at
23:53:29 (5x), 23:58:29, 00:13:55. 23:53:29 is the second right after a
compositor eviction of ghostty (no remap failure logged, "failed 0"), and all
three are while the game held VRAM full. Could be another OOM path that
leaves a map node behind, or our dmaFreeMap + dmaAllocMap remap; unknown
until the faulting VA is tied to an owner (turn on the evict trace).

Fix to make (not done): in `kchangrpapiConstruct_IMPL`, remember that the
alloc RPC succeeded and send `NV_RM_RPC_FREE` for the object in `failed:`
(or reserve the pool before the RPC). Test: in the VM, force
`ctxBufPoolReserve` to fail once (a module parameter or a counter), then
create contexts until the ID comes round again; without the fix GSP must say
STATE_IN_USE, with it nothing. Possibly also keep a few MB for RM context
buffers out of the game's reach (the old reserve did this for the
compositor only).

### The leak makes every new NVIDIA context fail (2026-11-05 12:10, same boot)

`vkcube --gpu_number 0` and `vulkaninfo` now fail in vkCreateDevice every
time (STATE_IN_USE in the kernel log for each try): the leaked TSG ID is the
lowest free one, so every new context gets it. Games already running keep
working; nothing new starts on the 3080 Ti until a reboot. The fix above is
worth doing before long gaming sessions.

## gamerec: recording the user's own gaming (2026-11-05)

`local/gamerec.sh` (logic in `local/gamerec/gamerec.py`, Perfetto config
`gamerec/flight.cfg`): the user runs `record` in a terminal before playing
(sudo once), marks problems with Meta+Ctrl+M (a KDE service shortcut,
`[services][gamerec-mark.desktop]` in kglobalshortcutsrc, runs `gamerec.sh
mark`), Ctrl-C at the end. Output `local/game-logs/<date_time>/` (ignored by
git); what each file is: the docstring of gamerec.py. How to read one:
- `summary.txt` / `gamerec.sh report DIR`: marks, problem lines by kind,
  MangoHud frame time stats with the worst spikes' wall times;
- `marks/*/note.txt` + `flight.pftrace` (Perfetto, deflate; trace_processor
  or ui.perfetto.dev) for the minute before each mark; marker text
  "gamerec: mark N ..." is in both traces;
- the whole session: `nvtrace/` (`trace/nvtrace.py report DIR/nvtrace`),
  `journal.log`, `sys.jsonl` (epoch time), `evict-status.log`, `hints.log`;
- `mark-calls.log` in game-logs/: every hotkey press, recording or not.
Auto marks: per trigger kind 30 s, then 5 min, then 30 min apart, at most 20
per recording (a broken state like the TSG leak repeats a line forever).
Tested as a user with a harness (collector without root: journal, NVML
samples, Perfetto without ftrace, marks, auto marks, stop, report, MangoHud
logging with vkcube on the iGPU: written as it goes, survives kill -9). Not
yet run as root: nvtrace record, ftrace in the flight recorder, evict status.
The hotkey is registered (kglobalaccel returns it); whether a press runs mark
is not confirmed: mark-calls.log will show it.
Bottles (flatpak, WoW Forever via Battle.net in the bottle `games`, soda
runner, DXVK/VKD3D): the sandbox sees only ~/Games and its ~/.var/app dir,
so gamerec also writes `~/.var/app/com.usebottles.bottles/config/MangoHud/
MangoHud.conf` (only if missing or ours) with output_folder
`.../data/gamerec-mangohud`, and the stop moves those CSVs into the
recording. Needs the bottle's MangoHud switch on (`mangohud: true` in
bottle.yml, was false on 2026-11-05); checked from inside the sandbox that
the config is read and the folder writable, not yet with a game.
MangoHud reloads its config file when it changes (inotify, also after an
atomic rename; checked with vkcube): "Stopped logging because config
reloaded", a summary file, and with autostart_log a new log file 1 s later.
So `gamerec.sh hud` (hotkey Meta+Ctrl+H, `gamerec-hud.desktop`) shows / hides
the overlay in running games, and a recording started or stopped while a game
runs starts / stops its frame log without restarting the game.

### gamerec as a service, started by GameMode (2026-11-05 13:05)

The user wants recordings to start by themselves with GameMode. The GameMode
hook (`~/.local/bin/game-mode-hook`, the user's: power profile, V-Cache mode,
notification; `~/.config/gamemode.ini` pins games to CCD0) runs as the user
and cannot ask for a password, and a NOPASSWD sudo for a script in this
checkout would let any process of the user's become root. So the root part
is now a service: `sudo local/gamerec.sh install` copies gamerec.py,
flight.cfg, nvtrace.py, evict.sh and tracebox to /usr/local/lib/gamerec
(root-owned) and enables `gamerec.service` (`_daemon --uid --gid`, idle until
asked). Requests (start / stop / mark) come only through /run/gamerec/inbox
(0700, the user's; read with O_NOFOLLOW, small regular files only); state in
/run/gamerec/state.json, last.json. Root writes only into /var/lib/gamerec/<id>
(root:<user's group>, 0750/0640); the user's half is local/game-logs/<id>/
(mangohud/, shots/, summary.txt, `system` symlink). `record` without the
service runs the same daemon in the terminal through sudo (`_daemon
--session ID`). The hook got two lines: `gamerec.sh auto "$1"` under setsid
(GameMode's script_timeout is 5 s). `auto end` stops only recordings `auto
start` began; a start while the previous one is still being written waits
for it. Changing gamerec.py needs `sudo gamerec.sh install` again (the
service runs the copy). Tested without root (daemon in a process with the
paths moved: auto start, mark, auto end, record + Ctrl-C, stop by SIGTERM).
Not yet: a real GameMode start. WoW in Bottles turns GameMode on even with
bottle.yml `gamemode: false`: gamemoded's log has Wow.exe as a client (pid
782824 in the morning, 9082 at 12:47-12:53; sp26-cod.exe 14775 last night),
so it will start a recording. Installed by the user 13:19, checked with
`auto start` + mark + `auto end` (all parts, group-readable files).
sys.jsonl now also has power_profile (powerprofilesctl, every 10 s),
x3d_mode and epp (cpu0): what the hook changes.

### WebGPU micro-stutter on the host: the policy tick holds the RM locks ~20 ms (2026-11-05 21:55)

Recording 20261105_215547 (gamerec record, Chrome with a WebGPU page, no game).
Every ~2.05 s `_vidmemEvictWorkItem` (vidmem_tick trigger=timer, evicted_mb=0)
holds the API+GPU locks 13-33 ms (median ~20). With KWin hints active every
policy step walks all clients (`_vidmemEvictGatherProcs`, twice), and the walk
is quadratic: for every vidmem allocation `_vidmemEvictCheck` scans all
resources of all clients for its mappings, and `_vidmemEvictIsFirstPidHolder`
rescans the process for every dup'd buffer (window buffers shared with kwin).
Chrome's GPU process, kwin, MangoHud's NVML thread wait behind it up to 20 ms.
While Chrome was focused (from 33117 s boottime) every display hitch (10-27 ms
gap at 165 Hz) overlapped a lock hold: the tick, the hint-triggered step
(kworker, vidmemEvictProcess ~22 ms) or gamerec's own evict-status read
(evict.sh every 10 s, 9-14 ms). Not fixed. Fix ideas: no client walk on the
timer when nothing changed (hint generation, PMA free, process set), one walk
per step instead of two, an index (memdesc -> mappings) instead of the
all-clients scan; gamerec should read evict status only at marks.
Unrelated background: PSI io ~75 % all day is ghostty, not the disk: its
renderer/io threads (libxev on io_uring) sleep in io_cqring_wait with a read
in flight, and the kernel counts that wait as iowait (the PSI is all in
app-com.mitchellh.ghostty.service; system.slice 0). Harmless. scx_lavd's
scx-urcu thread sitting in D only adds 1 to the load average.

## To fix (list as of 2026-11-05 22:10)

The user's overview of all open tasks (these items, the redesign, older
ideas) is `local/BACKLOG.md`, in Russian, same numbers. It is also the task
tracker (status column, dated «Ход» lines, «Сделано»; rules at its top). Keep
it in sync: add new problems there too, move statuses as work goes.
Its section «Решения для автономной работы (06.11)» holds the user's answers
for the unattended fix run (commit per task, revert on failure, decide by
"focused window responsiveness first" and log it, plus per-task choices for
#10 #7 #13 #3 R #5 #6 #1); read it before taking a task.

Asked by the user ("записать, что надо исправить"). None of it is done.
Driver changes go through the VM first (local/vm/vm.sh, nvtest), then the
user installs on the host.

1. **Policy tick holds the RM API+GPU locks ~20 ms every 2 s** (driver,
   src/nvidia/src/kernel/mem_mgr/vidmem_evict.c). Visible as micro-stutter in
   anything that calls RM per frame (Chrome WebGPU, kwin): 2-4 frames lost at
   165 Hz. Details in "WebGPU micro-stutter" above. In order:
   - `_vidmemEvictPolicyStep`: with hints active it walks all clients on every
     timer tick. Skip the walk when nothing changed since the last one: hint
     generation (osGetVidmemHint), avail MB beyond a small delta, the client
     set, no evicted memory waiting for restore;
   - one `_vidmemEvictGatherProcs` per step, not two (after moves adjust the
     totals instead of rewalking);
   - the quadratic parts: `_vidmemEvictCheck` scans every resource of every
     client for the mappings of each allocation, `_vidmemEvictIsFirstPidHolder`
     rescans the process for every dup'd buffer. One pass that builds
     memdesc -> holders/mappings, then the checks look it up;
   - same for the hint-triggered step (kworker, `vidmemEvictProcess` ~22 ms)
     and the procfs status read (~10-14 ms).
   Done when: vidmem_tick us < 2 ms with Chrome + kwin + hints, no rm_lock
   waits > 2 ms behind workItemLocksAcquire in an nvtrace, no display gaps at
   the tick times (check as in recording 20261105_215547: kwin flip gaps vs
   rm_lock holds).
2. **gamerec reads evict status every 10 s** (local/gamerec/gamerec.py,
   `Collector.loop`, evict-status.log): each read is a 9-14 ms lock hold, so
   the recorder causes stutter itself. Read it only at marks and at start/stop.
   The service runs a root-owned copy: after the change the user runs
   `sudo local/gamerec.sh install` again.
3. **Leaked GSP channel group after a failed channel-group alloc** (stock bug,
   `kchangrpapiConstruct_IMPL`, kernel_channel_group_api.c): send
   NV_RM_RPC_FREE in `failed:` when the alloc RPC succeeded. Without it a full
   VRAM breaks every new NVIDIA context until reboot (the plasmashell crash).
   Details in "plasmashell crash on the host" above.
4. **Unexplained "can't update VA space" errors** (23:53:29, 23:58:29, 00:13:55
   that night): record the next occurrence with `gamerec.sh record
   --evict-trace`. Recording 20261106_112014 (11:46:54, 11:57:32, 12:02:42)
   gives a clue: the failing check is `0 == (pMapNode->gpuMask & gpuMask)`
   (gpu_vaspace.c:4502, _gvaspaceMappingInsert -> INVALID_ARGUMENT), i.e. a
   mapping for this GPU already exists at that VA; each time 1-6 s after an
   evict+restore of the same app in one step (claude-desktop 136 MB at
   11:46:48-49, ghostty every 2 s before the other two). Not proven
   (ghostty cycled 449 times that session, only 3 errors), but check that
   the remap of evict/restore leaves no map node behind: nvtest that evicts
   and restores a process in quick cycles while it maps/unmaps.
5. **Slow eviction copy of some allocations** (Battle.net under Wine: 290 MB
   in 744 ms, ~0.4 GB/s instead of ~20): a 920 ms freeze in WoW, because the
   move ran synchronously in WoW's allocation. First log the page size and
   contiguity per allocation in vidmem_move; if it is 4 KB non-contiguous,
   copy runs of physically contiguous pages / allocate sysmem in bigger
   chunks while keeping the 4 KB mapping. Also cap what one allocation may
   move synchronously (e.g. 100 MB or ~20 ms) and do the rest from the work
   item.
6. **Evict/restore ping-pong next to a running game**: claude-desktop and
   bottles came back "idle-back" every 30-60 s and were evicted again by
   WoW's next allocation (8 cycles in 26 min, each a 30-40 ms frame in WoW).
   Do not restore idle-back while the foreground process is fullscreen and
   near the free-memory threshold, or require more room / a longer quiet time
   after the last eviction for the same process.
7. **A focused app is denied while a background game keeps gigabytes**
   (WebGPU page died next to CoD, see above). When the foreground process
   (hint f) needs memory and only a protected game that is not focused holds
   movable memory: either move out of the game just what is needed (request
   + margin, like WDDM demoting a background process), or create the
   allocation in sysmem instead of denying it (vidmemEvictWantBorn allows
   this only for hidden/idle processes now). Denying kills apps that do not
   handle OOM.
8. **makeroom on every small allocation**: when the foreground headroom
   target cannot be reached (nothing movable), every later allocation of the
   process still pays 4-11 ms under the lock. Remember "nothing movable"
   until something changes (a tick, a free, a hint) and only make room for
   the allocation itself.
9. **Evict and restore the same visible app in one step** (CoD fps drop,
   see above): "foreground again" moves a visible app out for the
   foreground's headroom, and _vidmemEvictPolicyHints restores it in the same
   step because it is visible. Cause found: the 60 s hold
   (VIDMEM_EVICT_FG_HOLD_SEC) compares `nowNs - pAct->evictedForFgNs` with
   nowNs taken at the start of the step and evictedForFgNs set later in the
   same step: the NvU64 difference wraps, the hold never applies in the step
   that moved it out. Guard `evictedForFgNs > nowNs` (or take the time anew). A process evicted for the foreground must not
   be restored "visible" while the foreground is still short (same rule as 6,
   but here it is every 2 s; in recording 20261106_112014 ghostty went out
   and back 449 times, claude-desktop 90, also from "foreground headroom"
   right after a restore). Also stop retrying a restore that failed
   (chrome, 1 MB, every 2 s for 9 min): back off after a failure.
10. **The focused game's own memory stays in sysmem for many minutes** (CoD,
    see "CoD, the rest of the session" below; the main fps loss of that
    session). "foreground again" restores a process all or nothing (needs
    evicted + 256 MB free); when nothing else is movable and the game fills
    the free space with new allocations, it fails every 2 s for as long as
    the game runs (1353 times; 8 and 22 min with 989 / 562 MB of CoD in
    sysmem, PCIe 12-16 GB/s, ~210 W instead of ~280 W). Restore partially:
    every step, whatever fits above a small margin (at 12:23:27 450 MB were
    free for 562 MB evicted, a minute later the game had taken them). Longer
    term: when the game itself is over VRAM, swap its hot evicted
    allocations with cold resident ones (access counters, the "hot memory"
    work of 2026-11-02).
11. **A restore that cannot reach its target still moves others first**:
    12:23:27 ghostty (visible) out 37 MB for CoD's restore, the restore
    failed (494 < 578 MB), ghostty back in the same step. Check free +
    movable >= need before moving anything (or partial restore, item 10).
    Also the failure line says "is visible" for the foreground and is
    printed every 2 s (1353 lines): say "focused", rate-limit it.
12. **Compositor makeroom takes 9x what was asked from the focused game**:
    12:21:48 a 32 MB request (udev-worker, counted as the compositor)
    evicted 284 MB of CoD (foreground) in 531 ms: the game and the
    requester stalled half a second. Cap it to the request + margin, take
    from hidden/idle processes before the focused game; 284 MB in 531 ms is
    also the slow copy of item 5.
13. **New channel groups fail when free VRAM is fragmented**: 12:15:28
    ctxBufPoolReserve (kernel_channel_group_api.c) got NV_ERR_NO_MEMORY with
    35 MB free, the largest contiguous block 1 MB; xdg-desktop-portal-kde
    segfaulted at the same second (coredump). Channel context buffers do not
    go through makeroom. Make room there too, or keep a contiguous cushion
    (~64 MB) for context buffers. 12:23:28 the next alloc on the same client
    as 11:48:14 (0xc1d00033) failed with 0x63 NV_ERR_STATE_IN_USE: the
    leaked handle of item 3.
14. (dropped by the user 10.11) **gamerec: no MangoHud log for CoD** although the overlay was on (the
    screenshot shows it): the game runs under its own wine in
    ~/Games/Call of Duty Modern Warfare 4/, which loads MangoHud with its
    own config, not ~/.config/MangoHud/gamerec.conf. Find that launcher's
    MangoHud setting (MANGOHUD_CONFIGFILE) or also write output_folder into
    the default MangoHud.conf while recording.

### WoW, 2.5 h on the host (recording 20261105_220725, 2026-11-05 22:07-00:52)

WoW Forever (Bottles, DXVK), GPU-bound all the time (97-100 %, ~340 W,
1750 MHz), 70-90 fps, median 12.3 ms, ~9.5 GB VRAM used, ~2 GB free.
MangoHud time aligned to the trace by the eviction frames (offset: main CSV
el=0 at boottime 35003.43). Findings:
- 8371 frames of 33 ms in 96 runs (293 s): WoW's own background fps cap,
  every run starts at a focus loss (vidmem_hint Wow.exe f->v/h). Not a problem.
- 22:27:12 a 920 ms freeze: WoW's allocation (makeroom, in WoW's thread)
  evicted 797 MB synchronously; 290 MB of Battle.net (pid 36248
  CrBrowserMain, CEF with --in-process-gpu under Wine) took 770 ms, copy_us
  744100 (~0.4 GB/s; its other moves ran at ~20 GB/s). The restore of
  Battle.net after WoW quit was as slow (759 MB, copy 751 ms). Likely
  non-contiguous 4 KB-page allocations: chunk = pPmaInfo->pageSize, so CeUtils
  copies 4 KB at a time (unconfirmed: the trace has no page size per alloc).
- Evictions in WoW's thread ("Disk Queue 0", "Wow.exe"): a 30-66 ms frame
  each, exactly at the move (-0.04 s). From 00:23 to 00:49 a ping-pong:
  claude-desktop (105-275 MB) and bottles restored "idle-back" 30-60 s after
  being evicted, then evicted again by WoW's next allocation, 8 times.
  Background restores cost ~20-25 ms frames (median 12).
- The 2 s policy tick: weak for WoW (DXVK rarely calls RM per frame): spikes
  overlap it only ~20 % more than at a random offset.

### WebGPU page died next to CoD (recording 20261106_112014, mark 01 at 11:22:16)

CoD (sp26-cod.exe) held 9084 MB (6484 movable), free vidmem 160-300 MB. Each
time the user focused Chrome (11:20:57, 11:21:55; CoD stayed visible on the
other screen, still rendering), the WebGPU page (a chart site) re-created a
4K MSAA texture of 220 MB: vidmem_makeroom ok=0, vidmem_reserve
outcome=denied -> Dawn "vkAllocateMemory failed with
VK_ERROR_OUT_OF_DEVICE_MEMORY While calling CreateTexture "msaa"", then every
frame "Invalid Texture msaa ... Invalid CommandBuffer": the page does not
recover, the canvas stays dead until reload. Nothing else was movable: the
hidden apps were already in sysmem (1504 MB), CoD is protected (busiest, and
>= 1 GB active = a game). Also: every Chrome allocation, even 4 KB, ran
makeroom (want_mb ~2049: the foreground headroom) at 4-11 ms each, ~100 in
a burst, all failing to reach the target.

### CoD fps drop: evict + restore of a visible app in the same step (mark 06 at 11:48:07)

Same recording (20261106_112014). From 11:47:59 every policy step (2 s) does
"foreground again: free 26-140 MB, want 778 MB, evicting ... claude-desktop
(pid 93448, visible, for the foreground)" (CoD focused, 6.5 GB of its own
movable) and in the same step "restoring claude-desktop (pid 93448, visible,
130-256 MB)": 130-256 MB moved out and back every 2 s, 13-114 ms under the
locks each, still going at 11:49:15 (47 restores so far). The user saw the
fps drop. Free vidmem hovered at 7-60 MB; at 11:48:12 a context buffer pool
reserve failed (rmMemPoolReserve NO_MEMORY, ctx_buf_pool.c:315) and at
11:48:14 the next channel-group alloc got GspRmAlloc 0x19 (hClass a06c,
hClient 0xc1d00033): the stock TSG leak again (see "plasmashell crash").
Earlier in the session: a failing 1 MB restore of chrome retried every 2 s
(150 "restore pid 2589: 0 allocs, failed 1" from ~11:37 to 11:46), and the
first "can't update VA space" with a recording (11:46:54, mark 05:
gpu_vaspace.c:4502 pMapNode->gpuMask check, _gvaspaceMappingInsert
INVALID_ARGUMENT).

### User's direction (2026-11-06 ~11:55): the focused window comes first

"We have to learn to tell which windows are active. Visible is fine, but the
window in focus matters more." The user is collecting problems now; the fixes
go in one later session together with the "To fix" list. Not started.

What the hints are today (kwin-vram-hints/contents/ui/main.qml, every 250 ms
and on activation): per pid 3 = foreground, 2 = visible (>= 2 % of the window
shows), 1 = hidden (minimized, other desktop/activity, covered). Foreground =
the game on top if there is one (fullscreen, also unmanaged X11), else the
active window if it shows. The driver keeps per process lastFgNs,
lastVisibleNs, hiddenSinceNs, evictedForFgNs.

Proposed redesign (to discuss at the fix session):

1. Order of importance: focused > recently focused > visible > hidden.
   - While the focused process is short (below its headroom, or it was just
     denied/had room made), nothing visible is restored: not only for 60 s
     after it was moved out (VIDMEM_EVICT_FG_HOLD_SEC), but until the focused
     one has its headroom and a quiet period passed.
   - A restore that failed backs off (it retried every 2 s for 9 min).
   - First the one-line bug: the hold never applies in the step that moved
     the app out (NvU64 wrap, item 9 of "To fix").

2. The focused app may take memory from a game that is visible but not
   focused (WebGPU page next to CoD, item 7). Today the busiest process and
   any active process with >= 1 GB are protected from everyone but the
   compositor. New rule: that protection holds against the background and
   against visible apps, not against the focused one. Take only what the
   allocation needs (+ margin), least recently used first, never more than a
   cap per step; the game draws partly from sysmem until it is focused again
   and gets it back by "foreground again". If even that is not possible,
   create the focused app's allocation in sysmem instead of denying it
   (vidmemEvictWantBorn, today only hidden/idle processes).

3. Recency, not only state. The KWin script sends an MRU rank with the
   state (workspace activation order; KWin has it, e.g. Workspace.stackingOrder
   + windowActivated timestamps kept by the script), and the driver keeps
   lastFgNs per process already. Victims ordered by: hidden before visible,
   then the longest since last focused, then idle before drawing, then the
   biggest. "Recently focused" (say < 30-60 s) is protected like visible
   even when hidden (alt-tab back and forth), extending
   VIDMEM_EVICT_RECENT_FG_SEC.

4. Drop "the fullscreen game is always the foreground" when another window
   is active: with two monitors the user works in Chrome/Claude on the second
   screen while CoD stays fullscreen on the first, and the game kept
   foreground priority over the window actually in focus. Keep a separate
   flag "game" (fullscreen, >= 1 GB, busiest) for the rules that need it
   (headroom while it loads, never evicted for the background), and give
   "foreground" only to the active window. When the game is focused again it
   gets its memory back first (pass 0 of _vidmemEvictPolicyHints).

5. Make "active" more than focus where it is cheap: a window that received
   input in the last seconds (KWin knows the active window only; input is not
   visible to the script, so leave this out unless KWin exposes it), video
   playing in a visible window (drawing steadily: _vidmemActivity already
   tracks drawingSinceNs) ranks above a visible idle one.

How to check: nvtest scenarios in the VM (fgprio, webgpu), plus the host
cases recorded with gamerec: WebGPU chart focused next to CoD (no denial, the
page keeps drawing), CoD focused with Claude visible on the second screen
(no evict/restore every 2 s, CoD fps steady), WoW with Battle.net hidden.

### CoD, the rest of the session (recording 20261106_112014, 11:20-13:02)

The user: "fps was so-so, at some point I lowered the settings". CoD
(sp26-cod.exe, VKD3D, 4K, 12 GB card), Chrome/Claude/ghostty on the second
monitor. No MangoHud log (item 14), so the measure is GPU power and PCIe from
nvtrace samples.jsonl; per-minute table via the scratch script (CoD's VRAM /
sysmem from evict-status.log).

| CoD state | CoD in sysmem | PCIe rx / tx GB/s | GPU power | GPU util |
|---|---|---|---|---|
| focused, all in VRAM (11:35-11:46, 12:06-12:20) | 0 | 2-5 / 0.3-1 | 265-290 W | 93-97 % |
| focused, part in sysmem (11:58-12:05, 12:24-12:45) | 562-989 MB | 12-16 / 5-6 | 210-216 W | 98-99 % |
| after the settings drop (12:46-12:57) | 0 | 0.7-1.8 / 0.1-0.2 | 285-293 W | 96-97 % |

With part of the game in sysmem the GPU shows 99 % but draws ~70 W less: it
waits on PCIe. Mark 15 (12:23:24, screenshot): 40 fps, 25 ms frames, GPU
99 %, CoD 562 MB in sysmem.

How the game's memory got there (every switch away does it):
- The user focuses Chrome or Claude on the second monitor. That app had been
  moved out while CoD was focused; "foreground again" restores it and takes
  the room from CoD, now visible, the only process with movable memory:
  11:27:16 494 MB for Chrome's 268, 12:21:48 277 MB for Claude's 261 (want =
  need + 256). Plus compositor makeroom: 12:21:48 284 MB of CoD in 531 ms for
  a 32 MB request (item 12).
- CoD is focused again. Its restore needs evicted + 256 MB free; what was
  free goes to the game's own new allocations within a minute (CoD 8830 ->
  9342 MB resident at 12:23-12:24, free 2 MB after), the rest of the desktop
  has nothing movable left (kwin/plasmashell never, the hidden apps' rest is
  not movable). The restore fails every 2 s: 11:57:25-12:05:22 (989 MB,
  restored when something freed), 12:23:34-12:45:57 (562 MB).
- 12:45:57: the user lowers the settings, CoD drops to 7922 MB, 1490 MB
  free, the restore goes through (493 MB). The fps came back mostly because
  of that, not because of the lighter settings: the GPU went from ~215 W to
  ~290 W.

Other things in the recording: 12:15:28 new channel groups failed with
fragmented VRAM (35 MB free, 1 MB contiguous) and xdg-desktop-portal-kde
crashed (item 13); 12:23:28 the leaked channel-group handle again (item 3);
12:23:27 ghostty out and in again in the same step (item 11); "can't update
VA space" at 11:57:32 and 12:02:42 (item 4).

So for the redesign: besides "focused first", the focused game must get its
memory back even if only part fits (item 10), and an app focused for a few
seconds on the second monitor should take from the game only what it needs
(+ small margin), not need + 256 MB. Ideally the game gives up cold
allocations, not the ones it reads every frame (access counters).

## Policy simulator (2026-11-06 evening)

Asked by the user: "make it possible to work on the driver without my GPU".
`local/sim/` (README there): the eviction policy runs against a memory model,
on gamerec recordings (replay.py) and hand-written scenarios, in well under a
second (1 h 40 min of CoD in 0.05 s). `run-tests.sh` runs scenarios/; those
marked `# expect-fail:` are known bugs whose checks say what should happen.

How: the policy moved out of vidmem_evict.c into
src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c (+ .h, added to
src/nvidia/srcs.mk). It decides; the platform does the rest through
vepPlat*() (avail MB, gather processes, move a pid, hot restore, chunk info,
contiguous window). The driver's entry points became wrappers around vep*()
cores (vepPolicyStep, vepWantBorn, vepBackgroundCushion, vepMakeRoomAllowed/
Run, vepKeepFgHeadroomWanted, vepMakeRoomCompositorRun, vepReclaimableFor);
the GPU sampling (_vidmemActivitySample/SampleSubmits, _vidmemActivityNote)
stayed in the driver. Meant as a pure move: a diff of the old regions against
the new file shows only static -> extern, RM calls -> vepPlat*, and the
entry points split at their RM parts. Built and linked on the host
(nv-kernel.o, 2026-11-06 18:49); NOT run on a GPU yet: `nvtest all` in the VM
before the next install (BACKLOG S).

The model (sim.c): vidmem for processes = free + per-process vidmem, taken
from the recording at every step (11 000 +- 80 MB that session, the driver's
own memory varies); allocations in vidmem or evicted; a denied allocation is
an event, not kept (the app's own sysmem fallback is invisible in recordings
too: CoD had 249 denials in the trace and none in its vram/ram); moves take
1 ms + 3.5 GB/s and the clock runs on meanwhile. Pitfalls found while
building it: all growth of a process at the tick instant meets the one-second
"fruitless" pause of make-room and gets denied (growth is spread over the
interval now); compositor requests from kernel-thread contexts (pid 667, 32
MB scanout buffers) are not in any process's footprint and are replayed as
`comp`.

First results:
- #10 reproduced: clip 11:49:30-12:06 (scenarios/cod-stuck-restore.scn), CoD
  1034 MB stuck while focused 11:57:25-12:05 (recorded 989 MB, back at 12:05
  in both).
- #9 has two causes. With the clock frozen inside a step the "visible" pass
  held (no wrap) and ghostty still came back the same step through 1b
  "drawing again", which ignores VIDMEM_EVICT_FG_HOLD_SEC. With the clock
  running during moves the sim restores through "visible" as the journal
  shows (98 out/back in 16 min); the evictedForFgNs > nowNs guard alone (tried
  on a scratch copy) cuts it to 11, the rest goes through 1b.

## Unattended fix run (2026-11-06 22:30 - 2026-11-07, user away)

The user answered questions per task (local/BACKLOG.md, «Решения для
автономной работы (06.11)») and set the goal "do the BACKLOG tasks as
decided". Rules for the run: one commit per task after the simulator and
the VM; revert what fails; decisions not covered by the answers are taken by
"the focused window's responsiveness first" and logged in the task's «Ход».

Branches: `local/vidmem-615` has what passed the VM (#9/#6 2637d828, #10
9c7817df). At 23:22 a host-watchdog reset of the guest (a test that had
left Chrome filling vidmem) wedged the GPU (see the Test VM section: GSP's
WPR2 survives the resets vfio can do), so everything after that is on
`local/vidmem-615-pending`, one commit per task, simulator-checked and
built, not run on the GPU: R + #7 (focus first; FG_ALLOC level; previous
window kept; KWin script "f" = active window, "g" = fullscreen; Steam
"reaper" flag; placement retry in whole chunks), #11 + #8, #12 (2 MB-page
allocations first when making chunks), #1 (holder index, one walk per step,
trace reuses the step's walk), #3 (free the TSG on GSP on a failed
construct), #13 (ctxBufPoolReserve makes room and retries), #5 part
(headroom refilled in the background), #2 (gamerec reads the status only
at start/end/marks), #4 test (nvtest vaflip, nvt-webgl churn=), I7
(background kept off the game's headroom), vm.sh NVVM_ROMBAR.

To do once the GPU is back (host reboot, it stays in vm mode):
`local/vm/vmtest.sh all` on the pending branch (new scenarios fgback,
focus, vaflip); if it passes, fast-forward local/vidmem-615 to it and move
the BACKLOG statuses to «VM ✓». Known open points: nvtest focus - the
WebGPU page opened in the focused window next to a game that fills vidmem
did not start drawing within 20 s (seen twice; by hand it drew but some
buffers were invalid; game B's own WebGL streaming allocations failed in
PMA with free memory to spare, which the placement retry now handles, with
a guard against retry storms); #12, #1, #3, #13 are only built; #5's copy
speed itself needs per-phase measurements on the GPU.

Replays of every recording, old policy (6af7edab) vs pending: CoD session
fg_sys 371k -> 59k MB*s, moves 466 -> 96, out-and-back 332 -> 20, CoD's
waits inside its allocations 1372 -> 91 ms, but CoD's denials 96 -> 851
(free memory that used to sit unused now holds its restored data, so its
growth is denied ~400 MB at a time and goes to sysmem through vkd3d
instead); WoW: waits 227 -> 0 ms, walks halved. The BACKLOG has the table.

VM run after the host reboot (2026-11-07 00:31-01:36): `vmtest.sh all` on
the pending branch, 13 then 15 scenarios. Found and fixed: the foreground's
last few MB never came back (64 MB margin; now whole when it fits with
16 MB), the 2 MB-page preference overrode the victim order and the
compositor got 64+64 MB margins (comproom), and test bugs (focus's wait
matched "errors=0"; set -e exits; the kernel-log check matched "default").
fgprio-anim now follows R5. New scenarios comproom (#12) and ctxroom (#13,
#3). Final: 15/15 PASS; local/vidmem-615 fast-forwarded to the pending
branch. Step time (local/tickbench.sh, guest with 2 Chromes, 13 tabs):
2.88 -> 2.21 ms median, with per-process tracing 3.6 -> 2.25 ms. Eviction
of 1 GB on a fresh host: 125-146 ms (copy ~20 GB/s); the slow cases come
from allocating 2 MB sysmem pages when RAM is fragmented. Not seen firing
in a test: the room-making for channel contexts (#13; a policy step moves
the hidden app out first).

Make-room vs. what PMA was really asked (2026-11-07 04:10-06:10). A full
`vmtest.sh all` after `stress.sh scanout` failed fgprio-idle (the focused
game got no room, the visible idle app gave nothing), reproducible until a
module reload; scanout churn-8x16 failed about one run in three. Two causes:
(1) vidmemConstruct passed make-room the *attribute* PHYSICALITY_CONTIGUOUS,
but PHYSICALITY_DEFAULT allocations go to PMA contiguous too (`bContig` in
_vidmemPmaAllocate): Chrome's 6 MB WebGL buffers (96 x 64 KB, PMA flags
0x242) and kwin's buffers. Make-room counted 35 free 2 MB chunks, saw enough
and moved nothing while the longest free run was 3-5 MB. Now
_vidmemPmaAllocate records the contiguity and page-rounded size of its last
PMA request (`_vidmemLastPmaContig/Bytes`) and both make-room paths use them.
(2) A victim with only 64 KB-page allocations frees bytes, not chunks: four
rounds of "what is missing" moved 213 MB and still had fewer than 16 free
chunks for a 32 MB 2 MB-page buffer. Each round now asks for twice the last
(_vidmemEvictMakeChunks). Also fixed on the way, in PMA:
`numFree2mbPages` counts 2 MB groups from the region base, and region 0
here starts at 0x37c0000, so the counter read 15 with 35 aligned 2 MB pages
free in every map; `_pmaPredictOutOfMemory` no longer trusts it when a
region is not 2 MB aligned. A 64 MB compositor margin, tried first, only
hid (1)/(2) and is not in. Results: stress.sh scanout 7/7 PASS, 4 rounds of
stress -> fgprio-idle clean (A gives all 1227 MB, B gets 10.9 GB), comproom
64 MB per 32 MB buffer from the hidden app only.

Freeze in WoW on the host, 2026-11-08 10:26 (recording 20261107_234433, user
mark 10:26:32). Build d294cce4. 5 s before the mark the policy restored the
hidden Battle.net GPU process (CrGpuMain, pid 35624, 557 MB, why=idle-back,
4.3 GB free so #6 allowed it) inside the timer tick: vidmem_move total 751 ms,
copy 746 ms; vidmem_tick us=757149; no drm vblank events for 0.759 s on either
CRTC (the RM locks stop nvidia-modeset too). Same at 10:25:50 on the evict
side (305 MB in 755 ms, copy 726 ms; vblank gap 0.632 s), while Chrome was
focused. The other moves of the session ran at 7-23 GB/s, including the
second Battle.net batch (252 MB, 33 ms copy) and claude-desktop (214 MB, 31
ms). So one or a few Battle.net allocations (~290 MB together) copy at ~0.4
GB/s both ways; the old WoW case (#5, 290 MB of Battle.net in 744 ms) is the
same thing, not sysmem fragmentation. vidmem_move only has per-process
totals, so which allocation it is (kind, page size, contig, compression) is
not known yet: `sudo local/evict.sh dry <pid>` lists them (EVICT_PRINTF), a
per-allocation trace for slow copies is the next step. Policy side: while a
game is focused, no background move should hold the locks for more than ~50
ms (user's #5 decision); a restore of a hidden app has no deadline at all.

`sudo local/evict.sh dry 35624` (Battle.net CrGpuMain), 2026-11-08: 58
eligible allocations, 557 MB. The slow ones are compressible (kind 0x8) on
64 KB pages, non-contiguous: 0xa9 256 MB and 0xba 32 MB; with the small 64 KB
allocations that is exactly the first evict batch (305 MB, copy 726 ms, ~0.42
GB/s). Kind 0x8 on 2 MB pages (4-28 MB each, the second batch, 252 MB) copied
in 33 ms (~7.6 GB/s). So the slow path is the CE copy of compressible memory
on 64 KB pages through the compressible FB alias (L2 decompression), not
launch overhead (the per-run merge in ceutilsMemcopy holds; uncompressed 64
KB runs cost ~3.5 us per launch). Not yet reproduced: next is compr-test with
a large image on 64 KB pages in the VM (the 16 MB compr-test image got 2 MB
pages), timing the evict per allocation.

Whole recording 20261107_234433 (23:44-11:58, WoW, a video on the second
monitor 11:02-11:48), build d294cce4, `nvtrace.py report` on a copy:
- No Xid, no "can't update VA", no NVRM errors besides the policy's own
  lines; 41 moves, 35 of them 2-44 ms.
- The six 736-778 ms moves are all Battle.net (#5), in three
  evict/restore pairs: the focused window's 2 GB headroom (Chrome growing
  to 2.5 GB, WoW loading) evicts the hidden Battle.net, Claude, Steam (~900
  MB); 37-81 s later the focused window has given memory back, 4 GB are free,
  and after VIDMEM_EVICT_QUIET_SEC (30 s) step 3 ("plenty free") restores
  them all. ~1.8 GB copied per cycle for apps nobody is looking at (#6).
- Policy tick on the host: p50 3.9 ms, p99 4.8 ms (17 processes), was ~20
  ms; the #1 goal (< 2 ms) is not met, but no frame gap lines up with a
  normal tick.
- Fullscreen Chrome video (and Telegram's media viewer, Spectacle) got the
  game flag "g" (fullscreen + GPU busy); no moves happened while it was set.
- 11:02 MakeRoomPlace for Chrome's own contiguous 130 MB buffers moved
  Chrome's previous 130 MB buffer to sysmem five times (Chrome then freed it):
  copies of soon-dead memory (I13).
- crtc0 (WoW's monitor) dropped from ~100 to ~35-60 flips/s with GPU load
  ~42% and ~255 W whenever Chrome was focused and WoW only visible; with WoW
  focused and the video still playing it was back at ~100 fps / 99%. That is
  WoW's own background frame cap (Max Background FPS), not the driver: no
  moves or long lock holds in those minutes. crtc1 (video) ~34 flips/s.


Mark 2026-11-10 00:02:53, recording 20261109_173525 (17:35-00:04, WoW
pid 2302067 started 23:15, YouTube in Chrome), build d294cce4. Five
whole-screen stalls (crtc0 and crtc1 both without vblank events):
00:02:07.8 1.6 s, 00:02:12.9 1.5 s, 00:02:35.2 0.9 s, 00:02:36.1 0.8 s,
00:03:36.6 1.6 s. Every one is a move of WoW's own buffers at 0.42-0.48
GB/s (640, 968, 351, 392, 1095 MB; copy_us is ~97% of total_us). Other
buffers of the same WoW moved at up to 23 GB/s; the earlier WoW (pid
1985432) never took more than 0.5 s. So slow allocations (#5) belong to
the game too, and deferring background moves does not cover them: the
click-restore of WoW's 968 MB took 1.47 s. Chain: Chrome's GPU process
(pid 2728) wants contiguous 130-192 MB, ~1.9 GB free but the longest free
run is 68-72 MB; _vidmemEvictMakeContig picks the window with the fewest MB
to move among all procs at VE_LEVEL_COMPOSITOR, the visible game included,
and moved 4 x 160 MB of WoW inside Chrome's allocation (why=alloc, 1.52 s).
Clicking WoW restored it (1.47 s); focusing Chrome again evicted WoW for
the 2 GB headroom (0.83 s) and for another contiguous window (0.81 s).
Whole recording: 591 vidmem_makeroom for Chrome, 579 evictions of Chrome's
own previous buffers, 74.7 GB copied, 11.5 s under the RM locks, in bursts
at 20:30, 21:30, 22:20, 00:00; all 39 "restore ... failed N" auto marks are
Chrome's evicted buffers not fitting back. New BACKLOG #15 (I13 folded in).
Asked the user for `sudo local/evict.sh dry Wow.exe` to see the kinds and
page sizes of the slow WoW buffers.
`evict.sh dry Wow.exe` on a fresh WoW (pid 3295776, 2026-11-10 00:40): of
115 eligible allocs (3816 MB), 10 are kind 0x8 on 64 KB pages, the
Battle.net signature: five of 256 MB (0xa6 0xa9 0xac 0xf4 0x10e), 0xa1 128
MB and four small, 1468 MB, ~3.2 s at 0.45 GB/s. WoW's other compressible
allocs (40-odd, 4-86 MB) are on 2 MB pages. 0xae (256 MB, kind 0x8, 64 KB)
is not eligible (map-kind). The 256 MB size looks like DXVK/vkd3d memory
chunks. Matches 00:02 (640 MB in 4 allocs, 968 MB in 8). The VM repro is a
256 MB compressible alloc on 64 KB pages, timing evict and restore.

2026-11-10 11:20 the user reports one Chrome page "breaking" while others
are fine: broker.smart-systems.club, tab FORCOMPASS (a ~600-row live table,
2728x9989 CSS px, no canvas, 213 SVGs). Recording 20261110_111712 (mark
11:20:06; the gamerec screenshot caught only WoW's monitor): storms at
11:18:45-52, 11:19:49-52, 11:20:00-04, Chrome's GPU process asking for
contiguous 133120 KB up to 30 times a second, 195 make-rooms, each moving
Chrome's own previous buffer (10-70 ms, sysalloc up to 63 ms). Reproduced by
opening the page in an agent tab (11:24:56-58, 2743x1416 at DPR 2): 15
requests of 294912 KB; the first moved 264 MB of the background WoW in 209
ms, and one make-room failed (ok=0). The agent tab rendered fine. Unknown:
what "breaks" looks like and whether the moves cause it; the test is the
same page with `evict.sh auto off`. gamerec's mark screenshot should cover
all monitors.

#16 step 1 (2026-11-10, branch local/vidmem-615-pending, not yet on the
GPU): VMT_FRAG / tracepoint vidmem_frag. _vidmemFragStats() walks
vidmemPmaUsedMap() in 2 MB chunks with popcounts: total, free, free in
whole chunks, longest run, runs (>= 32 MB, >= 128 MB), partly used chunks
and what is in use in them. Emitted every 5th timer policy step (~10 s,
at=tick) and after each evict/restore batch in _vidmemEvictPidLocked
(at=evict/restore, with the longest run and free chunks taken before the
batch). Same numbers on a "fragmentation:" line in evict.sh status (no
reserve). vidmemPmaUsedMap's largest-run loop now skips all-free/all-taken
words (checked equal to the bit loop on 20k random maps). nvtrace.py report
has a "Fragmentation" section and perfetto counters. Host build needs
LD=ld.lld IGNORE_CC_MISMATCH=1 (CachyOS kernel built with clang), as
vmtest.sh does.

2026-11-10 13:30: the host was updated before the reboot (kernel 7.3.3,
nvidia-utils / linux-cachyos-nvidia-open 620.70.06). Our 615 modules live
in /usr/lib/modules/7.3.1-1-cachyos/updates/nvidia-local only, so in host
mode the 7.3.3 kernel loads the stock 620.70.06 from extramodules (no
reserve, no eviction, no vidmem_hints; the KWin script and gamerec find no
procfs files). The guest is still pinned at 7.3.1 / 615.78.08. vmtest.sh now
builds in the guest (vm.sh build) when the host has no headers for the guest
kernel. #16 step 1 passed nvtest all 15/15 there (vmtest-2026-11-10_131303)
plus a guest trace during comproom (local/trace-logs/fragchk-132411):
vidmem_frag every ~10 s and after each batch, scan 22-30 us.

2026-11-10 13:45, driver 620.70.06: branch `local/vidmem-620` = tag
620.70.06 + one port commit (aaa57a24: the whole 615.78.08..0b994351 diff
applied with git apply -3; conflicts in mem.c, video_mem.c (SLI loop and
bcState gone upstream), nv-linux.h (upstream's own CXL guard taken),
nvidia-sources.Kbuild) + 7a383f22 (intermapGetDmaMapping without gpuMask,
no pNext / per-subdevice arrays). The per-commit history stays on
local/vidmem-615 (= 9723fcad). Guest upgraded (pacman -Syu with the pins
lifted, pins put back): kernel 7.3.3, nvidia-utils 620.70.06. nvtest all
15/15 PASS on the port (vmtest-2026-11-10_133003), no NVRM errors.
vidmem_frag and the status line work on 620 too. Work continues on
local/vidmem-620.

2026-11-10 afternoon (VM, driver 620.70.06). BACKLOG #5: the slow case does
not reproduce in the guest. `compr-test --chunk-mb 256` (new: the image sits
in a plain 256 MB vkAllocateMemory block, as DXVK does) gives exactly the
host's slow kind, 256 MB kind 0x8 on 64 KB pages, and it moves at ~19 GB/s
both ways (14 ms, 4200 CE launches of 64 KB, 3.4 us each), with a cleared
image or with noise (`--noise`), content intact. `local/compr-speed.sh` runs
the comparison; evict.sh evict/restore now print "copy: N launches, ms
submitting, ms waiting" (counters in ce_utils.c, uncommitted). So neither the
PTE kind nor the content is the cause; the host differs in something else.
Candidate: the host maps sysmem for the GPU through the IOMMU (DMA-FQ; 64 KB
chunks for 64 KB-page allocations), the guest's memory is one pinned block.
`NVVM_VIOMMU=1 vm.sh start` (new) gives the guest an emulated VT-d
(intel_iommu=on is now in the guest's boot entry; the GPU goes in without its
audio function, QEMU refuses two functions of one host group behind a vIOMMU)
to test that: NOT RUN YET. If it confirms: 2 MB sysmem chunks for 64 KB-page
allocations too (_vidmemEvictBegin, `chunk`).
The first try killed the GPU for the session: `vm.sh stop` got no ACPI
shutdown in 60 s and stopped QEMU with the driver running, which leaves GSP's
WPR2 up (same as 2026-11-06; guest then hangs in OVMF, with NVVM_ROMBAR=0 the
driver fails RmInitAdapter). Needs a host power cycle. vm.sh stop now powers
the guest off over ssh first and never kills QEMU by itself.
BACKLOG #6 (uncommitted, simulator only): hidden processes come back by
themselves only once the window they made room for has left the focus and
they would have fitted for VIDMEM_EVICT_HIDDEN_BACK_SEC (300 s) since;
scenario hidden-cycle.scn; replays 20261107_234433: 104 -> 49 moves,
20261109_173525: 191 -> 142. Needs `nvtest all` in the VM before the commit.
BACKLOG #16: vidmem_frag over a whole `nvtest all`
(local/trace-logs/frag-all-134619): the synthetic tests on a fresh guest do
not fragment vidmem (longest run 0.5-1.7 GB with 0.8-3 GB free), so the VM
needs a churn scenario (many processes, mixed sizes, starts and exits
interleaved, evicts and restores) before compaction can be judged there.

2026-11-10 14:45, vm.sh / win.sh: nothing ends a guest by force on its own
any more (the user: "second time we catch this"). `stop` powers off from
inside (ssh, then the root shell on the serial console, then ACPI), waits
180 s and otherwise leaves the guest running with an error; `reset` is a
reboot from inside (ssh, else serial console) and fails if the guest answers
on neither; `kill`, a hard reset and the kill after a failed stop need
`--force`. vmtest.sh's watchdog calls `vm.sh reset`, so it too no longer
hard-resets. Tested on a guest without the GPU (`start --no-gpu`): kill
refused, reset over ssh and over the serial console, stop in 2 s.
Side damage of the kill at 14:03: the guest's boot entry
(/boot/loader/entries/nvvm.conf, edited with sed -i a minute earlier, FAT)
came out empty, systemd-boot offered only the firmware. Recovered without
root on the image: EFI Internal Shell over the serial console
(`script -qfc "local/vm/vm.sh console" /dev/null` with the keys piped in),
`fs0:\vmlinuz-linux-cachyos initrd=... root=PARTUUID=...`, then the entry
rewritten and synced. After writing to the guest's /boot: `sync`.
The GPU is still wedged from 14:03: host reboot needed.

2026-11-10 15:40 (VM, after the host reboot; GPU fine). BACKLOG #6 committed
(da914ba9; nvtest all 14/15, vaflip hit a "Flip event timeout" WARNING in
nvidia-drm once, 3 reruns clean: BACKLOG I14). BACKLOG #16:
- local/fragchurn.sh [SECONDS] [moves]: random starts/exits of up to 24
  nvt-game --idle processes with 2-256 MB buffers; reproduces the host's
  picture (2.2-5.3 GB free, longest run ~1 GB). Evict/restore churn mostly
  adds partly used 2 MB chunks.
- _vidmemRelocate() (vidmem_evict.c): an allocation moves to other vidmem
  pages: new PMA pages (outside [avoidLo, avoidHi) when a window is being
  cleared), a sub-memdesc snapshot of the old pages as the CE copy source,
  mappings remade with their kinds (_vidmemEvictRemapOne), old pages freed.
  A mapping that cannot be remade leaves the old pages unfreed (leak, logged).
  Skips what the compositor maps. Mode 3 of _vidmemEvictPidLocked(), procfs
  "move PID" (rm_vidmem_evict mode 12), `evict.sh move`; trace op=move.
  local/move-test.sh: 10 moves each of rendering+rewriting nvt-game (compute
  and --gfx), an idle one, compr-test (2 MB and 64 KB pages, cleared and
  noise): no errors. ~1 GB in 120 ms on 64 KB pages (half of it PMA
  alloc/free of 16K pages), 2 GB on 2 MB pages in 127 ms.
- _vidmemEvictMakeContig() now moves a victim's allocations in the window
  inside vidmem first (mode 3 with the range set) and evicts to sysmem only
  what is left. Kernel log: "window cleared: N MB moved, M of them inside
  vidmem"; status line "windows cleared for contiguous allocations".
  local/contig-test.sh (interleaved 16 MB buffers, every other app exits,
  then 18 contiguous 256 MB scanout buffers as kwin_wayland): all granted,
  976 MB moved inside vidmem, 0 to sysmem, survivors intact. nvtest all 15/15
  (it hardly reaches this path: one 6 MB window in the whole run).
Next for #16: background compaction when idle; relocating compositor-mapped
allocations (two-phase like evict); letting the window clearing move (not
evict) drawing processes too. #5 still open: NVVM_VIOMMU=1 experiment not run
(needs a guest restart: `vm.sh stop` then `NVVM_VIOMMU=1 vm.sh start`).
2026-11-10 15:45: NVVM_VIOMMU=1 does boot the guest (DMAR on, the GPU in a
DMA domain), but the GPU then throws Xid 32 on kwin's channel with the stock
driver (72 in a minute, SDDM never comes up): not a usable way to imitate the
host's IOMMU. Back to the normal guest. guest.sh reload now stops
user@*.service when something still holds the GPU after SDDM stopped (a
Plasma session that outlived SDDM kept restarting kwin). #5 needs the host:
`evict.sh evict` there prints the "copy:" line (launches, submit vs wait).

2026-11-10 16:00, BACKLOG #16 step 3: background compaction.
vepPolicyStep() (hints mode), in a step where nothing moved and the last
pressure is VIDMEM_EVICT_QUIET_SEC old, calls vepPlatCompact(budget 32 MB,
8 MB next to a focused game; target _vidmemCompactRunMB = 1024, knob
`evict.sh compact MB`, procfs "compact", mode 13); a step that did nothing
rests 14 s. vepPlatCompact() (vidmem_evict.c): per-chunk used frames from the
PMA map; frames of allocations <= budget of processes that pass
_vidmemEvictHintVictimOk(VE_LEVEL_FG_IDLE) and are not compositor-mapped are
movable, any other used frame makes its chunk fixed; among stretches without
a fixed chunk that are longer than the current longest free run, a
two-pointer scan picks the longest window whose used frames fit the budget;
its allocations are relocated (mode 3) with the window as the range and the
existing longest run as a second range to keep free (_vidmemRelocateKeep*:
_vidmemRelocate() allocates in the gaps around both). Lessons from the first
tries: growing only the run that exists does nothing when a fixed chunk sits
at both ends; without the keep range the moved pages landed in the existing
run; a 30 s rest met the test apps' 30 s check every time.
Kernel log line when a step can do nothing ("compaction: longest free run
...: <why>"); status shows steps and MB moved. local/compact-test.sh [S] [MB].
2026-11-10 16:30: destination of a relocation. "Anywhere outside the window"
(PMA's choice) scattered the desktop's small long-lived buffers over vidmem:
idle guest 54 free runs / longest 2 GB (was 23 / 8.6 GB), and nvtest fgprio
then failed ("B not back": the game that fills vidmem found no contiguous
6 MB and kept evicting its own buffers through MakeContig, the #15/I13
weakness). _vidmemBestFitRange(): the smallest free run (from the PMA map,
outside the window and the kept run, aligned to the page size) that holds the
whole allocation; PMA is given that range; only if no single run fits, the
gaps around the avoided ranges. With a user reserve the block is avoided too.
After that nvtest all 15/15, move/contig/compact tests pass. Still true: what
was relocated while vidmem was full stays mid-vidmem afterwards (idle guest
longest run 4.4 GB after the test series vs 10.7 GB fresh); compaction only
works toward its 1 GB target. `vm.sh reload tree` restores the fresh layout.

2026-11-10 17:45. BACKLOG #5 root cause (commit 30b0c392): ceutilsMemcopy
called _ceutilsContigRun() for every launch; with one side contiguous for
long and the other in 64 KB pieces every launch walked up to 64K page
entries (O(n^2)). Now the run end is remembered (srcRunEnd/dstRunEnd).
Reproduced on a real game in the guest, not on synthetic apps: Steam in the
guest's KDE session (start it over ssh with XDG_RUNTIME_DIR, WAYLAND_DISPLAY,
DISPLAY from `pgrep -a Xwayland`, XAUTHORITY=/run/user/1000/xauth_*;
`steam -applaunch 3132990` = Black Myth Wukong Benchmark Tool, sits in its
menu with 2.2 GB). local/game-moves.sh PID (allocations by kind/page size,
evict/restore/move timings), local/game-seq.sh PID OP... (ops in a given
order). Before: move 410 ms / 4.2 GB/s, evict after a move 3.9-9 GB/s, 50 us
per launch in "submitting"; after: 16-27 GB/s. Host check on WoW pending.
Session run under nvtrace (local/trace-logs/session-171619 before the fix,
session2-172104 after: game, contig/compact/fragchurn with the game running,
nvtest all): 459 move batches, none under 6 GB/s, Xid 0. Findings for #16:
compaction does nothing next to a focused game (8 MB budget < buffer sizes);
the layout stays fragmented after load (idle desktop with Steam: 88 runs,
longest 1.2 GB of 10.6 free) and nvtest focus + comproom fail on such a
layout (pass after `reload tree`). A manual move right after a policy restore
is traced with why=restore (the reason global is not reset): cosmetic.
