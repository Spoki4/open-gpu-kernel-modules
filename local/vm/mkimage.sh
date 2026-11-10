#!/bin/bash
# Builds the test VM's disk image: CachyOS from the host's repositories
# (pacstrap), KDE Plasma on Wayland with autologin, Steam, Chromium, the stock
# nvidia-open modules as the boot fallback. Called by setup.sh; run again
# with FORCE=1 to start over (the old image is deleted).
#
#   sudo ./mkimage.sh
#
# Guest specifics: user spoki4 (uid 1000 like on the host, so files in the
# shared repo keep their owner) without a password, sudo without a password,
# ssh with the host's public keys, a root shell on the serial console, no
# Secure Boot, no screen locker, no suspend. It is a throw-away test machine.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$HERE/../.." && pwd)
STATE=${NVVM_STATE:-/var/lib/nvidia-vm}
IMG=$STATE/guest.img
SIZE=${SIZE:-160G}
USER_NAME=${SUDO_USER:-spoki4}
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
MNT=$STATE/mnt

PACKAGES=(
    base linux-cachyos linux-cachyos-headers linux-cachyos-nvidia-open
    nvidia-utils lib32-nvidia-utils
    cachyos-keyring cachyos-mirrorlist cachyos-v4-mirrorlist
    sudo openssh base-devel git socat psmisc nano htop nvtop vulkan-tools mesa-utils
    dosfstools e2fsprogs
    plasma-desktop sddm kscreen plasma-pa breeze-gtk xdg-desktop-portal-kde konsole dolphin
    pipewire pipewire-pulse pipewire-alsa wireplumber
    steam chromium ttf-liberation ttf-dejavu noto-fonts
)

die() { echo "mkimage.sh: $*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || die "run with sudo"
[[ ! -e $IMG || ${FORCE:-0} == 1 ]] || die "$IMG exists (FORCE=1 to rebuild it)"
command -v mkfs.fat >/dev/null || pacman -S --needed --noconfirm dosfstools

LOOP=
cleanup() {
    umount -R "$MNT" 2>/dev/null || true
    [[ -n $LOOP ]] && losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT

install -d -m 0755 "$STATE"
chattr +C "$STATE" 2>/dev/null || true   # btrfs: no copy-on-write under a VM disk
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
sfdisk -q "$IMG" <<EOF
label: gpt
size=1G, type=uefi, name=esp
type=linux, name=root
EOF
LOOP=$(losetup -P --show -f "$IMG")
mkfs.fat -F 32 -n NVVM_ESP "${LOOP}p1" >/dev/null
mkfs.ext4 -q -L nvvm-root "${LOOP}p2"
ROOT_PARTUUID=$(blkid -s PARTUUID -o value "${LOOP}p2")

install -d "$MNT"
mount "${LOOP}p2" "$MNT"
mount --mkdir "${LOOP}p1" "$MNT/boot"

# -P: the guest gets the host's pacman.conf (CachyOS repos, multilib).
pacstrap -c -P "$MNT" "${PACKAGES[@]}"

genfstab -U "$MNT" >"$MNT/etc/fstab"
cat >>"$MNT/etc/fstab" <<EOF
# the host's checkout, shared by vm.sh (virtiofs)
repo $REPO virtiofs defaults,nofail 0 0
EOF

echo nvvm >"$MNT/etc/hostname"
cp /etc/locale.conf /etc/vconsole.conf "$MNT/etc/"
ln -sf "$(readlink /etc/localtime)" "$MNT/etc/localtime"
sed -i -e 's/^#\(en_US.UTF-8\)/\1/' -e 's/^#\(ru_RU.UTF-8\)/\1/' "$MNT/etc/locale.gen"

# The guest kernel boots on virtio-blk, which autodetect in a chroot on the
# host would leave out of the initramfs.
sed -i 's/^HOOKS=(\(.*\)autodetect \(.*\))/HOOKS=(\1\2)/' "$MNT/etc/mkinitcpio.conf"

# The guest's packages are pinned: kernel, nvidia-utils and the tree build
# must stay the same release (update all of them together on purpose).
sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg = linux-cachyos linux-cachyos-headers linux-cachyos-nvidia-open nvidia-utils lib32-nvidia-utils/' \
    "$MNT/etc/pacman.conf"

cat >"$MNT/etc/systemd/network/20-wired.network" <<EOF
[Match]
Name=en*

[Network]
DHCP=yes
EOF

install -d "$MNT/etc/sddm.conf.d"
cat >"$MNT/etc/sddm.conf.d/10-nvvm.conf" <<EOF
[General]
DisplayServer=wayland
GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell

[Wayland]
CompositorCommand=kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1

[Autologin]
User=$USER_NAME
Session=plasma
Relogin=true
EOF

install -d "$MNT/etc/systemd/system/serial-getty@ttyS0.service.d"
cat >"$MNT/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty -o '-p -f -- \\\\u' --noclear --autologin root --keep-baud 115200,57600,38400,9600 - \$TERM
EOF

echo "$USER_NAME ALL=(ALL) NOPASSWD: ALL" >"$MNT/etc/sudoers.d/10-nvvm"
chmod 0440 "$MNT/etc/sudoers.d/10-nvvm"

arch-chroot "$MNT" /bin/bash -euo pipefail <<EOF
locale-gen >/dev/null
useradd -m -u $(id -u "$USER_NAME") -U -G wheel,video,render,input,audio "$USER_NAME"
passwd -d "$USER_NAME" >/dev/null
install -d -o "$USER_NAME" -g "$USER_NAME" "$REPO"
systemctl enable sddm sshd systemd-networkd systemd-resolved fstrim.timer
systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
# pacstrap ran the host's /etc/pacman.d/hooks, where limine's initcpio hook
# shadows the guest's: create the presets and images with the guest's script.
ls /usr/lib/modules/*/vmlinuz | sed 's|^/||' | /usr/share/libalpm/scripts/mkinitcpio install
ls /boot/vmlinuz-linux-cachyos /boot/initramfs-linux-cachyos.img >/dev/null
EOF
# Parents of the mount point belong to the user as on the host.
d=$REPO
while [[ $d != "$USER_HOME" && $d != / ]]; do
    chown "$(id -u "$USER_NAME"):$(id -g "$USER_NAME")" "$MNT$d"
    d=$(dirname "$d")
done
ln -sf /run/systemd/resolve/stub-resolv.conf "$MNT/etc/resolv.conf"

GH=$MNT$USER_HOME
install -d -m 0700 "$GH/.ssh" "$GH/.config"
cat "$USER_HOME"/.ssh/*.pub >"$GH/.ssh/authorized_keys"
chmod 0600 "$GH/.ssh/authorized_keys"
[[ -f $USER_HOME/.config/kxkbrc ]] && cp "$USER_HOME/.config/kxkbrc" "$GH/.config/"
cat >"$GH/.config/kscreenlockerrc" <<EOF
[Daemon]
Autolock=false
LockOnResume=false
EOF
cat >"$GH/.config/powerdevilrc" <<EOF
[AC][Display]
DimDisplayWhenIdle=false
TurnOffDisplayWhenIdle=false

[AC][SuspendAndShutdown]
AutoSuspendAction=0
EOF
chown -R "$(id -u "$USER_NAME"):$(id -g "$USER_NAME")" "$GH/.ssh" "$GH/.config"

# systemd-boot from the ESP's fallback path: no EFI variables to set.
install -D "$MNT/usr/lib/systemd/boot/efi/systemd-bootx64.efi" "$MNT/boot/EFI/BOOT/BOOTX64.EFI"
install -d "$MNT/boot/loader/entries"
cat >"$MNT/boot/loader/loader.conf" <<EOF
default nvvm.conf
timeout 2
console-mode keep
EOF
cat >"$MNT/boot/loader/entries/nvvm.conf" <<EOF
title   CachyOS test VM
linux   /vmlinuz-linux-cachyos
initrd  /initramfs-linux-cachyos.img
options root=PARTUUID=$ROOT_PARTUUID rw console=tty0 console=ttyS0,115200 nvidia_drm.modeset=1 nvidia_drm.fbdev=1
EOF

echo
echo "image: $IMG ($(du -h --apparent-size "$IMG" | cut -f1) apparent, $(du -h "$IMG" | cut -f1) used)"
echo "guest: kernel $(arch-chroot "$MNT" pacman -Q linux-cachyos | cut -d' ' -f2), $(arch-chroot "$MNT" pacman -Q nvidia-utils)"
