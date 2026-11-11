#!/bin/bash
# Unattended test bench for vidmem eviction. Every scenario runs synthetic processes only (nvt-game
# under different names, compr-test), checks their memory content, the kernel
# log and timing, and ends with one line
#   NVTEST <scenario> PASS|FAIL <reasons>
# Logs: local/stress-logs/nvtest-<date_time>/<scenario>/.
#
#   ./nvtest.sh integrity [CYCLES]   idle apps (plain buffers + a compressed image):
#                                    evict/restore CYCLES times (default 5), content
#                                    checked while evicted and after each restore;
#                                    reports evict/restore throughput
#   ./nvtest.sh active [CYCLES]      evict/restore an app that keeps rendering
#                                    (30 fps): the quiesce must not corrupt it or
#                                    stall other apps
#   ./nvtest.sh game [SECONDS]       a "game" that fills VRAM, background apps (idle
#                                    and rendering), automatic policy on: idle apps
#                                    must move out, the game and the rendering app
#                                    must not, game fps is compared with auto off
#   ./nvtest.sh chrome [CYCLES] [anim|idle]
#                                    a real Chrome drawing a self-checking WebGL page
#                                    (nvt-webgl.html) moved to sysmem and back
#   ./nvtest.sh switch [CYCLES]      window hints: fullscreen "game" Chrome B and a hidden
#                                    Chrome A; alt-tab CYCLES times, switch latency
#   ./nvtest.sh webgpu [CYCLES]      WebGPU page in Chrome moved and back; a new tab must
#                                    still get an adapter
#   ./nvtest.sh fgprio [CYCLES] [idle|anim]
#                                    two monitors: a visible idle Chrome gives memory to the
#                                    fullscreen game, a drawing one only as the last resort (R5) and keeps drawing; clicking between
#   ./nvtest.sh quiesce [CYCLES]     active scenario once per stop method
#                                    (evict.sh quiesce 0..4, NVTEST_MODES="0 2" to pick)
#   ./nvtest.sh fgback               the focused game's memory moved out while only part of
#                                    it fits: that part back within seconds, the rest once
#                                    there is room, its content intact (problem #10)
#   ./nvtest.sh focus                two monitors: clicking a window on monitor 2 makes it
#                                    the foreground, not the fullscreen game on monitor 1;
#                                    a WebGPU page it opens gets memory from the game (R4, #7)
#   ./nvtest.sh vaflip               a page that remakes a texture every frame moved out and
#                                    back every 0.3-1 s for a minute: same picture, no VA
#                                    errors in the kernel log (#4)
#   ./nvtest.sh comproom             vidmem full: buffers of the compositor (a process named
#                                    kwin_wayland) get room from a hidden app, about what they
#                                    need, not from the drawing game (#12)
#   ./nvtest.sh ctxroom              vidmem nearly full: a new process still gets its channel;
#                                    after a failure the next one works once memory is free (#13, #3)
#   ./nvtest.sh all                  integrity, active, chrome anim+idle, webgpu, occlusion,
#                                    switch, fgprio idle+anim, game, fgback, focus, vaflip,
#                                    comproom, ctxroom
#
# Scenarios that put the whole GPU under pressure (game) run only while nobody
# is logged in on seat0, so they never touch the user's session; override with
# NVTEST_FORCE=1. integrity and active only touch their own processes.
set -euo pipefail
# Whatever way this ends (set -e included), no test browser or nvt-game is
# left running: it kept vidmem full and the caller's ssh session open
# (2026-11-06 23:14, nvtest focus; the reset that followed wedged the GPU).
trap '"$(dirname -- "${BASH_SOURCE[0]}")/nvt-session.sh" stop >/dev/null 2>&1 || true; pkill -P $$ >/dev/null 2>&1 || true' EXIT

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
EVICT="sudo -n $HERE/../evict.sh"
LOGROOT=${LOGROOT:-$HERE/../stress-logs/nvtest-$(date +%F_%H%M%S)}
# Kernel log lines that fail any scenario.
BAD_K_RE='Xid|NVRM: .*([^a-zA-Z]fault|FAULT)|GPU has fallen off|RmInitAdapter failed|BUG:|WARNING:'

die() { echo "nvtest.sh: $*" >&2; exit 2; }
[[ -x $HERE/nvt-game && -x $HERE/compr-test ]] || die "tools missing, run ../build.sh tools"
ls /proc/driver/nvidia/gpus/*/vidmem_evict >/dev/null 2>&1 || die "patched module with vidmem_evict not loaded"

seat0_busy() {
    local s
    for s in $(loginctl list-sessions --no-legend | awk '{print $1}'); do
        [[ $(loginctl show-session "$s" -p Seat --value 2>/dev/null) == seat0 &&
           $(loginctl show-session "$s" -p Class --value 2>/dev/null) == user ]] && return 0
    done
    return 1
}

# --- per-scenario state -------------------------------------------------------
DIR= ; T0= ; FAILS=() ; PIDS=()

begin() {
    DIR=$LOGROOT/$1
    mkdir -p "$DIR"
    T0=$(date '+%F %T')
    FAILS=()
    PIDS=()
    echo "== $1 ($(date +%T)), logs in $DIR"
    $EVICT status >"$DIR/status-before.txt" 2>&1 || true
    nvidia-smi dmon -s ut -d 1 >"$DIR/dmon.log" 2>&1 &
    PIDS+=($!)
}

fail() { FAILS+=("$*"); echo "  FAIL: $*"; }
note() { echo "  $*" | tee -a "$DIR/notes.txt"; }

# start NAME ARGS...: runs nvt-game as NAME (a symlink, so comm == NAME) and
# sets LAST to its pid. Not called through $(...): the process must stay a
# child of this shell, and fail() must reach FAILS.
LAST=
start() {
    local name=$1; shift
    ln -sf nvt-game "$HERE/$name"
    "$HERE/$name" "$@" >"$DIR/$name.log" 2>&1 &
    LAST=$!
    PIDS+=("$LAST")
    local i
    for i in $(seq 1 100); do
        grep -q '^START\|^FATAL' "$DIR/$name.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q '^START' "$DIR/$name.log" || { fail "$name did not start: $(tail -1 "$DIR/$name.log")"; return 1; }
}

start_compr() {
    "$HERE/compr-test" --size 4096 --mode hold --interval-ms 500 --keep-going >"$DIR/compr-test.log" 2>&1 &
    LAST=$!
    PIDS+=("$LAST")
    sleep 2
}

# evict_op MODE PID LABEL: runs evict.sh, logs it, prints "<MB> <ms>".
evict_op() {
    local out
    out=$($EVICT "$1" "$2" 2>&1) || true
    printf '%s\n' "--- $(date +%T) $1 $2 ($3)" "$out" >>"$DIR/evict.log"
    sed -n 's/^summary: [a-z]* [0-9]* allocs \([0-9]*\) MB.*failed \([0-9]*\) .*, \([0-9]*\) ms$/\1 \3 \2/p' <<<"$out"
}

# finish: stop everything, final checks, verdict.
finish() {
    local name=$1 p
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null || true; done
    $EVICT status >"$DIR/status-after.txt" 2>&1 || true
    journalctl -k --since "$T0" --no-pager >"$DIR/kernel.log" 2>&1 || true
    if grep -E "$BAD_K_RE" "$DIR/kernel.log" >"$DIR/kernel-bad.log"; then
        fail "kernel log: $(head -1 "$DIR/kernel-bad.log" | cut -c1-160)"
    fi
    local f
    for f in "$DIR"/nvt-*.log; do
        [[ -e $f ]] || continue
        local r
        r=$(grep '^RESULT' "$f" | tail -1)
        [[ -n $r ]] || { fail "$(basename "$f" .log): no RESULT ($(tail -1 "$f" | cut -c1-100))"; continue; }
        [[ $r == *" errors=0" ]] || fail "$(basename "$f" .log): memory content wrong: ${r##* }"
    done
    if [[ -e $DIR/compr-test.log ]] && grep -q MISMATCH "$DIR/compr-test.log"; then
        fail "compr-test: $(grep -m1 MISMATCH "$DIR/compr-test.log")"
    fi
    if grep -q "^total evicted now: [1-9]" "$DIR/status-after.txt" 2>/dev/null; then
        note "left evicted after the test: $(grep '^total evicted now' "$DIR/status-after.txt")"
    fi
    local verdict=PASS
    [[ ${#FAILS[@]} -eq 0 ]] || verdict=FAIL
    local line="NVTEST $name $verdict ${FAILS[*]:-}"
    echo "$line" | tee "$DIR/verdict.txt"
    echo "$line" >>"$LOGROOT/summary.txt"
    [[ $verdict == PASS ]]
}

# --- scenarios ----------------------------------------------------------------

integrity() {
    local cycles=${1:-5}
    begin integrity
    local a b c
    start nvt-idle-a --mb 1024 --idle || { finish integrity; return 1; }; a=$LAST
    start nvt-idle-b --mb 300 --chunk-mb 100 --idle || { finish integrity; return 1; }; b=$LAST
    start_compr; c=$LAST
    local i tot_mb=0 tot_ev=0 tot_rs=0 pid r
    for i in $(seq 1 "$cycles"); do
        for pid in "$a" "$b" "$c"; do
            r=$(evict_op evict "$pid" "cycle $i")
            read -r mb ms failed <<<"${r:-0 0 0}"
            [[ ${failed:-0} -eq 0 ]] || fail "cycle $i evict $pid: $failed failed"
            tot_mb=$((tot_mb + mb)); tot_ev=$((tot_ev + ms))
        done
        # Content check while the memory sits in sysmem.
        kill -USR1 "$a" "$b"; sleep 1.5
        for pid in "$a" "$b" "$c"; do
            r=$(evict_op restore "$pid" "cycle $i")
            read -r mb ms failed <<<"${r:-0 0 0}"
            [[ ${failed:-0} -eq 0 ]] || fail "cycle $i restore $pid: $failed failed"
            tot_rs=$((tot_rs + ms))
        done
        kill -USR1 "$a" "$b"; sleep 1.5
    done
    if [[ $tot_ev -gt 0 && $tot_rs -gt 0 ]]; then
        note "moved $tot_mb MB each way in $cycles cycles: evict $((tot_mb * 1000 / tot_ev)) MB/s, restore $((tot_mb * 1000 / tot_rs)) MB/s"
    fi
    finish integrity
}

active() {
    local cycles=${1:-3} label=${2:-active}
    begin "$label"
    local bg heavy w
    # Short frames (well under the drain wait) and long frames (~50 ms of GPU
    # work each, more from sysmem): a quiesce that only waits a fixed time
    # moves memory under the long ones while they run.
    start nvt-bg-a --mb 512 --fps 30 --read-mb 128 --gfx --write || { finish active; return 1; }; bg=$LAST
    start nvt-bg-heavy --mb 512 --chunk-mb 512 --fps 5 --read-mb 16384 --gfx --write || { finish active; return 1; }; heavy=$LAST
    # A renderer that is never moved: its frame times show stalls the quiesce
    # of another process causes.
    start nvt-bg-witness --mb 1024 --fps 60 --read-mb 1024 --gfx --write || { finish active; return 1; }; w=$LAST
    sleep 3
    local i r pid
    for i in $(seq 1 "$cycles"); do
        for pid in "$bg" "$heavy"; do
            r=$(evict_op evict "$pid" "cycle $i")
            note "cycle $i evict $pid: ${r:-none}"
            sleep 2
            r=$(evict_op restore "$pid" "cycle $i")
            note "cycle $i restore $pid: ${r:-none}"
            sleep 2
        done
    done
    kill -USR1 "$bg" "$heavy"; sleep 2
    # A frame that never completes is the failure mode of an unsafe quiesce.
    local name
    for name in nvt-bg-a nvt-bg-heavy; do
        grep -q '^FATAL' "$DIR/$name.log" && fail "$name: $(grep -m1 '^FATAL' "$DIR/$name.log")"
        local last
        last=$(awk '/^T /{t=$2} END{sub("t=","",t); print t+0}' "$DIR/$name.log")
        local alive=$(( $(date +%s) - $(stat -c %Y "$DIR/$name.log") ))
        [[ $alive -lt 10 ]] || fail "$name stopped making frames (last report t=$last s, silent ${alive} s)"
    done
    local worst
    worst=$(awk '/^T /{for(i=1;i<=NF;i++) if($i ~ /^max_ms=/){split($i,a,"="); if(a[2]>m) m=a[2]}} END{print m+0}' "$DIR/nvt-bg-witness.log")
    note "witness worst frame ${worst} ms (heavy app drawing from sysmem while moved out holds GR up to ~250 ms)"
    # The stop itself takes 1-3 ms; a witness stuck for over a second means
    # the quiesce or a fault hit the other process.
    awk -v w="$worst" 'BEGIN{exit !(w > 1000)}' && fail "witness stalled ${worst} ms while another process was moved"
    # Ask them to stop (they print RESULT); one stuck in the driver does not
    # react to SIGTERM within its 30 s fence timeout and gets killed.
    for pid in "$bg" "$heavy"; do kill "$pid" 2>/dev/null || true; done
    local k
    for k in $(seq 1 50); do
        kill -0 "$bg" 2>/dev/null || kill -0 "$heavy" 2>/dev/null || break
        sleep 0.2
    done
    for pid in "$bg" "$heavy"; do
        if kill -0 "$pid" 2>/dev/null; then
            fail "pid $pid did not exit within 10 s of SIGTERM (stuck in the GPU?)"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    grep -h '^quiesce:' "$DIR/evict.log" | sort | uniq -c | sort -rn | head -3 | sed 's/^/  /' | tee -a "$DIR/notes.txt"
    grep -h '^phases ms:' "$DIR/evict.log" | tail -2 | sed 's/^/  /' | tee -a "$DIR/notes.txt"
    finish "$label"
}

# A real Chrome (nvt-session.sh: virtual KWin + test profile) drawing
# nvt-webgl.html, whose page hashes its own output every 30 frames: the GPU
# process is moved to sysmem and back CYCLES times while it draws (MODE anim)
# or while it sits in the background (MODE idle). Any changed hash, lost
# WebGL context or Xid fails.
chrome() {
    local cycles=${1:-5} mode=${2:-anim} label=${3:-chrome-${2:-anim}}
    begin "$label"
    "$HERE/nvt-session.sh" stop >/dev/null 2>&1 || true
    "$HERE/nvt-session.sh" start "file://$HERE/nvt-webgl.html?mode=$mode" >>"$DIR/notes.txt" 2>&1 ||
        { fail "test session did not start"; finish "$label"; return 1; }
    local g i r t
    for i in $(seq 1 60); do
        t=$("$HERE/nvt-session.sh" title 2>/dev/null | grep '^NVT' || true)
        [[ $t == *"checks="[1-9]* ]] && break
        sleep 0.5
    done
    g=$("$HERE/nvt-session.sh" gpu-pid)
    [[ -n $g && $t == NVT* ]] || { fail "chrome page not running (title '$t', gpu pid '$g')"; "$HERE/nvt-session.sh" stop; finish "$label"; return 1; }
    note "gpu process $g, page: $t"
    for i in $(seq 1 "$cycles"); do
        r=$(evict_op evict "$g" "cycle $i")
        note "cycle $i evict: ${r:-none} | $("$HERE/nvt-session.sh" title | grep '^NVT')"
        sleep 4
        r=$(evict_op restore "$g" "cycle $i")
        note "cycle $i restore: ${r:-none} | $("$HERE/nvt-session.sh" title | grep '^NVT')"
        sleep 4
    done
    t=$("$HERE/nvt-session.sh" title | grep '^NVT' || true)
    note "final: $t"
    [[ $t == *" bad=0 "* ]] || fail "page output changed: $t"
    [[ $t == *" lost=0 "* ]] || fail "WebGL context lost: $t"
    "$HERE/nvt-session.sh" stop
    finish "$label"
}

# Real-time switching with window hints (KWin script in the test compositor):
# Chrome B is a fullscreen "game" streaming textures until it needs more
# than is free; Chrome A sits behind it (covered = hidden) and must move out
# when B's allocations need the room. Then alt-tab CYCLES times: minimize B
# (A visible: back to vidmem, B out), activate B (B back, A out). Measures
# how long each switch takes until the shown app is fully in vidmem, checks
# both pages' output and B's fps.
ram_of() { $EVICT status 2>/dev/null | awk -v p="$1" '$1 == p { print $4; exit }'; }
vram_of() { $EVICT status 2>/dev/null | awk -v p="$1" '$1 == p { print $3; exit }'; }
title_of() { NVT_INSTANCE=$1 "$HERE/nvt-session.sh" title 2>/dev/null | grep '^NVT' | head -1; }

# wait_ram PID [T0]: seconds from T0 (default now) until PID has nothing in
# sysmem (timeout 10 s), or "timeout".
# LEFT_MB (default 0): this much may stay in sysmem and it still counts as back.
wait_ram() {
    local pid=$1 t0=${2:-$(date +%s.%N)} left=${3:-0} r
    while :; do
        r=$(ram_of "$pid")
        [[ ${r:-0} -le $left ]] && { awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN { printf "%.3f\n", a - b }'; return 0; }
        awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN { exit !(a - b > 10) }' && { echo timeout; return 1; }
        sleep 0.05
    done
}

switch_test() {
    local cycles=${1:-3}
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST switch SKIP somebody is logged in on seat0" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin switch
    local S="$HERE/nvt-session.sh" t mA mB gA gB free gameMB i lat r
    $S stop >/dev/null 2>&1 || true
    $S start "file://$HERE/nvt-webgl.html?mode=anim" >>"$DIR/notes.txt" 2>&1 || { fail "session A"; finish switch; return 1; }
    for i in $(seq 1 60); do t=$(title_of a); [[ $t == *"checks="[1-9]* ]] && break; sleep 0.5; done
    mA=$(NVT_INSTANCE=a $S main-pid); gA=$(NVT_INSTANCE=a $S gpu-pid)
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    # B wants what is free plus A's whole size: it can only load if A moves out
    # (its page counts 5.34 MB per texture, the driver sees ~5.1).
    gameMB=$(( free + $(vram_of "$gA") ))
    note "A: browser $mA gpu $gA, $t; free outside reserve $free MB, game B wants $gameMB MB"

    NVT_INSTANCE=b $S start "file://$HERE/nvt-webgl.html?mode=game&mb=$gameMB" >>"$DIR/notes.txt" 2>&1 ||
        { fail "session B"; $S stop; finish switch; return 1; }
    sleep 2
    mB=$(NVT_INSTANCE=b $S main-pid)
    $S win fullscreen "$mB"
    for i in $(seq 1 240); do t=$(title_of b); [[ $t == *" done=1 "* ]] && break; sleep 0.5; done
    gB=$(NVT_INSTANCE=b $S gpu-pid)
    note "B: browser $mB gpu $gB, $t"
    note "hints: $(tr '\n' ' ' < /proc/driver/nvidia/vidmem_hints)"
    note "after loading: A gpu vram $(vram_of "$gA") MB ram $(ram_of "$gA") MB; B gpu vram $(vram_of "$gB") MB ram $(ram_of "$gB") MB"
    [[ $t == *" done=1 "* ]] || fail "game B did not finish loading: $t"
    [[ $t == *" oom=0 "* ]] || fail "game B ran out of memory: $t"
    # A behind fullscreen B is hidden; it may stay if it keeps drawing there
    # (GP_PUT moves: drawing hidden apps are not moved, they would draw from
    # sysmem). Moved out or not, B must have loaded and both must be intact.
    note "hidden A moved out for B: $(ram_of "$gA") MB (0 is right if A kept drawing while covered)"

    for i in $(seq 1 "$cycles"); do
        t=$(date +%s.%N)
        $S win minimize "$mB"
        lat=$(wait_ram "$gA" "$t") || fail "cycle $i: A not back in vidmem within 10 s"
        sleep 2
        note "cycle $i alt-tab to A: A back in ${lat} s; A $(title_of a | cut -d' ' -f2-5); B ram $(ram_of "$gB") MB"
        t=$(date +%s.%N)
        $S win activate "$mB"
        lat=$(wait_ram "$gB" "$t") || fail "cycle $i: B not back in vidmem within 10 s"
        sleep 2
        note "cycle $i back to B: B back in ${lat} s; B $(title_of b | grep -o 'fps=[0-9]*'); A ram $(ram_of "$gA") MB"
    done

    for i in a b; do
        t=$(title_of $i)
        note "final $i: $t"
        [[ $t == *" bad=0 "* && $t == *" lost=0 "* ]] || fail "page $i output changed or context lost: $t"
    done
    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: hints" | cut -c 40- >"$DIR/policy.log" || true
    $S stop
    finish switch
}

# Foreground priority on two monitors: Chrome A (a page that redraws every
# 60 s) stays VISIBLE on monitor 2, the fullscreen "game" Chrome B on monitor
# 1 needs A's memory. A must give it up (visible but idle) without being
# shown wrong; then the user clicks A (A foreground, B still visible and
# drawing): A comes back at once, B gives up only what is missing; then
# back to B.
fgprio_test() {
    local cycles=${1:-3} amode=${2:-idle} label
    label=fgprio-$amode
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST $label SKIP somebody is logged in on seat0" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin "$label"
    local S="$HERE/nvt-session.sh" t mA mB gA gB free gameMB i lat
    $S stop >/dev/null 2>&1 || true
    NVT_OUTPUTS=2 $S start "file://$HERE/nvt-webgl.html?mode=$([[ $amode == idle ]] && echo 'idle&every=60' || echo anim)&tex=200" >>"$DIR/notes.txt" 2>&1 ||
        { fail "session A"; finish "$label"; return 1; }
    for i in $(seq 1 60); do t=$(title_of a); [[ $t == *"checks="[1-9]* ]] && break; sleep 0.5; done
    mA=$(NVT_INSTANCE=a $S main-pid); gA=$(NVT_INSTANCE=a $S gpu-pid)
    $S win screen2 "$mA"
    note "A on monitor 2: browser $mA gpu $gA, $t"
    sleep 14    # A idle on the GPU for over 10 s
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    gameMB=$(( free + $(vram_of "$gA") ))
    # B starts streaming 12 s after it opens: A redraws when it loses focus
    # and must be idle on the GPU for 10 s before it may give up memory.
    NVT_INSTANCE=b $S start "file://$HERE/nvt-webgl.html?mode=game&mb=$gameMB&delay=12" >>"$DIR/notes.txt" 2>&1 ||
        { fail "session B"; $S stop; finish "$label"; return 1; }
    sleep 2
    mB=$(NVT_INSTANCE=b $S main-pid)
    $S win screen1 "$mB"
    sleep 0.5
    $S win fullscreen "$mB"
    for i in $(seq 1 240); do t=$(title_of b); [[ $t == *" done=1 "* ]] && break; sleep 0.5; done
    gB=$(NVT_INSTANCE=b $S gpu-pid)
    note "B: browser $mB gpu $gB, wants $gameMB MB, $t"
    grep -q "^$mA v" /proc/driver/nvidia/vidmem_hints || fail "A is not visible on monitor 2 (test setup)"
    note "hints: $(tr '\n' ' ' < /proc/driver/nvidia/vidmem_hints)"
    note "after loading: A vram $(vram_of "$gA") ram $(ram_of "$gA") MB; B vram $(vram_of "$gB") ram $(ram_of "$gB") MB"
    [[ $t == *" done=1 "* && $t == *" oom=0 "* ]] || fail "game B did not load: $t"
    if [[ $amode == idle ]]; then
        [[ $(ram_of "$gA") -gt 0 ]] || fail "visible idle A gave up nothing for the foreground game"
    else
        #
        # Drawing: it gives memory only as the last resort, when the focused
        # game finds nothing else (rule R5, the user's choice "the game
        # comes first"; here B wants all of vidmem, so A gives). It must go
        # on drawing, correctly, from sysmem meanwhile (2026-11-02 13:15:
        # CoD at 1 fps was a drawing Chrome moved out for the game's
        # streaming, with hidden apps still holding memory).
        #
        local f1 f2
        f1=$(title_of a | sed -n 's/.*frames=\([0-9]*\).*/\1/p')
        sleep 3
        f2=$(title_of a | sed -n 's/.*frames=\([0-9]*\).*/\1/p')
        note "A (visible, drawing) ram $(ram_of "$gA") MB, frames $f1 -> $f2 in 3 s"
        [[ -n $f1 && -n $f2 && $f2 -gt $f1 ]] || fail "visible drawing A stopped drawing: frames $f1 -> $f2"
    fi

    for i in $(seq 1 "$cycles"); do
        t=$(date +%s.%N)
        $S win activate "$mA"
        lat=$(wait_ram "$gA" "$t") || fail "cycle $i: A not back within 10 s"
        sleep 2
        note "cycle $i click A: A back in ${lat} s; B (visible, drawing) ram $(ram_of "$gB") MB, $(title_of b | grep -o 'fps=[0-9]*')"
        t=$(date +%s.%N)
        $S win activate "$mB"
        #
        # B fills vidmem to the last MB, and its memory is 6 MB contiguous
        # buffers: one or two of them may find no three free 2 MB chunks in a
        # row when it comes back and stay in sysmem (2 runs of 8 on the RTX
        # 5000, 2026-11-11, on 67deb430 and on the current build alike;
        # idea I15). Up to 16 MB left is "back"; the note says how much.
        #
        lat=$(wait_ram "$gB" "$t" 16) || fail "cycle $i: B not back within 10 s"
        sleep 2
        note "cycle $i click B: B back in ${lat} s, $(ram_of "$gB") MB left in sysmem; $(title_of b | grep -o 'fps=[0-9]*'); A ram $(ram_of "$gA") MB"
    done
    for i in a b; do
        t=$(title_of $i)
        note "final $i: $t"
        [[ $t == *" bad=0 "* && $t == *" lost=0 "* ]] || fail "page $i output changed or context lost: $t"
    done
    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: hints" | cut -c 40- >"$DIR/policy.log" || true
    $S stop
    finish "$label"
}

# WebGPU in a real Chrome (Dawn on Vulkan): nvt-webgpu.html checks and
# rewrites 512 MB of storage buffers every frame while the GPU process is
# moved to sysmem and back CYCLES times. Afterwards a new tab must still get
# a WebGPU adapter and device (2026-11-02 morning: after an eviction
# experiment Chrome reported "WebGPU unsupported / no adapter").
webgpu_test() {
    local cycles=${1:-5} S="$HERE/nvt-session.sh" g i r t t2
    begin webgpu
    $S stop >/dev/null 2>&1 || true
    $S start "file://$HERE/nvt-webgpu.html?mb=512" >>"$DIR/notes.txt" 2>&1 || { fail "session"; finish webgpu; return 1; }
    for i in $(seq 1 60); do t=$($S title 2>/dev/null | grep '^WGPU' | head -1 || true); [[ $t == *"checks="[1-9]* ]] && break; sleep 0.5; done
    g=$($S gpu-pid)
    note "gpu process $g, page: $t"
    [[ $t == *"checks="[1-9]* ]] || { fail "WebGPU page not running: '$t'"; $S stop; finish webgpu; return 1; }
    local m
    m=$($S main-pid)
    for i in $(seq 1 "$cycles"); do
        # Minimized while moved out (else the policy brings the foreground
        # page back at once); WebGPU keeps running from sysmem meanwhile.
        $S win minimize "$m"
        sleep 0.5
        r=$(evict_op evict "$g" "cycle $i")
        sleep 3
        $S win activate "$m"
        sleep 0.5
        note "cycle $i evict: ${r:-none} | $($S title | grep '^WGPU' | head -1 || true)"
        r=$(evict_op restore "$g" "cycle $i")
        sleep 3
        note "cycle $i restore: ${r:-none} | $($S title | grep '^WGPU' | head -1 || true)"
    done
    t=$($S title | grep '^WGPU' | head -1 || true)
    sleep 2
    t2=$($S title | grep '^WGPU' | head -1 || true)
    note "final: $t2"
    [[ $t2 == *" errors=0 "* ]] || fail "WebGPU buffer content wrong: $t2"
    [[ $t2 == *" lost=0 "* ]] || fail "WebGPU device lost: $t2"
    [[ $t != "$t2" ]] || fail "WebGPU page stopped drawing: $t2"
    # A fresh page must still get an adapter and a device.
    curl -s -X PUT "http://127.0.0.1:9333/json/new?file://$HERE/nvt-webgpu.html?probe=1" >/dev/null
    for i in $(seq 1 20); do r=$($S title | grep '^WGPU probe' | head -1 || true); [[ -n $r ]] && break; sleep 0.5; done
    note "new tab: ${r:-no answer}"
    [[ $r == *"adapter=ok device=ok"* ]] || fail "WebGPU unavailable after the moves: ${r:-no answer}"
    grep -iE "GPU process exited|GPU process crashed|Lost UI shared context|context lost" /tmp/nvt-chrome/chrome-a.log >"$DIR/chrome-gpu-errors.log" &&
        fail "chrome GPU process trouble: $(head -1 "$DIR/chrome-gpu-errors.log" | cut -c1-120)"
    $S stop
    finish webgpu
}

# Window hints of the KWin script: A covered by two windows that each cover
# half of it must be hidden (the union counts); uncovered again, visible.
occlusion_test() {
    local S="$HERE/nvt-session.sh" mA mB mC H=/proc/driver/nvidia/vidmem_hints st
    begin occlusion
    $S stop >/dev/null 2>&1 || true
    $S start "file://$HERE/nvt-webgl.html?mode=idle&tex=8" >>"$DIR/notes.txt" 2>&1 || { fail "session"; finish occlusion; return 1; }
    NVT_INSTANCE=b $S start "file://$HERE/nvt-webgl.html?mode=idle&tex=8" >>"$DIR/notes.txt" 2>&1
    NVT_INSTANCE=c $S start "file://$HERE/nvt-webgl.html?mode=idle&tex=8" >>"$DIR/notes.txt" 2>&1
    sleep 5
    mA=$(NVT_INSTANCE=a $S main-pid); mB=$(NVT_INSTANCE=b $S main-pid); mC=$(NVT_INSTANCE=c $S main-pid)
    $S win geom "$mA" 100 100 800 600
    $S win geom "$mB" 80 80 440 640
    $S win geom "$mC" 500 80 420 640
    $S win activate "$mB"; $S win activate "$mC"
    sleep 1.5
    st=$(awk -v p="$mA" '$1 == p {print $2}' "$H")
    note "A under B (left half) and C (right half): $st; hints: $(grep -v '^#' "$H" | tr '\n' ' ')"
    [[ $st == h ]] || fail "A covered by two windows together is '$st', not hidden"
    $S win geom "$mC" 1000 80 420 640
    sleep 1.5
    st=$(awk -v p="$mA" '$1 == p {print $2}' "$H")
    note "C moved away: A $st"
    [[ $st == v ]] || fail "A half uncovered is '$st', not visible"
    $S stop
    finish occlusion
}

# Every stop method (evict.sh quiesce 0..4) against the active scenario.
quiesce_matrix() {
    local cycles=${1:-2} m
    for m in ${NVTEST_MODES:-0 1 2 3 4}; do
        $EVICT quiesce "$m" >/dev/null
        active "$cycles" "active-q$m" || true
    done
    $EVICT quiesce 0 >/dev/null
    grep '^NVTEST active-q' "$LOGROOT/summary.txt"
}

game() {
    local secs=${1:-60}
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST game SKIP somebody is logged in on seat0 (NVTEST_FORCE=1 overrides)" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin game
    "$HERE/nvt-session.sh" stop >/dev/null 2>&1 || true
    local idle1 idle2 bg g free want H=/proc/driver/nvidia/vidmem_hints
    start nvt-idle-a --mb 1024 --idle --verify-s 23 || { finish game; return 1; }; idle1=$LAST
    start nvt-idle-b --mb 768 --idle --verify-s 23 || { finish game; return 1; }; idle2=$LAST
    start nvt-bg-a --mb 256 --fps 30 --read-mb 64 --gfx --write || { finish game; return 1; }; bg=$LAST
    # Window hints as KWin would write them: this shell (so the game, its
    # child) in the foreground, the idle apps hidden, the drawing one visible.
    printf '%s f\n%s h\n%s h\n%s v\n' $$ "$idle1" "$idle2" "$bg" >"$H"
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    # The game wants more than is free: only the hidden apps' memory makes it fit.
    want=$((free + 1500))
    note "free outside reserve before the game: $free MB, game wants $want MB"

    $EVICT auto off >/dev/null
    start nvt-game --mb "$want" --read-mb 2048 --duration "$secs" --floor-mb 64 || { finish game; return 1; }; g=$LAST
    wait "$g" 2>/dev/null || true
    cp "$DIR/nvt-game.log" "$DIR/nvt-game-auto-off.log"
    grep '^START\|^RESULT' "$DIR/nvt-game-auto-off.log" | sed 's/^/  auto off: /' | tee -a "$DIR/notes.txt"

    $EVICT auto hints >/dev/null
    start nvt-game --mb "$want" --read-mb 2048 --duration "$secs" --floor-mb 64 || { finish game; return 1; }; g=$LAST
    wait "$g" 2>/dev/null || true
    grep '^START\|^RESULT' "$DIR/nvt-game.log" | sed 's/^/  auto on:  /' | tee -a "$DIR/notes.txt"
    $EVICT status >"$DIR/status-auto.txt" 2>&1 || true

    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: (hints|auto)" >"$DIR/policy.log" || true
    grep -q "of nvt-bg-a " "$DIR/policy.log" && fail "visible app nvt-bg-a was evicted"
    grep -q "of nvt-game " "$DIR/policy.log" && fail "the foreground game was evicted"
    grep -qE "of nvt-idle-" "$DIR/policy.log" || fail "no hidden app was evicted for the game"
    local got_off got_on
    got_off=$(sed -n 's/^START .*allocated_mb=\([0-9]*\).*/\1/p' "$DIR/nvt-game-auto-off.log")
    got_on=$(sed -n 's/^START .*allocated_mb=\([0-9]*\).*/\1/p' "$DIR/nvt-game.log")
    note "game got $got_off MB without eviction, $got_on MB with it"
    [[ ${got_on:-0} -gt $(( ${got_off:-0} + 1024 )) ]] || fail "the game did not get the hidden apps' memory"
    : >"$H"
    finish game
}

mkdir -p "$LOGROOT"
# Partial restore of the foreground (problem #10, CoD 2026-11-06: 1 GB of the
# focused game waited in sysmem for 8 minutes, all-or-nothing). The game's
# memory is moved out while only part of it fits again: that part must come
# back within seconds, in pieces, the rest as soon as there is room, and the
# game must read back exactly what it wrote.
fgback_test() {
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST fgback SKIP somebody is logged in on seat0 (NVTEST_FORCE=1 overrides)" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin fgback
    "$HERE/nvt-session.sh" stop >/dev/null 2>&1 || true
    local H=/proc/driver/nvidia/vidmem_hints g hog free gmb=2048 r t0 i got=0
    start nvt-game --mb "$gmb" --read-mb 256 --write --fps 60 --duration 120 --floor-mb 64 || { finish fgback; return 1; }
    g=$LAST
    # This shell, and so the game (its child), in the foreground.
    printf '%s f\n' $$ >"$H"
    sleep 4
    $EVICT auto off >/dev/null
    $EVICT evict "$g" >/dev/null
    note "game moved out: vram $(vram_of "$g") ram $(ram_of "$g") MB"
    # Meanwhile memory that cannot move takes all but ~1 GB of what is free
    # (as the desktop's 1.4 GB next to CoD): a process named like the login
    # screen, which the policy never moves out.
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    start sddm-greeter --mb $(( free - 1024 )) --idle --floor-mb 64 || { $EVICT auto hints >/dev/null; finish fgback; return 1; }
    hog=$LAST
    printf '%s f\n%s h\n' $$ "$hog" >"$H"
    note "free was $free MB, the hog holds $(vram_of "$hog") MB"
    t0=$(date +%s.%N)
    $EVICT auto hints >/dev/null
    sleep 6
    r=$(ram_of "$g")
    note "6 s later: game vram $(vram_of "$g") ram ${r:-?} MB (free $($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p') MB)"
    [[ -n $r && $r -le $(( gmb - 512 )) ]] || fail "less than 512 MB of the game came back within 6 s (ram $r MB)"
    # Room again: the rest must follow.
    kill "$hog" 2>/dev/null; wait "$hog" 2>/dev/null
    t0=$(date +%s.%N)
    r=$(wait_ram "$g" "$t0")
    note "hog gone: game fully back after $r s"
    [[ $r != timeout ]] || fail "the game's memory did not all come back within 10 s of room appearing"
    kill -INT "$g" 2>/dev/null; wait "$g" 2>/dev/null
    grep '^RESULT\|^ERROR' "$DIR/nvt-game.log" | sed 's/^/  /' | tee -a "$DIR/notes.txt"
    grep -q '^RESULT.* errors=0' "$DIR/nvt-game.log" || fail "the game read back wrong data (or did not finish)"
    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: (hints|restore pid)" >"$DIR/policy.log" || true
    grep -E "restoring nvt-game .*foreground, [0-9]+ of [0-9]+ MB" "$DIR/policy.log" | head -3 | sed 's/^/  /' | tee -a "$DIR/notes.txt"
    while read -r a b; do [[ $a -lt $b ]] && got=1; done < <(sed -n 's/.*restoring nvt-game .*foreground, \([0-9]*\) of \([0-9]*\) MB.*/\1 \2/p' "$DIR/policy.log")
    [[ $got == 1 ]] || fail "no partial restore of the game seen in the policy log"
    : >"$H"
    finish fgback
}

# The focused window comes first (rule R4, #7): a fullscreen "game" Chrome
# B on monitor 1 fills vidmem; the user clicks Chrome A on monitor 2. A is
# the foreground now and B only visible (the KWin script used to keep the
# fullscreen game the foreground); a WebGPU page A opens then gets its memory
# (the game on the other monitor gives what it needs, no more: next to CoD
# such a page was denied and its device lost). Back on B, B is whole again.
focus_test() {
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST focus SKIP somebody is logged in on seat0" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin focus
    local S="$HERE/nvt-session.sh" H=/proc/driver/nvidia/vidmem_hints t mA mB gA gB free gameMB i r
    $S stop >/dev/null 2>&1 || true
    NVT_OUTPUTS=2 $S start "file://$HERE/nvt-webgl.html?mode=idle&every=60&tex=20" >>"$DIR/notes.txt" 2>&1 ||
        { fail "session A"; finish focus; return 1; }
    for i in $(seq 1 60); do t=$(title_of a); [[ $t == *"checks="[1-9]* ]] && break; sleep 0.5; done
    mA=$(NVT_INSTANCE=a $S main-pid); gA=$(NVT_INSTANCE=a $S gpu-pid)
    $S win screen2 "$mA"
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    gameMB=$(( free - 64 ))
    NVT_INSTANCE=b $S start "file://$HERE/nvt-webgl.html?mode=game&mb=$gameMB" >>"$DIR/notes.txt" 2>&1 ||
        { fail "session B"; $S stop; finish focus; return 1; }
    sleep 2
    mB=$(NVT_INSTANCE=b $S main-pid)
    $S win screen1 "$mB"
    sleep 0.5
    $S win fullscreen "$mB"
    for i in $(seq 1 240); do t=$(title_of b); [[ $t == *" done=1 "* ]] && break; sleep 0.5; done
    gB=$(NVT_INSTANCE=b $S gpu-pid)
    note "B: browser $mB gpu $gB, wants $gameMB MB, $t"
    note "hints with B focused: $(tr '\n' ' ' < "$H")"
    grep -q "^$mB f g" "$H" || fail "the fullscreen B is not 'f g' in the hints"

    $S win activate "$mA"
    sleep 1.5
    note "hints after clicking A: $(tr '\n' ' ' < "$H")"
    grep -q "^$mA f" "$H" || fail "A clicked on monitor 2 is not the foreground (R4)"
    grep -q "^$mB v g" "$H" || fail "B is not 'v g' while A is focused (R4)"

    # A opens a WebGPU page that needs 256 MB more than is free.
    curl -s -X PUT "http://127.0.0.1:9333/json/new?file://$HERE/nvt-webgpu.html?mb=256" >/dev/null
    r=
    for i in $(seq 1 80); do
        r=$(NVT_INSTANCE=a $S title 2>/dev/null | grep '^WGPU' | head -1 || true)
        # Not *rror*: every title has "errors=0" (the wait used to end at once).
        [[ $r == *"checks="[1-9]* || $r == "WGPU error"* || $r == *" failed="* || $r == *" lost=1"* ]] && break
        sleep 0.5
    done
    grep -E "gpu_device.cc|GPU process|context lost" /tmp/nvt-chrome/chrome-a.log 2>/dev/null | head -3 | cut -c1-200 |
        sed 's/^/  chrome A: /' | tee -a "$DIR/notes.txt" || true
    note "WebGPU page in A: ${r:-no answer}; B vram $(vram_of "$gB") ram $(ram_of "$gB") MB"
    [[ $r == *"checks="[1-9]* && $r == *" errors=0 "* && $r == *" lost=0 "* ]] ||
        fail "the focused WebGPU page did not get its memory (#7): ${r:-no answer}"
    r=$(ram_of "$gB")
    [[ -n $r && $r -le 640 ]] || fail "B gave more than the page needed: ${r:-?} MB moved out"

    $S win activate "$mB"
    r=$(wait_ram "$gB") || true
    note "B clicked again: whole after $r s (ram $(ram_of "$gB") MB)"
    # A's page may hold memory that cannot move: B gets back all but a little.
    [[ $r != timeout || $(ram_of "$gB") -le 64 ]] || fail "B not back within 10 s"
    t=$(title_of b)
    note "final b: $t"
    [[ $t == *" bad=0 "* && $t == *" lost=0 "* ]] || fail "B output changed or context lost: $t"
    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: hints" | cut -c 40- >"$DIR/policy.log" || true
    $S stop
    finish focus
}

# GPU mappings made and dropped while the driver moves the memory (problem
# #4: "can't update VA space", each time 1-6 s after an app was moved out
# and back at once). A Chrome page that deletes and remakes a texture every
# frame (same picture) is moved out and back every 0.3-1 s for a minute;
# the picture must stay the same and the kernel log clean.
vaflip_test() {
    local S="$HERE/nvt-session.sh" t g i r
    begin vaflip
    $S stop >/dev/null 2>&1 || true
    $S start "file://$HERE/nvt-webgl.html?tex=24&churn=1" >>"$DIR/notes.txt" 2>&1 || { fail "session"; finish vaflip; return 1; }
    for i in $(seq 1 60); do t=$(title_of a); [[ $t == *"checks="[1-9]* ]] && break; sleep 0.5; done
    g=$($S gpu-pid)
    note "gpu $g, $t"
    for i in $(seq 1 60); do
        r=$(evict_op evict "$g" "flip $i")
        sleep 0.$(( RANDOM % 7 + 3 ))
        r="$r / $(evict_op restore "$g" "flip $i")"
        sleep 0.$(( RANDOM % 7 + 3 ))
        (( i % 10 == 0 )) && note "flip $i: $r | $(title_of a)"
    done
    sleep 3
    t=$(title_of a)
    note "final: $t"
    [[ $t == *" bad=0 "* && $t == *" lost=0 "* ]] || fail "picture changed or context lost: $t"
    [[ $t == *churned=[1-9]* ]] || fail "the page did not churn its textures: $t"
    journalctl -k --since "$T0" --no-pager | grep -E "can.t update VA|NV_ERR" >"$DIR/va.log" &&
        fail "VA errors: $(head -1 "$DIR/va.log" | cut -c1-160)"
    $S stop
    finish vaflip
}

# The compositor's buffers with vidmem full (problem #12: for kwin's 32 MB the
# driver moved 284 MB of the focused game in 531 ms). A visible game drawing
# and a hidden idle app fill vidmem; a process named kwin_wayland (the
# compositor by name: exempt, room made for it) then allocates 32 MB buffers
# beyond what is free: the hidden app gives, about what each buffer needs,
# and the drawing game keeps its memory. (GBM scanout buffers go through
# nvkms, outside this path: idea I12.)
comproom_test() {
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST comproom SKIP somebody is logged in on seat0" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin comproom
    "$HERE/nvt-session.sh" stop >/dev/null 2>&1 || true
    local H=/proc/driver/nvidia/vidmem_hints g idle comp free moved n
    # The hidden app in 32 MB buffers (kwin's size: whole buffers move).
    start nvt-idle-a --mb 1024 --chunk-mb 32 --idle || { finish comproom; return 1; }; idle=$LAST
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    start nvt-game --mb $(( free - 64 )) --read-mb 256 --fps 60 --duration 120 --floor-mb 32 || { finish comproom; return 1; }
    g=$LAST
    # This shell and so the game: visible (on the other monitor, drawing); the app hidden.
    printf '%s v\n%s h\n' $$ "$idle" >"$H"
    sleep 12     # the idle app idle for over 10 s
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    note "game vram $(vram_of "$g") MB, hidden app vram $(vram_of "$idle") MB, free $free MB"
    start kwin_wayland --mb $(( free + 256 )) --chunk-mb 32 --idle --floor-mb 0 || true
    comp=$LAST
    grep '^START' "$DIR/kwin_wayland.log" | sed 's/^/  compositor: /' | tee -a "$DIR/notes.txt" || true
    journalctl -k --since "$T0" --no-pager | grep -E "vidmem evict: (hints: compositor|compositor:)" >"$DIR/policy.log" || true
    sed 's/^.*NVRM: vidmem evict: /  /' "$DIR/policy.log" | head -10 | tee -a "$DIR/notes.txt" || true
    n=$(grep -c "compositor: room made" "$DIR/policy.log" || true)
    moved=$(sed -n 's/.*compositor: room made for .*, \([0-9]*\) MB moved in.*/\1/p' "$DIR/policy.log" | awk '{s+=$1} END {print s+0}')
    note "compositor make-rooms: $n, $moved MB moved; game ram $(ram_of "$g") MB, hidden app ram $(ram_of "$idle") MB"
    [[ $n -gt 0 ]] || fail "the compositor never needed room (test setup: vidmem not full?)"
    [[ $(ram_of "$g") -eq 0 ]] || fail "the drawing game was moved out for the compositor ($(ram_of "$g") MB) while a hidden app had memory"
    # Each 32 MB buffer: at most its own chunks, the 16 MB margin and one more
    # whole buffer of the victim (it was 284 MB of the game in CoD).
    local most
    most=$(sed -n 's/.*compositor: room made for .*, \([0-9]*\) MB moved in.*/\1/p' "$DIR/policy.log" | sort -n | tail -1)
    [[ ${most:-0} -le 96 ]] || fail "moved up to $most MB for one 32 MB buffer ($moved MB for $n)"
    grep -q '^START.*allocated_mb=' "$DIR/kwin_wayland.log" || fail "the compositor process did not get its buffers"
    kill "$comp" 2>/dev/null || true; wait "$comp" 2>/dev/null
    kill -INT "$g" 2>/dev/null || true; wait "$g" 2>/dev/null || true
    grep -q '^RESULT.* errors=0' "$DIR/nvt-game.log" || fail "the game read back wrong data"
    : >"$H"
    finish comproom
}

# New channels with vidmem nearly full (problem #13: xdg-desktop-portal-kde
# died when its channel's context buffers found no room; #3: a failed channel
# group stayed allocated on GSP and every later process failed until a
# reboot). Memory that cannot move (a process named like the login screen)
# leaves ~24 MB free next to a hidden idle app: a new process must still get
# its channel (room made from the hidden app). With nothing left to move a
# new process may fail, but once memory is free again the next one must work.
ctxroom_test() {
    if seat0_busy && [[ ${NVTEST_FORCE:-0} != 1 ]]; then
        echo "NVTEST ctxroom SKIP somebody is logged in on seat0" | tee -a "$LOGROOT/summary.txt"
        return 0
    fi
    begin ctxroom
    "$HERE/nvt-session.sh" stop >/dev/null 2>&1 || true
    local H=/proc/driver/nvidia/vidmem_hints idle hog hog2 p free
    start nvt-idle-a --mb 512 --chunk-mb 32 --idle || { finish ctxroom; return 1; }; idle=$LAST
    printf '%s h\n' "$idle" >"$H"
    sleep 12
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    # ~8 MB left: less than a new channel's context buffers (the hog, with no
    # window, takes it without making room). The next process starts at once,
    # before a policy step moves the hidden app out anyway; this shell, and so
    # that process, visible: its buffer gets room made too.
    start sddm-greeter --mb $(( free - 4 )) --chunk-mb 1024 --idle --floor-mb 0 || { finish ctxroom; return 1; }; hog=$LAST
    printf '%s v\n%s h\n' $$ "$idle" >"$H"
    start nvt-bg-a --mb 8 --fps 10 --floor-mb 0 || true
    p=$LAST
    note "free was $free MB, the never-moved hog took all but ~4 MB; hidden app vram $(vram_of "$idle") MB"
    sleep 3
    if grep -q '^START' "$DIR/nvt-bg-a.log"; then
        note "a new process got its channel: $(grep '^START' "$DIR/nvt-bg-a.log")"
    else
        fail "a new process could not start next to a hidden app it could take room from: $(tail -2 "$DIR/nvt-bg-a.log" | tr '\n' ' ')"
    fi
    journalctl -k --since "$T0" --no-pager | grep -E "channel contexts|ctxBufPool|Failed to reserve" | sed 's/^.*NVRM: //' | head -5 | sed 's/^/  /' | tee -a "$DIR/notes.txt" || true
    journalctl -k --since "$T0" --no-pager | grep -q "room made for .* of channel contexts" ||
        note "(the channel's context buffers fitted without making room this time)"
    kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true
    # Context buffers that do not fit while a hidden app still has memory
    # (#13): fill the rest with memory that cannot move, then a new process
    # with no window: its channel's contexts must get room from the hidden app.
    printf '%s h\n' "$idle" >"$H"
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    ln -sf nvt-game "$HERE/sddm-greeter-qt"
    "$HERE/sddm-greeter-qt" --mb $(( free - 4 )) --chunk-mb 1024 --idle --floor-mb 0 >"$DIR/hog1.log" 2>&1 &
    hog2=$!; PIDS+=("$hog2")
    for i in $(seq 1 100); do grep -q '^START\|^FATAL' "$DIR/hog1.log" 2>/dev/null && break; sleep 0.1; done
    note "filled to ~4 MB free; hidden app vram $(vram_of "$idle") MB"
    ln -sf nvt-game "$HERE/nvt-bg-d"
    "$HERE/nvt-bg-d" --mb 1 --chunk-mb 1 --idle --floor-mb 0 >"$DIR/ctx-try.log" 2>&1 &
    p=$!
    for i in $(seq 1 50); do grep -q '^START\|FATAL\|failed' "$DIR/ctx-try.log" 2>/dev/null && break; sleep 0.1; done
    note "new process, no window: $( (grep -E '^START|FATAL|failed' "$DIR/ctx-try.log" || echo '?') | head -1)"
    kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true
    journalctl -k --since "$T0" --no-pager | grep -E "of channel contexts" | sed 's/^.*NVRM: vidmem evict: /  /' | head -3 | tee -a "$DIR/notes.txt" || true
    if journalctl -k --since "$T0" --no-pager | grep -q "room made for .* of channel contexts"; then
        grep -q '^START' "$DIR/ctx-try.log" || fail "room was made for the contexts but the process did not start"
    else
        grep -q '^START' "$DIR/ctx-try.log" || fail "a new channel failed next to a hidden app with memory (#13): $(head -1 "$DIR/ctx-try.log")"
    fi
    kill "$hog2" 2>/dev/null || true; wait "$hog2" 2>/dev/null || true

    # Nothing left to move: fill the rest, a new process may fail...
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    $EVICT evict "$idle" >/dev/null 2>&1 || true
    free=$($EVICT status | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p')
    ln -sf nvt-game "$HERE/sddm-greeter-qt"
    "$HERE/sddm-greeter-qt" --mb $(( free - 4 )) --idle --floor-mb 0 >"$DIR/hog2.log" 2>&1 &
    hog2=$!; PIDS+=("$hog2")
    for i in $(seq 1 100); do grep -q '^START\|^FATAL' "$DIR/hog2.log" 2>/dev/null && break; sleep 0.1; done
    # (not start(): its failure here is expected, start() would count it)
    ln -sf nvt-game "$HERE/nvt-bg-b"
    "$HERE/nvt-bg-b" --mb 8 --fps 10 --floor-mb 0 >"$DIR/full-try.log" 2>&1 &
    p=$!
    sleep 3
    note "with vidmem full: $( (grep -E '^START|FATAL|failed' "$DIR/full-try.log" || echo '?') | head -1)"
    kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true
    journalctl -k --since "$T0" --no-pager | grep -q "ctxBufPoolReserve.*Failed\|Failed to reserve memory" &&
        note "a channel group failed to reserve its contexts on the way (the #3 path)" || true
    # ...but once memory is free again, the next one must work (no group left on GSP).
    kill "$hog2" 2>/dev/null || true; wait "$hog2" 2>/dev/null || true
    sleep 1
    start nvt-bg-c --mb 8 --fps 10 --floor-mb 0 || true
    sleep 3
    grep -q '^START' "$DIR/nvt-bg-c.log" || fail "no new channel after memory came free (a leaked group on GSP?): $(tail -2 "$DIR/nvt-bg-c.log" | tr '\n' ' ')"
    note "after memory came free: $(grep -E '^START|^FATAL' "$DIR/nvt-bg-c.log" | head -1)"
    : >"$H"
    finish ctxroom
}

case ${1:-} in
    integrity) integrity "${2:-}" ;;
    active) active "${2:-}" ;;
    quiesce) quiesce_matrix "${2:-}" ;;
    chrome) chrome "${2:-}" "${3:-}" ;;
    switch) switch_test "${2:-}" ;;
    fgprio) fgprio_test "${2:-}" "${3:-}" ;;
    webgpu) webgpu_test "${2:-}" ;;
    occlusion) occlusion_test ;;
    game) game "${2:-}" ;;
    fgback) fgback_test ;;
    focus) focus_test ;;
    vaflip) vaflip_test ;;
    comproom) comproom_test ;;
    ctxroom) ctxroom_test ;;
    all)
        rc=0
        integrity || rc=1
        active || rc=1
        chrome 5 anim || rc=1
        chrome 3 idle || rc=1
        webgpu_test 5 || rc=1
        occlusion_test || rc=1
        switch_test 5 || rc=1
        fgprio_test 3 idle || rc=1
        fgprio_test 2 anim || rc=1
        game 30 || rc=1
        fgback_test || rc=1
        focus_test || rc=1
        vaflip_test || rc=1
        comproom_test || rc=1
        ctxroom_test || rc=1
        echo "== summary"
        cat "$LOGROOT/summary.txt"
        exit $rc ;;
    *) sed -n '2,25p' "$0"; exit 2 ;;
esac
