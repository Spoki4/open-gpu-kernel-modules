#!/bin/bash
# A contiguous buffer made right after its process freed others of the same
# size waits for the scrubber (scrub on free) only until one of them is
# clean. On the host (recording 20261116_184241) Chrome freed two 130 MB
# scanout buffers and asked for one at once, with no other free run that
# could hold it: PMA waited for both to be scrubbed, 6-8 ms holding the RM API
# lock, 3308 times in two hours (16 of the game's 22 long frames caused by the
# driver). scrubCheckAndWaitForSize() waited for "as many pages in all, and
# one item more".
# Setup: a process nobody may move (named like the login screen) fills vidmem
# up to what gbm-hog's buffers take, so a new buffer has only the pages just
# freed. gbm-hog remakes them, first one at a time (free one, make one: one
# scrub to wait for, the reference), then two at a time (free two, make two:
# Chrome's order), and reports how long each creation took.
# Checks: with two at a time the slow creations (p99) take no longer than 1.5
# times the reference (before: about twice), the buffers keep being made, the
# kernel log stays clean. Ends with SCRUBWAITTEST PASS|FAIL.
#   ./scrubwait-test.sh [SIZE=5120x6656 (130 MB)] [REMAKES=150]
cd "$(dirname "$0")" || exit 1
size=${1:-5120x6656} n=${2:-150}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/scrubwait-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
free_mb() { st | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
hog=
trap 'kill $hog 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
ln -sf nvt-game ./sddm-greeter
: >"$H"
# No background compaction meanwhile: it would move the hog to make free runs.
oldrun=$(st | grep -o 'keeps a free run of [0-9]*' | grep -o '[0-9]*$')
sudo -n ../evict.sh compact 0 >/dev/null
trap 'sudo -n ../evict.sh compact "${oldrun:-1024}" >/dev/null; kill $hog 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
bufmb=$(( $(echo "$size" | awk -Fx '{print $1 * $2 * 4}') >> 20 ))
free=$(free_mb)
[[ ${free:-0} -gt $((2 * bufmb + 512)) ]] || { echo "  FAIL: only ${free:-?} MB free"; echo "SCRUBWAITTEST FAIL 1"; exit 0; }
# Leave room for two buffers and a little: no free run for a third.
./sddm-greeter --mb $((free - 2 * bufmb - 24)) --chunk-mb 64 --idle --floor-mb 0 >"$T/hog.log" 2>&1 &
hog=$!
wait_start "$T/hog.log"
echo "pinned: $(grep -o 'allocated_mb=[0-9]*' "$T/hog.log"); free now $(free_mb) MB, buffers of $bufmb MB"

run() {     # $1 batch, $2 log
    ./gbm-hog --force --scanout --size "$size" --max 2 --churn "$n" --free-first --churn-batch "$1" \
              --churn-us 20000 --floor-mb 0 --hold 0 >"$2" 2>&1
    grep '^CHURN' "$2" || echo "  (gbm-hog: $(grep -E '^RESULT|stopped' "$2" | head -2 | tr '\n' ' ')$(tail -1 "$2"))"
}
echo "  one at a time (the reference):"; run 1 "$T/b1.log" | sed 's/^/    /'
echo "  two at a time (Chrome's order):"; run 2 "$T/b2.log" | sed 's/^/    /'
p1=$(sed -n 's/^CHURN.* p99_ms=\([0-9.]*\).*/\1/p' "$T/b1.log")
p2=$(sed -n 's/^CHURN.* p99_ms=\([0-9.]*\).*/\1/p' "$T/b2.log")
c2=$(sed -n 's/^CHURN creates=\([0-9]*\).*/\1/p' "$T/b2.log")
[[ -n $p1 && -n $p2 ]] || fail "no timings (gbm-hog did not churn)"
[[ ${c2:-0} -ge $((2 * n)) ]] || fail "two at a time made only ${c2:-0} of $((2 * n)) buffers: $(grep -o 'stop=[a-z-]*' "$T/b2.log")"
if [[ -n $p1 && -n $p2 ]]; then
    awk -v a="$p2" -v b="$p1" 'BEGIN { exit !(a <= 1.5 * b) }' ||
        fail "two at a time p99 $p2 ms against $p1 ms one at a time (wanted at most 1.5 times)"
    echo "  p99: $p2 ms two at a time, $p1 ms one at a time"
fi
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|Assertion failed' && fail "kernel log"
(( fails == 0 )) && echo "SCRUBWAITTEST PASS" || echo "SCRUBWAITTEST FAIL $fails"
