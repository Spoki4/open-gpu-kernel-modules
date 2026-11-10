#!/usr/bin/env bash
# Runs every scenario in scenarios/ through the simulator (builds it first).
# A scenario whose first lines say "# expect-fail: ..." documents a known bug:
# its failures are expected, and it passing is reported too (time to drop the
# line). Exit status 1 if anything else fails or a known bug passes.
#   ./run-tests.sh [-v] [SCENARIO...]
set -euo pipefail
cd "$(dirname "$0")"
make -s sim
verbose=0
[[ ${1:-} == -v ]] && { verbose=1; shift; }
files=("$@")
[[ ${#files[@]} -eq 0 ]] && files=(scenarios/*.scn)
bad=0
for f in "${files[@]}"; do
    known=$(head -5 "$f" | sed -n 's/^# expect-fail: *//p')
    out=$(./sim "$f" 2>&1) && st=0 || st=$?
    res=$(grep -E '^expect: ' <<<"$out" | sed 's/^expect: //')
    if [[ $st -eq 2 ]]; then
        printf '%-32s ERROR\n%s\n' "$(basename "$f")" "$(tail -3 <<<"$out")"; bad=1; continue
    fi
    if [[ -n $known ]]; then
        if [[ $st -eq 0 ]]; then
            printf '%-32s PASSES NOW (%s), was known to fail: %s\n' "$(basename "$f")" "$res" "$known"; bad=1
        else
            printf '%-32s known bug (%s): %s\n' "$(basename "$f")" "$res" "$known"
        fi
    elif [[ $st -eq 0 ]]; then
        printf '%-32s ok (%s)\n' "$(basename "$f")" "$res"
    else
        printf '%-32s FAIL (%s)\n' "$(basename "$f")" "$res"; bad=1
    fi
    if [[ $verbose -eq 1 || ( $st -ne 0 && -z $known ) ]]; then
        grep -E '  (PASS|FAIL) ' <<<"$out" | sed 's/^/    /'
    fi
done
exit $bad
