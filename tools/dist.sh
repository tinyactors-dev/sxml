#!/bin/sh
# Builds the sxml command for every supported platform as installable
# archives (D256). Run it through mise, which pins the tools (mise.toml):
#
#   mise run dist                  every target, then SHA256SUMS
#   mise run dist:<target>         one target
#   mise run dist:checksums        SHA256SUMS over the archives there are
#   mise run install [-- PREFIX]   the build machine's target into PREFIX
#
#   mise run dist:verify           run what the build machine can run
#
# or directly: tools/dist.sh build TARGET | all | checksums | verify
#              | install [PREFIX] | targets
#
# Targets: x86_64-linux aarch64-linux (static, musl), x86_64-macos
# aarch64-macos, x86_64-windows aarch64-windows (static, MinGW-w64), all
# cross-compiled with `zig cc` (cmake/toolchains/zig.cmake); wasm32-wasi
# with the WASI SDK (cmake/wasi-sdk.cmake): WASI_SDK_PREFIX, else the one
# mise installed.
#
# Output, under SXML_DIST_DIRECTORY (default: build/dist):
#   build/<target>/                  the CMake build tree
#   sxml-<version>-<target>.tar.gz   (.zip for Windows), holding the
#                                    directory sxml-<version>-<target>/ with
#                                    the program, LICENSE, README.md and
#                                    USAGE.md (apps/sxml/README.md)
#   SHA256SUMS                       sha256sum's format, one line per archive
#
# The version is the one include/sxml/version.h declares.

set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
dist_directory=${SXML_DIST_DIRECTORY:-$repository_root/build/dist}
all_targets="x86_64-linux aarch64-linux x86_64-macos aarch64-macos x86_64-windows aarch64-windows wasm32-wasi"

fail() {
    echo "dist.sh: $*" >&2
    exit 1
}

read_version() {
    header="$repository_root/include/sxml/version.h"
    major=$(sed -n 's/^#define SXML_VERSION_MAJOR \([0-9]*\)$/\1/p' "$header")
    minor=$(sed -n 's/^#define SXML_VERSION_MINOR \([0-9]*\)$/\1/p' "$header")
    patch=$(sed -n 's/^#define SXML_VERSION_PATCH \([0-9]*\)$/\1/p' "$header")
    [ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || fail "no version in $header"
    echo "$major.$minor.$patch"
}

# The Zig target triple of a distribution target. macOS 11 is the oldest
# release that runs on Apple silicon, and the floor for both architectures.
zig_target_of() {
    case $1 in
        x86_64-linux | aarch64-linux) echo "$1-musl" ;;
        x86_64-macos | aarch64-macos) echo "$1.11.0" ;;
        x86_64-windows | aarch64-windows) echo "$1-gnu" ;;
        *) fail "unknown target '$1' (one of: $all_targets)" ;;
    esac
}

program_name_of() {
    case $1 in
        *-windows) echo "sxml.exe" ;;
        wasm32-wasi) echo "sxml.wasm" ;;
        *) echo "sxml" ;;
    esac
}

# The distribution target of the build machine, for `install`.
host_target() {
    case $(uname -m) in
        x86_64 | amd64) architecture=x86_64 ;;
        aarch64 | arm64) architecture=aarch64 ;;
        *) fail "no distribution target for the architecture $(uname -m)" ;;
    esac
    case $(uname -s) in
        Linux) echo "$architecture-linux" ;;
        Darwin) echo "$architecture-macos" ;;
        *) fail "no distribution target for the system $(uname -s)" ;;
    esac
}

configure_and_build() {
    target=$1
    build_directory="$dist_directory/build/$target"
    if [ "$target" = wasm32-wasi ]; then
        if [ -z "${WASI_SDK_PREFIX:-}" ] && command -v mise >/dev/null 2>&1; then
            mise_wasi_sdk=$(mise where github:WebAssembly/wasi-sdk 2>/dev/null || true)
            [ -z "$mise_wasi_sdk" ] || export WASI_SDK_PREFIX="$mise_wasi_sdk"
        fi
        # The wasm preset's build type (CMakePresets.json): MinSizeRel.
        set -- -DCMAKE_TOOLCHAIN_FILE="$repository_root/cmake/wasi-sdk.cmake" \
            -DCMAKE_BUILD_TYPE=MinSizeRel
    else
        # The release preset's build type.
        set -- -DCMAKE_TOOLCHAIN_FILE="$repository_root/cmake/toolchains/zig.cmake" \
            -DSXML_ZIG_TARGET="$(zig_target_of "$target")" -DCMAKE_BUILD_TYPE=Release
    fi
    # -s: no symbols or debug sections in the program (wasm-ld: the custom
    # sections, as javascript/size_budget measures the module).
    cmake -S "$repository_root" -B "$build_directory" -G Ninja \
        -DSXML_BUILD_TESTS=OFF -DSXML_BUILD_APPS=ON -DCMAKE_EXE_LINKER_FLAGS=-s "$@" >/dev/null
    cmake --build "$build_directory" --target sxml_cli
}

build_target() {
    target=$1
    [ "$target" = wasm32-wasi ] || zig_target_of "$target" >/dev/null
    version=$(read_version)
    name="sxml-$version-$target"
    configure_and_build "$target"
    # CMake names the WASI program "sxml"; the archive calls it sxml.wasm.
    case $target in
        *-windows) built_program="$dist_directory/build/$target/apps/sxml.exe" ;;
        *) built_program="$dist_directory/build/$target/apps/sxml" ;;
    esac
    [ -f "$built_program" ] || fail "the build made no $built_program"
    stage="$dist_directory/stage/$name"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$built_program" "$stage/$(program_name_of "$target")"
    cp "$repository_root/LICENSE" "$repository_root/README.md" "$stage/"
    cp "$repository_root/apps/sxml/README.md" "$stage/USAGE.md"
    # Owner, times and order fixed, so the archive depends only on its
    # files. The times are set on the staged files and the names listed
    # sorted, which both GNU tar and bsdtar (macOS) and zip accept; 1980
    # is the earliest time a zip archive can hold.
    find "$stage" -exec env TZ=UTC touch -h -t 198001010000.00 {} +
    case $target in
        *-windows)
            archive="$name.zip"
            rm -f "$dist_directory/$archive"
            (cd "$dist_directory/stage" && find "$name" | LC_ALL=C sort \
                | TZ=UTC zip -q -X -@ "../$archive") || fail "zip failed for $name"
            ;;
        *)
            archive="$name.tar.gz"
            case $(tar --version 2>/dev/null) in
                *GNU*) owner_options="--owner=0 --group=0" ;;
                *) owner_options="--uid 0 --gid 0 --uname root --gname wheel --no-xattrs --no-acls --no-mac-metadata" ;;
            esac
            unpacked_archive="$dist_directory/$name.tar"
            # shellcheck disable=SC2086
            (cd "$dist_directory/stage" && find "$name" | LC_ALL=C sort \
                | tar $owner_options --numeric-owner --format=ustar --no-recursion \
                    -cf "$unpacked_archive" -T -) || fail "tar failed for $name"
            gzip -n -9 -c "$unpacked_archive" >"$dist_directory/$archive"
            rm -f "$unpacked_archive"
            ;;
    esac
    rm -rf "$stage"
    rmdir "$dist_directory/stage" 2>/dev/null || true
    echo "dist.sh: $dist_directory/$archive"
}

write_checksums() {
    cd "$dist_directory"
    set -- sxml-*.tar.gz sxml-*.zip
    archives=""
    for archive in "$@"; do
        [ -f "$archive" ] && archives="$archives $archive"
    done
    [ -n "$archives" ] || fail "no archives in $dist_directory"
    # shellcheck disable=SC2086
    sha256sum $archives >SHA256SUMS
    cat SHA256SUMS
}

# Unpacks each archive there is and runs its program where the build
# machine can: natively, under qemu-user (qemu-aarch64 or
# qemu-aarch64-static on PATH), under wine (wine or wine64 on PATH) or
# under Node.js (tools/run-wasi.mjs). macOS programs are only checked with
# file(1). Each run is `sxml --version` and an `sxml xpath` query over a
# small document. Prints one line per target; fails when a run fails.
verify_archives() {
    version=$(read_version)
    work="$dist_directory/verify"
    rm -rf "$work"
    mkdir -p "$work"
    printf '<a><b>x</b><b>y</b></a>' >"$work/input.xml"
    host=$(host_target 2>/dev/null || echo none)
    failures=0
    for target in $all_targets; do
        name="sxml-$version-$target"
        case $target in
            *-windows) archive="$dist_directory/$name.zip" ;;
            *) archive="$dist_directory/$name.tar.gz" ;;
        esac
        if [ ! -f "$archive" ]; then
            echo "$target: no archive"
            continue
        fi
        case $target in
            *-windows) (cd "$work" && unzip -q -o "$archive") ;;
            *) tar -C "$work" -xzf "$archive" ;;
        esac
        program="$work/$name/$(program_name_of "$target")"
        runner=""
        case $target in
            "$host") runner="direct" ;;
            aarch64-linux) runner=$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true) ;;
            x86_64-linux) runner=$(command -v qemu-x86_64 || command -v qemu-x86_64-static || true) ;;
            x86_64-windows) runner=$(command -v wine || command -v wine64 || true) ;;
            wasm32-wasi) runner=$(command -v node || true) ;;
            *-macos)
                echo "$target: $(file -b "$program")"
                continue
                ;;
        esac
        if [ -z "$runner" ]; then
            echo "$target: not run (nothing here runs it); $(file -b "$program")"
            continue
        fi
        case $runner in
            direct) set -- "$program" ;;
            */node) set -- "$runner" --no-warnings "$repository_root/tools/run-wasi.mjs" "$program" ;;
            *) set -- "$runner" "$program" ;;
        esac
        # Relative names: the WASI module sees the working directory as ".".
        reported=$(cd "$work" && "$@" --version 2>&1 | tr -d '\r') || true
        result=$(cd "$work" && "$@" xpath --context-item=input.xml \
            'count(//b), //b[2]/string(), sum(1 to 10)' 2>&1 | tr -d '\r' | tr '\n' ' ') || true
        if [ "$reported" = "sxml $version" ] && [ "$result" = '2 "y" 55 ' ]; then
            echo "$target: ran ($runner): $reported; xpath gives $result"
        else
            echo "$target: FAILED ($runner): '$reported' / '$result'"
            failures=$((failures + 1))
        fi
    done
    rm -rf "$work"
    [ "$failures" -eq 0 ] || fail "$failures target(s) failed"
}

install_host() {
    prefix=${1:-${PREFIX:-$HOME/.local}}
    target=$(host_target)
    configure_and_build "$target"
    mkdir -p "$prefix/bin" "$prefix/share/doc/sxml"
    cp "$dist_directory/build/$target/apps/sxml" "$prefix/bin/sxml"
    chmod 755 "$prefix/bin/sxml"
    cp "$repository_root/LICENSE" "$repository_root/README.md" "$prefix/share/doc/sxml/"
    cp "$repository_root/apps/sxml/README.md" "$prefix/share/doc/sxml/USAGE.md"
    echo "dist.sh: installed $prefix/bin/sxml"
}

[ $# -ge 1 ] || fail "usage: dist.sh build TARGET | all | checksums | verify | install [PREFIX] | targets"
command=$1
shift
mkdir -p "$dist_directory"
case $command in
    build) [ $# -eq 1 ] || fail "usage: dist.sh build TARGET"; build_target "$1" ;;
    all) for target in $all_targets; do build_target "$target"; done; write_checksums ;;
    checksums) write_checksums ;;
    verify) verify_archives ;;
    install) install_host "$@" ;;
    targets) echo "$all_targets" ;;
    *) fail "unknown command '$command'" ;;
esac
