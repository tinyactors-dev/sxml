#!/bin/sh
# Publishes a draft into the directory site/: checks it, then writes the
# HTML page, the plain text, the requirements as JSON and a page of the
# changes since the earlier draft. Stops at the first step that fails.
#
# Usage: sh publish.sh [DRAFT [EARLIER-DRAFT]]
set -e
draft=${1:-latch.xml}
earlier=${2:-latch-00.xml}

sxml validate --schema=latch.xsd "$draft"
sxml xquery --param=draft="$draft" publish-check.xq

mkdir -p site
sxml transform --force --output=site/latch.html html.xsl "$draft"
sxml transform --force --output=site/latch.txt text.xsl "$draft"
sxml transform --force --output=site/requirements.json requirements.xsl "$draft"
sxml transform --force --output=site/changes.html --param=old="$earlier" changes.xsl "$draft"
echo "published $draft into site/"
