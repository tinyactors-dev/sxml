# Decisions

Each decision states what was chosen and why. A decision is revisited only when
something concrete blocks progress; the reopening is recorded here.

## Accepted

### D1. Name and prefix
sxml (small XML implementation). Public prefix `sxml_`, macro prefix `SXML_`.

### D2. Core language
Portable C17 with C23 features behind macros, following `docs/C_GUIDELINES.md`.
Chosen for the widest linking reach (every platform and language can call C).
Memory-safety risk is mitigated by sanitizers, fuzzing and checked arithmetic.

### D4. License: MIT

### D5. Build on an existing engine?
Start from scratch in C. Study Xee (Rust, MIT, https://github.com/Paligo/xee)
closely: it implements most of XPath 3.1 with one IR and bytecode interpreter
shared by XPath and XSLT, the same architecture as this plan.

### D6. String representation
UTF-8, with a per-string flag for ASCII-only content so codepoint indexing is
O(1) in the common case. Codepoint count cached on first use.

### D7. Ordered maps (XDM 4.0)
XDM 4.0 maps keep insertion order. Design after Saxon 13 (Michael Kay,
"Implementing Maps for XPath 4.0", XML Prague 2026):
- a built-once list of key/value pairs plus a key-to-offset index,
  built lazily on first lookup;
- a separate persistent layer for later `map:put` / `map:remove`;
- shared key "shapes" for records;
- entries addressable by position, needed for JNode sibling navigation.

### D8. Decimal arithmetic
libmpdec (mpdecimal, BSD-2-Clause). Check whether its allocator hooks are
process-global; if so, wrap it so the no-global-state rule holds.

### D9. Regular expressions
Own engine for the XSD/XPath regex dialect, designed after Saxon HE's engine
(regexml is a Rust port that shows the approach). Check the license of any
source before porting code.

### D10. WebAssembly toolchain
WASI SDK producing a standalone module, wrapped by a small hand-written
TypeScript package. The core does no I/O of its own, so Emscripten's
JavaScript system emulation is not needed.

### D11. Phase gates closed by agent review
Recorded 2026-10-16. The project owner chose, when asked, that work proceeds
through the phases until XQuery 4.0 and XSLT 4.0 work, with each gate closed by
a separate review agent instead of a human. The review agent checks the work
against `docs/C_GUIDELINES.md` and the gate criteria; its findings are fixed
before the next phase starts. Gate boxes ticked this way are marked
"(agent review)" so a later human review can find them. The substrate freeze at
the Phase 1 gate still applies: later substrate changes need a record here.

### D12. Parsing with tree-sitter
Recorded 2026-10-16, at the owner's request ("use tree-sitter wherever you need
parsing"), confirmed for XML as well as the query languages.
- The tree-sitter runtime (MIT) is vendored under `third_party/tree-sitter/`,
  pinned to a release tag.
- Grammars live under `grammars/<language>/grammar.js`. The generated parser
  (and any external scanner) is committed so the build needs no Node.js; a
  script regenerates it with a pinned `tree-sitter-cli`.
- sxml walks the concrete syntax tree and builds its own structures (the XML
  tree arena, the XPath/XQuery abstract syntax, then the shared IR). The
  concrete syntax tree is freed before the parse function returns.
- Any `ERROR` or `MISSING` node is a static error (XML: not well-formed;
  XPath/XQuery: `XPST0003`). Error recovery never accepts input.
- What a context-free grammar cannot express (XML well-formedness constraints,
  entity expansion, namespace processing, XQuery lexical constraints) is
  checked in the tree-building step.
- Vendored third-party code is exempt from `docs/C_GUIDELINES.md` and the lint
  gates. The tree-sitter runtime allocates with `malloc` unless
  `ts_set_allocator` is called, and that hook is process-global, so sxml does
  not call it. This exemption is limited to the parse step.

### D13. Conformance test suites are vendored
The W3C QT4 test suite (`qt4cg/qt4tests`) and the XSLT 4.0 test suite
(`qt4cg/xslt40-test`) are committed under `third_party/qt4tests/` and
`third_party/xslt40-test/` at a pinned commit, recorded in
`third_party/<suite>.commit`. `tools/update-test-suites.sh` moves them to a
new commit. Test lists in `tests/qt4/` refer to test cases by name.
Revised 2026-10-16: first fetched on demand into an ignored directory; the
owner asked for them to be vendored so they are not downloaded again for every
session and CI run. From xslt40-test, `report/`, `admin/unicode.xml`, the
zipped QT3 tests and the DocBook toolchain `.jar` files are left out, because no
test reads them (about 370 MB remain for both suites).
Pinned commits: qt4tests `166453359bd7770fb113b6d9c9015df7f7fa5d02`,
xslt40-test `91328fbf0fed4a5d2813eb3661808ee53cb961ba` (both HEAD on
2026-10-16). Spec applicability: a test-case `spec` dependency replaces the
test-set one; no `spec` means all versions (see `docs/QT4_CATALOG.md`).

### D14. Build layout and presets
Recorded 2026-10-16 (task 0.1). CMake 3.24+, Ninja. Presets: `debug` (Clang,
ASan + UBSan), `debug-gcc`, `release` (default compiler), `msvc` (Visual
Studio 2022, Windows only), `wasm` (`cmake/wasi-sdk.cmake`, wrapping the SDK's
`wasi-sdk-p1.cmake`; SDK from `WASI_SDK_PREFIX` or `/opt/wasi-sdk`; WASI SDK 34
in CI). Build trees go to `build/<preset>`.
- One library target `sxml`, static by default; `-DSXML_BUILD_SHARED=ON`
  builds it shared. Public functions are marked `SXML_EXPORT`
  (`include/sxml/export.h`); project code has hidden visibility.
- `sxml_apply_project_flags(target)` applies C17 and the guideline flags
  (MSVC: `/W4 /WX`) to project targets only; `sxml_apply_third_party_flags`
  builds vendored and generated code as C11 with warnings off.
- Sanitizers (`SXML_ENABLE_SANITIZERS`) apply to every target, third-party
  included, so instrumentation sees all memory accesses.
- Each public header is compiled alone, twice included, by the
  `sxml_header_check` target (self-containment and include guard).
- WASI test executables run under Node.js (`tools/run-wasi.mjs`, set as
  `CMAKE_CROSSCOMPILING_EMULATOR`), so `ctest --preset wasm` runs the tests.
- `sxml_version_text` returns a NUL-terminated string: an external boundary.

### D15. tree-sitter pin
Recorded 2026-10-16. Runtime `v0.27.1` (commit `10eb5ac1`), copied unmodified
to `third_party/tree-sitter/` (`LICENSE`, `lib/include`, `lib/src`; details
in `third_party/tree-sitter/VERSION`). Built as `sxml_tree_sitter` from the
single unit `lib/src/lib.c` with `TREE_SITTER_FEATURE_WASM` undefined.
`tree-sitter-cli` `0.27.1` is pinned in `tools/package.json`
(`npm ci --prefix tools`); `tools/generate-grammars.sh` regenerates
`grammars/<language>/src/` and `sxml_add_grammar(<language>)`
(`cmake/SxmlGrammar.cmake`) builds it as `sxml_grammar_<language>`. Runtime
and CLI versions move together.

### D16. Unit test harness
Recorded 2026-10-16 (task 0.2). `tests/test_runner.h` and `.c`, no
dependencies. One executable per module, `tests/<module>_test.c`, holding a
static `sxml_test_case` array passed to `sxml_test_run_suite` from `main`;
registered as one ctest test named `<module>` by `sxml_add_test(<module>)` in
`tests/CMakeLists.txt`. Checks: `SXML_TEST_EXPECT` (continue) and
`SXML_TEST_REQUIRE` (return from the case). Case names given on the command
line run only those cases. Tests may include `src/*_internal.h`.

### D17. Lint gates
Recorded 2026-10-16 (task 0.3). `tools/lint.sh` runs the ast-grep rule
self-tests and the `lint` CMake target (`lint-clang-tidy` + `lint-ast-grep`)
in the `debug` build tree. clang-tidy covers `src/`, `tests/` and the header
check units; `.clang-tidy` header filter now also covers `tests/`. ast-grep
(`@ast-grep/cli` `0.45.3`, pinned in `tools/package.json`) rules in
`tools/ast-grep/rules/`, with examples in `tools/ast-grep/tests/`:
banned abbreviations and single-letter names (as a whole name or one
snake_case word), allocation outside `src/allocator.c`, abort/exit/printing in
`src/`, more than three parameters (variadic counts), bare `bool` parameters.
The guideline rule "pointer parameters without a length" is not yet a lint:
it needs the view types from task 0.5 first.

### D18. Continuous integration
Recorded 2026-10-16 (task 0.4). `.github/workflows/ci.yml`: Linux Clang
(`debug`, plus lints) and GCC (`debug-gcc`), macOS Clang (`debug`), Windows
MSVC (`msvc`, build only), WASI (`wasm`, tests under Node.js 22). Lints run
on Linux Clang only.

### D19. View types in their own header
Recorded 2026-10-16 (task 0.5). `sxml_byte_view` and `sxml_memory_view` live
in `include/sxml/view.h`, with `SXML_BYTE_VIEW_INITIALIZER` (brace form, for
static tables) and `SXML_BYTE_VIEW_FROM_LITERAL` (compound literal).
`status.h` needs byte views and `memory.h` needs `sxml_status`, so the views
cannot live in either without a cycle. `memory.h` includes `view.h`, so
"memory.h provides the views" still holds. The view functions
(`sxml_byte_view_equals`, `sxml_memory_view_slice`, ...) are in `memory.h`.
The header `sxml/string.h` shares its file name with `<string.h>`; it is
only ever included as `"sxml/string.h"` and `include/sxml/` is never put on
an include path itself.

### D20. Outputs on failure are left untouched
Recorded 2026-10-16 (task 0.5). Project-wide default, chosen from the two
options in `docs/C_GUIDELINES.md` ("Ownership and lifetimes"): a function
that fails does not write its outputs. A declaration that deviates says so
(the only one so far: `sxml_utf8_validate` fills its result on both
outcomes, to report where the input stops being valid). Lifecycle functions
check the configuration and memory before writing anything.

### D21. Error records and diagnostics hold borrowed views
Recorded 2026-10-16 (task 0.5). `sxml_diagnostic` (description +
`sxml_source_location`: source name, line, column in codepoints, byte offset)
receives details of any failure. `sxml_error_record` adds the error code
(`sxml_error_code`: namespace URI, local name, display prefix) and the error
value (`const sxml_sequence *`, forward-declared in `status.h`; `xdm.h` in
task 0.7 must define `struct sxml_sequence`). Together they carry what an
XQuery catch clause binds: `$err:code`, `$err:description`, `$err:value`,
`$err:module`, `$err:line-number`, `$err:column-number`.
- All text is borrowed views: static text for errors sxml raises itself (so
  the common case allocates nothing), or memory owned by the object that
  raised the error (fn:error with a computed description or value). They are
  valid until that object is next used or finalized. No fixed-capacity
  description buffer: it would put a frozen size into the ABI and truncate.
- `SXML_STATUS_STATIC_ERROR`, `_DYNAMIC_ERROR`, `_TYPE_ERROR` mean "details
  in the error record" (`sxml_status_has_error_record`); every other status
  is a condition of the C interface.

### D22. The allocator interface has no reallocate
Recorded 2026-10-16 (task 0.5). `sxml_allocator_operations` has `allocate`
and `release` only, as in `docs/C_GUIDELINES.md`. A reallocate would need the
old size and alignment in its request, every host allocator (JavaScript,
Swift, Kotlin) would have to implement it, and the main grower, the arena,
grows by adding blocks. Growable arrays allocate anew and copy, geometrically,
which is amortized O(1). Interfaces are not extended in place, so if
profiling shows a need, a new interface type is published. `allocate` rejects
`byte_count` 0 (`SXML_STATUS_INVALID_ARGUMENT`), avoiding `malloc(0)`'s
implementation-defined result. The wrappers check every request, so
implementations may trust their arguments, and `sxml_allocator_allocate`
also checks the alignment of what an implementation returns. An allocator
with `operations == NULL` is the null allocator: "this object must not grow".
The default allocator uses `malloc` up to `alignof(max_align_t)` and
`aligned_alloc` above it (size rounded up to the alignment for C libraries
that still require it); on Windows it uses `_aligned_malloc` for everything,
since `_aligned_free` cannot release `malloc` memory.

### D23. The counting allocator is public
Recorded 2026-10-16 (task 0.6). The allocator test double,
`sxml_counting_allocator`, is in `memory.h` and the library, not in
`tests/`, so embedders can drive the failure paths of their own code. It
wraps a backing allocator, counts attempts, failures, releases, live
allocations and bytes, and fails exactly the allocation numbered
`failing_allocation_number` (from 1; 0 = none). Test pattern for every module
(see `test_arena_survives_every_allocation_failure` in
`tests/memory_test.c`): run the operation with failing number 1, 2, 3, ...
until it succeeds; after each run `live_allocation_count` must be 0.

### D24. The arena is part of the memory substrate
Recorded 2026-10-16 (task 0.5). `sxml_arena` in `memory.h`: bump allocation
from caller `initial_memory`, then from blocks of `block_byte_count` taken
from `block_allocator`; a request too large for a block gets a block of its
own and the current block keeps serving small requests. The null allocator
as block allocator gives a fixed-capacity arena
(`SXML_STATUS_CAPACITY_EXCEEDED`); `maximum_block_byte_count` bounds growth
for untrusted input (`SXML_STATUS_LIMIT_EXCEEDED`, 0 = no bound). Memory goes
back only on `sxml_arena_reset`, `_finalize` or `_destroy`;
`sxml_arena_as_allocator` exposes it as an `sxml_allocator` whose release does
nothing. Typed, index-addressed storage for trees and XDM values (task 0.7)
builds on it rather than replacing it.

### D25. Strings validate eagerly
Recorded 2026-10-16 (task 0.5). D6 holds: `sxml_string` is a UTF-8 byte view
with an `is_ascii` flag and a codepoint count. Both are computed in the same
pass that validates the bytes (`sxml_string_from_utf8`), so "cached on first
use" is "computed at construction", with no extra pass and no mutable cache
in a value type. `is_ascii == false` makes no claim, so a zero-initialized
`sxml_string` is a valid empty string. Validation is strict (Unicode 16.0
§3.9 table 3-7: no overlongs, surrogates or values above U+10FFFF).
Comparison is by codepoint, implemented as byte comparison, which gives the
same order for well-formed UTF-8. A constructor for producers that already
know the count (the XML parser) can be added later as a new function.
`string.h` is designed but not implemented in Phase 0.

### D26. Lint for byte pointers without a length
Recorded 2026-10-16 (task 0.6), completing D17. The ast-grep rule
`pointer-parameter-without-length` flags parameters in `include/` and `src/`
that are a single pointer to `char`, `signed char`, `unsigned char`, `uint8_t`
or `int8_t`. Object pointers, `void *` state or memory, and pointers to
pointers are allowed. An external boundary that takes a NUL-terminated string
(so far only `sxml_byte_view_from_c_string`) marks the line before with
`// ast-grep-ignore: pointer-parameter-without-length`. `tests/` is not
covered: the test runner takes `argv`. `SXML_TEST_REQUIRE` now branches on the
condition itself, so clang-analyzer knows what holds after it.

### D27. string.h implemented in Phase 0
Recorded 2026-10-16, amending the last sentence of D25: `src/string.c`
implements `include/sxml/string.h` unchanged, with 29 cases in
`tests/string_test.c` (including an exhaustive check of all scalar values and
of every lead/second byte pair against table 3-7). Choices the header leaves
open: on bytes that are not well-formed, decoding stays within the view,
decodes a byte that cannot start an encoding as itself, and decodes a
truncated encoding from the bytes present; `sxml_string_codepoint_at` returns
`SXML_STATUS_INDEX_OUT_OF_RANGE` when the bytes run out before the index, and
takes the ASCII fast path only when the index is also below `byte_count`.
`compare` and `equals` work on bytes (D25). Validation is a plain byte loop;
a word-at-a-time ASCII scan can come later without an interface change.

### D27. Query grammars: XPath, XQuery and XSLT patterns
Recorded 2026-10-16. Grammars `grammars/xpath`, `grammars/xquery` and
`grammars/xslt_pattern`, written from the QT4 CG drafts (XPath/XQuery 4.0
Appendix A, XSLT 4.0 6.3 and Appendix E) as of 2026-10-16.
- Inheritance: `xquery` and `xslt_pattern` are `grammar(require('../xpath/grammar'), ...)`.
  The start rule of a derived grammar is moved to the front of its rule
  table by hand (tree-sitter takes the first rule). Keyword lists and name
  regular expressions are in `grammars/xpath/lexical.js`.
- Node names are the EBNF production names in snake_case with abbreviations
  spelled out (`ExprSingle` is hidden, `VarRef` is `variable_reference`,
  `CompElemConstructor` is `computed_element_constructor`). Operands, steps,
  axes, clauses and bindings are named fields. Binary operators are
  left-recursive nodes with `left`, `operator`, `right`.
- Keywords are not reserved (A.3): every keyword is a tree-sitter reserved
  word (always lexed as the keyword), and every name position accepts each
  keyword aliased to `ncname`, except function names (A.4 reserved function
  names) and XQuery computed-constructor names (xgc: unreserved-name).
- One external scanner, `grammars/common/query_scanner.h`, included by each
  `src/scanner.c`, stateless: nested comments, string template fixed parts,
  string constructor characters, all of direct constructor syntax (tags,
  attribute values, content, comments, PIs, CDATA), and pragmas. Every
  grammar declares the same external token list.
- Checked later, in the tree builder (XPST0003 unless noted): terminal
  delimitation (A.3.2, two non-delimiting terminals with no separator, e.g.
  `10div 3`); a prefixed name as a lookup key (accepted by the grammar so that
  `a:b` stays one token); XQuery prolog declaration order (one repeat in the
  grammar); end tag name equal to start tag name (XQST0118); attribute and
  QName well-formedness that the scanner does not check.
- `tools/qt4-parse-check.py` (with `tools/qt4-parse-harness.c`) parses every
  applicable QT4 query and reports false rejects and false accepts; the
  harness also applies the A.3.2 delimitation check.

### D28. XML 1.0 grammar
Recorded 2026-10-16. `grammars/xml` is written for sxml, not vendored:
tree-sitter-grammars/tree-sitter-xml (MIT) was studied but is too lenient and
its tag-stack scanner is unsafe for deep or non-ASCII documents (reasons in
`grammars/xml/README.md`; credit in `grammars/xml/NOTICE`).
- Node names spell out the XML 1.0 production names in snake_case
  (`STag` is `start_tag`, `CharData` is `character_data`); names, values and
  identifiers are fields. Content items are direct children of `element`.
- External scanner (`src/scanner.c`): element names on a tag stack, so a
  mismatched end tag is an `ERROR` (WFC Element Type Match); character data,
  CDATA text, comment text and PI content with their delimiters and [2] Char;
  PI targets other than `xml`. The stack is a persistent, interned table of
  (parent frame, name) pairs owned by the scanner; the serialized state is one
  4-byte frame index, so depth and name length are unlimited. The scanner
  allocates through `ts_malloc`/`ts_calloc`/`ts_realloc` (D12 exemption).
- Only the document and its internal subset are parsed; parameter-entity
  references inside internal-subset declarations are a grammar error
  (WFC PEs in Internal Subset). The external subset and text declarations
  have no grammar.
- Every well-formedness check the grammar cannot make is listed in
  `grammars/xml/README.md` ("What the tree builder checks"). Entity
  replacement text is checked by parsing it wrapped in a dummy element.
- `grammars/CMakeLists.txt` registers every `grammars/<language>/src/parser.c`
  with `sxml_add_grammar`; a new grammar needs no CMake edit.
- Measured: W3C XML suite (standalone, XML 1.0 Fifth Edition) valid 601/601
  and invalid 175/175 parse cleanly, not-wf 870/948 rejected by the grammar
  (the other 78 are builder constraints); all 3583 well-formed UTF-8 `*.xml`
  files of the QT4 and XSLT 4.0 suites parse cleanly.
