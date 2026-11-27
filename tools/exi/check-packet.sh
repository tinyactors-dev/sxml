#!/bin/sh
# The gate of a Phase 8 packet (docs/tasks/phase-8.md, "Rules"; D333): what a
# change to the EXI codec must pass before it is committed. Every check is
# deterministic; the script exits non-zero at the first that fails.
#
#   tools/exi/check-packet.sh [--base=COMMIT] [--build=DIRECTORY] [--allow-headers]
#
#   --base           the commit the packet starts from (default: HEAD, so the
#                    working tree's changes are checked)
#   --build          the debug build directory (default: build/exi-debug)
#   --allow-headers  for the design packets 8.1 and 8.2 only: changes under
#                    include/sxml/ are allowed
#   --tests=REGEX    the ctest tests of step 4 (default '^(exi|deflate|inflate)');
#                    narrower only for a packet that does not touch the others
#
# Checks:
#   1. No change under include/sxml/ (headers first: packet 8.2 wrote them).
#   2. The exi_ratchet only grows: tests/exi/passing.txt loses no line.
#   3. The debug build (Clang, AddressSanitizer, UndefinedBehaviorSanitizer,
#      -Werror) builds the library, the EXI tests and the CLI.
#   4. The EXI unit tests and the golden test (with its ratchet) pass under
#      the sanitizers: ctest -R '^(exi|deflate|inflate)'.
#   5. clang-tidy is clean on the changed C files.
#   6. The ast-grep rules (tools/ast-grep) find nothing new in the changed
#      files (a finding the base had already is not the packet's).
#   7. The call graph has no new cycle (tools/check-call-cycles.py, D71).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
base=HEAD
build=$root/build/exi-debug
allow_headers=no
tests='^(exi|deflate|inflate)'
for argument in "$@"; do
    case $argument in
        --base=*) base=${argument#--base=} ;;
        --build=*) build=${argument#--build=} ;;
        --allow-headers) allow_headers=yes ;;
        --tests=*) tests=${argument#--tests=} ;;
        *) echo "check-packet: unknown argument '$argument'" >&2; exit 2 ;;
    esac
done

step() { printf '\n== %s\n' "$1"; }
fail() { printf 'check-packet: FAILED: %s\n' "$1" >&2; exit 1; }

changed_files=$( (git diff --name-only "$base" -- ; git ls-files --others --exclude-standard) | sort -u)

step "1. public headers unchanged"
if [ "$allow_headers" = no ]; then
    headers=$(printf '%s\n' "$changed_files" | grep '^include/sxml/' || true)
    [ -z "$headers" ] || fail "the packet changes public headers: $headers (write a proposal instead)"
fi
echo ok

step "2. the ratchet only grows"
if git cat-file -e "$base:tests/exi/passing.txt" 2>/dev/null; then
    removed=$(git show "$base:tests/exi/passing.txt" | grep -v '^#' | sort > "$build.passing-before" 2>/dev/null; \
              grep -v '^#' tests/exi/passing.txt | sort | comm -23 "$build.passing-before" - ; rm -f "$build.passing-before")
    [ -z "$removed" ] || fail "tests/exi/passing.txt lost checks: $(printf '%s\n' "$removed" | head -5)"
fi
echo ok

step "3. debug build with sanitizers"
mkdir -p "$build"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
    -DSXML_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
ninja -C "$build" sxml exi_golden deflate_vectors sxml_cli $(cd "$build" && ninja -t targets all 2>/dev/null | \
    sed -n 's/^\(\(exi\|deflate\|inflate\)[a-z_]*_test\): .*/\1/p' | sort -u) >/dev/null \
    || { ninja -C "$build" sxml exi_golden deflate_vectors 2>&1 | grep -E 'error' | head -20; fail "the debug build"; }
echo ok

step "4. EXI tests under the sanitizers"
ctest --test-dir "$build" -R "$tests" --output-on-failure || fail "ctest -R '$tests'"

c_files=$(printf '%s\n' "$changed_files" | grep -E '^(src|tests|apps|bindings)/.*\.c$' | while read -r file; do [ -f "$file" ] && echo "$file"; done || true)

step "5. clang-tidy on the changed C files"
if [ -n "$c_files" ]; then
    # shellcheck disable=SC2086
    clang-tidy -p "$build" --quiet --config-file=.clang-tidy $c_files 2>/dev/null | tee "$build.tidy" | grep -E 'warning:|error:' && \
        fail "clang-tidy reports the lines above"
fi
echo ok

step "6. ast-grep rules on the changed files: no new finding"
if [ -n "$c_files" ]; then
    ast_grep=$root/tools/node_modules/.bin/ast-grep
    [ -x "$ast_grep" ] || fail "ast-grep not found; run: npm ci --prefix tools"
    # Findings per file, now and at the base (a copy at the same path, so the
    # rules' file patterns apply alike): a packet adds none.
    base_tree=$build.base-tree
    rm -rf "$base_tree"
    mkdir -p "$base_tree"
    cp -r tools/ast-grep "$base_tree/" 2>/dev/null || true
    mkdir -p "$base_tree/tools" && cp -r tools/ast-grep "$base_tree/tools/"
    new_findings=no
    for file in $c_files; do
        now=$("$ast_grep" scan --config tools/ast-grep/sgconfig.yml "$file" 2>/dev/null | grep -c '^error\[' || true)
        before=0
        if git cat-file -e "$base:$file" 2>/dev/null; then
            mkdir -p "$base_tree/$(dirname "$file")"
            git show "$base:$file" > "$base_tree/$file"
            before=$(cd "$base_tree" && "$ast_grep" scan --config tools/ast-grep/sgconfig.yml "$file" 2>/dev/null | grep -c '^error\[' || true)
        fi
        if [ "$now" -gt "$before" ]; then
            "$ast_grep" scan --config tools/ast-grep/sgconfig.yml "$file" 2>/dev/null | head -40
            echo "$file: $now findings, $before at the base"
            new_findings=yes
        fi
    done
    rm -rf "$base_tree"
    [ "$new_findings" = no ] || fail "ast-grep rules"
fi
echo ok

step "7. no call cycle (D71)"
python3 tools/check-call-cycles.py "$build" || fail "tools/check-call-cycles.py"

printf '\ncheck-packet: all checks passed\n'
