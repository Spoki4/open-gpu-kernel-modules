#!/bin/bash
# One check of the working tree on the GPU: build the driver (on the host when
# it has the guest kernel's headers, else in the guest), load it in the test VM guest and run nvtest there.
# Two watchdogs: the guest's around nvtest (a stuck scenario is killed and its
# test session cleaned up), the host's around the ssh session. A silent ssh
# session is not a hung guest (2026-11-06 23:24: a test left its Chrome
# running, which kept ssh open; the hard reset that followed wedged the GPU
# until a host reboot): the host's watchdog first stops the test's processes
# over a new ssh session and resets the guest only if that does not answer. The guest must be running with the GPU
# (host in vm mode). Prints the NVTEST lines, ends with
#   VMTEST PASS|FAIL <why>      exit 0 on PASS
#
#   local/vm/vmtest.sh [--no-build] [--no-reload] [NVTEST ARGS...]   default: all
#
# Log: local/stress-logs/vmtest-<date_time>.log (nvtest's own logs next to it).
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD
build=1 reload=1
while [[ ${1:-} == --no-* ]]; do
    case $1 in
        --no-build) build=0 ;;
        --no-reload) reload=0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
args=("${@:-all}")
# One at a time: a second run would rebuild and reload the module under the first.
exec 9>/tmp/vmtest.lock
flock -n 9 || { echo "VMTEST FAIL another vmtest.sh is running" >&2; exit 1; }
mkdir -p local/stress-logs
log=local/stress-logs/vmtest-$(date +%F_%H%M%S).log
say() { echo "$*" | tee -a "$log"; }

if [[ $(local/vm/vm.sh status 2>/dev/null) != running:* ]]; then
    say "guest not running: starting it"
    local/vm/vm.sh start >>"$log" 2>&1 && timeout 300 local/vm/vm.sh wait >>"$log" 2>&1 ||
        { say "VMTEST FAIL guest did not start"; exit 1; }
fi
# Just started or reset by a watchdog: wait until it answers.
timeout 300 local/vm/vm.sh wait >>"$log" 2>&1 || { say "VMTEST FAIL guest does not answer ssh"; exit 1; }
if (( build )); then
    say "build: $(git log -1 --format='%h %s')$(git diff --quiet HEAD -- src kernel-open || echo ' + working tree')"
    # On the host when it has the guest kernel's headers (same kernel), else in
    # the guest (2026-11-10: host updated to 7.3.3, guest pinned at 7.3.1).
    gk=$(local/vm/vm.sh ssh 'uname -r' 2>/dev/null | tr -d '\r')
    if [[ -n $gk && -d /lib/modules/$gk/build ]]; then
        say "build on the host for $gk"
        make modules -j"$(nproc)" SYSSRC="/lib/modules/$gk/build" LD=ld.lld IGNORE_CC_MISMATCH=1 >>"$log" 2>&1
    else
        say "build in the guest (${gk:-?}; host has no headers for it)"
        timeout 1800 local/vm/vm.sh build >>"$log" 2>&1
    fi
    if (( $? != 0 )); then
        grep -E 'error|Error' "$log" | head -20
        say "VMTEST FAIL build"
        exit 1
    fi
fi

if (( reload )); then
    if ! timeout 180 local/vm/vm.sh reload tree 2>&1 | tee -a "$log" | grep '^now:'; then
        say "VMTEST FAIL reload (guest log: local/vm/vm.sh log)"
        exit 1
    fi
fi

say "nvtest ${args[*]}"
local/watchdog.sh --stall 600 --max 5400 --watch local/stress-logs --ignore dmon.log --ignore 'vmtest-*' \
    --on-kill "timeout 60 '$ROOT/local/vm/vm.sh' ssh 'cd $ROOT/local/tests && ./nvt-session.sh stop; pkill -f '\''[n]vt-'\'' ; true' \
               || '$ROOT/local/vm/vm.sh' reset" -- \
    local/vm/vm.sh ssh "T0=\$(date '+%F %T'); cd '$ROOT/local/tests' && NVTEST_FORCE=1 ../watchdog.sh --stall 480 --max 5000 \
        --watch ../stress-logs --ignore dmon.log --on-kill './nvt-session.sh stop' -- ./nvtest.sh ${args[*]}; \
        echo; echo 'kernel:'; sudo journalctl -k --since \"\$T0\" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|kernel panic|can.t update VA|[Aa]ssert|failed [1-9]|NV_ERR' | tail -20" \
    2>&1 | grep --line-buffered -v '^Pseudo-terminal' >>"$log"
rc=${PIPESTATUS[0]}

# nvtest all repeats its results under "== summary": count them once.
results=$(sed '/^== summary/,$d' "$log" | grep -E '^NVTEST ')
echo "$results"
grep -E '^WATCHDOG (STALL|TIMEOUT)' "$log"
sed -n '/^kernel:/,$p' "$log" | tail -21
fails=$(grep -c ' FAIL' <<<"$results")
passes=$(grep -c ' PASS' <<<"$results")
if grep -q '^WATCHDOG \(STALL\|TIMEOUT\)' "$log"; then
    say "VMTEST FAIL watchdog (stall or timeout, see $log)"
    exit 1
elif (( fails > 0 || passes == 0 || rc != 0 )); then
    say "VMTEST FAIL $fails failed, $passes passed (rc $rc), $log"
    exit 1
fi
say "VMTEST PASS $passes passed, $log"
