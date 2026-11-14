#!/bin/bash
# Sides (sim/scenarios/place-sides.scn): while a game runs, the others' new
# memory is placed from the bottom of vidmem and the game's from the top, so
# what the others free leaves holes between their own memory, not between the
# game's blocks. A first game process (512 MB, focused, fullscreen) makes the
# driver see a game; then a second one (a third of the free vidmem in 64 MB
# blocks) and six apps (39 % in 16 MB buffers, under 1 GB each: more would
# count as a game) allocate at the same time, and every other app exits. A
# quarter stays free throughout: the focused game's headroom is not the
# subject. With the focus on an app, compaction is asked for a
# long free run. Checks: the run grows, no memory of the game is moved for it
# (the kernel log names the owner of every move), nothing goes to sysmem,
# contents intact. With `evict.sh placesides off` the holes are between the
# game's blocks and the second check fails. Window hints written by hand:
# run with nobody logged in on the machine.
# Ends with SIDESTEST PASS|FAIL.
#   ./sides-test.sh [SECONDS=90]
cd "$(dirname "$0")" || exit 1
dur=${1:-90}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/sides-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
frag() { st | grep '^fragmentation' | sed 's/^fragmentation: //'; }
longest() { st | grep '^fragmentation' | grep -o 'longest [0-9]*' | cut -d' ' -f2; }
wait_start() { local i; for i in $(seq 300); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
old=$(st | grep -o 'keeps a free run of [0-9]*' | grep -o '[0-9]*$')
g1= g2=
pids=()
trap 'sudo -n ../evict.sh compact "${old:-1024}" >/dev/null; kill $g1 $g2 "${pids[@]}" 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
st | grep -q '^sides: on' || { echo "  FAIL: this driver has no sides, or they are off: $(st | grep '^sides')"; echo "SIDESTEST FAIL 1"; exit 0; }
sudo -n ../evict.sh compact 0 >/dev/null
free0=$(st | grep '^fragmentation' | grep -o 'free [0-9]*' | cut -d' ' -f2)
[[ ${free0:-0} -ge 6000 ]] || { echo "  FAIL: only ${free0:-?} MB free at the start"; echo "SIDESTEST FAIL 1"; exit 0; }
gmb=$(( (free0 - 512) * 33 / 100 / 64 * 64 ))
amb=$(( (free0 - 512) * 39 / 100 / 6 / 16 * 16 )); (( amb <= 896 )) || amb=896

# The game is up: a fullscreen window in focus, holding vidmem.
( printf '%s f g\n' "$BASHPID" >"$H"
  exec ./nvt-game --mb 512 --chunk-mb 64 --idle --verify-s 30 --floor-mb 0 ) >"$T/g1.log" 2>&1 &
g1=$!
wait_start "$T/g1.log"
for i in $(seq 20); do st | grep -q '^sides: on; a game holds vidmem' && break; sleep 0.5; done
st | grep -q '^sides: on; a game holds vidmem' || fail "the driver sees no game: $(st | grep '^sides')"

# The rest of the game and the apps allocate at the same time.
( printf '%s f g\n%s v g\n' "$g1" "$BASHPID" >"$H"
  exec ./nvt-game --mb "$gmb" --chunk-mb 64 --idle --verify-s 30 --floor-mb 0 ) >"$T/g2.log" 2>&1 &
g2=$!
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
for i in 0 1 2 3 4 5; do
    ./"${names[i]}" --mb "$amb" --chunk-mb 16 --idle --verify-s 30 --floor-mb 0 >"$T/p$i.log" 2>&1 &
    pids+=($!)
done
wait_start "$T/g2.log"
for i in 0 1 2 3 4 5; do wait_start "$T/p$i.log"; done
echo "game: 512 + $(grep -o 'allocated_mb=[0-9]*' "$T/g2.log" | cut -d= -f2) MB in 64 MB blocks; six apps of $amb MB in 16 MB buffers; $(st | grep '^sides')"
[[ $(grep -o 'allocated_mb=[0-9]*' "$T/g2.log" | cut -d= -f2) -ge $gmb ]] || fail "the game did not get its $gmb MB"
for i in 1 3 5; do kill "${pids[i]}"; wait "${pids[i]}" 2>/dev/null; done
sleep 1
l0=$(longest)
echo "holes:  $(frag)"

# The user looks at an app: compaction may take pieces of 256 MB, the game's blocks too if they are in the way.
printf '%s v g\n%s v g\n%s f\n' "$g1" "$g2" "${pids[0]}" >"$H"
freenow=$(st | grep '^fragmentation' | grep -o 'free [0-9]*' | cut -d' ' -f2)
sudo -n ../evict.sh compact $((freenow * 70 / 100)) >/dev/null
t0=$SECONDS
while (( SECONDS - t0 < dur )); do
    sleep 10
    printf '%4d s: longest %5d MB; %s\n' $((SECONDS - t0)) "$(longest)" "$(st | grep -o 'background compaction[^;]*;[^;]*')"
done
l1=$(longest)
echo "after:  $(frag)"
gamemb=$(sudo journalctl -k --since "$T0" --no-pager |
         sed -n "s/.*vidmem evict: move pid \($g1\|$g2\): [0-9]* allocs \([0-9]*\) MB.*/\2/p" | awk '{s += $1} END {print s + 0}')
othermb=$(sudo journalctl -k --since "$T0" --no-pager |
          sed -n 's/.*vidmem evict: move pid [0-9]*: [0-9]* allocs \([0-9]*\) MB.*/\1/p' | awk '{s += $1} END {print s + 0}')
ram=0
for p in $g1 $g2 "${pids[0]}" "${pids[2]}" "${pids[4]}"; do
    r=$(st | awk -v p="$p" '$1 == p {print $4; exit}')
    ram=$((ram + ${r:-0}))
done
echo "  longest free run $l0 -> $l1 MB; moved inside vidmem: $othermb MB, $gamemb of them the game's; in sysmem: $ram MB"
(( l1 >= l0 + 256 )) || fail "the longest free run did not grow ($l0 -> $l1 MB)"
(( othermb > 0 )) || fail "compaction moved nothing"
(( gamemb == 0 )) || fail "$gamemb MB of the game were moved: the holes were between its blocks"
(( ram == 0 )) || fail "$ram MB went to sysmem"
grep -q "^${pids[0]} f" "$H" || fail "the hints were overwritten (somebody logged in?): $(tr '\n' ' ' <"$H")"
sleep 3
for p in g1 g2; do
    pid=${!p}
    kill -INT "$pid" 2>/dev/null; wait "$pid"; rc=$?
    (( rc == 0 )) || fail "$p exited with $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/$p.log")"
    grep -q '^RESULT.* errors=0' "$T/$p.log" || fail "$p read back wrong data: $(grep '^RESULT' "$T/$p.log")"
done
g1= g2=
for i in 0 2 4; do
    kill -INT "${pids[i]}" 2>/dev/null; wait "${pids[i]}"; rc=$?
    (( rc == 0 )) || fail "app $i exited with $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/p$i.log")"
    grep -q '^RESULT.* errors=0' "$T/p$i.log" || fail "app $i read back wrong data: $(grep '^RESULT' "$T/p$i.log")"
done
pids=()
: >"$H"
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' | tail -5
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed' && fail "kernel log"
(( fails == 0 )) && echo "SIDESTEST PASS" || echo "SIDESTEST FAIL $fails"
