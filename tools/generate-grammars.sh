#!/bin/sh
# Regenerates every tree-sitter parser from its grammar (docs/DECISIONS.md, D12).
#
# For each grammars/<language>/grammar.js, runs the tree-sitter-cli pinned in
# tools/package.json, which writes grammars/<language>/src/parser.c,
# src/grammar.json, src/node-types.json and src/tree_sitter/*.h. An external
# scanner, grammars/<language>/src/scanner.c, is hand-written and left alone.
# The generated files are committed, so building sxml needs no Node.js.
#
# Usage: tools/generate-grammars.sh [language...]   (default: every grammar)
# Needs: Node.js and `npm ci --prefix tools`.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree_sitter="$repository_root/tools/node_modules/.bin/tree-sitter"
if [ ! -x "$tree_sitter" ]; then
    echo "tree-sitter-cli not found; run: npm ci --prefix tools" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    set --
    for grammar_file in "$repository_root"/grammars/*/grammar.js; do
        [ -f "$grammar_file" ] || continue
        set -- "$@" "$(basename "$(dirname "$grammar_file")")"
    done
fi

if [ "$#" -eq 0 ]; then
    echo "no grammars found under grammars/"
    exit 0
fi

for language in "$@"; do
    grammar_directory="$repository_root/grammars/$language"
    if [ ! -f "$grammar_directory/grammar.js" ]; then
        echo "no grammar at grammars/$language/grammar.js" >&2
        exit 1
    fi
    echo "generating grammars/$language"
    (cd "$grammar_directory" && "$tree_sitter" generate --output src grammar.js)
done
