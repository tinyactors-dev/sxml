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

### 4.14 Accumulators (implementation)
After 4.12. Owns `src/xslt_accumulator.c`, `tests/xslt_accumulator_test.c`.
xsl:accumulator without streaming, accumulator-before and -after,
use-accumulators on modes and sources. Done when decl-accumulator ≥ 80%.

### 4.15 XSLT API and CLI (implementation)
After 4.13. Owns `src/xslt.c`, `tests/xslt_test.c`,
`apps/sxml/command_transform.c`, `apps/sxml/program.c`, `docs/CLI.md`,
`tests/cli/`. Implements `xslt.h`; `sxml transform` exactly as docs/CLI.md
lists it, with golden tests. Done when the 4.1 driver runs the slice at
its baseline under `debug`, `debug-gcc` and `wasm` and 20 transformations
round-trip through the public headers alone.

### 4.16 Packages, minimal (implementation)
After 4.15. Owns `src/xslt_package.c`, `tests/xslt_package_test.c`.
xsl:package, use-package through the resolver (package-version ranges),
visibility (public, private, final, abstract, hidden), accept, expose,
override for functions, templates, variables, attribute sets and modes.
Done when decl-package, use-package, accept, expose, override,
attr-package-version ≥ 70%.

### 4.17 Conformance sweeps (implementation, repeatable)
After 4.15. As 2.19, over in-scope XSLT sets (misc-regex-syntax 987 and
misc-error 500 first: they measure the regex engine and error codes
cheaply). Each sweep claims its files in `docs/LOG.md`.

### 4.18 Phase 4 review (review)
After everything. Owns the gate boxes. A separate agent (D11) runs the
rubric and re-checks each "done when"; open items go under "Proposed".

## Gate

- [ ] In-scope XSLT 4.0 cases (D95) ≥ 70% pass under `release`; the core
  sets apply-templates, mode, match, for-each-group, sort, key, number,
  choose, copy, value-of, result-document, function ≥ 80% each.
- [ ] XQ40 still ≥ 85%, XP40 ≥ 90%; all ctest slices at their baselines.
- [ ] No in-scope case crashes, leaks or ends in an "error" outcome.
- [ ] `sxml transform` matches docs/CLI.md.
- [ ] Review agent pass (D11), findings fixed; the owner is told XSLT
  works (D11 goal).
