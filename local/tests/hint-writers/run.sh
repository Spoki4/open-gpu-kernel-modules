#!/bin/bash
# One desktop's scenarios for hint-writers-test.sh, in its container as the
# test user (under dbus-run-session): start the compositor headless, the hint
# writer with VRAM_HINTS_FILE, open and move windows, and compare the table
# after each step with what it should say. Windows are named A, B, ...; the
# table is shown with those names (A=f: A's process has the focus, +g:
# fullscreen). Prints "ok"/"FAIL" per step and RUN <desktop> PASS|FAIL.
# GPU=1 (hint-writers-test.sh --gpu): the writers write the driver's file,
# Hyprland is the real one, and each desktop ends with the memory scenario
# (mem_scenario).
#   run.sh sway|niri|openbox|xfwm4|i3|gnome|hyprland|hyprland-old
desk=$1
SRC=/src
# vram-hintd built here, with the sanitizers: a memory error fails the run.
HINTD=/tmp/hintd-build/vram-hintd
mkdir -p /tmp/hintd-build
make -s -C "$SRC/vram-hintd" O=/tmp/hintd-build CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
    >/tmp/hintd-build/make.log 2>&1 || { cat /tmp/hintd-build/make.log; echo "RUN $1 FAIL (build)"; exit 1; }
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
if [[ -n ${GPU:-} ]]; then
    H=/proc/driver/nvidia/vidmem_hints
    mkdir -p /tmp/nvt
    gcc -O2 -I"$SRC/tests" -o /tmp/nvt/nvt-game "$SRC/tests/nvt-game.c" -lvulkan -ldl -lm \
        >/tmp/nvt/make.log 2>&1 || { cat /tmp/nvt/make.log; echo "RUN $1 FAIL (nvt-game build)"; exit 1; }
    ln -sf nvt-game /tmp/nvt/foot
    ln -sf nvt-game /tmp/nvt/xterm
else
    export VRAM_HINTS_FILE=/tmp/hints-$desk
    H=$VRAM_HINTS_FILE
    rm -f "$H"
fi
fails=0
declare -A PID
# One desktop at a time: sockets a compositor before this one left behind
# would take its clients.
rm -f "$XDG_RUNTIME_DIR"/wayland-*
cleanup() {
    kill $(jobs -p) 2>/dev/null
    pkill -u t -x 'sleep|nvt-game' 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# The table with window names instead of pids, sorted (the driver's
# file starts with a "# generation" line).
names() {
    local p s g n k
    while read -r p s g; do
        [[ -n $p && $p != '#' ]] || continue
        n=pid$p
        for k in "${!PID[@]}"; do [[ ${PID[$k]} == "$p" ]] && n=$k; done
        echo "$n=$s${g:++g}"
    done <"$H" 2>/dev/null | sort | tr '\n' ' ' | sed 's/ $//'
}
expect() {
    local what=$1 want got i
    shift
    want=$(printf '%s\n' "$@" | sort | tr '\n' ' ' | sed 's/ $//')
    for i in $(seq 40); do
        got=$(names)
        [[ $got == "$want" ]] && { echo "  ok   $what: $got"; return; }
        sleep 0.2
    done
    echo "  FAIL $what: want [$want], got [$got]"
    fails=$((fails + 1))
}
# Some entry NAME=state matches a pattern: expect_has DESC 'M=[fv]'.
expect_has() {
    local what=$1 pat=$2 got i
    for i in $(seq 40); do
        got=$(names)
        [[ " $got " =~ \ $pat\  ]] && { echo "  ok   $what: $got"; return; }
        sleep 0.2
    done
    echo "  FAIL $what: want $pat, got [$got]"
    fails=$((fails + 1))
}
# Off: the writer wrote an empty table (the driver: 0 entries).
off() {
    if [[ -n ${GPU:-} ]]; then
        head -1 "$H" | grep -q ' 0 entries'
    else
        [[ -f $H && $(od -An -c "$H" | tr -d ' ') == '\n' ]]
    fi
}
expect_off() {
    local i
    for i in $(seq 40); do
        off && { echo "  ok   $1: hints off"; return; }
        sleep 0.2
    done
    echo "  FAIL $1: hints not off: $(head -3 "$H" 2>/dev/null | tr '\n' ' ')"
    fails=$((fails + 1))
}
# win NAME CMD...: start a client, remember its pid under NAME.
win() {
    local n=$1
    shift
    "$@" >/dev/null 2>&1 &
    PID[$n]=$!
    sleep 1
}
hintd() {
    "$HINTD" "$@" 2>>/tmp/hintd-$desk.log &
    HD=$!
    sleep 0.5
}
stop_hintd() {
    kill "$HD"
    wait "$HD" 2>/dev/null
}

# The memory scenario (GPU=1), after a desktop's own steps, on workspace 1:
# M, a terminal whose child (nvt-game named as the terminal: a helper of the
# same app, like Chrome's GPU process, which the policy does not spare as it
# spares a game loading under its launcher) holds 2 GB of vidmem, is hidden
# on workspace 1 while G, a terminal on workspace 2 in focus, has nvt-game
# ask for 1 GB more than is free: the driver makes room by moving M's memory
# to sysmem. G ends, workspace 1 is shown again: M gets its memory back. Both
# read back what they wrote. The desktop gives ws N (show workspace N) and
# term NAME CMD... (a terminal window running CMD). Where a process's memory
# is: evict.sh status, which hint-writers-test.sh writes to /gpu/status every
# second (column 4: MB in sysmem).
ram() { awk -v p="$1" '$1 == p {print $4; exit}' /gpu/status 2>/dev/null; }
mfail() { echo "  FAIL $*"; fails=$((fails + 1)); }
wait_log() {   # wait_log FILE PATTERN SECONDS
    local i
    for i in $(seq $(($3 * 5))); do grep -qE "$2" "$1" 2>/dev/null && return 0; sleep 0.2; done
    return 1
}
nvt_end() {    # nvt_end NAME PID LOG: SIGINT, then its result must say errors=0
    kill -INT "$2" 2>/dev/null
    wait_log "$3" '^RESULT' 20 || { mfail "$1 did not end: $(tail -1 "$3")"; return; }
    grep -q '^RESULT.* errors=0' "$3" && echo "  ok   $1 read back what it wrote: $(grep -o 'errors=[0-9]*' "$3" | tail -1)" ||
        mfail "$1 read back wrong data: $(grep -m1 -E '^ERROR' "$3")"
}
mem_scenario() {
    [[ -n ${GPU:-} ]] || return 0
    local mp gp free r i t tn=foot
    case $desk in openbox|xfwm4|i3) tn=xterm ;; esac
    term M sh -c "exec /tmp/nvt/$tn --mb 2048 --chunk-mb 64 --idle --verify-s 2 --floor-mb 0 >/tmp/mem-M.log 2>&1"
    wait_log /tmp/mem-M.log '^START|^FATAL' 30 && grep -q '^START' /tmp/mem-M.log ||
        { mfail "M did not start: $(tail -2 /tmp/mem-M.log)"; return; }
    mp=$(pgrep -u t -P "${PID[M]}")
    expect_has "M, holding $(grep -o 'allocated_mb=[0-9]*' /tmp/mem-M.log), in focus" 'M=f'
    sleep 2
    r=$(ram "$mp")
    [[ ${r:-x} == 0 ]] && echo "  ok   M's memory in vidmem" || mfail "M has ${r:-?} MB in sysmem before it was hidden"
    ws 2
    free=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits)
    term G sh -c "exec /tmp/nvt/nvt-game --mb $((free + 1024)) --chunk-mb 64 --read-mb 256 --fps 30 --floor-mb 0 >/tmp/mem-G.log 2>&1"
    expect_has "workspace 2: G in focus" 'G=f'
    expect_has "workspace 2: M hidden" 'M=h'
    wait_log /tmp/mem-G.log '^START|^FATAL' 60 && grep -q '^START' /tmp/mem-G.log ||
        { mfail "G did not start: $(tail -2 /tmp/mem-G.log)"; return; }
    gp=$(pgrep -u t -x nvt-game)
    for i in $(seq 30); do r=$(ram "$mp"); (( ${r:-0} >= 900 )) && break; sleep 1; done
    (( ${r:-0} >= 900 )) && echo "  ok   M hidden: $r MB of it in sysmem; G asked for $((free + 1024)) MB with $free free, $(grep -o 'allocated_mb=[0-9]*' /tmp/mem-G.log)" ||
        mfail "M hidden: ${r:-?} MB of it in sysmem after 30 s; G: $(grep -m1 '^START' /tmp/mem-G.log)"
    nvt_end G "$gp" /tmp/mem-G.log
    ws 1
    expect_has "workspace 1 again: M shown" 'M=[fv]'
    t=
    for i in $(seq 45); do r=$(ram "$mp"); [[ ${r:-x} == 0 ]] && { t=$i; break; }; sleep 1; done
    [[ -n $t ]] && echo "  ok   M shown: its memory back in vidmem after $t s" ||
        mfail "M shown: ${r:-?} MB of it still in sysmem after 45 s"
    nvt_end M "$mp" /tmp/mem-M.log
}
wl_term() { local n=$1; shift; win "$n" foot -T "$n" "$@"; }
x_term() {
    local n=$1
    shift
    win "$n" xterm -T "$n" -e "$@"
    xdotool search --sync --name "^$n\$" windowactivate --sync 2>/dev/null
}

sway_run() {
    export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
    /usr/local/bin/sway -c /dev/null >/tmp/sway.log 2>&1 &
    local i
    for i in $(seq 50); do SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1); [[ -n $SWAYSOCK ]] && break; sleep 0.1; done
    export SWAYSOCK WAYLAND_DISPLAY=wayland-1
    m() { swaymsg -q "$@"; }
    m "output HEADLESS-1 resolution 1920x1080"
    hintd
    win A foot -T A sleep 300
    win B foot -T B sleep 300
    expect "A and B side by side, B focused" A=v B=f
    m workspace 2
    win C foot -T C sleep 300
    expect "C alone on workspace 2" A=h B=h C=f
    m fullscreen
    expect "C fullscreen" A=h B=h C=f+g
    m fullscreen
    win D foot -T D sleep 300
    m "floating enable, resize set 1920 1080, move position 0 0"
    expect "D floating over the whole of C" A=h B=h C=h D=f
    m "move scratchpad"
    expect "D in the scratchpad" A=h B=h C=f D=h
    m workspace 1
    expect "back on workspace 1" A=v B=f C=h D=h
    m "layout tabbed"
    expect "tabbed: A is a hidden tab" A=h B=f C=h D=h
    ws() { m workspace "$1"; }
    term() { wl_term "$@"; }
    mem_scenario
    stop_hintd
    expect_off "vram-hintd stopped (SIGTERM)"
}

niri_run() {
    Xvfb :5 -screen 0 1920x1080x24 >/tmp/xvfb.log 2>&1 &
    sleep 1
    DISPLAY=:5 niri -c /dev/null >/tmp/niri.log 2>&1 &
    local np=$! i
    for i in $(seq 50); do NIRI_SOCKET=$(ls "$XDG_RUNTIME_DIR"/niri.*.sock 2>/dev/null | head -1); [[ -n $NIRI_SOCKET ]] && break; sleep 0.1; done
    export NIRI_SOCKET WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    a() { niri msg action "$@" >/dev/null; }
    hintd
    win A foot -T A sleep 300
    win B foot -T B sleep 300
    expect "A and B in two columns, B focused" A=v B=f
    a focus-workspace 2
    win C foot -T C sleep 300
    expect "C alone on workspace 2" A=h B=h C=f
    a fullscreen-window
    expect "C fullscreen" A=h B=h C=f+g
    a fullscreen-window
    win D foot -T D sleep 300
    a toggle-window-floating
    expect "D floating above C" A=h B=h C=v D=f
    a focus-workspace 1
    expect "back on workspace 1" A=v B=f C=h D=h
    ws() { a focus-workspace "$1"; }
    term() { wl_term "$@"; }
    mem_scenario
    kill "$np"
    expect_off "niri quit"
    sleep 1
    kill -0 "$HD" 2>/dev/null && { echo "  FAIL vram-hintd still runs after niri quit"; fails=$((fails + 1)); } ||
        echo "  ok   vram-hintd exited with niri"
}

x11_run() {
    local wm=$1
    export DISPLAY=:7
    Xvfb :7 -screen 0 1920x1080x24 >/tmp/xvfb.log 2>&1 &
    sleep 1
    case $wm in
        openbox) openbox >/tmp/wm.log 2>&1 & ;;
        xfwm4) xfwm4 --compositor=off >/tmp/wm.log 2>&1 & ;;
        i3) printf 'font pango:DejaVu Sans 8\n' >/tmp/i3.conf; i3 -c /tmp/i3.conf >/tmp/wm.log 2>&1 & ;;
    esac
    sleep 1.5
    hintd
    if [[ $wm == i3 ]]; then
        m() { i3-msg -q "$@"; }
        win A xterm -T A -e sleep 300
        win B xterm -T B -e sleep 300
        expect "A and B side by side, B focused" A=v B=f
        m layout tabbed
        expect "tabbed: A is a hidden tab" A=h B=f
        m workspace 2
        win C xterm -T C -e sleep 300
        expect "C alone on workspace 2" A=h B=h C=f
        m fullscreen enable
        expect "C fullscreen" A=h B=h C=f+g
        m fullscreen disable
        m workspace 1
        expect "back on workspace 1" A=h B=f C=h
        ws() { m workspace "$1"; }
    else
        win A xterm -T A -geometry 80x24+0+0 -e sleep 300
        win B xterm -T B -geometry 80x24+700+0 -e sleep 300
        # xfwm4's focus stealing prevention may keep A active.
        xdotool search --name '^B$' windowactivate --sync
        expect "A and B apart, B active" A=v B=f
        xdotool search --name '^A$' windowactivate --sync windowsize 900 700 windowmove 600 0
        expect "A active, moved over the whole of B" A=f B=h
        xdotool search --name '^A$' windowminimize
        expect "A minimized" A=h B=f
        wmctrl -n 2
        xdotool set_desktop 1
        win C xterm -T C -e sleep 300
        expect "C alone on desktop 2" A=h B=h C=f
        wmctrl -F -r C -b add,fullscreen
        expect "C fullscreen" A=h B=h C=f+g
        python3 "$SRC/tests/hint-writers/orwin.py" 4 &
        PID[O]=$!
        expect "O: an override-redirect window over the screen" A=h B=h C=h O=f+g
        wait "${PID[O]}"
        xdotool set_desktop 0
        expect "back on desktop 1" A=h B=f C=h
        ws() { xdotool set_desktop $(($1 - 1)); }
    fi
    term() { x_term "$@"; }
    mem_scenario
    stop_hintd
    expect_off "vram-hintd stopped (SIGTERM)"
}

gnome_run() {
    local U=nvidia-vram-hints@open-gpu-kernel-modules
    local E=$HOME/.local/share/gnome-shell/extensions/$U i g
    rm -rf "$E"
    mkdir -p "$E"
    cp "$SRC"/gnome-vram-hints/{extension.js,metadata.json} "$E"/
    gsettings set org.gnome.shell enabled-extensions "['$U']"
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.mutter dynamic-workspaces false
    gsettings set org.gnome.desktop.wm.preferences num-workspaces 2
    gnome-shell --headless --wayland --virtual-monitor 1920x1080 >/tmp/gnome-shell.log 2>&1 &
    g=$!
    for i in $(seq 100); do [[ -S $XDG_RUNTIME_DIR/wayland-0 ]] && break; sleep 0.1; done
    export WAYLAND_DISPLAY=wayland-0
    for i in $(seq 100); do gnome-extensions info $U 2>/dev/null | grep -q 'State: ACTIVE' && break; sleep 0.2; done
    gnome-extensions info $U 2>/dev/null | grep -q 'State: ACTIVE' ||
        { echo "  FAIL the extension is not active: $(gnome-extensions info $U 2>&1 | grep -i state)"; fails=$((fails + 1)); }
    # Leave the overview the shell starts in (nothing has the focus there).
    gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
        --method org.freedesktop.DBus.Properties.Set org.gnome.Shell OverviewActive "<false>" >/dev/null
    win A foot -T A sleep 300
    win B foot -T B sleep 300
    expect "Wayland windows A and B, B focused" A=v B=f
    # Xwayland starts with the first X11 client; mutter has written its
    # cookie already.
    export DISPLAY=:0 XAUTHORITY=$(ls -t "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* | head -1)
    win X xterm -T X -geometry 80x24+0+0 -e sleep 300
    expect "X11 window X (Xwayland) focused" A=v B=v X=f
    xdotool search --name '^X$' windowminimize
    expect "X minimized" A=v B=f X=h
    wmctrl -s 1
    win Y xterm -T Y -e sleep 300
    expect "Y alone on workspace 2" A=h B=h X=h Y=f
    wmctrl -F -r Y -b add,fullscreen
    expect "Y fullscreen" A=h B=h X=h Y=f+g
    python3 "$SRC/tests/hint-writers/orwin.py" 4 &
    PID[O]=$!
    expect "O: an override-redirect window over the screen" A=h B=h X=h Y=h O=f+g
    wait "${PID[O]}"
    wmctrl -s 0
    expect "back on workspace 1" A=v B=f X=h Y=h
    gnome-extensions disable $U
    expect_off "the extension disabled"
    gnome-extensions enable $U
    expect "the extension enabled again" A=v B=f X=h Y=h
    ws() { wmctrl -s $(($1 - 1)); }
    term() { wl_term "$@"; }
    mem_scenario
    kill "$g"
    wait "$g" 2>/dev/null
    expect_off "gnome-shell quit (SIGTERM)"
}

# Hyprland's IPC from a stand-in (hyprland-ipc.py), in the format of 0.57 or
# of 0.41 and before. Scenes: clients bottom to top; pids are made up.
hyprland_run() {
    local fmt=$1 sc=/tmp/hypr-scene.json
    export HYPRLAND_INSTANCE_SIGNATURE=test
    PID=([A]=101 [B]=102 [C]=103 [D]=104 [E]=105 [F]=106)
    scene() {   # scene INACTIVE_OPACITY ACTIVE MONITORS CLIENTS
        printf '{"format": "%s", "inactive_opacity": %s, "active": %s, "monitors": %s, "clients": %s}\n' \
            "$fmt" "$1" "$2" "$3" "$4" >$sc.tmp && mv $sc.tmp $sc
    }
    local M='[{"x": 0, "y": 0, "width": 1920, "height": 1080, "ws": "1"}]'
    local MS='[{"x": 0, "y": 0, "width": 1920, "height": 1080, "ws": "1", "special": "special:magic"}]'
    local M2='[{"x": 0, "y": 0, "width": 3840, "height": 2160, "scale": 2.0, "ws": "1"}]'
    local a='{"pid": 101, "ws": "1", "at": [0, 0], "size": [960, 1080]}'
    local b='{"pid": 102, "ws": "1", "at": [960, 0], "size": [960, 1080]}'
    local c='{"pid": 103, "ws": "2", "at": [0, 0], "size": [1920, 1080]}'
    local d='{"pid": 104, "ws": "1", "at": [0, 0], "size": [1000, 1080], "floating": true}'
    scene 1.0 1 "$M" "[$a, $b, $c]"
    python3 "$SRC/tests/hint-writers/hyprland-ipc.py" $sc &
    sleep 0.5
    hintd
    expect "A and B tiled on 1, B active, C on 2" A=v B=f C=h
    scene 1.0 3 "$M" "[$a, $b, $c, $d]"
    expect "D floating over the whole of A" A=h B=v C=h D=f
    scene 0.9 1 "$M" "[$a, $b, $c, $d]"
    expect "the same, B active, inactive windows translucent" A=v B=f C=h D=v
    local bf='{"pid": 102, "ws": "1", "at": [0, 0], "size": [1920, 1080], "fullscreen": 2}'
    scene 1.0 1 "$M" "[$a, $bf, $c, $d]"
    expect "B fullscreen: above the floating D" A=h B=f+g C=h D=h
    local bm='{"pid": 102, "ws": "1", "at": [0, 0], "size": [1920, 1080], "fullscreen": 1}'
    scene 1.0 1 "$M" "[$a, $bm, $c, $d]"
    expect "B maximized: not fullscreen, D floats above it" A=h B=f C=h D=v
    local e='{"pid": 105, "ws": "special:magic", "at": [0, 0], "size": [1920, 1080], "floating": true}'
    local f='{"pid": 106, "ws": "1", "at": [960, 0], "size": [960, 1080], "hidden": true}'
    scene 1.0 3 "$MS" "[$a, $b, $c, $e, $f]"
    expect "E on the shown special workspace, F a hidden group member" A=h B=h C=h E=f F=h
    local ao='{"pid": 101, "ws": "1", "at": [1920, 0], "size": [960, 1080]}'
    scene 1.0 1 "$M2" "[$ao, $b]"
    expect "a 4K monitor at scale 2: A is off the monitor" A=h B=f
    touch $sc.quit
    expect_off "Hyprland quit (event socket closed)"
    rm -f $sc.quit
}

# The real Hyprland (GPU=1, 0.57 or later), on the GPU's DRM device with a
# headless output: the steps of the stand-in's scenes, from the compositor
# itself.
hyprland_real_run() {
    local hp i
    export LIBSEAT_BACKEND=noop XDG_SESSION_TYPE=wayland
    # 0.57 reads its configuration as Lua, and hyprctl dispatch takes a Lua
    # dispatcher; options change with hyprctl eval.
    cat >/tmp/hyprland.lua <<'EOF'
hl.monitor({ output = "", mode = "1920x1080@60", position = "0x0", scale = 1 })
hl.config({
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    ecosystem = { no_update_news = true, no_donation_nag = true },
})
EOF
    Hyprland -c /tmp/hyprland.lua >/tmp/hyprland.log 2>&1 &
    hp=$!
    for i in $(seq 100); do
        HYPRLAND_INSTANCE_SIGNATURE=$(ls -t "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
        [[ -n $HYPRLAND_INSTANCE_SIGNATURE && -S $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock ]] && break
        sleep 0.1
    done
    export HYPRLAND_INSTANCE_SIGNATURE
    # No monitor is connected to the GPU: a headless output.
    hyprctl monitors 2>/dev/null | grep -q '^Monitor' || hyprctl output create headless >/dev/null
    export WAYLAND_DISPLAY=$(hyprctl -j instances | python3 -c 'import json, sys; print(json.load(sys.stdin)[0]["wl_socket"])')
    if ! hyprctl monitors 2>/dev/null | grep -q '^Monitor'; then
        mfail "Hyprland did not start: $(tail -5 /tmp/hyprland.log | tr '\n' ' ')"
        return
    fi
    [[ $(hyprctl configerrors) == ok || -z $(hyprctl configerrors) ]] || mfail "Hyprland config: $(hyprctl configerrors)"
    d() { hyprctl dispatch "$1" >/dev/null; }
    hintd
    win A foot -T A sleep 300
    win B foot -T B sleep 300
    expect "A and B tiled, B focused" A=v B=f
    d 'hl.dsp.focus({ workspace = 2 })'
    win C foot -T C sleep 300
    expect "C alone on workspace 2" A=h B=h C=f
    d 'hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" })'
    expect "C fullscreen" A=h B=h C=f+g
    d 'hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" })'
    expect "C maximized: not fullscreen" A=h B=h C=f
    d 'hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" })'
    d 'hl.dsp.focus({ workspace = 1 })'
    expect "back on workspace 1" A=v B=f C=h
    win D foot -T D sleep 300
    d 'hl.dsp.window.float({ action = "toggle" })'
    d 'hl.dsp.window.resize({ x = 1920, y = 1080, relative = false })'
    d 'hl.dsp.window.move({ x = 0, y = 0, relative = false })'
    expect "D floating over the whole of A and B" A=h B=h C=h D=f
    # D is the active window: active_opacity makes it translucent.
    hyprctl eval 'hl.config({ decoration = { active_opacity = 0.9 } })' >/dev/null
    expect "the active window translucent: A and B show through D" A=v B=v C=h D=f
    hyprctl eval 'hl.config({ decoration = { active_opacity = 1.0 } })' >/dev/null
    expect "opaque again" A=h B=h C=h D=f
    d 'hl.dsp.window.move({ workspace = "special:magic", follow = false })'
    expect "D on the special workspace, not shown" A=v B=f C=h D=h
    d 'hl.dsp.workspace.toggle_special("magic")'
    expect "the special workspace shown, D over everything" A=h B=h C=h D=f
    d 'hl.dsp.workspace.toggle_special("magic")'
    expect "the special workspace hidden" A=v B=f C=h D=h
    ws() { d "hl.dsp.focus({ workspace = $1 })"; }
    term() { wl_term "$@"; }
    mem_scenario
    kill "$hp"
    for i in $(seq 100); do kill -0 "$hp" 2>/dev/null || break; sleep 0.2; done
    expect_off "Hyprland quit"
    sleep 1
    kill -0 "$HD" 2>/dev/null && mfail "vram-hintd still runs after Hyprland quit" || echo "  ok   vram-hintd exited with Hyprland"
}

echo "== $desk"
case $desk in
    sway) sway_run ;;
    niri) niri_run ;;
    openbox|xfwm4|i3) x11_run "$desk" ;;
    gnome) gnome_run ;;
    hyprland) if [[ -n ${GPU:-} ]]; then hyprland_real_run; else hyprland_run 0.57; fi ;;
    hyprland-old) hyprland_run old ;;
    *) echo "unknown desktop $desk"; exit 2 ;;
esac
if grep -qE 'ERROR: AddressSanitizer|runtime error:' /tmp/hintd-$desk.log 2>/dev/null; then
    echo "  FAIL vram-hintd: sanitizer report"
    grep -E -A12 'ERROR: AddressSanitizer|runtime error:' /tmp/hintd-$desk.log | head -30 | sed 's/^/    /'
    fails=$((fails + 1))
fi
(( fails == 0 )) && echo "RUN $desk PASS" || echo "RUN $desk FAIL $fails"
