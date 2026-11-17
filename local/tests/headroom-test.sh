#!/bin/bash
# The headroom of a game in focus is refilled in pieces with a pause between
# them. On the host (recording 20261116_184241, 19:20:23) the refill after an
# alt-tab back to WoW moved eleven pieces of hidden Chrome, 6-12 ms each,
# back to back in 100 ms: the work item took the API lock again before the
# threads waiting for it, and the game's RM calls waited 21-38 ms. Now each
# piece is followed by a pause of twice its time (16-50 ms) while a game is in
# focus (vepHeadroomGapMs()).
# Setup: a process nobody may move (named like the login screen) fills
# vidmem, app B (hidden, idle) holds 1.5 GB, a "game" (nvt-game drawing,
# started under a process named "reaper" as Steam starts its games, so it is a
# game at once; the focus is that launcher's window) allocates with little
# free memory left (after a policy step has seen it in focus, as a game that
# streams), so the driver refills its headroom from B.
# Checks: the refill ran in several pieces, the pauses between them (from
# the end of a move to the next piece; the median, kernel log timestamps, so
# 14 ms) are at least the 16 ms of the rule, B's content and the game's stay
# right, the kernel log stays clean. Window hints written by hand: run with nobody
# logged in on the machine. Ends with HEADROOMTEST PASS|FAIL.
#   ./headroom-test.sh
cd "$(dirname "$0")" || exit 1
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/headroom-test.XXXX)
T0=$(date '+%F %T')
BMB=1500 GMB=512 LEFT=800
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
free_mb() { st | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p'; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
klog() { sudo journalctl -k --since "$T0" --no-pager -o short-precise; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
hog= b= g= gl=
trap 'kill $hog $b $g $gl 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
ln -sf nvt-game ./sddm-greeter
ln -sf "$(command -v bash)" "$T/reaper"
: >"$H"
free=$(free_mb)
[[ ${free:-0} -gt $((BMB + LEFT + 1024)) ]] ||
    { echo "  FAIL: only ${free:-?} MB free at the start"; echo "HEADROOMTEST FAIL 1"; exit 0; }

./nvt-idle-b --mb $BMB --chunk-mb 32 --idle --floor-mb 0 >"$T/b.log" 2>&1 &    # checked at the end only: work every 2 s would make it "drawing"
b=$!
wait_start "$T/b.log"
printf '%s h\n' "$b" >"$H"
free=$(free_mb)
./sddm-greeter --mb $((free - LEFT)) --chunk-mb 256 --idle --floor-mb 0 >"$T/hog.log" 2>&1 &
hog=$!
wait_start "$T/hog.log"
echo "pinned: $(grep -o 'allocated_mb=[0-9]*' "$T/hog.log"); B: $(grep -o 'allocated_mb=[0-9]*' "$T/b.log"); free now $(free_mb) MB"
sleep 32    # B idle, and past the 30 s a new process is spared: the headroom takes only such ones

T1=$(date '+%F %T')
( printf '%s h\n%s f g\n' "$b" "$BASHPID" >"$H"
  exec "$T/reaper" -c "./nvt-game --mb $GMB --chunk-mb 64 --gfx --fps 60 --verify-s 2 --floor-mb 0 --wait-s 5; exit \$?" ) >"$T/g.log" 2>&1 &
gl=$!
wait_start "$T/g.log"
g=$(pgrep -P "$gl" nvt-game)
sleep 10
grep -q "^$gl f g" "$H" || fail "the hints were overwritten (somebody logged in on the machine?)"
echo "  game: $(grep '^START' "$T/g.log" | cut -c1-100); B in sysmem $(col "$b" 4) MB; free $(free_mb) MB"

# The pieces: each "foreground headroom" line starts one, the move after it
# ends it; the pause is from that end to the next start.
sudo journalctl -k --since "$T1" --no-pager -o short-precise |
    grep -E 'hints: foreground headroom|vidmem evict: evict pid '"$b"':' >"$T/pieces"
read -r n med mn mx < <(python3 - "$T/pieces" <<'EOF2'
import sys, datetime
starts, ends = [], []
for l in open(sys.argv[1]):
    t = datetime.datetime.strptime(l.split()[2], "%H:%M:%S.%f")
    (starts if "foreground headroom" in l else ends).append(t)
pauses = []
for e in ends:
    nxt = [s for s in starts if s >= e]
    if nxt and (nxt[0] - e).total_seconds() < 1:
        pauses.append((nxt[0] - e).total_seconds() * 1000)
pauses.sort()
med = pauses[len(pauses) // 2] if pauses else 0
print(len(starts), round(med, 1), round(pauses[0], 1) if pauses else 0, round(pauses[-1], 1) if pauses else 0)
EOF2
)
echo "  refill: $n pieces; pauses between them (ms): median $med, min $mn, max $mx"
grep 'foreground headroom' "$T/pieces" | head -3 | sed 's/^.*hints: /    /' | cut -c1-140
(( n >= 4 )) || fail "the refill ran in $n pieces (the setup did not make one?)"
awk -v m="$med" 'BEGIN { exit !(m >= 14) }' || fail "pauses of $med ms between the pieces (median), wanted >= 16"

grep -q '^T ' "$T/g.log" && echo "  game frames: $(grep '^T ' "$T/g.log" | awk '{for(i=1;i<=NF;i++) if($i ~ /^max_ms=/){split($i,a,"="); if(a[2]>m) m=a[2]}} END{print "worst " m " ms a second"}')"
kill -INT $g 2>/dev/null; wait $gl; rc=$?; g= gl=
(( rc == 0 )) || fail "the game exited with $rc: $(grep -m1 -E '^ERROR|^FATAL|^RESULT' "$T/g.log")"
grep -q '^RESULT.* errors=0' "$T/g.log" || fail "the game read back wrong data: $(grep '^RESULT' "$T/g.log")"
kill -INT $b 2>/dev/null; wait $b; rc=$?; b=
(( rc == 0 )) || fail "B exited with $rc: $(grep -m1 -E '^ERROR|^FATAL|^RESULT' "$T/b.log")"
grep -q '^RESULT.* errors=0' "$T/b.log" || fail "B read back wrong data: $(grep '^RESULT' "$T/b.log")"
echo "  B: $(grep -o 'verifies=[0-9]* errors=[0-9]*' "$T/b.log")"
: >"$H"
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|[Aa]ssertion failed' && fail "kernel log"
(( fails == 0 )) && echo "HEADROOMTEST PASS" || echo "HEADROOMTEST FAIL $fails"
