# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

- Phases 2-4 planned (D95-D97): `docs/tasks/phase-2.md` (21 packets:
  seams, regex, Unicode, collations, formatting, resources, driver, JS
  package and release 0.2.0), `phase-3.md` (16: AST, front end,
  serializers, constructors, try/catch, FLWOR, group/window, prolog,
  modules, API, CLI) and `phase-4.md` (19: API, driver for xslt40-test,
  stylesheet AST and reader, value templates, patterns, templates and
  modes, grouping, keys, numbering, output, accumulators, packages).
  Targets (D95): XP40 >= 90%, XQ40 >= 85% (XQ-only >= 75%), XSLT 4.0 in
  scope (about 8,700 cases) >= 70%. Early packets of Phases 3 and 4 may
  start after the Phase 1 gate (D96).
- 1.27 done (D94): `tools/run-qt4.sh` (one `sxml-qt4` process per test
  set, parallel, timeout, crash recorded per set) and
  `tools/conformance-report.py` (dashboard, merged JSON, slice-baseline
  check) generate `docs/CONFORMANCE.md`. XP40, release: 47,313 cases,
  21,443 passed (1,007 wrong error), 6,933 failed, 489 errors, 18,448 not
  run (74.3 % of run cases pass); slice 198/198; 737 sets, none crashed;
  24 s on 4 cores. CI: jobs `conformance` (blocking on the slice,
  artifacts) and `fuzz` (60 s per target, D86); runs on main are no
  longer cancelled in progress.
- 1.25 done (D92): `sxml-qt4 run` (`test_run.c`, `assertion.c`,
  `report.c`, `reason.c`, `command_run.c`). Each test in its own processor
  and 512 MiB budget allocator, 10^8 steps, leak check per test; every
  assertion kind (assert-xml by parse-and-deep-equal; serialization ones
  not-run); outcomes pass / wrong-error / fail / error / not-run; text and
  JSON reports. Slice: **198/198 pass** (`debug`, `debug-gcc`, `wasm`);
  ctest `qt4/slice` against `tests/qt4/slice-baseline.txt` (198), goldens
  `qt4/run_*`. Whole catalog, XP40, release, one process per test set (20
  s on 3 cores; no crash, no leak): 47,313 cases: 21,443 passed (1,007
  wrong error), 6,933 failed, 489 errors, 18,448 not run (XQ-only and
  unsupported dependencies). By area (cases: passed/failed/errors/not
  run): fn 17,250: 8,374/5,751/90/3,035; prod 14,932: 7,859/521/391/6,161;
  op 4,789: 3,765/14/8/1,002; misc 3,042: 159/305/0/2,578; app 3,034:
  318/95/0/2,621; array 537: 456/35/0/46; map 361: 334/17/0/10; xs 227:
  178/14/0/35; math 181: 0/181/0/0; upd, ft, bin, method, expath: not run.
  Causes: XPST0017 unknown function 5,596 (+148 keyword arguments);
  FORX0002 regex category/block escapes 405, lookaround 42, name escapes
  32; FOCH0002 collations 100; XPTY0004 110; errors: not lowered in Phase
  1 (171 constructs, 157 for member/key-value/destructuring let, 43
  dynamic node tests, 24 item axis), 15 step/memory limits (fold-left/
  right-022/023, RangeExpr-408/410, cbcl-matches-004/038). Most failing
  sets: fn-graphemes 918, fn-matches.re 502, fn-format-number 216,
  fn-compare 204, fn-function-lookup 165, fn-map-to-element 153,
  fn-element-to-map 151, misc-ConstructorKeywords and
  misc-FocusConstructors 148 each, fn-serialize 146,
  prod-NamedFunctionRef 144, fn-deep-equal 120, fn-duplicate-values 106,
  fn-format-date 104, prod-PathExpr.J 104, prod-AxisStep.J 97.
- 1.23b follow-ups (D91): 62 library functions added (new parts
  `src/function_library_math.c` and `src/function_library_qname.c`, rows
  in the node, string, uri, sequence and context parts), found by listing
  the XP40 fn-/map-/array-/math- test sets whose function the library
  lacks (139 functions, 5,796 cases before; 77 and 4,165 after; the list
  is in the task file). Adaptive output writes the type annotation's name
  (`xs:dateTimeStamp(...)`, qtspecs PR 2981). The static base URI is
  lexical: IR units carry it, so a function item keeps its own.
  fn:string-join, sort, distinct-values, deep-equal, count of a lazy
  sequence and the regex functions charge evaluation steps (D89).
- 1.23 done (D88, D89): processor and XPath API. `src/processor.c`
  (clocks, table resolver, processor in one block with pool, registry,
  heap and the whole function library, document parsing, the
  per-evaluation document loader: fn:doc stable per URI, FODC0002/0006),
  `src/xpath.c` (compile, evaluate with its own dynamic context so
  evaluations nest, sequence types from text). Static base URI passed to
  fn:doc, parse-xml, json-to-xml and the new fn:static-base-uri and
  fn:resolve-uri (`src/function_library_uri.c`, RFC 3986 resolution
  shared from `src/xdm_node.c`). Limit comments fixed (heap, not C
  stack); P9 accepted as D89: an evaluation step bound (default 10^9).
  `sxml xpath` (`--context-item`, `--param`, `--method=adaptive`) with 19
  golden tests. Tests `processor` (7) and `xpath` (15, 30 slice queries
  through public headers). `debug`, `debug-gcc`, `wasm` pass (111 tests).
- 1.26 follow-up (D90): map:merge merges into its largest map, so folds
  that merge one entry into an accumulator, on either side, are O(n log n)
  (were O(n^2): 6 s and 18 s under ASan on two QT4 seeds). New internal
  `sxml_map_put_first` (`src/xdm_map_internal.h`). Scaling test
  `map_merge_scales`.
- 1.26 done (D86, P9): fuzz targets. `tests/fuzz/` (xml_parse, xpath_compile,
  xpath_evaluate, regex, cast; per-input leak checks and sxml-error
  statuses), preset `fuzz`, `tools/fuzz.sh`, `tools/fuzz-seeds.py` (xmlconf
  and QT4 seeds). 10 minutes each, 4 at once: cast 1.66M inputs, regex
  784k, xml 80k, compile 35k: clean; evaluate: clean but for 2 QT4 seeds
  that time out (fold-left over map:merge, 6 s and 18 s under ASan:
  map:merge re-inserts every entry; P9 for unbounded evaluation). Fixed:
  UTF-16 surrogates broken by the D80-to-D81 rename (xml/encodings failed
  on HEAD); null+0 for empty tokenized attribute values (xml.c) and empty
  lexical forms (cast_lexical.c); O(depth^2) content walk in xml.c (100000
  levels: 600 s to 7 s); choice map key types rejected by the type
  registry (fn:replace's signature); comparisons walking ranges for dates
  (evaluator.c). Each with a regression test. CI 60 s per target waits for
  1.27 (`ci.yml`).
- 1.22 done (D82): JSON, parse-xml, adaptive output. `grammars/json`
  (strict RFC 8259 tree-sitter grammar; D12 kept, reasons in D82),
  `src/json.c` (one cursor loop into JSON events; value sink and XML
  sink; xml-to-json writer with FOJS0006/0007 checks and a key index),
  `src/function_library_json.c` (parse-json, json-to-xml, xml-to-json,
  jtree, parse-xml through the document loader), `src/serialize.c`
  (output buffer, adaptive method on a heap stack). 12 cases in
  `tests/json_test.c` (all 22 JSON/parse-xml/JNode slice lines, 100,000
  nesting levels, allocation-failure loops), 8 in `tests/serialize_test.c`.
  `debug`, `debug-gcc`, `wasm` pass except the `xml` test (fails at HEAD,
  not 1.22); lint clean, no call cycles. Not yet: a `fallback` function,
  strip-space="all".
- XML fix (D87, P8 accepted): the position tracker moves back in place
  instead of restarting from byte 0, so parsing is linear: `sxml-qt4 list
  tests/qt4/torture-slice.txt` 23.3 s -> 0.76 s (release). The `xml` ctest
  failure (UTF-16 surrogates, 0xD810 for 0xD800) and its 600 s depth case
  (ts_tree_cursor_current_depth) were fixed in d44b472 and 14ece34; `xml`
  now runs 6.7 s debug, 8.5 s debug-gcc, 0.8 s wasm. Open: tree-sitter
  parses about 3.5 MB/s (9.6 MB in 2.7 s, release).
- 1.18b done (D85): follow-ups from 1.20/1.21. Dynamic calls on a
  sequence of functions (evaluator's `call_each` body) and dynamic partial
  application per function; maps and arrays become JNodes on the left of
  `/` and as step origins; the coercion rules on an explicit stack
  (choices/unions by alternative, typed arrays, maps, records, JNodes, full
  implicit-casting table, function coercion even for instances), so
  fn:replace's `$replacement` has its F&O 4.0 type again; integer ranges
  match in O(1) (codepoints-to-string(65 to 2^32) fails fast, FOCH0001);
  a left-out body-supplied parameter before a keyword argument gets ();
  array:index-of; static namespaces kept in the IR unit for run-time
  casts to xs:QName. Scratch QT4 comparison over ~70 sets: no regressions;
  DynamicFunctionCall 33 -> 65/73, RecordType 12 -> 16, ChoiceItemType
  23 -> 27, PathExpr-J 30 -> 48.
- 1.24 done (D83, P8): the QT4 driver `sxml-qt4` in `tests/qt4/driver/`
  (catalog, environments, applicability, test lists, `list` command) on
  the CLI framework, reading the suite with sxml's own XML parser through
  the public API. `sxml-qt4 list tests/qt4/torture-slice.txt`: 198/198
  resolve and apply to XP40 (`debug`, `debug-gcc`, `wasm`). Whole suite:
  47,313 cases, 32,928 XP40 by spec (as `docs/QT4_CATALOG.md`), 28,865
  applicable after dependencies and environments. Tests: `qt4_driver`
  (unit, in-memory files, allocation-failure loop) and 14 `qt4/*` golden
  tests. Blocked-ish: P8, the XML parser's positions are O(n^2), so the
  slice listing takes 160-200 s under ASan (13 s release, 21 s wasm).
- 1.20 done (D78): core function library. `src/function_library.c`
  (index over the configured parts, type text resolved on first find,
  xs: constructors, `sxml_function_table_find`), parts numeric, string,
  sequence, node, context, error, plus `function_library_deep_equal.c`
  (explicit stack) and `function_library_case.c` (Unicode 16.0 case maps,
  `tools/generate-case-tables.py`). ~110 functions; fn:sort and fn:replace
  resumable. 18 cases in `tests/function_library_test.c` (slice lines,
  allocation-failure loops); `debug`, `debug-gcc`, `wasm`: function_library
  passes (the new `xml` test fails on gcc/wasm, not 1.20); lint clean, no
  call cycles. Scratch QT4 run of the F&O sets touched: about 93% of
  applicable cases (rest: collations, regex seed limits, evaluator gaps).
- 1.14 done (D81, D69): the XML parser. `src/xml.c` (CST walk over
  explicit entity frames, all 16 builder checks, namespaces, events),
  `src/xml_encoding.c` (UTF-8, UTF-16, ISO-8859-1, US-ASCII; positions),
  `src/xml_entity.c` (DTD tables). W3C suite through the library: valid
  601/601, invalid 175/175, not-wf 948/948 (`xml_conformance` ctest, ~6 s
  under ASan; skipped under WASI). QT4: 1046/1051 `*.xml` parse, the 5
  others are bad on purpose. `sxml check [--format=text|json] FILE...`
  with 9 golden tests; the CLI framework gained `sxml_cli_file_reader`.
  P6 accepted as D69 (`ignores_parent_namespaces`).
- 1.21 done (D79, D80): higher-order, map and array functions.
  `src/function_library_higher_order.c` (F&O 4.0 2.5 except fn:sort, 13,
  fn:op; traversal, fold and sort engines shared through
  `src/function_library_higher_order_internal.h`), `src/function_library_map.c`
  (all of 14.4; map:build/merge as one resumable assembly),
  `src/function_library_array.c` (15.2 except array:index-of). Every
  function-calling row is a resumable body: fold-right over 10^6 items in
  constant C stack. 10 cases in `tests/function_library_higher_order_test.c`
  (about 50 slice lines, the 4.0 additions, depth limit, allocation-failure
  loop) over a test table (1.20's library not yet committed); `debug`,
  `debug-gcc`, `wasm`: 61/61; lint clean, no call cycles. Open:
  DynamicFunctionCall-146 needs the evaluator to call a sequence of
  functions; array:index-of waits for 1.20's deep-equal.
- 1.18 done (D75, D76, D77, D73): the evaluator. `src/evaluator.c`
  (explicit-stack machine: tasks for expressions, calls and native steps
  in allocator memory, tail calls reuse frames, depth as LIMIT_EXCEEDED),
  `src/evaluator_function.c` (atomization, EBV, type matching with
  backtracking for choices, coercion incl. function coercion),
  `src/evaluator_path.c` (XNode and JNode axes, document order, node
  sets). Resumable native bodies for 1.21 in
  `function_library_internal.h`. P4, P5, P7 accepted. 19 cases in
  `tests/evaluator_test.c` (about 90 slice lines without library calls,
  100k-deep recursion, 100k fold-left steps, allocation-failure loop);
  `debug`, `debug-gcc`, `wasm`: 60/60; lint clean, no call cycles.
- D71 for lowering, type resolution and type text: `src/xpath_lower.c` is a
  machine of frames in allocator memory (procedures with resumable steps),
  `src/xpath_static.c` and `src/ir.c` use 64-frame stacks; lowering's
  default depth is 10,000 (heap) instead of 32 (wasm stack). IR dumps of
  all 29,653 lowerable XP40 queries unchanged (464 unsupported, 2,292
  static errors, no leaks); 100,000 nested levels compile (wasm: 3.3 s for
  the suite). misc-no-recursion on, call-cycle allowlist empty.
- D71 for the regex seed (D53 amended): `src/regex.c` parses in one
  loop with a stack of group frames (16 inline, then `stack_allocator`),
  emits each atom once and copies it for quantifiers, reads and matches
  class subtraction chains in loops; misc-no-recursion clean. Fixed a
  write past the arrays for a trailing x{0}. Groups and subtraction
  nested 10,000 deep compile and match (also on wasm); 400,000 random
  regexes behave as before; regex 26/26, `debug`, `debug-gcc`, `wasm`:
  59/59; lint clean.
- D71 for the value model: `src/xdm_map.c` (insert, remove, replace
  value, key search, build) and `src/xdm_array.c` (join, split) walk their
  AVL trees in loops over fixed 96-entry paths on the C stack (no tree
  that fits in memory is higher than 91); `sxml_node_make_base_uri` is a
  loop; the dump helpers of `tests/tree_test.c` and
  `tests/xpath_parse_test.c` use an explicit stack and parent links.
  Indirect recursion through cell classes audited: releases only queue,
  concatenations hold no concatenations; only caller-supplied lazy
  generators can nest. `debug`, `debug-gcc`, `wasm`: 59/59; lint and
  misc-no-recursion clean.
- D74: libmpdec's transpose buffers (up to 128 KiB on the stack) moved to
  the heap through the decimal scope, marked "sxml patch"; long products
  (10^6000 * 10^6000) no longer trap on the 64 KiB wasm stack. Audit:
  largest libmpdec frame now 1.2 KiB on wasm, measured worst stack of
  multiply and divide 2.9 KiB, flat up to 16 million digits; recursion
  bounded, no digit cap needed. Four new cases in `tests/decimal_test.c`
  (10,000 and 60,000 digits, allocation failures); `debug`, `debug-gcc`,
  `wasm`: 59/59; lint clean.
- D71 for types and casts: `src/types.c` evaluates subtype questions as an
  and/or tree of subtype goals on a fixed explicit stack (192 frames, no
  allocation, as the header promises; last sub-goals replace their frame)
  and searches union members for derivation on a stack of 256;
  `src/cast.c` casts union, choice and list targets on a heap stack of
  cast frames (at most 33). Behaviour and limits unchanged except that
  choices of choices searched past 192 frames now answer false (D58).
  New tests nest function types, choices, enumerations and document tests
  10,000 deep and choice targets 10,000 deep; `debug`, `debug-gcc`,
  `wasm`: 59/59; lint and misc-no-recursion clean on these files.
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
