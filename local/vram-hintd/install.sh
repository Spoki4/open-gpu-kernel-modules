#!/bin/bash
# Builds and installs vram-hintd, the window hint writer for Hyprland, sway,
# niri and X11 window managers, for the current user. No root needed; a C
# compiler and make are (X11 also needs the libxcb, xcb-res and xcb-randr
# headers: libxcb on Arch, libxcb-res0-dev and libxcb-randr0-dev on Debian).
# It has to start with the session, in the compositor's environment:
#   - X11 desktops (Xfce, Cinnamon, MATE, LXQt, ...) and sessions under uwsm
#     run ~/.config/autostart/vram-hintd.desktop, which this installs;
#   - Hyprland, sway and niri started without uwsm need a line in their
#     configuration (printed below).
# One instance runs per user; a second start exits at once. On KDE Plasma and
# GNOME it exits too (kwin-vram-hints/, gnome-vram-hints/ report there).
#
#   ./install.sh            install / update
#   ./install.sh remove     stop it and remove what it installed
#   ./install.sh status     what is installed and running, what the driver received
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BIN=$HOME/.local/bin/vram-hintd
AUTO=${XDG_CONFIG_HOME:-$HOME/.config}/autostart/vram-hintd.desktop
HINTS=/proc/driver/nvidia/vidmem_hints

install_it() {
    local build
    build=$(mktemp -d)
    trap 'rm -rf "$build"' RETURN
    make -s -C "$HERE" O="$build" || { echo "build failed"; exit 1; }
    pkg-config --exists xcb xcb-res xcb-randr 2>/dev/null ||
        echo "note: built without X11 (no libxcb, xcb-res, xcb-randr headers); Wayland compositors work"
    mkdir -p "$(dirname "$BIN")" "$(dirname "$AUTO")"
    install -m 755 "$build/vram-hintd" "$BIN"
    cat >"$AUTO" <<EOF
[Desktop Entry]
Type=Application
Name=NVIDIA VRAM window hints
Comment=Tells the patched NVIDIA driver which windows are visible (installed by local/vram-hintd/install.sh)
Exec=$BIN
NotShowIn=KDE;GNOME;
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
    # A running one restarts with the new version.
    if pkill -u "$(id -u)" -x vram-hintd; then
        sleep 0.5
        echo "stopped the running vram-hintd: start it again (or log in again)"
    fi
    cat <<EOF
installed $BIN and $AUTO
Started at login on X11 desktops and under uwsm. Otherwise add to the compositor's configuration:
  Hyprland 0.57+ (~/.config/hypr/hyprland.lua):  hl.on("hyprland.start", function() hl.exec_cmd("$BIN") end)
  Hyprland before 0.57 (~/.config/hypr/hyprland.conf):  exec-once = $BIN
  sway      (~/.config/sway/config):          exec $BIN
  niri      (~/.config/niri/config.kdl):      spawn-at-startup "$BIN"
then log in again, or start it now from a terminal of the session: $BIN &
EOF
}

remove_it() {
    # SIGTERM: it writes an empty table, the driver stops using hints.
    pkill -u "$(id -u)" -x vram-hintd && echo "stopped vram-hintd" || true
    rm -f "$BIN" "$AUTO"
    echo "removed; remove the line from the compositor's configuration if you added one"
}

status() {
    echo "installed: $([[ -x $BIN ]] && echo yes || echo no)," \
         "autostart: $([[ -f $AUTO ]] && echo yes || echo no)," \
         "running: $(pgrep -u "$(id -u)" -x vram-hintd | tr '\n' ' ' || true)"
    if [[ -r $HINTS ]]; then
        echo "driver received:"
        sed 's/^/  /' "$HINTS"
    else
        echo "no $HINTS: the patched driver is not loaded"
    fi
}

case ${1:-install} in
    install) install_it ;;
    remove) remove_it ;;
    status) status ;;
    *) sed -n '2,15p' "$0"; exit 2 ;;
esac
