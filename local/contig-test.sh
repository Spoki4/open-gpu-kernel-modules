#!/bin/bash
# BACKLOG #16/#15: room for big contiguous buffers in fragmented vidmem is made
# by moving memory to other vidmem pages, not out to sysmem. Twelve idle apps
# allocate 16 MB buffers at the same time (interleaved), every other one
# exits: gigabytes free, in small pieces. Then a "compositor" (gbm-hog named
# kwin_wayland) asks for 256 MB scanout buffers, which are contiguous. Checks:
# it gets them, the survivors' memory stays in vidmem and intact. Run in the
# test VM guest with nobody logged in. Ends with CONTIGTEST PASS|FAIL.
#   ./contig-test.sh [BUFFERS=8]
cd "$(dirname "$0")" || exit 1
bufs=${1:-8}
T=$(mktemp -d /tmp/contig-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
frag() { sudo -n ./evict.sh status | grep '^fragmentation' | sed 's/^fragmentation: //'; }
pids=()
trap 'kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
for i in $(seq 0 11); do
    ./"${names[i % 6]}" --mb 800 --chunk-mb 16 --idle --verify-s 2 --floor-mb 512 >"$T/p$i.log" 2>&1 &
    pids+=($!)
done
for i in $(seq 0 11); do
    for _ in $(seq 150); do grep -q '^START' "$T/p$i.log" && break; sleep 0.2; done
done
echo "filled:   $(frag)"
keep=()
for i in $(seq 0 11); do
    if (( i % 2 )); then kill "${pids[i]}"; wait "${pids[i]}" 2>/dev/null; else keep+=("${pids[i]}"); fi
done
sleep 1
echo "holes:    $(frag)"
before=$(sudo -n ./evict.sh status | grep -o 'windows cleared.*: [0-9]* MB' | grep -o '[0-9]* MB$' | cut -d' ' -f1)
cp ./gbm-hog "$T/kwin_wayland"
"$T/kwin_wayland" --force --scanout --size 8192x8192 --max "$bufs" --floor-mb 64 --hold 6 >"$T/comp.log" 2>&1 &
comp=$!
for _ in $(seq 150); do grep -q '^RESULT' "$T/comp.log" && break; sleep 0.2; done
echo "  compositor: $(grep '^RESULT' "$T/comp.log" || tail -2 "$T/comp.log")"
echo "with them: $(frag)"
after=$(sudo -n ./evict.sh status | grep -o 'windows cleared.*: [0-9]* MB' | grep -o '[0-9]* MB$' | cut -d' ' -f1)
sudo journalctl -k --since "$T0" --no-pager | grep -o 'window .*cleared: .*' | sed 's/^/  /' | tail -12
ram=0
for p in "${keep[@]}"; do
    r=$(sudo -n ./evict.sh status | awk -v p="$p" '$2 == p || $1 == p {print $4; exit}')
    ram=$((ram + ${r:-0}))
done
sudo -n ./evict.sh status | sed -n '1,12p' >"$T/status.txt"
echo "  moved inside vidmem: $(( ${after:-0} - ${before:-0} )) MB; survivors in sysmem: $ram MB"
got=$(grep -o ' count=[0-9]*' "$T/comp.log" | cut -d= -f2)
[[ ${got:-0} -ge $bufs ]] || fail "the compositor got ${got:-0} of $bufs buffers"
(( ${after:-0} > ${before:-0} )) || fail "nothing was moved inside vidmem"
(( ram == 0 )) || fail "$ram MB of the survivors went to sysmem although vidmem had room"
wait $comp 2>/dev/null
sleep 3      # one more check of every survivor after the moves
for p in "${keep[@]}"; do kill -INT "$p"; done
for i in 0 2 4 6 8 10; do
    wait "${pids[i]}"; rc=$?
    (( rc == 0 )) || fail "app $i exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/p$i.log")"
done
pids=()
echo "  survivors: $(grep -h '^RESULT' "$T"/p{0,2,4,6,8,10}.log | grep -o 'verifies=[0-9]* errors=[0-9]*' | tr '\n' ';')"
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' | tail -5
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed' && fail "kernel log"
(( fails == 0 )) && echo "CONTIGTEST PASS" || echo "CONTIGTEST FAIL $fails"
