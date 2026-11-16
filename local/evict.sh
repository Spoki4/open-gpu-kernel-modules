#!/bin/bash
# Moves a process's vidmem to system memory and back, and controls the
# automatic policy (needs the patched module, see
# src/nvidia/src/kernel/mem_mgr/vidmem_evict.c).
#
#   sudo ./evict.sh status [--raw|--pretty]     # per process: vram, ram, evictable, idle; laid out
#                                               # by evict-status.py on a terminal, the driver's plain
#                                               # lines into a pipe (--raw / --pretty: either anywhere)
#   sudo ./evict.sh status --watch [SEC]        # the same, refreshed every SEC s (2, at least 1), with
#                                               # what changed since it began; q: quit, r: changes from
#                                               # now, another key: refresh now
#   sudo ./evict.sh dry     <pid|chrome|name>   # list what could move, and why not
#   sudo ./evict.sh move    <pid|chrome|name>   # move its vidmem to other vidmem pages (no sysmem; problem #16)
#   sudo ./evict.sh compact MB                  # background compaction keeps a free run of MB (0: off; problem #16)
#   sudo ./evict.sh evict   <pid|chrome|name>
#   sudo ./evict.sh restore <pid|chrome|name>
#   sudo ./evict.sh auto    on|off|hints        # automatic eviction under VRAM pressure
#                                               # (hints, the default: only while the KWin
#                                               # script reports window visibility)
#   sudo ./evict.sh shared  <pid|chrome|name>   # who else holds this process's shared vidmem
#   sudo ./evict.sh trace   on|off              # log every remap (VA ranges) to the kernel log
#   sudo ./evict.sh map                         # a snapshot of who holds which frames of vidmem into the
#                                               # trace (nvidia_vidmem:vidmem_ext; nvtrace.py map)
#   sudo ./evict.sh trace-lock US               # RM lock waits/holds >= US traced (tracepoint
#                                               # nvidia_vidmem:rm_lock, local/trace/nvtrace.py)
#   sudo ./evict.sh windows on|off              # also evict window buffers (default off)
#   sudo ./evict.sh moveshared on|off           # window buffers may move to other vidmem pages
#                                               # (compaction, cleared windows; default on)
#   sudo ./evict.sh fgown on|off                # a focused app that is not a game gets an allocation
#                                               # nobody can make room for; as much of its own older
#                                               # memory goes to sysmem (problem #7; default on)
#   sudo ./evict.sh compactfg on|off            # background compaction also moves the memory of the
#                                               # window in focus, 32 MB a step (default on)
#   sudo ./evict.sh compactbig on|off           # background compaction moves allocations of up to 256 MB
#                                               # (96 MB of the window in focus) instead of 32 MB: a
#                                               # game's blocks no longer stand in its way (default on)
#   sudo ./evict.sh placesides on|off           # while a game runs the others' new memory goes to the bottom of vidmem
#   sudo ./evict.sh gamegives on|off            # a focused app that is not a game may move a visible game's
#                                               # memory to sysmem (default off: only inside vidmem)
#   sudo ./evict.sh hot     on|off|momc         # access counters: evicted allocations in use
#                                               # come back one by one (experiment); momc: also
#                                               # count accesses to vidmem (a probe, tests/momc-probe.sh)
#   sudo ./evict.sh quiesce 0..4                # how channels are stopped (experiment, see
#                                               # _vidmemEvictQuiesce in vidmem_evict.c)
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "evict.sh: run with sudo" >&2; exit 1; }
usage() { echo "usage: sudo $0 status [--raw|--pretty|--watch [SEC]] | map | dry|evict|restore|move|shared <pid|chrome|name> | auto|trace|windows|moveshared|fgown|compactfg|compactbig|placesides|gamegives on|off | quiesce 0..4 | compact MB | trace-lock US" >&2; exit 2; }
[[ $# -ge 1 ]] || usage

mode=$1
case $mode in
    status)
        view=auto
        every=2
        case ${2-} in
            '') ;;
            --raw) view=raw ;;
            --pretty) view=pretty ;;
            --watch)
                view=watch
                if [[ $# -eq 3 ]]; then
                    # Each status takes the GPU lock and samples the GPU: not more often than once a second.
                    [[ $3 =~ ^[0-9]+([.][0-9]+)?$ ]] && awk -v s="$3" 'BEGIN { exit !(s >= 1) }' || usage
                    every=$3
                fi
                ;;
            *) usage ;;
        esac
        [[ $# -le 2 || $view == watch && $# -le 3 ]] || usage
        cmd="$mode"
        ;;
    map)
        [[ $# -eq 1 ]] || usage
        cmd="$mode"
        ;;
    hot)
        [[ $# -eq 2 ]] || usage
        case $2 in on|1) cmd="hot 1" ;; off|0) cmd="hot 0" ;; momc|2) cmd="hot 2" ;; *) usage ;; esac
        ;;
    auto|trace|windows|moveshared|fgown|compactfg|compactbig|placesides|gamegives)
        [[ $# -eq 2 ]] || usage
        case $2 in
            on|1) cmd="$mode 1" ;;
            off|0) cmd="$mode 0" ;;
            hints|2) [[ $mode == auto ]] || usage; cmd="auto 2" ;;
            *) usage ;;
        esac
        ;;
    quiesce)
        [[ $# -eq 2 && $2 =~ ^[0-4]$ ]] || usage
        cmd="quiesce $2"
        ;;
    compact)
        [[ $# -eq 2 && $2 =~ ^[0-9]+$ ]] || usage
        cmd="compact $2"
        ;;
    trace-lock)
        [[ $# -eq 2 && $2 =~ ^[0-9]+$ ]] || usage
        cmd="trace-lock $2"
        ;;
    dry|evict|restore|move|shared)
        [[ $# -eq 2 ]] || usage
        target=$2
        case $target in
            ''|*[!0-9]*)
                if [[ $target == chrome ]]; then
                    pid=$(pgrep -o -f -- '(/opt/google/chrome/chrome|/usr/lib/chromium/chromium) --type=gpu-process' || true)
                else
                    pid=$(pgrep -o -x -- "$target" || true)
                fi
                ;;
            *)
                pid=$target
                ;;
        esac
        [[ -n ${pid:-} ]] || { echo "evict.sh: no process for '$target'" >&2; exit 1; }
        cmd="$mode $pid"
        ;;
    *)
        usage
        ;;
esac

shopt -s nullglob
files=(/proc/driver/nvidia/gpus/*/vidmem_evict)
[[ ${#files[@]} -gt 0 ]] || { echo "evict.sh: vidmem_evict not found; is the patched module loaded?" >&2; exit 1; }

# One command at a time: the result buffer in the driver is shared, so a
# concurrent `watch evict.sh status` would overwrite this command's result.
exec 9>/run/nvidia-vidmem-evict.lock
flock 9

run() {
    local f status=0
    for f in "${files[@]}"; do
        [[ ${#files[@]} -gt 1 ]] && echo "== $f"
        echo "$cmd" > "$f" || status=$?
        cat "$f"
    done
    return $status
}

# The tests and gamerec read the plain lines: only a terminal (or --pretty) gets the layout.
view=${view:-raw}
[[ $view == auto ]] && { [[ -t 1 ]] && view=pretty || view=raw; }
pretty=$(dirname "$(readlink -f "$0")")/evict-status.py

if [[ $view == watch ]]; then
    [[ -t 1 && -t 0 ]] || { echo "evict.sh: status --watch needs a terminal" >&2; exit 1; }
    command -v python3 >/dev/null && [[ -f $pretty ]] || { echo "evict.sh: status --watch needs python3 and $pretty" >&2; exit 1; }
    tmp=$(mktemp -d)
    # The alternate screen, no cursor, no line wrap; all of it back on the way out.
    trap 'printf "\e[?7h\e[?25h\e[?1049l"; stty echo 2>/dev/null; rm -rf "$tmp"' EXIT
    trap 'exit 0' INT TERM
    printf '\e[?1049h\e[?25l\e[?7l'
    stty -echo 2>/dev/null || true
    since=
    while :; do
        status=0
        run >"$tmp/now" 2>&1 || status=$?
        flock -u 9      # free between refreshes: other evict.sh commands go on meanwhile
        if [[ -z $since ]]; then
            cp "$tmp/now" "$tmp/base"
            since=$(date +%T)
        fi
        read -r rows cols < <(stty size 2>/dev/null || echo "40 100")
        top="$(printf '\e[1mvidmem_evict\e[0m  %s  every %s s, changes since %s' "$(date +%T)" "$every" "$since")"
        [[ $status -eq 0 ]] || top+="$(printf '  \e[31mthe driver answered %s\e[0m' "$status")"
        top+="$(printf '  \e[2mq: quit  r: changes from now\e[0m')"
        frame=$( { echo "$top"; echo; COLUMNS=$cols FORCE_COLOR=1 python3 "$pretty" --prev "$tmp/base" --rows $((rows - 2)) "$tmp/now"; } \
                 | head -n "$rows")
        # Home, each line cleared to its end, the rest of the screen cleared: no flicker.
        printf '\e[H%s\e[K\e[J' "${frame//$'\n'/$'\e[K\n'}"
        key=
        IFS= read -rsn1 -t "$every" key || true
        case $key in
            q|Q) exit 0 ;;
            r|R) since= ;;
        esac
        flock 9
    done
fi
if [[ $view == pretty ]] && command -v python3 >/dev/null && [[ -f $pretty ]]; then
    # sudo does not pass COLUMNS on; a pipe (--pretty | less -R) has no width of its own.
    cols=$( { stty size </dev/tty; } 2>/dev/null | cut -d' ' -f2) || true
    color=; [[ -t 1 ]] || color=1
    run | COLUMNS=${cols:-} FORCE_COLOR=$color python3 "$pretty"     # set -e, pipefail: run's status
    exit 0
fi
run
