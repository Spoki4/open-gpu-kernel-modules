#!/bin/bash
# host 2026-11-12 00:42 (sim/scenarios/switch-drawing.scn): the window the
# user switches to gets its headroom from the app it has just covered, and
# that app, still drawing, must not come back two seconds later to go out
# again. A process nobody may move (named like the login screen) fills vidmem
# down to 1.5 GB; app B (1.6 GB) draws as a visible window; then app A (not a
# game: 800 MB, no fullscreen window) starts focused and B's hint turns hidden
# in the same write. Checks: B gave memory for A's headroom; for 24 s none of
# it comes back although B keeps drawing, and the driver's own count of moves
# undone within a minute (`evict.sh status`, "bounces:") does not grow;
# focused again, B is back within seconds; contents intact. Window hints
# written by hand: run with nobody logged in on the machine.
# Ends with BOUNCETEST PASS|FAIL.
#   ./bounce-test.sh
cd "$(dirname "$0")" || exit 1
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/bounce-test.XXXX)
T0=$(date '+%F %T')
BMB=1600 AMB=800 LEFT=1500
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
free_mb() { st | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p'; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
bounces() { st | sed -n 's/^bounces: \([0-9]*\) moves.* \([0-9]*\) MB copied.*/\1 \2/p'; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
hog= a= b=
trap 'kill $hog $a $b 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
[[ -n $(bounces) ]] || { echo "  FAIL: this driver has no bounce count in its status"; echo "BOUNCETEST FAIL 1"; exit 0; }
ln -sf nvt-game ./sddm-greeter
free=$(free_mb)
[[ ${free:-0} -gt $((BMB + LEFT + 512)) ]] ||
    { echo "  FAIL: only ${free:-?} MB free at the start"; echo "BOUNCETEST FAIL 1"; exit 0; }
./sddm-greeter --mb $((free - BMB - LEFT)) --chunk-mb 256 --idle --floor-mb 0 >"$T/hog.log" 2>&1 &
hog=$!
wait_start "$T/hog.log"
: >"$H"
./nvt-bg-a --mb $BMB --chunk-mb 32 --fps 30 --read-mb 64 --gfx --floor-mb 0 >"$T/b.log" 2>&1 &
b=$!
wait_start "$T/b.log"
printf '%s v\n' "$b" >"$H"
echo "pinned: $(grep -o 'allocated_mb=[0-9]*' "$T/hog.log"); B: $(grep -o 'allocated_mb=[0-9]*' "$T/b.log"); free now $(free_mb) MB"
read -r n0 mb0 <<<"$(bounces)"

# A starts focused over B. B is taken only in the moment after it was covered
# (later it counts as drawing while hidden and is left alone), so A must ask
# within that moment: up to three tries.
got=0
for try in 1 2 3; do
    sleep 12    # B draws steadily as a visible window
    ( printf '%s h\n%s f\n' "$b" "$BASHPID" >"$H"
      exec ./nvt-idle-a --mb $AMB --chunk-mb 32 --idle --verify-s 2 --floor-mb 0 ) >"$T/a.log" 2>&1 &
    a=$!
    wait_start "$T/a.log"
    sleep 1
    out=$(col "$b" 4)
    echo "  try $try: A $(grep -o 'allocated_mb=[0-9]*' "$T/a.log"), in sysmem $(col "$a" 4) MB; B in sysmem ${out:-?} MB; free $(free_mb) MB"
    if [[ ${out:-0} -ge 200 ]]; then got=1; break; fi
    kill -INT $a 2>/dev/null; wait $a 2>/dev/null; a=
    printf '%s v\n' "$b" >"$H"
done
if (( !got )); then
    fail "B gave nothing for A's headroom in three tries: the case did not arise (free memory, timing?)"
else
    [[ $(grep -o 'allocated_mb=[0-9]*' "$T/a.log" | cut -d= -f2) -ge $AMB ]] || fail "the focused app did not get its $AMB MB"
    [[ $(col "$a" 4) -eq 0 ]] || fail "the focused app has memory in sysmem"
    grep -q "^$a f" "$H" || fail "the hints were overwritten (somebody logged in?)"
    # B keeps drawing, hidden: nothing of it comes back while A is the focus.
    max=$out min=$out
    for i in $(seq 12); do
        sleep 2
        r=$(col "$b" 4); r=${r:-0}
        (( r > max )) && max=$r
        (( r < min )) && min=$r
    done
    read -r n1 mb1 <<<"$(bounces)"
    echo "  24 s later: B in sysmem $min..$max MB, now $(col "$b" 4); bounces $n0 -> $n1 ($((mb1 - mb0)) MB copied for nothing); B $(grep '^T ' "$T/b.log" | tail -1 | cut -c1-70)"
    (( min + 64 >= max )) || fail "B's memory came back while it was hidden ($max -> $min MB in sysmem)"
    (( n1 == n0 )) || fail "$((n1 - n0)) moves were undone within a minute ($((mb1 - mb0)) MB copied for nothing)"
    sudo journalctl -k --since "$T0" --no-pager | grep -o "hints: restoring nvt-bg-a.*" | head -3 | sed 's/^/  /'

    # The user looks at B again: all of it back at once.
    printf '%s f\n%s v\n' "$b" "$a" >"$H"
    for i in $(seq 20); do sleep 0.5; r=$(col "$b" 4); [[ ${r:-1} -le 16 ]] && break; done
    echo "  B focused again: in sysmem ${r:-?} MB after $((i * 500)) ms"
    [[ ${r:-1} -le 16 ]] || fail "B, focused, still has $r MB in sysmem after 10 s"
fi
for p in a b; do
    pid=${!p}; [[ -n $pid ]] || continue
    kill -INT "$pid" 2>/dev/null; wait "$pid"; rc=$?
    (( rc == 0 )) || fail "$p exited with $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/$p.log")"
    grep -q '^RESULT.* errors=0' "$T/$p.log" || fail "$p read back wrong data: $(grep '^RESULT' "$T/$p.log")"
done
a= b=
: >"$H"
st | grep '^bounces' | sed 's/^/  /'
sudo journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
sudo journalctl -k --since "$T0" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection' && fail "kernel log"
(( fails == 0 )) && echo "BOUNCETEST PASS" || echo "BOUNCETEST FAIL $fails"
