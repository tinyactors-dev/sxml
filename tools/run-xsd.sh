#!/bin/sh
# Runs the W3C XSD test suite with sxml-xsd-test, one process per test
# set, and writes one JSON report per test set (packet 5.3, D168), as
# tools/run-xslt.sh does for XSLT: a crash, a sanitizer report or a timeout
# ends only its own test set's process; it is recorded and the run goes on.
# tools/conformance-report.py reads the directory (spec XSD11).
#
# Usage: tools/run-xsd.sh --program PATH [--output DIR] [--jobs N]
#                         [--timeout SECONDS]
#   --program PATH     the sxml-xsd-test executable (a release build is fastest)
#   --output DIR       where the reports go (default build/qt4-run, beside
#                      the QT4 reports); its XSD11/ is emptied
#   --jobs N           test sets run at once (default: the processor count)
#   --timeout SECONDS  limit per test set (default 600; needs timeout(1))
#
# Writes, under DIR:
#   XSD11/NAME.json   the report of test set NAME (sxml-xsd-test run --format=json)
#   XSD11/NAME.exit   its exit code (0 and 1 are a finished run)
#   XSD11/NAME.stderr its standard error, kept when not empty
#   XSD11.test-sets   the test-set names in suite.xml order
#   xsd-slice.json    the XSD slice (tests/xsd/slice.txt)
#   xsd-slice.exit
#   run.txt           the commit and its date
# Run from anywhere; the catalog is third_party/xsdtests/suite.xml.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
program=""
output_directory="$repository_root/build/qt4-run"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
timeout_seconds=600
while [ $# -gt 0 ]; do
    case $1 in
        --program) program=$2; shift 2 ;;
        --output) output_directory=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$program" ] || { echo "--program is required" >&2; exit 2; }
case $program in /*) ;; *) program="$(pwd)/$program" ;; esac
[ -x "$program" ] || { echo "not an executable: $program" >&2; exit 2; }
case $output_directory in /*) ;; *) output_directory="$(pwd)/$output_directory" ;; esac

timeout_command=""
if command -v timeout >/dev/null 2>&1; then
    timeout_command="timeout $timeout_seconds"
fi

spec=XSD11
rm -rf "$output_directory/$spec"
mkdir -p "$output_directory/$spec"
cd "$repository_root"

{
    echo "commit $(git log -1 --format=%H -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
    echo "date $(git log -1 --format=%cs -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
} > "$output_directory/run.txt"

# Test-set names in suite.xml order, by the driver's rule
# (tests/xsd/driver/xsd_catalog.h, "Names"): of "saxonMeta/Assert.testSet",
# "saxon-Assert".
grep -v '<!--' third_party/xsdtests/suite.xml |
    sed -n 's/.*<ts:testSetRef[^>]*xlink:href="\([^"]*\)".*/\1/p' |
    sed 's|^\(.*/\)\{0,1\}\([^/]*\)/\([^/]*\)$|\2-\3|; s/Meta-/-/; s/\.[^.-]*$//' \
    > "$output_directory/$spec.test-sets"

xargs -P "$jobs" -I {} sh -c '
    base="$0/$1/$4"
    $2 "$3" run "--test-set=$4" --format=json > "$base.json" 2> "$base.stderr"
    echo $? > "$base.exit"
    [ -s "$base.stderr" ] || rm -f "$base.stderr"
' "$output_directory" "$spec" "$timeout_command" "$program" {} \
    < "$output_directory/$spec.test-sets"

set +e
$timeout_command "$program" run tests/xsd/slice.txt --format=json \
    > "$output_directory/xsd-slice.json" 2> "$output_directory/xsd-slice.stderr"
echo $? > "$output_directory/xsd-slice.exit"
set -e
[ -s "$output_directory/xsd-slice.stderr" ] || rm -f "$output_directory/xsd-slice.stderr"

crashed=0
while read -r name; do
    code=$(cat "$output_directory/$spec/$name.exit")
    if [ "$code" != 0 ] && [ "$code" != 1 ]; then
        echo "test set $name: exit $code" >&2
        crashed=$((crashed + 1))
    fi
done < "$output_directory/$spec.test-sets"
echo "$spec: $(wc -l < "$output_directory/$spec.test-sets") test sets, $crashed did not finish" >&2
