#!/bin/sh
# Runs the tests of the vendored tree-sitter grammars
# (third_party/tree-sitter-grammars/LANGUAGE/test, packet 7.22, D285) with
# the sxml command: tests/tree-sitter/tree-sitter-driver.xq once per file
# of test/corpus and test/highlight, one process each, so a crash or a
# timeout ends only its own file. Prints the tests that did not pass and
# the counts of each file, then the totals per kind and in all.
#
# Usage: tests/tree-sitter/run-tree-sitter-tests.sh --program PATH [--timeout SECONDS] [LANGUAGE...]
#   --program PATH     the sxml executable (a release build is fastest)
#   --timeout SECONDS  limit per file (default 300; needs timeout(1))
#   LANGUAGE...        only these grammars (default every vendored one)
# Exits 0 when every test that ran passed, 1 otherwise.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
program=""
timeout_seconds=300
while [ $# -gt 0 ]; do
    case $1 in
        --program) program=$2; shift 2 ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        --*) echo "unknown argument: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ -n "$program" ] || { echo "--program is required" >&2; exit 2; }
grammars="$repository_root/third_party/tree-sitter-grammars"
driver="$repository_root/tests/tree-sitter/tree-sitter-driver.xq"
limit=""
if command -v timeout >/dev/null 2>&1; then
    limit="timeout $timeout_seconds"
fi
languages="$*"
if [ -z "$languages" ]; then
    languages=$(cd "$grammars" && for directory in */; do printf '%s ' "${directory%/}"; done)
fi

totals_file=$(mktemp)
trap 'rm -f "$totals_file" "$totals_file.out" "$totals_file.err"' EXIT
for language in $languages; do
    for kind in corpus highlight; do
        [ -d "$grammars/$language/test/$kind" ] || continue
        for file in "$grammars/$language/test/$kind"/*; do
            [ -f "$file" ] || continue
            if ! $limit "$program" xquery --param="language=$language" --param="kind=$kind" \
                    --param="file=file://$file" "$driver" \
                    > "$totals_file.out" 2> "$totals_file.err"; then
                echo "error | $language | $(basename "$file") | the driver stopped: $(head -c 400 "$totals_file.err")"
                echo "$kind stopped" >> "$totals_file"
                continue
            fi
            cat "$totals_file.out"
            echo
            sed -n "s/^counts: /$kind /p" "$totals_file.out" >> "$totals_file"
        done
    done
done
awk '
$2 == "stopped" { stopped += 1; next }
{
    for (field = 3; field <= NF; field += 1) {
        split($field, pair, "=")
        sum[$1 " " pair[1]] += pair[2]
        sum["total " pair[1]] += pair[2]
    }
}
END {
    split("corpus highlight total", kinds, " ")
    for (index_number = 1; index_number <= 3; index_number += 1) {
        kind = kinds[index_number]
        printf "%s: tests=%d passed=%d failed=%d errors=%d not-run=%d\n", kind, sum[kind " tests"], sum[kind " passed"], sum[kind " failed"], sum[kind " errors"], sum[kind " not-run"]
    }
    printf "stopped-files=%d\n", stopped
    exit (sum["total failed"] + sum["total errors"] + stopped > 0) ? 1 : 0
}' "$totals_file"
