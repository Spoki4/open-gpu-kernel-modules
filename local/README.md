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
sudo ./local/safety.sh install           # watchdog + panic on lockups (before risky driver work)
sudo ./local/rollback.sh                 # back to stock modules, then reboot
```

`RESERVE_MB=1024 sudo ./local/install.sh install` changes the reserve.
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
./local/vm/vm.sh reset                   # hung guest
sudo ./local/vm/mode.sh host             # back to the 3080 Ti on the host after a reboot
```

The guest pins its kernel and `nvidia-utils`; the tree must be the same
release as the guest's `nvidia-utils` (`guest.sh` refuses otherwise). Guest
uses CCD0 (3D V-Cache, CPUs 0-7,16-23) and 24 GB; the host is limited to CCD1
while it runs (`NVVM_ISOLATE=0` to skip, `NVVM_MEM=16G` for less memory).
Timings differ a little from bare metal (IOMMU, nested paging): confirm final
numbers without the VM.
