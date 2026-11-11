#!/bin/bash
# Root half of the unattended test bench (the other half is nvtest.sh, run as
# the desktop user). Lets Claude swap driver builds without a reboot and
# without anybody at the keyboard, while a build that hangs the machine can
# never take over the next boot.
#
#   sudo ./nvtest-root.sh stage           # sign the freshly built modules (../kernel-open)
#                                         # into the candidate slot
#   sudo ./nvtest-root.sh reload candidate|installed [--kill]
#                                         # swap the running driver: stops SDDM, unloads
#                                         # nvidia*, loads the chosen slot, starts SDDM
#   sudo ./nvtest-root.sh promote         # make the candidate the installed build
#                                         # (what boots), via install.sh
#   ./nvtest-root.sh status               # which build runs, slots, who holds the GPU
#
# Two slots:
#   installed  /lib/modules/$KVER/updates/nvidia-local (install.sh), loaded at boot
#   candidate  /var/lib/nvidia-local/candidate, loaded ONLY by `reload candidate`
# A candidate that panics or hangs the kernel gets the board reset by the
# hardware watchdog (safety.sh), and the next boot loads the installed build.
#
# reload refuses while a user is logged in on seat0 (it would kill the
# session) and while non-system processes still hold /dev/nvidia* (--kill
# terminates them; they belong to a session that has already logged out).
set -euo pipefail

KVER=${KVER:-$(uname -r)}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD="$HERE/../kernel-open"
KEYDIR=/var/lib/nvidia-local-mok
SIGN=/usr/src/linux-headers-$KVER/scripts/sign-file
SLOT_ROOT=/var/lib/nvidia-local
CAND=$SLOT_ROOT/candidate          # signed, loadable
CAND_SRC=$SLOT_ROOT/candidate-src  # unsigned copies, input for promote (install.sh signs)
LOADED_MARK=/run/nvidia-local-loaded # which slot reload loaded (gone after a reboot: installed)
MODULES=(nvidia nvidia-modeset nvidia-drm nvidia-uvm nvidia-peermem)
UNLOAD=(nvidia_peermem nvidia_drm nvidia_modeset nvidia_uvm nvidia)
# Services that hold the GPU, stopped before the unload, started after.
SERVICES_STOP=(sddm nvidia-persistenced)
SERVICES_START=(nvidia-persistenced nvidia-memclk sddm)
LOG_TAG=nvtest-root
# While a candidate loads, udev must not pull nvidia-drm/uvm in from the
# installed slot (2026-11-02 11:32: "insmod: File exists" for both).
BLOCK_CONF=/etc/modprobe.d/zz-nvtest-reload-block.conf

die() { echo "nvtest-root.sh: $*" >&2; logger -t "$LOG_TAG" "FAILED: $*" 2>/dev/null || true; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run with sudo"; }
log() { echo "$*"; logger -t "$LOG_TAG" "$*" 2>/dev/null || true; }

# The version line names the build host and time, which tells builds apart
# (srcversion only covers kernel-open/, not the RM core).
running_build() {
    local v
    v=$(sed -n '1s/.*Module for [^ ]* *\([0-9.]*\) .*) *\(.*\)$/\1 built \2/p' /proc/driver/nvidia/version 2>/dev/null)
    echo "nvidia ${v:-not loaded}, slot $(cat $LOADED_MARK 2>/dev/null || echo installed)"
}

seat0_sessions() {
    local s
    for s in $(loginctl list-sessions --no-legend | awk '{print $1}'); do
        [[ $(loginctl show-session "$s" -p Seat --value 2>/dev/null) == seat0 ]] || continue
        [[ $(loginctl show-session "$s" -p Class --value 2>/dev/null) == user ]] || continue
        [[ $(loginctl show-session "$s" -p State --value 2>/dev/null) == closing ]] && continue
        echo "$s($(loginctl show-session "$s" -p Name --value))"
    done
}

# PIDs holding /dev/nvidia* or the NVIDIA DRM nodes, excluding the services
# reload stops itself.
gpu_holders() {
    local nodes=(/dev/nvidia*)
    local p
    for p in /dev/dri/by-path/pci-*-card /dev/dri/by-path/pci-*-render; do
        [[ -e $p ]] && nodes+=("$(readlink -f "$p")")
    done
    fuser "${nodes[@]}" 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -un || true
}

status() {
    echo "running:   $(running_build)"
    echo "installed: $(modinfo -k "$KVER" -F filename nvidia 2>/dev/null) srcversion $(modinfo -k "$KVER" -F srcversion nvidia 2>/dev/null)"
    if [[ -f $CAND/nvidia.ko ]]; then
        echo "candidate: $CAND/nvidia.ko srcversion $(modinfo -F srcversion "$CAND/nvidia.ko") staged $(stat -c %y "$CAND/nvidia.ko" | cut -d. -f1) ($(cat "$CAND/BUILD" 2>/dev/null))"
    else
        echo "candidate: none"
    fi
    echo "seat0 sessions: $(seat0_sessions | tr '\n' ' ')"
    echo "watchdog: $(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null), devices $(ls /dev/watchdog* 2>/dev/null | tr '\n' ' ')"
}

stage() {
    need_root
    [[ -f $KEYDIR/MOK.der ]] || die "no signing key in $KEYDIR (install.sh enroll-key)"
    [[ -x $SIGN ]] || die "missing $SIGN"
    local version want m
    version=$(modinfo -F version "$BUILD/nvidia.ko")
    want=$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}\n' 'libnvidia-gl-*:amd64' 2>/dev/null | awk '$1 == "ii" {print $2}' | cut -d- -f1 | sort -V | tail -1)
    [[ $version == "$want" ]] || die "built $version but userspace is $want"
    [[ $(modinfo -F vermagic "$BUILD/nvidia.ko" | cut -d' ' -f1) == "$KVER" ]] || die "modules were built for another kernel"
    install -d -m 0755 "$CAND" "$CAND_SRC"
    for m in "${MODULES[@]}"; do
        install -m 0644 "$BUILD/$m.ko" "$CAND_SRC/$m.ko"
        install -m 0644 "$BUILD/$m.ko" "$CAND/$m.ko"
        strip --strip-debug "$CAND/$m.ko"
        "$SIGN" sha256 "$KEYDIR/MOK.priv" "$KEYDIR/MOK.der" "$CAND/$m.ko"
    done
    git -C "$HERE/.." log -1 --format='%h %s' 2>/dev/null | cut -c1-100 >"$CAND/BUILD" || true
    log "staged candidate: $(cat "$CAND/BUILD"), srcversion $(modinfo -F srcversion "$CAND/nvidia.ko")"
}

# The options modprobe would pass for module $1 (from /etc/modprobe.d), so the
# candidate gets the same reserve and nvidia-drm settings as the boot build.
mod_options() {
    modprobe -c | awk -v m="$1" '$1 == "options" && $2 == m { $1 = ""; $2 = ""; sub(/^ +/, ""); printf "%s ", $0 }'
}

load_slot() {
    local slot=$1
    if [[ $slot == installed ]]; then
        modprobe nvidia_drm
        modprobe nvidia_uvm
        rm -f "$LOADED_MARK"
        return
    fi
    [[ -f $CAND/nvidia.ko ]] || die "no candidate staged"
    # Dependencies of the nvidia modules outside this package.
    modprobe -a drm drm_ttm_helper video drm_kms_helper 2>/dev/null || true
    local opts_nvidia opts_modeset opts_drm opts_uvm
    opts_nvidia=$(mod_options nvidia)
    opts_modeset=$(mod_options nvidia_modeset)
    opts_drm=$(mod_options nvidia_drm)
    opts_uvm=$(mod_options nvidia_uvm)
    printf 'install %s /bin/true\n' nvidia nvidia_modeset nvidia_drm nvidia_uvm nvidia_peermem >"$BLOCK_CONF"
    local ok=1
    eval insmod "$CAND/nvidia.ko" "$opts_nvidia" || ok=0
    [[ $ok == 1 ]] && { eval insmod "$CAND/nvidia-modeset.ko" "$opts_modeset" || ok=0; }
    [[ $ok == 1 ]] && { eval insmod "$CAND/nvidia-drm.ko" "$opts_drm" || ok=0; }
    [[ $ok == 1 ]] && { eval insmod "$CAND/nvidia-uvm.ko" "$opts_uvm" || ok=0; }
    rm -f "$BLOCK_CONF"
    [[ $ok == 1 ]] || return 1
    echo "candidate: $(cat "$CAND/BUILD" 2>/dev/null)" >"$LOADED_MARK"
}

reload() {
    need_root
    local slot=${1:-} kill=${2:-}
    [[ $slot == candidate || $slot == installed ]] || die "usage: reload candidate|installed [--kill]"
    local sessions
    sessions=$(seat0_sessions)
    [[ -z $sessions ]] || die "a user is logged in on seat0 ($sessions); log out first"

    log "reload $slot: was $(running_build)"
    local s
    for s in "${SERVICES_STOP[@]}"; do systemctl stop "$s" 2>/dev/null || true; done
    sleep 1

    local holders
    holders=$(gpu_holders)
    if [[ -n $holders ]]; then
        if [[ $kill == --kill ]]; then
            log "killing GPU holders: $(ps -o pid=,comm= -p "${holders//$'\n'/,}" | tr '\n' ';')"
            # shellcheck disable=SC2086
            kill $holders 2>/dev/null || true
            sleep 3
            holders=$(gpu_holders)
            # shellcheck disable=SC2086
            [[ -z $holders ]] || { kill -9 $holders 2>/dev/null || true; sleep 1; }
        fi
        holders=$(gpu_holders)
        if [[ -n $holders ]]; then
            ps -o pid=,user=,comm= -p "${holders//$'\n'/,}" >&2 || true
            for s in "${SERVICES_START[@]}"; do systemctl start "$s" 2>/dev/null || true; done
            die "processes above still hold the GPU (use --kill); services restarted, driver unchanged"
        fi
    fi

    local m
    for m in "${UNLOAD[@]}"; do
        if grep -q "^$m " /proc/modules; then
            rmmod "$m" || {
                load_slot installed || true
                for s in "${SERVICES_START[@]}"; do systemctl start "$s" 2>/dev/null || true; done
                die "rmmod $m failed (refcount $(cat /sys/module/$m/refcnt 2>/dev/null)); reloaded the installed build"
            }
        fi
    done

    if ! load_slot "$slot"; then
        log "loading $slot failed, falling back to installed"
        for m in "${UNLOAD[@]}"; do rmmod "$m" 2>/dev/null || true; done
        load_slot installed
    fi
    for s in "${SERVICES_START[@]}"; do systemctl start "$s" 2>/dev/null || true; done
    log "reload $slot: now $(running_build)"
}

promote() {
    need_root
    [[ -f $CAND_SRC/nvidia.ko ]] || die "no candidate staged"
    BUILD="$CAND_SRC" "$HERE/install.sh" install
    cp "$CAND/BUILD" "$SLOT_ROOT/installed-BUILD" 2>/dev/null || true
    log "promoted candidate to installed: $(cat "$CAND/BUILD" 2>/dev/null)"
}

case ${1:-} in
    stage) stage ;;
    reload) reload "${2:-}" "${3:-}" ;;
    promote) promote ;;
    status) status ;;
    *) echo "usage: sudo $0 stage | reload candidate|installed [--kill] | promote, or $0 status" >&2; exit 2 ;;
esac
