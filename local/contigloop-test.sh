#!/bin/bash
# BACKLOG #15: a focused app that remakes a big contiguous buffer over and
# over (Chrome on the host: 130 MB up to 30 times a second) in fragmented
# vidmem. Each request used to clear the window its own previous buffer sat
# in: that buffer went to sysmem (165 clearings in two hours on the host), and
# came back as "the foreground's memory" at the cost of the game. Now its own
# memory never goes to sysmem for its own request: the first requests clear
# windows from hidden apps (moved inside vidmem), after that the buffer it
# frees is the room for the next one.
# Setup as contig-test.sh (twelve idle apps, interleaved 16 MB buffers, every
# other one exits); the survivors are hidden, gbm-hog is the foreground (window
# hints written by hand: run with nobody logged in on the guest, or its KWin
# overwrites them). Ends with CONTIGLOOPTEST PASS|FAIL.
#   ./contigloop-test.sh [CHURN=40] [KEEP=2]
cd "$(dirname "$0")" || exit 1
churn=${1:-40} keepbufs=${2:-2}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/contigloop-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ./evict.sh status; }
frag() { st | grep '^fragmentation' | sed 's/^fragmentation: //'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
pids=()
trap 'kill "${pids[@]}" ${hog:-} 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
for i in $(seq 0 11); do
    ./"${names[i % 6]}" --mb 800 --chunk-mb 16 --idle --verify-s 2 --floor-mb 512 >"$T/p$i.log" 2>&1 &
    pids+=($!)
done
for i in $(seq 0 11); do
    for _ in $(seq 150); do grep -q '^START' "$T/p$i.log" && break; sleep 0.2; done
done
keep=()
for i in $(seq 0 11); do
    if (( i % 2 )); then kill "${pids[i]}"; wait "${pids[i]}" 2>/dev/null; else keep+=("${pids[i]}"); fi
done
sleep 1
echo "holes:    $(frag)"
# This shell (and so gbm-hog, by inheritance) is the foreground; the survivors hidden.
{ printf '%s f\n' $$; printf '%s h\n' "${keep[@]}"; } >"$H"
sleep 12    # hidden and idle for over 10 s: they may give memory
./gbm-hog --force --scanout --size 8192x4096 --max "$keepbufs" --churn "$churn" --floor-mb 64 --hold 4 >"$T/hog.log" 2>&1 &
hog=$!
for _ in $(seq 600); do grep -q '^RESULT' "$T/hog.log" && break; kill -0 $hog 2>/dev/null || break; sleep 0.2; done
echo "  app: $(grep '^RESULT' "$T/hog.log" || tail -2 "$T/hog.log")"
grep -q "^$$ f" "$H" || fail "the hints were overwritten (somebody logged in on the guest?): $(tr '\n' ' ' <"$H")"
echo "with it:  $(frag)"
hogram=$(st | awk -v p="$hog" '$1 == p {print $4; exit}')
klog | grep 'vidmem evict' >"$T/k.log"
clears=$(grep -c 'contiguous: clearing' "$T/k.log")
none=$(grep -c 'contiguous: no .* window' "$T/k.log")
own=$(grep -c "evict pid $hog:" "$T/k.log")
grep -E 'contiguous: |window (not )?cleared|inside vidmem failed' "$T/k.log" | sed 's/^.*vidmem evict: /  /' | head -12
ram=0
for p in "${keep[@]}"; do
    r=$(st | awk -v p="$p" '$1 == p {print $4; exit}')
    ram=$((ram + ${r:-0}))
done
echo "  $churn remakes: $clears windows cleared, $none refused, own memory moved out $own times (${hogram:-0} MB in sysmem now); hidden apps in sysmem: $ram MB"
got=$(grep -o ' churned=[0-9]*' "$T/hog.log" | cut -d= -f2)
[[ ${got:-0} -ge $churn ]] || fail "the app made ${got:-0} of $churn remakes ($(grep -o 'stop=[a-z-]*' "$T/hog.log"))"
(( own == 0 )) || fail "the app's own memory was moved out $own times for its own requests"
(( ${hogram:-0} == 0 )) || fail "$hogram MB of the app in sysmem"
(( clears >= 1 )) || fail "no window was cleared (test setup: vidmem not fragmented?)"
(( clears <= keepbufs + 3 )) || fail "$clears windows cleared for $churn remakes of $keepbufs buffers (a loop)"
wait $hog 2>/dev/null; hog=
: >"$H"
sleep 3      # one more check of every survivor after the moves
for p in "${keep[@]}"; do kill -INT "$p"; done
for i in 0 2 4 6 8 10; do
    wait "${pids[i]}"; rc=$?
    (( rc == 0 )) || fail "app $i exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/p$i.log")"
done
pids=()
echo "  survivors: $(grep -h '^RESULT' "$T"/p{0,2,4,6,8,10}.log | grep -o 'verifies=[0-9]* errors=[0-9]*' | tr '\n' ';')"
klog | grep -E 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' && fail "kernel log"
(( fails == 0 )) && echo "CONTIGLOOPTEST PASS" || echo "CONTIGLOOPTEST FAIL $fails"
