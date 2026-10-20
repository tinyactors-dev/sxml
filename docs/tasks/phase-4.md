# Phase 4: XSLT 4.0

Goal (PLAN.md): XSLT 4.0 templates, modes and minimal packages, lowered
into the shared IR (D46, D55) and run by the one evaluator; `sxml
transform` (docs/CLI.md). Gate: **70% of the in-scope XSLT 4.0 test
cases pass** (D95; in scope: spec XSLT40, not the `strm` and `sandp`
groups, no schema_aware, streaming, dtd, XML_1.1, XSD_1.1,
backwards_compatibility, HTML4 or ICU_spellout feature; about 8,700
cases). Rules for parallel packets are those of `docs/tasks/phase-2.md`
(D45, D96).

## Overlap with Phases 2 and 3 (D96)

Packets marked **Early** may start once the Phase 1 gate is closed and
their "After" list is committed, during Phases 2 and 3; they own no file
of a running packet. Early: 4.0, 4.1, 4.2, 4.3, 4.4, 4.5. The others wait
for the Phase 3 gate (they need XQuery's constructors, try/catch,
serializers and API, and must not race its sweeps for files).

## How XSLT lowers (`src/ir_internal.h`)

The stylesheet is parsed by sxml's XML parser into an arena tree, read
into a stylesheet AST (4.2) and lowered by XSLT lowering parts (2.1):
instructions as XQuery (xsl:variable LET, xsl:for-each MAP, xsl:sort SORT,
xsl:choose CONDITIONAL, literal result elements and xsl:element the
constructor kinds, xsl:copy SHALLOW_COPY, xsl:try TRY_CATCH); XPath in
attributes through the embedding entry of 2.1 with each element's static
context; template rules as the named extension point (modes, rules, match
functions from patterns, APPLY_TEMPLATES), as `src/ir_internal.h` writes
out. No new IR kind; a packet that needs one writes "Proposed".

## Parallelism plan

| Wave | Packets (run together) |
| --- | --- |
| E0 (Phase 2 wave 0) | 4.2 stylesheet AST |
| E1 (Phase 2 wave 1) | 4.3 value templates, 4.4 patterns |
| E2 (Phase 2 wave 2) | 4.0 XSLT API |
| E3 (Phase 2 wave 3) | 4.1 XSLT driver |
| E4 (Phase 3) | 4.5 stylesheet reader |
| 1 | 4.6 sequence constructors |
| 2 | 4.7 control flow, 4.8 templates and modes |
| 3 | 4.9 sorting and grouping, 4.10 keys and XSLT functions, 4.11 numbering and analyze-string, 4.12 functions and declarations |
| 4 | 4.13 output and result documents, 4.14 accumulators |
| 5 | 4.15 XSLT API and CLI |
| 6 | 4.16 packages, 4.17 conformance sweeps |
| 7 | 4.18 review (gate) |

Critical path: 4.2 4.5 4.6 4.8 4.13 4.15 4.17 4.18. "Working XSLT" for the
owner is the end of 4.15: `sxml transform` runs the insn-apply-templates,
for-each-group, sort, key and number tests.

## Packets

### 4.0 XSLT API (design) — Early
After 3.0. Owns `include/sxml/xslt.h`. A compiled stylesheet (lifecycle
quintet, D51) from bytes or from the resolver (XSLT_MODULE,
XSLT_PACKAGE of 2.2) with static parameters; a transformation request:
initial mode, template (default `xsl:initial-template`) or function,
global context item, stylesheet parameters, primary result as a sequence,
a tree or serialized by its output definition (3.0 parameters), a
result-document handler (absolute href to an `sxml_output_stream` or a
tree; D50 statuses), the message sink of 2.2 (terminate raises XTMM9000).
Done when header check, lints and rubric pass and 4.1 and 4.15 can be
written against it.
**Done** 2026-10-17 (D112): `include/sxml/xslt.h`, header only:
`sxml_xslt_stylesheet` (quintet; text, XSLT_MODULE or XSLT_PACKAGE;
static parameters), `sxml_xslt_output_definition`,
`sxml_xslt_stylesheet_transform` with the three invocation kinds,
`sxml_xslt_result_delivery`, the result document handler and its
default, `sxml_xslt_result_document_collector` (for 4.15 to implement).

### 4.1 XSLT 4.0 test driver (implementation) — Early
After 4.0, 2.16. Owns new `tests/xslt/driver/`, `docs/QT4_CATALOG.md`
(its xslt40-test section). Program `sxml-xslt-test` on the CLI framework,
linking `sxml_qt4_driver_library` for assertions, reports and documents.
Reads `third_party/xslt40-test/catalog.xml` (D13): `<dependencies>`
wrappers (spec XSLT40, the D95 feature exclusions, a case spec replaces
the set's), environments with `source` (file, inline `content`, `uri`),
`stylesheet` and `package` entries, `<test>` with stylesheets, packages,
`initial-template`, `initial-mode`, `initial-function`, `param` (static
or not), `output serialize="yes"`; assertions of QT4 plus
`assert-result-document`, `assert-message`, `assert-serialization`,
`assert-serialization-error` (`assert-warning` and
`assert-posture-and-sweep` not run). `list` works now; `run` reports
"unsupported" until 4.15. Done when the 8,700 in-scope cases resolve and
a ctest slice list `tests/xslt/slice.txt` (about 150 cases over the core
sets) is chosen with a baseline file.
**Done** 2026-10-17 (D117): `tests/xslt/driver/` (`sxml-xslt-test list`
and `run`, a sibling of sxml-qt4), `tests/xslt/slice.txt` (154 cases),
`tests/xslt/slice-baseline.txt` (0); all 13,867 cases resolve, 8,728 in
scope; `run` says "XSLT not implemented" until 4.15.

### 4.2 Stylesheet AST (design) — Early
After the Phase 1 gate. Owns `src/xslt_ast_internal.h`. Declarations and
instructions over the parsed arena tree, index-addressed (D24, D52
style): element kind, attributes as AST indexes of compiled XPath,
patterns or value templates, the static context of each element
(namespaces, xpath-default-namespace, expand-text, default-collation,
default-mode, version, exclude-result-prefixes), import precedence,
source locations. Done when the stylesheet of for-each-group-001, a
template rule and an xsl:function are written out in comments.
**Done** 2026-10-17 (D102): `src/xslt_ast_internal.h`, header only; the
builder and lifecycle functions go to 4.5 in a new `src/xslt_ast.c`.

### 4.3 Value templates (implementation) — Early
After 2.1, 4.2. Owns new `grammars/value_template/`,
`grammars/CMakeLists.txt`, `tools/qt4-parse-check.py`,
`src/xslt_value_template.c`, `tests/xslt_value_template_test.c`.
Attribute and text value templates: a tree-sitter grammar inheriting
`grammars/xpath` (D12, D29) for fixed parts, `{{ }}` escapes and enclosed
expressions; lowered as string templates are. XTSE0350, XTSE0370. Done
when every AVT and TVT of the in-scope stylesheets parses (a
value-template mode of `tools/qt4-parse-check.py --spec XT40`).
Spec: XSLT 4.0 "Value Templates".
**Done** 2026-10-17 (D120): `sxml_xslt_build_value_template_ast` in
`src/xslt_value_template_internal.h`; `--value-templates` accepts all
12,232 valid value templates of the in-scope stylesheets.

### 4.4 Patterns (implementation) — Early
After 2.1. Owns `src/xslt_pattern.c`, `tests/xslt_pattern_test.c`. CST of
`grammars/xslt_pattern` to AST to match functions (the IR example of
`src/ir_internal.h`): path patterns, predicate patterns `.[…]`, type
patterns, union/intersect/except, rooted patterns (`$v`, doc, id, key,
element-with-id), JNode patterns; default priorities; XTSE0340. Done when
the 19,168 parsable patterns of the suite lower and 40 hand-picked
patterns match a fixed document as a table says. Spec: XSLT 4.0
"Patterns", "Default Priority for Template Rules".
**Done** 2026-10-17 (D118): `src/xslt_pattern.c`,
`src/xslt_pattern_internal.h`, `tests/xslt_pattern_test.c`; pattern kinds
appended to `src/xpath_ast_internal.h`. 19,288 suite patterns parse and
build match expressions, 18,916 lower without stylesheet context; 51
patterns match the fixed document as the table says.

### 4.5 Stylesheet reader (implementation) — Early
After 4.2, 4.3, 4.4, 3.2. Owns `src/xslt_read.c`, `src/xslt_static.c`,
`tests/xslt_read_test.c`, `src/xslt_ast.c` and `tests/xslt_ast_test.c`
(the functions `src/xslt_ast_internal.h` declares, D102). Arena tree to stylesheet AST: XSLT element and
attribute rules (XTSE0010, XTSE0090), standard attributes, whitespace
stripping of the stylesheet, use-when with static parameters and
variables, shadow attributes, xsl:include and xsl:import with precedence
(XTSE0180 cycles), simplified stylesheets, forwards-compatible behaviour.
Done when every in-scope stylesheet module reads or fails with the
expected XTSE code (attr-use-when, attr-static, attr-shadow, decl-include,
decl-import, misc-forwards ≥ 90% once 4.15 runs them).
**Done** 2026-10-19 (D139): `src/xslt_ast.c`, `src/xslt_read.c` with
`src/xslt_read_internal.h`, `src/xslt_static.c` with
`src/xslt_static_internal.h`, tests `xslt_ast` and `xslt_read`. Of 8,712
in-scope cases 7,281 read as expected (or fail with the expected code),
1,323 expect a later error, 108 do not (55 `~T` patterns, 17 xsl:record).

### 4.6 Sequence constructors and node construction (implementation)
After 4.5, 3.5. Owns `src/xslt_lower.c`, `src/xslt_lower_internal.h`,
`tests/xslt_lower_test.c`. Lowering part: sequence constructors, literal
result elements (namespace fixup, exclude-result-prefixes,
xsl:namespace-alias), xsl:element, attribute, text, value-of (separator),
comment, processing-instruction, namespace, document, copy, copy-of,
sequence, xsl:map, map-entry, array, record (4.0), TVTs, whitespace text
nodes. Done when insn-lre, element, attribute, text, value-of, copy,
construct-node, sequence ≥ 85% under 4.15. Spec: XSLT 4.0 "Creating Nodes
and Sequences".
**Done** 2026-10-19 (D142): `src/xslt_lower.c`, `src/xslt_lower_construct.c`,
`src/xslt_simple_content.c`, `src/xslt_lower_internal.h`,
`tests/xslt_lower_test.c`; xsl:record in the AST (`src/xslt_read.c`).
Internal entry `sxml_xslt_lower_initial_template` until 4.8/4.15. Scratch
run of the eight sets plus array and xsl-record (436 applicable): 78 pass,
356 need 4.7/4.8/4.10/4.12 (variables, for-each, templates, functions), 2
fail (DTD entities).

### 4.7 Control flow and variables (implementation)
After 4.6, 3.6. Owns `src/xslt_lower_control.c`,
`tests/xslt_lower_control_test.c`. Local and global xsl:variable and
xsl:param (XTDE0640 cycles, XTDE0050 required), xsl:if, choose, switch,
for-each, iterate (break, next-iteration, on-completion), try/catch,
where-populated, on-empty, on-non-empty, message, assert, fallback,
evaluate (the XPath API inside). Done when insn-choose, for-each,
iterate, try, switch, where-populated, on-empty, message, evaluate,
decl-variable ≥ 85%.
**Done** 2026-10-19 (D145): `src/xslt_lower_control.c`,
`tests/xslt_lower_control_test.c`, hooks in `src/xslt_lower.c`. Scratch
harness: 105 of 181 runnable cases of the sets pass; evaluate is not
applicable (no dynamic evaluation, D117); xsl:sort in for-each waits for
4.9.

### 4.8 Templates and modes (implementation)
After 4.4, 4.6. Owns `src/evaluator_template.c`,
`src/xslt_lower_template.c`, `tests/evaluator_template_test.c`.
APPLY_TEMPLATES in the evaluator: rule order by precedence, priority and
position; on-multiple-match; built-in rules for each on-no-match value;
xsl:mode declarations, `#current`, `#unnamed`, default-mode, declared
modes; named templates, call-template, with-param, tunnel parameters,
next-match, apply-imports, current(), `as` on templates. Done when
attr-match, mode, priority, tunnel, declared-modes, insn-apply-templates,
next-match, apply-imports, call-template, misc-built-in-templates ≥ 85%.
Spec: XSLT 4.0 "Template Rules", "Modes".
**Done** 2026-10-17 (D144): `src/evaluator_template.c`,
`src/xslt_lower_template.c`, `tests/evaluator_template_test.c`; the
reserved APPLY_TEMPLATES payload, mode and rule of `src/ir_internal.h`
completed and verified; `sxml_xslt_lower_initial_template` takes an
invocation. Scratch run of the ten sets plus template: 316 of 615
applicable cases pass; 99 need globals and 59 stylesheet parameters (4.7),
73 other instructions, 18 XSLT functions (current() is 4.10's, D118), 21
packages. xsl:sort under apply-templates waits for 4.9.

### 4.9 Sorting and grouping (implementation)
After 4.7. Owns `src/xslt_lower_group.c`, `src/xslt_group.c`,
`tests/xslt_group_test.c`. xsl:sort (select, order, collation (2.13),
data-type, case-order, stable), xsl:perform-sort; xsl:for-each-group
(group-by, group-adjacent, group-starting-with, group-ending-with,
composite, collation), current-group, current-grouping-key; xsl:merge
with current-merge-group and -key (internal native rows, D55). Done when
insn-sort, for-each-group, merge ≥ 85%. Spec: XSLT 4.0 "Sorting",
"Grouping", "Merging".
**Done** 2026-10-18 (D152): `src/xslt_lower_group.c`, `src/xslt_group.c`,
`tests/xslt_group_test.c`, hooks in for-each, apply-templates, the
evaluator's SORT and `src/xpath_lower.c`. Scratch harness: insn/sort
74/82, for-each-group 79/86 (90%, 92%), merge 61/88 (69%: collections,
schemas and xsl:function in the rest); split-when and merge-when not done.

### 4.10 Keys and XSLT functions (implementation)
After 4.8. Owns `src/xslt_key.c`, `src/function_library_xslt.c` (a part
named only by XSLT compilation), `tests/xslt_key_test.c`. xsl:key and
fn:key (index per document and key, built on first use, composite,
collation), document, current, system-property,
available-system-properties, function-available, element-available,
type-available, unparsed-entity-uri/-public-id, current-output-uri,
snapshot, copy-of, apply-templates as a function if the draft keeps it.
Done when fn-key, document, system-property, function-available,
snapshot, copy-of ≥ 85%.
**Done** 2026-10-17 (D149): `src/xslt_key.c` with `src/xslt_key_internal.h`,
`src/function_library_xslt.c`, `tests/xslt_key_test.c`; the evaluation memo
(`src/evaluator.c`), the current item in lowering, and patterns without
re-selection (`src/xslt_pattern.c`). Scratch harness: key 75/91, document
64/98, system-property 20/23, snapshot 7/19, copy-of 3/8 of applicable
cases; most of the rest need xsl:function (4.12), xsl:sort (4.9) or
stylesheet parameters. fn:apply-templates and current-output-uri are left.

### 4.11 Numbering and analyze-string (implementation)
After 4.8, 2.9. Owns `src/xslt_lower_number.c`, `tests/xslt_number_test.c`.
xsl:number: level single, multiple, any; count and from patterns; value;
format, lang, letter-value, ordinal, grouping, start-at (4.0) on the 2.9
engine. xsl:analyze-string with matching-substring,
non-matching-substring and regex-group. Done when insn-number (268) and
analyze-string ≥ 85%. Spec: XSLT 4.0 "Numbering", "Regular Expressions".
**Done** 2026-10-18 (D153): `src/xslt_lower_number.c`,
`tests/xslt_number_test.c`; format-place-marker beside fn:format-integer,
format strings and grouping in `src/format.c`, fn:regex-group(s) in the
`xslt` part, the context-variable parameter default in `src/xpath_lower.c`.
Scratch harness, in-scope cases: insn/number 152/161 (94%),
analyze-string 56/62 (90%), misc/regex 64/65, format-number 67/70.

### 4.12 Functions and declarations (implementation)
After 4.8. Owns `src/xslt_lower_declaration.c`,
`tests/xslt_declaration_test.c`. xsl:function (visibility, new-each-time,
cache), attribute sets (use-attribute-sets, XTSE0720), xsl:decimal-format
(2.9), xsl:strip-space and preserve-space for sources,
xsl:global-context-item, xsl:context-item, xsl:initial-template. Done
when decl-function, attribute-set, strip-space, global-context-item,
context-item, misc-initial-template ≥ 85%.
**Done** 2026-10-17 (D150): `src/xslt_lower_declaration.c`,
`src/xslt_whitespace.c`, `tests/xslt_declaration_test.c`; hooks in
`src/xslt_lower.c`, `src/xslt_lower_construct.c`, the document
preparation of `src/processor.c`; on 4.7's globals, initializers before
the templates are released and the order through called components.
Scratch harness: function 98/124, attribute-set 47/49, strip-space 25/26,
global-context-item 10/12, context-item 29/32, misc-initial-template 2/4
(4 skipped: initial-template parameters), param 31/31, variable 90/108.
Character maps are 4.13's; memoization (cache) not done.

### 4.13 Output and result documents (implementation)
After 4.12, 3.4. Owns `src/xslt_output.c`, `tests/xslt_output_test.c`.
xsl:output and named output definitions, xsl:character-map,
xsl:result-document (href against the base output URI, XTDE1490, implicit
and explicit primary result), xsl:document, disable-output-escaping as
Serialization 4.0 allows. Done when decl-output (245), character-map,
insn-result-document (150), xsl-document ≥ 80%. Spec: XSLT 4.0
"Serialization", "Creating Final Result Trees".
**Done** 2026-10-18 (D156): `src/xslt_output.c` with
`src/xslt_output_internal.h`, `src/function_library_output.c`,
`tests/xslt_output_test.c`; hooks in `src/xslt_lower.c`,
`src/xslt_lower_template.c`, `src/processor_internal.h`. Stylesheet
outputs, the result writer and the result document collector for 4.15 to
call. Scratch harness: decl/output 235/244, character-map 32/33,
result-document 129/149, xsl-document 23/25, current-output-uri 6/16.

### 4.14 Accumulators (implementation)
After 4.12. Owns `src/xslt_accumulator.c`, `tests/xslt_accumulator_test.c`.
xsl:accumulator without streaming, accumulator-before and -after,
use-accumulators on modes and sources. Done when decl-accumulator ≥ 80%.
**Done** 2026-10-18 (D155): `src/xslt_accumulator.c`, rows in
`src/function_library_xslt.c`, `tests/xslt_accumulator_test.c`; hooks in
`src/xslt_lower.c`, `src/xslt_lower_construct.c` and `src/xslt_key.c`
(copies keep values); xsl:source-document lowered without streaming.
Scratch harness: decl-accumulator 68/69 (4 before), misc-error 370/499
(355), attr-mode 121/147 (109), insn-copy 126/146 (124).

### 4.15 XSLT API and CLI (implementation)
After 4.13. Owns `src/xslt.c`, `tests/xslt_test.c`,
`apps/sxml/command_transform.c`, `apps/sxml/program.c`, `docs/CLI.md`,
`tests/cli/`. Implements `xslt.h`; `sxml transform` exactly as docs/CLI.md
lists it, with golden tests. Done when the 4.1 driver runs the slice at
its baseline under `debug`, `debug-gcc` and `wasm` and 20 transformations
round-trip through the public headers alone.
**Done** 2026-10-19 (D157): `src/xslt.c`, `tests/xslt_test.c` (26
transformations and the parameter, function, delivery, resolver and
lifecycle cases), `apps/sxml/command_transform.c`, `docs/CLI.md`,
`tests/cli/`; the driver's runs (`tests/xslt/driver/xslt_execution.c`),
`tools/run-xslt.sh`, XT40 in `tools/conformance-report.py`. Slice 140 of
154 under all three presets. Release, whole suite: in scope 7,585 of 8,728
(86.9 %); number (57.5 %) and value-of (53.3 %) are the core sets below
80 %.

### 4.16 Packages, minimal (implementation)
After 4.15. Owns `src/xslt_package.c`, `tests/xslt_package_test.c`.
xsl:package, use-package through the resolver (package-version ranges),
visibility (public, private, final, abstract, hidden), accept, expose,
override for functions, templates, variables, attribute sets and modes.
Done when decl-package, use-package, accept, expose, override,
attr-package-version ≥ 70%.
**Done** 2026-10-19 (D160): `src/xslt_package.c` (the package table),
the reader's USED_PACKAGE modules, binding in the XSLT lowering,
`tests/xslt_package_test.c`, the driver's version choice. In scope:
package 94 %, override 89 %, accept 96 %, expose 100 %, package-version
97 %, use-package 64 % (76 % of the cases run: 8 need another
package_version_resolution, 1 streaming).

### 4.17 Conformance sweeps (implementation, repeatable)
After 4.15. As 2.19, over in-scope XSLT sets (misc-regex-syntax 987 and
misc-error 500 first: they measure the regex engine and error codes
cheaply). Each sweep claims its files in `docs/LOG.md`.
**Sweep 1 done** 2026-10-19 (D159): XT40 in scope 7,585 to 7,737 of
8,728 by this sweep (86.9 % to 88.6 %; 7,879, 90.3 %, with 4.16's
packages), errors 7 to 3; number 57.5 to 97.4 %. Open: value-of (empty
text nodes and cdata, Proposed), the call's static context at run time
(Proposed), memory of many small trees (json-to-xml-013, array-022),
xsl:function cache (function-1031), call-template-ext, host collations.
**Sweep 2 done** 2026-10-19 (D163): XT40 in scope 7,879 to 7,986 of
8,728 (90.3 % to 91.5 %), errors 3 to 1; value-of 53.3 to 93.3 % (every
core set ≥ 80 %); zero-length text nodes and CDATA requests (tree.h),
sized arena trees, xsl:function cache, the static context of a call at
run time, declarations local to a package, XTDE0160, XTSE1650, XTSE1660.
Open: json-to-xml-013 (memory, Proposed: streaming constructor content),
XTDE3052 when an abstract variable is read, call-template-ext, host
collations.
**Sweep 3 done** 2026-10-18 (D166): content streams into constructors
(receiving and delivering tasks, content LETs, building into the
receiver's tree), 56-byte arena nodes; json-to-xml-013 passes, XT40 in
scope 7,987 of 8,728, errors 1 to 0. Open: XTDE3052 when an abstract
variable is read (globals computed on first read, Proposed),
call-template-ext, host collations.

### 4.18 Phase 4 review (review)
After everything. Owns the gate boxes. A separate agent (D11) runs the
rubric and re-checks each "done when"; open items go under "Proposed".
**Done** 2026-10-19 (D169): numbers at 77bc5d5 and 8d2aa06, fuzzing
with a new XSLT target, the step budget judged fair, D71 through
function pointers, XSLT error locations and err:XTMM9000 fixed, `sxml
transform`'s relative references fixed, two helpers merged, `xslt.h`
stable, the D162 check; match="$var" membership and errors in included
modules Proposed.

## Gate

- [x] In-scope XSLT 4.0 cases (D95) ≥ 70% pass under `release`; the core
  sets apply-templates, mode, match, for-each-group, sort, key, number,
  choose, copy, value-of, result-document, function ≥ 80% each. (agent
  review, D169: 7,990 of 8,728, 91.5 %; the lowest core set is mode,
  88.5 %)
- [x] XQ40 still ≥ 85%, XP40 ≥ 90%; all ctest slices at their baselines.
  (agent review, D169: XQ40 97.2 %, XP40 97.1 %, no regression; slices
  198, 186, 147 under every preset)
- [x] No in-scope case crashes, leaks or ends in an "error" outcome.
  (agent review, D169: every test set finished, 0 errors)
- [x] `sxml transform` matches docs/CLI.md. (agent review, D169, after
  the fix of relative references)
- [x] Review agent pass (D11), findings fixed; the owner is told XSLT
  works (D11 goal). (agent review, D169; the larger follow-ups are
  Proposed or listed)

## Later sweeps

Sweeps after the gate, over the XSLT leftovers of `docs/agents/QUEUE.md`
(D181: debug preset, the unit tests touched, the affected XT40 sets with
the release driver).

- [x] **4.19 XSLT conformance sweep** (implementation). Owns the XSLT
  lowering and reader files it changes (`src/xslt_*`), their unit tests,
  and single insertions in the ledgers. In order of value: the "~T" type
  patterns of priority (only if the suite's draft is the project's, D29),
  xsl:item-type and xsl:record-type (D98), merge, fn:snapshot, extension
  instructions as template calls (10.1.3), an index for match="$var"
  membership, the module named by errors in included modules, the larger
  duplicates of D169. Done when each item is done or its cause is
  recorded, with before and after counts of the affected sets.
  **Done** 2026-10-19 (D185): XT40 8,650 to 8,759 passed, 0 errors;
  call-template-ext 32/32, record 39/47, type-alias 25/32, snapshot
  34/35, merge 79/120 (29 streaming), collection 5/6; priority unchanged
  (the "~T" patterns are not in the project's draft, D29). Errors in
  included modules name their module; one array allocation helper.
  Left: the match="$var" index (D185), the record constructor,
  XTDE3490, the group key equality.
- [x] **4.20 XSLT conformance sweep** (implementation). Owns the XSLT
  reader and lowering files it changes (`src/xslt_*`), the XSLT test
  driver, the files a leftover's root cause is in when no other packet
  has them open, their unit tests, and single insertions in the
  ledgers. In order of value: the XSLT leftovers of `docs/agents/QUEUE.md`
  (D185, D179, D195, D203, D206), then the largest failure clusters of a
  fresh XT40 run outside strm/: merge errors and validation, the record
  constructor, the schema-aware leftovers, xml-version and namespace
  cases, errors of inline functions and xsl:iterate in included modules,
  docbook-004, fn:transform's options, XTDE3052 (globals computed on
  first read, as a design if not done).
  **Done** 2026-10-19 (D230): XT40 11,826 to 11,912 passed, 0 errors,
  none lost; merge 97 to 107, record 35 to 37, validation 40 to 51,
  import-schema 200 to 206, error 448 to 474, namespace 209 to 217,
  normalize-unicode 18/18; QT4 fn-transform 130/137.
  Left (causes in D230): lazy globals (designed; needs the evaluator),
  typed modes and XTSE3105, XTDE3490, xml-version-007, namespace-2606
  and -6202, docbook-004, three fn:transform cases, xsl:copy of JNodes.
- [x] **4.21 Hoist guard, lazy globals, typed modes** (implementation).
  Owns `src/ir_loop_passes.c`, the new `src/ir_global_read.c`, the
  evaluator's global part and the files a fix needs when no other packet
  has them open, their unit tests, and single insertions in the ledgers.
  In order: the hoist pass's guard (merge-001a, -016; D223 kept), globals
  computed on first read (D230's design: a new IR expression and the
  evaluator; XTDE0640, XQDY0054 and D71 kept), typed modes reading name
  tests as schema-element() and XTSE3105.
  **Done** 2026-10-20 (D234): XT40 11,913 to 11,943 passed, 0 errors
  (document-4066 from wrong error to failure); QT4 XQ40 45,914, 0
  outcomes differ. Left: document-4066 (fn:document options).
- [x] **4.22 XSLT on the newest draft** (implementation, D245). Owns
  `grammars/xslt_pattern`, the pattern lowering and the XSLT files a
  draft change needs, `third_party/xslt40-test` (D13's script), their
  unit tests, the XT40 section of `docs/CONFORMANCE.md`, and single
  insertions in the ledgers. Fetch the newest XSLT 4.0 editor's draft
  (with XPath, XQuery and F&O 4.0) and the newest xslt40-test, run XT40
  before and after, classify each changed or failing case (agree:
  implement; disagree: the draft wins, recorded; neither: recorded),
  starting with the priority set and the "~T" patterns.
  **Done** 2026-10-20 (D252): neither moved. The drafts are those of 13
  October 2026 (qtspecs 72f2229), the suite is 91328fb; the draft has no
  type patterns, so 57 cases stay recorded as class (b) (53 "~T", 4
  priorities, 2 patterns the draft allows). New from the draft: typed="~T"
  on xsl:mode (6.9.4, XTTE3100). XT40 11,945 to 11,951 passed (+130
  wrong error), 0 errors, no case lost; the six from 7.20.
- [x] **4.23 XSLT conformance sweep** (implementation, D255). Owns the
  XSLT files a cluster's root cause is in, the XSLT and QT4 drivers'
  comparison, the XT40 section of `docs/CONFORMANCE.md` and README's
  XT40 row, their unit tests, and single insertions in the ledgers. From
  a fresh XT40 run, the failures and wrong-error passes outside D252's
  newest-draft cases, clustered by root cause, the largest fixed with
  general mechanisms, starting with the `error` set and the XSLT
  leftovers of `docs/agents/QUEUE.md`.
  **Done** 2026-10-20 (D260): XT40 12,081 to 12,156 passed (130 to 120
  wrong error), 437 to 362 failed, 0 errors, no case lost; gate rate
  95.7 %. Left (causes in D260): captured groups through templates,
  current-output-uri in templates called from functions,
  fn:apply-templates, absent-focus codes, Sequence JNode copies, XSLT
  without XPath's predeclared prefixes (a decision), and D230's.
- [x] **4.24 XSLT without XPath's predeclared prefixes** (implementation,
  D208, D245, D260). Owns the XSLT static contexts (`src/xslt_lower.c`,
  `src/xslt_static.c`, `src/xslt_evaluate.c`), prefix lookup in
  `src/xpath_static.c`, the XSLT functions' and the streamability
  analysis's name lookup, the stylesheets sxml ships that relied on the
  old behaviour (unit tests, the book, the bindings' tests, READMEs, CLI
  inputs), CHANGELOG, and single insertions in the ledgers. Take the
  draft's rule (5.3.1, 3.6.1) as the default and record the change for
  users.
  **Done** 2026-10-20 (D261): XT40 12,156 to 12,157 passed (120 wrong
  error), 362 to 361 failed, 0 errors, no case lost (namespace-6202).
  Left: run-time casts of strings to xs:QName still see the predeclared
  prefixes (`src/cast_lexical.c`, every language).
- [x] **4.25 XSLT conformance sweep** (implementation, D255, D260,
  D261). Owns the XSLT files a cluster's root cause is in, the XPath
  lowering's context variables, their unit tests, and single insertions
  in the ledgers. In order of value: D260's open list (captured groups
  through templates, fn:apply-templates, absent-focus codes, FORG0001 in
  coercions, Sequence JNode copies, prolog declarations in xsl:select,
  current-output-uri in functions, stream-available-004/005, the single
  cases), D261's run-time QName cast, then the largest clusters of a
  fresh run.
  **Done** 2026-10-20 (D262): XT40 12,157 to 12,187 passed (120 to 118
  wrong error), 361 to 331 failed, 0 errors, no case lost. Left (causes
  in D262): stream-available-004/005, casts and collations of inner
  static contexts, fn:load-xquery-module in XSLT, D260's single cases.
- [x] **4.26 XSLT conformance sweep** (implementation, D245, D262). Owns
  the XSLT files a cluster's root cause is in, the XSLT driver, the
  function library and loader files a cluster needs when no other
  packet has them open, their unit tests, and single insertions in the
  ledgers. From a fresh XT40 run at HEAD, the failures and wrong-error
  passes clustered by root cause, largest first; stream-available-004/005
  and D260's single cases; casts and collations of inner static
  contexts and src/xpath_lower.c stay with 2.24.
  **Done** 2026-10-20 (D273): XT40 12,191 to 12,209 passed (118 wrong
  error), 327 to 309 failed, 0 errors, no case lost (variable-0120 to
  wrong error as the draft decides). Left (causes in D273): "//"
  lowered right-nested (for 2.25), globals captured by XSLT's inline
  functions, CDATA text merging, fn:document strip-space "none".
- [x] **4.27 XSLT conformance sweep** (implementation, D245, D273). Owns
  the XSLT files a cluster's root cause is in, the XSLT driver, the
  serializer, tree, loader and function library files a cluster needs
  when no other packet has them open, their unit tests, and single
  insertions in the ledgers. From a fresh XT40 run at HEAD, failures and
  wrong-error passes by root cause, largest first; D273's open list
  (CDATA merging, static start-at, key namespace nodes, strip-space
  "none", XTTE lines); src/xpath_lower.c and XSLT's default collations
  and global capture stay with 2.25.
  **Done** 2026-10-20 (D274): XT40 12,210 to 12,226 passed (115 to 107
  wrong error), 308 to 292 failed, 0 errors, no case lost. Left (causes
  in D274): xsl:next-match in attribute sets, per-package strip-space
  for fn:document, xsl:source-document fragment identifiers, XTTE
  errors without a line (D271).
- [x] **4.28 XSLT conformance sweep** (implementation, D245, D274). Owns
  the XSLT files a cluster's root cause is in, their unit tests and CLI
  expectations, the book chapter whose output changes, and single
  insertions in the ledgers. From a fresh XT40 run at HEAD, failures and
  wrong-error passes by root cause, largest first; D274's open list.
  src/xpath_lower.c stays with the XPath sweep.
  **Done** 2026-10-20 (D276): XT40 12,239 to 12,250 passed (107 to 104
  wrong error), 279 to 268 failed, 0 errors, no case lost. Left (causes
  in D276): attribute sets in a changed focus see the enclosing rule,
  FORG0001 of failed coercion casts, dynamic XTTE0945, D274's
  fn:document and xsl:source-document items; xs:numeric in XPath 1.0
  compatibility for the XPath sweep.
- [x] **4.29 XSLT conformance sweep** (implementation, D245, D276). Owns
  the XSLT files a cluster's root cause is in, the function library and
  evaluator parts a cluster needs when no other packet has them open,
  their unit tests, and single insertions in the ledgers. From a fresh
  XT40 run at HEAD, failures and wrong-error passes by root cause,
  largest first, skipping what D252, D262, D273, D274 and D276 decided;
  D276's open list.
  **Done** 2026-10-20 (D281): XT40 12,250 to 12,261 passed (104 to 99
  wrong error), 268 to 257 failed, 0 errors, no case lost. Left (sizes
  in D281): xsl:evaluate and package codes, schema-aware codes,
  streaming, record coercion, fn:document per package, deterministic
  functions, split-when, JNode shallow copy, namespace undeclarations,
  copy-4308, the current rule in attribute sets.
