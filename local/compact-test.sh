#!/bin/bash
# BACKLOG #16: background compaction. Vidmem is fragmented as in
# contig-test.sh (twelve idle apps with interleaved 16 MB buffers, every other
# one exits); then the policy, asked to keep a TARGET MB free run, moves the
# survivors' buffers a little every step. Checks: the longest free run grows,
# nothing goes to sysmem, the content is intact. Run in the test VM guest.
# With `fg` the survivors are the window in focus (hints written by hand: run
# with nobody logged in on the guest): 40 s with `compactfg off` must move
# nothing, then compaction must gather the run out of the foreground's memory
# too (BACKLOG #16, 2026-11-11).
# Ends with COMPACTTEST PASS|FAIL.
#   ./compact-test.sh [SECONDS=120] [TARGET_MB=4096] [fg]
cd "$(dirname "$0")" || exit 1
dur=${1:-120} target=${2:-4096} fg=${3:-}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/compact-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ./evict.sh status; }
frag() { st | grep '^fragmentation' | sed 's/^fragmentation: //'; }
longest() { st | grep '^fragmentation' | grep -o 'longest [0-9]*' | cut -d' ' -f2; }
old=$(st | grep -o 'keeps a free run of [0-9]*' | grep -o '[0-9]*$')
pids=()
trap 'sudo -n ./evict.sh compact "${old:-1024}" >/dev/null; sudo -n ./evict.sh compactfg on >/dev/null; [[ -z $fg ]] || : >"$H" 2>/dev/null; kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
sudo -n ./evict.sh compact 0 >/dev/null
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
for i in $(seq 0 11); do
    ./"${names[i % 6]}" --mb 800 --chunk-mb 16 --idle --verify-s 30 --floor-mb 512 >"$T/p$i.log" 2>&1 &
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
l0=$(longest)
echo "holes:  $(frag)"
if [[ -n $fg ]]; then
    # This shell is the foreground, and so are the survivors, by inheritance.
    printf '%s f\n' $$ >"$H"
    sudo -n ./evict.sh compactfg off >/dev/null
    sudo -n ./evict.sh compact $((target - 1)) >/dev/null
    sleep 40
    loff=$(longest)
    echo "  compactfg off, 40 s: longest $l0 -> $loff MB; $(st | grep -o 'background compaction.*'); $(st | grep -o 'compaction saw.*')"
    (( loff < l0 + 64 )) || fail "the foreground's memory moved with compactfg off ($l0 -> $loff MB)"
    grep -q "^$$ f" "$H" || fail "the hints were overwritten (somebody logged in on the guest?): $(tr '\n' ' ' <"$H")"
    sudo -n ./evict.sh compactfg on >/dev/null
fi
sudo -n ./evict.sh compact "$target" >/dev/null
t0=$SECONDS
while (( SECONDS - t0 < dur )); do
    sleep 10
    printf '%4d s: longest %5d MB; %s\n' $((SECONDS - t0)) "$(longest)" "$(st | grep -o 'background compaction.*')"
done
l1=$(longest)
echo "after:  $(frag)"
ram=0
for p in "${keep[@]}"; do
    r=$(st | awk -v p="$p" '$1 == p {print $4; exit}')
    ram=$((ram + ${r:-0}))
done
echo "  longest free run $l0 -> $l1 MB; survivors in sysmem: $ram MB"
(( l1 >= l0 + 256 )) || fail "the longest free run did not grow ($l0 -> $l1 MB)"
(( ram == 0 )) || fail "$ram MB of the survivors went to sysmem"
sleep 3
for p in "${keep[@]}"; do kill -INT "$p"; done
for i in 0 2 4 6 8 10; do
    wait "${pids[i]}"; rc=$?
    (( rc == 0 )) || fail "app $i exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/p$i.log")"
done
pids=()
echo "  survivors: $(grep -h '^RESULT' "$T"/p{0,2,4,6,8,10}.log | grep -o 'verifies=[0-9]* errors=[0-9]*' | tr '\n' ';')"
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' | tail -5
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed' && fail "kernel log"
(( fails == 0 )) && echo "COMPACTTEST PASS" || echo "COMPACTTEST FAIL $fails"
