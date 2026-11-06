# shellcheck shell=bash
# lean4/lib.sh - the steps every PROJECT/lean4/run takes, so each run is a list of steps:
#
#   . "$(dirname "$0")/../../lean4/lib.sh" || exit 2
#   lean_build; lean_audit; lean_scan
#   diff_test c/HARNESS.c ORACLE      the harness, built from ../src with gcc and clang, prints one line per
#                                     operation; the lake exe ORACLE replays them on the model; outputs must match
#   mutant NAME FILE SED              a copy of ../src with one sed change; the differential test must catch it
#   lean_end                          the mutant line, then exit 1 if any step failed
#
# SEEDS=N sets the seeds per compiler (default 200), OPS=N the operations per run (default 3000). Each step prints
# "ok" or "FAIL" and a summary.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[1]}")" || exit 2
export PATH="$HOME/.elan/bin:$PATH"
fail=0 caught=0 total=0 survived="" lean_harness="" lean_oracle="" lean_ready=0
lean_tmp=$(mktemp -d)
trap 'rm -rf "$lean_tmp"' EXIT

step() { printf '  %-4s %s\n' "$1" "$2"; [[ $1 == ok ]] || fail=1; }

lean_build() {
    if lake build > .lake-build.log 2>&1; then step ok "lake build: $(grep -oE '[0-9]+ jobs' .lake-build.log | tail -1)"
    else tail -20 .lake-build.log; step FAIL "lake build"; fi
}

lean_audit() {
    if lake build +Holyc.Audit > .lake-audit.log 2>&1 && lake env lean Audit.lean > .lake-audit.log 2>&1 &&
            grep -q 'none rests' .lake-audit.log; then
        step ok "$(grep -oE 'axiom audit: [0-9]+ theorems \([0-9]+ written' .lake-audit.log) in the sources)"
    else cat .lake-audit.log; step FAIL "axiom audit"; fi
}

lean_scan() {
    if grep -rnE '\b(sorry|admit|native_decide|bv_decide)\b|^[[:space:]]*axiom ' --include=*.lean --exclude=Audit.lean \
            --exclude-dir=.lake .; then step FAIL "source scan"
    else step ok "source scan: no sorry, admit, native_decide, bv_decide or axiom"; fi
}

# HARNESS-BINARY SEED: its output and the oracle's replay of it are identical
lean_same_() {
    { "$1" "$2" "${OPS:-3000}" > "$lean_tmp/c"; } 2> /dev/null && "$lean_oracle" < "$lean_tmp/c" > "$lean_tmp/l" &&
        cmp -s "$lean_tmp/c" "$lean_tmp/l"
}

diff_test() {
    local cc s runs=0 bad="" seeds=${SEEDS:-200}
    lean_harness=$1 lean_oracle=.lake/build/bin/$2
    if ! lake build "$2" > .lake-oracle.log 2>&1; then tail -20 .lake-oracle.log; step FAIL "$2 build"; return; fi
    for cc in gcc clang; do
        command -v "$cc" > /dev/null || continue
        if ! "$cc" -std=gnu11 -O2 -w -D_GNU_SOURCE -I../src -o "$lean_tmp/h_$cc" "$lean_harness"; then bad="$bad $cc"; continue; fi
        for s in $(seq 1 "$seeds"); do
            if lean_same_ "$lean_tmp/h_$cc" "$s"; then runs=$((runs + 1))
            else bad="$bad $cc:seed$s"; diff "$lean_tmp/c" "$lean_tmp/l" | head -4; break; fi
        done
    done
    if [[ -z $bad && $runs -gt 0 ]]; then
        lean_ready=1
        step ok "differential: $runs runs of ${OPS:-3000} operations (seeds 1-$seeds, gcc and clang), the C equals the model"
    else step FAIL "differential:$bad"; fi
}

mutant() {
    local m s
    total=$((total + 1))
    (( lean_ready )) || return  # a mutant only counts against a differential test that passes
    m=$(mktemp -d "$lean_tmp/m.XXXX"); cp -r ../src "$m/src"
    sed -i "$3" "$m/src/$2"
    if cmp -s "../src/$2" "$m/src/$2"; then survived="$survived $1(sed-matched-nothing)"; return; fi
    gcc -std=gnu11 -O2 -w -D_GNU_SOURCE -I"$m/src" -o "$m/h" "$lean_harness" 2> /dev/null || { caught=$((caught + 1)); return; }
    for s in 1 2 3 4 5; do lean_same_ "$m/h" "$s" || { caught=$((caught + 1)); return; }; done
    survived="$survived $1"
}

lean_end() {
    if (( total > 0 )); then
        if (( !lean_ready )); then step FAIL "mutants: not run, the differential test did not pass"
        elif [[ -z $survived ]]; then step ok "mutants: $caught of $total caught"
        else step FAIL "mutants: survived:$survived"; fi
    fi
    exit "$fail"
}
