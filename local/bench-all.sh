#!/bin/bash
# Runs the whole bench in the background under watchdog.sh: log /tmp/nvtest-all.log,
# ends with BENCH_DONE. A scenario that writes no notes for 8 minutes is killed
# (WATCHDOG STALL in the log) and its test session cleaned up.
#   NVTEST_FORCE=1 ./bench-all.sh     (needed in the test VM guest: always logged in)
cd "$(dirname "$0")" || exit 1
./watchdog.sh --stall "${BENCH_STALL:-480}" --max "${BENCH_MAX:-5400}" \
    --watch stress-logs --ignore dmon.log --on-kill "./nvt-session.sh stop" \
    -- ./nvtest.sh "${@:-all}" > /tmp/nvtest-all.log 2>&1
echo "BENCH_DONE rc=$?" >> /tmp/nvtest-all.log
