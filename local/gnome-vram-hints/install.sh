#!/bin/bash
# Installs the GNOME Shell extension that reports window visibility to the
# patched NVIDIA driver (/proc/driver/nvidia/vidmem_hints), for the current
# user. No root needed. GNOME 45 or later. The shell finds a new extension
# only when it starts: the first install takes effect at the next login (an
# update too, since the shell keeps the loaded code).
#
#   ./install.sh            install / update and enable
#   ./install.sh remove     disable and remove it
#   ./install.sh status     what is installed, what the driver received
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
UUID=nvidia-vram-hints@open-gpu-kernel-modules
DIR=${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID
HINTS=/proc/driver/nvidia/vidmem_hints

install_it() {
    mkdir -p "$DIR"
    cp "$HERE/extension.js" "$HERE/metadata.json" "$DIR/"
    # Enabled through the settings, which also works before the shell has
    # seen the extension (gnome-extensions enable refuses an unknown one).
    local cur
    cur=$(gsettings get org.gnome.shell enabled-extensions)
    if [[ $cur != *"'$UUID'"* ]]; then
        if [[ $cur == "@as []" || $cur == "[]" ]]; then
            gsettings set org.gnome.shell enabled-extensions "['$UUID']"
        else
            gsettings set org.gnome.shell enabled-extensions "${cur%]}, '$UUID']"
        fi
    fi
    echo "installed $DIR; log out and back in, then: $0 status"
}

remove_it() {
    # Disabling writes an empty table: the driver stops using hints.
    gnome-extensions disable "$UUID" 2>/dev/null || true
    local cur
    cur=$(gsettings get org.gnome.shell enabled-extensions)
    cur=${cur//", '$UUID'"/}
    cur=${cur//"'$UUID', "/}
    cur=${cur//"'$UUID'"/}
    gsettings set org.gnome.shell enabled-extensions "$cur"
    rm -rf "$DIR"
    echo "removed"
}

status() {
    echo "files: $([[ -f $DIR/extension.js ]] && echo installed || echo missing)"
    gnome-extensions info "$UUID" 2>/dev/null | grep -E 'State|Enabled' | sed 's/^ */  /' ||
        echo "  the running shell does not know it yet (log out and back in)"
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
    *) sed -n '2,11p' "$0"; exit 2 ;;
esac
