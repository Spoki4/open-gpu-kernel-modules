#!/bin/bash
# R5 over R2 (the user's choice 2026-11-16): a focused app that is not a game
# never moves a visible game out to sysmem. On the host (2026-11-15 22:18)
# Chrome, focused, remade 130 MB contiguous buffers ~145 times a second next
# to WoW on the other monitor; three 256 MB blocks of WoW found no other place
# in vidmem for the windows cleared over them and went to sysmem, and the game
# ran at 25 fps for ten seconds. Now the game's memory only moves inside
# vidmem; room for that is made by hidden apps and the app's own memory.
# Setup: a "game" (nvt-game drawing, its window visible and fullscreen) holds
# blocks of 256 MB, small idle apps allocate at the same time and land between
# them; a quarter of them exit, leaving holes of 16 MB: enough free memory for
# the buffers, no free run that holds one. gbm-hog is the foreground and
# remakes a contiguous buffer longer than any free run. Its buffers are made by
# nvidia-drm's kernel client, so it has no memory of its own to pay with: when
# the free bytes run short, the new rule turns it away (the hidden apps check
# their memory every 2 s, "drawing", and the game is not to be moved out).
# Window hints written by hand: run with nobody logged in, or KWin overwrites
# them.
# Checks: nothing of the game goes to sysmem, its content stays right, the app
# makes its remakes, the kernel log stays clean. GIVES=1 runs the old rule
# (evict.sh gamegives on) as a control: the game's memory then may go out.
# Ends with CONTIGGAMETEST PASS|FAIL.
#   ./contiggame-test.sh [CHURN=20] [GIVES=0] [EXIT=4]
# EXIT: one app in EXIT exits (4: a quarter; 8: less free memory, the game's
# blocks find no other place and room is made for them, or the app is refused).
cd "$(dirname "$0")" || exit 1
churn=${1:-20} gives=${2:-0} every=${3:-4}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/contiggame-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
frag() { st | grep '^fragmentation' | sed 's/^fragmentation: //'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
pids=() game= hog=
# No background compaction meanwhile: it would gather the free run the test needs short.
oldrun=$(st | grep -o 'keeps a free run of [0-9]*' | grep -o '[0-9]*$')
sudo -n ../evict.sh compact 0 >/dev/null
sudo -n ../evict.sh gamegives "$gives" >/dev/null
trap 'sudo -n ../evict.sh compact "${oldrun:-1024}" >/dev/null; sudo -n ../evict.sh gamegives 0 >/dev/null; kill "${pids[@]}" $game $hog 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
free0=$(frag | grep -o 'free [0-9]*' | cut -d' ' -f2)
free0=${free0:-11000}
# Two thirds for the game in 256 MB blocks, the rest for 16 apps of 16 MB pieces, all at once.
gmb=$(( free0 * 2 / 3 / 256 * 256 ))
amb=$(( (free0 - gmb) / 16 ))
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
./nvt-game --mb "$gmb" --chunk-mb 256 --read-mb 64 --fps 60 --verify-s 5 --floor-mb 0 >"$T/game.log" 2>&1 &
game=$!
for i in $(seq 0 15); do
    ./"${names[i % 6]}" --mb "$amb" --chunk-mb 16 --idle --verify-s 2 --floor-mb 96 >"$T/p$i.log" 2>&1 &
    pids+=($!)
done
for f in "$T/game.log" "$T"/p*.log; do
    for _ in $(seq 300); do grep -q '^START' "$f" && break; sleep 0.2; done
done
echo "game:     $(grep '^START' "$T/game.log")"
# A quarter of the apps exit: holes of 16 MB between the game's blocks and the others' pieces.
keep=()
for i in $(seq 0 15); do
    if (( i % every == 3 )); then kill "${pids[i]}"; wait "${pids[i]}" 2>/dev/null; else keep+=("${pids[i]}"); fi
done
sleep 1
echo "holes:    $(frag)"
lrun=$(frag | grep -o 'longest [0-9]*' | cut -d' ' -f2)
# A buffer longer than the longest free run (a window is needed), 8192 rows of 4 bytes.
w=$(( ((${lrun:-64} + 64) * 32 + 1023) / 1024 * 1024 )); (( w >= 4096 )) || w=4096; (( w <= 16384 )) || w=16384
echo "  buffers: ${w}x8192, $((w / 32)) MB (longest free run ${lrun:-?} MB); gamegives $gives"
gram0=$(st | awk -v p="$game" '$1 == p {print $4; exit}')
# The game visible and fullscreen on the other monitor, the idle apps hidden, the app in focus.
( { printf '%s f\n' "$BASHPID"; printf '%s v g\n' "$game"; printf '%s h\n' "${keep[@]}"; } >"$H"
  sleep 12; exec ./gbm-hog --force --scanout --size ${w}x8192 --max 2 --churn "$churn" --floor-mb 64 --hold 4 ) >"$T/hog.log" 2>&1 &
hog=$!
for _ in $(seq 900); do grep -q '^RESULT' "$T/hog.log" && break; kill -0 $hog 2>/dev/null || break; sleep 0.2; done
echo "  app: $(grep '^RESULT' "$T/hog.log" || tail -2 "$T/hog.log")"
grep -q "^$game v" "$H" || fail "the hints were overwritten (somebody logged in?): $(tr '\n' ' ' <"$H")"
echo "with it:  $(frag)"
gram=$(st | awk -v p="$game" '$1 == p {print $4; exit}')
klog | grep 'vidmem evict' >"$T/k.log"
clears=$(grep -c 'contiguous: clearing' "$T/k.log")
gout=$(grep -cE "evict pid $game: [1-9]" "$T/k.log")
gmove=$(grep -cE "move pid $game: [1-9]" "$T/k.log")
space=$(grep -c 'wanted to move others inside vidmem' "$T/k.log")
grep -E 'contiguous: |window (not )?cleared|inside vidmem failed|wanted to move others|room to move' "$T/k.log" | sed 's/^.*vidmem evict: /  /' | head -16
echo "  $churn remakes: $clears windows cleared; the game: moved inside vidmem $gmove times, out $gout times, ${gram:-0} MB in sysmem now (${gram0:-0} before); room made for its moves $space times"
echo "  game frames: $(grep '^T ' "$T/game.log" | awk '{for(i=1;i<=NF;i++) if($i ~ /^max_ms=/){split($i,a,"="); if(a[2]>m) m=a[2]}} END {print "worst " m+0 " ms"}'), $(grep '^T ' "$T/game.log" | tail -1)"
got=$(grep -o ' churned=[0-9]*' "$T/hog.log" | cut -d= -f2)
# With little free memory a refusal is the rule's answer (nobody else may give), not a failure.
denied=$(klog | grep -cE 'vidmem user reserve: denied|Failed to allocate NVKMS memory')
echo "  refusals of the app: $denied"
# Refused with nobody left to pay (the hidden apps draw, the game only moves, the app has none of its own):
# the rule's answer. Stopped otherwise: a failure.
[[ ${got:-0} -ge $churn ]] || (( every > 4 )) || (( denied > 0 )) || fail "the app made ${got:-0} of $churn remakes ($(grep -o 'stop=[a-z-]*' "$T/hog.log"))"
(( clears >= 1 )) || fail "no window was cleared (test setup: vidmem not fragmented?)"
if (( gives == 0 )); then
    (( gout == 0 )) || fail "the game's memory was moved out $gout times for the focused app"
    (( ${gram:-0} == 0 )) || fail "${gram} MB of the game in sysmem"
fi
wait $hog 2>/dev/null; hog=
: >"$H"
sleep 3
kill -INT "$game"; wait "$game"; rc=$?
(( rc == 0 )) || fail "game exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/game.log")"
echo "  game: $(grep '^RESULT' "$T/game.log")"
game=
for p in "${keep[@]}"; do kill -INT "$p"; done
for i in $(seq 0 15); do
    (( i % every == 3 )) && continue
    wait "${pids[i]}"; rc=$?
    (( rc == 0 )) || fail "app $i exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/p$i.log")"
done
pids=()
klog | grep -E 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|not remade|move copy failed|[Aa]ssert' && fail "kernel log"
(( fails == 0 )) && echo "CONTIGGAMETEST PASS" || echo "CONTIGGAMETEST FAIL $fails"
