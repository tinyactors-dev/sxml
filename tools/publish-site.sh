#!/bin/sh
# Publishes sxml.tinyactors.dev (D304): builds the site into build/site
# (the landing page in website/ with VitePress, the book in docs/book with
# mdbook at /book/) and force-pushes it as one commit to the gh-pages
# branch, which GitHub Pages serves. Needs node, npm and mdbook on PATH.
#
#   mise run site:publish          or   tools/publish-site.sh
#
# SXML_SITE_REMOTE names the repository to push to (default: origin's URL).
set -eu

repository_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repository_root"

site_directory="$repository_root/build/site"
remote=${SXML_SITE_REMOTE:-$(git remote get-url origin)}
source_commit=$(git rev-parse --short HEAD)
if [ -n "$(git status --porcelain -- website docs/book)" ]; then
    source_commit="$source_commit with uncommitted changes"
fi

rm -rf "$site_directory"
npm ci --prefix website --no-audit --no-fund
npm run --prefix website build
mdbook build docs/book --dest-dir ../../build/site/book

publish_directory=$(mktemp -d)
trap 'rm -rf "$publish_directory"' EXIT
cp -R "$site_directory/." "$publish_directory/"
git -C "$publish_directory" init --quiet --initial-branch=gh-pages
git -C "$publish_directory" add --all
git -C "$publish_directory" \
    -c user.name="$(git config user.name)" -c user.email="$(git config user.email)" \
    commit --quiet --message="Publish sxml.tinyactors.dev from $source_commit"
git -C "$publish_directory" push --force "$remote" gh-pages:gh-pages
