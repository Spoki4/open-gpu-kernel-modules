#!/bin/bash
# Removes the locally patched NVIDIA modules and the reserve option, so the
# stock Canonical modules load again after a reboot. The MOK key stays enrolled.
set -euo pipefail

KVER=${KVER:-$(uname -r)}
[[ $EUID -eq 0 ]] || { echo "rollback.sh: run with sudo" >&2; exit 1; }

rm -rf "/lib/modules/$KVER/updates/nvidia-local"
rm -f /etc/modprobe.d/nvidia-vidmem-reserve.conf
depmod -a "$KVER"
update-initramfs -u -k "$KVER"

echo "modprobe will now load:"
modinfo -k "$KVER" -F filename nvidia
echo "Reboot to switch back to the stock modules."
