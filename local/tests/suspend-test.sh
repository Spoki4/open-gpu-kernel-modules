#!/bin/bash
# Memory moved to sysmem survives a suspend and resume of the driver (idea
# I11, not verified before). On suspend RM saves what PMA holds in vidmem
# (NVreg_PreserveVideoMemoryAllocations, on by default with the kernel's
# suspend notifiers) and restores it on resume; evicted memory is no longer in
# PMA, its copy lives in sysmem pages RM leaves alone, its GPU mappings point
# at them, and its descriptor still has the stale vidmem page array. A laptop
# whose GPU powers off at runtime goes through the same save (GC-off: every
# allocated PMA block).
# Setup: A (idle, checks its content every 2 s) is moved to sysmem by hand, B
# (drawing, checks its content) stays in vidmem. The driver is suspended and
# resumed through /proc/driver/nvidia/suspend, as nvidia-sleep.sh does around
# a system suspend; the machine itself does not sleep.
# Checks: the suspend and the resume succeed, A is still in sysmem after the
# resume and comes back to vidmem by hand, both keep their content throughout
# and exit cleanly, the kernel log stays clean. No desktop on the GPU (the
# compositor would be suspended too). The file exists only with the modules
# loaded with NVreg_UseKernelSuspendNotifiers=0 (620 defaults to 1: the
# kernel's notifiers call the same save and restore, at a real system
# suspend only); without it the test says SKIP. Ends with SUSPENDTEST
# PASS|FAIL|SKIP.
#   ./suspend-test.sh [suspend|hibernate] [CYCLES=3]
cd "$(dirname "$0")" || exit 1
how=${1:-suspend} cycles=${2:-3}
P=/proc/driver/nvidia/suspend
T=$(mktemp -d /tmp/suspend-test.XXXX)
T0=$(date '+%F %T')
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
st() { sudo -n ../evict.sh status; }
col() { st | awk -v p="$1" -v c="$2" '$1 == p {print $c; exit}'; }
klog() { sudo journalctl -k --since "$T0" --no-pager; }
wait_start() { local i; for i in $(seq 150); do grep -q '^START\|^FATAL\|^ERROR' "$1" && return; sleep 0.2; done; }
pm() { echo "$1" | sudo -n tee "$P" >/dev/null; }
[[ -e $P ]] || { echo "SUSPENDTEST SKIP no $P: load the modules with NVreg_UseKernelSuspendNotifiers=0"; exit 0; }
a= b=
# The policy off meanwhile: without window hints it would bring A back by itself.
mode=$(st | sed -n 's/^auto \(on\|off\|with window hints only\),.*/\1/p' | head -1)
case $mode in on) mode=on;; off) mode=off;; *) mode=hints;; esac
sudo -n ../evict.sh auto off >/dev/null
trap 'pm resume 2>/dev/null; kill $a $b 2>/dev/null; wait 2>/dev/null; sudo -n ../evict.sh auto "$mode" >/dev/null; rm -rf "$T"' EXIT
grep -E 'PreserveVideoMemoryAllocations|TemporaryFilePath' /proc/driver/nvidia/params | sed 's/^/  /'

./nvt-idle-a --mb 512 --chunk-mb 32 --idle --verify-s 2 --floor-mb 0 >"$T/a.log" 2>&1 &
a=$!
./nvt-bg-a --mb 512 --chunk-mb 32 --verify-s 2 --floor-mb 0 >"$T/b.log" 2>&1 &
b=$!
wait_start "$T/a.log"; wait_start "$T/b.log"
echo "  A: $(grep '^START' "$T/a.log" | cut -c1-80)"
echo "  B: $(grep '^START' "$T/b.log" | cut -c1-80)"
sudo -n ../evict.sh evict "$a" | grep -E 'summary' | sed 's/^/  /'
ram=$(col "$a" 4)
[[ ${ram:-0} -ge 400 ]] || fail "A was not moved to sysmem (${ram:-?} MB there)"
sleep 3

for c in $(seq "$cycles"); do
    t=$(date +%s.%N)
    pm "$how" || fail "cycle $c: $how was refused"
    sleep 2
    pm resume || fail "cycle $c: resume was refused"
    echo "  cycle $c: $how + resume in $(awk -v t="$t" -v n="$(date +%s.%N)" 'BEGIN {printf "%.1f", n - t}') s"
    sleep 5     # a few content checks after the resume
    kill -0 $a 2>/dev/null || fail "cycle $c: A is gone: $(tail -1 "$T/a.log")"
    kill -0 $b 2>/dev/null || fail "cycle $c: B is gone: $(tail -1 "$T/b.log")"
done
ram=$(col "$a" 4)
echo "  A after the resumes: ${ram:-?} MB in sysmem"
[[ ${ram:-0} -ge 400 ]] || fail "A is no longer in sysmem after the resume (${ram:-?} MB): who brought it back?"
sudo -n ../evict.sh restore "$a" | grep -E 'summary' | sed 's/^/  /'
ram=$(col "$a" 4)
[[ ${ram:-1} -eq 0 ]] || fail "A did not come back to vidmem (${ram:-?} MB still in sysmem)"
sleep 5
for p in a b; do
    pid=${!p}
    kill -INT "$pid" 2>/dev/null; wait "$pid"; rc=$?
    grep -q '^RESULT.* errors=0' "$T/$p.log" || fail "${p^^} read back wrong data: $(grep '^RESULT' "$T/$p.log")"
    (( rc == 0 )) || fail "${p^^} exited with $rc: $(grep -m1 -E '^ERROR|^FATAL' "$T/$p.log")"
    echo "  ${p^^}: $(grep -o 'verifies=[0-9]* errors=[0-9]*' "$T/$p.log")"
done
a= b=
klog | grep -E 'Xid|BUG:|Oops|general protection|[Aa]ssert|NVRM: .*(fail|error)' | grep -v 'vidmem evict' | tail -8
klog | grep -qE 'Xid|BUG:|Oops|general protection|Assertion failed' && fail "kernel log"
(( fails == 0 )) && echo "SUSPENDTEST PASS" || echo "SUSPENDTEST FAIL $fails"
