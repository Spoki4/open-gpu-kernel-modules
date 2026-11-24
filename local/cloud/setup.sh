#!/bin/bash
# Build deps, our modules, the matching NVIDIA userspace. Log: ~/setup.log; ends with SETUP DONE|FAIL.
exec >~/setup.log 2>&1
set -x
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -q || { echo SETUP FAIL apt-update; exit 1; }
sudo apt-get install -y -q build-essential linux-headers-$(uname -r) pkg-config libelf-dev kmod git rsync \
    libvulkan-dev vulkan-tools glslang-tools libgbm-dev libdrm-dev libegl-dev libgles-dev || { echo SETUP FAIL apt-install; exit 1; }
cd ~/open-gpu-kernel-modules
make modules -j4 >~/build.log 2>&1 || { tail -30 ~/build.log; echo SETUP FAIL build; exit 1; }
ls -la kernel-open/*.ko
# The userspace of the release the tree is (version.mk).
v=$(sed -n 's/^NVIDIA_VERSION = //p' version.mk)
cd ~ && curl -fsSLO "https://us.download.nvidia.com/XFree86/Linux-x86_64/$v/NVIDIA-Linux-x86_64-$v.run" || { echo SETUP FAIL download; exit 1; }
ls -la "NVIDIA-Linux-x86_64-$v.run"
echo SETUP DONE
