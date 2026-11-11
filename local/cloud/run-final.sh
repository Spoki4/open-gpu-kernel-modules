#!/bin/bash
cd ~/open-gpu-kernel-modules/local
exec >~/final.log 2>&1
export NVT_CHROME_FLAGS="--enable-unsafe-webgpu --enable-features=Vulkan" NVTEST_FORCE=1
T0=$(date "+%F %T")
echo "build: $(head -1 /proc/driver/nvidia/version | grep -o "Wed.*")"
./watchdog.sh --stall 480 --max 5000 --watch stress-logs --ignore dmon.log --on-kill "./nvt-session.sh stop" -- ./nvtest.sh all
./nvt-session.sh stop >/dev/null 2>&1
for t in "move-test.sh" "contig-test.sh" "contigloop-test.sh" "compact-test.sh 120 4096" "compact-test.sh 120 4096 fg" "fgown-test.sh"; do
    echo "== $t $(date +%T)"; timeout 900 ./$t 2>&1 | tail -12
    if journalctl -k --since "$T0" --no-pager | grep -qE "Xid|BUG:|Oops|general protection"; then echo "FINAL STOP kernel errors after $t"; break; fi
done
echo "kernel:"; journalctl -k --since "$T0" --no-pager | grep -E "Xid|BUG:|Oops|general protection|kernel panic|can.t update VA|[Aa]ssert" | tail -20
echo FINALRUN DONE
