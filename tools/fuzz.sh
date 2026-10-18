#!/bin/sh
# Runs the libFuzzer targets of tests/fuzz/ (packet 1.26) for a while each,
# all at once, seeded from the vendored test suites.
#
# Usage: tools/fuzz.sh [--seconds N] [--build-directory DIR] [--work-directory DIR]
#                      [--no-build] [TARGET...]
#   --seconds N           how long each target runs (default 60)
#   --build-directory DIR a build of the fuzz preset (default build/fuzz,
#                         configured and built here unless --no-build)
#   --work-directory DIR  seeds, the corpora the targets grow, crash
#                         artifacts and logs (default DIR/fuzz-work under
#                         the build directory); kept between runs
#   TARGET                the targets to run (default all: cast_fuzz
#                         regex_fuzz xml_parse_fuzz xpath_compile_fuzz
#                         xpath_evaluate_fuzz xquery_fuzz)
#
# Seeds come from tools/fuzz-seeds.py once per work directory. Each target
# runs with a 10 s timeout per input and a 2 GB memory limit; an input
# that crashes, leaks, trips a sanitizer or times out is written to
# WORK/artifacts/TARGET/ and the script exits 1 (timeouts of
# xpath_evaluate_fuzz and xquery_fuzz are listed for triage but do not fail, see P9 in
# docs/DECISIONS.md). Reproduce one with
#   BUILD/tests/fuzz/TARGET ARTIFACT
# Needs Clang with libFuzzer, cmake, ninja and python3.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
seconds=60
build_directory=""
work_directory=""
builds=true
while [ $# -gt 0 ]; do
    case $1 in
        --seconds) seconds=$2; shift 2 ;;
        --build-directory) build_directory=$2; shift 2 ;;
        --work-directory) work_directory=$2; shift 2 ;;
        --no-build) builds=false; shift ;;
        --) shift; break ;;
        -*) echo "unknown argument: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
targets="$*"
[ -n "$targets" ] || targets="cast_fuzz regex_fuzz xml_parse_fuzz xpath_compile_fuzz xpath_evaluate_fuzz xquery_fuzz"

if [ -z "$build_directory" ]; then
    build_directory="$repository_root/build/fuzz"
    if [ "$builds" = true ]; then
        (cd "$repository_root" && cmake --preset fuzz >/dev/null && cmake --build --preset fuzz)
    fi
elif [ "$builds" = true ]; then
    cmake --build "$build_directory"
fi
[ -n "$work_directory" ] || work_directory="$build_directory/fuzz-work"

if [ ! -d "$work_directory/seeds" ]; then
    python3 "$repository_root/tools/fuzz-seeds.py" "$work_directory/seeds.partial"
    mv "$work_directory/seeds.partial" "$work_directory/seeds"
fi

pids=""
for target in $targets; do
    executable="$build_directory/tests/fuzz/$target"
    [ -x "$executable" ] || { echo "no fuzz target $executable" >&2; exit 2; }
    maximum_byte_count=4096
    [ "$target" = xml_parse_fuzz ] && maximum_byte_count=16384
    corpus="$work_directory/corpus/$target"
    artifacts="$work_directory/artifacts/$target"
    mkdir -p "$corpus" "$artifacts"
    # The evaluator has no bound on steps yet (docs/DECISIONS.md, P9): an
    # expression that is slow by nature times out, so xpath_evaluate_fuzz
    # runs in fork mode, keeps going after a timeout and leaves the input
    # in its artifacts for triage.
    mode_options=""
    case $target in xpath_evaluate_fuzz | xquery_fuzz) mode_options="-fork=1 -ignore_timeouts=1" ;; esac
    echo "== $target for $seconds s (log: $work_directory/$target.log)"
    (
        # The first run of a work directory merges the seeds into a
        # corpus of the inputs that add coverage: tens of thousands of QT4
        # queries would otherwise take the whole time to read.
        if [ -z "$(ls -A "$corpus")" ]; then
            "$executable" -merge=1 -timeout=10 -rss_limit_mb=2048 \
                -max_len="$maximum_byte_count" \
                -artifact_prefix="$artifacts/" "$corpus" \
                "$work_directory/seeds/$target" >"$work_directory/$target.merge.log" 2>&1 ||
                true
        fi
        # shellcheck disable=SC2086
        "$executable" $mode_options -max_total_time="$seconds" -timeout=10 \
            -rss_limit_mb=2048 -max_len="$maximum_byte_count" -print_final_stats=1 \
            -artifact_prefix="$artifacts/" "$corpus"
    ) >"$work_directory/$target.log" 2>&1 &
    pids="$pids $!"
done

for pid in $pids; do
    wait "$pid" || true
done
failed=false
for target in $targets; do
    echo "== $target"
    grep -E '^(stat::number_of_executed_units|stat::peak_rss_mb)|^#[0-9]+.*DONE|^==[0-9]+==ERROR|^SUMMARY|^leak|does not round-trip|disagree' \
        "$work_directory/$target.log" || true
    for artifact in "$work_directory/artifacts/$target"/*; do
        [ -e "$artifact" ] || continue
        echo "   $artifact"
        case $(basename "$artifact") in
            timeout-*) case $target in xpath_evaluate_fuzz | xquery_fuzz) ;; *) failed=true ;; esac ;;
            *) failed=true ;;
        esac
    done
done
if [ "$failed" = true ]; then
    echo "a fuzz target failed; see the artifacts above" >&2
    exit 1
fi
