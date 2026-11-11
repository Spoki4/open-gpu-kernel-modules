#!/bin/bash
exec >~/ab.log 2>&1
export NVT_CHROME_FLAGS="--enable-unsafe-webgpu --enable-features=Vulkan" NVTEST_FORCE=1
load() { sudo rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia 2>/dev/null; sudo insmod $1/nvidia.ko && sudo insmod $1/nvidia-modeset.ko && sudo insmod $1/nvidia-drm.ko modeset=1; echo "== loaded $2: $(head -1 /proc/driver/nvidia/version | grep -o "Wed.*")"; }
cd ~/open-gpu-kernel-modules/local
for b in "old $HOME/old-67deb430/kernel-open" "new $HOME/open-gpu-kernel-modules/kernel-open"; do
    set -- $b; load $2 $1
    for i in 1 2 3; do
        timeout 300 ./nvtest.sh active 2>&1 | grep -E "witness worst|^NVTEST" | sed "s/^/$1 active $i: /" | cut -c1-150
    done
    for i in 1 2; do
        timeout 500 ./nvtest.sh fgprio 3 idle 2>&1 | grep -E "click B|^NVTEST" | sed "s/^/$1 fgprio $i: /" | cut -c1-150
    done
done
echo "kernel:"; sudo dmesg | grep -E "Xid|BUG:|Oops|general protection" | tail -5
echo ABRUN DONE
