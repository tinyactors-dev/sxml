#!/bin/sh
# Refresh the vendored IANA Time Zone Database in third_party/tzdata (packet
# 7.16, D13, D209). The data is public domain (its LICENSE file). It is
# committed, so this is only needed to move to a new release: edit
# TZDATA_VERSION below, run the script, review the diff and commit it.
#
# Usage: tools/update-tzdata.sh
#
# third_party/tzdata/source holds the release's zone sources and tables as
# IANA ships them. third_party/tzdata/zoneinfo holds what sxml's hosts serve
# as SXML_RESOURCE_KIND_TIMEZONE_DATA (include/sxml/processor.h): one TZif
# file per zone and link (RFC 8536), compiled by zic with "-b slim", and the
# tables zone1970.tab and zone.tab, laid out as /usr/share/zoneinfo is. sxml
# never compiles this data in: the QT4 driver and the unit tests read it
# from the source tree, the sxml command reads the host's zoneinfo.
#
# Needs curl, tar and zic (the tz code's compiler; on Debian and Ubuntu it
# comes with libc-bin, on macOS with the system).

set -eu

TZDATA_VERSION="2026e"
TZDATA_URL="https://data.iana.org/time-zones/releases/tzdata$TZDATA_VERSION.tar.gz"

# The zone sources zic compiles: every region, Etc/*, and the backward
# links (US/Eastern, Asia/Calcutta and the like). backzone, factory and the
# leap second files are left out, as most distributions do.
ZONE_SOURCES="africa antarctica asia australasia europe northamerica southamerica etcetera backward"
# The release's other files that are kept.
KEPT_FILES="LICENSE README version zone1970.tab zone.tab iso3166.tab"

repository_root=$(cd "$(dirname "$0")/.." && pwd)
destination="$repository_root/third_party/tzdata"
download_directory=$(mktemp -d)
trap 'rm -rf "$download_directory"' EXIT

echo "tzdata: fetching $TZDATA_VERSION"
curl --fail --silent --show-error --location -o "$download_directory/tzdata.tar.gz" "$TZDATA_URL"
mkdir "$download_directory/release"
tar -xzf "$download_directory/tzdata.tar.gz" -C "$download_directory/release"
if [ "$(cat "$download_directory/release/version")" != "$TZDATA_VERSION" ]; then
    echo "tzdata: the release says it is $(cat "$download_directory/release/version")" >&2
    exit 1
fi

rm -rf "${destination:?}"
mkdir -p "$destination/source" "$destination/zoneinfo"
for file in $ZONE_SOURCES $KEPT_FILES; do
    cp "$download_directory/release/$file" "$destination/source/$file"
done
(cd "$destination/source" && zic -b slim -d "$destination/zoneinfo" $ZONE_SOURCES)
# Links become copies, so a checkout on any system has plain files.
find "$destination/zoneinfo" -type l | while read -r link; do
    target=$(readlink -f "$link")
    rm "$link"
    cp "$target" "$link"
done
cp "$destination/source/zone1970.tab" "$destination/source/zone.tab" "$destination/zoneinfo/"
echo "$TZDATA_VERSION" > "$repository_root/third_party/tzdata.version"
echo "tzdata: $(find "$destination/zoneinfo" -type f | wc -l) files in third_party/tzdata/zoneinfo"
