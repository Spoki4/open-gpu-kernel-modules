#!/bin/bash
# Installs the locally patched NVIDIA modules built in ../kernel-open.
#
#   sudo ./install.sh enroll-key   # step 1: create MOK key, queue it for enrollment, then reboot
#   sudo ./install.sh install      # step 2: after the key is enrolled, sign and install modules
#
# Modules go to /lib/modules/$KVER/updates/nvidia-local, which depmod prefers
# over Canonical's kernel/nvidia-595-open. Undo with ./rollback.sh.
set -euo pipefail

KVER=${KVER:-$(uname -r)}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD=${BUILD:-$HERE/../kernel-open}   # nvtest-root.sh promote passes its candidate copies
KEYDIR=/var/lib/nvidia-local-mok
DEST=/lib/modules/$KVER/updates/nvidia-local
SIGN=/usr/src/linux-headers-$KVER/scripts/sign-file
RESERVE_MB=${RESERVE_MB:-768}
MODPROBE_CONF=/etc/modprobe.d/nvidia-vidmem-reserve.conf
MODULES=(nvidia nvidia-modeset nvidia-drm nvidia-uvm nvidia-peermem)

die() { echo "install.sh: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run with sudo"

# The wording of `mokutil --test-key` differs between versions, so compare the
# key's SHA1 fingerprint with the enrolled list instead. The list is captured
# first: piping it into `grep -q` breaks under pipefail (SIGPIPE upstream).
key_enrolled() {
    local fp enrolled
    fp=$(openssl x509 -inform DER -in "$KEYDIR/MOK.der" -noout -fingerprint -sha1 \
         | cut -d= -f2 | tr -d ':' | tr 'A-F' 'a-f')
    enrolled=$(mokutil --list-enrolled 2>/dev/null | tr -d ': ' | tr 'A-F' 'a-f')
    [[ -n $fp && $enrolled == *"$fp"* ]]
}

enroll_key() {
    if [[ ! -f $KEYDIR/MOK.der ]]; then
        install -d -m 0700 "$KEYDIR"
        openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 -sha256 \
            -subj "/CN=$(hostname) local NVIDIA module signing/" \
            -keyout "$KEYDIR/MOK.priv" -outform DER -out "$KEYDIR/MOK.der"
        chmod 0600 "$KEYDIR/MOK.priv"
        echo "created key in $KEYDIR"
    fi
    if key_enrolled; then
        echo "key is already enrolled, run: sudo $0 install"
        return
    fi
    echo "Choose a one-time password; you will type it again in MOK Manager after reboot."
    mokutil --import "$KEYDIR/MOK.der"
    cat <<EOF

Reboot now. On the blue "MOK management" screen choose:
  Enroll MOK -> Continue -> Yes -> enter the password -> Reboot
Then run: sudo $0 install
EOF
}

install_modules() {
    [[ -f $KEYDIR/MOK.der ]] || die "no key, run: sudo $0 enroll-key"
    if [[ $(mokutil --sb-state 2>/dev/null) == *"SecureBoot enabled"* ]]; then
        key_enrolled || die "key is not enrolled yet; the kernel would refuse the modules. Run enroll-key and reboot first."
    fi
    [[ -x $SIGN ]] || die "missing $SIGN (install linux-headers-$KVER)"

    local version
    version=$(modinfo -F version "$BUILD/nvidia.ko")
    local want
    want=$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}\n' 'libnvidia-gl-*:amd64' 2>/dev/null | awk '$1 == "ii" {print $2}' | cut -d- -f1 | sort -V | tail -1)
    [[ $version == "$want" ]] || die "built $version but userspace is $want; versions must match"
    [[ $(modinfo -F vermagic "$BUILD/nvidia.ko" | cut -d' ' -f1) == "$KVER" ]] || die "modules were built for another kernel"

    install -d "$DEST"
    for m in "${MODULES[@]}"; do
        install -m 0644 "$BUILD/$m.ko" "$DEST/$m.ko"
        strip --strip-debug "$DEST/$m.ko"
        "$SIGN" sha256 "$KEYDIR/MOK.priv" "$KEYDIR/MOK.der" "$DEST/$m.ko"
    done
    depmod -a "$KVER"

    echo "options nvidia NVreg_RegistryDwords=\"RmVidmemUserReserveMB=$RESERVE_MB\"" > "$MODPROBE_CONF"
    update-initramfs -u -k "$KVER"

    echo
    echo "modprobe will now load:"
    modinfo -k "$KVER" -F filename nvidia
    echo "reserve: $RESERVE_MB MB ($MODPROBE_CONF)"
    echo "Reboot to switch to the patched modules."
}

case ${1:-} in
    enroll-key) enroll_key ;;
    install)    install_modules ;;
    *)          echo "usage: sudo $0 enroll-key | install"; exit 2 ;;
esac
