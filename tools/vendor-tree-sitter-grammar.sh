#!/bin/sh
# Vendors one tree-sitter grammar repository for sxml:parse-code
# (docs/DECISIONS.md, D285) into third_party/tree-sitter-grammars/LANGUAGE:
# its LICENSE, the generated parser (src/parser.c, any src/scanner.c and
# the headers next to them, src/tree_sitter/), the highlights queries its
# tree-sitter.json names, in that order, and its tests (test/corpus, any
# test/highlight), unmodified, with a VERSION file that records the
# upstream commit. Then run tools/generate-source-code-languages.py.
#
# Usage: tools/vendor-tree-sitter-grammar.sh LANGUAGE URL COMMIT [SUBDIRECTORY]
#   LANGUAGE      the name sxml:parse-code knows the language by: lowercase
#                 letters, digits and underscores; also the grammar's
#                 tree_sitter_LANGUAGE() entry point
#   URL           the grammar's git repository
#   COMMIT        the commit to vendor
#   SUBDIRECTORY  the grammar's directory in a repository of several
#                 grammars (typescript/tsx), default the root
# The target directory must not exist yet: remove an older copy first.
set -eu

language=${1:?LANGUAGE required}
url=${2:?URL required}
commit=${3:?COMMIT required}
subdirectory=${4:-.}
case $language in
    *[!a-z0-9_]* | "") echo "bad language name: $language" >&2; exit 2 ;;
esac

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target="$repository_root/third_party/tree-sitter-grammars/$language"
if [ -e "$target" ]; then
    echo "$target exists; remove it first" >&2
    exit 2
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
git init -q "$work/repository"
git -C "$work/repository" fetch -q --depth 1 "$url" "$commit"
git -C "$work/repository" checkout -q FETCH_HEAD
source="$work/repository/$subdirectory"

mkdir -p "$target/src/tree_sitter" "$target/queries" "$target/test"
cp "$work/repository/LICENSE" "$target/"
cp "$source/src/parser.c" "$target/src/"
for file in "$source"/src/scanner.c "$source"/src/*.h; do
    if [ -f "$file" ]; then
        cp "$file" "$target/src/"
    fi
done
cp "$source"/src/tree_sitter/*.h "$target/src/tree_sitter/"
# The highlights queries tree-sitter.json names for this grammar (a string
# or a list); queries of another grammar (../javascript/...) are copied
# under their file names.
highlights=$(python3 -I - "$work/repository/tree-sitter.json" "$subdirectory" <<'PYTHON'
import json, os, sys
configuration = json.load(open(sys.argv[1]))
subdirectory = os.path.normpath(sys.argv[2])
for grammar in configuration["grammars"]:
    if os.path.normpath(grammar.get("path", ".")) == subdirectory:
        names = grammar.get("highlights", "queries/highlights.scm")
        print(" ".join([names] if isinstance(names, str) else names))
        break
PYTHON
)
copied=""
for query in $highlights; do
    cp "$source/$query" "$target/queries/"
    copied="$copied queries/$(basename "$query")"
done
cp -R "$source/test/corpus" "$target/test/"
if [ -d "$source/test/highlight" ]; then
    cp -R "$source/test/highlight" "$target/test/"
fi
tag=$(git ls-remote --tags "$url" | awk -v commit="$commit" '
    $1 == commit { name = $2; sub("^refs/tags/", "", name); sub("\\^\\{\\}$", "", name); print name; exit }')
[ -n "$tag" ] || tag=none
cat > "$target/VERSION" <<EOF
tree-sitter grammar for $language, vendored for sxml:parse-code
(docs/DECISIONS.md, D285) by tools/vendor-tree-sitter-grammar.sh.

Upstream:   $url
Directory:  $subdirectory
Tag:        $tag
Commit:     $commit
Highlights:$copied
Copied:     LICENSE, src/ (parser.c, any scanner.c and its headers,
            tree_sitter/), the highlights queries above in the order of
            tree-sitter.json, test/corpus and any test/highlight;
            unmodified.
EOF
echo "vendored $language at $commit into $target"
