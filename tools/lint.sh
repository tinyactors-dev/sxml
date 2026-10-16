#!/bin/sh
# Runs every lint gate over project code (docs/DECISIONS.md, D17):
#   1. the ast-grep rule self-tests (each rule fires on its bad examples),
#   2. the lint CMake target: clang-tidy and the ast-grep rules.
# Usage: tools/lint.sh            (from anywhere; uses the debug preset)
# Needs: cmake, ninja, clang, clang-tidy, and `npm ci --prefix tools`.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repository_root"

ast_grep=tools/node_modules/.bin/ast-grep
if [ ! -x "$ast_grep" ]; then
    if command -v ast-grep >/dev/null 2>&1; then
        ast_grep=ast-grep
    else
        echo "ast-grep not found; run: npm ci --prefix tools" >&2
        exit 1
    fi
fi

"$ast_grep" test --config tools/ast-grep/sgconfig.yml --skip-snapshot-tests
cmake --preset debug >/dev/null
cmake --build --preset lint
