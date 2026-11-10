#!/bin/bash
# What a running game's vidmem looks like and how fast it moves: allocations
# by kind / page size / contiguity (evict.sh dry), then CYCLES of evict,
# restore and move with the driver's phase and copy lines. The game keeps
# running; a move pauses it for the copy. Run in the test VM guest.
#   ./game-moves.sh PID|NAME [CYCLES=3]
cd "$(dirname "$0")" || exit 1
t=$1 cycles=${2:-3}
E=(sudo -n ./evict.sh)
"${E[@]}" dry "$t" >/tmp/game-dry.txt 2>&1
head -2 /tmp/game-dry.txt
awk '/KB kind/ { kb=$2; kind=$5; page=""; contig="scattered"; ok=($NF=="ok")?"movable":"fixed (" $NF ")";
       for (i=1;i<=NF;i++) { if ($i=="page") page=$(i+1); if ($i=="contig") contig="contiguous" }
       if (page=="") page="?"; k=ok " | kind " kind " | pages " page " KB | " contig; n[k]++; s[k]+=kb; if (kb>m[k]) m[k]=kb }
     END { for (k in n) printf "  %5d MB in %3d allocs (largest %4d MB): %s\n", s[k]/1024, n[k], m[k]/1024, k }' /tmp/game-dry.txt | sort -rn
grep '^summary' /tmp/game-dry.txt
for c in $(seq "$cycles"); do
    for op in evict restore move; do
        out=$("${E[@]}" $op "$t" 2>&1)
        echo "$out" | awk -v op="$op" -v c="$c" '
            /^summary/ { for (i=1;i<=NF;i++) if ($i=="allocs") { n=$(i-1); mb=$(i+1) } ms=$(NF-1); fail=$0; sub(/.*failed /,"",fail); sub(/ .*/,"",fail) }
            /^phases/  { ph=$0; sub(/^phases ms: /,"",ph) }
            /^copy:/   { cp=$0; sub(/^copy: /,"",cp); split(ph,a,", "); cms=a[3]; sub(/copy /,"",cms) }
            END { printf "cycle %d %-7s %5d MB %3d allocs failed %s: %4d ms (%s); %s; copy %.1f GB/s\n", c, op, mb, n, fail, ms, ph, cp, (cms>0)? mb/1024/(cms/1000):0 }'
        sleep 2
    done
done
