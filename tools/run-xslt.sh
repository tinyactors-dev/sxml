#!/bin/sh
# Runs the XSLT 4.0 test suite with sxml-xslt-test, one process per test
# set, and writes one JSON report per test set (packet 4.15, D157), as
# tools/run-qt4.sh does for QT4: a crash, a sanitizer report or a timeout
# ends only its own test set's process; it is recorded and the run goes on.
# tools/conformance-report.py reads the directory (spec XT40).
#
# Usage: tools/run-xslt.sh --program PATH [--output DIR] [--jobs N]
#                          [--timeout SECONDS]
#   --program PATH     the sxml-xslt-test executable (a release build is fastest)
#   --output DIR       where the reports go (default build/qt4-run, beside
#                      the QT4 reports); its XT40/ is emptied
#   --jobs N           test sets run at once (default: the processor count)
#   --timeout SECONDS  limit per test set (default 600; needs timeout(1))
#
# Writes, under DIR:
#   XT40/NAME.json    the report of test set NAME (sxml-xslt-test run --format=json)
#   XT40/NAME.exit    its exit code (0 and 1 are a finished run)
#   XT40/NAME.stderr  its standard error, kept when not empty
#   XT40.test-sets    the test-set names in catalog order
#   xslt-slice.json   the XSLT slice (tests/xslt/slice.txt)
#   xslt-slice.exit
#   run.txt           the commit and its date
# Run from anywhere; the catalog is third_party/xslt40-test/catalog.xml.
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

spec=XT40
rm -rf "$output_directory/$spec"
mkdir -p "$output_directory/$spec"
cd "$repository_root"

{
    echo "commit $(git log -1 --format=%H -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
    echo "date $(git log -1 --format=%cs -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
} > "$output_directory/run.txt"

# Test-set names in catalog order (a commented-out entry is none).
grep -v '<!--' third_party/xslt40-test/catalog.xml |
    sed -n 's/.*<test-set[^>]*name="\([^"]*\)".*/\1/p' > "$output_directory/$spec.test-sets"

xargs -P "$jobs" -I {} sh -c '
    base="$0/$1/$4"
    $2 "$3" run "--test-set=$4" --format=json > "$base.json" 2> "$base.stderr"
    echo $? > "$base.exit"
    [ -s "$base.stderr" ] || rm -f "$base.stderr"
' "$output_directory" "$spec" "$timeout_command" "$program" {} \
    < "$output_directory/$spec.test-sets"

set +e
$timeout_command "$program" run tests/xslt/slice.txt --format=json \
    > "$output_directory/xslt-slice.json" 2> "$output_directory/xslt-slice.stderr"
echo $? > "$output_directory/xslt-slice.exit"
set -e
[ -s "$output_directory/xslt-slice.stderr" ] || rm -f "$output_directory/xslt-slice.stderr"

crashed=0
while read -r name; do
    code=$(cat "$output_directory/$spec/$name.exit")
    if [ "$code" != 0 ] && [ "$code" != 1 ]; then
        echo "test set $name: exit $code" >&2
        crashed=$((crashed + 1))
    fi
done < "$output_directory/$spec.test-sets"
echo "$spec: $(wc -l < "$output_directory/$spec.test-sets") test sets, $crashed did not finish" >&2
