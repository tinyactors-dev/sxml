# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

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
