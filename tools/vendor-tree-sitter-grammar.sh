#!/bin/sh
# Vendors one tree-sitter grammar repository for sxml:parse-code
# (docs/DECISIONS.md, D285) into third_party/tree-sitter-grammars/LANGUAGE:
# its LICENSE, the generated parser (src/parser.c, any src/scanner.c and
# the headers next to them, src/tree_sitter/, and the C files its
# tree-sitter.json lists as external), the highlights, locals and
# injections queries its tree-sitter.json names, in that order, and its
# tests (test/corpus, any test/highlight), unmodified, with a VERSION file
# that records the upstream commit and the queries (packet 7.23, D288).
# Then run tools/generate-source-code-languages.py.
#
# Usage: tools/vendor-tree-sitter-grammar.sh LANGUAGE URL COMMIT [SUBDIRECTORY]
#   LANGUAGE      the name sxml:parse-code knows the language by: lowercase
#                 letters, digits and underscores; also the grammar's
#                 tree_sitter_LANGUAGE() entry point. A grammar whose
#                 queries name another grammar's (TypeScript's name
#                 JavaScript's) is vendored after that one.
#   URL           the grammar's git repository
#   COMMIT        the commit to vendor
#   SUBDIRECTORY  the grammar's directory in a repository of several
#                 grammars (typescript, tsx, tree-sitter-markdown),
#                 default the root
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

repository="$work/repository"
mkdir -p "$target/src/tree_sitter" "$target/queries" "$target/test"
cp "$repository/LICENSE" "$target/"
cp "$source/src/parser.c" "$target/src/"
for file in "$source"/src/scanner.c "$source"/src/*.h; do
    if [ -f "$file" ]; then
        cp "$file" "$target/src/"
    fi
done
cp "$source"/src/tree_sitter/*.h "$target/src/tree_sitter/"
# The C files tree-sitter.json lists as external-files (a scanner header
# shared by the grammars of one repository, such as TypeScript's
# common/scanner.h) go where the scanner's "../../" includes find them,
# beside the grammars: third_party/tree-sitter-grammars/common/. A file
# already there must be the same.
external=""
for file in $(python3 -I - "$repository/tree-sitter.json" "$subdirectory" external-files <<'PYTHON'
import json, os, sys
configuration = json.load(open(sys.argv[1]))
subdirectory = os.path.normpath(sys.argv[2])
for grammar in configuration["grammars"]:
    if os.path.normpath(grammar.get("path", ".")) == subdirectory:
        print(" ".join(name for name in grammar.get(sys.argv[3], []) or []
                       if name.endswith((".c", ".h"))))
        break
PYTHON
); do
    case $file in
        src/* | "$subdirectory"/src/*) continue ;;
    esac
    destination="$repository_root/third_party/tree-sitter-grammars/$file"
    mkdir -p "$(dirname "$destination")"
    if [ -e "$destination" ] && ! cmp -s "$repository/$file" "$destination"; then
        echo "$destination exists and differs from $url's" >&2
        exit 2
    fi
    cp "$repository/$file" "$destination"
    external="$external ../$file"
done
# The queries tree-sitter.json names for this grammar (a string or a list,
# paths from the repository's root): highlights, locals and injections, in
# its order. A query of another grammar's package
# (node_modules/tree-sitter-OTHER/queries/NAME.scm, as TypeScript names
# JavaScript's) is taken from the vendored OTHER grammar and copied as
# queries/OTHER-NAME.scm.
copy_queries() {
    copied=""
    for query in $(python3 -I - "$repository/tree-sitter.json" "$subdirectory" "$1" <<'PYTHON'
import json, os, sys
configuration = json.load(open(sys.argv[1]))
subdirectory = os.path.normpath(sys.argv[2])
kind = sys.argv[3]
for grammar in configuration["grammars"]:
    if os.path.normpath(grammar.get("path", ".")) == subdirectory:
        names = grammar.get(kind, f"queries/{kind}.scm")
        print(" ".join([names] if isinstance(names, str) else names))
        break
PYTHON
    ); do
        case $query in
            node_modules/tree-sitter-*/queries/*)
                other=${query#node_modules/tree-sitter-}
                other=$(echo "${other%%/*}" | tr '-' '_')
                origin="$repository_root/third_party/tree-sitter-grammars/$other/queries/$(basename "$query")"
                name="$other-$(basename "$query")"
                ;;
            *)
                origin="$repository/$query"
                name=$(basename "$query")
                ;;
        esac
        if [ -f "$origin" ]; then
            cp "$origin" "$target/queries/$name"
            copied="$copied queries/$name"
        elif [ "$1" = highlights ]; then
            echo "no highlights query $query (looked at $origin)" >&2
            exit 2
        fi
    done
    echo "$copied"
}
highlights=$(copy_queries highlights)
locals=$(copy_queries locals)
injections=$(copy_queries injections)
injection_regex=$(python3 -I - "$repository/tree-sitter.json" "$subdirectory" <<'PYTHON'
import json, os, sys
configuration = json.load(open(sys.argv[1]))
subdirectory = os.path.normpath(sys.argv[2])
for grammar in configuration["grammars"]:
    if os.path.normpath(grammar.get("path", ".")) == subdirectory:
        print(grammar.get("injection-regex", ""))
        break
PYTHON
)
# The tests: the grammar's own test/, or for the first grammar of a
# repository of several the repository's (TypeScript's corpus, whose
# ":language(tsx)" tests name the second grammar).
first=$(python3 -I - "$repository/tree-sitter.json" <<'PYTHON'
import json, os, sys
print(os.path.normpath(json.load(open(sys.argv[1]))["grammars"][0].get("path", ".")))
PYTHON
)
tests="$source/test"
if [ ! -d "$tests/corpus" ] && [ "$(cd "$source" && pwd)" != "$(cd "$repository" && pwd)" ] &&
        [ "$first" = "$(echo "$subdirectory" | sed 's|/*$||')" ]; then
    tests="$repository/test"
fi
if [ -d "$tests/corpus" ]; then
    cp -R "$tests/corpus" "$target/test/"
fi
if [ -d "$tests/highlight" ]; then
    cp -R "$tests/highlight" "$target/test/"
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
Highlights:$highlights
Locals:    $locals
Injections:$injections
Injection-regex: $injection_regex
External:  $external
Copied:     LICENSE, src/ (parser.c, any scanner.c and its headers,
            tree_sitter/), the external C files above, the queries above
            in the order of tree-sitter.json, test/corpus and any
            test/highlight; unmodified.
EOF
echo "vendored $language at $commit into $target"
