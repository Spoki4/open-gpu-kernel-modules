#!/bin/bash
# Everything that waits for the GPU, in one run on the test VM guest (host in
# vm mode): build and load the tree's driver, then stage after stage, each
# under a watchdog, each with its own verdict; a failed stage does not stop the
# next ones, an Xid or a guest that stops answering does (what follows would
# say nothing). Ends with a table and
#   FULLTEST PASS|FAIL <failed stages>      exit 0 on PASS
#
#   local/vm/fulltest.sh [--from STAGE] [--only STAGE[,STAGE...]] [--no-build] [--no-game] [--here]
#
# Stages, in order:
#   smoke        vmtest.sh integrity (build, reload, evict/restore keeps the content)
#   tools        the test tools rebuilt in the guest (gbm-hog --churn)
#   move         move-test.sh        moves inside vidmem keep the content (#16)
#   contig       contig-test.sh      windows for the compositor's contiguous buffers (#16, #12)
#   contigloop   contigloop-test.sh  a focused app remaking a contiguous buffer: no loop (#15)
#   compact      compact-test.sh     background compaction lengthens the free run (#16)
#   compactfg    compact-test.sh fg  the same out of the memory of the window in focus (#16)
#   compactbig   compact-test.sh     the same with 128 MB blocks, as a game on the other monitor holds
#   compactbigfg compact-test.sh fg  the same with 64 MB blocks of the window in focus
#   sides        sides-test.sh       next to a game the others' memory is placed at the other end of vidmem
#   fgown        fgown-test.sh       the focused app gets memory nobody can make room for (#7)
#   bounce       bounce-test.sh      an app covered by the focused window gives its headroom and stays out
#   nvtest       vmtest.sh all       the whole bench (15 scenarios; S, #5-#13, R)
#   desktop      the guest's Plasma session under fragchurn with moves: compaction and
#                windows now also move buffers the compositor maps (evict.sh
#                moveshared); kwin and plasmashell must survive, the log stay clean
#   game         the game from Steam (game-start.sh) under fragchurn with moves and the
#                contiguous loop: it keeps running, no Xid (#16: bdb7f072 was never
#                checked whole). SKIP when Steam is not logged in (the user does that).
#   momcprobe    momc-probe.sh       a probe, not a check: do access counters report accesses
#                                    to vidmem (I5)? The answer is in the PASS line
#
# On a rented machine (local/cloud/), run there, with this tree's modules loaded
# and nobody logged in: local/vm/fulltest.sh --here. No VM is involved; the
# bench runs scenario by scenario (stages nv-*), and the stages that need the
# guest's Plasma session or Steam (desktop, game) are left out.
#
# Not here: pressing "start benchmark" in the game (input is the user's), see
# problem #16.
#
# Log: local/stress-logs/fulltest-<date_time>.log; vmtest's and nvtest's own next to it.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD
from= only= build=1 game=1 here=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --from) from=$2; shift 2 ;;
        --only) only=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        --no-game) game=0; shift ;;
        --here) here=1; game=0; shift ;;
        *) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
exec 8>/tmp/fulltest.lock
flock -n 8 || { echo "FULLTEST FAIL another fulltest.sh is running" >&2; exit 1; }
mkdir -p local/stress-logs
log=local/stress-logs/fulltest-$(date +%F_%H%M%S).log
say() { echo "$*" | tee -a "$log"; }
# The machine under test: the guest over ssh, or (--here) this one.
if (( here )); then
    gssh() { timeout "${GSSH_T:-60}" bash -c "$*" 2>/dev/null; }
else
    gssh() { timeout "${GSSH_T:-60}" local/vm/vm.sh ssh "$@" 2>/dev/null | tr -d '\r'; }
fi
BAD='Xid|BUG:|Oops|general protection|kernel panic|soft lockup|hung task'
NOISE="$BAD|can.t update VA|[Aa]ssert|not remade|move copy failed"
CLEAN="cd '$ROOT/local/tests' && { ./nvt-session.sh stop; pkill -f '[n]vt-'; pkill -x gbm-hog; pkill -f '^[.]/sddm-greeter'; pkill -f '[f]ragchurn'; pkill -f '[c]ontig'; pkill -f '[c]ompact-test'; : | sudo -n tee /proc/driver/nvidia/vidmem_hints; } >/dev/null 2>&1; true"

names=() verdicts=() notes=()
stop_why=
want() { # STAGE: is it to run?
    local s=$1
    if [[ -n $only ]]; then [[ ,$only, == *,$s,* ]]; return; fi
    if [[ -n $from ]]; then [[ $s == "$from" ]] && from=; [[ -z $from ]]; return; fi
    return 0
}
record() { names+=("$1"); verdicts+=("$2"); notes+=("$3"); say "== $1: $2 $3"; }
# After a stage: the guest answers, and nothing fatal in its kernel log since T.
health() { # T
    local k
    k=$(GSSH_T=40 gssh "sudo journalctl -k --since '$1' --no-pager | grep -E '$BAD' | tail -3")
    if ! GSSH_T=20 gssh true; then stop_why="the guest stopped answering"; return 1; fi
    if [[ -n $k ]]; then say "$k"; stop_why="kernel: $(head -1 <<<"$k" | cut -c1-120)"; return 1; fi
    return 0
}
# One stage: a command in the guest that prints a line matching PASSRE on success.
stage() { # NAME STALL MAX PASSRE GUESTCMD
    local name=$1 stall=$2 max=$3 passre=$4 cmd=$5 t0 out rc line
    want "$name" || return 0
    if [[ -n $stop_why ]]; then record "$name" SKIP "($stop_why)"; return 0; fi
    say; say "== $name ($(date +%T))"
    t0=$(gssh "date '+%F %T'")
    out=$(mktemp)
    if (( here )); then
        local/watchdog.sh --stall "$stall" --max "$max" --on-kill "$CLEAN" -- \
            bash -c "cd '$ROOT/local/tests' && $cmd" 2>&1 | tee -a "$log" >"$out"
    else
        local/watchdog.sh --stall "$stall" --max "$max" --on-kill "timeout 60 '$ROOT/local/vm/vm.sh' ssh \"$CLEAN\"" -- \
            local/vm/vm.sh ssh "cd '$ROOT/local/tests' && $cmd" 2>&1 | grep --line-buffered -v '^Pseudo-terminal' | tee -a "$log" >"$out"
    fi
    rc=${PIPESTATUS[0]}
    line=$(grep -E "$passre|FAIL|WATCHDOG (STALL|TIMEOUT)" "$out" | tail -1)
    if grep -qE "^(${name^^}|NVTEST [a-z0-9-]+) SKIP" "$out"; then
        record "$name" SKIP "$(grep -E "^(${name^^}|NVTEST [a-z0-9-]+) SKIP" "$out" | tail -1)"
    elif grep -qE 'WATCHDOG (STALL|TIMEOUT)' "$out"; then
        record "$name" FAIL "watchdog: $(grep -E 'WATCHDOG (STALL|TIMEOUT)' "$out" | head -1)"
    elif grep -qE "$passre" "$out" && ! grep -q ' FAIL' "$out"; then
        record "$name" PASS "$(grep -E "$passre" "$out" | tail -1)"
    else
        record "$name" FAIL "${line:-no verdict (rc $rc)}"
    fi
    rm -f "$out"
    gssh "sudo journalctl -k --since '$t0' --no-pager | grep -E '$NOISE' | tail -5" | sed 's/^/  kernel: /' | tee -a "$log"
    # For reading afterwards (the log only): the policy's state and what compaction said during the stage.
    { gssh "cd '$ROOT/local' && sudo -n ./evict.sh status | sed -n '1,/^pid /p'"
      gssh "sudo journalctl -k --since '$t0' --no-pager | grep -oE 'vidmem evict: (compaction|move pid).*' | tail -80"; } | sed 's/^/  | /' >>"$log"
    health "$t0" || true
    GSSH_T=90 gssh "$CLEAN" || true
}
vmt() { # NAME ARGS...: a vmtest.sh run as a stage
    local name=$1 t0 rc; shift
    want "$name" || return 0
    if [[ -n $stop_why ]]; then record "$name" SKIP "($stop_why)"; return 0; fi
    say; say "== $name ($(date +%T)): vmtest.sh $*"
    t0=$(gssh "date '+%F %T'")
    local/vm/vmtest.sh "$@" 2>&1 | tee -a "$log" | tail -40 >/tmp/fulltest.vmt.$$
    rc=${PIPESTATUS[0]}
    record "$name" "$([[ $rc == 0 ]] && echo PASS || echo FAIL)" "$(grep '^VMTEST' /tmp/fulltest.vmt.$$ | tail -1)"
    grep -E '^NVTEST .* FAIL' /tmp/fulltest.vmt.$$ | sed 's/^/  /' | tee -a "$log"
    rm -f /tmp/fulltest.vmt.$$
    [[ -n $t0 ]] && { health "$t0" || true; }
}

say "FULLTEST $(git log -1 --format='%h %s' 2>/dev/null)$(git diff --quiet HEAD -- src kernel-open local 2>/dev/null || echo ' + working tree')$( ((here)) && echo ' (on this machine)')"
if (( here )); then
    [[ -e /proc/driver/nvidia/vidmem_hints ]] || { say "FULLTEST FAIL the modules loaded here are not this tree's (no vidmem_hints)"; exit 1; }
elif [[ $(local/vm/vm.sh status 2>/dev/null) != running:* ]]; then
    say "guest not running: starting it"
    local/vm/vm.sh start >>"$log" 2>&1 && timeout 300 local/vm/vm.sh wait >>"$log" 2>&1 ||
        { say "FULLTEST FAIL the guest did not start (host in vm mode? sudo local/vm/mode.sh vm, reboot)"; exit 1; }
fi

if (( here )); then stage smoke 240 900 '^NVTEST integrity PASS' "NVTEST_FORCE=1 ./nvtest.sh integrity"
elif (( build )); then vmt smoke integrity; else vmt smoke --no-build integrity; fi
gssh "cd '$ROOT/local' && sudo -n ./evict.sh status | sed -n '1,4p'" | sed 's/^/  /' | tee -a "$log"

stage tools 300 900 'TOOLS OK' "../build.sh tools >/tmp/build-tools.log 2>&1 && echo TOOLS OK || { tail -5 /tmp/build-tools.log; echo TOOLS FAIL; }"
stage move 180 900 'MOVETEST PASS' "./move-test.sh"
stage contig 180 600 'CONTIGTEST PASS' "./contig-test.sh"
stage contigloop 180 600 'CONTIGLOOPTEST PASS' "./contigloop-test.sh"
stage compact 180 900 'COMPACTTEST PASS' "./compact-test.sh 120 4096"
stage compactfg 180 900 'COMPACTTEST PASS' "./compact-test.sh 120 4096 fg"
stage compactbig 180 900 'COMPACTTEST PASS' "CHUNK_MB=128 ./compact-test.sh 120 4096"
stage compactbigfg 180 900 'COMPACTTEST PASS' "CHUNK_MB=64 ./compact-test.sh 120 4096 fg"
stage sides 180 600 'SIDESTEST PASS' "./sides-test.sh 90"
stage fgown 180 600 'FGOWNTEST PASS' "./fgown-test.sh"
stage bounce 180 600 'BOUNCETEST PASS' "./bounce-test.sh"
if (( here )); then
    # Scenario by scenario: one that fails does not take the rest with it.
    for sc in active "chrome 5 anim" "chrome 3 idle" "webgpu 5" occlusion "switch 5" "fgprio 3 idle" "fgprio 2 anim" \
              "game 30" fgback focus vaflip comproom ctxroom; do
        stage "nv-${sc// /-}" 480 1500 '^NVTEST [a-z0-9-]+ PASS' "NVTEST_FORCE=1 ./nvtest.sh $sc"
    done
else
    vmt nvtest --no-build --no-reload all
fi

# The real session: kwin and plasmashell with their window buffers, vidmem
# churned around them, compaction on. They must be the same processes after.
(( here )) || stage desktop 240 900 'DESKTOP PASS' "
    k0=\$(pgrep -x kwin_wayland | head -1); p0=\$(pgrep -x plasmashell | head -1)
    [[ -n \$k0 ]] || { echo 'DESKTOP FAIL no Plasma session in the guest'; exit 0; }
    T0=\$(date '+%F %T')
    s0=\$(sudo -n ../evict.sh status | grep -o 'background compaction.*')
    ./fragchurn.sh 150 moves 7 | tail -6
    sleep 30
    sudo -n ../evict.sh status | grep -E '^(compaction saw|fragmentation|windows cleared)' | sed 's/^/  /'
    echo \"  before: \$s0\"
    k1=\$(pgrep -x kwin_wayland | head -1); p1=\$(pgrep -x plasmashell | head -1)
    bad=\$(sudo journalctl -k --since \"\$T0\" --no-pager | grep -cE '$NOISE')
    shared=\$(sudo -n ../evict.sh status | sed -n 's/.*movable with a pause of the compositor \\([0-9]*\\).*/\\1/p')
    echo \"  kwin \$k0 -> \$k1, plasmashell \$p0 -> \$p1, kernel complaints \$bad, window buffers movable \${shared:-?} MB\"
    if [[ \$k0 == \"\$k1\" && \$p0 == \"\$p1\" && \$bad == 0 ]]; then echo DESKTOP PASS; else echo DESKTOP FAIL; fi"

if (( game )); then
    stage game 240 1500 'GAME PASS' "
        T0=\$(date '+%F %T')
        g=\$(./game-start.sh 2>/tmp/game-start.err) || { echo \"GAME SKIP \$(cat /tmp/game-start.err)\"; exit 0; }
        echo \"  game pid \$g: \$(sudo -n ../evict.sh status | awk -v p=\$g '\$1 == p {print \$3\" MB vram, \"\$4\" MB ram\"}')\"
        ./fragchurn.sh 150 moves 11 | tail -4
        ./contigloop-test.sh 40 2 | tail -5
        sleep 20
        sudo -n ../evict.sh status | grep -E '^(compaction saw|fragmentation|windows cleared)' | sed 's/^/  /'
        alive=0; kill -0 \$g 2>/dev/null && alive=1
        bad=\$(sudo journalctl -k --since \"\$T0\" --no-pager | grep -cE '$NOISE')
        echo \"  game alive \$alive: \$(sudo -n ../evict.sh status | awk -v p=\$g '\$1 == p {print \$3\" MB vram, \"\$4\" MB ram\"}'); kernel complaints \$bad\"
        pkill -f '[b]1_benchmark' ; sleep 3; (steam -shutdown >/dev/null 2>&1 &) ; sleep 8
        if [[ \$alive == 1 && \$bad == 0 ]]; then echo GAME PASS; else echo GAME FAIL; fi"
fi
# Last: counters nobody has run on this GPU; whatever they do, the checks are over.
stage momcprobe 120 300 'MOMCPROBE PASS' "./momc-probe.sh"

say; say "== summary"
failed=() ran=0
for i in "${!names[@]}"; do
    say "$(printf '%-11s %-5s %s' "${names[i]}" "${verdicts[i]}" "${notes[i]}")"
    [[ ${verdicts[i]} == FAIL ]] && failed+=("${names[i]}")
    [[ ${verdicts[i]} != SKIP ]] && ran=$((ran + 1))
done
[[ -n $stop_why ]] && say "stopped early: $stop_why"
if (( ${#failed[@]} > 0 )) || [[ -n $stop_why ]] || (( ran == 0 )); then
    say "FULLTEST FAIL ${failed[*]:-} ${stop_why:+($stop_why)} $log"
    exit 1
fi
say "FULLTEST PASS $ran stages, $log"
