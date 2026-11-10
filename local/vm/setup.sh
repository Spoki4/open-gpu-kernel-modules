#!/bin/bash
# One-time setup of the test VM. Run by the user in a terminal (sudo asks for
# the password):
#
#   sudo ./setup.sh
#
#   1. /etc/sudoers.d/nvidia-local-vm: spoki4 may run vm.sh as root without a
#      password (SETENV for its NVVM_* settings). Like the other rules, the
#      script lives in your home and is writable by you: whoever can edit it
#      is root.
#   2. mkimage.sh: the guest image in /var/lib/nvidia-vm (needs dosfstools,
#      installed if missing; downloads a few GB of packages).
# Undo: sudo rm -r /etc/sudoers.d/nvidia-local-vm /var/lib/nvidia-vm
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
USER_NAME=${SUDO_USER:-spoki4}
RULE=/etc/sudoers.d/nvidia-local-vm

echo "== 1. sudoers rule $RULE"
tmp=$(mktemp)
echo "$USER_NAME ALL=(root) NOPASSWD:SETENV: $HERE/vm.sh" >"$tmp"
visudo -cf "$tmp" >/dev/null
install -m 0440 -o root -g root "$tmp" "$RULE"
rm -f "$tmp"
cat "$RULE"

echo "== 2. guest image"
if [[ -f ${NVVM_STATE:-/var/lib/nvidia-vm}/guest.img && ${FORCE:-0} != 1 ]]; then
    echo "image exists, kept (FORCE=1 rebuilds it)"
else
    "$HERE/mkimage.sh"
fi

echo
echo "done. Test the VM without the GPU: $HERE/vm.sh start --no-gpu"
echo "Then for the real thing: sudo $HERE/mode.sh vm, move one monitor to the motherboard, reboot."
