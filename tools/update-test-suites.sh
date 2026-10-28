#!/bin/sh
# Refresh the vendored W3C QT4, XSLT 4.0, XSD 1.1 and XML 1.0 conformance test
# suites in third_party/ (decision D13). The suites are committed to the repository, so
# this is only needed to move to a new pinned commit: edit the commits below,
# run the script, review the diff and commit it.
#
# Usage: tools/update-test-suites.sh
#
# Left out of xslt40-test, because no test reads them: report/ (results
# reported by other processors), admin/unicode.xml and the zipped QT3 tests
# under admin/, and the .jar files of the DocBook toolchain under
# tests/misc/docbook. The rest of admin/ stays: schema tests use its .xsd files.
#
# Left out of xsdtests (packet 5.1, D165), because no test reads them: the
# HTML, CSS and image files of the reports (nistMeta/*.html, about 25 MB, and
# common/), and the CVS backup copies named .#FILE.REVISION.
#
# From the iXML community test suite (packet 7.18, D215), only tests/ is
# vendored, as third_party/ixml-tests, with the repository's LICENSE;
# tests/performance (78 MB of generated inputs, which no catalog of the
# top-level test-catalog.xml names) is left out.
#
# Schematron test data (packet 7.28, D323), none of it shipped: from EN
# 16931's validation artefacts (EUPL 1.2) only the Schematron sources of
# the UBL and CII bindings and src/test (the VEFA unit tests and test
# files), as third_party/en16931; from XRechnung's Schematron (Apache
# 2.0) only src/validation/schematron and test/instances, as
# third_party/xrechnung; from SchXslt 1.10.1 (MIT) only its 2.0/
# directory, with LICENSE and README.md, as third_party/schxslt, for the
# differential test.

set -eu

QT4TESTS_URL="https://github.com/qt4cg/qt4tests"
QT4TESTS_COMMIT="166453359bd7770fb113b6d9c9015df7f7fa5d02"
XSLT40_TEST_URL="https://github.com/qt4cg/xslt40-test"
XSLT40_TEST_COMMIT="91328fbf0fed4a5d2813eb3661808ee53cb961ba"
XSDTESTS_URL="https://github.com/w3c/xsdtests"
XSDTESTS_COMMIT="7bc3365c652a322f3d762021b3879eb92dae7e30"
IXML_TESTS_URL="https://codeberg.org/InvisibleXML/ixml"
IXML_TESTS_COMMIT="d1f37f6bb1f86a1d370a2ada326d9430922e9967"
EN16931_URL="https://github.com/ConnectingEurope/eInvoicing-EN16931"
EN16931_COMMIT="b17b2695e5e3e021945c38658326b78a76f80f84"
XRECHNUNG_URL="https://github.com/itplr-kosit/xrechnung-schematron"
XRECHNUNG_COMMIT="7e3ddbe224674427fd174a8406635708b0d50ed1"
SCHXSLT_VERSION="1.10.1"
SCHXSLT_URL="https://github.com/schxslt/schxslt/releases/download/v$SCHXSLT_VERSION/schxslt-$SCHXSLT_VERSION-xslt-only.zip"
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
update_suite "$XSDTESTS_URL" "$XSDTESTS_COMMIT" xsdtests

rm -rf "$destination/xslt40-test/report"
rm -f "$destination/xslt40-test/admin/unicode.xml" \
    "$destination/xslt40-test/admin/qt3-converted-tests/qt3-xslt.zip"
find "$destination/xslt40-test/tests/misc/docbook" -name '*.jar' -delete
find "$destination/xsdtests" -type f \( -name '*.html' -o -name '*.css' \
    -o -name '*.gif' -o -name '*.img' -o -name '.#*' \) -delete

update_suite "$IXML_TESTS_URL" "$IXML_TESTS_COMMIT" ixml
rm -rf "$destination/ixml-tests"
mv "$destination/ixml/tests" "$destination/ixml-tests"
mv "$destination/ixml/LICENSE" "$destination/ixml-tests/LICENSE"
rm -rf "$destination/ixml-tests/performance" "$destination/ixml"
mv "$destination/ixml.commit" "$destination/ixml-tests.commit"

# The W3C XML conformance suite is published as a dated tarball, not in git.
echo "xmlconf: fetching $XMLCONF_VERSION"
curl -sSL "$XMLCONF_URL" | tar -xz -C "$download_directory"
rm -rf "$destination/xmlconf"
mv "$download_directory/xmlconf" "$destination/xmlconf"
echo "$XMLCONF_VERSION" > "$destination/xmlconf.version"

update_suite "$EN16931_URL" "$EN16931_COMMIT" en16931-source
rm -rf "$destination/en16931"
mkdir -p "$destination/en16931/ubl" "$destination/en16931/cii"
mv "$destination/en16931-source/src/ubl/schematron" "$destination/en16931/ubl/schematron"
mv "$destination/en16931-source/src/cii/schematron" "$destination/en16931/cii/schematron"
mv "$destination/en16931-source/src/test" "$destination/en16931/test"
mv "$destination/en16931-source/src/LICENSE.txt" "$destination/en16931/LICENSE.txt"
rm -rf "$destination/en16931-source"
mv "$destination/en16931-source.commit" "$destination/en16931.commit"

update_suite "$XRECHNUNG_URL" "$XRECHNUNG_COMMIT" xrechnung-source
rm -rf "$destination/xrechnung"
mkdir -p "$destination/xrechnung"
mv "$destination/xrechnung-source/src/validation/schematron" "$destination/xrechnung/schematron"
mv "$destination/xrechnung-source/test/instances" "$destination/xrechnung/instances"
mv "$destination/xrechnung-source/LICENSE" "$destination/xrechnung/LICENSE"
rm -rf "$destination/xrechnung-source"
mv "$destination/xrechnung-source.commit" "$destination/xrechnung.commit"

echo "schxslt: fetching $SCHXSLT_VERSION"
curl -sSL -o "$download_directory/schxslt.zip" "$SCHXSLT_URL"
(cd "$download_directory" && unzip -q schxslt.zip)
rm -rf "$destination/schxslt"
mkdir -p "$destination/schxslt"
mv "$download_directory/schxslt-$SCHXSLT_VERSION/2.0" "$destination/schxslt/2.0"
mv "$download_directory/schxslt-$SCHXSLT_VERSION/LICENSE" \
    "$download_directory/schxslt-$SCHXSLT_VERSION/README.md" "$destination/schxslt/"
echo "$SCHXSLT_VERSION" > "$destination/schxslt.version"
