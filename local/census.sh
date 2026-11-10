#!/bin/bash
# Prints the vidmem census of one process (needs the patched module).
#
#   sudo ./census.sh <pid>
#   sudo ./census.sh chrome        # Chrome's GPU process
#   sudo ./census.sh <name>        # oldest process with that exact name
#
# Add --summary to print only the totals.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "census.sh: run with sudo" >&2; exit 1; }
[[ $# -ge 1 ]] || { echo "usage: sudo $0 <pid|chrome|name> [--summary]" >&2; exit 2; }

target=$1
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
[[ -n ${pid:-} ]] || { echo "census.sh: no process for '$target'" >&2; exit 1; }

shopt -s nullglob
files=(/proc/driver/nvidia/gpus/*/vidmem_census)
[[ ${#files[@]} -gt 0 ]] || { echo "census.sh: vidmem_census not found; is the patched module loaded?" >&2; exit 1; }

for f in "${files[@]}"; do
    echo "== $f ($(cat "/proc/$pid/comm" 2>/dev/null || echo '?'))"
    echo "$pid" > "$f"
    if [[ ${2:-} == --summary ]]; then
        sed -n '/^summary:/,$p' "$f"
    else
        cat "$f"
    fi
done
