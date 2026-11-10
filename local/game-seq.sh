#!/bin/bash
# game-seq.sh PID OP [OP...]: evict.sh OPs on PID in the given order, one line each
# (wait: sleep until the policy has brought everything back, at most 20 s).
cd "$(dirname "$0")" || exit 1
t=$1; shift
for op in "$@"; do
    if [[ $op == wait ]]; then
        for _ in $(seq 40); do
            r=$(sudo -n ./evict.sh status | awk -v p="$t" '$1 == p {print $4; exit}')
            [[ ${r:-0} -eq 0 ]] && break; sleep 0.5
        done
        echo "wait: in sysmem ${r:-0} MB"; continue
    fi
    sudo -n ./evict.sh "$op" "$t" 2>&1 | awk -v op="$op" '
        /^summary/ { for (i=1;i<=NF;i++) if ($i=="allocs") { n=$(i-1); mb=$(i+1); break } }
        /^phases/  { ph=$0; sub(/^phases ms: /,"",ph); split(ph,a,", "); cms=a[3]; sub(/copy /,"",cms) }
        /^copy:/   { cp=$0; sub(/^copy: /,"",cp) }
        END { printf "%-7s %5d MB %3d allocs: copy %4d ms = %5.1f GB/s; %s\n", op, mb, n, cms, (cms>0)? mb/1024/(cms/1000):0, cp }'
    sleep 1
done
