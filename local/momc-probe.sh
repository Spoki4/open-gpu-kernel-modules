#!/bin/bash
# BACKLOG I5, a probe: do the GPU's access counters tell which memory is in
# use while it still is in vidmem? (Needed to move cold memory out first; the
# counters in use now, MIMC, speak only of what already is in sysmem.)
# An app reads 512 MB of its own vidmem every frame, nothing is evicted.
# 15 s with the usual counters (`evict.sh hot on`), then 15 s with the vidmem
# counters too (`hot momc`); the notifications of each phase are compared.
# The answer is the result, either way: ends with
#   MOMCPROBE PASS vidmem-accesses-reported=yes|no ...
# and FAIL only when the counters cannot be had, the app broke or the kernel
# log has errors. Run in the test VM guest.
#   ./momc-probe.sh [SECONDS=15]
cd "$(dirname "$0")" || exit 1
dur=${1:-15}
T=$(mktemp -d /tmp/momc-probe.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ./evict.sh status; }
hot() { st | grep '^hot memory'; }
num() { hot | grep -o "$1 [0-9]*" | grep -o '[0-9]*$'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
app=
trap 'sudo -n ./evict.sh hot off >/dev/null; kill ${app:-} 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
./nvt-bg-a --mb 512 --chunk-mb 16 --fps 30 --floor-mb 512 >"$T/app.log" 2>&1 &
app=$!
for _ in $(seq 150); do grep -q '^START' "$T/app.log" && break; sleep 0.2; done
grep -q '^START' "$T/app.log" || fail "the app did not start: $(tail -1 "$T/app.log")"
phase() { # NAME KNOB
    sudo -n ./evict.sh hot off >/dev/null
    sudo -n ./evict.sh hot "$2" >/dev/null
    sleep "$dur"
    echo "  $1: $(hot)"
    hot | grep -q 'unavailable' && fail "access counters unavailable ($1)"
    hot | grep -q 'not started' && fail "the policy did not start the counters in $dur s ($1)"
}
phase "usual counters" on
n1=$(num notifications); e1=$(num 'outside evicted allocations')
phase "vidmem counters too" momc
n2=$(num notifications); e2=$(num 'outside evicted allocations'); c2=$(num 'type cpu'); v2=$(num 'aperture vidmem')
ram=$(st | awk -v p="$app" '$1 == p {print $4; exit}')
(( ${ram:-0} == 0 )) || echo "  note: $ram MB of the app in sysmem, its accesses count as the usual kind"
sudo -n ./evict.sh hot off >/dev/null
kill -INT $app; wait $app; rc=$?; app=
(( rc == 0 )) || fail "app exit $rc: $(grep -m1 -E '^ERROR|^RESULT' "$T/app.log")"
echo "  app: $(grep '^RESULT' "$T/app.log" | grep -o 'errors=[0-9]*')"
klog | grep 'hot memory:' | sed 's/^.*vidmem evict: /  /' | tail -12
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert' | tail -5
klog | grep -qE 'Xid|BUG:|Oops|general protection|[Aa]ssert' && fail "kernel log"
# "Reported": the app's 256 two-megabyte regions read 30 times a second give
# far more than a handful; without the vidmem counters next to nothing comes.
if (( ${e2:-0} >= 32 && ${e2:-0} > 4 * ${e1:-0} )); then rep=yes; else rep=no; fi
(( fails == 0 )) && echo "MOMCPROBE PASS vidmem-accesses-reported=$rep (in ${dur} s: usual $n1 notifications, $e1 in resident memory; with vidmem counters $n2, $e2 in resident memory, type cpu $c2, aperture vidmem $v2)" \
    || echo "MOMCPROBE FAIL $fails"
