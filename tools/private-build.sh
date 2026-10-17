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
# Assemble HEAD plus the named files in a staging directory, then copy into
# the snapshot only the files whose content differs. `git archive` stamps
# every file with the commit time, so copying it over blindly can leave a
# changed file older than its object file, and the build would not see it.
# Unchanged files keep their old times, so rebuilds stay incremental.
staging="$snapshot.staging"
rm -rf "$staging"
mkdir -p "$staging"
git -C "$repository_root" archive HEAD -- . \
    ':(exclude)third_party/qt4tests' ':(exclude)third_party/xslt40-test' \
    ':(exclude)third_party/xmlconf' | tar -x -C "$staging"
for file in "$@"; do
    if [ -e "$repository_root/$file" ]; then
        mkdir -p "$staging/$(dirname "$file")"
        cp "$repository_root/$file" "$staging/$file"
    else
        rm -f "$staging/$file"
    fi
done
python3 - "$staging" "$snapshot" <<'PYTHON'
import filecmp, os, shutil, sys
staging, snapshot = sys.argv[1], sys.argv[2]
kept = set()
for directory, _, files in os.walk(staging):
    for name in files:
        source = os.path.join(directory, name)
        relative = os.path.relpath(source, staging)
        kept.add(relative)
        target = os.path.join(snapshot, relative)
        if os.path.isfile(target) and not os.path.islink(target) \
                and filecmp.cmp(source, target, shallow=False):
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if os.path.lexists(target):
            os.remove(target)
        shutil.copyfile(source, target)
        shutil.copymode(source, target)
# Remove files that are no longer part of the snapshot (build/ and the
# linked suites excepted).
for directory, subdirectories, files in os.walk(snapshot):
    relative_directory = os.path.relpath(directory, snapshot)
    if relative_directory == ".":
        subdirectories[:] = [d for d in subdirectories if d != "build"]
    subdirectories[:] = [d for d in subdirectories
                         if not os.path.islink(os.path.join(directory, d))]
    for name in files:
        path = os.path.join(directory, name)
        relative = os.path.normpath(os.path.join(relative_directory, name))
        if not os.path.islink(path) and relative not in kept:
            os.remove(path)
PYTHON
rm -rf "$staging"
for suite in qt4tests xslt40-test xmlconf; do
    [ -e "$snapshot/third_party/$suite" ] || \
        ln -s "$repository_root/third_party/$suite" "$snapshot/third_party/$suite"
done
if [ -d "$repository_root/tools/node_modules" ] && [ ! -e "$snapshot/tools/node_modules" ]; then
    ln -s "$repository_root/tools/node_modules" "$snapshot/tools/node_modules"
fi

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
