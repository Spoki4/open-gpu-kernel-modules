#!/bin/bash
# problem #7: the focused window gets its allocation even when nobody is left
# to give; as much of its own older memory goes to sysmem instead (it was a
# denial: a WebGPU page focused next to a game died). A process nobody may
# move (named like the login screen) fills vidmem down to LEFT MB; a focused
# app (not a game: under 1 GB, no fullscreen window) then asks for twice that
# in 32 MB buffers. Checks: it gets all of it, part of it sits in sysmem, the
# content is intact, the pinned process is untouched; with `evict.sh fgown
# off` the same app is turned away (so the test tests something). Window
# hints written by hand: run with nobody logged in on the guest.
# Ends with FGOWNTEST PASS|FAIL.
#   ./fgown-test.sh [LEFT_MB=320]
cd "$(dirname "$0")" || exit 1
left=${1:-320}
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/fgown-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
free_mb() { st | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p'; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
hog= app=
trap 'sudo -n ../evict.sh fgown on >/dev/null; kill $hog $app 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
ln -sf nvt-game ./sddm-greeter
free=$(free_mb)
[[ ${free:-0} -gt $((left + 512)) ]] || { echo "  FAIL: only ${free:-?} MB free at the start"; echo "FGOWNTEST FAIL 1"; exit 0; }
./sddm-greeter --mb $((free - left)) --chunk-mb 256 --idle --floor-mb 0 >"$T/hog.log" 2>&1 &
hog=$!
wait_start "$T/hog.log"
echo "pinned: $(grep -o 'allocated_mb=[0-9]*' "$T/hog.log"), free now $(free_mb) MB"
run_app() { # LOG
    # Only the app is the foreground (not this shell: the pinned process is its child too).
    # The hint is there before its first allocation: written by the shell that becomes the app.
    ( printf '%s f\n' "$BASHPID" >"$H"; exec ./nvt-idle-a --mb $((left * 2)) --chunk-mb 32 --idle --verify-s 2 --floor-mb 0 ) >"$1" 2>&1 &
    app=$!
    wait_start "$1"
}

# 1. Off: the old behaviour, a denial (the app stops short of what it wanted).
sudo -n ../evict.sh fgown off >/dev/null
run_app "$T/off.log"
got_off=$(grep -o 'allocated_mb=[0-9]*' "$T/off.log" | cut -d= -f2)
echo "  fgown off: $(grep '^START' "$T/off.log" | cut -c1-110 || tail -1 "$T/off.log"); in sysmem $(col "$app" 4) MB"
kill -INT $app 2>/dev/null; wait $app 2>/dev/null; app=
sleep 2

# 2. On: all of it, part in sysmem.
sudo -n ../evict.sh fgown on >/dev/null
run_app "$T/on.log"
got_on=$(grep -o 'allocated_mb=[0-9]*' "$T/on.log" | cut -d= -f2)
grep -q "^$app f" "$H" || fail "the hints were overwritten (somebody logged in on the guest?)"
sleep 6     # a few content checks with part of it in sysmem
ram=$(col "$app" 4); hogram=$(col "$hog" 4)
echo "  fgown on:  $(grep '^START' "$T/on.log" | cut -c1-110 || tail -1 "$T/on.log"); in sysmem ${ram:-?} MB; pinned in sysmem ${hogram:-?} MB"
sudo journalctl -k --since "$T0" --no-pager | grep -o "nobody left to give.*" | head -3 | sed 's/^/  /'
[[ ${got_on:-0} -ge $((left * 2)) ]] || fail "the focused app got ${got_on:-0} of $((left * 2)) MB"
[[ ${ram:-0} -ge $((left / 2)) ]] || fail "only ${ram:-0} MB of the app in sysmem (where did the rest fit?)"
[[ ${hogram:-0} -eq 0 ]] || fail "the pinned process was moved ($hogram MB)"
[[ ${got_off:-0} -lt $((left * 2)) ]] || echo "  note: with fgown off the app also got everything ($got_off MB): the setup left room, the comparison says nothing"
kill -INT $app 2>/dev/null; wait $app; rc=$?; app=
(( rc == 0 )) || fail "the app exited with $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/on.log")"
grep -q '^RESULT.* errors=0' "$T/on.log" || fail "the app read back wrong data: $(grep '^RESULT' "$T/on.log")"
echo "  app: $(grep -o 'verifies=[0-9]* errors=[0-9]*' "$T/on.log")"
: >"$H"
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection' && fail "kernel log"
(( fails == 0 )) && echo "FGOWNTEST PASS" || echo "FGOWNTEST FAIL $fails"
