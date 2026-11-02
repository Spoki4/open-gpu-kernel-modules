#!/bin/bash
# One-time setup of the unattended test bench. Run once by the user, in a real
# terminal (sudo asks for the password):
#
#   ssh -t spoki4-pc "sudo ~/projects/open-gpu-kernel-modules/local/nvtest-setup.sh"
#
# It does four things and prints what it did:
#   1. safety.sh install: hardware watchdog sp5100_tco fed by systemd
#      (RuntimeWatchdogSec=30s), panic on lockups/oops, reboot 10 s after a
#      panic; efi-pstore keeps the panic log (systemd-pstore is enabled).
#   2. /etc/sudoers.d/nvidia-local-nvtest: spoki4 may run nvtest-root.sh
#      (stage / reload / promote / status) without a password, like the
#      existing rules for install.sh, evict.sh and friends. Note: these scripts
#      live in your home and are writable by you, so the rule is as strong as
#      the existing ones (whoever can edit them is root).
#   3. /var/lib/nvidia-local for the candidate slot.
#   4. adds you to the groups render and video: with nobody logged in on
#      seat0 the DRM nodes lose their per-seat ACL, and the headless test
#      compositor (kwin_wayland --virtual) and Chrome need /dev/dri/renderD*.
# Undo: sudo ./safety.sh remove; sudo rm /etc/sudoers.d/nvidia-local-nvtest
#       /var/lib/nvidia-local -r; sudo gpasswd -d spoki4 render (and video)
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
USER_NAME=${SUDO_USER:-spoki4}
RULE=/etc/sudoers.d/nvidia-local-nvtest

echo "== 1. safety net"
"$HERE/safety.sh" install

echo "== 2. sudoers rule $RULE"
tmp=$(mktemp)
echo "$USER_NAME ALL=(root) NOPASSWD: $HERE/nvtest-root.sh" >"$tmp"
visudo -cf "$tmp" >/dev/null
install -m 0440 -o root -g root "$tmp" "$RULE"
rm -f "$tmp"
cat "$RULE"

echo "== 3. candidate slot"
install -d -m 0755 /var/lib/nvidia-local
ls -ld /var/lib/nvidia-local

echo "== 4. groups render, video for $USER_NAME"
usermod -aG render,video "$USER_NAME"
id "$USER_NAME"

echo
echo "done. Check: $HERE/nvtest-root.sh status"
