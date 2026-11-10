#!/bin/bash
# BACKLOG #5: how fast does an image's memory move to sysmem and back, as a
# dedicated allocation (2 MB pages) and inside a plain 256 MB block (64 KB
# pages, what DXVK does)? Content is checked by compr-test while it is out
# and after it is back. Run in the test VM guest.
#   ./compr-speed.sh [SIZE=8192] [variant args...]
cd "$(dirname "$0")" || exit 1
size=${1:-8192}
run() {
    local name=$1; shift
    ./compr-test --size "$size" --mode hold --interval-ms 300 --keep-going "$@" >/tmp/compr-speed.log 2>&1 &
    local pid=$!
    for _ in $(seq 100); do grep -q '^OK 1' /tmp/compr-speed.log && break; sleep 0.2; done
    echo "== $name: $(head -1 /tmp/compr-speed.log)"
    sudo -n ./evict.sh dry $pid 2>&1 | awk '$2 > 60000 && /KB kind/' | head -4
    for op in evict restore; do
        sudo -n ./evict.sh $op $pid 2>&1 | grep -E 'summary|phases|copy:' | sed "s/^/  $op: /"
        n=$(grep -c '^OK' /tmp/compr-speed.log); sleep 1.2
        (( $(grep -c '^OK' /tmp/compr-speed.log) > n )) || echo "  no check ran after $op"
    done
    kill $pid; wait $pid 2>/dev/null
    echo "  checks: $(grep -c '^OK' /tmp/compr-speed.log) ok, $(grep -c MISMATCH /tmp/compr-speed.log) mismatch $(grep -m1 MISMATCH /tmp/compr-speed.log)"
}
shift
if (( $# )); then run "$*" "$@"; exit; fi
run dedicated
run "chunk 256 MB" --chunk-mb 256
run "dedicated, noise" --noise
run "chunk 256 MB, noise" --chunk-mb 256 --noise
