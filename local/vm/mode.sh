#!/bin/bash
# Who gets the RTX 3080 Ti after the next boot.
#
#   sudo ./mode.sh vm       the host never loads nvidia, vfio-pci takes the card
#                           (desktop on the AMD iGPU, the 3080 Ti is for the VM)
#   sudo ./mode.sh host     back to normal: the host's nvidia drives the card
#   ./mode.sh status
#
# Takes effect after a reboot. The initramfs has no GPU modules (no kms hook),
# so the files below are read when udev loads drivers from the root fs.
set -euo pipefail

MODPROBE_CONF=/etc/modprobe.d/nvidia-vm-mode.conf
LOAD_CONF=/etc/modules-load.d/nvidia-vm-mode.conf
GPU=0000:01:00.0

die() { echo "mode.sh: $*" >&2; exit 1; }

status() {
    if [[ -f $MODPROBE_CONF ]]; then echo "next boot: vm"; else echo "next boot: host"; fi
    echo "3080 Ti now: $(basename "$(readlink /sys/bus/pci/devices/$GPU/driver 2>/dev/null)" 2>/dev/null || echo none)"
}

case ${1:-} in
vm)
    [[ $EUID -eq 0 ]] || die "run with sudo"
    cat >"$MODPROBE_CONF" <<EOF
# local/vm/mode.sh vm: the RTX 3080 Ti (and its HDMI audio) belongs to vfio-pci,
# the host never loads nvidia. Undo: sudo local/vm/mode.sh host, reboot.
options vfio-pci ids=10de:2208,10de:1aef
softdep snd_hda_intel pre: vfio-pci
install nvidia /bin/false
install nvidia_modeset /bin/false
install nvidia_drm /bin/false
install nvidia_uvm /bin/false
install nvidia_peermem /bin/false
install nouveau /bin/false
install nova_core /bin/false
EOF
    echo vfio-pci >"$LOAD_CONF"
    status
    echo "reboot to apply"
    ;;
host)
    [[ $EUID -eq 0 ]] || die "run with sudo"
    rm -f "$MODPROBE_CONF" "$LOAD_CONF"
    status
    echo "reboot to apply"
    ;;
status) status ;;
*) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
