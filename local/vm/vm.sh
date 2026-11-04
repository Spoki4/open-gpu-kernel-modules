#!/bin/bash
# Test VM with the RTX 3080 Ti passed through, so a driver build that hangs or
# panics only takes the guest down. The host keeps its desktop on the AMD iGPU
# (one monitor on the motherboard), the guest drives the monitor on the 3080 Ti
# with its own KDE Plasma on Wayland. Prerequisites: sudo ./setup.sh once,
# sudo ./mode.sh vm and a reboot (the host must not load nvidia).
#
#   ./vm.sh start [--no-gpu]     boot the guest (--no-gpu: without the 3080 Ti,
#                                to test the VM itself while the host still uses it)
#   ./vm.sh stop                 ACPI shutdown, killed after 60 s
#   ./vm.sh reset                hard reset (a hung driver)
#   ./vm.sh kill                 stop QEMU at once
#   ./vm.sh status
#   ./vm.sh input on|off         hand keyboard and mouse to the guest (both Ctrl
#                                switch between host and guest while on)
#   ./vm.sh console              guest serial console (root shell; Ctrl-] quits)
#   ./vm.sh log                  follow the serial log (kernel messages survive a hang)
#   ./vm.sh qmp JSON             one QMP command, e.g. '{"execute":"qom-list","arguments":{"path":"/objects"}}'
#   ./vm.sh ssh [CMD...]         shell or command in the guest as spoki4
#   ./vm.sh wait                 wait until the guest answers ssh
#   ./vm.sh build                build the modules in the guest, for the guest kernel
#   ./vm.sh reload [tree|installed|stock]
#                                swap the guest's driver without a reboot (guest.sh)
#   ./vm.sh install [stock]      make the tree build the one the guest boots
#
# The repository is shared into the guest via virtiofs at the same path, so
# edits on the host are visible there at once. QEMU runs as root (VFIO pins
# all guest memory) in the transient unit nvvm.service; the root subcommands
# re-run this script with sudo -n (rule from setup.sh).
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$HERE/../.." && pwd)
STATE=${NVVM_STATE:-/var/lib/nvidia-vm}
RUN=/run/nvidia-vm
IMG=$STATE/guest.img
GPU=0000:01:00.0
GPU_AUDIO=0000:01:00.1
MEM=${NVVM_MEM:-24G}
# The CCD with the 3D V-Cache (L3 96 MB) goes to the guest, the host keeps the
# other one while the VM runs (NVVM_ISOLATE=0 leaves the host on all cores).
GUEST_CPUS=0-7,16-23
HOST_CPUS=8-15,24-31
ISOLATE=${NVVM_ISOLATE:-1}
# The keyboard has two key interfaces: the boot one (6KRO) and the NKRO one on
# input1, which carries the keys in NKRO mode.
KBDS=(
    /dev/input/by-id/usb-0416_Gaming_Keyboard-event-kbd
    /dev/input/by-id/usb-0416_Gaming_Keyboard-event-if01
)
# wheel-debounce.service grabs the G703's own node and re-emits through uinput
# as "... (debounced)", which has no by-id link: found by name.
MOUSE_NAME="Logitech G703 LS (debounced)"
MOUSE_RAW=/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse
SSH_PORT=2230   # 2222 is taken by the vsos-winbuild VM
OVMF_CODE=/usr/share/edk2/x64/OVMF_CODE.4m.fd
GUEST_USER=spoki4

die() { echo "vm.sh: $*" >&2; exit 1; }
# The NVVM_* settings survive sudo thanks to SETENV in the sudoers rule.
as_root() {
    [[ $EUID -eq 0 ]] || exec sudo -n --preserve-env=NVVM_MEM,NVVM_ISOLATE,NVVM_INPUT,NVVM_STATE \
        "$HERE/vm.sh" "$@"
}
running() { systemctl is-active -q nvvm.service; }

qmp() {
    printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" \
        | socat -t2 - "UNIX-CONNECT:$RUN/qmp.sock" | tail -n +3
}

ssh_guest() {
    ssh -p "$SSH_PORT" -o UserKnownHostsFile="$HOME/.ssh/known_hosts_nvvm" \
        -o StrictHostKeyChecking=accept-new -o ConnectTimeout=3 \
        "$GUEST_USER@127.0.0.1" "$@"
}

# libvirt's "managed" passthrough by hand: the device goes to vfio-pci
# whatever driver had it (snd_hda_intel may grab the audio function first).
bind_vfio() {
    local dev=$1 drv
    modprobe vfio-pci
    drv=$(basename "$(readlink "/sys/bus/pci/devices/$dev/driver" 2>/dev/null)" 2>/dev/null || true)
    [[ $drv == vfio-pci ]] && return
    [[ $drv == nvidia ]] && die "$dev is driven by nvidia; run sudo ./mode.sh vm and reboot"
    echo vfio-pci >"/sys/bus/pci/devices/$dev/driver_override"
    [[ -n $drv ]] && echo "$dev" >"/sys/bus/pci/devices/$dev/driver/unbind"
    echo "$dev" >/sys/bus/pci/drivers_probe
    [[ $(basename "$(readlink "/sys/bus/pci/devices/$dev/driver")") == vfio-pci ]] \
        || die "could not bind $dev to vfio-pci"
}

isolate_host() {
    local cpus=$1 unit
    for unit in user.slice system.slice init.scope; do
        systemctl set-property --runtime "$unit" "AllowedCPUs=$cpus"
    done
}

cleanup() {
    [[ $ISOLATE == 1 ]] && isolate_host "" || true
    systemctl stop nvvm-virtiofsd.service 2>/dev/null || true
}

start() {
    as_root start "$@"
    local gpu=1
    [[ ${1:-} == --no-gpu ]] && gpu=0
    running && die "already running"
    [[ -f $IMG ]] || die "no guest image $IMG (sudo ./setup.sh)"
    install -d -m 0755 "$RUN"
    [[ -f $STATE/OVMF_VARS.4m.fd ]] || install -m 0644 /usr/share/edk2/x64/OVMF_VARS.4m.fd "$STATE/"

    local args=(
        -name nvvm,debug-threads=on
        -machine q35,accel=kvm,kernel-irqchip=on,memory-backend=mem
        -cpu host,topoext=on,host-phys-bits=on
        -smp 16,sockets=1,cores=8,threads=2
        -m "$MEM"
        # shared memory: virtiofs needs it
        -object "memory-backend-memfd,id=mem,size=$MEM,share=on"
        -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,unit=1,file=$STATE/OVMF_VARS.4m.fd"
        # BAR1 of the 3080 Ti is 16 GB (Resizable BAR): give OVMF a large 64-bit window
        -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=131072
        -drive "file=$IMG,if=none,id=disk,format=raw,cache=none,aio=io_uring,discard=unmap"
        -device virtio-blk-pci,drive=disk
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
        -device virtio-net-pci,netdev=net0
        -chardev "socket,id=fs0,path=$RUN/virtiofs.sock"
        -device vhost-user-fs-pci,chardev=fs0,tag=repo
        -chardev "socket,id=ser0,path=$RUN/serial.sock,server=on,wait=off,logfile=$STATE/serial.log,logappend=on"
        -serial chardev:ser0
        -qmp "unix:$RUN/qmp.sock,server=on,wait=off"
        # kernel debugging: gdb vmlinux -ex 'target remote :1234'
        -gdb tcp:127.0.0.1:1234
        -device virtio-keyboard-pci
        -device virtio-mouse-pci
        -device virtio-rng-pci
        -nodefaults -display none -vga none
    )
    if [[ $gpu == 1 ]]; then
        bind_vfio "$GPU"
        bind_vfio "$GPU_AUDIO"
        args+=(
            -device pcie-root-port,id=rp1,chassis=1,slot=1
            -device "vfio-pci,host=$GPU,bus=rp1,addr=00.0,multifunction=on"
            -device "vfio-pci,host=$GPU_AUDIO,bus=rp1,addr=00.1"
        )
    fi

    rm -f "$RUN/virtiofs.sock"
    systemd-run -q --unit=nvvm-virtiofsd --slice=nvvm.slice --collect \
        /usr/bin/virtiofsd --socket-path="$RUN/virtiofs.sock" --shared-dir="$REPO" \
        --cache=auto --sandbox=namespace
    local i
    for i in $(seq 50); do [[ -S $RUN/virtiofs.sock ]] && break; sleep 0.1; done
    [[ -S $RUN/virtiofs.sock ]] || { cleanup; die "virtiofsd did not start (journalctl -u nvvm-virtiofsd)"; }

    touch "$STATE/serial.log"
    chmod 0644 "$STATE/serial.log"
    echo "=== vm.sh start $(date '+%F %T') gpu=$gpu" >>"$STATE/serial.log"
    [[ $ISOLATE == 1 ]] && isolate_host "$HOST_CPUS"
    systemd-run -q --unit=nvvm --slice=nvvm.slice --collect \
        -p LimitMEMLOCK=infinity -p "AllowedCPUs=$GUEST_CPUS" \
        -p "ExecStopPost=$HERE/vm.sh _cleanup" \
        /usr/bin/qemu-system-x86_64 "${args[@]}"
    sleep 1
    running || { cleanup; journalctl -u nvvm -n 20 --no-pager >&2; die "QEMU exited"; }
    echo "started (gpu=$gpu); ./vm.sh log for the boot, ./vm.sh wait for ssh"
    [[ $gpu == 1 && ${NVVM_INPUT:-0} == 1 ]] && input on
    return 0
}

input() {
    as_root input "$@"
    running || die "not running"
    case ${1:-} in
    on)
        # Each keyboard interface may see both Ctrl, and grab_all makes it
        # switch the others too.
        local i
        for i in "${!KBDS[@]}"; do
            qmp '{"execute":"object-add","arguments":{"qom-type":"input-linux","id":"kbd'"$i"'","evdev":"'"${KBDS[$i]}"'","grab_all":true,"repeat":true}}'
        done
        local mouse=$MOUSE_RAW n
        for n in /sys/class/input/event*/device/name; do
            [[ $(<"$n") == "$MOUSE_NAME" ]] && mouse=/dev/input/$(basename "$(dirname "$(dirname "$n")")")
        done
        qmp '{"execute":"object-add","arguments":{"qom-type":"input-linux","id":"mouse","evdev":"'"$mouse"'"}}'
        echo "keyboard and mouse ($mouse) belong to the guest now; both Ctrl switch back and forth"
        ;;
    off)
        local i
        for i in "${!KBDS[@]}"; do qmp '{"execute":"object-del","arguments":{"id":"kbd'"$i"'"}}'; done
        qmp '{"execute":"object-del","arguments":{"id":"mouse"}}'
        ;;
    *) die "usage: input on|off" ;;
    esac
}

stop() {
    as_root stop
    running || { echo "not running"; return; }
    qmp '{"execute":"system_powerdown"}' >/dev/null
    local i
    for i in $(seq 60); do running || { echo "stopped"; return; }; sleep 1; done
    echo "no shutdown after 60 s, killing QEMU"
    systemctl stop nvvm.service
}

status() {
    if running; then
        echo "running: $(systemctl show -p ActiveEnterTimestamp --value nvvm.service)"
        [[ $EUID -eq 0 ]] && qmp '{"execute":"query-status"}'
    else
        echo "not running"
    fi
    echo "3080 Ti driver: $(basename "$(readlink /sys/bus/pci/devices/$GPU/driver 2>/dev/null)" 2>/dev/null || echo none)"
    if ssh_guest -o BatchMode=yes true 2>/dev/null; then
        echo "guest: $(ssh_guest -o BatchMode=yes "$REPO/local/vm/guest.sh status" 2>&1 | tr '\n' ';')"
    else
        echo "guest: no ssh"
    fi
}

wait_ssh() {
    local i
    for i in $(seq 180); do
        ssh_guest -o BatchMode=yes true 2>/dev/null && { echo "guest is up"; return; }
        sleep 1
    done
    die "no ssh after 180 s (./vm.sh log)"
}

cmd=${1:-}
shift || true
case $cmd in
start) start "$@" ;;
stop) stop ;;
reset) as_root reset; qmp '{"execute":"system_reset"}' ;;
kill) as_root kill; systemctl stop nvvm.service ;;
status) status ;;
input) input "$@" ;;
console)
    as_root console
    echo "serial console, Ctrl-] quits"
    socat -,raw,echo=0,escape=0x1d "UNIX-CONNECT:$RUN/serial.sock"
    ;;
log) tail -n 100 -F "$STATE/serial.log" ;;
qmp) as_root qmp "$@"; qmp "$1" ;;
ssh) ssh_guest -t "$@" ;;
wait) wait_ssh ;;
build) ssh_guest "cd '$REPO' && nice make modules -j\$(nproc) SYSSRC=/lib/modules/\$(uname -r)/build" ;;
reload) ssh_guest "sudo '$REPO/local/vm/guest.sh' reload ${1:-tree}" ;;
install) ssh_guest "sudo '$REPO/local/vm/guest.sh' install ${1:-}" ;;
_cleanup) as_root _cleanup; cleanup ;;
*) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
