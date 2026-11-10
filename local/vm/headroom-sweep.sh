#!/bin/bash
# I2: Superposition apps-evict with different foreground headrooms, from the host
# (HEADROOMS="2048 1024 512" local/vm/headroom-sweep.sh; run it under local/watchdog.sh).
cd /home/spoki4/projects/open-gpu-kernel-modules || exit 1
for H in ${HEADROOMS:-2048 1024 512}; do
    echo "== headroom $H MB"
    local/vm/vm.sh ssh "echo 'options nvidia NVreg_RegistryDwords=\"RmVidmemUserReserveMB=0;RmVidmemFgHeadroomMB=$H\"' | sudo tee /etc/modprobe.d/nvidia-vidmem-reserve.conf >/dev/null" 2>/dev/null
    timeout 180 local/vm/vm.sh reload tree 2>&1 | grep '^now' || { echo "reload failed"; break; }
    sleep 25
    timeout 900 local/vm/vm.sh ssh "cd ~/projects/open-gpu-kernel-modules/local && ./watchdog.sh --stall 300 --max 840 --watch bench-logs -- python3 bench/bench.py run superposition --label hr$H-apps-evict --apps --bg-fill 1500 --auto hints 2>&1 | tail -3; d=\$(ls -td bench-logs/hr$H-apps-evict-* | head -1); python3 bench/bench.py analyze \$d >/dev/null 2>&1; python3 -c \"import json;m=json.load(open('\$d/metrics.json'))['frames'];print('RESULT headroom $H', 'avg', m['avg_fps'], 'low1', m['low1_fps'], 'low01', m['low01_fps'], 'p99', m['p99_ms'])\"" 2>&1 | grep -v Pseudo
done
local/vm/vm.sh ssh "echo 'options nvidia NVreg_RegistryDwords=\"RmVidmemUserReserveMB=0\"' | sudo tee /etc/modprobe.d/nvidia-vidmem-reserve.conf >/dev/null" 2>/dev/null
timeout 180 local/vm/vm.sh reload tree 2>&1 | grep '^now'
echo SWEEP_DONE
