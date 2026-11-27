#!/bin/sh
# Prints the release notes of version X.Y.Z: its section of CHANGELOG.md,
# without the heading, up to the next release's heading (docs/RELEASE.md;
# .github/workflows/release.yml makes the GitHub release's notes of it).
#
# Usage: tools/release-notes.sh X.Y.Z
# Exit status: 0 with the notes; 1 when CHANGELOG.md has no dated section
# "## [X.Y.Z] - YYYY-MM-DD" (an unreleased version has none).
set -eu

version=${1:?usage: tools/release-notes.sh X.Y.Z}
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
notes=$(awk -v version="$version" '
    BEGIN { heading = "## [" version "] - " }
    index($0, "## [") == 1 {
        if (found) { exit }
        if (index($0, heading) == 1 && $0 ~ /[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$/) { found = 1; next }
    }
    index($0, "[") == 1 && $0 ~ /^\[[^]]*\]: / { if (found) { exit } }
    found { print }
    END { if (!found) { exit 1 } }
' "$repository_root/CHANGELOG.md") || {
    echo "release-notes: CHANGELOG.md has no dated section for $version" >&2
    exit 1
}
# The section without its leading blank lines; $(...) drops the trailing ones.
printf '%s\n' "$notes" | sed -e '/./,$!d'
