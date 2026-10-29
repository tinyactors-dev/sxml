#!/usr/bin/env bash
# tools/spec/test.sh: the spec tools' tests (spec/README.md, "Checks and tests").
#
# In a copy of docs/, spec/ and tools/spec/ in a temporary directory: the
# check, the self-test, every read, every write and its refusals, and the
# detection of hand edits. Then the hook's cases. Needs the sxml command
# ($SXML or build/release/apps/sxml).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SXML=${SXML:-$root/build/release/apps/sxml}
[[ $SXML == /* ]] || SXML=$root/$SXML
export SXML
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tools" "$work/.claude/hooks"
cp -R "$root/docs" "$root/spec" "$work/"
cp -R "$root/tools/spec" "$work/tools/"
cp "$root/.claude/hooks/spec-nodes-only.py" "$work/.claude/hooks/"
spec=$work/tools/spec/spec
failures=0

fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
expect_success() {  # expect_success LABEL COMMAND...
  local label=$1; shift
  "$@" > "$work/out" 2>&1 || { fail "$label"; sed 's/^/  /' "$work/out" | head -20; }
}
expect_refusal() {  # expect_refusal LABEL PATTERN COMMAND...
  local label=$1 pattern=$2; shift 2
  if "$@" > "$work/out" 2>&1; then fail "$label: was not refused"
  elif ! grep -q -- "$pattern" "$work/out"; then fail "$label: no '$pattern' in"; sed 's/^/  /' "$work/out"
  fi
}
expect_output() {  # expect_output LABEL PATTERN COMMAND...
  local label=$1 pattern=$2; shift 2
  "$@" > "$work/out" 2>&1 || true
  grep -q -- "$pattern" "$work/out" || { fail "$label: no '$pattern' in"; sed 's/^/  /' "$work/out" | head -20; }
}

# The spec as committed.
expect_success "check" "$spec" check
expect_output "every article reads back from its source form" " 0 do not read back" "$spec" selftest
expect_output "titles, summaries and definitions read back" "definitions; 0 do not read back" "$spec" selftest

# Reads.
expect_success "show" "$spec" show D238 --depth=2
expect_success "show a section" "$spec" show 'tasks/phase-6#packets' --depth=1
expect_success "refs" "$spec" refs host_shim
expect_success "brief" "$spec" brief 6.2
expect_success "status" "$spec" status phase-6
expect_success "list" "$spec" list decision --since=2026-10-28
expect_success "find" "$spec" find tree-sitter pin
expect_success "text" "$spec" text 'agents/QUEUE#running'
expect_success "markdown" "$spec" markdown PLAN
expect_success "query" "$spec" query 'count($articles/decision)'
expect_refusal "an unknown name" "did you mean" "$spec" show D9999

# Writes, and the pages they generate.
expect_success "amend a summary" "$spec" amend D238 --summary='One C layer, the `host_shim`, under every binding (D138).'
grep -q '<summary>One C layer, the <term name="host_shim"/>' "$work/spec/articles/decisions/D238.xml" \
  || fail "the summary's term and reference were not recognized"
grep -q 'One C layer, the `host_shim`' "$work/docs/DECISIONS.md" || fail "the page does not show the summary"
expect_success "add a decision" "$spec" add decision --title='A test decision' --after='D11 D181' --body=$'Uses `host_shim`.\n\n- one\n- two'
new=$(ls "$work/spec/articles/decisions" | sed 's/[^0-9]//g' | sort -n | tail -1)
grep -q "^### D$new. A test decision" "$work/docs/DECISIONS.md" || fail "the new decision is not on its page"
[[ -z $(cd "$work" && find spec/articles/documents -newer "spec/articles/decisions/D$new.xml") ]] \
  || fail "adding a decision changed a document"
expect_success "supersede" "$spec" supersede D99 --by="D$new"
expect_success "add a term" "$spec" add term test_term --definition='A term the tests add, after `host_shim`.'
grep -q '^| `test_term` |' "$work/docs/GLOSSARY.md" || fail "the new term is not on the glossary page"
expect_success "rename a term and its uses" "$spec" rename host_shim binding_shim
grep -rq 'name="host_shim"' "$work/spec/articles" && fail "a use of the renamed term is left"
expect_refusal "removing a term in use" "refers to term:binding_shim" "$spec" remove binding_shim
expect_success "remove a term nothing uses" "$spec" remove test_term
[[ -e $work/spec/articles/terms/test_term.xml ]] && fail "the removed term's file is left"
expect_success "set a packet's status" "$spec" set-status 6.12 done --date=2026-10-29
expect_success "log" "$spec" log 'Tested the spec tools (D238).' --date=2030-01-02
grep -q '^## Week of 2029-12-31' "$work/docs/LOG.md" || fail "the new week is not on the log page"
"$spec" text 'agents/QUEUE#running' > "$work/running.md"
printf '\nA paragraph the tests add, citing D71 and [[packet:6.2]].\n' >> "$work/running.md"
expect_success "amend a section's text" "$spec" amend 'agents/QUEUE#running' --body=- < "$work/running.md"
grep -q 'A paragraph the tests add, citing D71 and 6.2.' "$work/docs/agents/QUEUE.md" || fail "the section's new text is not on its page"
"$spec" text tasks/phase-6 | grep -v '^\[\[section:tests\]\]$' > "$work/phase-6.md"
expect_refusal "a section's text must keep its subsections" "must keep one line" "$spec" amend tasks/phase-6 --body=- < "$work/phase-6.md"
expect_success "update with XQuery Update" "$spec" update 6.11 'insert node <summary>Owner work.</summary> after title'
expect_success "apply to several articles" "$spec" apply \
  'for $packet in $articles/packet[@id = ("6.9", "6.10")] return $packet transform with { insert node <summary>Owner machine.</summary> after title }'
expect_refusal "a reference to nothing" "D9999, which does not exist" "$spec" amend D238 --summary='See D9999.'
expect_refusal "an invalid article" "is not valid" "$spec" update D238 'insert node <bogus/> after title'
expect_refusal "a self-supersession" "is not valid" "$spec" supersede D98 --by=D98
expect_refusal "an unknown status" "is not valid" "$spec" set-status 6.10 finished
expect_refusal "a rename onto a term" "exists" "$spec" rename binding_shim item
expect_success "the writes leave the spec checked" "$spec" check
expect_output "the writes keep the source form readable" " 0 do not read back" "$spec" selftest

# Hand edits are found: an article not in canonical form, a page edited.
printf '\n' >> "$work/spec/articles/terms/item.xml"
printf 'edited\n' >> "$work/docs/PLAN.md"
"$spec" check > "$work/out" 2>&1 || true
grep -q "terms/item.xml is not in canonical form" "$work/out" || fail "a hand edit of an article was not found"
grep -q "stale page: docs/PLAN.md" "$work/out" || fail "a hand edit of a page was not found"

# The hook.
hook=$work/.claude/hooks/spec-nodes-only.py
hook_case() {  # hook_case EXPECTED TOOL INPUT-JSON [CWD]
  local status=0
  python3 -c 'import json,sys; print(json.dumps({"tool_name": sys.argv[1], "tool_input": json.loads(sys.argv[2]), "cwd": sys.argv[3]}))' \
    "$2" "$3" "${4:-$work}" | CLAUDE_PROJECT_DIR=$work python3 "$hook" > /dev/null 2>&1 || status=$?
  [[ $status == "$1" ]] || fail "hook: $2 $3 gave $status, not $1"
}
hook_case 2 Read "{\"file_path\": \"$work/spec/articles/decisions/D1.xml\"}"
hook_case 2 Read "{\"file_path\": \"$work/docs/DECISIONS.md\"}"
hook_case 0 Read "{\"file_path\": \"$work/docs/CONFORMANCE.md\"}"
hook_case 0 Read "{\"file_path\": \"$work/spec/lib/spec.xqm\"}"
hook_case 2 Write '{"file_path": "spec/articles/terms/x.xml"}'
hook_case 2 Edit '{"file_path": "docs/tasks/phase-6.md"}'
hook_case 2 Grep "{\"pattern\": \"x\", \"path\": \"$work/spec/articles\"}"
hook_case 2 Grep '{"pattern": "x", "path": "docs"}'
hook_case 0 Grep '{"pattern": "x", "path": "src"}'
hook_case 0 Grep '{"pattern": "x"}'
hook_case 0 Glob '{"pattern": "docs/**/*.md"}'
while IFS=$'\t' read -r expected command directory; do
  hook_case "$expected" Bash "$(python3 -c 'import json,sys; print(json.dumps({"command": sys.argv[1]}))' "$command")" "$work${directory:+/$directory}"
done << 'CASES'
2	sed -i s/a/b/ spec/articles/terms/item.xml
2	grep -r D71 spec/articles
2	cat spec/articles/decisions/D1.xml | head
2	cd spec/articles && sed -i s/x/y/ terms/item.xml
2	cd spec && grep -r x articles
2	grep -r x .	spec/articles
2	python3 -c 'open("spec/articles/x.xml", "w")'
2	echo hi > spec/articles/terms/x.xml
2	find spec/articles -name '*.xml' -exec sed -i s/a/b/ {} +
2	find spec/articles -name '*.xml' | xargs perl -pi -e s/a/b/
2	cp /tmp/x.xml spec/articles/terms/
2	grep -n D71 docs/DECISIONS.md
2	sed -i s/a/b/ docs/GLOSSARY.md
2	cat docs/agents/BRIEF.md
2	grep -rn foo docs
2	cd docs && sed -n 1p PLAN.md
2	echo x >> docs/LOG.md
0	tools/spec/spec show D1
0	build/release/apps/sxml xpath --collection=spec/articles 'count(//decision)'
0	git add spec/articles docs && git commit -m x -- spec/articles docs
0	git diff docs/DECISIONS.md
0	ls spec/articles/decisions | wc -l
0	ls docs
0	head docs/CONFORMANCE.md
0	grep -rn foo .
0	grep -rn foo src docs/book
0	cd spec && ls articles
CASES

if (( failures )); then echo "$failures failures"; exit 1; fi
echo "spec tools: all tests pass"
