# shellcheck shell=bash
# lean4/lib.sh - the steps every PROJECT/lean4/run takes, so each run is a list of steps:
#
#   . "$(dirname "$0")/../../lean4/lib.sh" || exit 2
#   lean_build; lean_audit; lean_scan
#   diff_test c/HARNESS.c ORACLE      the harness, built from ../src with gcc and clang, prints one line per
#                                     operation; the lake exe ORACLE replays them on the model; outputs must match
#   mutant NAME FILE SED              a copy of ../src with one sed change; the differential test must catch it
#   sanitize                          the same harness under ASan+UBSan (gcc, clang) and MSan (clang): any report aborts, and
#                                     the output must still equal the plain build's (a sanitizer must not change behaviour)
#   fuzz_diff                         random seeds and operation counts (0, 1, tiny, huge) for FUZZ_SECS (default 20),
#                                     C against the model, so a case nobody picked by hand is also checked
#   crash_test                        kill -9 the harness at random moments: what it printed before dying must still be a
#                                     prefix the model reproduces exactly (nothing half-applied is ever visible), and a
#                                     memory cap (ENOMEM paths) must end in a clean exit or a clean abort, never a hang
#   stress                            big operation counts, NPROC runs in parallel, same seed twice: identical output
#                                     (no nondeterminism, no leak that grows), time and peak RSS printed
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


# build the harness with extra flags into $lean_tmp/NAME; fails quietly (the step reports it)
lean_cc_() { local cc=$1 out=$2; shift 2; "$cc" -std=gnu11 -O1 -g -w -D_GNU_SOURCE -I../src "$@" -o "$out" "$lean_harness" -lm 2> /dev/null; }

sanitize() {
    local cc s name flags runs=0 bad="" skipped="" seeds=${SANSEEDS:-30}
    (( lean_ready )) || { step FAIL "sanitize: needs a passing diff_test first"; return; }
    for cc in gcc clang; do
        command -v "$cc" > /dev/null || continue
        for name in asan-ubsan msan; do
            case $name in
                asan-ubsan) flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
                msan) [[ $cc == clang ]] || continue; flags="-fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer" ;;
            esac
            if ! lean_cc_ "$cc" "$lean_tmp/s_${cc}_$name" $flags; then
                [[ $name == msan ]] && { skipped="$skipped $cc:$name(no libs)"; continue; }
                bad="$bad $cc:$name(build)"; continue
            fi
            lean_cc_ "$cc" "$lean_tmp/p_${cc}_$name" || { bad="$bad $cc:plain"; continue; }
            for s in $(seq 1 "$seeds"); do
                if ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                        "$lean_tmp/s_${cc}_$name" "$s" "${OPS:-3000}" > "$lean_tmp/san.out" 2> "$lean_tmp/san.err" &&
                        "$lean_tmp/p_${cc}_$name" "$s" "${OPS:-3000}" > "$lean_tmp/pl.out" 2> /dev/null &&
                        cmp -s "$lean_tmp/san.out" "$lean_tmp/pl.out"; then runs=$((runs + 1))
                else bad="$bad $cc:$name:seed$s"; head -12 "$lean_tmp/san.err"; break; fi
            done
        done
    done
    if [[ -z $bad && $runs -gt 0 ]]; then step ok "sanitizers: $runs runs clean (ASan, UBSan, MSan; leaks on)${skipped:+; skipped$skipped}"
    else step FAIL "sanitizers:$bad"; fi
}

fuzz_diff() {
    local end=$((SECONDS + ${FUZZ_SECS:-20})) n=0 seed ops bad="" odd=(0 1 2 3 7 8 9 63 64 65 1000 20000)
    (( lean_ready )) || { step FAIL "fuzz_diff: needs a passing diff_test first"; return; }
    lean_cc_ gcc "$lean_tmp/fz" -O2 || { step FAIL "fuzz_diff: build"; return; }
    while (( SECONDS < end )); do
        seed=$(( (RANDOM << 15 | RANDOM) + 1000 ))
        (( n % 3 == 0 )) && ops=${odd[RANDOM % ${#odd[@]}]} || ops=$(( RANDOM % 6000 ))
        if ! { "$lean_tmp/fz" "$seed" "$ops" > "$lean_tmp/c"; } 2> /dev/null ||
                ! "$lean_oracle" < "$lean_tmp/c" > "$lean_tmp/l" || ! cmp -s "$lean_tmp/c" "$lean_tmp/l"; then
            bad="seed=$seed ops=$ops"; break
        fi
        n=$((n + 1))
    done
    if [[ -z $bad ]]; then step ok "fuzz: $n random (seed, ops) cases in ${FUZZ_SECS:-20}s, C equals the model"
    else step FAIL "fuzz: first failure $bad (rerun: HARNESS $bad)"; fi
}

crash_test() {
    local i seed ms n=${CRASHES:-60} ok=0 bad="" cut
    (( lean_ready )) || { step FAIL "crash_test: needs a passing diff_test first"; return; }
    lean_cc_ gcc "$lean_tmp/cr" -O2 || { step FAIL "crash_test: build"; return; }
    for i in $(seq 1 "$n"); do
        seed=$((i * 7919)); ms=$((RANDOM % 40 + 1))
        { timeout -s KILL "0.0$(printf %02d "$ms")" "$lean_tmp/cr" "$seed" 200000 > "$lean_tmp/part"; } 2> /dev/null
        # a kill can cut a line in half: drop the last, incomplete line; the rest must replay identically
        head -c -0 "$lean_tmp/part" | sed '$d' > "$lean_tmp/cut"
        [[ -s $lean_tmp/part && $(tail -c1 "$lean_tmp/part" | wc -l) -eq 1 ]] && cp "$lean_tmp/part" "$lean_tmp/cut"
        if "$lean_oracle" < "$lean_tmp/cut" > "$lean_tmp/l" 2> /dev/null && cmp -s "$lean_tmp/cut" "$lean_tmp/l"; then ok=$((ok + 1))
        else bad="$bad seed$seed@${ms}ms"; fi
    done
    # memory cap: a failing allocation must end the run cleanly, not hang or corrupt (a signal exit is a bad exit)
    local rc
    { ( ulimit -v $((${CAP_MB:-64} * 1024)); timeout 20 "$lean_tmp/cr" 1 2000000 > /dev/null 2> /dev/null ); } 2> /dev/null; rc=$?
    # a deliberate trap (SIGILL from __builtin_trap) or abort (SIGABRT) is a clean refusal; SIGSEGV/SIGBUS or a hang is not
    if (( rc == 124 )); then bad="$bad hang-under-ENOMEM"
    elif (( rc == 139 || rc == 135 || rc == 138 )); then bad="$bad crash(signal$((rc - 128)))-under-ENOMEM"; fi
    if [[ -z $bad ]]; then step ok "crash: $ok of $n kill -9 cuts replay exactly; ${CAP_MB:-64} MB cap ended with exit $rc"
    else step FAIL "crash:$bad"; fi
}

stress() {
    local par=${NPROC:-$(nproc)} big=${STRESS_OPS:-300000} i t0 rss
    (( lean_ready )) || { step FAIL "stress: needs a passing diff_test first"; return; }
    lean_cc_ gcc "$lean_tmp/st" -O2 || { step FAIL "stress: build"; return; }
    t0=$SECONDS
    for i in $(seq 1 "$par"); do ( "$lean_tmp/st" 4242 "$big" > "$lean_tmp/st.$i" 2> /dev/null ) & done; wait
    for i in $(seq 2 "$par"); do
        cmp -s "$lean_tmp/st.1" "$lean_tmp/st.$i" || { step FAIL "stress: same seed, different output (run $i)"; return; }
    done
    "$lean_oracle" < "$lean_tmp/st.1" > "$lean_tmp/st.l" 2> /dev/null && cmp -s "$lean_tmp/st.1" "$lean_tmp/st.l" ||
        { step FAIL "stress: $big operations differ from the model"; return; }
    rss=$(python3 -c 'import resource,subprocess,sys;subprocess.run(sys.argv[1:],stdout=subprocess.DEVNULL);print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)' \
        "$lean_tmp/st" 4242 "$big" 2> /dev/null)
    step ok "stress: $big operations x $par parallel, identical and equal to the model; $((SECONDS - t0))s, peak RSS ${rss:-?} KB"
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
