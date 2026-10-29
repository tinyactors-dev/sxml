#!/usr/bin/env bash
# tools/spec/test.sh: the spec tools' tests (spec/README.md, "Tests").
#
# In a copy of docs/, spec/ and tools/spec/ in a temporary directory:
# import, round trip, check, every write and its refusals, and the
# detection of a hand edit. Then the hook's cases. Needs the sxml command
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
  "$@" > "$work/out" 2>&1 || { fail "$label"; sed 's/^/  /' "$work/out"; }
}
expect_refusal() {  # expect_refusal LABEL PATTERN COMMAND...
  local label=$1 pattern=$2; shift 2
  if "$@" > "$work/out" 2>&1; then fail "$label: was not refused"
  elif ! grep -q -- "$pattern" "$work/out"; then fail "$label: no '$pattern' in"; sed 's/^/  /' "$work/out"
  fi
}
errors() { "$spec" check 2>&1 | grep '^error:' || true; }

# The import and the round trip.
expect_success "import" python3 "$work/tools/spec/import.py" --sxml "$SXML"
expect_success "every document assembles to its source" "$spec" roundtrip
expect_success "the shadow copy in the repository is the import of docs/" \
  diff -r "$root/spec/articles" "$work/spec/articles"

# The errors the import finds in docs/ today (spec/README.md, "What the
# import found"). A change that adds one fails here.
known=$(errors)
unexpected=$(grep -v -E 'refers to decision:D(186|192|194|204|242|244),' <<< "$known" || true)
[[ -z $unexpected ]] || fail "check finds errors beyond the known ones: $unexpected"

# Reads.
expect_success "show" "$spec" show D238 --depth=2
expect_success "show a section" "$spec" show 'tasks/phase-6#packets' --depth=1
expect_success "refs" "$spec" refs host_shim
expect_success "brief" "$spec" brief 6.2
expect_success "status" "$spec" status phase-6
expect_success "list" "$spec" list decision --since=2026-10-28
expect_success "find" "$spec" find tree-sitter pin
expect_success "query" "$spec" query 'count($articles/decision)'
expect_refusal "an unknown name" "did you mean" "$spec" show D9999

# Writes.
expect_success "amend a summary" "$spec" amend D238 --summary='One C layer, the `host_shim`, under every binding (D138).'
grep -q '<summary>One C layer, the <term name="host_shim"/>' "$work/spec/articles/decisions/D238.xml" \
  || fail "the summary's term and reference were not recognized"
expect_success "update with XQuery Update" "$spec" update 6.11 'insert node <summary>Owner work.</summary> after title'
expect_success "apply to several articles" "$spec" apply \
  'for $packet in $articles/packet[@id = ("6.9", "6.10")] return $packet transform with { insert node <summary>Owner machine.</summary> after title }'
expect_success "summaries keep the round trip: the assembled text is the same" "$spec" roundtrip
expect_success "add a decision" "$spec" add decision --title='A test decision' --after='D11 D181' --body=$'Uses `host_shim`.\n\n- one\n- two'
new=$(ls "$work/spec/articles/decisions" | sed 's/[^0-9]//g' | sort -n | tail -1)
"$spec" show "D$new" > "$work/out"; grep -q "in DECISIONS" "$work/out" || fail "the new decision is not in DECISIONS"
expect_success "supersede" "$spec" supersede D99 --by="D$new"
expect_success "add a term" "$spec" add term test_term --definition='A term the tests add, after `host_shim`.' --after=host_shim
expect_success "rename a term and its uses" "$spec" rename host_shim binding_shim
grep -rq 'name="host_shim"' "$work/spec/articles" && fail "a use of the renamed term is left"
expect_success "set a packet's status" "$spec" set-status 6.12 done --date=2026-10-29
expect_refusal "a reference to nothing" "D9999, which does not exist" "$spec" amend D238 --summary='See D9999.'
expect_refusal "an invalid article" "is not valid" "$spec" update D238 'insert node <bogus/> after title'
expect_refusal "a self-supersession" "is not valid" "$spec" supersede D98 --by=D98
expect_refusal "an unknown status" "is not valid" "$spec" set-status 6.10 finished
expect_refusal "a rename onto a term" "exists" "$spec" rename binding_shim item
[[ $(errors) == "$known" ]] || fail "the writes changed what check finds"

# A hand edit is found: a file that is not in canonical form.
printf '\n' >> "$work/spec/articles/terms/item.xml"
"$spec" check > "$work/out" 2>&1 || true
grep -q "terms/item.xml is not in canonical form" "$work/out" || fail "a hand edit was not found"

# The hook.
hook=$work/.claude/hooks/spec-nodes-only.py
hook_case() {  # hook_case EXPECTED TOOL INPUT-JSON [CWD]
  local status=0
  python3 -c 'import json,sys; print(json.dumps({"tool_name": sys.argv[1], "tool_input": json.loads(sys.argv[2]), "cwd": sys.argv[3]}))' \
    "$2" "$3" "${4:-$work}" | CLAUDE_PROJECT_DIR=$work python3 "$hook" > /dev/null 2>&1 || status=$?
  [[ $status == "$1" ]] || fail "hook: $2 $3 gave $status, not $1"
}
hook_case 2 Read "{\"file_path\": \"$work/spec/articles/decisions/D1.xml\"}"
hook_case 0 Read "{\"file_path\": \"$work/spec/lib/spec.xqm\"}"
hook_case 2 Write '{"file_path": "spec/articles/terms/x.xml"}'
hook_case 2 Edit "{\"file_path\": \"$work/spec/articles/terms/item.xml\"}"
hook_case 2 Grep "{\"pattern\": \"x\", \"path\": \"$work/spec/articles\"}"
hook_case 0 Grep "{\"pattern\": \"x\", \"path\": \"$work/docs\"}"
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
0	tools/spec/spec show D1
0	build/release/apps/sxml xpath --collection=spec/articles 'count(//decision)'
0	git add spec/articles && git commit -m x -- spec/articles
0	ls spec/articles/decisions | wc -l
0	python3 tools/spec/import.py
0	sed -n 1p docs/DECISIONS.md
0	grep -rn articles docs
0	cd spec && ls articles
CASES

if (( failures )); then echo "$failures failures"; exit 1; fi
echo "spec tools: all tests pass"
