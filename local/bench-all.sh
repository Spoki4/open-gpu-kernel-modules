#!/bin/bash
# Runs the whole bench in the background: log /tmp/nvtest-all.log, ends with BENCH_DONE.
cd "$(dirname "$0")" || exit 1
./nvtest.sh all > /tmp/nvtest-all.log 2>&1
echo "BENCH_DONE rc=$?" >> /tmp/nvtest-all.log
