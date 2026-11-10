#!/bin/bash
# Windows 11 guest with the RTX 3080 Ti passed through: the same games and
# benchmarks as the Linux test guest, on NVIDIA's Windows driver, for a
# comparison on the same hardware (same CPUs, memory size and GPU as
# vm.sh's guest; the two never run at once). Called through vm.sh, whose
# sudoers rule covers the root parts:
#
#   ./vm.sh win create ISO      unattended install from a Windows 11 ISO
#                               (~20-40 min, several reboots; the screen is
#                               the monitor on the 3080 Ti)
#   ./vm.sh win start | stop | kill | status
#   ./vm.sh win destroy         delete the guest's disk and firmware state (reinstall)
#   ./vm.sh win wait            until setup is done and ssh answers
#   ./vm.sh win ssh [CMD]       PowerShell over ssh (a service session: no desktop)
#   ./vm.sh win run 'PS...'     PowerShell in the desktop session (job runner,
#                               agent.ps1): games, installers with a UI
#   ./vm.sh win put FILE DEST   copy a file in (scp; DEST like C:/nvwin/x.exe)
#   ./vm.sh win get SRC FILE    copy a file out
#   ./vm.sh win input on|off    keyboard and mouse to the guest (both Ctrl switch)
#   ./vm.sh win qmp JSON
#   ./vm.sh win screen FILE.png  the emulated VGA's screen (during create only)
#
# Setup Windows: no TPM and no Secure Boot keys (LabConfig skips Setup's
# checks), AHCI disk and e1000e network (drivers in the box), the unattend
# files on a virtual FAT stick. Disk: sparse raw file $STATE/win/disk.img.
# The local account's password and the ssh key: ~/.local/share/nvwin
# (generated, never printed).
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
STATE=${NVVM_STATE:-/var/lib/nvidia-vm}/win
RUN=/run/nvidia-vm
GPU=0000:01:00.0
GPU_AUDIO=0000:01:00.1
MEM=${NVWIN_MEM:-24G}
DISK_SIZE=${NVWIN_DISK:-200G}
GUEST_CPUS=0-7,16-23
HOST_CPUS=8-15,24-31
SSH_PORT=2240
OVMF_CODE=/usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS=/usr/share/edk2/x64/OVMF_VARS.4m.fd
WIN_USER=bench
UDIR=${NVWIN_USER_DIR:-/home/spoki4/.local/share/nvwin}
KEY=$UDIR/id_ed25519
KBDS=(
    /dev/input/by-id/usb-0416_Gaming_Keyboard-event-kbd
    /dev/input/by-id/usb-0416_Gaming_Keyboard-event-if01
)
MOUSE_NAME="Logitech G703 LS (debounced)"
MOUSE_RAW=/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse

die() { echo "win.sh: $*" >&2; exit 1; }
# Root parts go through vm.sh (its sudoers rule), which execs us as root.
as_root() { [[ $EUID -eq 0 ]] || exec "$HERE/vm.sh" win _root "$@"; }
running() { systemctl is-active -q nvwin.service; }
qmp() {
    printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" \
        | socat -t2 - "UNIX-CONNECT:$RUN/win-qmp.sock" | tail -n +3
}
SSH=(ssh -p "$SSH_PORT" -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR
     -o UserKnownHostsFile="$UDIR/known_hosts" -o StrictHostKeyChecking=accept-new "$WIN_USER@127.0.0.1")

bind_vfio() {
    local dev=$1 drv
    modprobe vfio-pci
    drv=$(basename "$(readlink "/sys/bus/pci/devices/$dev/driver" 2>/dev/null)" 2>/dev/null || true)
    [[ $drv == vfio-pci ]] && return
    [[ $drv == nvidia ]] && die "$dev is driven by nvidia; run sudo ./mode.sh vm and reboot"
    echo vfio-pci >"/sys/bus/pci/devices/$dev/driver_override"
    [[ -n $drv ]] && echo "$dev" >"/sys/bus/pci/devices/$dev/driver/unbind"
    echo "$dev" >/sys/bus/pci/drivers_probe
}

isolate_host() {
    local unit
    for unit in user.slice system.slice init.scope; do
        systemctl set-property --runtime "$unit" "AllowedCPUs=$1"
    done
}

# $@: extra QEMU arguments (install media).
launch() {
    as_root _launch "$@"
    systemctl is-active -q nvvm.service && die "the Linux test guest has the GPU: ./vm.sh stop first"
    running && die "already running"
    [[ -f $STATE/disk.img ]] || die "no disk: ./vm.sh win create ISO"
    install -d -m 0755 "$RUN"
    bind_vfio "$GPU"
    bind_vfio "$GPU_AUDIO"
    local args=(
        -name nvwin,debug-threads=on
        -machine q35,accel=kvm,kernel-irqchip=on
        -cpu host,topoext=on,host-phys-bits=on,hv_relaxed,hv_vapic,hv_spinlocks=0x1fff,hv_time,hv_vpindex,hv_synic,hv_stimer,hv_frequencies,hv_runtime,hv_reset
        -smp 16,sockets=1,cores=8,threads=2
        -m "$MEM"
        -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,unit=1,file=$STATE/OVMF_VARS.4m.fd"
        -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=131072
        -device ahci,id=ahci
        -drive "file=$STATE/disk.img,if=none,id=disk,format=raw,cache=none,aio=io_uring,discard=unmap"
        -device ide-hd,drive=disk,bus=ahci.0,bootindex=1
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
        -device e1000e,netdev=net0
        -device qemu-xhci,id=xhci
        -device usb-kbd -device usb-mouse -device usb-tablet
        -rtc base=localtime
        -qmp "unix:$RUN/win-qmp.sock,server=on,wait=off"
        -nodefaults -display none -vga none
        -device pcie-root-port,id=rp1,chassis=1,slot=1
        -device "vfio-pci,host=$GPU,bus=rp1,addr=00.0,multifunction=on"
        -device "vfio-pci,host=$GPU_AUDIO,bus=rp1,addr=00.1"
        "$@"
    )
    isolate_host "$HOST_CPUS"
    systemd-run -q --unit=nvwin --slice=nvvm.slice --collect \
        -p LimitMEMLOCK=infinity -p "AllowedCPUs=$GUEST_CPUS" \
        -p "ExecStopPost=$HERE/vm.sh win _root _cleanup" \
        /usr/bin/qemu-system-x86_64 "${args[@]}"
    sleep 1
    running || { isolate_host ""; journalctl -u nvwin -n 20 --no-pager >&2; die "QEMU exited"; }
    echo "started; the screen is the monitor on the 3080 Ti"
}

create() {
    local iso=${1:?usage: vm.sh win create ISO}
    [[ -f $iso ]] || die "no ISO at $iso"
    iso=$(realpath "$iso")
    if [[ $EUID -ne 0 ]]; then
        # User part: password, key and the unattend stick contents.
        install -d -m 0700 "$UDIR"
        [[ -f $KEY ]] || ssh-keygen -q -t ed25519 -N "" -C "nvwin bench guest" -f "$KEY"
        [[ -f $UDIR/password ]] || ( umask 077; python3 -c 'import secrets; print(secrets.token_urlsafe(18))' >"$UDIR/password" )
        local stage=$UDIR/unattend
        rm -rf "$stage"; install -d -m 0700 "$stage"
        NVWIN_PASSWORD=$(cat "$UDIR/password") NVWIN_USER=$WIN_USER python3 - \
            "$HERE/win/autounattend.xml" "$stage/autounattend.xml" <<'EOF'
import os, sys
from xml.sax.saxutils import escape
t = open(sys.argv[1], encoding="utf-8").read()
t = t.replace("@USER@", escape(os.environ["NVWIN_USER"])).replace("@PASSWORD@", escape(os.environ["NVWIN_PASSWORD"]))
open(sys.argv[2], "w", encoding="utf-8").write(t)
EOF
        cp "$HERE/win/nvwin-setup.ps1" "$HERE/win/agent.ps1" "$stage/"
        cp "$KEY.pub" "$stage/authorized_keys"
        ( umask 077; printf '%s\r\n%s\r\n' "$WIN_USER" "$(cat "$UDIR/password")" >"$stage/user.txt" )
        exec "$HERE/vm.sh" win _root create "$iso"
    fi
    [[ -e $STATE/disk.img ]] && die "$STATE/disk.img exists: remove it by hand to reinstall"
    install -d -m 0700 "$STATE"
    truncate -s "$DISK_SIZE" "$STATE/disk.img"
    install -m 0644 "$OVMF_VARS" "$STATE/OVMF_VARS.4m.fd"
    # The unattend files as a FAT stick (Setup looks at removable media); usb-storage
    # wants a writable node (vvfat rw: writes would land in the staging directory).
    # An emulated VGA during the install only: its screen can be read over QMP
    # (screendump), the 3080 Ti's goes to the monitor alone.
    launch \
        -device VGA \
        -drive "id=cd0,file=$iso,if=none,media=cdrom,readonly=on" \
        -device ide-cd,drive=cd0,bus=ahci.1,bootindex=0 \
        -drive "id=ua,file=fat:rw:$UDIR/unattend,if=none,format=raw" \
        -device usb-storage,drive=ua,bus=xhci.0,removable=on
    # "Press any key to boot from CD or DVD": only while that prompt is up.
    local i
    for i in $(seq 20); do
        qmp '{"execute":"human-monitor-command","arguments":{"command-line":"sendkey ret"}}' >/dev/null || true
        sleep 0.3
    done
    echo "Windows is installing unattended (~20-40 min, several reboots); then: ./vm.sh win wait"
}

stop() {
    as_root stop "$@"
    running || { echo "not running"; return; }
    # Windows ignores the ACPI button here (seen twice): shut down from inside first.
    timeout 20 "${SSH[@]}" 'shutdown /s /t 0' </dev/null 2>/dev/null || true
    qmp '{"execute":"system_powerdown"}' >/dev/null
    local i
    for i in $(seq 180); do running || { echo "stopped"; return; }; sleep 1; done
    # Killing QEMU while the guest's GPU driver runs wedges the 3080 Ti until
    # the host is power cycled (vm.sh, 2026-11-10): only when asked.
    if [[ ${1:-} == --force ]]; then
        echo "no shutdown after 180 s, killing QEMU (--force)"
        systemctl stop nvwin.service
        return
    fi
    die "Windows did not shut down in 180 s and is left running (a kill can wedge the GPU until a host power cycle); ./vm.sh win kill --force only if it is dead"
}

kill_vm() {
    running || { echo "not running"; return; }
    [[ ${1:-} == --force ]] || die "not killing a running guest (that can wedge the GPU until a host power cycle): ./vm.sh win stop, or kill --force if it is dead"
    systemctl stop nvwin.service
}

input() {
    as_root input "$@"
    running || die "not running"
    case ${1:-} in
    on)
        local i mouse=$MOUSE_RAW n
        for i in "${!KBDS[@]}"; do
            qmp '{"execute":"object-add","arguments":{"qom-type":"input-linux","id":"kbd'"$i"'","evdev":"'"${KBDS[$i]}"'","grab_all":true,"repeat":true}}'
        done
        for n in /sys/class/input/event*/device/name; do
            [[ $(<"$n") == "$MOUSE_NAME" ]] && mouse=/dev/input/$(basename "$(dirname "$(dirname "$n")")")
        done
        qmp '{"execute":"object-add","arguments":{"qom-type":"input-linux","id":"mouse","evdev":"'"$mouse"'"}}'
        echo "keyboard and mouse belong to Windows now; both Ctrl switch back and forth"
        ;;
    off)
        local i
        for i in "${!KBDS[@]}"; do qmp '{"execute":"object-del","arguments":{"id":"kbd'"$i"'"}}'; done
        qmp '{"execute":"object-del","arguments":{"id":"mouse"}}'
        ;;
    *) die "usage: input on|off" ;;
    esac
}

wait_ready() {
    local i
    for i in $(seq 720); do
        if "${SSH[@]}" 'if ((Test-Path C:\nvwin\setup.done) -and (Get-ScheduledTask nvwin-agent).State -eq "Running") { exit 0 } else { exit 1 }' </dev/null 2>/dev/null; then
            echo "Windows is ready"; return
        fi
        sleep 5
    done
    die "not ready after 60 min (look at the monitor)"
}

# PowerShell in the desktop session through the job runner (agent.ps1):
# the script goes into C:\nvwin\jobs, its output and exit code come back.
run() {
    local id="j$$_${RANDOM}" body
    body=$(printf '$ErrorActionPreference = "Stop"\n$ProgressPreference = "SilentlyContinue"\n%s\n' "$*")
    "${SSH[@]}" "\$input | Set-Content -Encoding UTF8 C:\\nvwin\\jobs\\$id.tmp; Rename-Item C:\\nvwin\\jobs\\$id.tmp $id.ps1" <<<"$body"
    local rc
    while :; do
        rc=$("${SSH[@]}" "if (Test-Path C:\\nvwin\\jobs\\$id.rc) { Get-Content C:\\nvwin\\jobs\\$id.rc }" </dev/null 2>/dev/null | tr -d '\r' || true)
        [[ -n $rc ]] && break
        sleep 1
    done
    "${SSH[@]}" "Get-Content C:\\nvwin\\jobs\\$id.out; Remove-Item C:\\nvwin\\jobs\\$id.*" </dev/null | tr -d '\r'
    return "$rc"
}

cmd=${1:-}
shift || true
case $cmd in
create) create "$@" ;;
start) launch ;;
stop) stop "$@" ;;
kill) as_root kill "$@"; kill_vm "$@" ;;
destroy) as_root destroy; running && die "running: stop it first"; rm -f "$STATE/disk.img" "$STATE/OVMF_VARS.4m.fd"; echo "removed $STATE/disk.img" ;;
status)
    if running; then echo "running since $(systemctl show -p ActiveEnterTimestamp --value nvwin.service)"; else echo "not running"; fi
    if "${SSH[@]}" 'exit 0' </dev/null 2>/dev/null; then echo "ssh: up"; else echo "ssh: down"; fi
    ;;
wait) wait_ready ;;
ssh) exec "${SSH[@]/BatchMode=yes/BatchMode=no}" "$@" ;;
run) run "$@" ;;
put) scp -q -o LogLevel=ERROR -P "$SSH_PORT" -i "$KEY" -o IdentitiesOnly=yes -o UserKnownHostsFile="$UDIR/known_hosts" "$1" "$WIN_USER@127.0.0.1:$2" ;;
get) scp -q -o LogLevel=ERROR -P "$SSH_PORT" -i "$KEY" -o IdentitiesOnly=yes -o UserKnownHostsFile="$UDIR/known_hosts" "$WIN_USER@127.0.0.1:$1" "$2" ;;
input) input "$@" ;;
qmp) as_root qmp "$@"; qmp "$1" ;;
# A frame of the emulated VGA (install only): screen FILE.png
screen) as_root screen "$@"; qmp '{"execute":"screendump","arguments":{"filename":"'"$1"'","format":"png"}}' ;;
# Root re-entry from vm.sh: _root CMD ARGS.
_root)
    [[ $EUID -eq 0 ]] || die "_root is for vm.sh"
    sub=${1:-}; shift || true
    case $sub in
    _launch) launch "$@" ;;
    create) create "$@" ;;
    stop) stop "$@" ;;
    kill) kill_vm "$@" ;;
    destroy) running && die "running: stop it first"; rm -f "$STATE/disk.img" "$STATE/OVMF_VARS.4m.fd"; echo "removed $STATE/disk.img" ;;
    input) input "$@" ;;
    qmp) qmp "$1" ;;
    screen) qmp '{"execute":"screendump","arguments":{"filename":"'"$1"'","format":"png"}}' ;;
    _cleanup) isolate_host "" ;;
    *) die "unknown root command $sub" ;;
    esac
    ;;
*) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
