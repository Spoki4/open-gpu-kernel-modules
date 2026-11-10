#!/bin/bash
# BACKLOG #16: fragment free vidmem the way a long desktop session does, to
# see how short the free runs get and to try compaction on it. Many idle
# processes (nvt-game --idle under the nvt-idle-*/nvt-bg-* names) of mixed
# sizes and buffer sizes start and exit in random order while free vidmem is
# kept between LOW and HIGH MB; with "moves" some are also moved to sysmem and
# back (evict.sh), as the policy does on the host. Prints the driver's
# fragmentation line every 10 s and at the end, with everything still running
# and after all of it exited. Run in the test VM guest; record alongside with
# nvtrace.py for the vidmem_frag samples.
#   ./fragchurn.sh [SECONDS=300] [moves] [SEED]
cd "$(dirname "$0")" || exit 1
dur=${1:-300} moves=0 seed=1
[[ ${2:-} == moves ]] && { moves=1; shift; }
seed=${2:-1}
LOW=${LOW:-1200} HIGH=${HIGH:-2600} MAXPROCS=${MAXPROCS:-24}
RANDOM=$seed
names=(nvt-idle-a nvt-idle-b nvt-bg-a nvt-bg-b nvt-bg-c nvt-bg-d)
chunks=(2 2 4 4 8 8 16 16 32 32 64 128 256)
pids=()
trap 'kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null' EXIT
free_mb() { nvidia-smi --query-gpu=memory.total,memory.used --format=csv,noheader,nounits | awk -F', ' '{print $1 - $2}'; }
frag() { sudo -n ./evict.sh status 2>/dev/null | grep '^fragmentation' | sed 's/^fragmentation: //'; }
alive() { local p out=(); for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && out+=("$p"); done; pids=("${out[@]}"); }
t0=$SECONDS last=-10 started=0 ended=0 moved=0
echo "fragchurn: $dur s, moves $moves, seed $seed, free kept in $LOW..$HIGH MB"
echo "start: $(frag)"
while (( SECONDS - t0 < dur )); do
    alive
    f=$(free_mb)
    if (( f > HIGH && ${#pids[@]} < MAXPROCS )); then
        c=${chunks[RANDOM % ${#chunks[@]}]}
        mb=$(( c * (2 + RANDOM % 14) ))
        (( mb > f - LOW )) && mb=$(( f - LOW ))
        (( mb >= c )) || { sleep 0.3; continue; }
        ./"${names[RANDOM % ${#names[@]}]}" --mb "$mb" --chunk-mb "$c" --idle --floor-mb "$LOW" >/dev/null 2>&1 &
        pids+=($!); started=$((started + 1))
        sleep 0.4
    elif (( ${#pids[@]} )); then
        i=$(( RANDOM % ${#pids[@]} ))
        if (( moves && RANDOM % 3 == 0 )); then
            op=evict; (( RANDOM % 2 )) && op=restore
            sudo -n ./evict.sh $op "${pids[i]}" >/dev/null 2>&1; moved=$((moved + 1))
        else
            kill "${pids[i]}" 2>/dev/null; wait "${pids[i]}" 2>/dev/null; ended=$((ended + 1))
        fi
        sleep 0.2
    fi
    if (( SECONDS - last >= 10 )); then
        last=$SECONDS
        printf '%4d s, %2d procs: %s\n' $((SECONDS - t0)) ${#pids[@]} "$(frag)"
    fi
done
alive
echo "end ($started started, $ended ended, $moved moves, ${#pids[@]} running): $(frag)"
kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null; pids=()
sleep 2
echo "all exited: $(frag)"
