#!/bin/sh
# Build and test a private snapshot: committed HEAD plus the files you name.
#
# Several agents share one working tree, and the build picks up every
# src/*.c by glob (D45), so someone else's unfinished file can break your
# build. This script copies HEAD (without the vendored test suites, which are
# linked instead) into a directory of its own, overlays your files from the
# working tree, and runs configure, build and ctest for each preset there.
#
# Usage: tools/private-build.sh SNAPSHOT-DIRECTORY [--preset NAME]... [--lint] -- FILE...
#   Default presets: debug debug-gcc wasm. FILE paths are relative to the
#   repository root; deleted files may be named too (they are removed).
#   Example:
#     tools/private-build.sh "$SCRATCH/build-1.6" -- src/types.c tests/types_test.c

set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
snapshot=${1:?snapshot directory required}
shift
presets=""
run_lint=false
while [ $# -gt 0 ]; do
    case $1 in
        --preset) presets="$presets $2"; shift 2 ;;
        --lint) run_lint=true; shift ;;
        --) shift; break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$presets" ] || presets="debug debug-gcc wasm"

mkdir -p "$snapshot"
# Refresh the source copy but keep build/ so rebuilds are incremental.
find "$snapshot" -mindepth 1 -maxdepth 1 ! -name build -exec rm -rf {} +
git -C "$repository_root" archive HEAD -- . \
    ':(exclude)third_party/qt4tests' ':(exclude)third_party/xslt40-test' \
    ':(exclude)third_party/xmlconf' | tar -x -C "$snapshot"
for suite in qt4tests xslt40-test xmlconf; do
    ln -s "$repository_root/third_party/$suite" "$snapshot/third_party/$suite"
done
if [ -d "$repository_root/tools/node_modules" ]; then
    ln -s "$repository_root/tools/node_modules" "$snapshot/tools/node_modules"
fi

for file in "$@"; do
    if [ -e "$repository_root/$file" ]; then
        mkdir -p "$snapshot/$(dirname "$file")"
        cp -p "$repository_root/$file" "$snapshot/$file"
    else
        rm -f "$snapshot/$file"
    fi
done

cd "$snapshot"
for preset in $presets; do
    echo "== $preset"
    cmake --preset "$preset" >/dev/null
    cmake --build --preset "$preset"
    ctest --preset "$preset" --output-on-failure
done
if [ "$run_lint" = true ]; then
    tools/lint.sh
fi
