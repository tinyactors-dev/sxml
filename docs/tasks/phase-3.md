# Phase 3: XQuery 4.0 and serialization

Goal (PLAN.md): XQuery 4.0 (FLWOR, constructors, prolog, modules) and
serialization, lowered into the shared IR (D46) and run by the one
evaluator; `sxml xquery` and `sxml serialize` (docs/CLI.md). Gate: **85% of
the applicable XQ40 test cases pass, 75% of the XQ-only ones** (D95). Rules
for parallel packets are those of `docs/tasks/phase-2.md` (D45, D96).

## Overlap with Phase 2 (D96)

Packets marked **Early** may start as soon as the Phase 1 gate is closed
(the substrate freeze) and their "After" list is committed, while Phase 2
is still running; the Phase 2 file shows the waves they join. They own no
file of a Phase 2 packet. The other packets wait for the Phase 2 gate.
Early: 3.0, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9.

## How XQuery lowers (D55, `src/ir_internal.h`)

One AST holds XPath and XQuery kinds (3.1). Lowering parts (2.1) add the
XQuery kinds in their own files: FLWOR clauses to MAP, LET, CONDITIONAL
and SORT over tuple arrays; group by and windows to internal native rows;
constructors to the reserved constructor kinds; try/catch to TRY_CATCH;
typeswitch and switch to CONDITIONAL chains; prolog functions to IR
functions, prolog variables to globals with initializers; library modules
into the same unit. The evaluator runs the reserved kinds in the stub
files of 2.0. Nothing here adds an IR kind; a packet that needs one stops
and writes the reason under "Proposed".

## Parallelism plan

| Wave | Packets (run together) |
| --- | --- |
| E1 (with Phase 2 wave 1) | 3.0 API, 3.1 query AST, 3.5 constructors, 3.6 try/catch |
| E2 (with Phase 2 wave 2) | 3.2 front end, 3.3 XML serializer |
| E3 (with Phase 2 wave 3) | 3.4 other serializers, 3.7 FLWOR and expressions |
| E4 (with Phase 2 wave 4) | 3.8 group by and windows, 3.9 constructor lowering |
| 1 | 3.10 prolog, 3.13 driver |
| 2 | 3.11 modules, 3.12 XQuery API and CLI |
| 3 | 3.14 conformance sweeps |
| 4 | 3.15 review (gate) |

Critical path: 3.1 3.2 3.7 3.10 3.12 3.14 3.15. "Working XQuery" for the
owner is the end of 3.12: `sxml xquery` runs app-Walmsley and
app-FunctxFunctx queries with XML output.

## Packets

### 3.0 XQuery and serialization API (design) — Early
After 2.2. Owns `include/sxml/xquery.h`, `include/sxml/serialize.h`.
`xquery.h`: a compiled main module (lifecycle quintet, D51) from text with
the shared static context configuration; library modules found through the
resolver kind XQUERY_MODULE (2.2); evaluation with the shared dynamic
context; the serialization parameters the prolog declares, readable by the
caller. `serialize.h`: methods xml, xhtml, html, text, json, csv, adaptive;
one parameters struct with every Serialization 4.0 parameter (indent,
omit-xml-declaration, standalone, doctype-system/public, cdata-section-
elements, suppress-indentation, undeclare-prefixes, encoding (UTF-8,
UTF-16, US-ASCII, ISO-8859-1), byte-order-mark, normalization-form,
html-version, include-content-type, escape-uri-attributes, item-separator,
json-node-output-method, allow-duplicate-names, escape-solidus, canonical,
use-character-maps); parameters from an `output:serialization-parameters`
element or a map (SEPM0017, SEPM0019). Done when the header check, lints
and rubric pass and 3.3, 3.12 and the driver can be written against them.
**Done** 2026-10-17 (D106): `xquery.h` (`sxml_xquery_query`, evaluation
request, `sxml_xquery_query_get_serialization_parameters`); `serialize.h`
with every parameter, `sxml_serialization_flag`, encodings, character
maps and `sxml_serialization_parameter_reader`. Follow-up for 3.3:
non-adaptive methods should return UNSUPPORTED_FEATURE until implemented.

### 3.1 Query AST and parser seam (design) — Early
After 2.1. Owns `src/xpath_ast_internal.h`, `src/xpath_ast.c`,
`src/xpath_parse.c`, `tests/xpath_ast_test.c`. One kind per visible
production of `grammars/xquery` (D29) added to the AST (D52 conventions);
the CST walker of `xpath_parse.c` dispatches XQuery node types to a
parser part (3.2) the way lowering parts work. IR for five XQuery
examples written out in comments (FLWOR with group by, a window clause,
a direct constructor with namespaces, try/catch with finally, a prolog
function). Done when every XP40 query still builds the same AST.
**Done** 2026-10-17 (D111): XQuery kinds, fields, details and flags and
five examples in `src/xpath_ast_internal.h`; `sxml_ast_build_with_parser_part`
in `src/xpath_parse.c`, seam tests in `tests/xpath_ast_test.c`. 3.2 adds
`sxml_grammar_xquery` to the `sxml` link line of `CMakeLists.txt`.

### 3.2 XQuery front end (implementation) — Early
After 3.1. Owns `src/xquery_parse.c`, `tests/xquery_parse_test.c`. CST of
`grammars/xquery` to the AST: direct constructors (XQST0118 end tags,
XQST0040 duplicate attributes, namespace declaration attributes
XQST0070/0071, boundary whitespace marked, character and entity references
XQST0090), CDATA, pragmas, string constructors, the three D29 false
accepts. Done when all 43,663 valid XQ40 queries and the 145 parsable
library modules build an AST and the 772 XPST0003 cases fail statically
(`tools/qt4-parse-check.py --spec XQ40`).
**Done** 2026-10-17 (D121): the XQuery parser part in
`src/xquery_parse.c`; all 43,663 valid XQ40 queries and 145 library
modules build (70 raise the static error their test expects), all 772
XPST0003 cases fail. Prolog XQST0070 and expanded-name XQST0040 are
left to 3.10 and lowering.

### 3.3 Serializer: XML and XHTML methods (implementation) — Early
After 3.0, 2.4. Owns `src/serialize.c`, new `src/serialize_internal.h`,
`src/serialize_xml.c`, `tests/serialize_test.c`,
`tests/serialize_xml_test.c`. Sequence normalization (Serialization 4.0
"Sequence Normalization"), escaping, namespace fixup and undeclaration,
indentation with suppress-indentation, CDATA sections, doctype, XML
declaration and standalone, encodings with character references,
normalization-form (2.4), character maps; XHTML rules (empty elements,
`xml:lang`). Writes through `sxml_output_stream` (D50). Done when
method-xml, method-xhtml ≥ 90% under the 3.13 driver (unit cases until
then). Spec: Serialization 4.0 "XML Output Method", "XHTML Output Method".
**Done** 2026-10-17 (D120b): `src/serialize_internal.h` (character
pipeline, sequence normalizer), `src/serialize_xml.c` (XML, XHTML,
Canonical XML, adaptive nodes); HTML, text, JSON, CSV return
UNSUPPORTED_FEATURE. Unit cases only until the 3.13 driver; XP40
unchanged (26,951). The parameter reader is still undefined (3.4).

### 3.4 Serializer: HTML, text, JSON, CSV; fn:serialize — Early
After 3.3. Owns `src/serialize_html.c`, `src/serialize_json.c`,
`src/serialize_text.c`, `src/function_library_serialize.c`,
`tests/serialize_html_test.c`, `tests/serialize_json_test.c`. HTML method
(versions 4 and 5, void elements, raw text elements, boolean attributes,
include-content-type, escape-uri-attributes), text, JSON (nodes by
json-node-output-method, SERE0020/0022/0023), CSV; fn:serialize with
element and map parameters. Done when method-html, method-text,
method-json, method-csv, fn-serialize ≥ 90%. Spec: Serialization 4.0
per method.
**Done** 2026-10-17 (D128): every method on the 3.3 pipeline (escape
styles HTML, none, JSON, CSV; raw text), HTML as a mode of the XML node
writer, `src/serialize_parameters.c` (the parameter reader),
fn:serialize. fn-serialize 48 to 165/194 (wrong errors 48 to 3); the 32
left need expath binary, fn:parse-csv, fn:unparsed-text, an entity fix in
the parser, or XQuery character references. method-* are XQuery-only:
unit cases until 3.10 and 3.13.

### 3.5 Node constructors in the evaluator (implementation) — Early
After 2.0. Owns `src/evaluator_construct.c`,
`tests/evaluator_construct_test.c`. The seven constructor kinds and
SHALLOW_COPY over hand-built IR (as `tests/evaluator_test.c` does): a new
arena tree per constructed node (D40 builder), content sequences (XQuery
4.0 "Content": adjacent text merged, attributes after children XQTY0024,
document nodes unwrapped, XQDY0025 duplicate attributes), copy-namespaces
preserve/inherit, construction strip (untyped annotations), namespace
nodes (XQDY0101, XQDY0102), base URI and xml:base, the names of computed
constructors (XQDY0074, XQDY0044, XQDY0064). Done when every constructor
and copy rule has a unit case with allocation-failure loops. Spec: XQuery
4.0 "Node Constructors"; XSLT 4.0 "Constructing Complex Content".
**Done** 2026-10-17 (D110): `src/evaluator_construct.c`, 62 IR cases in
`tests/evaluator_construct_test.c`, each also under an allocation-failure
loop; IR modes and adders proposed for 3.9 (D110, "Proposed").

### 3.6 try/catch in the evaluator (implementation) — Early
After 2.0. Owns `src/evaluator_try.c`, `tests/evaluator_try_test.c`.
TRY_CATCH with name tests and wildcards, `$err:code`, description, value,
module, line-number, column-number, additional, the 4.0 `$err:map`, and
`finally` (an error in finally replaces the first). Only dynamic and type
errors with an error record are caught; LIMIT_EXCEEDED and allocation
failures are never caught. Unwinding uses the machine API of 2.0. Done
when every rule has a unit case, also for an error raised inside a
resumable native call. Spec: XQuery 4.0 "Try/Catch Expressions".
**Done** 2026-10-17 (D109): own error record when the caller passes
none; `$err:map` keys for lowering; IR adders for catch clauses proposed.
**3.6b IR extensions done** 2026-10-17 (D124): adders for name tests,
catch clauses and namespace bindings; `construction_flags` and
`name_namespaces` on constructors, honoured by the evaluator; verifier and
text form for TRY_CATCH and the constructor kinds.

### 3.7 FLWOR and other expressions (implementation) — Early
After 3.2. Owns `src/xquery_lower.c`, `src/xquery_lower_internal.h`,
`tests/xquery_lower_test.c`. Lowering part: for (at, allowing empty,
member, key/value), let, where, order by (stable, empty
greatest/least, collation), count, while (4.0), trace clause, return;
quantified expressions with types; typeswitch, switch (4.0 forms);
ordered, unordered; extension expressions (unknown pragmas ignored);
validate raises XQST0075 (D97). IR dumps of ten queries match golden
text. Done when prod-FLWORExpr, prod-ForClause, prod-LetClause,
prod-WhereClause, prod-OrderByClause, prod-CountClause, prod-WhileClause,
prod-TypeswitchExpr, prod-SwitchExpr ≥ 90% (through 3.12, unit IR
dumps before). Spec: XQuery 4.0 "FLWOR Expressions".
**Done** 2026-10-17 (D126): part `xquery` in `src/xquery_lower.c`,
internal entry `sxml_xquery_compile`; 12 IR dumps, static errors and
allocation failures in `tests/xquery_lower_test.c`. Scratch harness
(XQ40): For 139/212, Let 158/189, Where 68/85, OrderBy 133/210, Count
13/13, While 69/83, Typeswitch 80/88, Switch 34/58, TryCatch 190/212;
every other run case needs a prolog (3.10) or group by (3.8) but 8.

### 3.8 group by and window clauses (implementation) — Early
After 3.7. Owns `src/xquery_lower_window.c`, `src/xquery_flwor.c`,
`tests/xquery_flwor_test.c`. The internal native rows of D55 over tuple
arrays: group by (grouping keys, collations, deep-equal-style key
equality, non-grouping variables become sequences), tumbling and sliding
windows (start/end conditions as closures, `only end`, previous/next
variables). Done when prod-GroupByClause and prod-WindowClause ≥ 90%.
Spec: XQuery 4.0 "Group By Clause", "Window Clause".
**Done** 2026-10-17 (D137): group by as a tuple stream clause over the
internal group-by row; window clauses as segment clauses over the window
row; one condition-closure procedure, also for while. XQ40 (release):
prod-GroupByClause 35/40 (38 run; one prolog, one UCA tailoring, one
SORT over an empty input in the evaluator), prod-WindowClause 125/158
(139 run; every error a prolog, 3.10). Tests `xquery_flwor`.

### 3.9 Constructor lowering (implementation) — Early
After 3.2, 3.5. Owns `src/xquery_lower_construct.c`,
`tests/xquery_lower_construct_test.c`. Direct and computed element,
attribute, namespace, text, comment, PI and document constructors;
enclosed expressions; boundary-space; in-scope namespaces of constructed
elements (XQST0085); name expressions. Done when prod-DirElemConstructor,
prod-DirElemContent, prod-DirElemContent.namespace,
prod-DirElemContent.whitespace, prod-DirAttributeList,
prod-CompElemConstructor, prod-CompAttrConstructor and the other
prod-Comp*Constructor sets ≥ 90%. Spec: XQuery 4.0 "Direct Element
Constructors", "Computed Constructors".
**Done** 2026-10-17 (D127): part `construct`, `tests/xquery_lower_construct_test.c`
(golden IR, values, static errors, allocation-failure loops). The 13
sets pass 932 of 1,165 (scratch harness, 59 not run); all but 7 of the
run queries without a prolog pass (none of the 7 is lowering's), so the
90% bar waits for 3.10 to let prologs through (167 queries).

### 3.10 Prolog and static context (implementation)
After 3.7, 2.9, 2.13. Owns `src/xquery_static.c`, `src/xquery_prolog.c`,
`src/evaluator_global.c`, `tests/xquery_prolog_test.c`. Version,
namespace, default namespace, boundary-space, default collation, base
URI, construction, ordering, empty order, copy-namespaces, decimal-format
and option declarations (output:* into serialization parameters); context
value declaration; variable declarations (global initializers evaluated on
first read, XQDY0054 cycles, external with defaults); function
declarations (%public/%private, recursion, XQST0034); annotations
(%updating rejected, D97); item type and named record type declarations
(4.0; records registered in the type registry). Done when prod-VarDecl,
prod-VarDecl.external, prod-FunctionDecl, prod-Annotation,
prod-ItemTypeDecl, prod-NamedRecordTypeDecl, prod-DecimalFormatDecl,
prod-OptionDecl.serialization and the other prod-*Decl sets ≥ 90%. Spec:
XQuery 4.0 "Modules and Prologs".

**Done** 2026-10-17 (D135): `src/xquery_static.c`, `src/xquery_prolog.c`
(now also `sxml_xquery_lower` / `sxml_xquery_compile`),
`src/evaluator_global.c`, `tests/xquery_prolog_test.c`; core additions in
`src/xpath_static.c` (named item types, function namespace rule). See
docs/LOG.md for the counts; open items under "Proposed" (D135).

### 3.11 Modules (implementation)
After 3.10, 3.12. Owns `src/xquery_module.c`,
`src/function_library_module.c`, `tests/xquery_module_test.c`. Library
modules, `import module` through the resolver (location hints, several
modules per namespace, cycles), XQST0048, XQST0059, private names;
fn:load-xquery-module. Done when prod-ModuleImport and
fn-load-xquery-module ≥ 90%. Spec: XQuery 4.0 "Library Module", "Module
Import"; F&O 4.0 "fn:load-xquery-module".
**Done** 2026-10-17 (D148): `src/xquery_module.c` (module set through the
resolver, location hints, cycles, module URIs), per-module static contexts
read imports first and linked (`src/xquery_static.c`), one unit for the set
(`src/xquery_prolog.c`), `tests/xquery_module_test.c`, `sxml xquery` imports
local files. prod-ModuleImport 23 to 112 of 141 (79 %; the rest XQ10-only,
schema import, or Proposed). fn:load-xquery-module not done: it needs
cross-unit globals in the evaluator (Proposed, D148); fn-load-xquery-module
stays 0.

### 3.12 XQuery API and CLI (implementation)
After 3.3, 3.10. Owns `src/xquery.c`, `tests/xquery_test.c`,
`apps/sxml/command_xquery.c`, `apps/sxml/command_serialize.c`,
`apps/sxml/program.c`, `docs/CLI.md`, `tests/cli/`. Implements
`xquery.h`; `sxml xquery` and `sxml serialize` exactly as docs/CLI.md
lists them, with golden tests (XML, HTML, JSON, text output; static and
dynamic errors). Golden: `apps/sxml/command_xpath.c`. Done when 20 XQ40
queries round-trip through the public headers alone, with allocation-
failure loops on compile.
**Done** 2026-10-17 (D146): `src/xquery.c` over the shared unit
evaluation of `src/xpath.c` (`src/xpath_api_internal.h`) and
`sxml_xquery_compile_module`; output declarations through the parameter
reader's text form over the parameter document; the QT4 adapter deleted;
`sxml xquery`, `sxml serialize`, `sxml xpath --method` (shared
`apps/sxml/query_command.c`); `tests/xquery_test.c` (22 queries,
allocation-failure loop). XQ40 gate 96.0 %, XQ-only 83.9 %, method-*
433/444 run. `--output` and the JavaScript `compileQuery()` wait
(Proposed: the WebAssembly budget).

### 3.13 QT4 driver for XQ40 (implementation)
After 3.0, 2.16. Owns `tests/qt4/driver/`. `--spec=XQ40` runs; `<module>`
files through the resolver; `declared="true"` params; collections with a
`query`; serialization assertions (`serialization-matches`,
`assert-serialization-error`) and `assert-xml` through the 3.3 serializer;
a second ctest slice `qt4/xquery-slice` (about 200 XQ-only cases chosen
like the torture slice, baseline only goes up). Done when the slice runs
under `debug`, `debug-gcc` and `wasm` and goldens cover each new feature.
**Done** 2026-10-17 (D134): `--spec=XQ40` through the temporary
`xquery_adapter.c` (internal `sxml_xquery_compile`, xquery.h signatures;
3.12 deletes it); modules served by namespace and hint, declared params
bound only, collection queries, serialization-matches and
assert-serialization-error on `sxml_serialize_sequence`; assert-xml kept
on the adaptive method (writes nodes as XML). `qt4/xquery-slice`: 186
XQ-only cases, baseline 186, passes under debug, debug-gcc, wasm. XQ40
(release): 33,374/47,313 passed, gate rate 81.0 %, XQ-only 40.2 %.

### 3.14 Conformance sweeps (implementation, repeatable)
After 3.11, 3.13. As 2.19, over XQ40 sets (misc-CombinedErrorCodes,
app-Walmsley, app-FunctxFunctx, app-spec-examples first). Each sweep
claims its files in `docs/LOG.md` and stops after one session.
**Sweep 1 done** 2026-10-18 (D154): fn:load-xquery-module (global tables
per unit, a unit evaluation step), the QT4 driver's non-goal cases not
run; XQ40 in scope 96.4 % to 96.9 %, XQ-only 85.3 % to 97.9 %, errors 5
to 1 (modules-40). Open for the next sweep: D135 (b), (c), (g), a library
module's static context at run time, trusted=false (FODC0016).
**Sweep 2 done** 2026-10-18 (D158): default functions (focus-reading
defaults, keyword arguments after omitted ones), XQuery globals read at
call time by closures, fn:function-lookup of unit functions, "##any",
library module base URIs and source names at run time, trust, function
annotations; XQ40 in scope 96.9 % to 97.2 %, XQ-only 97.9 % to 98.8 %,
errors 1 to 0; XP40 97.1 %. Open: D158's "Not done".

### 3.15 Phase 3 review (review)
After everything. Owns the gate boxes. A separate agent (D11) runs the
rubric and re-checks each "done when"; open items go under "Proposed".
**Done** 2026-10-19 (D161): numbers at 50a3651, fuzzing with a new
XQuery target, the lowering machine's guard (D71), one heapsort and one
parameter document reader, fn:load-xquery-module in the run-time
function table, `xquery.h` stable, the D162 check of delayed features.

## Gate

- [x] XQ40 (D95 denominator) ≥ 85% pass, XQ-only cases ≥ 75%, method-*
  ≥ 80%, under `release`; XP40 still ≥ 90%. (agent review, D161: XQ40
  97.2 %, XQ-only 98.8 %, method-* 433 of 508; XP40 97.1 %)
- [x] `qt4/slice` and `qt4/xquery-slice` at their baselines in CI
  (`debug`, `debug-gcc`, `wasm`). (agent review, D161: 198/198 and
  186/186 under every preset, run locally as D123 says)
- [x] No XQ40 case crashes, leaks or ends in an "error" outcome.
  (agent review, D161)
- [x] `sxml xquery` and `sxml serialize` match docs/CLI.md. (agent
  review, D161; `--output` stays planned)
- [x] Review agent pass (D11), findings fixed; the owner is told XQuery
  works (D11 goal). (agent review, D161; the larger follow-ups are
  listed there, the owner's questions in docs/LOG.md)

## Later sweeps

### 2.21 XPath and XQuery conformance sweep (implementation)
After 5.14. Takes the "XPath and XQuery" leftovers of
`docs/agents/QUEUE.md` in order of value: (a) casts and castable to
array, map and record types (prod-CastExpr.composite,
prod-CastableExpr.composite; XPath 4.0, section 4.19.2); (b)
fn:function-lookup results that need the caller's focus; (c) path steps
over JNodes (prod-AxisStep.J, prod-PathExpr.J); (d) fn:serialize option
edge cases; (e) a built-in schema for the fn namespace (json-to-xml and
analyze-string validation) and fn:doc with xsd-validation; (f)
op-same-key and prod-NamedRecordTypeDecl. Files: the evaluator, the XPath
lowering and the function library parts the fixes touch (never `src/xslt*`
or `src/schema*`, which 4.19 and 5.14 own). Verification as D181 says;
before/after counts in each commit and the LOG entry.
**Done** 2026-10-19 (D190): composite casts (prod-Cast*Expr.composite
19/33 to 66 of 67), fn:function-lookup with the caller's focus (612/630
to 656/674), JNode paths (prod-AxisStep.J XQ 132 to 147, prod-PathExpr.J
177 to 184), distinct-values with the implicit timezone (op-same-key XQ
19 to 27), fn:doc's xsd-validation (65 to 69); XP40 30,931 to 31,098,
XQ40 41,273 to 41,456, none lost. Open: D190's "Not done".
