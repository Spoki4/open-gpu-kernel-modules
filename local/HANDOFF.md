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

Layout:
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
  (`IgnorePkg`) at 7.3.1-1 / 615.78.08; update them together on purpose only.
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
