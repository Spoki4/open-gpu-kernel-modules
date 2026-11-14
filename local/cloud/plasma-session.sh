#!/bin/bash
# A Plasma session on a machine without a monitor, for the stages `desktop`
# and `game` of vm/fulltest.sh --here: KWin on a virtual output of the GPU
# (socket wayland-1, Xwayland :0) and plasmashell in it, as user units, so
# they outlive the ssh login (loginctl enable-linger once). The NVIDIA card
# must be the only one with a render node (KWin's virtual backend takes the
# first); hiding the others with bwrap, as tests/nvt-session.sh does, is not
# an option here: AppArmor then denies plasmashell the compositor's socket.
#   plasma-session.sh start|status|stop
#   plasma-session.sh shot FILE.png     what is on the screen (Steam's login QR code, a game's menu)
#   plasma-session.sh run UNIT CMD...   a program in the session (run steam-test steam)
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
ENVS=(-E WAYLAND_DISPLAY=wayland-1 -E DISPLAY=:0 -E XDG_SESSION_TYPE=wayland -E XDG_CURRENT_DESKTOP=KDE)
case ${1:-} in
start)
    systemctl --user is-active -q plasma-test-kwin && { echo "already running"; exit 0; }
    systemctl --user reset-failed plasma-test-kwin plasma-test-shell 2>/dev/null
    # The window hints script, as on the desktop: the driver learns which
    # window is in focus and which process is a game.
    "$(dirname "$0")/../kwin-vram-hints/install.sh" >/dev/null
    # Nobody can unlock a screen here.
    kwriteconfig6 --file kscreenlockerrc --group Daemon --key Autolock false
    kwriteconfig6 --file kscreenlockerrc --group Daemon --key LockOnResume false
    # No permission checks for screenshots: nobody sits at this session to grant them.
    systemd-run -q --user --unit plasma-test-kwin -E XDG_SESSION_TYPE=wayland -E XDG_CURRENT_DESKTOP=KDE \
        -E KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 -E QML_XHR_ALLOW_FILE_WRITE=1 \
        kwin_wayland --virtual --socket wayland-1 --xwayland --width 1920 --height 1080
    for _ in $(seq 100); do [[ -S $XDG_RUNTIME_DIR/wayland-1 ]] && break; sleep 0.1; done
    sleep 2
    systemd-run -q --user --unit plasma-test-shell "${ENVS[@]}" -E QT_QPA_PLATFORM=wayland plasmashell
    sleep 8 ;&
status)
    systemctl --user is-active plasma-test-kwin plasma-test-shell
    pgrep -a -x kwin_wayland | cut -c1-80; pgrep -a -x plasmashell ;;
shot)
    WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=wayland timeout 20 spectacle -b -n -f -o "$2" >/dev/null 2>&1; ls -l "$2" ;;
run)
    unit=$2; shift 2
    systemctl --user reset-failed "$unit" 2>/dev/null
    systemd-run -q --user --unit "$unit" "${ENVS[@]}" "$@" ;;
stop)
    systemctl --user stop plasma-test-shell plasma-test-kwin ;;
*)  sed -n '2,11p' "$0"; exit 2 ;;
esac
