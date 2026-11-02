#!/bin/bash
# Headless test desktop for nvtest.sh: a virtual KWin on the NVIDIA GPU and
# Chrome with a throw-away profile inside it, no login needed (run at the
# SDDM greeter, as the desktop user in the groups render and video).
#
#   ./nvt-session.sh start [URL...]   compositor (if not running) + Chrome
#   ./nvt-session.sh title            titles of Chrome's pages (DevTools port)
#   ./nvt-session.sh gpu-pid          pid of the test Chrome's GPU process
#   ./nvt-session.sh win ACTION PID   minimize|unminimize|activate|fullscreen|normal
#                                     every window of PID (a KWin script over DBus)
#   ./nvt-session.sh stop             Chrome and the compositor
# NVT_INSTANCE=b runs a second Chrome (own profile and DevTools port);
# start/title/gpu-pid/stop-chrome act on that instance.
#
# KWin's virtual backend takes the first render node, the AMD iGPU here, so
# the AMD nodes are hidden from it with bwrap.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SOCK=nvtest-0
INST=${NVT_INSTANCE:-a}
PROFILE=/tmp/nvt-chrome/profile-$INST
case $INST in a) PORT=9333 ;; b) PORT=9334 ;; *) PORT=9335 ;; esac
LOG=/tmp/nvt-chrome
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
unset DISPLAY

# AMD nodes (everything that is not the NVIDIA GPU at 01:00.0).
hide_args() {
    local n
    for n in /dev/dri/by-path/*; do
        [[ $n == *0000:01:00.0* ]] && continue
        printf -- '--bind /dev/null %s ' "$(readlink -f "$n")"
    done
}

kwin_running() { pgrep -f -- "kwin_wayland --virtual --socket $SOCK" >/dev/null; }
chrome_pids() { pgrep -f -- "--user-data-dir=$PROFILE" || true; }

start() {
    mkdir -p "$LOG"
    if ! kwin_running; then
        # Own config dir (the user's kwinrc stays untouched) with the window
        # hints script enabled; QML may write the procfs file.
        local cfg=$LOG/kwin-config data=$LOG/kwin-data
        mkdir -p "$cfg" "$data/kwin-wayland/scripts" "$data/kwin/scripts"
        printf '[Plugins]\nnvidia-vram-hintsEnabled=true\n' >"$cfg/kwinrc"
        # The script from this checkout, not whatever the user has installed.
        rm -rf "$data/kwin-wayland/scripts/nvidia-vram-hints" "$data/kwin/scripts/nvidia-vram-hints"
        cp -r "$HERE/kwin-vram-hints" "$data/kwin-wayland/scripts/nvidia-vram-hints"
        cp -r "$HERE/kwin-vram-hints" "$data/kwin/scripts/nvidia-vram-hints"
        # shellcheck disable=SC2046
        XDG_CONFIG_HOME=$cfg XDG_DATA_HOME=$data QML_XHR_ALLOW_FILE_WRITE=1 bwrap --dev-bind / / $(hide_args) \
            kwin_wayland --virtual --socket "$SOCK" --width 1920 --height 1080 >"$LOG/kwin.log" 2>&1 &
        local i
        for i in $(seq 1 50); do [[ -S $XDG_RUNTIME_DIR/$SOCK ]] && kwin_running && break; sleep 0.1; done
        sleep 0.5
        kwin_running && [[ -S $XDG_RUNTIME_DIR/$SOCK ]] ||
            { echo "nvt-session: compositor did not start: $(tail -2 "$LOG/kwin.log")" >&2; exit 1; }
    fi
    [[ -z $(chrome_pids) ]] || { echo "nvt-session: test Chrome already running" >&2; exit 1; }
    WAYLAND_DISPLAY=$SOCK google-chrome --ozone-platform=wayland --user-data-dir="$PROFILE" \
        --password-store=basic --no-first-run --no-default-browser-check --disable-background-networking \
        --remote-debugging-port=$PORT --new-window "$@" >"$LOG/chrome-$INST.log" 2>&1 &
    local i
    for i in $(seq 1 100); do curl -s "http://127.0.0.1:$PORT/json/version" >/dev/null && break; sleep 0.1; done
    echo "started: kwin $(pgrep -f -- "kwin_wayland --virtual --socket $SOCK" | head -1), chrome $(chrome_pids | head -1)"
}

title() {
    curl -s "http://127.0.0.1:$PORT/json" | python3 -c '
import json, sys
for t in json.load(sys.stdin):
    if t.get("type") == "page":
        print(t["title"])'
}

# The GPU process is forked from the no-sandbox zygote, a child of the
# browser process: find the one whose ancestors include the test browser.
gpu_pid() {
    local main p q i
    main=$(pgrep -f -- "--user-data-dir=$PROFILE" | head -1 || true)
    [[ -n $main ]] || return 0
    for p in $(pgrep -f -- "--type=gpu-process" || true); do
        q=$p
        for i in 1 2 3 4; do
            q=$(ps -o ppid= -p "$q" 2>/dev/null | tr -d ' ')
            [[ -n $q && $q != 1 ]] || break
            [[ $q == "$main" ]] && { echo "$p"; return 0; }
        done
    done
}

# Run a one-off KWin script acting on every window of a process.
win() {
    local action=$1 pid=$2 js name
    case $action in
        minimize) js='w.minimized = true;' ;;
        unminimize) js='w.minimized = false;' ;;
        activate) js='w.minimized = false; workspace.activeWindow = w;' ;;
        fullscreen) js='w.fullScreen = true; workspace.activeWindow = w;' ;;
        normal) js='w.fullScreen = false;' ;;
        *) echo "nvt-session: unknown action $action" >&2; exit 2 ;;
    esac
    name=nvt-win-$$
    printf 'var o = workspace.stackingOrder;\nfor (var i = 0; i < o.length; i++) { var w = o[i]; if (w.pid == %s) { %s } }\n' \
        "$pid" "$js" >"$LOG/$name.js"
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$LOG/$name.js" "$name" >/dev/null
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null
    # Returns as soon as KWin ran it (callers time what follows); clean up later.
    ( sleep 2; qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null
      rm -f "$LOG/$name.js" ) >/dev/null 2>&1 &
}

# This instance's Chrome only.
stop_chrome() {
    local p
    p=$(chrome_pids)
    [[ -z $p ]] || kill $p 2>/dev/null || true
    sleep 1
    p=$(chrome_pids)
    [[ -z $p ]] || kill -9 $p 2>/dev/null || true
}

# The browser process (owner of the windows).
main_pid() { pgrep -f -- "--user-data-dir=$PROFILE" | head -1 || true; }

stop() {
    local p
    p=$(pgrep -f -- "--user-data-dir=/tmp/nvt-chrome/profile" || true)
    [[ -z $p ]] || kill $p 2>/dev/null || true
    sleep 1
    p=$(pgrep -f -- "--user-data-dir=/tmp/nvt-chrome/profile" || true)
    [[ -z $p ]] || kill -9 $p 2>/dev/null || true
    pkill -f -- "kwin_wayland --virtual --socket $SOCK" 2>/dev/null || true
    local i
    for i in $(seq 1 50); do kwin_running || break; sleep 0.1; done
    kwin_running && pkill -9 -f -- "kwin_wayland --virtual --socket $SOCK" 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock"
}

case ${1:-} in
    start) shift; start "$@" ;;
    title) title ;;
    gpu-pid) gpu_pid ;;
    main-pid) main_pid ;;
    win) win "${2:?action}" "${3:?pid}" ;;
    stop-chrome) stop_chrome ;;
    stop) stop ;;
    *) sed -n '2,12p' "$0"; exit 2 ;;
esac
