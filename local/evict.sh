#!/bin/bash
# Moves a process's vidmem to system memory and back, and controls the
# automatic policy (needs the patched module, see
# src/nvidia/src/kernel/mem_mgr/vidmem_evict.c).
#
#   sudo ./evict.sh status                      # per process: vram, ram, evictable, idle
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
#   sudo ./evict.sh hot     on|off|momc         # access counters: evicted allocations in use
#                                               # come back one by one (experiment); momc: also
#                                               # count accesses to vidmem (a probe, tests/momc-probe.sh)
#   sudo ./evict.sh quiesce 0..4                # how channels are stopped (experiment, see
#                                               # _vidmemEvictQuiesce in vidmem_evict.c)
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "evict.sh: run with sudo" >&2; exit 1; }
usage() { echo "usage: sudo $0 status | dry|evict|restore|move|shared <pid|chrome|name> | auto|trace|windows|moveshared|fgown|compactfg on|off | quiesce 0..4 | compact MB | trace-lock US" >&2; exit 2; }
[[ $# -ge 1 ]] || usage

mode=$1
case $mode in
    status)
        [[ $# -eq 1 ]] || usage
        cmd="status"
        ;;
    hot)
        [[ $# -eq 2 ]] || usage
        case $2 in on|1) cmd="hot 1" ;; off|0) cmd="hot 0" ;; momc|2) cmd="hot 2" ;; *) usage ;; esac
        ;;
    auto|trace|windows|moveshared|fgown|compactfg)
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

status=0
for f in "${files[@]}"; do
    [[ ${#files[@]} -gt 1 ]] && echo "== $f"
    echo "$cmd" > "$f" || status=$?
    cat "$f"
done
exit $status
