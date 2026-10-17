#!/bin/sh
# Runs the QT4 test suite with sxml-qt4, one process per test set, and
# writes one JSON report per test set (packet 1.27, D92). A crash, a
# sanitizer report or a timeout ends only its own test set's process; it is
# recorded and the run goes on. tools/conformance-report.py turns the
# directory into docs/CONFORMANCE.md.
#
# Usage: tools/run-qt4.sh --program PATH [--output DIR] [--spec XP40|XQ40]
#                         [--jobs N] [--timeout SECONDS]
#   --program PATH     the sxml-qt4 executable (a release build is fastest)
#   --output DIR       where the reports go (default build/qt4-run); emptied
#   --spec NAME        the specification to run (default XP40)
#   --jobs N           test sets run at once (default: the processor count)
#   --timeout SECONDS  limit per test set (default 600; needs timeout(1))
#
# Writes, under DIR:
#   SPEC/NAME.json   the report of test set NAME (sxml-qt4 run --format=json)
#   SPEC/NAME.exit   its exit code (0 and 1 are a finished run)
#   SPEC/NAME.stderr its standard error, kept when not empty
#   slice.json       the torture slice (tests/qt4/torture-slice.txt)
#   slice.exit
#   run.txt          the commit and its date (see tools/conformance-report.py)
# Run from anywhere; the catalog is third_party/qt4tests/catalog.xml.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
program=""
output_directory="$repository_root/build/qt4-run"
spec=XP40
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
timeout_seconds=600
while [ $# -gt 0 ]; do
    case $1 in
        --program) program=$2; shift 2 ;;
        --output) output_directory=$2; shift 2 ;;
        --spec) spec=$2; shift 2 ;;
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

rm -rf "$output_directory/$spec"
mkdir -p "$output_directory/$spec"
cd "$repository_root"

# The commit the code under test comes from: the last commit outside docs/,
# so that committing a regenerated docs/CONFORMANCE.md does not change it.
{
    echo "commit $(git log -1 --format=%H -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
    echo "date $(git log -1 --format=%cs -- . ':(exclude)docs' 2>/dev/null || echo unknown)"
} > "$output_directory/run.txt"

# Test-set names in catalog order.
sed -n 's/.*<test-set[^>]*name="\([^"]*\)".*/\1/p' third_party/qt4tests/catalog.xml \
    > "$output_directory/$spec.test-sets"

# One test set per process: report, exit code, standard error.
xargs -P "$jobs" -I {} sh -c '
    base="$0/$1/$4"
    $2 "$3" run "--spec=$1" "--test-set=$4" --format=json > "$base.json" 2> "$base.stderr"
    echo $? > "$base.exit"
    [ -s "$base.stderr" ] || rm -f "$base.stderr"
' "$output_directory" "$spec" "$timeout_command" "$program" {} \
    < "$output_directory/$spec.test-sets"

set +e
$timeout_command "$program" run tests/qt4/torture-slice.txt --format=json \
    > "$output_directory/slice.json" 2> "$output_directory/slice.stderr"
echo $? > "$output_directory/slice.exit"
set -e
[ -s "$output_directory/slice.stderr" ] || rm -f "$output_directory/slice.stderr"

crashed=0
while read -r name; do
    code=$(cat "$output_directory/$spec/$name.exit")
    if [ "$code" != 0 ] && [ "$code" != 1 ]; then
        echo "test set $name: exit $code" >&2
        crashed=$((crashed + 1))
    fi
done < "$output_directory/$spec.test-sets"
echo "$spec: $(wc -l < "$output_directory/$spec.test-sets") test sets, $crashed did not finish" >&2
