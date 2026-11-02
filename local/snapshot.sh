#!/bin/bash
# Restore points for this PC's root btrfs before risky driver work.
#
#   sudo ./snapshot.sh create [label]   # read-only snapshot /.snapshots/<date>-<label>
#   ./snapshot.sh list                  # existing restore points
#   sudo ./snapshot.sh delete <name>    # remove one
#
# The PC's root filesystem is the top-level btrfs volume (subvolid=5) with
# /home and /boot inside it, so a snapshot holds the whole system including
# home. A snapshot costs no space at first; it grows as files it still
# references are changed or deleted (e.g. deleting a game keeps its space used
# until the snapshot is deleted).
#
# Restoring:
#   - single files (preferred): copy them back from the snapshot, e.g.
#       sudo cp -a /.snapshots/<name>/etc/modprobe.d/nvidia-vidmem-reserve.conf /etc/modprobe.d/
#     or the patched modules:
#       sudo rsync -a --delete /.snapshots/<name>/lib/modules/$(uname -r)/updates/ /lib/modules/$(uname -r)/updates/
#       sudo depmod -a && sudo update-initramfs -u
#   - everything (last resort, also rolls back /home!): make a writable copy and
#     boot it as the default subvolume:
#       sudo btrfs subvolume snapshot /.snapshots/<name> /.snapshots/<name>-rw
#       sudo btrfs subvolume set-default /.snapshots/<name>-rw && reboot
#     (undo with: sudo btrfs subvolume set-default 5 /)
set -euo pipefail

DIR=/.snapshots

die() { echo "snapshot.sh: $*" >&2; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run with sudo"; }

case ${1:-} in
    create)
        need_root
        label=${2:-manual}
        [[ $label =~ ^[A-Za-z0-9._-]+$ ]] || die "label: letters, digits, . _ - only"
        [[ $(stat -f -c %T /) == btrfs ]] || die "/ is not btrfs"
        if [[ ! -d $DIR ]]; then
            # A subvolume of its own, so snapshots of / do not contain older snapshots.
            btrfs subvolume create "$DIR" >/dev/null
        fi
        name=$(date +%F_%H%M)-$label
        btrfs subvolume snapshot -r / "$DIR/$name"
        sync
        echo "restore point: $DIR/$name"
        ;;
    list)
        if [[ -d $DIR ]]; then
            ls -1 "$DIR"
        else
            echo "no restore points ($DIR does not exist)"
        fi
        ;;
    delete)
        need_root
        [[ -n ${2:-} && $2 != */* ]] || die "usage: sudo $0 delete <name>"
        [[ -d $DIR/$2 ]] || die "no restore point $DIR/$2"
        btrfs subvolume delete "$DIR/$2"
        ;;
    *)
        echo "usage: sudo $0 create [label] | $0 list | sudo $0 delete <name>" >&2
        exit 2
        ;;
esac
