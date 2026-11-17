#!/bin/bash
# Allocations of a hidden idle app that vidmem turns away are created in sysmem
# instead (vidmemEvictBornAllocation()): placed in vidmem for the moment it
# takes to move them, anywhere, the reserve block included. That "anywhere"
# was a PMA range from address 0, below the region's base, which is not 2 MB
# aligned: regmap.c asserted on each 2 MB-page one (host, recording
# 20261116_184241, 20:20:13, ghostty's buffers). Setup: a process nobody may
# move (named like the login screen) leaves little free vidmem; app C, hidden
# and idle for 40 s (the driver does this only for idle ones, 30 s after they
# started at the earliest; ghostty on the
# host had been idle for 4 minutes), then allocates buffers past it.
# Checks: some of C's memory was created in sysmem, its content stays right,
# no assertion and nothing else in the kernel log. Window hints written by
# hand: run with nobody logged in. Ends with BORNTEST PASS|FAIL.
#   ./born-test.sh
cd "$(dirname "$0")" || exit 1
H=/proc/driver/nvidia/vidmem_hints
T=$(mktemp -d /tmp/born-test.XXXX)
T0=$(date '+%F %T')
CMB=768 LEFT=600         # C fits up to the background cushion (256 MB free), the rest is made in sysmem
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
free_mb() { st | sed -n 's/.*free outside reserve \([0-9]*\) MB.*/\1/p'; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
hog= c=
trap 'kill $hog $c 2>/dev/null; wait 2>/dev/null; : >"$H" 2>/dev/null; rm -rf "$T"' EXIT
ln -sf nvt-game ./sddm-greeter
: >"$H"
free=$(free_mb)
[[ ${free:-0} -gt $((CMB + LEFT + 512)) ]] ||
    { echo "  FAIL: only ${free:-?} MB free at the start"; echo "BORNTEST FAIL 1"; exit 0; }
./sddm-greeter --mb $((free - LEFT)) --chunk-mb 256 --idle --floor-mb 0 >"$T/hog.log" 2>&1 &
hog=$!
wait_start "$T/hog.log"
echo "pinned: $(grep -o 'allocated_mb=[0-9]*' "$T/hog.log"); free now $(free_mb) MB"

( printf '%s h\n' "$BASHPID" >"$H"
  exec ./nvt-idle-a --mb $CMB --chunk-mb 32 --idle --verify-s 2 --floor-mb 0 --wait-s 40 ) >"$T/c.log" 2>&1 &
c=$!
sleep 40
wait_start "$T/c.log"
sleep 6     # a few content checks with part of it in sysmem
grep -q "^$c h" "$H" || fail "the hints were overwritten (somebody logged in on the machine?)"
ram=$(col "$c" 4)
born=$(klog | grep -c "created in sysmem instead of denied: nvt-idle-a($c)")
echo "  C: $(grep '^START' "$T/c.log" | cut -c1-100); in sysmem ${ram:-?} MB; born-in-sysmem lines $born"
(( born > 0 )) || fail "nothing of C was created in sysmem (the setup left room?)"
kill -INT $c 2>/dev/null; wait $c; rc=$?; c=
(( rc == 0 )) || fail "C exited with $rc: $(grep -m1 -E '^ERROR|^FATAL|^RESULT' "$T/c.log")"
grep -q '^RESULT.* errors=0' "$T/c.log" || fail "C read back wrong data: $(grep '^RESULT' "$T/c.log")"
echo "  C: $(grep -o 'verifies=[0-9]* errors=[0-9]*' "$T/c.log")"
: >"$H"
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|Assertion failed' && fail "kernel log"
(( fails == 0 )) && echo "BORNTEST PASS" || echo "BORNTEST FAIL $fails"
