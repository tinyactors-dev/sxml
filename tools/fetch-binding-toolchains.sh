#!/bin/sh
# Fetch the pinned toolchains of the bindings (D238) into a directory the
# caller names, checking each download's SHA-256 sum (packet 6.3).
#
# Usage: tools/fetch-binding-toolchains.sh DIRECTORY TOOL...
#   TOOL  ndk          Android NDK r28c          DIRECTORY/android-ndk-r28c
#         android-sdk  cmdline-tools 19.0, platform 35, build-tools 35.0.0
#                                                DIRECTORY/android-sdk
#         kotlin       Kotlin 2.2.20 compiler    DIRECTORY/kotlinc
#         swift        Swift 6.2 (Ubuntu 24.04)  DIRECTORY/swift-6.2-RELEASE-ubuntu24.04
#
# The downloads are Linux x86_64 builds. A tool already in DIRECTORY is
# kept. At the end the script prints the environment to set, for example
# ANDROID_NDK_HOME for tools/build-android.sh. JDK 21 and Gradle 8.14.3 are
# not fetched: they come with the system.
#
# Sizes unpacked: the NDK 2.2 GB, Swift 3.2 GB, the SDK 0.5 GB, kotlinc
# 83 MB. Agents fetch into their scratchpad and delete the tools afterwards,
# because the disk is shared.

set -eu

directory=${1:?directory required}
shift
[ $# -gt 0 ] || { echo "fetch-binding-toolchains: name a tool: ndk android-sdk kotlin swift" >&2; exit 2; }
mkdir -p "$directory"
directory=$(cd "$directory" && pwd)

ndk_url=https://dl.google.com/android/repository/android-ndk-r28c-linux.zip
ndk_sum=dfb20d396df28ca02a8c708314b814a4d961dc9074f9a161932746f815aa552f
sdk_url=https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip
sdk_sum=7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee
kotlin_url=https://github.com/JetBrains/kotlin/releases/download/v2.2.20/kotlin-compiler-2.2.20.zip
kotlin_sum=81f0264c9073b5cbbdb3ff8418cf2c5dac076879fc156fa1a6462f5a5acc4420
swift_url=https://download.swift.org/swift-6.2-release/ubuntu2404/swift-6.2-RELEASE/swift-6.2-RELEASE-ubuntu24.04.tar.gz
swift_sum=8e3d63a3371caad495e694414b9b9a22c5f68c6473077406c7296472a41bc077

# download URL SUM FILE: fetch URL to FILE (with retries; Maven Central and
# others answer 429 at times) and check its SHA-256 sum.
download() {
    echo "fetching $1" >&2
    curl -sSfL --retry 5 --retry-delay 10 -o "$3" "$1"
    actual=$(sha256sum "$3" | cut -d ' ' -f 1)
    if [ "$actual" != "$2" ]; then
        echo "fetch-binding-toolchains: $1: SHA-256 $actual, expected $2" >&2
        rm -f "$3"
        exit 1
    fi
}

environment=""
for tool in "$@"; do
    case $tool in
        ndk)
            if [ ! -d "$directory/android-ndk-r28c" ]; then
                download "$ndk_url" "$ndk_sum" "$directory/ndk.zip"
                unzip -q "$directory/ndk.zip" -d "$directory"
                rm -f "$directory/ndk.zip"
            fi
            environment="$environment
ANDROID_NDK_HOME=$directory/android-ndk-r28c"
            ;;
        android-sdk)
            sdk="$directory/android-sdk"
            if [ ! -d "$sdk/cmdline-tools/latest" ]; then
                download "$sdk_url" "$sdk_sum" "$directory/cmdline-tools.zip"
                mkdir -p "$sdk/cmdline-tools"
                unzip -q "$directory/cmdline-tools.zip" -d "$sdk/cmdline-tools"
                mv "$sdk/cmdline-tools/cmdline-tools" "$sdk/cmdline-tools/latest"
                rm -f "$directory/cmdline-tools.zip"
            fi
            # sdkmanager checks the sums of what it installs.
            yes | "$sdk/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$sdk" \
                --licenses >/dev/null
            "$sdk/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$sdk" \
                "platforms;android-35" "build-tools;35.0.0" "platform-tools" >/dev/null
            environment="$environment
ANDROID_HOME=$sdk"
            ;;
        kotlin)
            if [ ! -d "$directory/kotlinc" ]; then
                download "$kotlin_url" "$kotlin_sum" "$directory/kotlin.zip"
                unzip -q "$directory/kotlin.zip" -d "$directory"
                rm -f "$directory/kotlin.zip"
            fi
            environment="$environment
SXML_KOTLINC=$directory/kotlinc/bin/kotlinc"
            ;;
        swift)
            swift="$directory/swift-6.2-RELEASE-ubuntu24.04"
            if [ ! -d "$swift" ]; then
                download "$swift_url" "$swift_sum" "$directory/swift.tar.gz"
                tar -xzf "$directory/swift.tar.gz" -C "$directory"
                rm -f "$directory/swift.tar.gz"
            fi
            environment="$environment
PATH=$swift/usr/bin:\$PATH"
            ;;
        *)
            echo "fetch-binding-toolchains: unknown tool $tool (ndk android-sdk kotlin swift)" >&2
            exit 2
            ;;
    esac
done
echo "# Set:$environment"
