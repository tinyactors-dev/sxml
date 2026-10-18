#!/bin/sh
# Check the health of committed HEAD: build a clean snapshot (no files from
# the working tree) and run every preset's build and ctest, then the full
# lint (tools/lint.sh). Unlike tools/private-build.sh it does not stop at
# the first failure; it prints a summary line per step and exits non-zero
# if any step failed.
#
# Usage: tools/check-main.sh SNAPSHOT-DIRECTORY [PRESET]...
#   Default presets: debug debug-gcc release wasm. Logs go to
#   SNAPSHOT-DIRECTORY/logs/<step>.log. Rerunning reuses build/ so rebuilds
#   are incremental.

set -u

repository_root=$(cd "$(dirname "$0")/.." && pwd)
snapshot=${1:?snapshot directory required}
shift
presets=${*:-"debug debug-gcc release wasm"}

mkdir -p "$snapshot"
find "$snapshot" -mindepth 1 -maxdepth 1 ! -name build -exec rm -rf {} +
git -C "$repository_root" archive HEAD -- . \
    ':(exclude)third_party/qt4tests' ':(exclude)third_party/xslt40-test' \
    ':(exclude)third_party/xmlconf' ':(exclude)third_party/xsdtests' | tar -x -C "$snapshot" || exit 1
for suite in qt4tests xslt40-test xmlconf xsdtests; do
    ln -s "$repository_root/third_party/$suite" "$snapshot/third_party/$suite"
done
if [ -d "$repository_root/tools/node_modules" ]; then
    ln -s "$repository_root/tools/node_modules" "$snapshot/tools/node_modules"
fi
mkdir -p "$snapshot/logs"
echo "HEAD $(git -C "$repository_root" rev-parse --short HEAD)"

cd "$snapshot" || exit 1
summary=""
failed=0
record() {
    summary="$summary
$1: $2"
    [ "$2" = ok ] || failed=1
}

for preset in $presets; do
    log="logs/$preset.log"
    if ! cmake --preset "$preset" >"$log" 2>&1; then
        record "$preset" "configure FAILED ($log)"
    elif ! cmake --build --preset "$preset" >>"$log" 2>&1; then
        record "$preset" "build FAILED ($log)"
    elif ! ctest --preset "$preset" --output-on-failure >>"$log" 2>&1; then
        record "$preset" "ctest FAILED ($log)"
    else
        record "$preset" ok
    fi
done
if tools/lint.sh >logs/lint.log 2>&1; then
    record lint ok
else
    record lint "FAILED (logs/lint.log)"
fi

echo "$summary" | sed '1d'
exit $failed
