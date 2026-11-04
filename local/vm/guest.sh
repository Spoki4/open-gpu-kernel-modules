#!/bin/bash
# Runs inside the test VM (vm.sh calls it over ssh). Swaps the guest's NVIDIA
# driver without a reboot; no Secure Boot in the guest, so nothing is signed.
#
#   sudo ./guest.sh reload [tree|installed|stock]
#                           stop SDDM, unload nvidia*, load the chosen build, start SDDM
#                           tree:      the modules just built in ../../kernel-open
#                           installed: what boots (updates/nvidia-local, else stock)
#                           stock:     linux-cachyos-nvidia-open from the guest's packages
#   sudo ./guest.sh install         copy the tree build to updates/nvidia-local: boots from now on
#   sudo ./guest.sh install stock   remove it again
#   ./guest.sh status
#
# A build that hangs the guest: ./vm.sh reset on the host, the guest comes back
# with the installed build.
set -euo pipefail

KVER=$(uname -r)
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TREE="$HERE/../../kernel-open"
DEST=/usr/lib/modules/$KVER/updates/nvidia-local
DEPMOD_CONF=/etc/depmod.d/nvidia-local.conf
RESERVE_CONF=/etc/modprobe.d/nvidia-vidmem-reserve.conf
RESERVE_MB=${RESERVE_MB:-768}
MODULES=(nvidia nvidia-modeset nvidia-drm nvidia-uvm)
UNLOAD=(nvidia_drm nvidia_modeset nvidia_uvm nvidia)
MARK=/run/nvidia-local-loaded

die() { echo "guest.sh: $*" >&2; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run with sudo"; }

running_build() {
    local v
    v=$(sed -n '1s/.*Module for [^ ]* *\([0-9.]*\) .*) *\(.*\)$/\1 built \2/p' /proc/driver/nvidia/version 2>/dev/null)
    echo "nvidia ${v:-not loaded} ($(cat $MARK 2>/dev/null || echo installed))"
}

# The kernel side and the userspace driver must be the same release.
check_tree() {
    [[ -f $TREE/nvidia.ko ]] || die "nothing built in $TREE (./vm.sh build)"
    local built want
    built=$(modinfo -F version "$TREE/nvidia.ko")
    want=$(pacman -Q nvidia-utils | awk '{print $2}' | cut -d- -f1)
    [[ $built == "$want" ]] || die "tree is $built, guest userspace is nvidia-utils $want"
    [[ $(modinfo -F vermagic "$TREE/nvidia.ko" | cut -d' ' -f1) == "$KVER" ]] \
        || die "tree was built for another kernel (./vm.sh build builds for the guest's $KVER)"
}

mod_options() {
    modprobe -c | awk -v m="$1" '$1 == "options" && $2 == m { $1 = ""; $2 = ""; sub(/^ +/, ""); printf "%s ", $0 }'
}

gpu_holders() {
    fuser /dev/nvidia* /dev/dri/by-path/pci-*-card /dev/dri/by-path/pci-*-render 2>/dev/null \
        | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -un || true
}

load_tree() {
    modprobe -a drm drm_ttm_helper video drm_kms_helper 2>/dev/null || true
    local m opts
    for m in nvidia nvidia-modeset nvidia-drm nvidia-uvm; do
        opts=$(mod_options "${m//-/_}")
        eval insmod "$TREE/$m.ko" "$opts" || return 1
    done
}

reload() {
    need_root
    local slot=${1:-tree}
    case $slot in
    tree) check_tree ;;
    installed | stock) ;;
    *) die "usage: reload tree|installed|stock" ;;
    esac
    echo "was: $(running_build)"
    systemctl stop sddm
    sleep 1
    local holders
    holders=$(gpu_holders)
    if [[ -n $holders ]]; then
        # shellcheck disable=SC2086
        kill $holders 2>/dev/null || true
        sleep 2
        holders=$(gpu_holders)
        # shellcheck disable=SC2086
        [[ -z $holders ]] || { kill -9 $holders 2>/dev/null || true; sleep 1; }
    fi
    local m
    for m in "${UNLOAD[@]}"; do
        grep -q "^$m " /proc/modules || continue
        rmmod "$m" || die "rmmod $m failed (refcount $(cat /sys/module/$m/refcnt)); SDDM stays stopped"
    done
    case $slot in
    tree)
        if load_tree; then
            echo "tree: $(git -C "$HERE" log -1 --format='%h %s' 2>/dev/null | cut -c1-80)" >$MARK
        else
            echo "loading the tree build failed, loading the installed one" >&2
            for m in "${UNLOAD[@]}"; do rmmod "$m" 2>/dev/null || true; done
            modprobe nvidia_drm && modprobe nvidia_uvm
            rm -f $MARK
        fi
        ;;
    installed)
        modprobe nvidia_drm && modprobe nvidia_uvm
        rm -f $MARK
        ;;
    stock)
        local f
        for m in "${MODULES[@]}"; do
            f=$(ls /usr/lib/modules/"$KVER"/extramodules/"$m".ko* 2>/dev/null | head -1)
            [[ -n $f ]] || die "no stock $m (linux-cachyos-nvidia-open)"
            modprobe -a drm drm_ttm_helper video drm_kms_helper 2>/dev/null || true
            eval insmod "$f" "$(mod_options "${m//-/_}")"
        done
        echo "stock" >$MARK
        ;;
    esac
    systemctl start sddm
    echo "now: $(running_build)"
}

install_tree() {
    need_root
    if [[ ${1:-} == stock ]]; then
        rm -rf "$DEST" "$DEPMOD_CONF" "$RESERVE_CONF"
        depmod "$KVER"
        echo "boots the stock modules from now on"
        return
    fi
    check_tree
    install -d -m 0755 "$DEST"
    local m
    for m in "${MODULES[@]}"; do install -m 0644 "$TREE/$m.ko" "$DEST/$m.ko"; done
    # Stock modules live in extramodules/, which depmod would otherwise prefer.
    for m in "${MODULES[@]}"; do echo "override ${m//-/_} $KVER updates/nvidia-local"; done >"$DEPMOD_CONF"
    echo "options nvidia NVreg_RegistryDwords=\"RmVidmemUserReserveMB=$RESERVE_MB\"" >"$RESERVE_CONF"
    depmod "$KVER"
    echo "installed $(modinfo -F version "$DEST/nvidia.ko") into $DEST, boots from now on; reserve $RESERVE_MB MB"
}

case ${1:-} in
reload) reload "${2:-tree}" ;;
install) install_tree "${2:-}" ;;
status)
    echo "running: $(running_build)"
    echo "boots: $(modinfo -F filename nvidia 2>/dev/null)"
    ;;
*) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
