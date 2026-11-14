#!/bin/bash
# The map of vidmem (`trace/nvtrace.py map`, `gamerec.sh map`): who holds
# which frames at any moment of a recording, and what each move took from
# where to where.
# Without a GPU: a simulator scenario with the layout written as a trace
# (sim -map) must give a map whose last snapshot agrees with the changes
# before it; the same trace with one change cut out must be noticed; lines as
# the kernel prints them (a process name with spaces) must be read.
# With the patched driver loaded (run in the test VM guest or on a rented
# machine, nobody logged in): a recording around bounce-test.sh and
# compact-test.sh. The snapshot at the end must agree with the changes the
# driver traced since the one at the start (not one frame differs: nothing
# is allocated or freed around the trace), and the moves to sysmem, back and
# inside vidmem must come with the frames they left and took.
# Ends with MAPTEST PASS|FAIL.
#   ./map-test.sh [sim]        sim: only the part without a GPU
cd "$(dirname "$0")" || exit 1
T=$(mktemp -d /tmp/map-test.XXXX)
trap 'rm -rf "$T"' EXIT
fails=0
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }
num() { grep -oP -- "$1" <<<"$out" | head -1; }      # a number out of nvtrace's summary lines
SNAPS='\d+(?= later snapshot)' DIFF='\d+(?= frames differed)' MOVES='moves \K\d+' OUT='to sysmem \K\d+'
BACK='back \K\d+' INSIDE='inside vidmem \K\d+' PLACED='\d+(?= with their frames)' WINDOWS='\d+(?= windows)'

make -s -C ../sim sim || { echo "MAPTEST FAIL 1"; exit 0; }
mkdir "$T/sim" "$T/cut" "$T/kern"
../sim/sim -map "$T/sim/trace.txt" ../sim/scenarios/compact-game.scn >/dev/null
out=$(../trace/nvtrace.py map "$T/sim" 2>&1); echo "$out" | sed 's/^/  sim: /'
[[ $(num "$SNAPS") -ge 1 && $(num "$DIFF") == 0 ]] ||
    fail "the simulator's last snapshot does not agree with its changes"
[[ $(num "$INSIDE") -ge 10 && $(num "$PLACED") == $(num "$MOVES") ]] ||
    fail "moves without their frames"
[[ $(num "$WINDOWS") -ge 5 ]] || fail "no compaction windows"
grep -q '"comm":"game"' "$T/sim/map.html" || fail "no process names in the page"

# The last arrival of a move lost: the snapshot at the end must show it.
last=$(grep -n 'op=alloc mode=move' "$T/sim/trace.txt" | tail -1 | cut -d: -f1)
sed "${last}d" "$T/sim/trace.txt" >"$T/cut/trace.txt"
out=$(../trace/nvtrace.py map "$T/cut" 2>&1)
[[ $(num "$DIFF") -gt 0 ]] || fail "a lost change went unnoticed: $out"

# As trace_pipe prints them.
cat >"$T/kern/trace.txt" <<'EOF'
 Isolated Web Co-4242    [003] ..... 100.000100: vidmem_ext: op=snap mode=- why=- pid=0 comm=- n=1 runs={0x800,0x400}
 Isolated Web Co-4242    [003] ..... 100.000110: vidmem_ext: op=have mode=- why=- pid=1 comm=- n=1 runs={0x0,0x40}
 Isolated Web Co-4242    [003] ..... 100.000120: vidmem_ext: op=have mode=- why=- pid=4242 comm=Isolated Web Co n=2 runs={0x40,0x20,0x100,0x20}
 Isolated Web Co-4242    [003] ..... 100.000130: vidmem_ext: op=snapend mode=- why=- pid=0 comm=- n=1 runs={0x80,0x0}
     kworker/3:1-77      [003] ..... 101.000000: vidmem_ext: op=free mode=evict why=pressure pid=4242 comm=Isolated Web Co n=1 runs={0x100,0x20}
     kworker/3:1-77      [003] ..... 101.004000: vidmem_move: op=evict pid=4242 comm=Isolated Web Co why=pressure kb=2048 allocs=1 failed=0 shared=0 channels=2 total_us=4100 quiesce_us=900 sysalloc_us=100 copy_us=2000 remap_us=500 pma_us=100 sysfree_us=0
 Isolated Web Co-4242    [003] ..... 102.000100: vidmem_ext: op=snap mode=- why=- pid=0 comm=- n=1 runs={0x800,0x400}
 Isolated Web Co-4242    [003] ..... 102.000110: vidmem_ext: op=have mode=- why=- pid=1 comm=- n=1 runs={0x0,0x40}
 Isolated Web Co-4242    [003] ..... 102.000120: vidmem_ext: op=have mode=- why=- pid=4242 comm=Isolated Web Co n=1 runs={0x40,0x20}
 Isolated Web Co-4242    [003] ..... 102.000130: vidmem_ext: op=snapend mode=- why=- pid=0 comm=- n=1 runs={0x60,0x0}
EOF
out=$(../trace/nvtrace.py map "$T/kern" 2>&1)
[[ $(num "$DIFF") == 0 && $(num "$OUT") == 1 && $(num "$PLACED") == 1 ]] ||
    fail "the kernel's lines were not read: $out"
grep -q '"comm":"Isolated Web Co"' "$T/kern/map.html" || fail "a process name with spaces was cut"

if [[ ${1:-} != sim ]]; then
    if ! ls /proc/driver/nvidia/gpus/*/vidmem_evict >/dev/null 2>&1; then
        fail "no patched driver loaded (./map-test.sh sim for the part without a GPU)"
    elif ! sudo -n ../evict.sh map | grep -q '^map: '; then
        fail "this driver has no map of vidmem: $(sudo -n ../evict.sh map 2>&1 | head -1)"
    else
        sudo -n ../evict.sh map | sed 's/^/  driver: /'
        sudo -n ../trace/nvtrace.py record -o "$T/rec" -- bash -c './bounce-test.sh; ./compact-test.sh 60 4096' 2>&1 | tail -2 | sed 's/^/  /'
        grep -E 'TEST (PASS|FAIL)' "$T/rec/cmd.log" | sed 's/^/  /'
        out=$(../trace/nvtrace.py map "$T/rec" 2>&1); echo "$out" | sed 's/^/  driver: /'
        [[ $(num "$SNAPS") -ge 1 ]] || fail "no snapshot at the end of the recording"
        [[ $(num "$DIFF") == 0 ]] ||
            fail "$(num "$DIFF") frames changed hands around the trace"
        grep -q '"map_end": "map: .* 0 frames differed from the allocator' "$T/rec/meta.json" ||
            fail "the driver's table and the allocator's own map disagree: $(grep -o '"map_end": "[^"]*"' "$T/rec/meta.json")"
        to=$(num "$OUT") back=$(num "$BACK") in=$(num "$INSIDE")
        [[ ${to:-0} -ge 1 && ${back:-0} -ge 1 && ${in:-0} -ge 1 ]] || fail "the tests made no moves of each kind ($to out, $back back, $in inside)"
        [[ $(num "$PLACED") == $(num "$MOVES") ]] || fail "moves without their frames"
        [[ $(num "$WINDOWS") -ge 1 ]] || fail "no compaction window in the trace"
        sudo journalctl -k --since "-4min" --no-pager | grep -qE 'Xid|BUG:|Oops|general protection' && fail "kernel log"
        cp "$T/rec/map.html" /tmp/map-test.html 2>/dev/null && echo "  the page: /tmp/map-test.html"
    fi
fi
(( fails == 0 )) && echo "MAPTEST PASS" || echo "MAPTEST FAIL $fails"
