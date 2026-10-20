#!/bin/sh
# Build sxml for Android with the NDK (packet 6.3, D238): the library, the
# unit tests (built, not run) and libsxml_kotlin.so (the library, the host
# shim and the JNI glue, D247) for arm64-v8a, armeabi-v7a, x86_64 and x86
# at API level 24 (minSdk), MinSizeRel, with -Werror, through the
# android-* presets of CMakePresets.json. Then it checks the 16 KB page
# alignment of the 64-bit libraries (Android 15) and writes a size report
# of libsxml_kotlin.so, stripped and gzip -9.
#
# Nothing runs on a device or an emulator here: that needs an Android
# device or a host with KVM (the owner's machine, packet 6.6).
#
# Usage: tools/build-android.sh [BUILD-DIRECTORY] [ABI...]
#   BUILD-DIRECTORY  default build/android; one subdirectory per ABI, and
#                    sizes.txt, the report
#   ABI              default: arm64-v8a armeabi-v7a x86_64 x86
# Environment:
#   ANDROID_NDK_HOME  the NDK, r28c (tools/fetch-binding-toolchains.sh ndk)
#   SXML_ANDROID_TARGETS  the targets to build (default: all)
#
# Exits nonzero when a build fails, a 64-bit library is not 16 KB aligned,
# or the NDK is missing.

set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
build_directory=${1:-"$repository_root/build/android"}
[ $# -gt 0 ] && shift
abis=${*:-"arm64-v8a armeabi-v7a x86_64 x86"}
targets=${SXML_ANDROID_TARGETS:-all}

if [ -z "${ANDROID_NDK_HOME-}" ] || \
        [ ! -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" ]; then
    echo "build-android: set ANDROID_NDK_HOME to an NDK r28c" \
        "(tools/fetch-binding-toolchains.sh ndk DIRECTORY)" >&2
    exit 2
fi
case $build_directory in
    /*) ;;
    *) build_directory="$(pwd)/$build_directory" ;;
esac
mkdir -p "$build_directory"

llvm_bin=$(echo "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin)
strip_tool="$llvm_bin/llvm-strip"
readelf_tool="$llvm_bin/llvm-readelf"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

report="$build_directory/sizes.txt"
{
    echo "# libsxml_kotlin.so, MinSizeRel, API 24: stripped and gzip -9 bytes"
    printf '%-12s %12s %12s %s\n' "abi" "stripped" "gzip-9" "load-align"
} >"$report.new"

cd "$repository_root"
status=0
for abi in $abis; do
    echo "== android-$abi"
    abi_directory="$build_directory/$abi"
    cmake --preset "android-$abi" -B "$abi_directory" >/dev/null
    cmake --build "$abi_directory" --target $targets -j "$jobs"

    library="$abi_directory/bindings/kotlin/libsxml_kotlin.so"
    stripped="$abi_directory/libsxml_kotlin.stripped.so"
    "$strip_tool" --strip-all -o "$stripped" "$library"
    stripped_bytes=$(wc -c <"$stripped" | tr -d ' ')
    gzip_bytes=$(gzip -9 -c "$stripped" | wc -c | tr -d ' ')
    # The largest alignment of the LOAD segments, as readelf prints it.
    alignment=$("$readelf_tool" -lW "$library" |
        awk '$1 == "LOAD" { print $NF }' | sort -u | tr '\n' ' ')
    printf '%-12s %12s %12s %s\n' "$abi" "$stripped_bytes" "$gzip_bytes" \
        "$alignment" >>"$report.new"

    # Android 15 needs 16 KB pages of 64-bit apps; r28c links them so.
    case $abi in
        arm64-v8a | x86_64)
            for value in $alignment; do
                if [ "$value" != 0x4000 ]; then
                    echo "build-android: $abi LOAD alignment $value, not 0x4000" >&2
                    status=1
                fi
            done
            ;;
    esac
    # Only the JNI entry points are exported (sxml_kotlin.map).
    if "$llvm_bin/llvm-nm" -D --defined-only "$library" |
            awk '{ print $NF }' | grep -v -e '^Java_' -e '^JNI_OnLoad$' |
            grep -q .; then
        echo "build-android: $abi exports symbols other than the JNI ones" >&2
        status=1
    fi
done
mv "$report.new" "$report"
cat "$report"
exit $status
