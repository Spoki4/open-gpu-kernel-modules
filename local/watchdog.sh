#!/bin/bash
# Runs a long command (bench, trace, build) and stops it when it makes no
# progress, instead of waiting for it forever. Progress = a new output line
# or a file changed under one of the --watch paths (the bench writes its notes
# and dmon logs there). On a stall or the time limit it prints why, the last
# log lines and the process tree, kills the command's whole process group,
# runs the --on-kill cleanup and exits 124.
#
#   ./watchdog.sh [--stall SEC] [--max SEC] [--watch PATH]... [--ignore NAME]... [--on-kill CMD] -- CMD...
#     --stall SEC   no progress for this long = hung (default 300)
#     --max SEC     total time limit (default: none)
#     --watch PATH  directory or file whose changes count as progress
#     --ignore NAME file name (glob) under the watched paths that does not count:
#                   samplers that append every second while the test is stuck
#                   (nvtest's dmon.log)
#     --on-kill CMD shell command run after a kill (e.g. ./nvt-session.sh stop)
#
# Every output line of CMD is passed through; the watchdog's own lines start
# with "WATCHDOG", so a monitor can grep for them:
#   WATCHDOG STALL no progress for 300 s ...
#   WATCHDOG TIMEOUT after 1800 s ...
#   WATCHDOG EXIT rc=N after S s
set -uo pipefail

stall=300 max=0 on_kill=
watch=()
ignore=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --stall) stall=$2; shift 2 ;;
        --max) max=$2; shift 2 ;;
        --watch) watch+=("$2"); shift 2 ;;
        --ignore) ignore+=(! -name "$2"); shift 2 ;;
        --on-kill) on_kill=$2; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
[[ $# -gt 0 ]] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

out=$(mktemp -t watchdog.XXXXXX)
trap 'rm -f "$out"' EXIT

# Newest mtime (s) under the watched paths, 0 if none.
newest() {
    local p t m=0
    for p in "${watch[@]}"; do
        [[ -e $p ]] || continue
        t=$(find "$p" -type f "${ignore[@]}" -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
        t=${t%.*}
        [[ -n $t && $t -gt $m ]] && m=$t
    done
    echo "$m"
}

newest_file() {
    local p
    for p in "${watch[@]}"; do
        [[ -e $p ]] && find "$p" -type f "${ignore[@]}" -printf '%T@ %p\n' 2>/dev/null
    done | sort -n | tail -1 | cut -d' ' -f2-
}

# Own session, so the whole tree (nvtest -> nvt-session -> chrome, kwin) dies together.
setsid "$@" > >(tee -a "$out") 2>&1 &
pid=$!
start=$(date +%s)
last_progress=$start
last_size=0
last_mtime=$(newest)

kill_all() {
    local why=$1 f
    echo "WATCHDOG $why"
    f=$(newest_file)
    if [[ -n $f ]]; then
        echo "WATCHDOG last lines of $f:"
        tail -n 8 "$f" | sed 's/^/WATCHDOG   /'
    fi
    echo "WATCHDOG last output:"
    tail -n 5 "$out" | sed 's/^/WATCHDOG   /'
    echo "WATCHDOG processes:"
    ps -o pid,etimes,stat,args --forest -s "$(ps -o sid= -p "$pid" 2>/dev/null | tr -d ' ')" 2>/dev/null |
        cut -c1-160 | sed 's/^/WATCHDOG   /'
    kill -TERM -- "-$pid" 2>/dev/null
    sleep 5
    kill -KILL -- "-$pid" 2>/dev/null
    if [[ -n $on_kill ]]; then
        echo "WATCHDOG cleanup: $on_kill"
        bash -c "$on_kill" 2>&1 | sed 's/^/WATCHDOG   /'
    fi
    wait "$pid" 2>/dev/null
    exit 124
}

while kill -0 "$pid" 2>/dev/null; do
    sleep 5
    now=$(date +%s)
    size=$(stat -c %s "$out")
    mt=$(newest)
    if [[ $size != "$last_size" || $mt != "$last_mtime" ]]; then
        last_size=$size last_mtime=$mt last_progress=$now
    fi
    if (( now - last_progress >= stall )); then
        kill_all "STALL no progress for $((now - last_progress)) s (limit $stall s), running $((now - start)) s: $*"
    fi
    if (( max > 0 && now - start >= max )); then
        kill_all "TIMEOUT after $((now - start)) s (limit $max s): $*"
    fi
done
wait "$pid"
rc=$?
echo "WATCHDOG EXIT rc=$rc after $(( $(date +%s) - start )) s"
exit "$rc"
