#!/bin/bash
# When the window hints go away, what the policy moved out comes back. With
# the default auto mode (hints only) the policy stopped altogether without
# hints, so the memory of a hidden app stayed in sysmem for as long as it
# lived, and a hint writer that stopped (logout, its script turned off) left
# a stale table that kept the policy acting on windows nobody reported. Now a
# snapshot without lines turns the hints off and the policy without hints
# brings the memory back as room allows (local/HINTS.md).
# Setup: app A, hidden by the hints, is moved to sysmem; with the hints on it
# stays there (a hidden app comes back only after minutes of room). A does
# nothing on the GPU until it ends (it checks its content then): a hidden app
# that draws comes back by itself. Then an empty snapshot is written.
# Checks: the table reads empty, A comes back within 45 s (the policy's quiet
# time is 30 s), its content stays right, the kernel log stays clean. Window
# hints written by hand: run with nobody logged in. Ends with
# HINTSOFFTEST PASS|FAIL.
#   ./hintsoff-test.sh
cd "$(dirname "$0")" || exit 1
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/hintsoff-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
a=
trap 'kill $a 2>/dev/null; wait 2>/dev/null; echo >"$H" 2>/dev/null; rm -rf "$T"' EXIT

( printf '%s h\n' "$BASHPID" >"$H"
  exec ./nvt-idle-a --mb 768 --chunk-mb 32 --idle --verify-s 0 --floor-mb 0 ) >"$T/a.log" 2>&1 &
a=$!
wait_start "$T/a.log"
echo "  A: $(grep '^START' "$T/a.log" | cut -c1-80)"
sudo -n ../evict.sh evict "$a" | grep -E 'summary' | sed 's/^/  /'
sleep 12
ram=$(col "$a" 4)
echo "  with the hints on, 12 s later: ${ram:-?} MB of A in sysmem"
[[ ${ram:-0} -ge 600 ]] || fail "A did not stay in sysmem while hidden (${ram:-?} MB there)"

echo >"$H"
grep -q '0 entries' "$H" || fail "the table is not empty: $(head -1 "$H")"
back=
for i in $(seq 45); do
    sleep 1
    ram=$(col "$a" 4)
    [[ ${ram:-1} -eq 0 ]] && { back=$i; break; }
done
[[ -n $back ]] && echo "  hints off: A back in vidmem after $back s" ||
    fail "A still has ${ram:-?} MB in sysmem 45 s after the hints went off"
klog | grep 'vidmem evict: auto' | tail -2 | sed 's/^/  /'
kill -INT $a 2>/dev/null; wait $a; rc=$?; a=
(( rc == 0 )) || fail "A exited with $rc: $(grep -m1 -E '^ERROR|^FATAL' "$T/a.log")"
grep -q '^RESULT.* errors=0' "$T/a.log" || fail "A read back wrong data: $(grep '^RESULT' "$T/a.log")"
echo "  A: $(grep -o 'verifies=[0-9]* errors=[0-9]*' "$T/a.log")"
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|Assertion failed' && fail "kernel log"
(( fails == 0 )) && echo "HINTSOFFTEST PASS" || echo "HINTSOFFTEST FAIL $fails"
