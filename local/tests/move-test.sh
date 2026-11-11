#!/bin/bash
# problem #16: evict.sh move (vidmem -> other vidmem pages, no sysmem) keeps
# the content: a rendering nvt-game that rewrites what it reads, an idle one,
# and compr-test images (compressible kind on 2 MB and on 64 KB pages, a
# cleared image and noise) are moved CYCLES times each and checked. Run in the
# test VM guest. Ends with MOVETEST PASS|FAIL.
#   ./move-test.sh [CYCLES=10]
cd "$(dirname "$0")" || exit 1
cycles=${1:-10}
T=$(mktemp -d /tmp/move-test.XXXX)
fails=0
T0=$(date '+%F %T')
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
moves() { # PID -> prints the last summary, fails on anything not moved
    local pid=$1 i out
    for i in $(seq "$cycles"); do
        out=$(sudo -n ../evict.sh move "$pid" 2>&1)
        grep -q 'summary: moved [1-9]' <<<"$out" || { fail "move $i: $(grep -E 'summary|failed' <<<"$out" | head -2)"; return; }
        grep -q 'failed 0 ' <<<"$out" || fail "move $i: $(grep -E 'summary|failed 0x' <<<"$out" | head -3)"
        sleep 0.3
    done
    grep -E 'summary|phases|copy:' <<<"$out" | sed 's/^/  /'
}
game() { # NAME ARGS...
    local name=$1; shift
    ./"$name" "$@" >"$T/$name.log" 2>&1 &
    local pid=$!
    for _ in $(seq 100); do grep -q '^START' "$T/$name.log" && break; sleep 0.2; done
    echo "== $name $*: $(grep '^START' "$T/$name.log")"
    moves $pid
    sleep 2
    kill -INT $pid; wait $pid; local rc=$?
    echo "  $(grep '^RESULT' "$T/$name.log")"
    (( rc == 0 )) || fail "$name exit $rc: $(grep -m1 '^ERROR' "$T/$name.log")"
}
compr() { # ARGS...
    ./compr-test --size 4096 --mode hold --interval-ms 100 --keep-going "$@" >"$T/compr.log" 2>&1 &
    local pid=$! n
    for _ in $(seq 100); do grep -q '^OK 1' "$T/compr.log" && break; sleep 0.2; done
    echo "== compr-test $*: $(head -1 "$T/compr.log")"
    sudo -n ../evict.sh dry $pid 2>&1 | awk '$2 > 60000 && /KB kind/' | head -2 | sed 's/^/  /'
    n=$(grep -c '^OK' "$T/compr.log")
    moves $pid
    # Two more checks: the first may have begun before the last move.
    n=$(grep -c '^OK\|^MISMATCH' "$T/compr.log")
    for _ in $(seq 100); do (( $(grep -c '^OK\|^MISMATCH' "$T/compr.log") >= n + 2 )) && break; sleep 0.2; done
    (( $(grep -c '^OK\|^MISMATCH' "$T/compr.log") >= n + 2 )) || fail "compr-test: no check ran after the moves"
    kill $pid; wait $pid 2>/dev/null
    grep -q MISMATCH "$T/compr.log" && fail "compr-test $*: $(grep -m1 MISMATCH "$T/compr.log")"
    echo "  checks: $(grep -c '^OK' "$T/compr.log") ok, $(grep -c MISMATCH "$T/compr.log") mismatch"
}
game nvt-bg-a --mb 1024 --chunk-mb 32 --fps 30 --write --verify-s 2
game nvt-bg-b --mb 1024 --chunk-mb 256 --fps 30 --gfx --write --verify-s 2
game nvt-idle-a --mb 2048 --chunk-mb 8 --idle --verify-s 1
compr
compr --noise
compr --chunk-mb 128
compr --chunk-mb 128 --noise
echo "kernel:"
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|WARNING|general protection|not remade|move copy failed|[Aa]ssert|NV_ERR' | tail -10
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' && fail "kernel log"
sudo -n ../evict.sh status | grep '^fragmentation'
rm -rf "$T"
(( fails == 0 )) && echo "MOVETEST PASS" || echo "MOVETEST FAIL $fails"
