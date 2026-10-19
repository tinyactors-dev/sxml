#!/bin/sh
# Runs the iXML community test suite (third_party/ixml-tests, packet 7.18,
# D215) with the sxml command: tests/ixml/ixml-driver.xq once per catalog
# that the suite's test-catalog.xml names, one process each, so a crash or
# a timeout ends only its own catalog. Prints the tests that did not pass
# and the counts of each catalog, then the totals.
#
# Usage: tests/ixml/run-ixml-tests.sh --program PATH [--timeout SECONDS]
#   --program PATH     the sxml executable (a release build is fastest)
#   --timeout SECONDS  limit per catalog (default 600; needs timeout(1))
# Exits 0 when every test that ran passed, 1 otherwise.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
program=""
timeout_seconds=600
while [ $# -gt 0 ]; do
    case $1 in
        --program) program=$2; shift 2 ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$program" ] || { echo "--program is required" >&2; exit 2; }
suite="$repository_root/third_party/ixml-tests"
driver="$repository_root/tests/ixml/ixml-driver.xq"
limit=""
if command -v timeout >/dev/null 2>&1; then
    limit="timeout $timeout_seconds"
fi

totals_file=$(mktemp)
trap 'rm -f "$totals_file"' EXIT
for catalog in $(sed -n 's/.*<test-set-ref href="\([^"]*\)".*/\1/p' "$suite/test-catalog.xml"); do
    if ! $limit "$program" xquery --param="catalog=file://$suite/$catalog" "$driver" \
            > "$totals_file.out" 2> "$totals_file.err"; then
        echo "error | $catalog | the driver stopped: $(head -c 400 "$totals_file.err")"
        echo "counts: $catalog stopped" >> "$totals_file"
        continue
    fi
    cat "$totals_file.out"
    echo
    grep '^counts: ' "$totals_file.out" >> "$totals_file" || true
done
rm -f "$totals_file.out" "$totals_file.err"
awk '
/stopped$/ { stopped += 1; next }
{
    for (field = 3; field <= NF; field += 1) {
        split($field, pair, "=")
        sum[pair[1]] += pair[2]
    }
}
END {
    printf "total: tests=%d passed=%d failed=%d errors=%d not-run=%d", sum["tests"], sum["passed"], sum["failed"], sum["errors"], sum["not-run"]
    printf " grammar-tests=%d grammar-passed=%d", sum["grammar-tests"], sum["grammar-passed"]
    printf " correct-grammar-cases=%d correct-grammar-passed=%d", sum["correct-grammar-cases"], sum["correct-grammar-passed"]
    printf " stopped-catalogs=%d\n", stopped
    exit (sum["failed"] + sum["errors"] + stopped > 0) ? 1 : 0
}' "$totals_file"
