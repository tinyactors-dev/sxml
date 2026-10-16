#!/bin/sh
# Fetch the W3C QT4 and XSLT 4.0 conformance test suites at pinned commits
# into third_party/ (decision D13). Safe to run repeatedly: a checkout that
# is already at the pinned commit is left alone.
#
# Usage: tools/fetch-test-suites.sh [destination-directory]
#   destination-directory defaults to <repository>/third_party.

set -eu

QT4TESTS_URL="https://github.com/qt4cg/qt4tests"
QT4TESTS_COMMIT="166453359bd7770fb113b6d9c9015df7f7fa5d02"
XSLT40_TEST_URL="https://github.com/qt4cg/xslt40-test"
XSLT40_TEST_COMMIT="91328fbf0fed4a5d2813eb3661808ee53cb961ba"

repository_root=$(cd "$(dirname "$0")/.." && pwd)
destination=${1:-"$repository_root/third_party"}

fetch_suite() {
    suite_url=$1
    suite_commit=$2
    suite_directory=$3

    if [ -d "$suite_directory/.git" ]; then
        current_commit=$(git -C "$suite_directory" rev-parse HEAD 2>/dev/null || echo none)
        if [ "$current_commit" = "$suite_commit" ]; then
            echo "$suite_directory: already at $suite_commit"
            return 0
        fi
    else
        mkdir -p "$suite_directory"
        git -C "$suite_directory" init --quiet
        git -C "$suite_directory" remote add origin "$suite_url"
    fi

    echo "$suite_directory: fetching $suite_commit"
    git -C "$suite_directory" fetch --quiet --depth 1 origin "$suite_commit"
    git -C "$suite_directory" checkout --quiet --force FETCH_HEAD
}

mkdir -p "$destination"
fetch_suite "$QT4TESTS_URL" "$QT4TESTS_COMMIT" "$destination/qt4tests"
fetch_suite "$XSLT40_TEST_URL" "$XSLT40_TEST_COMMIT" "$destination/xslt40-test"
