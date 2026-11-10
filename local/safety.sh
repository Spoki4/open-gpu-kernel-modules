#!/bin/bash
# Safety net for driver development on this PC: a hang should end in an
# automatic reboot with the kernel log kept, not in a frozen machine that needs
# someone at the power button.
#
#   sudo ./safety.sh install   # hardware watchdog + panic on lockups (persistent)
#   sudo ./safety.sh remove    # undo everything install did
#   ./safety.sh status         # what is active right now (no root needed)
#
# What install does:
#   - loads sp5100_tco (AMD chipset watchdog) now and at boot
#     (/etc/modules-load.d/nvidia-local-watchdog.conf)
#   - lets systemd feed it: RuntimeWatchdogSec=30s
#     (/etc/systemd/system.conf.d/nvidia-local-watchdog.conf, applied with
#     daemon-reexec): if the kernel stops scheduling PID 1 for 30 s, the board
#     resets
#   - sysctl (/etc/sysctl.d/90-nvidia-local-dev.conf): panic on soft/hard
#     lockups and oopses, reboot 10 s after a panic. efi_pstore keeps the panic
#     log; systemd-pstore copies it to /var/lib/systemd/pstore on the next boot.
#     hung_task_panic stays off: long D-state waits are normal on a desktop.
set -euo pipefail

MODLOAD=/etc/modules-load.d/nvidia-local-watchdog.conf
SYSTEMD_CONF=/etc/systemd/system.conf.d/nvidia-local-watchdog.conf
SYSCTL_CONF=/etc/sysctl.d/90-nvidia-local-dev.conf

die() { echo "safety.sh: $*" >&2; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run with sudo"; }

status() {
    echo "watchdog devices: $(ls /dev/watchdog* 2>/dev/null | tr '\n' ' ' || true)"
    for w in /sys/class/watchdog/watchdog*; do
        [[ -d $w ]] || continue
        echo "  $(basename "$w"): $(cat "$w/identity" 2>/dev/null), timeout $(cat "$w/timeout" 2>/dev/null) s, state $(cat "$w/state" 2>/dev/null)"
    done
    echo "systemd RuntimeWatchdogUSec: $(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null)"
    sysctl kernel.panic kernel.panic_on_oops kernel.softlockup_panic kernel.hardlockup_panic \
        kernel.hung_task_panic 2>/dev/null | sed 's/^/sysctl /'
    echo "pstore backend: $(cat /sys/module/pstore/parameters/backend 2>/dev/null || echo none)"
    for f in "$MODLOAD" "$SYSTEMD_CONF" "$SYSCTL_CONF"; do
        [[ -f $f ]] && echo "installed: $f" || echo "missing:   $f"
    done
}

install() {
    need_root
    modprobe sp5100_tco || die "sp5100_tco did not load (check dmesg); nothing else changed"
    [[ -e /dev/watchdog0 || -e /dev/watchdog ]] || die "sp5100_tco loaded but no /dev/watchdog appeared"
    echo sp5100_tco >"$MODLOAD"

    mkdir -p "$(dirname "$SYSTEMD_CONF")"
    cat >"$SYSTEMD_CONF" <<'CONF'
# Written by local/safety.sh: reset the machine if PID 1 stops for 30 s.
[Manager]
RuntimeWatchdogSec=30s
RebootWatchdogSec=2min
CONF
    systemctl daemon-reexec

    cat >"$SYSCTL_CONF" <<'CONF'
# Written by local/safety.sh: turn lockups into a panic, and a panic into a reboot.
kernel.panic = 10
kernel.panic_on_oops = 1
kernel.softlockup_panic = 1
kernel.hardlockup_panic = 1
CONF
    sysctl -q -p "$SYSCTL_CONF"

    status
}

remove() {
    need_root
    rm -f "$MODLOAD" "$SYSTEMD_CONF" "$SYSCTL_CONF"
    systemctl daemon-reexec
    sysctl -q kernel.panic=0 kernel.panic_on_oops=0 kernel.softlockup_panic=0 kernel.hardlockup_panic=0
    # The watchdog module stays loaded until the next boot; with systemd no
    # longer feeding it, it is only armed if something opens /dev/watchdog.
    status
}

case ${1:-} in
    install) install ;;
    remove) remove ;;
    status) status ;;
    *) echo "usage: sudo $0 install|remove, or $0 status" >&2; exit 2 ;;
esac
