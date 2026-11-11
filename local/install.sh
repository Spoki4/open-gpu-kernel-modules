#!/bin/bash
# Makes the modules built in ../kernel-open the ones that load at boot
# (Arch / CachyOS: stock modules come from the linux-*-nvidia-open package and
# stay in place, ours go beside them and win).
#
#   sudo ./install.sh           copy the build to updates/nvidia-local; reboot to use it
#   sudo ./install.sh stock     remove it again; reboot to be back on the stock modules
#   ./install.sh status         which build runs now and which one boots
#
# Nothing is signed: with Secure Boot the kernel must accept unsigned modules
# (no lockdown / module.sig_enforce), as CachyOS does by default.
# RESERVE_MB=768 sudo ./install.sh also keeps a fixed block of vidmem for the
# compositor (the first approach; 0, the default, relies on eviction instead).
set -euo pipefail

KVER=${KVER:-$(uname -r)}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TREE="$HERE/../kernel-open"
DEST=/usr/lib/modules/$KVER/updates/nvidia-local
DEPMOD_CONF=/etc/depmod.d/nvidia-local.conf
RESERVE_CONF=/etc/modprobe.d/nvidia-vidmem-reserve.conf
RESERVE_MB=${RESERVE_MB:-0}
MODULES=(nvidia nvidia-modeset nvidia-drm nvidia-uvm)

die() { echo "install.sh: $*" >&2; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run with sudo"; }

case ${1:-install} in
install)
    need_root
    [[ -f $TREE/nvidia.ko ]] || die "nothing built in $TREE (./build.sh)"
    # The kernel side and the userspace driver must be the same release.
    built=$(modinfo -F version "$TREE/nvidia.ko")
    want=$(pacman -Q nvidia-utils | awk '{print $2}' | cut -d- -f1)
    [[ $built == "$want" ]] || die "the build is $built, userspace is nvidia-utils $want"
    [[ $(modinfo -F vermagic "$TREE/nvidia.ko" | cut -d' ' -f1) == "$KVER" ]] \
        || die "the build is for another kernel, not $KVER"
    install -d -m 0755 "$DEST"
    for m in "${MODULES[@]}"; do install -m 0644 "$TREE/$m.ko" "$DEST/$m.ko"; done
    # Stock modules live in extramodules/, which depmod would otherwise prefer.
    for m in "${MODULES[@]}"; do echo "override ${m//-/_} $KVER updates/nvidia-local"; done >"$DEPMOD_CONF"
    echo "options nvidia NVreg_RegistryDwords=\"RmVidmemUserReserveMB=$RESERVE_MB\"" >"$RESERVE_CONF"
    depmod "$KVER"
    echo "installed $built into $DEST, boots from now on; reserve $RESERVE_MB MB"
    ;;
stock)
    need_root
    rm -rf "$DEST" "$DEPMOD_CONF" "$RESERVE_CONF"
    depmod "$KVER"
    echo "boots the stock modules from now on"
    ;;
status)
    echo "running: $(sed -n '1s/.*Module for [^ ]* *\([0-9.]*\) .*) *\(.*\)$/\1 built \2/p' /proc/driver/nvidia/version 2>/dev/null || true)"
    echo "boots:   $(modinfo -F filename nvidia 2>/dev/null)"
    ;;
*) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
