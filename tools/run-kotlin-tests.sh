#!/bin/sh
# Compile and run the Kotlin binding's tests on the JVM (ctest kotlin/jvm;
# D238): bindings/kotlin/src/main/kotlin and src/test/kotlin, compiled by
# kotlinc with kotlin.test, run by their own main against libsxml_kotlin.so.
# No Gradle and no network.
#
# Usage: tools/run-kotlin-tests.sh LIBRARY OUTPUT-DIRECTORY [FILTER]
#   LIBRARY           libsxml_kotlin.so (empty: JNI was not found; skipped)
#   OUTPUT-DIRECTORY  where the compiled tests go (recompiled when a source
#                     is newer)
#   FILTER            only the cases whose "suite > case" name contains it
# Environment:
#   SXML_KOTLINC          kotlinc (default: kotlinc on PATH)
#   JAVA_HOME             the JDK whose java runs the tests (default: PATH)
#   SXML_KOTLIN_FEATURES  the features the library must list
#   SXML_KOTLIN_PRELOAD   a sanitizer runtime the JVM must load first
#   SXML_KOTLIN_RESOURCES a directory of class path resources (the DUCET,
#                         dev/tinyactors/sxml/allkeys.txt)
#
# Exits 77 (ctest's skip) with the reason when JNI, kotlinc or java is
# missing.

set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
library=${1-}
output=${2:?output directory required}
filter=${3-}

if [ -z "$library" ]; then
    echo "kotlin/jvm skipped: JNI headers were not found when CMake configured (install a JDK)"
    exit 77
fi
kotlinc=${SXML_KOTLINC:-$(command -v kotlinc || true)}
if [ -z "$kotlinc" ] || [ ! -x "$kotlinc" ]; then
    echo "kotlin/jvm skipped: kotlinc not found (set SXML_KOTLINC or put kotlinc on PATH)"
    exit 77
fi
if [ -n "${JAVA_HOME-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
    java=$JAVA_HOME/bin/java
else
    java=$(command -v java || true)
fi
if [ -z "$java" ]; then
    echo "kotlin/jvm skipped: java not found (set JAVA_HOME or put java on PATH)"
    exit 77
fi

kotlin_home=$(cd "$(dirname "$(readlink -f "$kotlinc")")/.." && pwd)
kotlin_libraries="$kotlin_home/lib/kotlin-stdlib.jar:$kotlin_home/lib/kotlin-test.jar"
binding="$repository_root/bindings/kotlin"
jar="$output/sxml-kotlin-tests.jar"

mkdir -p "$output"
if [ ! -f "$jar" ] || [ -n "$(find "$binding/src" -name '*.kt' -newer "$jar" | head -n 1)" ]; then
    echo "kotlinc: compiling the binding and its tests"
    rm -rf "$output/part"
    mkdir -p "$output/part"
    # shellcheck disable=SC2046 # The source paths have no spaces.
    "$kotlinc" -nowarn -classpath "$kotlin_home/lib/kotlin-test.jar" -d "$output/part/tests.jar" \
        $(find "$binding/src/main/kotlin" "$binding/src/test/kotlin" -name '*.kt' | sort)
    mv "$output/part/tests.jar" "$jar"
    rm -rf "$output/part"
fi

log="$output/kotlin-tests.log"
status=0
# -Xcheck:jni checks every JNI call of the glue; its warnings fail the test.
# Under the sanitizers the JVM keeps its own SIGSEGV handling, and
# LeakSanitizer skips what the JVM itself leaves allocated at exit
# (bindings/kotlin/lsan-jvm.supp) but reports any leak of the binding.
env ${SXML_KOTLIN_PRELOAD:+LD_PRELOAD="$SXML_KOTLIN_PRELOAD"} \
    ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}handle_segv=0:allow_user_segv_handler=1" \
    LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}suppressions=$binding/lsan-jvm.supp" \
    "$java" -Xcheck:jni -Dsxml.library.path="$library" -Dsxml.sourceDirectory="$repository_root" \
    -classpath "$jar:$kotlin_libraries${SXML_KOTLIN_RESOURCES:+:$SXML_KOTLIN_RESOURCES}" dev.tinyactors.sxml.TestMainKt "$filter" >"$log" 2>&1 || status=$?
cat "$log"
if grep -q "WARNING in native method\|FATAL ERROR in native method" "$log"; then
    echo "kotlin/jvm: -Xcheck:jni reported a misuse of JNI"
    exit 1
fi
exit "$status"
