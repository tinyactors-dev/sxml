# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

- 1.19 done (D68, D72): operators. `src/operator.c` (promotion, numeric
  operators with int64 overflow into libmpdec, exact double idiv/mod,
  unary), `src/operator_temporal.c` (duration, date and time arithmetic,
  PR2628 scaling), `src/operator_compare.c` (value comparisons via
  fn:compare, general comparisons with the PR2218 untyped rules, GNode
  comparisons). P2 and P3 accepted into `xdm.h` (D68); recursion removed
  from `xdm_atomic.c` (D71). The 22 arithmetic and comparison slice lines
  hold as unit cases; 15 cases in `tests/operator_test.c`; `debug`,
  `debug-gcc`, `wasm`: 59/59; lint clean. Open: libmpdec FNT
  multiplication overflows the wasm stack (D72).
- 1.13 done (D70): `src/xdm_node.c` (node items, string value, typed
  value derived or from the tree, base URI through xml:base with RFC 3986
  resolution), `src/xdm_json_node.c` (JNodes on demand, identity and
  document order of generic nodes by child-position paths). op-union-406
  and fn-root-29 hold as unit cases; 12 cases in `tests/xdm_node_test.c`;
  `debug`, `debug-gcc`, `wasm`: 58/58; lint clean. Open: P7.
- 1.12 done (D63): static analysis and lowering. `src/xpath_static.c`
  (static context, names, sequence and item types, also for function
  library rows), `src/xpath_lower.c` (AST to IR: ANF blocks, chains in a
  loop, focus slots, captures, closures for placeholders and
  focus-dependent references, `sxml_xpath_compile`), `src/ir.c` (unit,
  text form, pass list), `src/ir_verify.c`. XPST0008/0017/0081/0051/0080/
  0039/0152/0021 with locations; K-QuantExprWithout-79, fn-filter-003,
  map-merge-001 fail statically; 28 slice and 46 construct IR dumps match
  golden text. With a permissive table, 29,653 of 32,409 valid XP40
  queries lower and verify (the rest: test environments, unsupported
  4.0 constructs). 23 cases in `tests/xpath_static_test.c` and
  `tests/ir_test.c`; `debug`, `debug-gcc`, `wasm`: 57/57; lint clean.
- 1.11 done (D67): trees. `src/tree.c` (wrapper, reference count and
  owner, namespace nodes on demand, document order, iterative
  `copy_node`), `src/tree_axis.c` (all 17 axes incl. -or-self),
  `src/arena_tree.c` (node segments, text store, text merging, duplicate
  attribute policies with a hash index, namespace fixup, limits,
  locations, unparsed entities, self-destruction). Node items now retain
  their tree (1.7 hand-off). 27 cases in `tests/tree_test.c` (64-row axis
  table) and `tests/arena_tree_test.c`; `debug`, `debug-gcc`, `wasm`:
  55/55; lint clean. Open: P6 (no-inherit copies).
- 1.15 done (D66): `src/cast.c`, `src/cast_lexical.c`,
  `src/cast_internal.h`: the F&O 4.0 casting table, lexical forms of every
  primitive type, canonical text (XPath rules for xs:double/xs:float,
  shortest digits), integer facets, xs:NMTOKENS, unions, enumerations and
  choices, cast and castable (FORG0001, FOCA0002, XPTY0004, FONS0004,
  FODT0001/2, XPST0080). The 17 cast slice lines pass as unit cases; 17
  cases in `tests/cast_test.c` incl. an allocation-failure loop;
  `debug`, `debug-gcc`, `wasm`: 53/53; lint clean.
- 1.10 done (D64): `src/xdm_array.c`, persistent arrays as AVL trees of
  flat (made arrays, O(1) get), run (part of a flat node, no copying)
  and branch nodes; append, put, insert, remove and slice are split plus
  join, O(log n). `src/xdm_function.c`: function items (signature must be
  a non-wildcard function type; captures retained, body retained once and
  released with the cell), arity check (XPTY0004), maps and arrays called
  as map:get and array:get (FOAY0001, also beyond `int64_t`). 8 cases in
  `tests/xdm_array_test.c` (random updates against a model with old
  versions checked, 10^5 appends with allocations per append bounded,
  10^5-deep nesting, failure loop, byte limit) and 6 in
  `tests/xdm_function_test.c`; `debug`, `debug-gcc`, `wasm`: 52/52; lint
  clean.
- 1.9 done (D61): `src/xdm_map.c`, maps and records. Entry blocks keep
  entries in order with a hash index (more than 8 entries) built while
  making, for the three duplicate key policies (XQDY0137); put and remove
  give persistent maps (entry tree by stamp, key tree by hash; O(log n),
  old versions intact); records hold field values, find fields through
  the registry and make their xs:string keys on first need; put on a
  record gives a map. map-merge-401/-402 order, map-get-014/-015 (NaN)
  and map-put-011/-024/-025 hold as unit cases. 9 cases in
  `tests/xdm_map_test.c` incl. an allocation-failure loop and a byte
  limit; `debug`, `debug-gcc`, `wasm`: 50/50; lint clean. Open: P3.
- 1.8 done (D62): `src/xdm_sequence.c`, integer ranges (`int64_t` bounds,
  `uint64_t` indices; 1 to 10^11 counts right on `wasm`), flattened
  concatenations with a part-end cache (binary search, lazy parts read
  only as far as needed), memoized lazy sequences whose kept error is
  raised again on every later read, `sxml_sequence_test_emptiness`.
  16 cases in `tests/xdm_sequence_test.c` incl. an allocation-failure
  loop; `debug`, `debug-gcc`, `wasm`: 49/49; lint clean. Seen: wasm
  `xpath_parse` segfaults intermittently (about 1 run in 8) on HEAD too.
- 1.7 done (D60): item heap and atomic core. `src/xdm_internal.h` is the
  shared interface for 1.8-1.11 and 1.15 (item layout, cell classes with
  sequence read operations, iterative release, byte accounting incl.
  decimal coefficients, `sxml_atomic_item_make_taking_decimal`, static
  error records). `src/xdm.c`: heap, raised error, vectors, iterator,
  builder; `src/xdm_atomic.c`: every primitive and built-in derived type
  with value-space checks, atomic-equal, hash (1, 1.0, 1.0e0, 1.0f hash
  alike), fn:compare per PR2256. 36 cases in `tests/xdm_test.c` and
  `tests/xdm_atomic_test.c`; `debug`, `debug-gcc`, `wasm`: 48/48; lint
  clean. Open: tree retention hand-off to 1.11; P2 (heap in
  `sxml_atomic_comparison`).
- 1.6 done (D58): type registry, all of `types.h`: static built-in
  descriptions (names interned per pool), interned registered item types
  in a normal form, nominative record types (declare, define, recursive),
  record field and enumeration value indexes, derivation, and the
  XPath 4.0 §3.3 subtype relations (choices, unions, enumerations, node,
  JNode, function, map, array, record rules; transitivity folded into the
  rules) with a depth and step budget. map-put-002-hof holds.
  `tests/types_test.c`: 28 cases incl. allocation-failure loops; `debug`,
  `debug-gcc`, `wasm`: 44/44 tests; lint clean.
- 1.16 done (D59): XPath front end, `src/xpath_parse.c` (CST to AST in one
  iterative cursor walk; XPST0003 with location for ERROR/MISSING, A.3.2
  delimitation and prefixed lookup keys; literals and names decoded;
  sequence types through `() instance of`) and `src/xpath_ast.c` (AST
  lifecycle and builder). All 198 slice queries and all 32,542 valid XP40
  queries build; the two D29 false accepts are now rejected. 32 unit cases
  in `tests/xpath_ast_test.c` and `tests/xpath_parse_test.c`; `debug`,
  `debug-gcc`, `wasm` pass, lint clean.
- 1.17 done (D53): regex seed. `src/regex_internal.h` is the interface
  the D9 engine keeps (flags with FORX0001; immutable compiled regex with
  FORX0002 and exact memory requirements; per-thread matcher with
  `match_at`, `find`, `find_next` over disjoint matching segments, step
  and backtrack-stack limits). Hand-written recursive-descent parser
  rather than a tree-sitter grammar (reasons in D53). Subset: classes with
  subtraction, `\s \d \w` (Unicode 16.0 tables), greedy and reluctant
  quantifiers, groups, alternation, `^ $`, back-references, flags
  `s m i x q c`; the rest is FORX0002 "not supported". fn-tokenize-1a
  and -35 pass as unit cases. `debug`, `debug-gcc`, `wasm`: 43/43; lint
  clean.
- 1.2 done (D57): libmpdec 4.0.1 vendored and patched (scoped
  allocation of D42, size_t-based configuration, WASI), `src/decimal.c`:
  parse, canonical text, exact add/subtract/multiply, divide (34 digits),
  idiv, mod, round (all nine modes), compare, int64/uint64/double/float
  conversions. `tests/decimal_test.c`: 24 cases incl. allocation failures
  and two heaps on two threads. `debug`, `debug-gcc`, `wasm`: 42/42; lint
  clean. WebAssembly growth: +89 KB stripped (+35 KB gzip).
- 1.4 and 1.5 done (D52, D55), design only: `src/xpath_ast_internal.h`
  (one kind per visible XPath production, children by grammar field,
  fixed-capacity arrays sized from the CST, front-end entry points for
  1.16), `src/ir_internal.h` (units, atoms, slots, explicit focus slots,
  reserved XQuery/XSLT kinds; worked IR for inline-fn-005, `/a/b[2]`, a
  FLWOR and an apply-templates), `src/ir_pass_internal.h` (pass seam,
  verifier invariants), `src/evaluator_internal.h` (dynamic context,
  evaluate, coercion, document loader) and
  `src/function_library_internal.h` (native rows with F&O type text, one
  part per file, function table interface). Headers compile alone under
  Clang and GCC with the project flags; clang-tidy and ast-grep clean.
- 1.1 done (D54): name pool, all of `name.h`: hash-indexed interning of
  URIs, prefixes and names, static well-known namespaces, find without
  interning, byte limit; `src/name_internal.h` gives identifier limits.
  `tests/name_test.c`: 18 cases incl. allocation-failure and limit loops.
  `debug`, `debug-gcc`, `wasm`: 41/41 tests; lint clean.
- 1.0 done: `src/*.c` and `tests/*_test.c` registered by glob (D45);
  `sxml` links the xml and xpath grammars and `sxml_mpdecimal` once
  vendored; `tests/qt4/driver`, `tests/fuzz` (`SXML_BUILD_FUZZERS`) and
  `third_party/mpdecimal` join when their `CMakeLists.txt` exists. P1
  accepted as D50: `SXML_STATUS_INPUT_OUTPUT_FAILED`, used by the CLI
  platform layer. `debug`, `debug-gcc`, `wasm`: 40/40 tests; lint clean.
- 1.3 done (D51): public API headers `xml.h` (reusable XML parser into
  any tree builder, or check only; diagnostics with line and column),
  `processor.h` (processor owning pool, registry and heap; limits; resource
  resolver for `fn:doc` with a table resolver; system and fixed clocks;
  `sxml_processor_parse_document`), `xpath.h` (static and dynamic context
  configurations shared with later XQuery and XSLT, compiled expressions,
  evaluation, SequenceType from text) and `serialize.h` (output stream,
  output buffer, adaptive method). Design only: header check under
  `debug`, `debug-gcc` and `wasm`, lint clean. Implemented by 1.14, 1.22
  and 1.23; the driver (1.24) and the CLI's `check` and `xpath` use them.
- CLI (D44, `docs/CLI.md`): CLI framework in `apps/cli_framework/`
  (command tables, parser, help, "did you mean", exit codes 0-4, output
  streams) and the `sxml` program in `apps/sxml/` with `help`, `--help`,
  `--version` and `version [--format=text|json]` only. 4 unit suites and
  30 golden end-to-end tests in `tests/cli/` pass under `debug`,
  `debug-gcc`, `release` and `wasm`; lint clean, new rule
  `input-output-outside-platform-layer`. Next: `sxml check` after 1.14,
  `sxml xpath` after 1.23. Proposed P1: an input/output status.
- Phase 1 planned: `docs/tasks/phase-1.md` splits the torture slice into
  30 packets with owned files, dependencies and an 11-wave parallel plan.
  Decisions: glob-based registration and file ownership (D45), internal
  tree-walking ANF-like IR with a pass seam (D46), public `xml.h`,
  `processor.h`, `xpath.h`, `serialize.h` (D47), regex seed (D48),
  libmpdec in Phase 1 (D49). The 198 slice queries use about 53 `fn:`
  functions, 9 `map:` and 9 `array:` functions and 17 `xs:` constructors.
- 0.8 done (review agent, D11): rubric run over all ten headers. Fixed:
  wildcard name tests in node tests; lookup of namespace URIs without
  interning; fn:empty on lazy sequences no longer swallows errors
  (`sxml_sequence_test_emptiness`); coercion is the caller's in dynamic
  calls; computed error descriptions and values kept by the item heap
  (`sxml_item_heap_raise_error`, D41); duplicate attribute policy for XQuery
  versus XSLT builders; ownership verbs, iterator ABI and comment wrapping
  (D43). libmpdec hooks confirmed process-global; scoped thread-local
  allocator decided (D42). D3 (Phase 0 time box) recorded, D4-D10 confirmed.
  6/6 suites pass under `debug`, `debug-gcc`, `wasm`; lint clean; CI green.
  Phase 0 gate passed by agent review.
- 0.7 done: substrate headers `name.h` (name pool, D31), `types.h` (type
  registry with constant built-in identifiers, interned item types, sequence
  types, D32), `tree.h` (tree interface with preorder node numbering,
  namespace nodes by number, document order by tree identifier, axes, tree
  builder events, arena tree, D39-D40) and `xdm.h` (item heap; 16-byte items
  and sequences; immutable reference-counted values; atomic datums; vector,
  range, concatenation and lazy sequences; function bodies as an interface;
  ordered persistent maps, records, arrays, JNodes; D33-D38). Header order
  and resolver deferral in D30, thread confinement in D35. Design only:
  header check and lints green under Clang and GCC, no implementation.
- Open questions for 0.8: whether `sxml_tree_operations` should be split
  (one optional typed-value op versus a separate schema interface in
  Phase 5); whether decimals should get an inline fast path; how a host
  shares one parsed document between threads (D35 defers it).
- Query grammars (D29): tree-sitter grammars for XPath 4.0
  (`grammars/xpath`), XQuery 4.0 (`grammars/xquery`, inherits XPath) and
  XSLT 4.0 patterns (`grammars/xslt_pattern`, inherits XPath), with one shared
  external scanner (`grammars/common/query_scanner.h`) and corpus tests.
  `tools/qt4-parse-check.py` parses the QT4 suite: XP40 0 false rejects of
  32,118 valid queries, XQ40 0 of 43,663; false accepts 2 of 363 and 3 of
  772 XPST0003 queries, left to the tree builder; XSLT patterns 3 of 19,168
  (invalid DocBook patterns). The XSLT test suite still uses type patterns
  (`~T`) that the 13 October 2026 draft has dropped (136 patterns).
- XML 1.0 grammar `grammars/xml` (D28): strict grammar plus external
  scanner (tag stack, text tokens, [2] Char), corpus tests (59 pass).
  W3C XML suite: valid 601/601 and invalid 175/175 parse cleanly, not-wf
  870/948 rejected by the grammar; the rest is listed for the tree builder in
  `grammars/xml/README.md`. QT4 + XSLT 4.0 suites: every well-formed UTF-8
  `*.xml` file (3583 of 3612) parses cleanly. `grammars/CMakeLists.txt`
  builds every generated grammar.
- `string.h` implemented in `src/string.c` (D27): strict UTF-8 validation,
  ASCII flag, codepoint count, `codepoint_at`, iterator, codepoint
  comparison; 29 cases in `tests/string_test.c` pass under `debug`,
  `debug-gcc` and `wasm`; lint clean.
- 0.5–0.6 done: core headers `status.h` (statuses, `SXML_NODISCARD`, error
  record and diagnostic, D21), `view.h` (D19), `memory.h` (views, checked
  arithmetic, memory requirements, allocator interface without reallocate
  (D22), default and counting allocators (D23), arena (D24)) and `string.h`
  (D25, designed, not implemented). Golden module `src/memory.c` +
  `src/allocator.c` (+ `src/status.c`) with 47 unit test cases in
  `tests/memory_test.c`, `allocator_test.c`, `status_test.c`; 5/5 suites pass
  under `debug`, `debug-gcc`, `release`, `wasm` and a shared GCC build; lint
  clean. New lint `pointer-parameter-without-length` (D26). Failure-path
  convention: outputs untouched on failure (D20).
- Open questions for 0.8: should `string.h` be implemented before the Phase 0
  gate; is the borrowed-view lifetime of error records (D21) acceptable for
  host bindings, which will copy them anyway.
- Conformance suites vendored under `third_party/` (D13 revised);
  `tools/update-test-suites.sh` replaces the fetch script.
- 0.1–0.4 done: CMake presets `debug`, `debug-gcc`, `release`, `msvc`, `wasm`
  (D14); test runner with one executable per module (D16); `tools/lint.sh`
  with clang-tidy and ast-grep rules (D17); CI workflow (D18, not yet run on
  GitHub). tree-sitter v0.27.1 vendored and built on every preset, wasm
  included; grammar generation script and `sxml_add_grammar` ready (D15).
  2/2 tests pass under ASan+UBSan (Clang, GCC), release and WASI.
- 0.9 done: `tools/fetch-test-suites.sh` pins qt4tests and xslt40-test (D13);
  `docs/QT4_CATALOG.md` documents the catalog; `tests/qt4/torture-slice.txt`
  lists 198 XP40 tests, checked by `tools/check-test-list.py`.
- Repository bootstrapped with plan, guidelines, glossary and decisions.
- Next: Phase 0 tasks 0.1–0.4.

## Open questions at the Phase 0 gate (task 0.8)

Questions the review left open, for the owner or a later phase. None blocks
Phase 1.

1. **Thread-local scope for libmpdec (D42).** It keeps heaps independent and
   leaves no state between calls, but it is the one thread-local in sxml.
   Accept it, or require the own-decimal fallback?
2. **Phase 0 time box (D3).** Recorded by the review agent because the
   original D3 was never written; the owner may want another length.
3. **Typed values (from 0.7).** Kept as one optional `get_typed_value`
   operation in `sxml_tree_operations`. Phase 5 publishes a separate schema
   interface only if validated trees need more than typed values and the
   `is_id`/`is_idrefs`/`is_nilled` flags; "interfaces are not extended in
   place" applies.
4. **Inline decimals (from 0.7).** Not added: integers that fit `int64_t`
   are inline already (D36); a scaled-integer fast path for short decimals
   can be a later cell format without an interface change.
5. **Sharing one parsed document between threads (D35).** Still deferred;
   the likely shape is an immortal, frozen tree with a read-only name pool.
6. **Error record lifetime for host bindings (from 0.5).** Kept as borrowed
   views (D21, D41); bindings copy what they keep.
7. **Strings without a copy.** `sxml_atomic_item_make` copies text, so
   `fn:substring` and string concatenation cannot share or adopt a buffer.
   If profiling in Phase 2 shows the cost, add a constructor that adopts a
   heap buffer, with a decision record (the substrate is frozen at the
   Phase 1 gate).
8. **Schema-derived atomic types (Phase 5).** The registry has no function
   to register them yet; it will be a new function, with a record.
9. **Document and collection resolution (D30).** Designed with the dynamic
   context in Phase 1, outside the substrate headers.
