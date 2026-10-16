# Phase 1: Torture slice

Goal (PLAN.md): a minimal XML parser and XPath 4.0 subset exercising all of
the value model, native and WebAssembly, passing the 198 tests of
`tests/qt4/torture-slice.txt` under sanitizers; then the substrate freezes.
Packets of one agent session each run in parallel in one working tree; each
names the files it **owns**, and concurrent packets never share one.
**Design** packets may change headers; **implementation** packets must not
(they write the need under "Proposed" in `docs/DECISIONS.md`). Until the
gate, substrate headers change only through a decision record.

## Rules for parallel packets (D45)

- **Registration is by glob.** After 1.0, `src/*.c` is the library, each
  `tests/<name>_test.c` is ctest test `<name>`, and `tests/qt4/driver`,
  `tests/fuzz`, `third_party/mpdecimal` join when their `CMakeLists.txt`
  exists. Packets add files, never lines to a shared list.
- **Module prefixes own files**, `src/` stays flat: `src/<module>.c`,
  `src/<module>_<part>.c`, `src/<module>_internal.h`, `tests/<module>*_test.c`.
- **Ledgers are shared** (`docs/DECISIONS.md`, `GLOSSARY.md`, `LOG.md`): one
  small insertion, committed at once and alone; re-read before claiming a
  D-number. Glossary terms come before code. Root `CMakeLists.txt`,
  `CMakePresets.json`, `ci.yml`: only the owning packet edits them. `apps/`
  and `docs/CLI.md` belong to the CLI (D44).
- **Every packet** imitates `src/memory.c` and `tests/memory_test.c`
  (lifecycle, allocation-failure loops, D23) unless it names another golden
  file; checks leaks with `sxml_item_heap_read_counts` where values are made;
  cites the specification in comments; is done when `debug`, `debug-gcc` and
  `wasm` pass, `tools/lint.sh` is clean and the slice count (after 1.25) has
  not dropped.

## Parallelism plan

| Wave | Packets (run together) |
| --- | --- |
| 0 | 1.0 scaffolding |
| 1 | 1.1 name pool, 1.2 decimal, 1.3 public API, 1.4 AST, 1.5 IR, 1.17 regex |
| 2 | 1.6 types, 1.16 XPath front end |
| 3 | 1.7 heap and atomics, 1.12 static analysis and lowering |
| 4 | 1.8 sequences, 1.9 maps, 1.10 arrays, functions, 1.11 tree, 1.15 cast |
| 5 | 1.13 node items and JNodes, 1.14 XML parser, 1.19 operators |
| 6 | 1.18 evaluator, 1.22 JSON, 1.24 driver catalog, 1.26 fuzzing |
| 7 | 1.20 core functions, 1.21 higher-order functions |
| 8 | 1.23 processor and XPath API |
| 9 | 1.25 driver assertions |
| 10 | 1.27 dashboard, 1.28 WebAssembly slice |
| 11 | 1.29 review (gate) |

A packet starts once those after "After" are committed. Critical path: 1.0
1.1 1.6 1.7 1.11 1.13 1.18 1.20 1.23 1.25 1.27 1.29. The CLI (not a packet)
adds `sxml check` after 1.14 and `sxml xpath` after 1.23.

## Packets

### 1.0 Scaffolding (design, build only)
After the Phase 0 gate. Owns root `CMakeLists.txt`, `tests/CMakeLists.txt`,
`third_party/CMakeLists.txt`. Globs for `src/*.c` and `tests/*_test.c`;
`sxml` links `sxml_grammar_xml`, `sxml_grammar_xpath` and, when present,
`sxml_mpdecimal`; guarded `add_subdirectory` for the optional directories
(`tests/fuzz` only with `SXML_BUILD_FUZZERS`); the CLI's `apps` line kept.
Done when presets pass unchanged and a probe source and test need no edit.
**Done** 2026-10-16: globs in place, probe verified under `debug`; also
added `SXML_STATUS_INPUT_OUTPUT_FAILED` (D50, was P1).

### 1.1 Name pool (implementation)
After 1.0. Owns `src/name.c`, `src/name_internal.h`, `tests/name_test.c`.
All of `name.h` (D31): interning with a hash index, well-known namespaces,
find without interning, byte limit. Done when every function is tested with
limit and allocation-failure loops. Spec: XDM 4.0 §5.6 (QNames).
**Done** 2026-10-16 (D54).

### 1.2 Decimal arithmetic with libmpdec (implementation)
After 1.0. Owns `third_party/mpdecimal/`, `third_party/mpdecimal.version`,
`src/decimal.c`, `src/decimal_internal.h`, `tests/decimal_test.c`. Vendor
mpdecimal 4.0.x, patched as D42, in Phase 1 (D49). Internal API: parse,
canonical text, arithmetic incl. `idiv` and `mod`, compare, round, to and
from `int64_t` and `double`. Done when two heaps on two threads work and the
`wasm` size growth is logged. Spec: F&O 4.0 "Arithmetic operators on numeric
values" (decimal precision), D36.
**Done** 2026-10-16 (D57): two heaps on two threads tested; `wasm` growth
+89 KB stripped (+35 KB gzip), logged.

### 1.3 Public API: XML parser, processor, XPath, adaptive output (design)
After 1.0. Owns `include/sxml/xml.h`, `processor.h`, `xpath.h`,
`serialize.h` (D47). `xml.h`: bytes (encoding detected, source name, limits)
into any `sxml_tree_builder`, errors with line and column (D21).
`processor.h`: owns pool, registry and heap (D30); host resolver for
`fn:doc` (D30); implicit timezone, current dateTime. `xpath.h`: static
context configuration (namespaces, external variables and types, static base
URI), compile, evaluate with context item, variables and resolver.
`serialize.h`: adaptive method only. Done when header check, lints and rubric
pass and the driver (1.24) and CLI can be written against these alone.
**Done** 2026-10-16 (D51).

### 1.4 XPath abstract syntax tree (design)
After 1.0. Owns `src/xpath_ast_internal.h`. Index-addressed nodes in an
arena (D24), one kind per visible production of `grammars/xpath` (D29),
names as written (1.12 resolves them), a source location per node. Covers
every construct the slice uses, notably inline functions with types, EQName
function references, `?` placeholders, static and dynamic `=>`, `map{}` and
`{}`, square and curly arrays, `?*`, `?(expr)`, unary lookup, and sequence
types such as `map(K, V)`, `function(T) as U`, `array(empty-sequence())`.
Spec: XPath 4.0 Appendix A.
**Done** 2026-10-16 (D52).

### 1.5 Shared IR, optimizer seam, evaluator interfaces (design)
After 1.0. Owns `src/ir_internal.h`, `src/ir_pass_internal.h`,
`src/evaluator_internal.h`, `src/function_library_internal.h` (D46). IR as
in D46: atoms, explicit focus variables, steps with a context atom, map,
filter, quantified and sort nodes, functions with capture lists, static
function references, `try`/`catch` and node construction reserved for
XQuery and XSLT. The pass interface (Phase 1: a verifier only). Evaluator
entry points and `struct sxml_dynamic_context`. The native function table
(name, arities, signature, body, focus dependence) with one row array
declared per `src/function_library_<part>.c` of 1.20 to 1.22. Done when IR
for `let $f := function($x){$x + .} return $f(4)` and `/a/b[2]` is written
out in comments, and an XQuery FLWOR and an XSLT `apply-templates` lower into
it with no new node kind (or a named extension point). Spec: XPath 4.0 §2.2,
§3.4 (coercion rules).
**Done** 2026-10-16 (D55).

### 1.6 Type registry and built-in types (implementation)
After 1.1. Owns `src/types.c`, `src/types_builtin.c`, `src/types_internal.h`,
`tests/types_test.c`. All of `types.h` (D32, D43). Done when `map(xs:string,
xs:integer+)` is a subtype of `function(xs:string) as item()*`
(map-put-002-hof). Spec: XPath 4.0 "Subtype Relationships"; XDM 4.0 §4.
**Done** 2026-10-16 (D58).

### 1.7 Item heap and atomic core (implementation)
After 1.2, 1.6. Owns `src/xdm.c`, `src/xdm_internal.h`, `src/xdm_atomic.c`,
`tests/xdm_test.c`, `tests/xdm_atomic_test.c`. Heap and cells (D33), raised
errors (D41), tree identifiers, all atomic items and datums (D36),
`atomic_equal`, `compare`, `hash`; vector sequences, builder, iterator. Done
when equal numerics of different types hash alike. Spec: XDM 4.0 §3, §5.
**Done** 2026-10-16 (D60): shared interface `src/xdm_internal.h` (cell
classes, iterative release, heap byte accounting); node-item tree
retention is a two-line hand-off to 1.11; QName order in `fn:compare`
needs the heap (P2).

### 1.8 Range, concatenation and lazy sequences (implementation)
After 1.7. Owns `src/xdm_sequence.c`, `tests/xdm_sequence_test.c`. Ranges
with `int64_t` bounds and `uint64_t` counts (D34), flattened concatenation,
memoized lazy sequences that raise their error on every read,
`test_emptiness` (D43). Done when a 10¹¹-based range counts right on `wasm`.
**Done** 2026-10-16 (D62): all of `sxml_sequence_make_integer_range`,
`_concatenate`, `_make_lazy`, `_test_emptiness`; XPDY0130 only for counts
past 2^64 - 1; lazy errors kept and raised again per read.

### 1.9 Maps and records (implementation)
After 1.7. Owns `src/xdm_map.c`, `tests/xdm_map_test.c`. D37: entry order,
lazy hash index, persistent put/remove, duplicate policies, records sharing
registry shapes. Done when map-merge-401 order and map-get-014 (NaN key)
hold and `map:put` on a record gives a map. Spec: XDM 4.0 §8.2, §8.3.
**Done** 2026-10-16 (D61): entry blocks (index built while making, for
duplicates), persistent maps of two AVL trees, records with lazily made
field keys; P3 (`sxml_map_get_entry` on a record may fail for memory).

### 1.10 Arrays and function items (implementation)
After 1.7. Owns `src/xdm_array.c`, `src/xdm_function.c`,
`tests/xdm_array_test.c`, `tests/xdm_function_test.c`. Persistent arrays
with O(log n) put, insert, remove and slice; function items (D38); maps and
arrays answering `sxml_function_item_call`. Done when 10⁵ appends stay
O(n log n) and captures are released with the item. Spec: XDM 4.0 §8.1, §8.4.
**Done** 2026-10-16 (D64): AVL arrays of flat, run and branch nodes (split
and join; 10^5 appends at most 1 + 3 x height allocations each), function
items with retained captures and body state, maps and arrays as map:get
and array:get.

### 1.11 Tree: interface, axes, arena tree, builder (implementation)
After 1.7. Owns `src/tree.c`, `src/tree_axis.c`, `src/arena_tree.c`,
`src/tree_internal.h`, `tests/tree_test.c`, `tests/arena_tree_test.c`. All
of `tree.h` (D39, D40, D43), every axis including `-or-self`, namespace
fixup, limits, `copy_node`. Done when axis results on a fixed document match
a table. Spec: XDM 4.0 §6; XPath 4.0 "Axes".
**Done** 2026-10-16 (D67): 64-row axis table holds; node items retain
their tree (1.7 hand-off); no-inherit copies wait on P6.

### 1.12 Static analysis and lowering (implementation)
After 1.3, 1.5, 1.6, 1.16. Owns `src/xpath_static.c`, `src/xpath_lower.c`,
`src/ir.c`, `src/ir_verify.c`, `tests/xpath_static_test.c`, `tests/ir_test.c`.
Static context; names (XPST0081), variables to slots (XPST0008), functions
by name and arity against the table interface (XPST0017; tests bring their
own table); placeholders, arrows and lookups lowered as specified; capture
lists; verifier. Done when K-QuantExprWithout-79, fn-filter-003 and
map-merge-001 fail statically and IR dumps of 20 slice queries match golden
text. Spec: XPath 4.0 §2.2.1, §3.
**Done** 2026-10-16 (D63): also `src/xpath_static_internal.h`; 28 slice
dumps and 46 construct dumps match golden text; all valid XP40 queries
lower without a verifier failure (P4, P5 proposed).

### 1.13 Node items, JNodes, node accessors (implementation)
After 1.9, 1.10, 1.11. Owns `src/xdm_node.c`, `src/xdm_json_node.c`,
`tests/xdm_node_test.c`. Node items; string value, typed value, base URI;
JNodes made on demand, identical for equal parent and key. Done when
op-union-406 and fn-root-29 hold as unit cases. Spec: XDM 4.0 §8.5.
**Done** 2026-10-16 (D70): both hold in `tests/xdm_node_test.c` (12
cases, RFC 3986 §5.4 resolution examples, allocation-failure loop); P7
open (lazy values in JNode making).

### 1.14 XML parser (implementation)
After 1.3, 1.11. Owns `src/xml.c`, `src/xml_encoding.c`, `src/xml_entity.c`,
`src/xml_internal.h`, `tests/xml_test.c`, `tests/xml_conformance_test.c`.
CST of `grammars/xml` to builder events with all 16 checks of
`grammars/xml/README.md`, encodings UTF-8, UTF-16, ISO-8859-1, US-ASCII; CST
freed before return (D12). Done when the W3C suite (`third_party/xmlconf`,
Fifth Edition) gives 601/601 valid, 175/175 invalid, 948/948 not-wf right,
and every slice document parses. Spec: XML 1.0 Fifth Edition; Namespaces in
XML 1.0 Third Edition.
**Done** 2026-10-16 (D81): 601/601, 175/175, 948/948 through
`tests/xml_conformance_test.c`; 1046/1051 QT4 `*.xml` (the 5 others are
not well-formed on purpose); `sxml check` added; P6 done as D69.

### 1.15 Casting and lexical forms (implementation)
After 1.7. Owns `src/cast.c`, `src/cast_lexical.c`, `src/cast_internal.h`,
`tests/cast_test.c`. Lexical and canonical forms of every primitive type the
slice uses, integer subtypes with facets, `xs:NMTOKENS`; cast and castable
(FORG0001, FOCA0002, XPTY0004). Done when the cast slice lines pass as unit
cases. Spec: F&O 4.0 "Casting"; XSD 1.1 Part 2.
**Done** 2026-10-16 (D66): all 17 cast slice lines pass as unit cases in
`tests/cast_test.c` (17 cases, allocation-failure loop included).

### 1.16 XPath front end: CST to AST (implementation)
After 1.4. Owns `src/xpath_parse.c`, `tests/xpath_parse_test.c`. Any
`ERROR` or `MISSING` is XPST0003 with a location; the checks D29 leaves to
the builder (A.3.2 delimitation, prefixed lookup key); literals decoded.
Done when all 198 slice queries build an AST and every XP40 query that
`tools/qt4-parse-check.py` accepts also builds one.
**Done** 2026-10-16 (D59): also `src/xpath_ast.c` (D52) and
`tests/xpath_ast_test.c`; 198/198 slice queries and 32,542/32,542 valid
XP40 queries build, the two D29 false accepts now fail with XPST0003.

### 1.17 Regular expression seed (implementation)
After 1.0. Owns `src/regex.c`, `src/regex_internal.h`, `tests/regex_test.c`.
D48: a backtracking matcher for a subset of the dialect (classes, `\s \d
\w`, quantifiers, groups, alternation; flags `s i x m`) with a step limit,
behind the interface the D9 engine keeps. Done when fn-tokenize-1a, -35
pass and unsupported syntax raises FORX0002. Spec: F&O 4.0 "Regular expression syntax".
**Done** 2026-10-16: hand-written parser, not tree-sitter (D53); also
`src/regex_unicode.c` (\d, \w from Unicode 16.0) and
`tools/generate-regex-tables.py`.

### 1.18 Evaluator core (implementation)
After 1.8, 1.12, 1.13, 1.15, 1.19. Owns `src/evaluator.c`,
`src/evaluator_path.c`, `src/evaluator_function.c`, `tests/evaluator_test.c`.
Focus (XPDY0002), frames, IR function bodies and closures (D38), coercion
(XPTY0004), paths in document order, predicates, `!`, quantifiers, `treat`
(XPDY0050), `instance of`, node comparisons, map and array constructors and
lookups, errors with code, description and value (D41). Done when the slice
queries without library calls pass (filterexpressionhc2, bang-1,
inline-fn-*, Lookup-225, ...) and deep recursion gives a limit status, not a
stack overflow. Spec: XPath 4.0 §3, §4.
**Done** 2026-10-16 (D75): explicit-stack machine (D71), resumable native
bodies for 1.21; P4 (D77), P5 (D76), P7 (D73) accepted; slice lines
without library calls pass as unit cases in `tests/evaluator_test.c`.

### 1.19 Comparisons and arithmetic (implementation)
After 1.8, 1.15. Owns `src/operator.c`, `src/operator_compare.c`,
`src/operator_internal.h`, `tests/operator_test.c`. Numeric promotion,
arithmetic with integer overflow into libmpdec, value and general
comparisons with untypedAtomic rules. Done when those slice lines pass as
unit cases. Spec: XPath 4.0 "Arithmetic Expressions", "Comparison Expressions".
**Done** 2026-10-16 (D68, D72): also `src/operator_temporal.c`, GNode
comparisons; P2 and P3 accepted.

### 1.20 Function library: core (implementation)
After 1.17, 1.18. Owns `src/function_library.c`,
`src/function_library_{numeric,string,sequence,node,context,error}.c`,
`tests/function_library_test.c`. Used by the slice: `abs avg boolean
ceiling codepoints-to-string concat count current-date current-time data
deep-equal distinct-ordered-nodes doc error false floor last local-name not
number position QName random-number-generator reverse root round sort
starts-with string string-join string-length subsequence substring sum
tokenize true upper-case year-from-date`; for the driver and a margin:
`contains current-dateTime distinct-values empty ends-with exists head max
min name normalize-space tail`; `xs:` constructors. Done when each has a
unit case (deep-equal 4.0 rules, FOER0000). Spec: F&O 4.0 per function.

### 1.21 Function library: higher-order, maps, arrays (implementation)
After 1.18. Owns `src/function_library_{higher_order,map,array}.c`,
`tests/function_library_higher_order_test.c`. `apply filter fold-left
fold-right for-each for-each-pair function-arity function-lookup
function-name op partial-apply`; `map:contains entry for-each get keys merge
put remove size`; `array:append flatten fold-left for-each get put size sort
subarray`. Done when those slice lines pass as unit cases and
`fold-right(1 to 1000000, ...)` uses constant C stack. Spec: F&O 4.0.
**Done** 2026-10-16 (D79, D80): also the rest of F&O 4.0 sections 2.5
(but fn:sort, D78), 13, 14.4 and 15.2 (but array:index-of), fn:op;
`src/function_library_higher_order_internal.h` (shared resumable engines).

### 1.22 JSON, parse-xml and adaptive output (implementation)
After 1.5, 1.13, 1.14. Owns `src/json.c`, `src/function_library_json.c`,
`src/serialize.c`, `tests/json_test.c`, `tests/serialize_test.c`.
`parse-json` (options `duplicates`, `null`, `number-format`; FOJS0001),
`json-to-xml`, `xml-to-json` (FOJS0006), `jtree`, `parse-xml`; `serialize.h`.
Done when the JSON slice lines pass as unit cases. Spec: F&O 4.0 "Functions
on JSON data"; RFC 8259; Serialization 4.0 "Adaptive output method".

### 1.23 Processor and XPath API (implementation)
After 1.14, 1.18, 1.20, 1.21, 1.22. Owns `src/processor.c`, `src/xpath.c`,
`tests/processor_test.c`, `tests/xpath_test.c`. `processor.h` and `xpath.h`;
`fn:doc` through the resolver with one tree per URI (fn-doc-6), a new tree
per `parse-xml` call (parse-xml-012). Done when 20 slice queries round-trip
through the public API alone, with allocation-failure loops on compile.

### 1.24 QT4 driver: catalog and environments (implementation)
After 1.3, 1.14, the CLI framework (D44). Owns `tests/qt4/driver/`. Golden:
`apps/sxml`. Reads catalogs with sxml's parser through public headers;
XP40 spec and dependency filtering (`docs/QT4_CATALOG.md`); environments
(sources, params, static base URI); runs a list file or `--test-set=NAME`.
Done when it resolves the 198 slice cases natively and on `wasm`.

### 1.25 QT4 driver: assertions and reports (implementation)
After 1.23, 1.24. Owns `tests/qt4/driver/assertion.c`, `report.c` and, from
now, the driver's `CMakeLists.txt`. Every assertion except the serialization
ones (reported "unsupported", failing); `assert` binds `$result`; a wrong
error code passes and is reported. Counts per test set, text and
`--format=json`. ctest `qt4_slice` fails on any slice failure. Done when
198/198 pass under `debug` and `debug-gcc`.

### 1.26 Fuzz targets (implementation)
After 1.12, 1.14. Owns `tests/fuzz/`, a `fuzz` preset in `CMakePresets.json`.
libFuzzer targets (Clang) for XML parsing and XPath compilation, seeded from
`third_party/xmlconf` and the slice, with memory limits. Done when each runs
10 minutes clean (LOG) and CI runs each for 60 seconds.

### 1.27 Conformance dashboard (implementation)
After 1.25. Owns `tools/conformance-report.py`, `docs/CONFORMANCE.md`,
`.github/workflows/ci.yml`. Driver JSON to `docs/CONFORMANCE.md` (slice,
each XP40 test set, total; run, pass, fail, skip; commit, date). CI runs the
slice (blocking) and all XP40 sets (informational). Done when regeneration
on a clean tree gives no diff.

### 1.28 WebAssembly slice (implementation)
After 1.25. Owns `tools/run-wasi.mjs`. `ctest --preset wasm` runs
`qt4_slice` with the suite directory preopened (the CI `wasm` job needs no
edit); sizes of `libsxml.a` and the driver module become the first size
budget (PLAN.md, risks). The TypeScript package waits for Phase 2 (D47).
Done when 198/198 pass under `wasm`.

### 1.29 Phase 1 review (review)
After everything. Owns the gate boxes. A separate agent (D11) runs the rubric
and re-checks each "done when"; open items go under "Proposed".

## Gate

- [ ] 198/198 slice tests pass in CI: `debug`, `debug-gcc`, `wasm`.
- [ ] XML and XPath fuzz targets run clean.
- [ ] CI generates `docs/CONFORMANCE.md`; Phase 2 and 3 targets proposed.
- [ ] "Proposed" substrate changes decided; a record freezes the substrate.
- [ ] Review agent pass (D11), findings fixed.
