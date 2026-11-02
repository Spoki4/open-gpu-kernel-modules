#!/bin/bash
# Installs the KWin script that reports window visibility to the patched
# NVIDIA driver (/proc/driver/nvidia/vidmem_hints), for the current user. No
# root needed. Takes effect at the next login (KWin must start with
# QML_XHR_ALLOW_FILE_WRITE=1 so the script may write the procfs file).
#
#   ./install.sh            install / update and enable
#   ./install.sh remove     disable and remove everything it installed
#   ./install.sh status     what is installed, what the driver received
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ID=nvidia-vram-hints
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
CONF=${XDG_CONFIG_HOME:-$HOME/.config}
ENV_SH=$CONF/plasma-workspace/env/$ID.sh
ENV_D=$CONF/environment.d/90-$ID.conf
HINTS=/proc/driver/nvidia/vidmem_hints

install_it() {
    local d main
    # KWin keeps compiled QML per file URL for its whole lifetime, so a new
    # version under the same name would not load until the next login: the
    # main file is named after its content.
    main=main-$(sha1sum "$HERE/contents/ui/main.qml" | cut -c1-12).qml
    for d in kwin-wayland kwin; do
        mkdir -p "$DATA/$d/scripts"
        rm -rf "${DATA:?}/$d/scripts/$ID"
        cp -r "$HERE" "$DATA/$d/scripts/$ID"
        rm -f "$DATA/$d/scripts/$ID/install.sh"
        mv "$DATA/$d/scripts/$ID/contents/ui/main.qml" "$DATA/$d/scripts/$ID/contents/ui/$main"
        sed -i "s|\"ui/main.qml\"|\"ui/$main\"|" "$DATA/$d/scripts/$ID/metadata.json"
    done
    mkdir -p "$(dirname "$ENV_SH")" "$(dirname "$ENV_D")"
    cat >"$ENV_SH" <<'EOF'
# Lets KWin's QML scripts write files (nvidia-vram-hints writes
# /proc/driver/nvidia/vidmem_hints). Installed by local/kwin-vram-hints/install.sh.
export QML_XHR_ALLOW_FILE_WRITE=1
EOF
    printf '# See %s\nQML_XHR_ALLOW_FILE_WRITE=1\n' "$ENV_SH" >"$ENV_D"
    kwriteconfig6 --file kwinrc --group Plugins --key "${ID}Enabled" true
    # Load the new version into the running KWin now (writing the procfs
    # file needs QML_XHR_ALLOW_FILE_WRITE, i.e. a login after the first install).
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$ID" >/dev/null 2>&1 || true
    qdbus6 org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
    echo "installed; log out and back in, then: $0 status"
}

remove_it() {
    local d
    kwriteconfig6 --file kwinrc --group Plugins --key "${ID}Enabled" --delete || true
    qdbus6 org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
    for d in kwin-wayland kwin; do rm -rf "${DATA:?}/$d/scripts/$ID"; done
    rm -f "$ENV_SH" "$ENV_D"
    # An empty snapshot: the driver forgets the last state.
    [[ -w $HINTS ]] && : >"$HINTS" || true
    echo "removed (the driver keeps using hints until the next module load or an empty write)"
}

status() {
    echo "script: $([[ -f $DATA/kwin-wayland/scripts/$ID/metadata.json ]] && echo installed || echo missing)," \
         "enabled: $(kreadconfig6 --file kwinrc --group Plugins --key "${ID}Enabled" 2>/dev/null || echo ?)," \
         "env: $([[ -f $ENV_SH ]] && echo yes || echo no)"
    local k
    k=$(pgrep -u "$(id -u)" -x kwin_wayland | head -1 || true)
    if [[ -n $k ]]; then
        if [[ ! -r /proc/$k/environ ]] || ! tr '\0' '\n' <"/proc/$k/environ" >/dev/null 2>&1; then
            # KWin is not dumpable (no ptrace), so its environment is unreadable;
            # entries below prove the script can write.
            grep -qE '^[0-9]+ [fvh]' "$HINTS" 2>/dev/null &&
                echo "running kwin_wayland ($k) writes hints" ||
                echo "running kwin_wayland ($k): no hints yet (log out and back in?)"
        elif tr '\0' '\n' <"/proc/$k/environ" | grep -q '^QML_XHR_ALLOW_FILE_WRITE=1$'; then
            echo "running kwin_wayland ($k) may write files"
        else
            echo "running kwin_wayland ($k) started without QML_XHR_ALLOW_FILE_WRITE: log out and back in"
        fi
    fi
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
    *) sed -n '2,10p' "$0"; exit 2 ;;
esac
