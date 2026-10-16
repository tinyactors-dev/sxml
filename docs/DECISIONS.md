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

### D3. Phase 0 time box
Recorded 2026-10-16 at the Phase 0 gate review. The original file skipped
D3 (no commit ever used the number); the plan's risk table names a
"Time-boxed Phase 0" without a length, so this record fills it. Phase 0 is
time-boxed to two weeks from its start (2026-10-16 to 2026-10-30). When the
box ends, open design questions are written down as decision records or
"Proposed" items and design stops; they are not a reason to extend Phase 0.
Phase 0 closed on its first day, well inside the box.

### D4. License: MIT
Confirmed at the Phase 0 gate (2026-10-16). Vendored code is compatible:
tree-sitter (MIT, D12, D15), the W3C test suites (test data only, D13),
mpdecimal (BSD-2-Clause, D8, D42).

### D5. Build on an existing engine?
Start from scratch in C. Study Xee (Rust, MIT, https://github.com/Paligo/xee)
closely: it implements most of XPath 3.1 with one IR and bytecode interpreter
shared by XPath and XSLT, the same architecture as this plan.
Confirmed at the Phase 0 gate: the substrate keeps the IR out of the value
model (function bodies are an interface, D38), and parsing uses tree-sitter
(D12) rather than a hand-written parser.

### D6. String representation
UTF-8, with a per-string flag for ASCII-only content so codepoint indexing is
O(1) in the common case. Codepoint count cached on first use.
Confirmed at the Phase 0 gate, refined by D25: count and flag are computed
when the string is validated, not cached later.

### D7. Ordered maps (XDM 4.0)
XDM 4.0 maps keep insertion order. Design after Saxon 13 (Michael Kay,
"Implementing Maps for XPath 4.0", XML Prague 2026):
- a built-once list of key/value pairs plus a key-to-offset index,
  built lazily on first lookup;
- a separate persistent layer for later `map:put` / `map:remove`;
- shared key "shapes" for records;
- entries addressable by position, needed for JNode sibling navigation.
Confirmed at the Phase 0 gate; the design is in D37. The 2026 drafts
dropped extensible record types (`record(a, *)`) and optional field markers
(`a?`); a field whose type allows the empty sequence may be absent from a
map that matches, and coercion fills it, so record cells hold every field.

### D8. Decimal arithmetic
libmpdec (mpdecimal, BSD-2-Clause). Check whether its allocator hooks are
process-global; if so, wrap it so the no-global-state rule holds.
Checked at the Phase 0 gate: they are process-global, so the wrapping is
decided in D42. D36 uses libmpdec for integers beyond `int64_t` too.

### D9. Regular expressions
Own engine for the XSD/XPath regex dialect, designed after Saxon HE's engine
(regexml is a Rust port that shows the approach). Check the license of any
source before porting code.
Confirmed at the Phase 0 gate, unchanged; it is a Phase 2 packet (PLAN.md,
risks) and needs no substrate support beyond `sxml_string`.

### D10. WebAssembly toolchain
WASI SDK producing a standalone module, wrapped by a small hand-written
TypeScript package. The core does no I/O of its own, so Emscripten's
JavaScript system emulation is not needed.
Confirmed at the Phase 0 gate: the `wasm` preset builds with WASI SDK 34
and runs the tests under Node.js (D14, D18). The TypeScript package arrives
with the first public API (Phase 1 onwards).

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
The W3C XML conformance suite (xmlts20130923, 17 MB) is vendored the same way
under `third_party/xmlconf/` for the XML parser (see `grammars/xml/README.md`).
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
- Amended by D41: computed descriptions and values are kept by the item
  heap, which a native function body can reach, rather than by "the object
  that raised the error", which a body does not have.

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

### D29. Query grammars: XPath, XQuery and XSLT patterns
Recorded 2026-10-16. Grammars `grammars/xpath`, `grammars/xquery` and
`grammars/xslt_pattern`, written from the QT4 CG drafts (XPath/XQuery 4.0
Appendix A, XSLT 4.0 section 6.3 and Appendix E; drafts of 13 October 2026).
Where the test suites still use syntax the drafts have dropped (XSLT type
patterns `~T`), the drafts win. (This record was first committed as a second
"D27"; renumbered.)
- Inheritance: `xquery` and `xslt_pattern` are
  `grammar(require('../xpath/grammar'), ...)`. tree-sitter takes the first
  rule as the start rule, so a derived grammar moves its start rule (`module`,
  `pattern`) to the front of the rule table. Keyword lists and name regular
  expressions are in `grammars/xpath/lexical.js`.
- Node names are the EBNF production names in snake_case with abbreviations
  spelled out (`VarRef` is `variable_reference`, `CompElemConstructor` is
  `computed_element_constructor`); choice productions such as `ExprSingle`
  are hidden. Operands, steps, axes, clauses and bindings are named fields;
  binary operators are left-recursive nodes with `left`, `operator`, `right`.
  XSLT pattern productions ending in "P" become `..._pattern` nodes.
- Keywords are not reserved (A.3): every keyword is a tree-sitter reserved
  word, so the lexer always returns the keyword token, as the context-free
  tokenizer of A.3 would; every name position accepts each keyword aliased to
  `ncname`, except function names (A.4 reserved function names; in patterns
  also `atomic` and `empty-sequence`) and XQuery computed-constructor names
  (xgc: unreserved-name).
- One stateless external scanner, `grammars/common/query_scanner.h`, included
  by each `src/scanner.c`: nested comments, string template fixed parts,
  string constructor characters, all of direct constructor syntax (tags,
  attribute values, content, comments, PIs, CDATA) and pragmas. All grammars
  declare the same external token list. In XQuery the scanner applies the
  "<" rules of A.3 without regard to context: a direct constructor is
  recognized even where the operator "<" is expected, and the parse fails.
- GLR conflicts are declared only where one token of lookahead is not
  enough: `namespace #x {}` against `namespace#1`; `element instance {}`
  against `element instance of T` (also attribute, namespace,
  processing-instruction); and the two prolog groups, where `declare` may
  continue either group or start the query body.
- In patterns, a step at the outermost level has no JNodeType test, so
  `jnode(` always starts a JNodePattern.
- Checked later, in the tree builder (XPST0003 unless noted): terminal
  delimitation (A.3.2, two non-delimiting terminals with no separator, e.g.
  `10div 3`); a prefixed name as a lookup key (accepted so that `a:b` stays
  one token); end tag name equal to start tag name (XQST0118); duplicate
  attributes, QName and namespace well-formedness; reserved PI target names
  in computed constructors.
- `tools/qt4-parse-check.py` (with `tools/qt4-parse-harness.c`, which also
  applies the A.3.2 delimitation check) parses every applicable query.
  Results on 2026-10-16: XP40 0 false rejects of 32,118 valid queries, 2
  false accepts of 363 XPST0003 queries; XQ40 0 of 43,663 and 3 of 772;
  XSLT 4.0 patterns 3 false rejects of 19,168 (invalid DocBook patterns).
  Details in `grammars/xquery/README.md`.

### D30. Substrate headers and their order
Recorded 2026-10-16 (task 0.7). The substrate is five headers, each including
only those before it: `name.h` (name pool) < `types.h` (type registry,
sequence types, node kinds) < `tree.h` (tree interface, node references,
axes, tree builder, arena tree) < `xdm.h` (item heap, items, sequences,
atomic items, node items, function items, maps, records, arrays, JNodes).
`tree.h` forward-declares `sxml_item_heap`; `status.h` already declares
`sxml_sequence`, which `xdm.h` defines. Accessors that produce XDM values
from nodes (string value, typed value, base URI) are in `xdm.h`; structural
accessors in `tree.h`. There is no "processor" object in the substrate: the
name pool, type registry and item heap are separate objects with stored
references (heap -> registry -> pool), and the Phase 1 processor owns them.
Document and collection resolution (`fn:doc`, `fn:collection`,
`fn:unparsed-text`, module import) is a host interface of the dynamic
context, designed with it in Phase 1, not a substrate header: what it
returns (a tree made through `sxml_tree_builder`, or bytes) is already
covered here.

### D31. Name pool
Recorded 2026-10-16 (task 0.7). Namespace URIs, prefixes and expanded names
are interned into dense 32-bit identifiers in a per-processor
`sxml_name_pool`; comparing names is comparing integers, and nodes and QName
items hold no text. URI and prefix identifier 0 mean "none"; name identifier
0 means "no name". A fixed set of well-known namespaces (`xml`, `xs`, `fn`,
`err`, ...) have constant identifiers in every pool. Names are never
removed; `maximum_byte_count` bounds growth on untrusted input, and
`sxml_name_pool_find_name` looks up without interning. Prefixes are kept
beside names, never part of their identity (XDM 4.0, section 5.6).

### D32. Type registry and sequence types
Recorded 2026-10-16 (task 0.7). `sxml_type_identifier` (32-bit index) names
every type: schema types and item types share one space, since atomic types
are both (XDM 4.0, section 4.1.2). Built-in types (all XSD/XDM built-ins
including `xs:untyped`, `xs:numeric`, `xs:error`, the list types, and the
generic item types `item()`, `node()`, `element()`, `function(*)`,
`map(*)`, `array(*)`, `record(*)`, ...) have constant identifiers
`SXML_TYPE_*` below 70, described by static const data; the enum only grows.
Identifiers 70-511 are reserved for later built-ins; registered types start
at 512. Item types written in queries (node tests, function, map, array,
record, enumeration, choice and JNode types) are registered and interned,
so structural equality is identifier equality; nominative record types are
declared, then defined, so they can be recursive. Schema types (Phase 5)
are registered into the same table. A sequence type is not registered: it
is the plain value `sxml_sequence_type` (item type + occurrence), with
occurrences as bit sets so occurrence subtyping is a subset test.
Predicates (`is_derived_from`, `is_subtype`) return `bool` and treat unknown
identifiers as false.

### D33. XDM values are immutable and reference counted
Recorded 2026-10-16 (task 0.7). Considered: an arena per evaluation (simple,
but garbage grows without bound in long loops and folds, and values that
escape the evaluation, such as results, global variables, XSLT keys and
cached documents, need another mechanism), tracing garbage collection (needs
root scanning of C stacks), and reference counting. Chosen: immutable values
with reference counts on heap cells, small values inline. XDM values cannot
form cycles (built bottom-up, never changed; closures capture values), so
counting reclaims everything. Rules: outputs of make/put/append/... are
owned (+1) and released exactly once; get/describe outputs are borrowed;
inputs are borrowed and functions that store an input retain it themselves.
Retain and release are no-ops on inline values and on immortal cells
(static data, counts that would overflow). Cells live in an `sxml_item_heap`
and record their heap, so `sxml_item_release(item)` needs no other
argument. `sxml_item_heap_read_counts` reports live cells, so every test
can check for leaks (with D23's counting allocator for failure paths). Trees
are counted too: node items retain their tree.

### D34. Item and sequence representation
Recorded 2026-10-16 (task 0.7). `sxml_item` and `sxml_sequence` are 16-byte
plain values with private fields; the size is frozen as ABI (static assert).
Inline: booleans, doubles, floats, integers that fit `int64_t` (any integer
type), QName/NOTATION (name identifier + prefix identifier), node references
(tree pointer + node index + namespace number). Cells: strings, decimals,
big integers, dates, durations, binary, function items, maps, arrays, JNodes.
A sequence is the empty sequence (all zero), one inline item (an item is a
singleton sequence, XDM 4.0, section 3.2), or a cell: vector, integer range
(two `int64_t` bounds), concatenation (flattened parts), or lazy (a
`sxml_sequence_generator`, memoized so every reading sees the same items).
One read interface serves all: count, get item by index, iterator. Item
counts and indices in sequences are `uint64_t`, since `1 to 10000000000` is
valid on 32-bit WebAssembly; counts of things held in memory are `size_t`.

### D35. Thread confinement
Recorded 2026-10-16 (task 0.7). Reference counts are not atomic and the name
pool, registry and lazy indexes mutate on read. Rule: an item heap and
everything tied to it (its name pool, type registry, trees and values) are
used by one thread at a time; separate heaps are fully independent. Static
const data and functions on inline values are safe from any thread. Sharing
one parsed document between threads is not supported in year one; if needed
later it becomes a new decision (an immortal, frozen tree with a read-only
name pool is the likely shape).

### D36. Numbers, dates and durations
Recorded 2026-10-16 (task 0.7). `xs:integer` is unbounded: values that fit
`int64_t` are inline; arithmetic that overflows moves to a cell holding a
libmpdec value with exponent 0 (D8), so there is one arbitrary-precision
library, not two. `xs:decimal` is always a libmpdec cell. Date/time values
keep the seven XDM components (local values, optional timezone in minutes)
with `int64_t` year and nanosecond seconds; durations are months plus
seconds plus nanoseconds (XDM 4.0, section 5.7 note). Nine fractional digits
exceed the required three; more digits are truncated by casting. The public
`sxml_atomic_datum` reads and writes every datum; decimals are exchanged as
canonical text, cached in the cell on first read.

### D37. Maps, records, arrays and JNodes
Recorded 2026-10-16 (task 0.7). Maps follow D7: entry order kept, entries
addressable by position (`sxml_map_get_entry`), hash index built on first
lookup, persistent put/remove sharing structure. Keys compare by
`fn:atomic-equal` with a consistent `sxml_atomic_item_hash` (equal numeric
values of different types hash alike). Map construction takes a whole entry
list and a duplicate-key policy (reject with XQDY0137, use first, use last);
combining duplicates with a function is left to `map:build`/`map:merge`.
Records are maps whose cell holds only field values plus the record type,
whose field names and name index live once in the type registry (D7's
shared shapes); `map:put` on a record gives a plain map. Arrays are
persistent vectors of sequences with O(log n) updates. A JNode is a cell
(parent, key, value, child index, JTree identifier); roots take a fresh tree
identifier so JTrees order among XTrees; children are made on demand and
are identical when parent and key are (XDM 4.0, section 8.5).

### D38. Function items
Recorded 2026-10-16 (task 0.7). A function item cell holds name (optional),
signature (a registered function type; arity is its parameter count),
annotations (borrowed from the body's owner), captured values (retained) and
a body. The body is an interface (`sxml_function_body`: state, `call`,
optional `retain`/`release`) shared by native C functions and IR functions:
the evaluator supplies the operations of IR bodies, so the substrate never
sees the IR. Bodies receive arguments already coerced, the item (for
captures), the dynamic context (opaque), the heap and an error record.
Partial application and function coercion are native bodies capturing the
wrapped function. Maps and arrays answer `sxml_function_item_call` as
`map:get`/`array:get`. Function identity is not modelled (withdrawn from
XDM 4.0, section 8.1).

### D39. Trees: interface, node numbering, document order
Recorded 2026-10-16 (task 0.7). `sxml_tree` wraps an implementation (state +
`sxml_tree_operations`) with a tree identifier and a reference count; the
built-in `sxml_arena_tree` is one implementation and hosts can supply
others. Contract for every implementation: node indices are in document
order (preorder, attributes right after their element), so document order
in a tree is an integer comparison, descendants are an index range
(`last_descendant`), and attributes are the indices after their element.
Namespace nodes are not stored: a namespace node is (element index,
namespace number from 1) into the element's in-scope namespaces, which keeps
lexicographic order correct (XDM 4.0, section 6.2). Across trees, order is
by tree identifier, issued in increasing order by the item heap to XTrees and
JTrees alike, so it is stable and consistent (XDM 4.0, section 6.2). An
`sxml_node_reference` is tree pointer + node index + namespace number and
does not retain; node items do. When the count drops to zero the tree's
owner is told (evaluation-built arena trees destroy themselves; host trees
are destroyed by the host). Axes, XPath 4.0's `-or-self` axes included, are
iterated by the library over the interface.

### D40. Tree builder and arena tree
Recorded 2026-10-16 (task 0.7). One event interface, `sxml_tree_builder`
(start/end document and element, namespace binding, attribute, text,
comment, processing instruction, unparsed entity), is fed by the XML parser,
XQuery constructors and XSLT instructions, and implemented by the arena tree
and later by serializers, so results can stream to output. Builders merge
adjacent text, drop empty text, reject duplicate attributes and perform
namespace fixup (XSLT 4.0, section 5.7.3). `sxml_tree_builder_copy_node`
replays a subtree as events (deep copy with copy-namespaces modes). The
arena tree stores nodes in index-addressed arrays and text in one store
(D24), shares in-scope namespace lists between elements that declare
nothing, keeps source locations on request, and enforces node-count, depth
and text-size limits for untrusted input.

### D41. Raised errors are kept by the item heap
Recorded 2026-10-16 (task 0.8, header review). An error record holds
borrowed views (D21), but fn:error and other native function bodies have no
object of their own that outlives the call to keep a computed description
or `$err:value` in. `sxml_item_heap_raise_error` copies the code,
description and source name into the heap and retains the value; the record
points there until the next error raised on that heap,
`sxml_item_heap_clear_raised_error`, or finalization. One raised error per
heap is enough: a catch clause retains `$err:value` (and makes its
description an item) before evaluating anything else, and an error that
propagates is the latest one. The function returns the error status itself
so bodies can `return` it. A lazy sequence that must report its error again
keeps its own copy and raises it again.

### D42. libmpdec allocation through a scoped allocator
Recorded 2026-10-16 (task 0.8), completing D8. Checked in mpdecimal 2.5.1
(`mpalloc.c`, as bundled by CPython 3.12): `mpd_mallocfunc`,
`mpd_callocfunc`, `mpd_reallocfunc` and `mpd_free` are process-global
function pointers without a state argument, `MPD_MINALLOC` and
`mpd_traphandler` are globals too, and the internal resize path
(`mpd_qresize`; about 90 allocation call sites in `mpdecimal.c` alone)
receives no context. Chosen, for the decimal packet of Phase 2:
- Vendor libmpdec (mpdecimal 4.0.x, pinned like D15) under
  `third_party/mpdecimal/`. Patch only `mpalloc.c` and the matching
  declarations: the four hooks become fixed functions that allocate through
  the `sxml_allocator` of the current decimal scope.
- The scope is one thread-local pointer, set by sxml's decimal module on
  entry to every operation that may allocate and restored before it returns
  (so nesting works); it holds the item heap's allocator. Every block
  starts with a header naming its allocator, so free and resize never
  depend on the scope. No state survives between library calls, two heaps
  on two threads never share it, and nothing is visible to the host: the
  no-global-state rule holds in effect. This thread-local is the only one
  in sxml and is documented at its definition.
- sxml never calls `mpd_setminalloc` and runs contexts with no traps, so
  the other globals stay constant and `mpd_traphandler` is never called.
- Rejected: threading an allocator through every allocation site (a large,
  permanent patch against upstream), and a process-wide hook (breaks heap
  independence, D35). Fallback if the patch or the WebAssembly size budget
  fails: an own decimal module (add, subtract, multiply, divide with the
  precision rules of F&O 4.0), behind the same cell format.

### D43. Header review findings (task 0.8)
Recorded 2026-10-16. Changes made by the Phase 0 review agent (D11):
- `types.h`: node tests carry an `sxml_name_test` (any name, expanded name,
  `prefix:*`/`Q{uri}*`, `*:local`), because XPath 4.0 element and attribute
  tests take a NameTestUnion with wildcards (`element(p:*)`); the same
  struct serves path steps.
- `name.h`: `sxml_name_pool_find_namespace_uri`, so a name from untrusted
  input can be looked up without interning its namespace URI.
- `xdm.h`: `sxml_sequence_test_emptiness` replaces a `bool` predicate that
  swallowed generator errors (fn:empty on a lazy sequence must raise them);
  `sxml_function_item_call` does not coerce arguments, the evaluator does
  (coercion needs casting and function coercion); the borrowed-output verbs
  are get, describe, find and iterator next; iterator sizes are ABI.
- `tree.h`: `sxml_duplicate_attribute_policy` in the arena tree
  configuration (XQuery rejects with XQDY0025, XSLT 4.0 section 5.7.1
  replaces); builder errors use XQuery codes and XSLT rewrites them; the
  event order is the caller's contract (XQTY0024 and XTDE0410 are checked
  by the evaluators, well-formedness by the XML parser).
- `status.h` and `xdm.h`: D41.

### D44. The sxml command-line program and its CLI framework
Recorded 2026-10-16. Rules and planned commands in `docs/CLI.md`.
- Layout: `apps/cli_framework/` (static library `sxml_cli_framework`:
  declarative command tables, parser, help, "did you mean", output streams,
  dispatch); `apps/sxml/` (`sxml_program`: table and command functions;
  executable target `sxml_cli`, output name `sxml`). Option
  `SXML_BUILD_APPS` (default ON), added after `tests/`. Application code:
  public `include/sxml/` API only, memory from an `sxml_allocator`.
- I/O boundary: only `apps/sxml/platform_stdio.c` touches stdio; lint
  `input-output-outside-platform-layer` enforces it for `apps/**`, and
  `pointer-parameter-without-length` now covers `apps/` too.
- One intent, one action: no option abbreviations, no aliases beyond
  `help`/`--help`/`version`/`--version`, a repeated single option is a usage
  error, `-h`/`--help` wins over everything before `--`, no implicit stdin,
  no files written without `--output`, no network. Typos get a suggestion
  (optimal string alignment distance), never an action.
- Exit codes 0 success, 1 operation failed, 2 usage error, 3 input/output
  error (incl. failed writes to stdout), 4 internal error.
- Built-ins: `help [COMMAND]` and the program options `--help`,
  `--version`; tables cannot redeclare them (`sxml_cli_program_validate`,
  run at start: a broken table exits 4). The program name is fixed in the
  table, never argv[0], so output is identical natively and under WASI.
- Today's commands: `help`, `version [--format=text|json]`. Nothing else
  is listed until it works.
- Tests in `tests/cli/`, registered from `apps/CMakeLists.txt` so the
  `tests/*_test.c` glob (D45) never sees them: framework unit tests and
  golden end-to-end tests (stdout, stderr, exit code) under every preset,
  MSVC included (CI runs `ctest -R ^cli` there).
- The QT4 driver (packet 1.24) is a separate tool under `tests/qt4/driver/`
  built on this framework, not an `sxml` subcommand: it reads the vendored
  catalogs and is never shipped. Rejected: `sxml test-suite`, which would
  advertise a developer tool to users.

### D45. Phase 1 work packets and conflict-free registration
Recorded 2026-10-16 (Phase 1 planning; D44 is left to the CLI work, which
cites it). Phase 1 is split into packets of one agent session
(`docs/tasks/phase-1.md`) that run in parallel in one working tree. Each
packet names the files it owns; packets that can run together own disjoint
files. To make additions conflict-free, packet 1.0 replaces the explicit
lists with globs: `src/*.c` is the library and every `tests/<name>_test.c`
is the ctest test `<name>`; `tests/qt4/driver`, `tests/fuzz` (with
`SXML_BUILD_FUZZERS`) and `third_party/mpdecimal` join the build when their
`CMakeLists.txt` exists. `src/` stays flat; a module owns the prefix
`src/<module>[_<part>].c` and `src/<module>_internal.h`. The ledgers
(DECISIONS, GLOSSARY, LOG) take one small insertion per change, committed
at once and alone; a D-number is claimed by re-reading the file first.
Rejected: one subdirectory per module (breaks the module/header pairing of
`docs/C_GUIDELINES.md` for little gain) and per-packet branches (the brief
forbids branches).

### D46. The shared IR is internal, tree-walking and ANF-like
Recorded 2026-10-16 (Phase 1 planning; designed in packet 1.5).
- **Internal**, in `src/ir_internal.h`, not a public header. XPath, XQuery
  and XSLT front ends are all in this repository, so nothing outside needs
  to produce IR; a public IR would join the frozen ABI before XQuery and
  XSLT have tested it. Hosts extend sxml through function bodies (D38) and
  the resolver, not through IR. If a later need appears (an external
  front end, a persisted compiled form), a decision publishes a versioned
  copy.
- **Shape**, after Xee's `xee-ir` (D5): expressions whose operands are
  atoms (a constant or a variable slot), with `let` bindings for
  intermediate values; branches, loop bodies and function bodies are nested
  expressions. The focus is not implicit state: map, filter, quantified,
  sort and path-step nodes bind the context item, position and size as
  ordinary variables, so closures capture them like any variable and
  XPDY0002 is a static or dynamic check on an unbound slot. Variables are
  resolved to frame slots during lowering; function definitions carry
  their capture list. Node-construction, `try`/`catch` and template-rule
  nodes are reserved for XQuery and XSLT (XSLT rules lower to IR functions
  plus a rule table, as in Xee).
- **Evaluation** walks the IR (no bytecode: PLAN.md non-goal), with
  evaluation depth bounded so deep recursion returns a limit status.
- **Optimizer seam**: `src/ir_pass_internal.h` declares a pass as an
  IR-to-IR function over a compiled unit, run in a fixed list between
  lowering and evaluation. Phase 1 has only a verifier pass; rewrites
  (constant folding, type-based simplification) come later as new passes,
  each with tests that compare results with and without it.

### D47. Phase 1 public API headers
Recorded 2026-10-16 (Phase 1 planning; designed in packet 1.3). Four new
public headers, outside the substrate: `xml.h` (XML parser into any
`sxml_tree_builder`), `processor.h` (the processor of D30, owning name pool,
type registry and item heap, plus the host resolver interface for `fn:doc`
and the implicit timezone and current dateTime), `xpath.h` (static context
configuration, compile, evaluate) and `serialize.h` (adaptive method only in
Phase 1; XML, JSON and text methods arrive with XQuery in Phase 3). The CLI
and the QT4 driver use only these and the substrate headers. They are not
frozen at the Phase 1 gate; they become stable with the first public release
(Phase 2). The TypeScript package of D10 also waits for Phase 2: Phase 1
runs the slice under Node.js WASI only.

### D48. A regular expression seed in Phase 1
Recorded 2026-10-16. Three slice tests need `fn:tokenize`, but the D9
engine is a Phase 2 packet. Phase 1 has a small backtracking matcher for a
subset of the XPath dialect behind `src/regex_internal.h` (compile, match at
a position, step limit), which the D9 engine later implements unchanged.
Syntax outside the subset raises FORX0002 rather than matching wrongly.

### D49. libmpdec arrives in Phase 1
Recorded 2026-10-16, amending D42 ("for the decimal packet of Phase 2"). The
slice divides and multiplies decimals and builds integer ranges beyond
`int64_t` (D36), so mpdecimal is vendored and wrapped as D42 describes in
Phase 1 (packet 1.2). Decimal formatting and the rest of the decimal
function library stay in Phase 2.

### D50. A status for failed input and output
Recorded 2026-10-16 (was P1, from D44; accepted by the orchestrator, added
in packet 1.0). `SXML_STATUS_INPUT_OUTPUT_FAILED` in `include/sxml/status.h`
means the platform refused to read or write bytes for a host interface (a
closed pipe, a full disk, a missing file) while the sxml object and its
arguments were fine. Host callbacks that read or write bytes return it: the
CLI's output streams and platform layer now, the serializer's output and
the document resolver of D30 later. It carries no error record; a host that
maps it to a specification error (for example `fn:doc` and FODC0002) does
so itself. Rejected: reusing `SXML_STATUS_INVALID_ARGUMENT`, which blames
the caller for a platform failure.

### D51. The Phase 1 public API (packet 1.3)
Recorded 2026-10-16, completing D47. Headers: `xml.h` (needs `tree.h`) <
`processor.h` (`xdm.h`, `xml.h`) < `xpath.h`; `serialize.h` needs only
`xdm.h` (it serializes from an item heap).
- **XML parser**: a reusable object (allocator, name pool,
  `sxml_xml_limits`: depth, entity expansion bytes) that parses a whole
  document in memory into any `sxml_tree_builder`, or only checks it
  (`sxml check`). Failures are interface statuses (`MALFORMED_INPUT`,
  `INVALID_UTF8`, `UNSUPPORTED_FEATURE` for other encodings,
  `LIMIT_EXCEEDED`) with a diagnostic naming the failed production or WFC:
  well-formedness errors have no QName codes; fn:doc and fn:parse-xml turn
  them into FODC0002 and FODC0006. Nothing outside the input is read.
- **Processor**: pool, registry and heap in its memory, one allocator for
  everything, one limits struct, a resource resolver and a clock;
  `sxml_processor_parse_document` gives an owned document node whose arena
  tree destroys itself when unreferenced.
- **Resource resolver**: one `resolve` operation with an
  `sxml_resource_kind` (only `XML_DOCUMENT` now; text, XQuery modules and
  XSLT stylesheet modules later), after Saxon's ResourceResolver, so later
  phases add kinds instead of publishing new interfaces. It answers bytes
  (sxml parses them with the processor's limits), a document node, or
  none. "Not available" (a missing file, a refused URI, a failed read) is
  `SXML_RESOURCE_FORM_NONE`, which fn:doc raises as FODC0002; a failure
  status (D50 included) stops the evaluation. The null resolver makes
  nothing available (the library does no I/O); the default implementation
  and test double is `sxml_table_resource_resolver` over in-memory entries.
- **Clock**: read at most once per evaluation, so fn:current-dateTime is
  stable; `sxml_system_clock` (C17 `timespec_get`, implicit timezone UTC)
  and `sxml_fixed_clock`.
- **Contexts**: `sxml_static_context_configuration` and
  `sxml_dynamic_context_configuration` are plain structs embedded by value
  in requests; zero means the default context. XQuery and XSLT reuse them.
  Names are name pool identifiers and types registry identifiers, as in the
  substrate. External variables are declared with a type and bound by name
  (coerced, XPTY0004; unbound XPDY0002); the context value is a sequence
  (XPath 4.0) with a presence flag; available documents make `fn:doc(uri)`
  the very node bound elsewhere (QT4 sources with a `uri`). A namespace
  binding with prefix 0 is the default namespace for elements and types.
- **Expressions**: `sxml_xpath_expression` follows the lifecycle quintet;
  initialize compiles. The compiled form is allocated from the processor
  and counted, so inline function items in a result outlive the expression
  (D38). Results are complete: nothing lazy that can fail later.
  Evaluations may nest. `sxml_xpath_parse_sequence_type` turns declared
  types given as text (279 QT4 `param ... as=`) into sequence types.
- **Serialization**: `sxml_output_stream` (one `write`, which returns D50's
  status when the platform refuses), the `sxml_output_buffer` as default
  and test double, and `sxml_serialize_sequence` with the adaptive method;
  the method enum and parameters struct grow in Phase 3.
- **Limits**: 0 means no bound, except `maximum_expression_depth` and
  `maximum_evaluation_depth`, where 0 means sxml's default: no bound would
  mean a C stack overflow.
- Left out until needed, each a new field that designated initializers
  keep source-compatible: host extension functions in the static context
  (meanwhile a function item bound to a variable), decimal formats,
  collations, default language, fn:trace output, cancellation, a seed for
  fn:random-number-generator.

### D52. The abstract syntax tree (packet 1.4)
Recorded 2026-10-16. `src/xpath_ast_internal.h`, internal. One node kind
per visible production of `grammars/xpath` (D29), except `comment`, and
`axis` and `occurrence_indicator`, which only hold a token and become their
parent's detail; anonymous field tokens (operator, quantifier,
abbreviation) become detail or flags. Children are linked in source order
and tagged with their grammar field, so lists are "children with field F"
and code finds children by field, never by position: XQuery and pattern
grammars, which add node types and add fields to four XPath types, only
append to the kind and field enumerations. Nodes and decoded text live in
two fixed-capacity arrays (D24) sized from the concrete syntax tree (its
node count bounds the AST nodes, the source byte count bounds decoded
text), so building never grows; index 0 means "no node". Names are kept as
written (prefix, local name, braced URI); string literals and template
parts are unescaped and numeric literals lose `_` and `0x`/`0b`, while
lowering makes the atomic items. Each node has a source span (byte offset
and count, line, column in codepoints). Front-end entry points
`sxml_xpath_build_ast` and `sxml_xpath_build_sequence_type_ast` are
declared there and defined by packet 1.16 in `src/xpath_parse.c`; the
lifecycle and builder functions go in `src/xpath_ast.c`, also written by
1.16 (the AST module's only producer in Phase 1). Identifiers use the
module prefix `sxml_ast_`, since XQuery and patterns share the tree.

### D53. Regular expressions: hand-written parser, compiled regex and matcher (packet 1.17)
Recorded 2026-10-16. The regex text is parsed by a hand-written
recursive-descent parser in `src/regex.c`, not by a tree-sitter grammar:
an exception to D12, for these reasons. (1) A regex is run-time data,
compiled during evaluation (`fn:tokenize($s, $p)` with a computed `$p`),
and tree-sitter allocates with `malloc` outside every sxml allocator and
limit (D12 accepts that only for the parse step of source text). (2) The
flags change the lexing before any grammar applies (F&O 4.0, section 6.2:
`x` removes whitespace except inside `[...]`, `c` removes `#...#`
comments, `q` makes every character literal), so a grammar would need a
preprocessing pass that is most of a parser. (3) Tokens are single
characters whose meaning depends on context (inside or outside `[...]`,
after `\`, the hyphen rules of XSD 1.1), and back-references need the
groups closed so far; a context-free grammar plus a tree walk adds a
layer without removing any check. (4) FORX0002 needs a precise reason,
and tree-sitter `ERROR` nodes give none. (5) Saxon HE and regexml, the
models of D9, parse by hand too. The XSD dialect (Phase 5) is a flag of
the same parser.
Interface (`src/regex_internal.h`), which the D9 engine keeps:
`sxml_regex_parse_flags` (FORX0001); `sxml_regex`, compiled from
`regex_text` and flags, immutable, shareable between threads, with the
lifecycle of D23 (memory requirements found by a counting pass of the
parser, so they are exact and need no scratch memory); `sxml_regex_matcher`,
the per-thread working state (backtrack stack through an allocator,
captured groups, step count), with `match_at` (anchored), `find` (first
matching segment at or after an offset) and `find_next` (the next of the
disjoint matching segments, F&O 4.0, section 6.1.1), each writing an
`sxml_regex_search_result` (found or not, and the segment: the bool
outputs the lint forbids become a struct), and captured groups as byte
ranges of the regex input. Positions are byte offsets at
codepoint boundaries. A regex error is SXML_STATUS_DYNAMIC_ERROR with
err:FORX0002 in the error record the configuration points to; running out
of steps or stack is SXML_STATUS_LIMIT_EXCEEDED, not a spec error.
The seed (D48) supports: literals, `.`, classes with ranges, negation and
subtraction, `\s \S \d \D \w \W` (Unicode 16.0 tables generated by
`tools/generate-regex-tables.py`), greedy and reluctant quantifiers,
capturing and non-capturing groups, alternation, `^ $`, back-references,
and flags `s m i x q c`. FORX0002 with "not supported by the regex seed":
`\p \P \i \I \c \C \b \B`, lookahead and lookbehind, named groups
and `\k`; with `i`, non-ASCII characters in the regex and back-references
(case variants beyond ASCII need Unicode case data). A public step limit in
`sxml_processor_limits` is left for the D9 packet (a new field).

**No recursion (D71)**, amended 2026-10-16: the parser reads pieces in
one loop and keeps open groups on an explicit stack of group frames,
inline for `SXML_REGEX_INLINE_GROUP_DEPTH` (16) groups, then grown
through the new `stack_allocator` of `sxml_regex_configuration` and
released before the call returns (`sxml_regex_create` uses its own
allocator; without one, deeper nesting is CAPACITY_EXCEEDED);
`maximum_group_depth` (default 100) now bounds that heap. Every atom is
parsed and emitted once: a "|" inserts the split of the branch before it,
a quantifier inserts what precedes the first copy and appends copies of
the atom's instructions (targets moved, progress registers renumbered,
classes shared), so the lexical skipping and re-parsing are gone. Class
subtraction is a chain, read and matched in loops. Arrays are sized by
the most a pass wrote, which fixes a write past the arrays when an atom
quantified with {0} came last (`(ab){0}` at the end). 400,000 random
regexes compile, fail and match exactly as before.

### D54. Name pool internals (packet 1.1)
Recorded 2026-10-16. Three tables (URIs, prefixes, names), each an entry
array in issue order plus an open-addressing index (linear probing, at most
3/4 full, slots keep the hash so growth never reads entries); FNV-1a, not
seeded: hash flooding is bounded by `maximum_byte_count`. Interned text
lives in an arena placed in the pool's own memory, in 1 KiB blocks.
`maximum_byte_count` counts every allocation the pool holds (tables and
whole text blocks, so a pool may stop up to one block early); a replaced
table gives its bytes back. A failed intern leaves the pool's names as
they were. Interned URI identifiers start at
`SXML_NAMESPACE_WELL_KNOWN_COUNT`; prefix and name identifiers at 1.
`src/name_internal.h` gives `sxml_name_pool_name_limit` (and URI, prefix)
so tables indexed by identifier can be sized.

### D55. IR, passes, evaluator and function table (packet 1.5)
Recorded 2026-10-16, completing D46. Four internal headers.
- `src/ir_internal.h`: a unit holds every array (expressions, atoms,
  functions, constants, globals; reserved: catch clauses, name tests,
  namespace bindings, parameter bindings, modes, template rules), index 0
  meaning none; it grows while built, is sealed before evaluation and is
  reference counted, so function items made from IR functions keep it
  alive (D38, D51). Slots number from 1: parameters, then captured
  variables, then locals. 25 expression kinds for XPath (MAP, FILTER,
  SORT, QUANTIFIED bind the focus to slots; STEP yields axis order and
  DOCUMENT_ORDER sorts; COERCE and RAISE are explicit) plus reserved
  TRY_CATCH, seven constructors, SHALLOW_COPY and APPLY_TEMPLATES. The
  header writes out the IR of `let $f := function($x){$x + .} return
  $f(4)`, `/a/b[2]`, an XQuery FLWOR with order by (tuples as arrays, no
  new kind) and an XSLT apply-templates (named extension point: modes
  and template rules, match functions from patterns). group by and
  windows use internal native rows. Globals read by an inline function
  are captured, so escaped closures need no global table of their own.
- `src/ir_pass_internal.h`: `sxml_ir_pass` (name, function over a
  request), `sxml_ir_standard_passes` = {verifier}, `sxml_ir_run_passes`
  optionally verifying after each pass; passes only add and replace.
- `src/evaluator_internal.h`: `struct sxml_dynamic_context` (heap,
  frames, clock read once, document loader, function table for
  fn:function-lookup, depth limit, globals); `sxml_evaluator_evaluate`;
  coercion, dynamic calls with coercion and the effective boolean value
  for native bodies; the function body operations of IR functions. fn:doc
  and fn:parse-xml go through an internal `sxml_document_loader` that the
  processor (1.23) implements, so 1.20 and 1.22 test with doubles.
- `src/function_library_internal.h`: native rows give parameter and
  result types as F&O-spelled text ("fn(item(), xs:integer) as
  xs:boolean?"), resolved once per library through the front end and
  static analysis; defaults are required, the context value (lowering
  supplies it) or supplied by the body; properties flag dynamic-context
  use, nondeterminism, higher-order calls and position/last. One
  `extern const sxml_native_function_view` per part file; the
  configuration names the parts, so no file lists another's functions.
  Static analysis finds functions through the `sxml_function_table`
  interface (tests bring their own).

### D56. `memcpy` and `memset` are allowed
Recorded 2026-10-16 by the orchestrator. clang-tidy's
`clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling` rejects
`memcpy`, `memset` and `snprintf` in favour of the C11 Annex K `_s` functions,
which glibc, macOS and WASI do not provide. Packets wrote byte loops instead.
The check is switched off in `.clang-tidy`; `memcpy`, `memmove` and `memset`
are allowed when the byte count comes from checked size arithmetic. Printing
functions stay banned outside the CLI platform layer (D17, D44). Existing byte
loops may stay; new code uses `memcpy`.

### D57. Decimal arithmetic over libmpdec (packet 1.2)
Recorded 2026-10-16, completing D42 and D49.
- **Vendoring**: mpdecimal 4.0.1 (`third_party/mpdecimal.version`), libmpdec
  only; patches marked "sxml patch" in `mpalloc.c`, `mpdecimal.h` and
  `context.c`. The configuration is chosen in `mpdecimal.h` from the width
  of `size_t` (CONFIG_64 or CONFIG_32 with the portable ANSI arithmetic,
  `__uint128_t` under GCC and Clang), so one header serves every target,
  32-bit WebAssembly included; no MASM or inline assembler. WASI has no
  `signal.h`: the never-called trap handler does nothing there.
- **Scope location**: the D42 thread-local lives in the patched `mpalloc.c`,
  not in `src/`, so libmpdec does not depend on the sxml library that links
  it; `src/decimal.c` sets it with `mpd_sxml_exchange_allocator_scope`.
  Shrinking a block keeps it (never fails); growth is allocate, copy,
  release (D22).
- **Value type**: `sxml_decimal` wraps an `mpd_t` with a static struct and
  a dynamic coefficient: movable byte for byte, made by the functions that
  write it, released by `sxml_decimal_finalize` without a scope. No
  create/destroy: cells (1.7) embed it.
- **Limits and precision**: a configuration (allocator,
  `maximum_digit_count` default 10000, `division_digit_count` default 34)
  per item heap. Sums, differences, products, `idiv` and `mod` are exact or
  `SXML_STATUS_LIMIT_EXCEEDED` (FOAR0002); quotients are computed to
  max(34, digits of both operands) significant digits, half to even
  (op-numeric-dividepint2args-5 then gives its second allowed result).
  Division by zero is `SXML_STATUS_INVALID_ARGUMENT` (callers raise
  FOAR0001).
- **Binary floating point**: casting uses the shortest round-trip decimal
  (`0.1e0` gives `0.1`, as XPath processors do); `fn:atomic-equal` and
  numeric comparison use the exact value. Decimal to double/float goes
  through `strtod`/`strtof` on text without a decimal point (locale-free),
  780 digits plus a sticky digit, so rounding is exact.
- **Size**: a stripped `wasm32-wasip1` program using parse, divide,
  shortest conversion, canonical text and `to_double` grows by 89 KB
  (35 KB gzip) over one using only the allocator.

### D58. Type registry internals and the subtype search (packet 1.6)
Recorded 2026-10-16.
- **Storage**: built-in descriptions are static (`src/types_builtin.c`);
  their names are interned at initialization and found through a fixed
  256-slot index inside the registry. Registered types are an entry array
  plus two open-addressing indexes (structure, names), as in D54; copied
  views live in an arena of 4 KiB blocks, released at finalize. A
  registration that fails after copying leaves its copies unused there.
- **Normal form**: registered descriptions are normalized before interning
  (name test fields its kind does not use cleared; `allows_nilled` only on
  `element(N, T)`; `map(*)`, `array(*)`, `function(*)` and `jnode(*)` carry
  their explicit equivalents, as the built-ins do), so `map(*)` with any
  key gives `SXML_TYPE_MAP` and a node test of kind any gives xnode().
  node() and xnode() share a description; node() is told apart by
  identifier. A choice of one member is that member.
- **Rejected on registration** (INVALID_ARGUMENT): duplicate enumeration
  values and duplicate field names (the front end removes duplicate enum
  values first), empty choices and enumerations, map keys that are not
  atomic, union or enumeration types, `document-node()` around anything
  but element tests, JNode key constants outside the six key types.
- **Subtyping** follows XPath 4.0 §3.3.2 rule by rule; transitivity is
  built into the rules where it adds pairs (maps, arrays, records as
  functions; records as maps; nominative records as their structural
  twin). A choice as derived type is split first (every member), then a
  choice as base (some member, else the abstract expansion of
  xs:anyAtomicType, xnode(), node(), item()); `document-node(element(a|b))`
  is split per element. Two places are slightly more permissive than the
  letter of §3.3.2.2 (a derived member is compared with the whole base
  choice, so a union member may spread over several base members); both are
  sound. Without schemas (Phase 5) schema-element/-attribute tests are
  subtypes only of themselves, `element()`/`attribute()` and same-name
  `attribute(N)`. JNode key constants compare by type and lexical form.
- **Budget**: one question may nest 128 deep and apply 10⁶ rules. Every
  rule is monotone, so running out answers false, never a wrong true; this
  bounds work on hostile types.

### D59. The XPath front end (packet 1.16)
Recorded 2026-10-16. `src/xpath_parse.c` builds the AST of D52 in one
iterative walk of a tree-sitter cursor in source order; the current AST
parent is kept through the AST's own parent links, so nesting depth costs
no C stack (20,000 nested parentheses pass under the `wasm` preset). Node
types and field names map to kinds and fields through sorted static
tables; the AST capacity is `ts_node_descendant_count` of the top node.
- Checks of D29 in the walk: A.3.2 delimitation over the terminals in
  order (non-delimiting: keywords, `ncname`, `prefixed_name`,
  `uri_qualified_name` and the numeric literals; a comment separates) and
  a `prefixed_name` in the `key` field of `lookup` or `unary_lookup`.
  Every failure, and the first ERROR or MISSING node, is XPST0003 with a
  static description, the source name, line, codepoint column and byte
  offset. With these, XP40 has no false rejects or false accepts left
  (32,542 valid queries build; Lookup-156 and MapConstructor-025a, the
  two D29 false accepts, now fail).
- A sequence type is parsed as the type of `() instance of T`: the
  prefix is served before the text through a `TSInput` callback (no
  copy), the result must be that instance-of expression with the type
  starting in the text, and spans are relative to the text.
- tree-sitter aliases each token of `alias(seq('(', ')'), jroot_selector)`
  separately; the front end makes one JROOT_SELECTOR spanning both.
- `maximum_expression_depth` counts AST nodes from the root (depth 1).
- The only memory taken from the request's allocator is the AST, one
  allocation; tree-sitter's parser, tree and cursors use malloc (D12).
- AST builder tests are in `tests/xpath_ast_test.c` (module prefix
  `xpath`, D45), front-end tests in `tests/xpath_parse_test.c`.

### D60. Item heap, cells and atomic items (packet 1.7)
Recorded 2026-10-16. `src/xdm_internal.h` is the shared interface of the
value model; later XDM packets add files, not edits.
- **Layout**: `sxml_item.representation` is an internal enum (inline
  boolean, integer, float, double, QName; atomic, JNode, function, map,
  array cells; from 64 the sequence cells vector, integer range,
  concatenation, lazy); 0 is "no item" and, as sequence content, the empty
  sequence. Atomic items keep their primitive type in `flags`, so type
  families need no registry; `type_or_node_index` holds the annotation
  (node index for XNodes).
- **Cells**: header (cell class, heap, byte count, reference count) at the
  start of a module's struct. A `sxml_cell_class` (static const) gives
  `release_contents` and, for sequence cells, count/get item/iterator
  next, so `src/xdm.c` dispatches to range, concatenation and lazy cells
  (1.8) without link-time dependencies. Releasing the last reference
  queues the cell on the heap's pending list; the outermost release frees
  the list in a loop, so deep values free in constant C stack (a 10^6
  chain is tested). Count overflow makes a cell immortal.
- **Memory**: every byte the heap hands out counts in `live_byte_count`
  and against `maximum_byte_count`: cells, side tables
  (`sxml_item_heap_allocate_bytes`), the raised error, and decimal
  coefficients, which go through `sxml_item_heap_as_allocator` (a hidden
  size header per block) as the heap's decimal configuration.
- **Decimals**: a decimal cell stores its canonical text (made with the
  cell, not on first read), the nearest double and whether it equals it
  exactly. atomic-equal, the hash and compare therefore never allocate:
  doubles are compared through their exact decimal text, built on the
  stack. Numbers equal to some double hash as that double, others as
  their canonical text, so 1, 1.0, 1.0e0 and xs:float 1 hash alike.
- **Compare** follows F&O 4.0 `fn:compare` with PR2256 (QT4 fn-compare):
  durations by months then seconds, binary by octets across hex and
  base64, QNames by namespace URI then local name. QName order needs the
  name pool, which `sxml_atomic_comparison` lacks: internal
  `sxml_atomic_item_compare_in_heap` takes the heap; the public function
  returns `SXML_STATUS_UNSUPPORTED_FEATURE` for distinct names (P2).
- **Making**: strings are revalidated as UTF-8 and checked against the
  derived types (NCName, token, language, ...); integer subtypes by range,
  beyond `int64_t` only where the type allows (unsignedLong up to
  2^64-1). `sxml_atomic_item_make_taking_decimal` lets operators and
  casting hand over a computed `sxml_decimal`.
- **Node items**: `sxml_item_retain`/`_release` must retain the tree, but
  `src/tree.c` arrives with 1.11. `src/xdm.c` has two empty static
  functions, `sxml_node_item_retain_tree`/`_release_tree`; packet 1.11
  fills them with `sxml_tree_retain`/`sxml_tree_release` (a two-line
  hand-off; no node item exists before 1.13).
- `sxml_error_record_report` fills an error record from a
  `sxml_static_error` (err namespace), for every packet's static errors.

### D61. Maps and records (packet 1.9)
Recorded 2026-10-16. `src/xdm_map.c`; three cell classes behind
`SXML_ITEM_REPRESENTATION_MAP_CELL`, told apart by cell class.
- **Entry block** (`sxml_map_make`): entries in one cell. Maps of more
  than 8 entries get a hash index (key hash, entry index; open addressing,
  load at most 1/2). Duplicate detection needs it, so it is built while
  making and kept, not built on first lookup (a refinement of D37); maps
  of 8 entries or fewer are searched linearly.
- **Persistent map** (`sxml_map_put`, `sxml_map_remove`): an entry tree
  (AVL, ordered by entry stamp, entry counts per subtree) and a key tree
  (AVL, ordered by key hash, then stamp), both of `map_node` cells shared
  by path copying. Put and remove are O(log n), `sxml_map_get_entry` and
  `sxml_map_find` O(log n). A new key gets the next stamp, so it goes last;
  a removed and re-added key goes last too. Put on an existing key keeps
  the old key and its position and replaces the value (which key is kept
  is implementation-dependent, QT4 map-put-011). Put or remove on an entry
  block or record converts it in O(n log n) once; results of 8 entries or
  fewer stay entry blocks (copied).
- **Record**: the cell holds the record type, the field values and
  `field_key`s, made on first need (`sxml_map_get_entry`, `sxml_map_find`)
  in a side table of the cell; lookup by name goes through
  `sxml_type_registry_find_record_field`, shared by every record of the
  type. Only string-family keys (xs:string, xs:anyURI, xs:untypedAtomic)
  can match a field. Put and remove give a plain map (persistent or block).
  Because field keys are made lazily, `sxml_map_get_entry` on a record can
  return the heap's failures on its first call (P3).

### D62. Ranges, concatenations and lazy sequences (packet 1.8)
Recorded 2026-10-16. `src/xdm_sequence.c` adds three sequence cell classes
and `sxml_sequence_test_emptiness`; xdm.h and src/xdm.c are unchanged.
- **Ranges** store the two `int64_t` bounds; an empty range is the empty
  sequence and a one-integer range an inline item, so a range cell always
  has two or more items. Members are computed in `uint64_t`; every index
  below 2^64 is readable. Only the count of `INT64_MIN to INT64_MAX`
  (2^64 items) does not fit: `sxml_sequence_count` raises XPDY0130 (XPath
  4.0, implementation-dependent limit), as for a concatenation whose total
  passes 2^64 - 1.
- **Concatenations** drop empty inputs, flatten nested concatenations and
  return a lone remaining part itself (retained). Part ends (items up to
  and including each part) are a cache filled in order as reads count the
  parts; `get_item` binary-searches the counted parts and asks each further
  part for the item before counting it, so a lazy part is read only as far
  as needed. Iteration uses `part_index`/`next_index_in_part`: O(1) per
  item.
- **Lazy sequences** keep produced items in a doubling array (room is made
  before calling the generator, so an item is never lost and an allocation
  failure is retryable). The generator always gets the cell's own error
  record; a failure is final: its error is copied into the cell (the kept
  error, D41) and raised again through the heap by every later read that
  needs an item past the last produced one; items before it stay
  readable. A non-error status (out of memory in the generator) is kept
  and returned the same way. The generator's state is released only when
  the cell is freed, as xdm.h says. A generator that reads its own lazy
  sequence gets `SXML_STATUS_INVALID_ARGUMENT` instead of reentry.
- **Emptiness** is `get_item(0)`: at most one item of a lazy sequence is
  produced, and its error is reported (D43).

### D63. Static analysis and lowering (packet 1.12)
Recorded 2026-10-16. `src/xpath_static_internal.h` (module `xpath_static`):
`sxml_xpath_static_context` (heap, allocator, namespace bindings, variable
declarations, function table); `sxml_xpath_resolve_name`,
`_resolve_sequence_type` and `_resolve_item_type` (also for the type text
of function library rows, D55); `sxml_xpath_lower` (AST to an unsealed
unit) and `sxml_xpath_compile` (parse, lower, `sxml_ir_standard_passes`,
seal), which 1.23 calls.
- **Names**: the AST's namespace declarations, then the configuration,
  then the predeclared prefixes; unprefixed functions are in fn,
  variables, attributes and QName literals in no namespace, element and
  type names in the default element namespace. Function names are found
  with `sxml_name_pool_find_name`: a name never interned is XPST0017.
  Global variable n is variable declaration n, referenced or not, so the
  processor binds by index.
- **Shape**: ANF blocks append LETs; atoms (literals, variables) are never
  bound. Left-recursive chains (binary operators, paths, filters,
  lookups, dynamic calls, arrows) are walked in a loop; other nesting
  recurses, bounded by `maximum_expression_depth` (default 32: the
  costliest level, an arrow into a call, takes about 1.4 KB of the
  64 KiB WebAssembly stack; LIMIT_EXCEEDED beyond). The processor's
  default for this limit (1.23) should not exceed it. An inline function's
  free variables are found by walking its AST through parent links; each
  becomes a captured slot, except a constant (a `let` of a literal),
  which is substituted. Globals read in a function are captured (D55).
- **Focus**: slots made on first read (main, MAP, FILTER); absent in an
  inline function body (a RAISE of XPDY0002 where read); a focus function
  and `->` fix the value, with position and size 1.
- **Paths**: `E1/E2` is MAP then DOCUMENT_ORDER; `//` adds a MAP of
  `descendant-or-self::node()` and DOCUMENT_ORDER; a reverse-axis step is
  followed by DOCUMENT_ORDER after its predicates; default axis attribute
  or namespace for those tests; `node()` steps use the generic node type.
  A name test on a JNode origin is the evaluator's to read as a key test
  (XPath 4.0, 4.6.5), since STEP carries element(N).
- **Calls**: COERCE unless the type is `item()*` or a constant already
  matches; constructor functions are CAST allowing empty; a left-out
  context-value parameter is the focus value (static calls) or captured
  by a closure (`f#N`); `position()`/`last()` read focus slots; static
  placeholders make a closure (constant arguments in its body, others
  coerced and captured); dynamic placeholders call fn:partial-apply with
  `xs:positiveInteger` keys; keyword arguments are reordered; `and`/`or`
  are CONDITIONALs giving `true()`/`false()`, `otherwise` tests
  `instance of item()+`, string templates use fn:string-join#2 and `||`.
  fn:root, fn:partial-apply and fn:string-join must be in the table.
- **Literals**: integers inline or as decimal cells (hex and binary
  converted); doubles through decimal text, rounded once (D57).
- **Not lowered in Phase 1** (UNSUPPORTED_FEATURE, diagnostic filled):
  for member, for key/value, destructuring let, method calls, but with,
  dynamic node tests, the item axis, named namespace-axis tests (P4),
  jnode() selectors other than `*`, `()`, NCName, string, integer,
  boolean.
- **Text form**: nested expression operands start a line two columns
  past their parent's "(", except a LET's bound expression; constants
  print as XPath literals (doubles as `<shortest decimal>e0`).
- **Checks**: with a permissive function table, 29,653 of the 32,409
  valid XP40 queries lower and pass the verifier; the rest are static
  errors from missing test environments, or the constructs above.
- `sxml_native_function` now has `properties` after `function_namespace`
  (no padding; clang-tidy's padding check failed on the first file to
  include it). Field order only.
- `sxml_function_table_find` (D55) is declared but nobody defines it yet;
  static analysis uses `sxml_xpath_find_function`; 1.20 may define the
  wrapper over it.

### D64. Arrays and function items (packet 1.10)
Recorded 2026-10-16. `src/xdm_array.c`, `src/xdm_function.c`.
- **Arrays** are AVL trees of `array_node` cells (one cell class): a flat
  node holds members inline (`sxml_array_make`, so get is O(1)); a run node
  names a part of a flat node's members (splitting a leaf copies nothing);
  a branch node joins two subtrees. The array item's cell is the root; the
  empty array is a flat node of no members. Every update is split plus
  join (Blelloch et al., "Just Join", without middle keys): append, put,
  insert, remove and slice are O(log n), get O(log n). 10^5 appends are
  tested at most 1 + 3 x height allocations each; deep nesting frees
  through the pending list.
- **Function items**: the signature must be a registered function type
  other than function(*) (arity = its parameter count), else
  INVALID_ARGUMENT. Annotations are kept as the borrowed view; captured
  values are retained; the body's `retain` runs once on make and
  `release` once when the cell is freed.
- **Maps and arrays as functions**: `sxml_function_item_describe` gives a
  stateless static body (map:get through `sxml_map_find`, array:get with
  1-based positions). The bodies check their argument although coercion is
  the caller's: not one atomic key / one integer is XPTY0004; a position
  out of range, also beyond int64_t, is FOAY0001.

### D65. WASI test runner disables V8 fast API calls

- **Problem**: the `xpath_parse` test crashed Node.js (SIGSEGV, exit 139)
  about one run in eight on the `wasm` preset; `deep_nesting_needs_no_recursion`
  alone, after it had passed. Not a memory error in sxml: the same test is
  clean under ASan/UBSan (Clang, GCC) and valgrind, and the crash is not a
  wasm trap or a stack overflow (it happens with `--stack-size=200` too).
- **Cause**: in Node 22 the WASI imports are V8 fast API callbacks. `fd_write`
  allocates through uvwasi, which reports external memory to V8 and can start
  a full garbage collection inside the fast call; the collector then faults
  walking the optimized wasm frame (`InnerPointerToCodeCache::GetCacheEntry`).
  Whether a collection falls there depends on heap timing, hence one run in
  eight; tests that grow linear memory a lot (20,000 nested parentheses)
  make it likely. `--no-allow-allocation-in-fast-api-call` turns it into a
  deterministic V8 abort in `WASI::FdWrite`'s FastCallback (45 of 60 runs).
- **Decision**: `tools/run-wasi.mjs` sets `--no-turbo-fast-api-calls` through
  `v8.setFlagsFromString` before compiling the module, so WASI functions are
  called the ordinary way. 0 failures in 110 runs afterwards, also with
  allocation in fast calls forbidden. The test depth and the wasm stack size
  stay as they are; the library is not involved. Revisit when the runner's
  Node version no longer uses fast calls for WASI.

### D66. Casting and lexical forms (packet 1.15)
Recorded 2026-10-16. `src/cast_internal.h`; `src/cast.c` (casting table,
targets), `src/cast_lexical.c` (lexical forms, canonical text).
- **Interface**: `sxml_cast_make_value` and `sxml_cast_test_castability`
  take the atomized operand (atomizing is the evaluator's) and any target
  type: atomic, list, union, enumeration or choice; a target that cannot
  be cast to gives XPST0080 at run time too. `sxml_cast_make_value_from_text`
  casts text as an xs:string without making one (list tokens use it).
  `sxml_cast_write_canonical_text` borrows the text of strings and
  decimals, writes the rest to the caller's buffer and reports the size
  needed on CAPACITY_EXCEEDED (96 bytes hold every number, date and
  duration).
- **xs:QName**: the request carries the statically known namespaces as an
  `sxml_namespace_binding_view` (prefix 0: default namespace for elements
  and types), predeclared prefixes as fallback; no callback interface.
  XPTY0117 (coercing untypedAtomic to xs:QName) stays with the evaluator.
- **Checks**: syntax here, value space in `sxml_atomic_item_make`, whose
  INVALID_ARGUMENT becomes FORG0001. XSD 1.1 lexical spaces: "+INF"
  accepted, base64 with single spaces, year of 4 to 16 digits ("-0000"
  refused, more is FODT0001), 24:00:00 is the next day, fraction digits
  past nine truncated (D36), duration parts beyond `int64_t` FODT0002;
  digits beyond the heap's limit FOCA0003 (integer targets) or FOCA0001.
- **Numbers**: xs:double and xs:float write the shortest digits (D57) as a
  decimal when the first significant digit's power of ten is -6 to 5,
  else as d.dddEn, so xs:float("1e-6") writes 0.000001. To integer types
  truncates (doubles beyond 2^63 by their exact value); to xs:decimal the
  shortest decimal. Text to float goes through `strtof` (one rounding) on
  locale-free text, as D57.
- **Unions and choices**: an instance of an atomic member is returned
  unchanged, otherwise members in order; when all fail XPTY0004 if every
  member refused the source type, else FORG0001 (xs:error: FORG0001).
  **Enumerations**: cast to xs:string, then codepoint membership; the
  result is an xs:string. **Lists**: only from the string types
  (XPTY0004), collapsed and split, no tokens is FORG0001. Targets nest at
  most 32 deep.
- **castable**: dynamic and type errors of the cast give false; errors
  reading the operand, XPST0080, memory and limits are returned.

### D67. Tree, axes and arena tree (packet 1.11)
Recorded 2026-10-16. `src/tree_internal.h` holds `struct sxml_tree` (the
arena tree embeds it) and node-reference helpers.
- **Wrapper**: caches node count and name pool at initialize (trees are
  immutable); the identifier is issued last, so a failed initialize uses
  none. Namespace node names are found in the pool, interned only if new.
- **Axes** keep only origin and next node and derive each step from
  descriptions (D39 numbering): descendants/following by
  `N + 1 + attribute_count(N)`; preceding scans backwards from the
  reference node (an attribute's or namespace node's element), skipping
  attributes and nodes whose subtree reaches it (ancestors). Following of
  an attribute or namespace node includes its element's descendants.
- **copy_node** walks index order and closes elements by parent links:
  linear, constant C stack. Preserve sends the bindings the copy's parent
  lacks plus undeclarations; no-preserve sends none and lets fixup bind
  what names use. Without type preservation nilled is dropped, ID flags
  kept. `inherits_namespaces` cannot be honoured for the top element (P6).
- **Arena tree**: one `sxml_arena` over initial memory and the allocator.
  Nodes in segments of 16·2^k (never moved or copied); content in a text
  store whose pending run becomes one text node; in-scope lists shared
  when an element declares nothing new (xml binding in a base list).
  Fixup at header close: names keep a bound prefix, else bind it unless
  the element declared it otherwise, else reuse a prefix bound to the URI,
  else `ns0`, `ns1`...; a no-namespace element undeclares the default.
  XQDY0025, XQDY0102, XQDY0101 (xml/xmlns misuse) and XQDY0044 are raised
  into the configured error record. Duplicate checks scan up to 8
  attributes, then use a hash index. `maximum_depth` counts open
  elements; the text limit counts every byte copied into the store.
  Unparsed entities only before document content. After a failed event
  only finalize/destroy is meaningful.

### D68. Name pool in comparisons; record entries may fail (P2, P3)
Recorded 2026-10-16 by packet 1.19, on the orchestrator's acceptance of P2
and P3 (design edits to `include/sxml/xdm.h`).
- **P2 accepted**: `sxml_atomic_comparison` gains `const sxml_name_pool
  *name_pool`, NULL allowed when no xs:QName or xs:NOTATION is compared
  (a pool, not the heap: ordering names needs nothing else).
  `sxml_atomic_item_compare` now orders QNames by namespace URI, then local
  name (F&O 4.0, fn:compare, PR2256); two different names without a pool
  give SXML_STATUS_INVALID_ARGUMENT. The internal
  `sxml_atomic_item_compare_in_heap` stays (it uses the heap's pool, else
  the comparison's) because `src/xdm_internal.h` is not this packet's; it
  can go when 1.7's files are next edited.
- **P3 accepted, option "document"**: the contract of `sxml_map_get_entry`
  states that the first call on a record may return the heap's failures
  (OUT_OF_MEMORY, LIMIT_EXCEEDED), since record keys are made lazily (D61);
  `sxml_map_find` and `sxml_json_node_make_child` already list them.

### D69. No-inherit for the top element of a copy (P6)
Recorded 2026-10-16 by packet 1.14, on the orchestrator's acceptance of P6
(design edit to `include/sxml/tree.h`). `sxml_element_start_event` gains
`bool ignores_parent_namespaces`: true makes the element start from the
xml binding alone (plus its own bindings and fixup), not from its parent's
in-scope namespaces. Named so that zero, the default of every existing
designated initializer, keeps inheriting (P6 had proposed
`inherits_namespaces` defaulting to true, which zero cannot express).
`sxml_tree_builder_copy_node` sets it on the copy's top element when
`inherits_namespaces` is false (XQuery 4.0 copy-namespaces no-inherit);
descendants inherit from the top element as usual. The arena tree honours
it when it computes the header element's inherited list.

### D70. Node items, accessors and JNodes (packet 1.13)
Recorded 2026-10-16. `src/xdm_node.c`, `src/xdm_json_node.c`.
- **Node items** are inline (D60 layout) and retain their tree.
- **String value** of a document or element walks the index range up to
  `last_descendant` (D39) for text nodes; one text node is passed as is,
  more are joined in heap scratch memory released before returning.
- **Typed value**: derived (xs:untypedAtomic for documents, text,
  elements, attributes; xs:string for comments, PIs, namespace nodes)
  when the annotation is none, xs:untyped or xs:untypedAtomic, or the
  tree has no `get_typed_value`; otherwise the tree answers.
- **Base URI**: the xml:base values of the element and its ancestors, up
  to the first absolute one, resolved outermost-in against the tree's
  base URI (RFC 3986, section 5.2.2, strict; checked on all 42 examples
  of section 5.4). A relative value with no base is kept as written. No
  base URI for namespace nodes and for parentless attributes, text and
  comments; a parentless PI has the tree's. xml:base values are not
  percent-encoded first (XML Base, section 3.1).
- **JNodes**: one cell holds parent (retained), key, value, child
  position, child count (computed at make), depth and JTree identifier
  (issued last, so a failed root uses none). Under a map: one child per
  entry; an array: per member; two or more items: per item, keyed by
  position from 1; anything else is a leaf. Identity and order compare
  the paths of child positions from the root in O(depth) without memory:
  equal positions under one parent select the same key, so this is XDM's
  "same parent and atomic-equal key". The generic node functions live in
  `src/xdm_json_node.c`.

### D71. No C recursion
Recorded 2026-10-16 at the owner's request ("make sure we don't use C
recursion"). Project code never recurses: not directly, not through a chain of
functions, not through function pointers or operations tables. Every walk over
nested structure (CST, AST, IR, types, regex patterns, maps, arrays, trees,
JNodes, sequences) is a loop over an explicit stack or work list in allocator
memory, bounded by a configured limit that reports `LIMIT_EXCEEDED` (or the
specification's error). Consequences:
- clang-tidy `misc-no-recursion` is on (`.clang-tidy`); it sees recursion
  within one file. Review checks recursion across files and through pointers.
- The evaluator is an explicit-stack machine. Calling a function item from a
  native function (for example `fn:for-each`, `fn:sort` with a key, `fn:fold-left`)
  must not re-enter the evaluator on the C stack: native higher-order
  functions are written as resumable steps that ask the evaluator to make the
  call and resume them with the result. The same holds for lazy sequence
  generators that evaluate IR. Packets 1.18 and 1.21 follow this.
- Limits such as `maximum_expression_depth` bound heap use, not C stack, so
  their defaults no longer depend on the WebAssembly stack size (this replaces
  the 32-level lowering depth of D63).
- Existing recursive code is rewritten: `xpath_lower.c`, `xpath_static.c`,
  `ir.c`, `types.c`, `cast.c`, `regex.c`, `xdm_map.c`, `xdm_array.c`,
  `xdm_atomic.c` and two tests.
- Vendored third-party code (tree-sitter, libmpdec) is exempt; the generated
  grammar scanners are ours and have no recursion.

### D72. Arithmetic and comparison operators (packet 1.19)
Recorded 2026-10-16. `src/operator_internal.h`; `src/operator.c`
(operands, promotion, numbers, unary), `src/operator_temporal.c`
(durations, dates, times), `src/operator_compare.c` (value, general and
GNode comparisons).
- **Interface**: own enums (`sxml_arithmetic_operator`,
  `sxml_comparison_operator`, `sxml_node_comparison_operator`), not the IR's,
  so the function library and later front ends share them; operands are
  atomized sequences (atomizing is the evaluator's), results owned; the
  implicit timezone and, for untyped-to-QName casts, namespace bindings
  travel in the request. Non-atomic items are INVALID_ARGUMENT.
- **Numbers**: int64 fast paths with checked arithmetic, else libmpdec;
  integer div gives xs:decimal; xs:float computed in double and rounded
  once; idiv and mod of doubles are exact (int64 within 2^53, else exact
  decimals; no libm). Decimal LIMIT_EXCEEDED (digit or heap byte limit) is
  FOAR0002.
- **Durations** (PR2628): any xs:duration times/div a number cast to
  xs:decimal (shortest digits, D57); months by fn:round#1, nanoseconds half
  to even; NaN FOCA0005, times INF or div 0 FODT0002, div INF zero.
  **Dates**: 400-year cycle days (Hinnant), so every int64 year works;
  results beyond it FODT0001, differences beyond int64 seconds FODT0002.
- **Comparisons**: value = NaN rule + fn:compare (`sxml_atomic_item_compare`
  with the heap's name pool, D68); general: untyped vs number cast to the
  number's primitive type, else xs:double; vs string or untyped compared as
  text; else cast to the other primitive type; left outer, first error
  ends. GNode: document order (equal only for the same node).
- **Known problem (not this packet's)**: libmpdec's large-number
  multiplication (`six_step_fnt`/`transpose_pow2`, two 4096-word stack
  buffers) overflows the 64 KiB WebAssembly stack: 10^6000 * 10^6000
  traps under `wasm`. The operator tests avoid it; the decimal owner must
  fix it (smaller BUFSIZE under WASI or a bigger stack).

### D73. JNode making may fail with a lazy value's error (P7)
Recorded 2026-10-16 by packet 1.18, on the orchestrator's acceptance of P7
(option "amend contract"; design edit to `include/sxml/xdm.h`, comments
only). `sxml_json_node_make_root` and `sxml_json_node_make_child` count
their value's items, so a lazy value's generator may fail inside them; both
contracts now list those failures. They take no error record, so the status
comes without one: callers that need the error read the value first. The
evaluator never makes lazy sequences whose generator evaluates IR (D74), and
it reads values completely before fn:jtree sees them.

## Proposed

### P4. Namespace node tests with a name (from D63)
`namespace::x` is a STEP whose node test is a namespace test named x (a
prefix, in no namespace), but the type registry's normal form (D58)
accepts namespace tests only without a name. Proposed: accept an
`EXPANDED_NAME` name test in no namespace for `SXML_NODE_KIND_NAMESPACE`.
Until then such steps are not lowered.

### P5. XPTY0019 for the path operator (from D63)
`E1/E2` raises XPTY0019 when E1 yields an item that is not a node, but
the IR's MAP has no such check; a following STEP raises XPTY0020 instead,
and `$atoms/string()` raises nothing. Proposed: a flag
`SXML_IR_MAP_REQUIRES_NODES` in `sxml_ir_map_flag`, set by lowering on
the MAP of a path and checked by the evaluator.
