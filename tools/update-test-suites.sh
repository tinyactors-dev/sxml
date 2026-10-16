#!/bin/sh
# Refresh the vendored W3C QT4, XSLT 4.0 and XML 1.0 conformance test suites in
# third_party/ (decision D13). The suites are committed to the repository, so
# this is only needed to move to a new pinned commit: edit the commits below,
# run the script, review the diff and commit it.
#
# Usage: tools/update-test-suites.sh
#
# Left out of xslt40-test, because no test reads them: report/ (results
# reported by other processors), admin/unicode.xml and the zipped QT3 tests
# under admin/, and the .jar files of the DocBook toolchain under
# tests/misc/docbook. The rest of admin/ stays: schema tests use its .xsd files.

set -eu

QT4TESTS_URL="https://github.com/qt4cg/qt4tests"
QT4TESTS_COMMIT="166453359bd7770fb113b6d9c9015df7f7fa5d02"
XSLT40_TEST_URL="https://github.com/qt4cg/xslt40-test"
XSLT40_TEST_COMMIT="91328fbf0fed4a5d2813eb3661808ee53cb961ba"
XMLCONF_VERSION="xmlts20130923"
XMLCONF_URL="https://www.w3.org/XML/Test/$XMLCONF_VERSION.tar.gz"

repository_root=$(cd "$(dirname "$0")/.." && pwd)
destination="$repository_root/third_party"
download_directory=$(mktemp -d)
trap 'rm -rf "$download_directory"' EXIT

# Fetch one commit shallowly, then replace the vendored copy with its files.
update_suite() {
    suite_url=$1
    suite_commit=$2
    suite_name=$3
    checkout="$download_directory/$suite_name"

    echo "$suite_name: fetching $suite_commit"
    git init --quiet "$checkout"
    git -C "$checkout" fetch --quiet --depth 1 "$suite_url" "$suite_commit"
    git -C "$checkout" checkout --quiet --force FETCH_HEAD
    rm -rf "$checkout/.git"

    rm -rf "${destination:?}/$suite_name"
    mv "$checkout" "$destination/$suite_name"
    echo "$suite_commit" > "$destination/$suite_name.commit"
}

mkdir -p "$destination"
update_suite "$QT4TESTS_URL" "$QT4TESTS_COMMIT" qt4tests
update_suite "$XSLT40_TEST_URL" "$XSLT40_TEST_COMMIT" xslt40-test

rm -rf "$destination/xslt40-test/report"
rm -f "$destination/xslt40-test/admin/unicode.xml" \
    "$destination/xslt40-test/admin/qt3-converted-tests/qt3-xslt.zip"
find "$destination/xslt40-test/tests/misc/docbook" -name '*.jar' -delete

# The W3C XML conformance suite is published as a dated tarball, not in git.
echo "xmlconf: fetching $XMLCONF_VERSION"
curl -sSL "$XMLCONF_URL" | tar -xz -C "$download_directory"
rm -rf "$destination/xmlconf"
mv "$download_directory/xmlconf" "$destination/xmlconf"
echo "$XMLCONF_VERSION" > "$destination/xmlconf.version"
