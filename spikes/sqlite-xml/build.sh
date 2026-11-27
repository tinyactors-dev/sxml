#!/bin/sh
# Builds xml.so against an sxml build directory (default: build/release,
# made with `cmake --preset release && ninja -C build/release sxml sxml_host`).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${1:-$root/build/release}
libraries=$(find "$build" -name '*.a' | tr '\n' ' ')
clang -O2 -fPIC -shared -std=c11 -Wall -Wextra -Wno-unused-function \
    -I"$root/include" -I"$build/include" -I"$root/bindings/host" \
    -o "$here/xml.so" "$here/sqlite_xml.c" "$here/xdmb.c" \
    -Wl,--start-group $libraries -Wl,--end-group -lm
