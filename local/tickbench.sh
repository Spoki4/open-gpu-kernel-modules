#!/bin/bash
# Policy step time (vidmem_tick us) with two test Chromes and 13 tabs (BACKLOG #1).
# In the test VM guest: local/vm/vm.sh ssh 'PROC=1 bash ~/projects/open-gpu-kernel-modules/local/tickbench.sh'
# (PROC=1: per-process tracing on as well, as gamerec records). Prints median/p90/max.
cd ~/projects/open-gpu-kernel-modules/local; HERE=$PWD; S=./nvt-session.sh
T=/sys/kernel/tracing
$S stop >/dev/null 2>&1
$S start "file://$HERE/nvt-webgl.html?mode=anim&tex=8" >/dev/null 2>&1
sleep 4
for i in $(seq 1 12); do curl -s -X PUT "http://127.0.0.1:9333/json/new?file://$HERE/nvt-webgl.html?mode=idle%26every=30%26tex=4" >/dev/null; sleep 0.3; done
NVT_INSTANCE=b $S start "file://$HERE/nvt-webgl.html?mode=anim&tex=8" >/dev/null 2>&1
sleep 10
echo 1 | sudo tee $T/events/nvidia_vidmem/vidmem_tick/enable >/dev/null
[[ ${PROC:-0} == 1 ]] && echo 1 | sudo tee $T/events/nvidia_vidmem/vidmem_proc/enable >/dev/null
sudo timeout 40 cat $T/trace_pipe > /tmp/ticks.txt
echo 0 | sudo tee $T/events/nvidia_vidmem/vidmem_tick/enable >/dev/null
echo 0 | sudo tee $T/events/nvidia_vidmem/vidmem_proc/enable >/dev/null
sudo -n ./evict.sh status | head -3 | tail -1
grep -o " us=[0-9]*" /tmp/ticks.txt | cut -d= -f2 | sort -n | awk '{a[NR]=$1} END {print "steps", NR, "median_us", a[int((NR+1)/2)], "p90_us", a[int(NR*0.9)], "max_us", a[NR]}'
grep -o " procs=[0-9]*" /tmp/ticks.txt | tail -1
$S stop >/dev/null 2>&1
