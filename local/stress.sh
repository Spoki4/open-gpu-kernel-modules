#!/bin/bash
# Stage 0 stress tests: runs VRAM hogs under a watchdog and reports whether the
# vidmem user reserve held and whether the compositor survived. Run on the PC
# as the desktop user (works from SSH as well); census needs the sudoers rule.
#
#   ./stress.sh race              # parallel fills: does the check/alloc race break the reserve?
#   ./stress.sh gbm               # GBM render/scanout buffers, dumb buffers: bound by the reserve?
#   ./stress.sh scanout           # with vidmem filled to the reserve, can the compositor still
#                                 # get contiguous scanout buffers? (needs vidmem_pma, 2026-11-01)
#   ./stress.sh game              # wait for GAME (default sp26-cod.exe), log its census
#                                 # (incl. sysmem) and vidmem_pma every 10 s until it exits
#   ./stress.sh run NAME CMD...   # any command under the same watchdog
#
# Logs: local/stress-logs/<date_time>/<test>/, summary printed per test.
# Watchdog: when free vidmem (PMA) drops below KILL_MB (default 100), every
# vram-hog and gbm-hog is killed at once. REPEAT (default 3) repeats race runs.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
KILL_MB=${KILL_MB:-100}
REPEAT=${REPEAT:-3}
PROBE=${PROBE:-1}   # 0: no screenshot probe (the game scenario sets this)
# Kernel log lines that fail a test. The gbm scenario drops the nvkms
# allocation failures from it: gbm-hog provokes those on purpose.
BAD_K_RE_DEFAULT='Failed to allocate NVKMS memory|Failed to allocate NvKmsKapiMemory|pmaAllocatePages|ctxBufPoolReserve|Xid'
BAD_K_RE=${BAD_K_RE:-$BAD_K_RE_DEFAULT}
LOGROOT=${LOGROOT:-$HERE/stress-logs/$(date +%F_%H%M%S)}

params=$(cat /proc/driver/nvidia/params)
RESERVE_MB=$(sed -n 's/.*RmVidmemUserReserveMB=\([0-9]*\).*/\1/p' <<<"$params")
[[ ${RESERVE_MB:-0} -gt 0 ]] || { echo "stress.sh: RmVidmemUserReserveMB is not set" >&2; exit 1; }
for t in vram-hog gbm-hog; do
    [[ -x $HERE/$t ]] || { echo "stress.sh: $t missing, run ./build.sh tools" >&2; exit 1; }
done

# The desktop session's environment, so spectacle and journalctl --user work over SSH.
uid=$(id -u)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$uid}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}

PMA_FILE=$(ls /proc/driver/nvidia/gpus/*/vidmem_pma 2>/dev/null | head -1 || true)
# "<free> <largest contiguous free> <reserve block> <block free> <block largest free>" (MB)
pma() { [[ -n $PMA_FILE ]] && cat "$PMA_FILE" || echo "- -"; }

free_mb() { # real free vidmem (PMA), NVML as a fallback
    if [[ -n $PMA_FILE ]]; then read -r f _ <"$PMA_FILE"; echo "$f"
    else nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | tr -d ' '; fi
}

# --- background helpers (each runs in its own process group) ---------------

sampler() { # $1 = test dir. Free memory every 200 ms, plus the kill switch.
    # PMA's own number when the module has vidmem_pma: NVML reports the free
    # memory minus the reserve to ordinary processes.
    local f pf
    pf=$(ls /proc/driver/nvidia/gpus/*/vidmem_pma 2>/dev/null | head -1 || true)
    while :; do
        if [[ -n $pf ]]; then
            read -r f _ <"$pf" || f=
        else
            f=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | tr -d ' ')
        fi
        if [[ $f =~ ^[0-9]+$ ]]; then
            echo "$(date +%T.%3N) $f"
            if (( f < KILL_MB )); then
                echo "$(date +%T.%3N) KILLSWITCH: free $f MB < $KILL_MB MB" >>"$1/killswitch.log"
                pkill -KILL -x vram-hog || true
                pkill -KILL -x gbm-hog || true
            fi
        fi
        sleep 0.2
    done
}

pma_sampler() { # $1 = pma file. Free and largest contiguous free block every 200 ms.
    while :; do
        echo "$(date +%T.%3N) $(cat "$1")"
        sleep 0.2
    done
}

probe() { # $1 = test dir. Every 3 s kwin has to render a screenshot (and allocate for it).
    local t0 t1 st
    while :; do
        t0=$(date +%s%N)
        if QT_QUICK_BACKEND=software timeout --foreground 10 spectacle -b -n -f -o "$1/shot.png" >/dev/null 2>&1; then
            st=ok
        else
            st=FAIL
        fi
        t1=$(date +%s%N)
        echo "$(date +%T) $st $(( (t1 - t0) / 1000000 ))ms"
        sleep 3
    done
}

BG=()
start_bg() { # $1 = log file, rest = command string
    local log=$1; shift
    setsid bash -c "$(declare -f sampler probe pma_sampler); KILL_MB=$KILL_MB; $*" >"$log" 2>&1 &
    BG+=($!)
}
stop_bg() {
    local p
    for p in "${BG[@]}"; do kill -- -"$p" 2>/dev/null || true; done
    BG=()
}
cleanup() {
    stop_bg
    pkill -KILL -x vram-hog || true
    pkill -KILL -x gbm-hog || true
}
trap cleanup EXIT INT TERM

census_when_holding() { # $1 = test dir, $2 = pid of the test command
    local p
    while kill -0 "$2" 2>/dev/null; do
        if grep -q holding "$1/cmd.log" 2>/dev/null; then
            sleep 1
            for p in $(pgrep -x vram-hog; pgrep -x gbm-hog); do
                sudo -n "$HERE/census.sh" "$p" --summary || true
            done >"$1/census.txt" 2>&1
            return
        fi
        sleep 0.5
    done
}

# --- one test under the watchdog --------------------------------------------

watch_run() { # $1 = test name, rest = command
    local name=$1; shift
    local dir=$LOGROOT/$name
    local since kwin0 plasma0 free0 free1 rc cmdpid
    mkdir -p "$dir"
    since=$(date '+%F %T')
    kwin0=$(pgrep -x -u "$uid" kwin_wayland | head -1 || true)
    plasma0=$(pgrep -x plasmashell || true)
    free0=$(free_mb)
    echo "== $name: free $free0 MB, reserve $RESERVE_MB MB, kill switch at $KILL_MB MB"
    echo "   $*"

    start_bg "$dir/free.log" sampler "$dir"
    if [[ $PROBE == 1 ]]; then start_bg "$dir/probe.log" probe "$dir"; else : >"$dir/probe.log"; fi
    [[ -z $PMA_FILE ]] || start_bg "$dir/pma.log" pma_sampler "$PMA_FILE"
    start_bg "$dir/kernel.log" journalctl -k -f -o short-precise --since "'$since'"
    start_bg "$dir/user.log" journalctl --user -f -o short-precise --since "'$since'"
    sleep 1

    "$@" >"$dir/cmd.log" 2>&1 &
    cmdpid=$!
    census_when_holding "$dir" "$cmdpid" &
    rc=0
    wait "$cmdpid" || rc=$?
    wait $! 2>/dev/null || true
    sleep 3
    free1=$(free_mb)
    stop_bg

    summarize "$name" "$dir" "$rc" "$free0" "$free1" "$kwin0" "$plasma0" | tee "$dir/summary.txt"
}

summarize() {
    local name=$1 dir=$2 rc=$3 free0=$4 free1=$5 kwin0=$6 plasma0=$7
    local minfree kills pfail pmax denials dmin badk badu nvkmsf fallb kwin1 plasma1 verdict=PASS why=()

    minfree=$(awk '$2 ~ /^[0-9]+$/ && (m == "" || $2 < m) {m = $2} END {print m}' "$dir/free.log")
    kills=0
    [[ ! -f $dir/killswitch.log ]] || kills=$(wc -l <"$dir/killswitch.log")
    pfail=$(grep -c ' FAIL ' "$dir/probe.log" || true)
    pmax=$(awk '{v = $3; sub("ms", "", v); if (v + 0 > m) m = v + 0} END {print m + 0}' "$dir/probe.log")
    denials=$(grep -c 'vidmem user reserve: denied' "$dir/kernel.log" || true)
    dmin=$(sed -n 's/.*vidmem user reserve: denied.*, free \([0-9]*\) MB.*/\1/p' "$dir/kernel.log" |
           awk 'm == "" || $1 < m {m = $1} END {print m}')
    badk=$(grep -cE "$BAD_K_RE" "$dir/kernel.log" || true)
    nvkmsf=$(grep -cE 'Failed to allocate (NVKMS memory|NvKmsKapiMemory)' "$dir/kernel.log" || true)
    fallb=$(grep -c 'trying to fall back to sysmem' "$dir/kernel.log" || true)
    badu=$(grep -i 'kwin' "$dir/user.log" | grep -ciE 'fail|error|gbm|egl' || true)
    local lmin=- bmin=- blmin=-
    if [[ -s $dir/pma.log ]]; then
        lmin=$(awk '$3 ~ /^[0-9]+$/ && (m == "" || $3 < m) {m = $3} END {print m}' "$dir/pma.log")
        bmin=$(awk '$5 ~ /^[0-9]+$/ && (m == "" || $5 < m) {m = $5} END {print (m == "" ? "-" : m)}' "$dir/pma.log")
        blmin=$(awk '$6 ~ /^[0-9]+$/ && (m == "" || $6 < m) {m = $6} END {print (m == "" ? "-" : m)}' "$dir/pma.log")
    fi
    kwin1=$(pgrep -x -u "$uid" kwin_wayland | head -1 || true)
    plasma1=$(pgrep -x plasmashell || true)

    (( rc == 0 )) || why+=("command exit $rc")
    (( kills == 0 )) || why+=("KILL SWITCH fired")
    (( pfail == 0 )) || why+=("$pfail screenshot probes failed")
    (( badk == 0 )) || why+=("$badk bad kernel lines")
    (( badu == 0 )) || why+=("$badu kwin error lines")
    [[ $kwin0 == "$kwin1" ]] || why+=("kwin pid changed")
    [[ $plasma0 == "$plasma1" ]] || why+=("plasmashell pid changed")
    (( ${#why[@]} == 0 )) || verdict=FAIL

    echo "-- $name: $verdict${why:+ (${why[*]})}"
    # Not a failure: other apps grow or shrink their VRAM while a test runs.
    (( free1 + 64 >= free0 )) || echo "   note: $((free0 - free1)) MB less free afterwards (apps re-allocating?)"
    echo "   free before/after: $free0/$free1 MB; min free (200 ms): ${minfree:-?} MB;" \
         "min largest contiguous free: $lmin MB"
    echo "   reserve block: min free $bmin MB, min largest contiguous free $blmin MB"
    echo "   kernel: $denials denial lines logged (first 8 per boot, then every 1024th)," \
         "lowest PMA free at a logged denial: ${dmin:--} MB; bad lines: $badk"
    echo "   nvkms GEM/dumb allocation failures: $nvkmsf; sysmem fallbacks: $fallb"
    echo "   screenshot probe: $pfail failed, slowest ${pmax} ms; kwin errors: $badu"
    grep '^RESULT\|^\[[0-9]*\] RESULT' "$dir/cmd.log" | sed 's/^/   /' || true
    if [[ -s $dir/census.txt ]]; then
        grep -E '^==|total|candidate|shared|sysmem' "$dir/census.txt" | sed 's/^/   census: /' || true
    fi
    if (( badk + badu > 0 )); then
        { grep -E "$BAD_K_RE" "$dir/kernel.log"
          grep -i 'kwin' "$dir/user.log" | grep -iE 'fail|error|gbm|egl'; } | head -5 | sed 's/^/   ! /' || true
    fi
}

# --- scenarios ---------------------------------------------------------------

# $1 = number of processes, rest = vram-hog arguments; all start filling at the same moment.
multi() {
    local n=$1; shift
    local at pids=() i rc=0
    at=$(awk -v t="$(date +%s.%N)" 'BEGIN {printf "%.3f", t + 2}')
    for i in $(seq 1 "$n"); do
        "$HERE/vram-hog" --start-at "$at" "$@" 2>&1 | sed -u "s/^/[$i] /" &
        pids+=($!)
    done
    for i in "${pids[@]}"; do wait "$i" || rc=1; done
    return $rc
}

race() {
    local i
    # Calibration: one thread, so the result shows where NVML "free" sits
    # relative to the reserve when nothing races.
    watch_run race-1x64 "$HERE/vram-hog" --threads 1 --chunk-mb 64 --hold 5
    for i in $(seq 1 "$REPEAT"); do
        watch_run "race-8x64-$i" "$HERE/vram-hog" --threads 8 --chunk-mb 64 --hold 5
        watch_run "race-16x32-$i" "$HERE/vram-hog" --threads 16 --chunk-mb 32 --hold 5
        watch_run "race-4procx2x64-$i" multi 4 --threads 2 --chunk-mb 64 --hold 5
    done
}

gbm() {
    local BAD_K_RE='pmaAllocatePages|ctxBufPoolReserve|Xid'
    watch_run gbm-render "$HERE/gbm-hog" --hold 10
    watch_run gbm-scanout "$HERE/gbm-hog" --scanout --hold 10
    watch_run gbm-dumb "$HERE/gbm-hog" --dumb --hold 10
}

# A copy of gbm-hog named kwin_wayland: the reserve exempts it by name exactly
# like the real compositor. Never pkill by this name.
KWIN_PROBE=
make_kwin_probe() {
    local d
    d=$(mktemp -d)
    cp "$HERE/gbm-hog" "$d/kwin_wayland"
    KWIN_PROBE=$d/kwin_wayland
}

# $1 = buffers to try, rest = vram-hog arguments (empty: no hog). Fills vidmem
# with vram-hog, then asks for scanout buffers as "kwin_wayland".
scanout_case() {
    local n=$1; shift
    local hl hog=
    if (( $# > 0 )); then
        hl=$(mktemp)
        "$HERE/vram-hog" "$@" >"$hl" 2>&1 &
        hog=$!
        while kill -0 "$hog" 2>/dev/null && ! grep -q holding "$hl"; do sleep 0.5; done
        grep -E 'RESULT|stopped|filled' "$hl"
    fi
    echo "pma before (free, largest contiguous MB): $(pma)"
    "$KWIN_PROBE" --scanout --max "$n" --floor-mb 64 --hold 2 | grep -vE '^  [0-9]+ buffers'
    echo "pma after: $(pma)"
    if [[ -n $hog ]]; then
        kill "$hog" 2>/dev/null || true
        wait "$hog" 2>/dev/null || true
        rm -f "$hl"
    fi
}

scanout() {
    local BAD_K_RE='pmaAllocatePages|ctxBufPoolReserve|Xid'
    [[ -n $PMA_FILE ]] || { echo "stress.sh: vidmem_pma missing (module too old)" >&2; exit 1; }
    make_kwin_probe
    watch_run scanout-idle scanout_case 6
    watch_run scanout-full scanout_case 6 --threads 1 --chunk-mb 64 --hold 120
    watch_run scanout-full-2m scanout_case 6 --threads 1 --chunk-mb 2 --hold 120
    watch_run scanout-churn scanout_case 6 --threads 4 --chunk-mb 64 --churn 30 --hold 120
    watch_run scanout-churn-8x16 scanout_case 6 --threads 8 --chunk-mb 16 --churn 60 --hold 120
    rm -rf "$(dirname "$KWIN_PROBE")"
}

# Waits for the game, then censuses it every 10 s while it runs.
game_watch() {
    local name=${GAME:-sp26-cod.exe} pid= t=0
    echo "waiting for $name"
    while [[ -z $pid ]]; do
        pid=$(pgrep -x "$name" | head -1 || true)
        [[ -n $pid ]] && break
        sleep 2
        (( (t += 2) < 3600 )) || { echo "no $name within an hour"; return 1; }
    done
    echo "game pid $pid since $(date +%T)"
    while kill -0 "$pid" 2>/dev/null; do
        echo "== $(date +%T) pma (free largest block block_free block_largest): $(pma)"
        sudo -n "$HERE/census.sh" "$pid" --summary 2>&1 |
            grep -E 'total|candidate|compressed|cpu-mapped|shared|sysmem' || true
        sleep 10
    done
    echo "game exited $(date +%T)"
}

game() {
    local PROBE=0
    local BAD_K_RE='pmaAllocatePages|ctxBufPoolReserve|Xid'
    watch_run game game_watch
}

case ${1:-} in
    game) game ;;
    race) race ;;
    gbm) gbm ;;
    scanout) scanout ;;
    run) shift; [[ $# -ge 2 ]] || { echo "usage: $0 run NAME CMD..." >&2; exit 2; }; watch_run "$@" ;;
    *) echo "usage: $0 race|gbm|scanout|game|run NAME CMD..." >&2; exit 2 ;;
esac
echo "logs: $LOGROOT"
