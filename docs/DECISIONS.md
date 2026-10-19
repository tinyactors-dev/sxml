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
- **No recursion (D71)**: the rules reduce a question to an and/or tree of
  subtype goals, evaluated left to right with short-circuiting on an
  explicit stack of subtype frames; the last sub-goal of a frame replaces
  it, so array(array(...)) and function results take one frame however
  deep. The header promises the subtype queries never allocate, so the 192
  frames (about 6 KiB under wasm, 9 KiB on 64-bit) are a fixed array in the
  query's own C frame; the 128 depth is unchanged. A goal that would need a
  193rd frame is false: only choices nested inside choices (an enumeration
  value or document-node() searched through a base choice of choices) need
  frames without depth. Derivation tries union members depth first on a
  fixed stack of 256 (the old derivation depth).

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
  costs one frame of the lowering machine in allocator memory (D71;
  no C recursion), bounded by `maximum_expression_depth` (default
  `SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH`, 10,000 levels, a few MB
  of frames at most; LIMIT_EXCEEDED beyond). The former default of 32 for
  the WebAssembly stack is gone. An inline function's
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
  most 32 deep (LIMIT_EXCEEDED beyond): unions, choices and lists are cast
  on an explicit stack of cast frames in heap memory (D71), grown up to 33.
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
- `tools/check-call-cycles.py` (run by `tools/lint.sh`) links all of src/
  and apps/ into one LLVM module and fails on any call-graph cycle, so
  recursion across files is caught too (not through function pointers).
  `tools/call-cycles-allowlist.txt` lists the cycles still being rewritten;
  it is temporary: each rewrite deletes its lines (a stale entry fails the
  check) until the file is empty and the allowlist is removed.
- Lowering (`src/xpath_lower.c`), type resolution (`src/xpath_static.c`)
  and the type text of `src/ir.c` are rewritten: lowering is a machine of
  frames allocated one by one (a procedure, a step, locals; results
  written into the frame below), type resolution and type text use stacks
  of at most 64 frames (`SXML_XPATH_MAXIMUM_TYPE_DEPTH`, the text's depth
  limit). Free-variable analysis skips AST subtrees without variable
  references, so nested inline functions cost linear time. 100,000 levels
  of parentheses, calls, inline functions and predicates compile, also on
  wasm. `misc-no-recursion` is on and the call-cycle allowlist is empty.

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
- **Known problem (fixed by D74)**: libmpdec's large-number
  multiplication (`six_step_fnt`/`transpose_pow2`, stack buffers of up to
  128 KiB) overflowed the 64 KiB WebAssembly stack: 10^6000 * 10^6000
  trapped under `wasm`. D74 moves the buffers to the heap; the operator
  tests may now multiply at the digit limit.

### D73. JNode making may fail with a lazy value's error (P7)
Recorded 2026-10-16 by packet 1.18, on the orchestrator's acceptance of P7
(option "amend contract"; design edit to `include/sxml/xdm.h`, comments
only). `sxml_json_node_make_root` and `sxml_json_node_make_child` count
their value's items, so a lazy value's generator may fail inside them; both
contracts now list those failures. They take no error record, so the status
comes without one: callers that need the error read the value first. The
evaluator never makes lazy sequences whose generator evaluates IR (D75), and
it reads values completely before fn:jtree sees them.

### D74. libmpdec keeps no large buffers on the stack
Recorded 2026-10-16, amending D57. Upstream `transpose.c` holds two
`SIDE*SIDE`-word buffers in `squaretrans_pow2` (128 KiB with 32-bit words)
and two `BUFSIZE`-word buffers in `swap_halfrows_pow2` (32 KiB). Every
product of more than 1024 words (on WebAssembly 9216 digits, smaller
operand above 2304) whose transform is not a power of two up to 4096
words takes the four- or six-step path through them and trapped on the
64 KiB WebAssembly stack; an untrusted query could do it within the
default digit limit.
- **Patch** ("sxml patch (D74)" in `transpose.c`): both pairs are one heap
  block from `mpd_alloc` (the current decimal scope, D42), sized for the
  actual block; `squaretrans_pow2` now returns 0 on failure and
  `transpose_pow2` passes it on, so the product reports `MPD_Malloc_error`
  (OUT_OF_MEMORY) and leaks nothing.
- **Audit** (`-fstack-usage`, wasm32 at `-Os`): after the patch the
  largest libmpdec frame is 1248 bytes (`mpd_qpowmod`; 2.5 KiB on x86-64
  GCC); the `MPD_MINALLOC_MAX` (64-word) buffers in `mpdecimal.c`,
  `basearith.c` and `io.c` and the 3-word arrays in `fourstep.c`,
  `sixstep.c`, `crt.c`, `numbertheory.c` stay. Measured by painting the
  stack under wasm: multiply needs 785 bytes, divide and integer divide at
  most 2.9 KiB, flat from 5000 to 16,000,000 digits.
- **Recursion** (D71 exempts libmpdec): only `_kmul_worksize`,
  `_karatsuba_rec` (operands of at most 1024 words: depth 7) and
  `_karatsuba_rec_fnt` recurse. The last runs only for products above
  3 * 2^25 words on 32-bit targets, more than two `MPD_MAX_PREC` (425
  million digit) operands have, and above 3 * 2^32 words on 64-bit ones,
  depth log2 of the size ratio with frames under 100 bytes. Newton
  division and the integer power sxml uses are loops. So no recursion
  patch and no lower cap on `maximum_digit_count` are needed.
- **Tests** (`tests/decimal_test.c`): squares and quotients at 10,000
  digits (four-step on wasm), 10^6000 * 10^6000 and two 10,000-digit
  operands refused with LIMIT_EXCEEDED, 60,000 digits (the plain six-step
  path on wasm), and every allocation failure of a long
  product reported and clean.

### D75. The evaluator: an explicit-stack machine (packet 1.18)
Recorded 2026-10-16. `src/evaluator.c` (dynamic context, machine,
expressions), `src/evaluator_function.c` (atomization, effective boolean
value, type matching, coercion, function items and bodies),
`src/evaluator_path.c` (steps over XNodes and JNodes, document order, node
sets, merge sort). Internal headers changed as D71 requires.
- **Machine** (D71): every expression with a nested expression, every call
  and every native step is an `evaluator_task` on a task stack in allocator
  memory; one loop runs the top task; a finished task hands its value to
  the task below. LET bodies, CONDITIONAL branches and calls in tail
  position replace their task; a tail call reuses the caller's frame
  (100k-deep non-tail recursion and 200k tail calls with depth limit 10 are
  tested, also on wasm). Leaf work (operators, casts, axes, atomization,
  matching) is plain C over explicit stacks of its own.
- **Depth** = calls in progress (IR calls and resumable native calls);
  `maximum_evaluation_depth` 0 means 10^6 (heap, not C stack, per level);
  beyond it LIMIT_EXCEEDED. `maximum_frame_slot_count` bounds slots.
- **Reentry**: `sxml_evaluator_evaluate`, `sxml_evaluator_call_function_item`
  and the call operations of IR, coerced and resumable native bodies run
  the machine only on an idle dynamic context (INVALID_ARGUMENT
  otherwise). Inside a run the machine recognizes those bodies by their
  operations pointer and calls them through tasks; maps, arrays,
  constructor functions and host bodies are leaves.
- **Resumable native bodies** (`function_library_internal.h`): a row with
  `CALLS_FUNCTION_ITEMS` has a `resumable_body` (step function, state size,
  `release_state`) instead of `call`. Each `native_step` finishes with a
  value or asks for one call of a function item; the evaluator coerces the
  arguments, makes the call on its stack and runs the next step with the
  result (owned). State is evaluator memory, zeroed, released exactly once.
  1.21 writes fn:for-each, filter, fold-*, sort with keys, apply,
  partial-apply's bodies, map:/array:for-each this way;
  `sxml_native_function_body_operations` is now defined in
  `src/evaluator_function.c`, because the evaluator dispatches on it.
- **Lazy sequences**: the evaluator makes none whose generator evaluates
  IR; MAP, FILTER, paths and calls build complete values (Phase 1).
- **Coercion** (XPath 4.0, 3.4): atomization, untypedAtomic cast (XPTY0117
  for QName/NOTATION), numeric and URI promotion, relabelling to a derived
  type of the same primitive only when the cast keeps the value (3 to
  xs:positiveInteger yes, 4.2 to xs:integer no), function coercion by a
  coerced-function body (lower arity allowed, extra arguments dropped),
  then a full type match. Type matching handles occurrences, atomic,
  union, enumeration, node, JNode (key and value), function, map, array,
  record and choice types on an explicit stack with backtracking for
  choices.
- **Paths**: STEP from a JNode reads element tests as key tests and runs
  every axis over the JTree; several origins are put in document order;
  DOCUMENT_ORDER keeps sorted input as it is. Constant numeric predicates
  index directly.
- **Errors** get the source span of the failing expression when the record
  has no location. The implicit timezone is read from the clock only when
  an operand is a date, time or duration (null clock: UTC).
- **Not yet**: TRY_CATCH, constructors, APPLY_TEMPLATES and global
  initializers return UNSUPPORTED_FEATURE; static namespaces are not
  available to casts to xs:QName at run time (predeclared prefixes only).

### D76. XPTY0019 for the path operator (P5)
Recorded 2026-10-16 by packet 1.18, on the orchestrator's acceptance of P5.
`SXML_IR_MAP_REQUIRES_NODES` in `sxml_ir_map_flag`: lowering sets it on the
MAP of `E1/E2` and of `//`; the evaluator raises XPTY0019 (XPath 4.0,
section 4.6.3) for an input item that is not a generic node. The text form
does not print the flag, so the golden dumps of D63 are unchanged; the
verifier accepts it.

### D77. Namespace node tests with a name (P4)
Recorded 2026-10-16 by packet 1.18, on the orchestrator's acceptance of P4
(design edit to the comment of `sxml_node_test` in `include/sxml/types.h`).
The registry's normal form accepts any name test for
`SXML_NODE_KIND_NAMESPACE` (no type, no document element); lowering
resolves `namespace::x` like an attribute name (no namespace), so it
matches the namespace node whose prefix is x; a prefixed name matches
nothing. The node-test matcher compares the namespace node's name, which
the tree interns as the prefix in no namespace (D67).

### D78. Function library: core (packet 1.20)
Recorded 2026-10-16; written first so packet 1.21 can rely on it.
- **Assembly**: `src/function_library.c` indexes whatever parts its
  configuration names (D55); no file lists another file's rows, and no
  shared list exists to edit. 1.21 only defines
  `sxml_function_library_higher_order`, `_map` and `_array` in its own
  files; the processor (1.23) names every part, unit tests name the parts
  they test (plus a test-only part of their own if they need one).
  `sxml_function_table_find` is defined in `src/function_library.c`.
- **fn:sort** (every arity, with or without `$key`) is one resumable row
  in `src/function_library_sequence.c` (1.20); 1.21 writes `array:sort`
  only. fn:position and fn:last are rows of `src/function_library_context.c`.
- **Files** (all under the module prefix): the six part files of D55, plus
  `src/function_library_deep_equal.c` (fn:deep-equal, a row of the
  sequence part), `src/function_library_case.c` (Unicode 16.0 full case
  mappings without conditions, generated by `tools/generate-case-tables.py`
  from UnicodeData.txt and SpecialCasing.txt) and
  `src/function_library_core_internal.h` (row macros, argument readers,
  heap text builder, collation check, position spans; defined in
  `src/function_library.c`). `function_library_internal.h` is unchanged.
- **Library**: indexed rows sorted by interned name (binary search); types
  resolved on first find and kept; variadic rows get repeated-type arrays
  kept in a chain so entry views stay valid; signatures registered per
  find. xs: constructors for atomic, union and list types (not
  anyAtomicType, NOTATION).
- **4.0 semantics followed** (from the QT4 tests): fn:max/min return the
  item itself, unpromoted, ordered by fn:compare (durations and QNames
  too), the first NaN wins when reached first; fn:subsequence and
  fn:substring take xs:numeric and add integers exactly (FOAR0002 past
  int64_t, PR2543); rounding of doubles uses the exact binary value;
  codepoints-to-string accepts XML 1.1 characters; string-length and
  normalize-space accept any item; fn:index-of matches NaN; fn:error's
  result type is xs:error; unknown string keys of deep-equal options are
  XPTY0004.
- **Resumable rows** (D75): fn:sort (key calls) and fn:replace (function
  replacement, coerced by the body with `sxml_evaluator_coerce` because the
  evaluator does not apply function coercion inside a choice type, so the
  parameter is `item()?`).
- **Not supported yet**: collations other than codepoint (FOCH0002);
  deep-equal options items-equal, ordered=false, unordered-elements,
  ignore-empty-entries, false-on-error, normalization-form and non-default
  schema-related options (UNSUPPORTED_FEATURE); deep-equal of two inline
  functions written alike is false; fn:trace does not print; the
  random-number-generator map is not annotated with the named record type.

### D79. Function library: higher-order, maps, arrays (packet 1.21)
Recorded 2026-10-16; written first so packet 1.20 can rely on it.
- **Ownership**: `src/function_library_higher_order.c` holds F&O 4.0
  section 2.5 except fn:sort (D78): fn:apply, do-until, drop-while, every,
  filter, fold-left, fold-right, for-each, for-each-pair, highest,
  index-where, lowest, partial-apply, partition, scan, some, sort-by,
  sort-with, take-while, transitive-closure, while-do; section 13
  (fn:function-lookup, function-name, function-arity,
  function-annotations) and fn:op (section 18.4).
  `src/function_library_map.c` holds section 14.4 (map:*),
  `src/function_library_array.c` section 15.2 (array:*), array:sort
  included. 1.20 writes none of these rows.
- Every row that calls a function item has a resumable body (D71, D75).

### D80. Higher-order, map and array functions (packet 1.21)
Recorded 2026-10-16, completing D79.
- **Engines**: `src/function_library_higher_order_internal.h` declares
  three resumable engines defined in the higher-order part and used by all
  three parts: a traversal (one call per item, member or entry, with its
  position; for-each, filter, index-where, take-/drop-while, every, some,
  for-each-pair, array:build/for-each/filter/index-where/for-each-pair,
  map:for-each/filter), folds (fold-left/right, scan, array folds) and
  sorts (sort-by, sort-with, highest, lowest, array:sort/-by/-with). Keys
  are computed one call each, then sorted by `sxml_evaluator_sort_indices`;
  sort-with runs a bottom-up merge sort one comparator call per step. No
  engine calls a function item (D71).
- **Hidden rows**: partially applied functions and fn:op's functions are
  function items whose body is a row no part lists (native operations,
  D75): captured values hold the wrapped function, the bound positions and
  values, or the operator's code.
- **Types the front end lacks**: the `$keys` of sort-by are `map(*)*`
  (the grammar has no optional or extensible record fields); the engine
  checks `key`, `collation` and `order` and atomizes key values itself.
  fn:partial-apply takes `map(xs:integer, item()*)` because map coercion
  does not relabel keys (an xs:integer key is not an
  xs:positiveInteger); a position below 1 is XPTY0004.
- **Placeholders**: lowering turns `$f(?)` into fn:partial-apply with the
  empty map, so the empty map returns `$function` unchanged only for an
  anonymous function item; a map, an array or a named function is
  wrapped, so `$m(?)` is not a map (hof-920) and has no name (XPath 4.0,
  4.5.2.5).
- **fn:function-lookup** uses the dynamic context's function table; a row
  that needs the focus at that arity raises XPDY0002 at lookup (F&O 4.0
  allows raising early), IR functions (XQuery) are UNSUPPORTED_FEATURE.
- **Not yet**: array:index-of (needs 1.20's deep-equal); a dynamic call on
  a sequence of functions (DynamicFunctionCall-146) is the evaluator's.

### D81. The XML parser (packet 1.14)
Recorded 2026-10-16. `src/xml.c` (walk and checks), `src/xml_encoding.c`
(encodings, character classes, positions), `src/xml_entity.c` (buffers,
DTD tables with hash indices).
- **Decoding**: Appendix F detection; UTF-8 and US-ASCII are parsed in
  place, UTF-16 and ISO-8859-1 are transcoded to a UTF-8 copy kept by the
  parser. UCS-4, EBCDIC and other names are UNSUPPORTED_FEATURE; an 8-bit
  document declaring UTF-16, a UTF-8 mark with another declared name, or
  BOM-less UTF-16 not declared UTF-16LE/BE is MALFORMED (4.3.3). U+0000 is
  refused before parsing (tree-sitter would read it as the end).
  Diagnostics give line, column (codepoints) and the input byte offset.
- **Walks** without C recursion (D71): the internal subset, then the
  document's children, each over a stack of entity frames (cursor + tree)
  in allocator memory. A general entity in content is reparsed as
  `<x>TEXT</x>` (4.3.2); its tree is cached per entity for the parse;
  replacement text without `<` or `&` is sent as text without a parse.
  An internal parameter entity between declarations is included by
  parsing `<!DOCTYPE d [TEXT]><d/>` (2.8); conditional sections in it are
  not supported (the grammar has none). Entities in attribute values are
  expanded with an explicit stack (4.4.5). Element nesting is bounded by
  `maximum_depth`, expansion by `maximum_entity_expansion_byte_count`
  (every replacement text counted, parameter entities included).
- **Choices**: line ends are normalized as text is emitted (pieces split
  at CR, the builder joins them), not in a copy; replacement text is not
  normalized again. Predefined entity names are always predefined
  (declarations of them are ignored). External parsed entities are
  skipped in content (4.4.3); undeclared ones are skipped when WFC Entity
  Declared does not apply. xml:id is an ID and collapsed; DTD types ID,
  IDREF(S) give is_id/is_idrefs. Element types and attribute names in
  DTD declarations must be QNames too. version 1.1 documents are parsed
  by the 1.0 rules, except that `xmlns:p=""` undeclares p (Namespaces
  1.1, section 5). Unparsed entity system identifiers are passed as
  written, not resolved against the base URI.
- **Measured**: W3C suite (Fifth Edition selection of D28) valid 601/601,
  invalid 175/175, not-wf 948/948 through the library
  (`tests/xml_conformance_test.c`, which reads `third_party/xmlconf` from
  the source tree and passes with a "skipped" line where it cannot, as
  under WASI). Of the 9 NAMESPACE="no" valid/invalid tests, 8 are refused
  as not namespace-well-formed and counted as passing. QT4 suite: 1046 of
  1051 `*.xml` files parse; the 5 refused are not well-formed on purpose
  (bad bytes, U+00BC in names, unknown encoding "foo", `invalid-xml.xml`).
  XSLT 4.0 suite, the 2541 files under 5 MB: 18 refused, 17 not
  well-formed on purpose and `xml-version-03.xml`, an XML 1.1 document
  with character references to control characters (1.0 rules apply).
- **CLI**: `sxml check` (docs/CLI.md); the framework gains
  `sxml_cli_file_reader` in the invocation, implemented over stdio in
  `apps/sxml/platform_stdio.c`; CLI golden tests run in `tests/cli/input/`
  so file names work under WASI too.

### D82. JSON texts are parsed with tree-sitter (packet 1.22)
Recorded 2026-10-16. fn:parse-json and fn:json-to-xml parse with a
tree-sitter grammar, `grammars/json` (strict RFC 8259, original work),
not with a hand-written parser; D12 applies without exception. Weighed
against D53's reasons for regexes: (1) JSON is run-time data and
tree-sitter allocates with malloc outside sxml's allocators and limits,
but fn:parse-xml already parses run-time data this way (D81), so that
alone is no exception; (2) unlike a regex, JSON is context-free with
context-free tokens: no flags change the lexing, no back-references, so
the grammar is complete and the walk adds only what a grammar cannot say
(surrogate pairing, permitted characters, duplicates, numbers); (3)
FOJS0001 is one code, and the walk reports the line and column of the
first ERROR or MISSING node in the description. The cost: the concrete
syntax tree is held while the walk builds values (several times the text
in memory), and U+0000 is refused before parsing (tree-sitter would read
it as the end; RFC 8259 forbids it unescaped). The walk is one loop over
a tree cursor (D71) that sends JSON events to a sink: one sink makes
maps, arrays and atomic items on explicit stacks, the other sends tree
builder events. `src/json_internal.h` is the interface.
- **Functions** (`src/function_library_json.c`, part
  `sxml_function_library_json`): fn:parse-json, json-to-xml, xml-to-json,
  jtree, parse-xml. Options follow F&O 4.0, 1.7: unknown string keys
  XPTY0004, QName keys with a namespace ignored, values coerced
  (`sxml_evaluator_coerce`), bad values FOJS0005; `liberal` accepts
  nothing beyond RFC 8259. Characters an escape gives that xs:string
  cannot hold (U+0000, unpaired surrogates, U+FFFE/FFFF) become U+FFFD;
  a `fallback` function is UNSUPPORTED_FEATURE for now (it needs a
  resumable body). json-to-xml: untyped tree in heap memory that destroys
  itself, default namespace on the outer element, `validate=true`
  FOJS0004. xml-to-json: FOJS0006 checks without a schema, duplicate
  normalized keys through a hash index, `indent` adds nothing.
  parse-xml: through the document loader (D55); xs:string as UTF-8,
  binary values as their octets (the string's encoding declaration is
  not ignored yet); dtd-validation, xinclude, strip-space="all" FODC0013,
  xsd-validation other than skip FODC0009.
- **Adaptive output** (`src/serialize.c`): frames on a heap stack (D71);
  strings in double quotes with quotes doubled, also at the top level;
  xs:double as mantissa and exponent (`1.0e0`), other non-string,
  non-decimal types and non-finite doubles as `xs:TYPE("value")`; QNames
  `#prefix:local` (array fn map math xs) or `#Q{uri}local`; anonymous
  functions `(anonymous-function)#N`; nodes by the XML method without a
  declaration, each element declaring the in-scope namespaces its written
  parent lacks; JNodes `jtree(V)` / `jnode(K:V)`.
- Root `CMakeLists.txt` links `sxml_grammar_json` (one line, 1.0's file).

### D83. The QT4 driver: catalog and environments (packet 1.24)
Recorded 2026-10-16. `tests/qt4/driver/`, the developer tool `sxml-qt4`
on the CLI framework (D44), public API only.
- **Reading**: catalog.xml and test-set files go through
  `sxml_xml_parser_parse` into an arena tree (own name pool, type registry,
  item heap) and are walked with the public node accessors; only elements
  in the catalog namespace count. One catalog object (lifecycle quintet;
  `load` makes the parts, `initialize` allocates nothing) keeps catalog.xml
  and one open test set; a test case's views and arrays live until the
  next `open_test_set` (a per-test-set arena). File paths are suite paths:
  relative to catalog.xml's directory, dot segments removed (RFC 3986
  5.2.4), so output is the same wherever the suite is and under WASI.
- **Environments**: `ref` looks in the test set first, then the catalog;
  an environment element without `ref` is inline even with a `name`
  (`<environment name="empty"/>`); no element means absent. All children
  of catalog-schema.xsd are read (sources, params with `as`, namespaces,
  context item, static base URI with `#UNDEFINED`, resources, collections,
  collations, decimal formats, schemas, function library, sandpit, modules).
- **Applicability** (first failing rule is the skip reason): spec as
  `tools/check-test-list.py` (case spec replaces set spec; every value
  needs a matching token); every other dependency of set and case must be
  satisfied as asked, satisfied meaning one of its tokens is in a table:
  feature higherOrderFunctions and namespace-axis, xml-version 1.0 and
  1.0:5+, xsd-version 1.1, limits big_integer and year_lt_0, language and
  default-language en; then the environment: source validation, schema,
  resource, collection, non-codepoint collation, decimal format, function
  library, sandpit; then modules. The table grows with sxml.
- **CLI**: `sxml-qt4 list [--catalog=FILE] [--spec=XP40|XQ40]
  [--test-set=NAME] [TEST-LIST]` (default catalog
  `third_party/qt4tests/catalog.xml`; neither selection means the whole
  catalog; both is a usage error). Text blocks per test case, summary
  line; problems on stderr, exit 1/3/4 as `sxml check`. Packet 1.25 adds
  `run` with `assertion.c` and `report.c`; the library target globs the
  directory, so it needs no CMake edit for new modules.
- **Tests**: ctest `qt4_driver` (unit tests over in-memory files,
  allocation-failure loop) and `qt4/*` golden tests over a small suite in
  `tests/qt4/driver/input/` and the slice (`qt4/list_slice`: 198/198
  resolve and apply to XP40).

### D84. (not used)
Recorded 2026-10-17 at the Phase 1 gate review. No record ever took this
number (packets 1.18b and 1.26 recorded D85 and D86 out of order, now
sorted); it stays empty so later references keep their numbers.

### D85. Evaluator follow-ups: calls, paths, coercion, namespaces (packet 1.18b)
Recorded 2026-10-16. Gaps reported by 1.20 and 1.21; sections are those of
the XPath 4.0 editor's draft of 13 October 2026 (vendored with QT4).
- **Dynamic calls** (4.6.3.1): the target is coerced to `function(*)*` (a
  JNode gives its jvalue); one function is called as before (tail calls
  kept), none gives (), several go through `call_each`, a resumable body of
  the evaluator's own (not a library row) that calls each in turn and
  concatenates. A dynamic partial application (4.6.4) lowers to a MAP over
  the target whose body is fn:partial-apply on the focus (golden hof-920
  of D63 changed).
- **Paths** (4.7.3, 4.7.5): maps and arrays on the left of `/` and `//`
  and as the origin of a step become JNode roots as if by fn:jtree. The
  error codes stay XPTY0019 and XPTY0020 (D76); the draft's XPTY0004 (PR
  2627) and its selector interpretation of non-navigational steps are not
  done.
- **Coercion** (3.4.2) runs on an explicit stack of `coercion_frame`s:
  choices and unions take the first alternative an item matches, else try
  each in order and keep the first attempt's error; typed arrays, maps and
  records are coerced part by part (records built with sxml_record_make;
  surplus keys and keys equal after coercion are XPTY0004); a JNode that
  does not match is coerced through its jvalue; the whole implicit-casting
  table applies when the required type is the "to" type exactly (so no
  xs:anyURI to xs:NCName); an enumeration accepts an xs:anyURI by its
  string; function coercion wraps even an instance of the type, except an
  item whose signature is that type. A value that matches a type whose
  matching items never change is returned after one match; an integer
  range is matched in O(1) (xs:integer a subtype of the item type).
- **fn:replace**: `$replacement` has its F&O 4.0 type again, `(xs:string |
  fn(xs:untypedAtomic, map((xs:integer | xs:string), xs:untypedAtomic)) as
  item()?)?`; the function gets untyped values (integer group keys only:
  the regex seed has no named groups), coerced by the call; D78's coercion
  in the body is gone.
- **Lowering**: a parameter whose default the body supplies, left out
  before a keyword argument, gets () when its type allows it: every such
  default in F&O 4.0 is () or default-on-empty (section 1.5), except
  `$timezone` of adjust-*-to-timezone, a last parameter. Otherwise still
  UNSUPPORTED_FEATURE.
- **Static namespaces**: a unit keeps the statically known namespaces
  beyond the predeclared ones (`sxml_ir_unit_set_static_namespaces`,
  `_get_static_namespaces`; collected by `sxml_xpath_collect_namespaces`:
  the expression's declarations, then the configuration's); CAST,
  CASTABLE and xs: constructor calls pass them to the cast. xs:QName#1 as
  a function item still sees the predeclared prefixes only.
- **array:index-of** calls fn:deep-equal's body once per member (a leaf).
- **number(xs:integer(...)) eq integer** is false by design: comparisons
  are decimal-exact since PR 2218 (QT4 fn-numberintg1args-2a); the cases
  that expect true depend on XP20 to XP31 only.

### D86. Fuzz targets (packet 1.26)
- **Targets.** Every `tests/fuzz/<name>_fuzz.c` is a libFuzzer executable
  (glob, D45) linked with `tests/fuzz/fuzz_fixture.c`: `xml_parse_fuzz`
  (parse into an arena tree and read every node back; `check` must give
  the same status), `xpath_compile_fuzz` (both front ends, then
  `sxml_xpath_compile` against the whole function library),
  `xpath_evaluate_fuzz` (compile and evaluate with a parsed context
  document and small limits; the internal evaluator until 1.23's
  processor), `regex_fuzz` (flags byte, regex, NUL, input; every disjoint
  match and its groups) and `cast_fuzz` (target byte, text; the canonical
  text of an atomic result must cast back to the same canonical text).
- **What a finding is.** A crash or sanitizer report; a leak (the counting
  allocator's blocks and the item heap's cells are checked after every
  input, on a fixture made fresh per input); an entry point returning
  `INVALID_ARGUMENT`, `INSUFFICIENT_MEMORY`, `MISALIGNED_MEMORY` or
  `INDEX_OUT_OF_RANGE` for arguments the target made valid (an error in
  sxml: for compilation, the verifier or the function table); a broken
  invariant above; a timeout (10 s per input). Deep input is not bounded
  in the targets: it must cost heap, not C stack (D71).
- **Build.** Preset `fuzz`: Clang, `RelWithDebInfo`, ASan and UBSan, the
  library built with `-fsanitize=fuzzer-no-link`, no `apps/`.
  `ctest --preset fuzz` runs each target on 2000 generated inputs.
- **Running.** `tools/fuzz.sh --seconds N` runs every target at once for N
  seconds in a kept work directory; seeds come from `tools/fuzz-seeds.py`
  (xmlconf and QT4 documents, QT4 queries, the literal arguments of
  `fn:matches`-like calls and `xs:T("...")` constructors; at most 4000 per
  target), never committed (D13); the first run merges them into a corpus.
  The script fails on any artifact, except timeouts of
  `xpath_evaluate_fuzz`, which runs in fork mode and lists them for
  triage until the evaluator has a step limit (P9).
- **CI.** Running each target for 60 s in CI is a `ci.yml` change, owned by
  1.27: a Linux job `cmake --preset fuzz && cmake --build --preset fuzz &&
  tools/fuzz.sh --no-build --build-directory build/fuzz --seconds 60`.

### D87. XML positions move back in place (P8)
Recorded 2026-10-16. The position tracker of `src/xml_encoding.c` no
longer restarts from byte 0 when asked for an earlier offset: it walks
back from where it is, undoing line, column and input offset, and when
a line end lies between it recounts the column from the start of that
line; it restarts only when the start is nearer. The parser keeps asking
for an element's position after its attributes' (and diagnostics may ask
anywhere), so the fix is in the tracker, not in the call order. Cost is
now linear in the document. Measured (release): `sxml-qt4 list
tests/qt4/torture-slice.txt` 23.3 s before, 0.76 s after, same output;
a 9.6 MB document with multi-line start tags did not finish in 600 s
before, parses in 2.7 s after (about 70% of that is `ts_parser_parse`,
so D12's tree-sitter bounds parse speed at about 3.5 MB/s here). Tests:
`xml/positions_move_back` (every pair of offsets against a fresh walk),
`xml/large_documents_parse_in_linear_time` (10000 elements, 400 KB).

### D88. The processor and the XPath API (packet 1.23)
Recorded 2026-10-16. `src/processor.c`, `src/processor_internal.h`,
`src/xpath.c`, `src/function_library_uri.c`; tests `processor`, `xpath`.
- **Processor**: one block holds the processor, name pool, type registry,
  item heap and the function library over every part (numeric, string,
  sequence, node, context, error, higher_order, map, array, json, uri);
  member requirements are queried with stand-in pointers (they only check
  for NULL). A null clock means `sxml_system_clock` (C17 `timespec_get`,
  civil date by Hinnant's algorithm); `sxml_clock_read` checks the whole
  reading contract (components, leap years, offsets) and the evaluator
  now reads through it. Parsed documents: arena trees from the
  processor's allocator with its tree and XML limits, no locations,
  destroyed with their last reference.
- **Limits** (comments in `processor.h` fixed): both depths bound heap,
  not C stack (D71); 0 means 10,000 for `maximum_expression_depth` (D63)
  and 1,000,000 for `maximum_evaluation_depth` (D75); new
  `maximum_evaluation_step_count` (D89).
- **Expressions**: compile is `sxml_xpath_compile` with the processor's
  function table; identifiers of the static context are checked against
  the pool and registry (INVALID_ARGUMENT), the static base URI must have
  a scheme and is copied. Each evaluation allocates its own dynamic
  context, global slots (bound by name to the unit's globals) and
  document loader, and frees them before returning, so evaluations nest
  (tested through a host function body).
- **Document loader** (per evaluation): `fn:doc` resolves its argument
  against the static base URI (`sxml_uri_resolve`, RFC 3986 5.2, now in
  `src/xdm_internal.h` over the resolver of `src/xdm_node.c`), then looks
  in the available documents, then the documents loaded so far, then asks
  the resolver; a resource that does not parse or a non-document node is
  FODC0002, the resolver's description goes into the error. fn:parse-xml
  makes a new tree per call (FODC0006).
- **Static base URI** reaches functions through the dynamic context
  (`sxml_evaluation_request.static_base_uri`,
  `sxml_dynamic_context_get_static_base_uri`): fn:doc, fn:parse-xml (when
  no `base-uri` option), fn:json-to-xml (document base URI), and the new
  rows fn:static-base-uri and fn:resolve-uri (FONS0005, FORG0002). A
  function item called after its evaluation sees the caller's.
- **CLI**: `sxml xpath` (docs/CLI.md): file resolver over the invocation's
  file reader (paths and `file://` only), `--param` as
  `xs:untypedAtomic` exactly one, `--method=adaptive` the only choice;
  errors as `expression:LINE:COLUMN: err:CODE: description`. 19 golden
  tests.
- `tests/fuzz/fuzz_fixture.c` names the json and uri parts too.

### D89. A bound on evaluation steps (P9 accepted)
Recorded 2026-10-16 (orchestrator accepted P9, with "0 means sxml's
default"). `uint64_t maximum_evaluation_step_count` in
`sxml_processor_limits`; 0 means
`SXML_EVALUATOR_DEFAULT_MAXIMUM_EVALUATION_STEP_COUNT`, 10^9 steps (about
a minute of work, far beyond any QT4 test). `sxml_evaluator_configuration`
has the same field with 0 meaning no bound. The machine charges one step
per task run; `sxml_dynamic_context_charge_steps` lets leaf work charge
what it reads: a general comparison charges the product of its operand
counts first (O(1) for ranges), so `-1 = -100000000000 to -1` stops at
once, even though an early match would have ended it (the price of
charging up front). The count restarts with every evaluation; past the
bound, LIMIT_EXCEEDED with the diagnostic filled. Native bodies that loop
over many items without tasks (fn:count, fn:string-join, sorting, regex)
do not charge yet; each can call the function when needed.
`xpath_evaluate_fuzz` sets 10^7 steps.

### D90. map:merge merges into its largest map (fuzzing follow-up of 1.26)
Recorded 2026-10-16. A fold that merges a small map into its accumulator
was O(n^2): map:merge inserted every entry of every map into a new map.
- map:merge starts from its first largest map L (`src/function_library_
  map.c`) instead of the empty map. With L first, it is taken as it is
  (no map has duplicate keys, so inserting it into the empty map meets
  none, under every policy). With L later, and without a duplicates
  function, the entries of the earlier maps are put first, last map and
  last entry first, through `sxml_map_put_first` (`src/xdm_map_
  internal.h`): an entry of an equal key is dropped and the earlier entry
  takes the front with its key and the combined value (use-first/any: the
  earlier value; use-last: the later; combine: earlier then later; reject:
  FOJS0003). These combinations are associative, so the result, entry
  order and kept key included, is the forward insertion's. The maps after
  L are inserted as before. A duplicates function keeps the forward order
  (its calls stay in F&O 4.0 order) but still takes the first map as is.
- Persistent maps (D61) number entries from 2^63: puts last count up from
  there, puts first down; `previous_entry_stamp` beside `next_entry_stamp`.
- Cost: O(m log n) for the m entries outside L, so
  `fold-left(1 to N, map{}, fn($m, $i) { map:merge(($m, {$i: $i})) })`
  and its mirror `map:merge(({$i: $i}, $m))` (QT4 duplicates-maps-2) are
  O(N log N). `map_merge_scales` in
  `tests/function_library_higher_order_test.c`: folds of 10,000 steps,
  3.6 s for the case under ASan (release: 320,000 merges in 1.8 s); the
  quadratic version took 6-18 s under ASan for 1,000.
  map:put in a fold was already O(log n) per step.

### D91. Packet 1.23b: more library functions, lexical base URI, charged steps
Recorded 2026-10-17.
- **Functions added** (62): the math namespace (`src/function_library_math.c`,
  part `sxml_function_library_math`: pi, e, exp, exp10, log, log10, pow,
  sqrt, sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, as the
  C17 Annex F functions; pow with an integer exponent is pown, sign by
  parity beyond 2^53); QNames and namespaces (`src/function_library_qname.c`,
  part `sxml_function_library_qname`: local-name-, prefix-,
  namespace-uri-from-QName, expanded-QName, in-scope-prefixes,
  in-scope-namespaces, namespace-uri-for-prefix); node: doc-available
  (loads through the document loader, FODC errors mean false), base-uri,
  document-uri, has-children, generate-id (`t<tree>n<index>[s<ns>]`;
  JNodes UNSUPPORTED_FEATURE); string: translate, codepoint-equal,
  contains-token, substring-before-last, substring-after-last,
  trim-space, parse-integer (int64, then exact decimals); uri:
  encode-for-uri, iri-to-uri, escape-html-uri; sequence: identity,
  unordered, void, foot, trunk, slice, items-at, replicate,
  insert-separator, compare, atomic-equal, all-equal, all-different,
  duplicate-values, default-collation; context: dateTime and the six
  *-from-duration functions. The processor and the fuzz fixture name
  the two new parts. Gaps (77 functions with their own XP40 test sets,
  4,165 test cases) are listed in the 1.23b line of the task file;
  fn:normalize-unicode needs the Unicode decomposition tables and is one.
- **Adaptive output** (`src/serialize.c`): the constructor-function form
  writes the name of the item's type annotation, not of its primitive
  type (`xs:dateTimeStamp(...)`, `xs:dayTimeDuration(...)`), as qtspecs PR
  2981 revises Serialization 4.0 and QT4 Serialization-adaptive-37a
  expects. The other rules still go by "instance of" (derived strings
  quoted, derived integers as numbers), which every XQ40 adaptive test
  accepts; -41 and -42 (XQ31+) still expect `xs:duration` and fail.
- **The static base URI is lexical** (XPath 4.0, section 2.2.1): IR units
  carry it (`sxml_ir_unit_set_static_base_uri`, set by lowering from
  `sxml_xpath_static_context.static_base_uri`), and
  `sxml_dynamic_context_get_static_base_uri` answers that of the unit of
  the innermost task that runs IR, falling back to the evaluation
  request's for units that never set one. A function item called after
  its evaluation, or by another expression, sees its own (replaces the
  last bullet of D88's "Static base URI"). The document loader no longer
  falls back to the evaluation's static base URI: callers pass it.
- **Charged steps** (completes D89): `sxml_function_library_charge_steps`
  and `_charge_items` (no-ops without a dynamic context) charge leaf work:
  one step per item for fn:string-join, fn:distinct-values,
  fn:duplicate-values, fn:contains-token, fn:items-at, fn:replicate,
  fn:insert-separator and fn:count of a lazy sequence or concatenation;
  n log2 n for fn:sort; one per pair for fn:deep-equal; the regex
  instructions each search ran (new `sxml_regex_matcher_get_step_count`)
  for fn:matches, fn:replace and fn:tokenize; |value| x |replace| for
  fn:translate.

### D92. The QT4 driver: running tests, assertions, reports (packet 1.25)
Recorded 2026-10-17. `sxml-qt4 run [--catalog] [--spec] [--test-set=NAME |
TEST-LIST] [--format=text|json]`; `tests/qt4/driver/test_run.c`,
`assertion.c`, `report.c`, `reason.c`, `command_run.c`.
- **Isolation**: each test case gets its own processor, made through a
  budget allocator (at most 512 MiB live; header-prefixed blocks) with
  `maximum_evaluation_step_count` 10^8; afterwards the budget must have no
  live allocation, else the outcome is error "leaked N allocations". A
  crash still ends the process: whole-suite runs use one process per test
  set. No `--output=FILE`: the CLI platform layer has no file writer
  (redirect standard output).
- **Environment**: sources parsed with document and base URI = their
  `uri` (made absolute against the test-set file) or `file:///SUITE-PATH`;
  "." context value, "$name" variables `item()*`, `uri` available
  documents; params via `select` with `as` from
  `sxml_xpath_parse_sequence_type`; QName param names through the
  environment's namespaces; static base URI `file:///SUITE-PATH` of the
  test-set file unless given or `#UNDEFINED`. The resolver serves
  `file:///` URIs as suite files and nothing else.
- **Assertions** are XPath over `$result` (and `$expected`, the
  assertion's value) with the environment's namespaces: assert-eq =
  one atomic item and `$result = $expected or deep-equal(...)` (untyped
  cast, NaN = NaN); permutation by per-item deep-equal counts; assert-type
  = `$result instance of TEXT`; assert-xml = result written as XML (nodes
  by the adaptive serializer, atomics as escaped text joined by spaces),
  both sides `parse-xml`ed in a wrapper element and deep-equal with
  comments and PIs (no full canonicalization; prefixes never matter);
  expected files lose their XML declaration and surrounding space. A wrong
  error code is the outcome "wrong-error" and counts as passed.
  Serialization assertions are "unsupported": the outcome is not-run, not
  fail (deviation from the task text). Verdicts combine worst-to-best
  fail < unsupported < wrong-error < pass on a fixed 32-frame stack (D71).
- **Outcomes**: pass, wrong-error, fail, error (a status without an error
  record, a limit, a leak, an environment that cannot be set up),
  not-run (not applicable or unsupported). Text lists every non-pass,
  non-not-run case, a line per test set and a final `total:` line; JSON
  (schema in `report.h`) lists every case. Exit 1 on a fail or error.
- **Slice gate**: ctest `qt4/slice` runs the slice and fails when fewer
  pass than `tests/qt4/slice-baseline.txt` (198); a higher count asks for
  the baseline to be raised. Goldens `qt4/run_*` over
  `input/run-catalog.xml` cover every assertion kind passing and failing.

### D93. WebAssembly slice: preopens and the first size budget (packet 1.28)
Recorded 2026-10-17. Node 22.22, WASI SDK 34, `wasm` preset (MinSizeRel).
- **Preopens**: `tools/run-wasi.mjs` preopens the working directory as "."
  (as before) and the source tree, the parent of `tools/`, under its own
  absolute path. Tests that find files from `__FILE__`
  (`xml_conformance`: `<root>/tests/../third_party/xmlconf`) now run under
  WASI instead of "skipped": 601/175/948, as native. The whole tree, not
  only `third_party/`, because wasi-libc picks a preopen by path prefix
  without resolving "..". `qt4/slice` already ran in the repository root;
  198/198 pass under `wasm`. The CLI's `cli/xpath_*` goldens (19) already
  run under WASI, so no new e2e tests.
- **Sizes** (module stripped with `llvm-strip`; gzip -9): `sxml` CLI
  1,534,842 bytes raw, 1,213,261 stripped, 358,020 gzip (code 808 KB,
  data 401 KB of which 374 KB read-only: parse tables); `sxml-qt4`
  1,588,530 / 1,264,250 / 376,528. Archives: `libsxml.a` 947,910;
  tree-sitter 189,342; mpdecimal 153,762; grammars xml 93,664, xpath
  340,646. Native release `sxml` stripped: 1,743,480 (x86-64).
- **Time** (3 runs, 4 cores): the slice, `sxml-qt4 run
  tests/qt4/torture-slice.txt`: native release 0.89-1.00 s, `wasm` 1.88-1.92
  s (about 2x; Node start-up 0.05 s); `xml_conformance` 0.11 s native,
  0.26 s `wasm`.
- **Budget** (PLAN.md risks): the stripped `sxml` CLI module stays under
  1.5 MB and 450 KB gzipped until a decision raises it; a change that adds
  more than 50 KB stripped logs the growth (as 1.2 did). Not yet checked
  by CI: that needs a size test in CMake or `ci.yml` (not this packet's
  files).

### D94. Conformance dashboard and CI jobs (packet 1.27)
Recorded 2026-10-17. `tools/run-qt4.sh --program SXML-QT4` runs one
`sxml-qt4 run --spec=XP40 --test-set=NAME --format=json` process per test
set of catalog.xml (in parallel, `timeout` 600 s each) plus the slice, into
`build/qt4-run/`; an exit other than 0 or 1 marks the set "did not finish"
and the run goes on. `tools/conformance-report.py DIR` writes
`docs/CONFORMANCE.md` (slice; totals; areas by test-set prefix fn, op,
prod, map, array, math, xs, misc, app, method, upd, ft, bin, expath; error
codes, reasons, wrong errors, not-run reasons; worst test sets; every test
set), optionally a merged JSON and a slice-baseline check (exit 1 below
it). The commit shown is the last one outside `docs/`, so committing the
regenerated file does not change it: same run, same file. An XQ40
directory is reported too once XQuery runs (`--spec XQ40`). CI: job
`conformance` (release, Clang, XP40 + slice, artifacts `CONFORMANCE.md`
and `conformance.json`, step summary, blocking only on the slice) and job
`fuzz` (D86 command); concurrency cancels in-progress runs except on main.
The committed dashboard is refreshed by hand (or by an agent), not by CI.

### D95. Pass-rate targets for Phases 2, 3 and 4
Recorded 2026-10-17 (Phases 2 to 4 planning; PLAN.md leaves the targets to
the end of Phase 1). Measured by `sxml-qt4 run` (release, one process per
test set) and, for XSLT, the driver of packet 4.1; "wrong error" counts as
passed (D92), as the QT4 guide allows.
- **Denominators**: the cases whose spec applies (XP40, XQ40, XSLT40; a
  case spec replaces the set's), minus those that need a non-goal: features
  schemaImport, schemaValidation, typedData, staticTyping, XQUpdate,
  fullText*, xpath-1.0-compatibility, fn-transform-XSLT*, fn-invisible-xml,
  remote_http, expath-file, binary, advanced-uca-fallback,
  schema-location-hint; sets upd-*, ft-*, bin-*, expath-*,
  misc-HtmlTestSuite, fn-parse-html, fn-html-doc, fn-invisible-xml,
  fn-transform (D97). About 31,500 XP40 and 41,200 XQ40 cases (9,700 of
  them XQ-only). XSLT: spec XSLT40 outside the `strm` and `sandp` groups
  and without the features schema_aware, streaming, dtd, XML_1.1, XSD_1.1,
  backwards_compatibility, HTML4, ICU_spellout: about 8,700 of 12,627.
  Cases with serialization assertions count (they fail until 3.3).
- **Targets**: Phase 2, XP40 ≥ 90%, each area fn, op, prod, map, array,
  math, xs ≥ 80%. Phase 3, XQ40 ≥ 85%, XQ-only ≥ 75%, method-* ≥ 80%, XP40
  kept. Phase 4, XSLT in scope ≥ 70%, twelve core sets ≥ 80% (listed in
  `docs/tasks/phase-4.md`). Every gate also needs zero crashes, leaks and
  "error" outcomes.
- **Why these**: Phase 1 ended at 21,443 of about 31,500 XP40 cases (68%).
  The known causes are missing work, not design faults: unknown functions
  5,596 cases, regex 480, unlowered constructs 395, math 181, collations
  100; together about 21% of the denominator, so 90% asks for that plus a
  third of the remaining failures. XQuery adds 9,700 XQ-only cases that
  share the XPath core; 75% of those with 90% of the rest gives 86%. The
  XSLT suite is younger, its cases compare whole outputs and many test
  packages and error codes, so 70% is the first honest bar; regex-syntax
  (987) and error (500) alone are 17% of it.
- A target is changed only by a new record, before its gate review.

### D96. Phases 2 to 4 overlap
Recorded 2026-10-17. To reach working XQuery and XSLT sooner (D11),
packets marked **Early** in `docs/tasks/phase-3.md` and `phase-4.md` may
start once the Phase 1 gate has closed (the substrate freeze) and the
packets in their "After" lists are committed, while earlier phases still
run. All other packets of a phase wait for the previous phase's gate.
- Ownership rules of D45 hold across phases: packets that run at the
  same time never own the same file; a file named by packets of two phases
  belongs to whichever runs first, and the other waits.
- Seams first: Phase 2 wave 0 makes library parts, evaluator parts for the
  reserved IR kinds and lowering parts register by glob (packets 2.0,
  2.1), so XQuery and XSLT packets add files instead of editing the
  evaluator, the lowering machine or the processor's lists.
- Gates stay sequential: the Phase 3 review needs the Phase 2 gate, and a
  phase's review counts only its own targets. Early packets do not move
  their phase's gate forward.
- `CLAUDE.md` names the current phase; at each gate the review names the
  next phase file and the Early packets that may run with it.

### D97. Scope of Phases 2 to 4
Recorded 2026-10-17 (non-goals per PLAN.md, made concrete).
- **Collations**: the Unicode codepoint collation and the HTML ASCII
  case-insensitive collation (both required by F&O 4.0), and UCA collation
  URIs with fallback (on by default): codepoint order, with strength
  primary and secondary matching case-blind (QT4 simple-uca-fallback).
  Full UCA (DUCET tables, tailoring, numeric and case-order options;
  advanced-uca-fallback) stays out: the tables cost more `wasm` size than
  the 81 tests that need them are worth. fallback=no on a UCA URI is
  FOCH0002.
- **Out**: fn:parse-html and fn:html-doc (an HTML5 tree builder is its
  own project; 1,400 QT4 cases, excluded by D95), invisible XML, schema
  awareness (validate is XQST0075; XSLT validation="strict" and type
  attributes raise XTSE1660, as for a basic processor), streaming (streamable="yes" runs
  unstreamed, as a non-streaming processor may), the Update Facility
  (its tests are not run; %updating is rejected statically), Full Text, EXPath,
  XPath 1.0 compatibility mode and XSLT backwards-compatible behaviour,
  civil-timezone and Olson names (no tz database), calendars other than
  AD and ISO and languages other than English in format-date and
  format-integer (the fallback markers F&O allows).
- **Later in this plan**: fn:serialize with every method (3.4),
  fn:load-xquery-module (3.11); fn:transform only if Phase 4 sweeps leave
  time.
- **Hand-written scanners**: regex syntax (D53), picture strings of the
  formatting functions and CSV are scanned by hand, because their lexical
  rules are run-time options (decimal formats, CSV delimiters) that a
  tree-sitter grammar cannot take. Everything else keeps D12, including
  XSLT value templates (`grammars/value_template`).

### D98. Substrate review at the Phase 1 gate (packet 1.29)
Recorded 2026-10-17 by the Phase 1 review agent (D11). Before the freeze
(D99) every record that touched the substrate headers (D19-D43, D68, D69,
D73, D77, D89) was checked against the Phase 2-4 task files; "Proposed" was
empty. All earlier records stand. Found and changed:
- **Base URI of a parentless root** (`tree.h`): the arena tree had no way
  to give a constructed element, attribute, text, comment or PI a base URI;
  only `start_document` carried one, but XQuery 4.0 constructors (packet
  3.5) need the static base URI and XSLT 4.0 the instruction's (section
  11.1: it matters only for a parentless node, a child takes its parent's;
  XQuery copies behave the same, QT4 cbcl-base-uri-001 to -004). New field
  `sxml_arena_tree_configuration.base_uri`, copied by `finish`, reported
  as `sxml_tree_description.base_uri`; zero keeps the old behaviour.
- **Parentless namespace nodes** (`tree.h`, contract only): computed
  namespace constructors and xsl:namespace make namespace nodes without an
  element, which D39's (element, namespace number) cannot name. A tree may
  now hold one stored namespace node, its root (index 0, namespace number
  0), built by an `add_namespace_binding` sent first; its name is the
  prefix, its content the URI (empty URI: XQDY0101). The arena tree and
  `sxml_tree_builder_copy_node` implement it (`src/arena_tree.c`,
  `src/tree.c`, test `parentless_namespace_nodes_are_stored`); the adaptive
  serializer and the evaluator's namespace paths still assume an element
  and are packets 3.3 and 3.5's.
- **Named item types** (`types.h`, comment): `find_type` no longer claims
  to find them; XQuery `declare type` and xsl:item-type are aliases in a
  module's static context, never registered by name.
- **Host re-entry** (`xdm.h`, comment): a host function body, generator,
  resolver or builder that starts an evaluation nests it on the C stack;
  bounding that is the host's (D71 holds for sxml's own code).
- `status.h`, `string.h`: "D92" is Unicode's definition D92, not this file's.
Deferred, each needing its own record when it comes: adopting heap buffers
for strings (Phase 0 question 7, only if Phase 2 profiling asks),
registering schema-derived atomic types (Phase 5), a frozen shareable tree
(D35), a per-element base URI for XSLT literal result elements that come
from different stylesheet modules and are copied as parentless nodes with
relative xml:base (no QT4 or XSLT test seen needs it).

### D99. Substrate frozen
Recorded 2026-10-17 at the Phase 1 gate (PLAN.md: "substrate frozen at the
Phase 1 gate"). Frozen: `include/sxml/status.h`, `view.h`, `memory.h`,
`string.h`, `name.h`, `types.h`, `tree.h`, `xdm.h` (with `export.h`), as of
the commit that records this, including D98. Not frozen: `xml.h`,
`processor.h`, `xpath.h`, `serialize.h`, `version.h` (D47: they change
through their design packets, and after packet 2.2 only with a record,
until release 0.2.0 makes them stable). Rule for a frozen header:
- Any change, a comment that changes a contract included, needs a new
  decision record first that names the header, the change and the packet
  that needs it; implementation packets still write the need under
  "Proposed" and stop (CLAUDE.md).
- Allowed with a record: new functions, new types, new enumeration
  constants at the end of an enumeration whose header says it only grows
  (built-in types, well-known namespaces, statuses), and new fields at the
  end of configuration and request structs whose zero value keeps the old
  behaviour. Not allowed without reopening this record: changing or removing
  a declaration, the meaning of an existing field or status, the 16-byte
  item and sequence layout (D34), node numbering (D39), or an operations
  table (interfaces are not extended in place: a new table type instead).
- Comments that only fix wording or a citation need no record.
- ABI: nothing is ABI-stable before release 0.2.0 (packet 2.18); the freeze
  is about the design, so fields are still added at the end only.

### D100. Phase 1 gate review (packet 1.29)
Recorded 2026-10-17 by the review agent (D11), who did not write Phase 1.
- **Rubric** over `include/sxml/` (all headers read), the evaluator,
  lowering, XDM, tree, XML parser and processor (sampled), the rest spot
  checked: names and glossary, at most three parameters (also every
  function-pointer type), lifecycle quintets (public and internal types
  complete), ownership, units, failure and thread safety in public headers,
  error codes (61 distinct, all well formed), spec citations. No writable
  file-scope data in `libsxml`, the CLI or the driver (release objects
  have empty `.data` and `.bss`; libmpdec's are vendored, D42); no
  `malloc`/`free` outside `src/allocator.c`; no printing, `abort` or
  `exit` in `src/`.
- **D71 through pointers**: every indirect call in `src/` was followed.
  Cell release re-enters `sxml_cell_release` from `release_contents` only
  to queue (a flat loop frees); concatenations are flattened, so a part is
  never a concatenation; the evaluator runs IR, native-row and coerced
  bodies as tasks and calls only map, array, constructor and host bodies
  directly; `sxml_evaluator_call_function_item` refuses a running context;
  arena-tree and tree-builder operations never call back into `tree.c`;
  tree owners only destroy; generators, resolvers and output streams are
  the host's. `tools/check-call-cycles.py` now also links the QT4 driver
  (with the CLI framework) as a second program: 85 files, no cycle.
- **CI cannot run**: every GitHub Actions job since 2026-10-16 is refused
  ("recent account payments have failed or your spending limit needs to be
  increased"); the last green run is 38931572793 (Phase 0 gate). All gate
  checks were run locally (Linux x86-64, Clang and GCC, WASI SDK 34 on
  Node 22): presets `debug`, `debug-gcc`, `release`, `wasm`, `fuzz`, lint,
  slice, fuzzing, dashboard (numbers in LOG). macOS, Windows and MSVC are
  unverified since the Phase 0 gate: an open gate risk, not ticked. The CI
  boxes of the gate stay open until the owner restores billing and a run
  is green.

### D101. Part registries and the evaluator seams (packet 2.0)
Recorded 2026-10-17, completing D45 and D55 for Phases 2 to 4.
- **Library parts by glob.** `cmake/SxmlParts.cmake` reads every
  `src/function_library_*.c` and generates `function_library_parts.c` in
  the build tree from each definition `const sxml_native_function_view
  sxml_function_library_<part> =`; files without one (case tables, deep
  equality, `function_library.c`) are not parts. A part serves every
  language unless it also defines `const sxml_language_set
  sxml_function_library_<part>_languages = SXML_LANGUAGE_XSLT;` (any of
  `SXML_LANGUAGE_XPATH`, `_XQUERY`, `_XSLT` joined by `|`): CMake reads
  the names, the generated table also points at the variable, and a unit
  test checks the two agree. `src/function_library_parts_internal.h`
  declares `sxml_function_library_registered_parts()` (every part, sorted
  by name) and `sxml_function_library_parts_for_language()` (a static
  list per language). The processor and the fuzz fixture take the XPath
  list; XQuery and XSLT compilation take theirs (3.x, 4.x). A new part
  declares its own `extern` view (or none: the generated unit declares
  it); `function_library_internal.h` keeps the Phase 1 declarations.
- **Evaluator parts by glob.** The same module reads `src/evaluator_*.c`
  for `const sxml_evaluator_part sxml_evaluator_part_<part> =`: a table
  of IR expression kinds and step functions. The machine runs its
  built-in kinds itself and every other kind through the registry; a
  kind no part claims, or two parts claim, is
  SXML_STATUS_UNSUPPORTED_FEATURE (a unit test checks each reserved kind
  has exactly one). Parts `construct` (seven constructors, SHALLOW_COPY),
  `try` (TRY_CATCH), `template` (APPLY_TEMPLATES) and `global` (no kind;
  `sxml_evaluator_start_global_initializers`, called before the main
  function's body runs for globals with an initializer and no value)
  return UNSUPPORTED_FEATURE until 3.5, 3.6, 4.x and 3.10 fill them. A
  given value of a global with an initializer wins (an external
  variable's default). 3.10's reads on demand need the reading task to
  wait for the initializer; if the global part cannot do it with tasks
  pushed at the start (in dependency order), 3.10 proposes a machine
  change.
- **A part is live when its file is**: a committed part file joins every
  build, the processor and the fuzz fixture at once, so its rows must not
  repeat a name and arity another part serves (the library then refuses
  to start, INVALID_ARGUMENT); a packet moving rows moves them in one
  commit.
- **Machine API** in `src/evaluator_machine_internal.h` (defined in
  `src/evaluator.c`): the task type, top task, push an expression task in
  the top task's frame, push a part task (a step function and owned state
  with its release function), hand a value down (finish), take the value
  of the task above, read and bind atoms, raise (report into the error
  record), and catching: a task whose `catches_errors` is set receives a
  dynamic or type error raised above it. The run loop locates the error,
  unwinds every task above the innermost catching task above its base,
  stores the status in the context (`caught_status`) and steps that task
  again; limits, memory and invalid-argument statuses are never caught.
  No C recursion: step functions return to the one loop (D71).
- Rejected: a hand-kept part list (every new part edits the processor,
  the fuzz fixture and tests: D45's conflict); per-language libraries
  chosen at run time from flags in the view (needs a change of the frozen
  row header and filtering memory).

### D102. The stylesheet AST (packet 4.2)
Recorded 2026-10-17. `src/xslt_ast_internal.h`, internal, after D52 and
the IR unit (D55).
- **One AST per stylesheet** (a used package is another): modules, nodes,
  attributes, expressions, static contexts, a declaration list, name
  tokens, namespace bindings and URIs, text; index-addressed arrays with
  entry 0 "none" that grow through an allocator while the reader builds
  and are sealed before lowering (as `sxml_ir_unit`). Module URIs get
  their own allocations, so the per-expression `sxml_ast` borrows them as
  source name. The AST owns those ASTs and a reference to each module's
  document node.
- **Nodes**: one kind per XSLT 4.0 element in the 4.0 schema
  (schema-for-xslt40.xsd; xsl:transform is STYLESHEET, xsl:note never
  arrives), plus LITERAL_RESULT_ELEMENT, TEXT_NODE, EXTENSION_INSTRUCTION,
  UNKNOWN_INSTRUCTION (forwards-compatible) and DATA_ELEMENT. A node's
  attributes are consecutive; children linked as in D52.
- **Attributes**: one kind per attribute name of the schema; the value is
  read into the form the element syntax names: EXPRESSION, PATTERN,
  VALUE_TEMPLATE, SEQUENCE_TYPE, ITEM_TYPE (each its own `sxml_ast`),
  TEXT, BOOLEAN, NUMBER, KEYWORD, NAME, NAME_LIST, NAMESPACE_BINDING.
  QName-valued attributes are resolved while reading (XTSE0280); names
  inside expressions stay as written for static analysis. An AVT is
  always a VALUE_TEMPLATE, even without `{`: an XPath AST with a
  STRING_TEMPLATE body, lowered as string templates are. Standard
  attributes stay as TEXT; their effect is in the static context.
- **Static context per element**, resolved and shared until something
  changes: statically known namespaces (after fixed-namespaces), xpath
  default namespace (or ##any), default collation, mode and validation,
  expand-text, version, cumulative excluded and extension namespaces,
  static base URI. Lowering never walks up the tree.
- **Reader duties** (4.5): whitespace and commentary stripping, use-when,
  shadow attributes, simplified stylesheets (synthesized STYLESHEET and
  TEMPLATE match "."; expand-text from the version, 3.7), include and
  import read in place depth first (duplicate includes in a level
  ignored, 4.0), syntax of every value. Import precedence is computed by
  `sxml_xslt_ast_seal` from the module tree (post-order, includes share
  their includer's), since modules are added in pre-order.
- **Who writes what**: the builder and lifecycle functions are defined in
  a new `src/xslt_ast.c` with `tests/xslt_ast_test.c`, both written by
  packet 4.5 (the AST's only producer, as 1.16 wrote `xpath_ast.c`);
  added to 4.5's files in `docs/tasks/phase-4.md`. A text form
  (`sxml_xslt_ast_write_text`) serves golden tests; the header writes
  out for-each-group-001, a template rule and an xsl:function in it.
- Rejected: one shared `sxml_ast` for all expressions (its capacity is
  fixed from one concrete syntax tree, D52); keeping attribute values as
  text for lowering to parse (every lowering part would repeat the
  checks, and XTSE errors belong to reading); a fixed-capacity AST sized
  from the trees (modules arrive one by one through the resolver).

### D103. Lowering parts and the embedding entry (packet 2.1)
Recorded 2026-10-17, completing D63 and D71 for Phases 3 and 4.
`src/xpath_lower_internal.h` (module `xpath_lower`) opens the lowering
machine to other files.
- **The lowerer** (`sxml_xpath_lowerer`, lifecycle quintet): one lowering
  into one unit, over a static context, for one `sxml_language` (D101),
  with a part view (empty: the registry). Building blocks are exported
  under `sxml_xpath_lowerer_*`: scratch memory, add an expression or a
  slot, bind in a block, finish a block, constants, raise, push and pop
  variables, read the focus, resolve and coerce types, signatures, enter
  and leave an IR function.
- **Procedures are descriptors**: a frame holds a pointer to a static
  `sxml_xpath_lowering_procedure` (name, step function, locals size);
  the core's 24 procedures are a table of them, so core and part frames
  run alike. Steps ask for parts with `sxml_xpath_lowering_call_*` and
  return; no step runs a machine (D71). `_push_procedure` pushes any
  procedure with initial locals (copied bytes; one depth level), for
  constructs that are not AST kinds (XSLT instructions over the D102
  AST); `sxml_xpath_lowerer_run_procedure` runs one from outside.
- **Lowering parts by glob**: a file `src/<name>_lower.c` or
  `src/<name>_lower_<part>.c` defining `const sxml_xpath_lowering_part
  sxml_xpath_lowering_part_<part> =` (languages, rules of AST kind to a
  procedure or a leaf function) is registered by
  `sxml_generate_lowering_part_registry` in `cmake/SxmlParts.cmake`
  (generated `lowering_parts.c`, `sxml_xpath_lowering_registered_parts()`,
  sorted by file). `call_expression` takes the first part serving the
  lowerer's language that lists the kind, then the core; so a part may
  replace a core kind for its language (XQuery FLWOR). A unit test checks
  that no two registered parts list one kind for a common language.
  The CMake function and its two lines in the root `CMakeLists.txt` join
  packet 2.0's files after 2.0 is committed (2.1 adds them then).
- **Embedding entry**: `sxml_xpath_lowering_call_embedded` (from a step)
  and `sxml_xpath_lowerer_lower_embedded` (outside) lower one XPath AST
  into the current function: variables the caller pushed are in scope as
  their atoms, the focus is the caller's choice (absent, slots, fixed) or
  the current one, the static context is the AST's (namespaces, function
  table with XSLT's in-scope functions; same heap and allocator; its
  variable declarations are the unit's globals in order). The lowerer's
  AST, static context and focus are restored afterwards. `sxml_xpath_lower`
  lowers its main function through it, so the slice dumps cover it.
- After a failure a lowerer is fit only for finalize. `sxml_xpath_lower`
  and `_compile` (`xpath_static_internal.h`) are unchanged: XPath, the
  registered parts; XQuery and XSLT compile with a lowerer of their own.
- Rejected: parts as a second enum range in the core's switch (every
  part would edit the core); running a nested machine for embedded
  expressions (C recursion through a step).

### D104. The public API for Phase 2 (packet 2.2)
Recorded 2026-10-17, completing D51's "left out until needed" for
Phase 2. `processor.h`, `xpath.h`, `version.h`; only new fields at the end
of structs and new constants at the end of enumerations, so Phase 1 code
compiles unchanged (zero keeps the old behaviour).
- **Decimal formats** (static context): `sxml_decimal_format` (name, index
  0 the unnamed format) with properties as text, `{kind, value}`, one kind
  per F&O 4.0 property (11). Text, not codepoints, because every source
  gives text (xsl:decimal-format attributes, XQuery declarations, QT4
  `decimal-format`), and 4.0's marker-and-rendition form (`%:pc`, type
  `'.|.:.*'`) parses once in sxml. An absent kind has its default. A
  breach (wrong form, markers not distinct, zero-digit not an Nd zero,
  duplicate names) is INVALID_ARGUMENT for the XPath API; XQuery and XSLT
  raise their own codes (XQST0097/8, XTSE1300) before handing formats over.
- **Collations** (static context): `default_collation_uri` (empty is
  codepoint; XPath 4.0 puts it in the dynamic context, but every host
  knows it statically) and `collation_bindings`, host URIs that stand for
  a supported collation (the spec's "mapping from URI to collation").
  `sxml_collation_is_supported(uri)` tells hosts and the QT4 driver which
  URIs sxml implements (D97); packet 2.12 defines it in `src/xpath.c`
  over the collation lookup that 2.13 later completes. No host collation
  interface: D97 keeps collations inside sxml.
- **Dynamic context**: `default_language` (empty is "en"),
  `environment_variables` (name/value views; sxml never reads the
  platform's), `trace_sink` (null means the processor's).
- **Resources**: kinds TEXT, BINARY, COLLECTION, URI_COLLECTION,
  XQUERY_MODULE, XSLT_MODULE, XSLT_PACKAGE, each with the forms it allows
  (a wrong form is INVALID_ARGUMENT); new form SEQUENCE with an owned
  `items` sequence for collections; request fields `encoding_name`
  (fn:unparsed-text's argument; sxml decodes by the resource's encoding,
  then the request's, then BOM, then UTF-8), `is_default_collection`,
  `target_namespace_uri` (import module) and `package_version`
  (xsl:use-package; uri_reference is the package name). The table
  resolver serves bytes for every kind that takes them. `processor.c`
  gains the SEQUENCE case of fn:doc's switch (INVALID_ARGUMENT).
- **Trace sink**: one interface for fn:trace, fn:message and xsl:message
  (and failed xsl:assert): `receive(state, event)` with kind, label, value
  (borrowed sequence), `is_terminating` and `error_code` for XSLT,
  location and processor; the null sink discards; a failure stops the
  evaluation; a sink may nest an evaluation, as a host function may.
  `sxml_stream_trace_sink(&output_stream)` writes `label: value` lines
  (adaptive, item separator " "): the default implementation and test
  double, defined by 2.12 in `src/processor.c`. `processor.h` now
  includes `serialize.h` (no cycle). The two new functions are declared
  only until 2.12; nothing calls them before.
- **Version**: `SXML_VERSION_ENCODE(major, minor, patch)`,
  `SXML_VERSION_NUMBER` and `SXML_API_VERSION` = 0.2.0, the first public
  release's API; 2.18 sets the version macros themselves.
- Still left out, each a new field later: host extension functions,
  cancellation, a random seed, default calendar and place.
- From now on these headers change only through a record until 0.2.0
  (D47, D99).

### D105. The regex engine: the whole F&O 4.0 dialect (packet 2.3)
Recorded 2026-10-17. D9 is met by growing the D53 seed in place, behind
the same `src/regex_internal.h` (compile, match at, find, find next),
not by a rewrite: its parser, emitted program and backtracking machine
already follow Saxon HE's design.
- **Syntax** (F&O 4.0, section 6.1.3): category escapes `\p{Lu}` (no
  `Cs`, as the grammar), block escapes `\p{IsX}` by the Blocks.txt name
  without spaces plus the four renamed XSD 1.0 names (`IsGreek`,
  `IsCombiningMarksforSymbols`, `IsCyrillicSupplementary`,
  `IsPrivateUse`); unknown names are FORX0002 (6.1.7). `\i \c` are XML
  1.0 fifth edition NameStartChar and NameChar. XSD 1.1 hyphens: a "-"
  that separates no range is a character (`[a-d-b-c]`, QT4 re00056a).
  `\b \B`, `(?=) (?!) (?<=) (?<!)` and the `(*positive_lookahead:...)`
  spellings; `(?<name>...)`, `\k<name>`. Assertions take no quantifier.
- **Lookaround** without recursion: LOOK_START pushes an assertion entry
  on the backtrack stack; LOOK_END of a positive assertion drops the
  choices above it but keeps the slot-restoring entries (captures stay,
  and are undone if matching later backtracks past the assertion; first
  match wins, 6.1.8.3), of a negative one undoes them and fails;
  backtracking onto the entry fails a positive assertion and resumes a
  negative one after it. A lookbehind branch is fixed-length
  (6.1.8.5), so it starts with STEP_BACK n.
- **Case-blind** matching (6.2): variants by `fn:lower-case`/`upper-case`
  through `sxml_function_library_map_case` (the tables of
  `src/function_library_case.c`, shared), plus 94 pairs those mappings do
  not reach (KELVIN SIGN, final sigma, ...), generated with the same code
  as the case tables. A literal becomes a class of its variants at
  compile time; a range checks the input's variants at match time;
  back-references compare codepoint by codepoint.
- **Names**: the compiled regex keeps each group's name and whether it is
  in a lookahead (new `sxml_regex_group_name`, `_find_named_group`,
  `_group_is_in_lookahead`); while compiling, names live in a table that
  grows through the stack allocator like the group stack.
- **Tables** (`tools/generate-regex-tables.py`, Unicode 16.0:
  UnicodeData, SpecialCasing, Blocks): 4,099 category runs packed in
  `uint32_t` (\d and \w now derive from them), 342 blocks, 94 pairs; about
  25 KB of read-only data.
- Not done: counted repetition is still expanded, so `a{2147483647}` is
  LIMIT_EXCEEDED (QT4 cbcl-matches-004, -038).

### D106. The XQuery and serialization API (packet 3.0)
Recorded 2026-10-17. `include/sxml/xquery.h` is new; `serialize.h` grows
by new constants at the end of `sxml_serialization_method` and new fields
in `sxml_serialization_parameters`, regrouped by size (enumerations,
presence flags, views) because clang-tidy's padding check rejects the
interleaved order. Callers initialize by field name, so Phase 1 and 2 code
compiles unchanged (zero is still the adaptive method with its defaults);
nothing is ABI-stable before 0.2.0 (D99).
- **Methods**: XML, XHTML, HTML, TEXT, JSON, CSV after ADAPTIVE. No
  constant for implementation-defined (namespaced) methods; the readers
  raise SEPM0016 for them. A method not implemented yet returns
  UNSUPPORTED_FEATURE (the header's contract); `src/serialize.c` still
  returns INVALID_ARGUMENT for non-adaptive methods until 3.3 changes it.
- **Parameters**: one struct with every Serialization 4.0 parameter,
  including the 4.0 additions (canonical, csv-*, escape-solidus,
  indent-unit, indent-attributes, json-lines, line-ending), field names
  the parameter names with `_`. Booleans are `sxml_serialization_flag`
  (DEFAULT, NO, YES), because several defaults are "yes"
  (escape-solidus, escape-uri-attributes, include-content-type,
  omit-xml-declaration) and a zero struct must mean "defaults"; DEFAULT
  is fn:serialize's default (F&O 4.0 table), so omit-xml-declaration
  defaults to yes; XSLT sets NO itself. Enumerations have their default
  as constant 0 (UTF-8, standalone omit, normalization none, LF,
  json-node-output-method xml). Parameters the spec lets be absent
  (doctype-*, html-version, indent-unit, item-separator, media-type,
  version) are `has_` + text; html-version stays text (an xs:decimal
  lexical form) because the HTML method also falls back on version.
  csv-separator and csv-quote-character are codepoints, 0 the default.
  Element-name lists are name pool identifiers (`sxml_element_name_view`,
  in serialize.h because name.h is frozen); character maps are
  codepoint/replacement pairs. Encodings: UTF-8, UTF-16 (big endian),
  US-ASCII, ISO-8859-1 (`sxml_character_encoding`); other names SESU0007.
  Indent unit default two spaces.
- **Errors**: a value Serialization 4.0 does not permit is SEPM0016 (a
  dynamic error) when serialized, so fn:serialize sees the spec code; what
  XDM cannot even express (ill-formed UTF-8, unknown constants, name 0,
  a repeated character-map codepoint) is INVALID_ARGUMENT.
- **Parameter reader**: `sxml_serialization_parameter_reader` (lifecycle
  quintet; allocator and item heap) reads fn:serialize's `$options` (empty,
  a map, or an `output:serialization-parameters` element) into the struct
  and owns the copied text and lists until its next read. Map: XPTY0004
  for wrong types, SEPM0016 for bad values; element: SEPM0017, SEPM0018,
  SEPM0019 (Serialization 4.0, 3.1); method xml when none is given. The
  textual forms of XQuery output declarations and xsl:output (yes/no/1/0,
  `\t`, QName lists) are read by internal code of 3.10 and Phase 4, not
  by a public function.
- **XQuery**: `sxml_xquery_query` (quintet, like `sxml_xpath_expression`;
  the main module's text is `expression_text`, glossary) compiled with
  the shared `sxml_static_context_configuration`, evaluated with
  `sxml_xquery_evaluation_request` holding the shared dynamic context.
  Host variable declarations are in scope in the main module only (a
  prolog declaration of the same name is XQST0049); bindings also bind
  every module's external variables. Library modules come from the
  resolver, kind XQUERY_MODULE, one request per location hint (an
  empty uri_reference for none); the parameter document of
  output:parameter-document is an XML_DOCUMENT request, read at compile
  time (XQST0119/XQST0115). Output declaration errors (XQST0108-0110,
  SEPM0016) are compile errors.
  `sxml_xquery_query_get_serialization_parameters` returns the struct by
  value: declarations over the parameter document over XQuery defaults
  (method xml); the host overrides fields of the copy (CLI `--method`).
- Left out, each a later field: XQuery-only static context settings
  from the host (boundary-space, construction, ordering), a list of the
  query's external variables, fn:load-xquery-module options (3.11 is
  internal), implementation-defined serialization parameters.

### D107. Unicode tables and functions (packet 2.4)
Recorded 2026-10-17. `src/unicode_internal.h` (defined in `src/unicode.c`
over the data of `src/unicode_tables.c`, generated by
`tools/generate-unicode-tables.py` from Unicode 16.0 and the WHATWG
`entities.json`), used by `src/function_library_unicode.c` (fn:char,
fn:characters, fn:graphemes, fn:normalize-unicode) and open to the
serializer's normalization-form (3.3) and to collations (2.13).
- **Separate from the regex tables** (D105): those hold general
  categories, blocks and case variants; these hold combining classes, full
  canonical and compatibility decompositions (looked up, not expanded at
  run time; Hangul by the arithmetic of Unicode 16.0, section 3.12),
  primary composites, the composing starters, the UAX #29 properties
  (Grapheme_Cluster_Break, Indic_Conjunct_Break, Extended_Pictographic
  packed per run) and the 2,125 HTML names. No property is in both.
- **Normalization** (UAX #15) works on the whole string: decompose into a
  codepoint array that grows through the caller's allocator, put each run
  of non-starters in canonical order (insertion sort for short runs, a
  stable counting sort by combining class for long ones, so the cost stays
  linear), compose in place by the UAX #15 sample algorithm, encode. The
  result is UTF-8 owned through that allocator. FULLY-NORMALIZED prepends
  a space when the first character is a composing character, then NFC
  (F&O 4.0 rules). ASCII input is returned as it is.
- **Graphemes**: an iterator over a string yields byte ranges; its state
  (previous break value, count of regional indicators, the emoji and
  conjunct sequences in progress) decides rules GB3 to GB999 of UAX #29
  (Unicode 16.0, with GB9c) one codepoint at a time.
- **fn:char**: an integer must be a permitted character (FOCH0005, also
  beyond 0x10FFFF); a name is looked up case-sensitively by binary search;
  the escapes are \n \r \t \b \f.

### D108. The QT4 driver for Phase 2: environments, scope, areas (packet 2.16)
Recorded 2026-10-17. Extends D83 and D92 in `tests/qt4/driver/`.
- **Environments now applied**: `decimal-format` (every attribute one
  D104 property, value as written; the name's prefix bound by the
  element's in-scope namespaces, unprefixed names in no namespace);
  `collation` (any D97 URI runs: codepoint, HTML ASCII, UCA unless
  `fallback=no`; `default="true"` sets `default_collation_uri`; other URIs
  stay "environment collation"; the driver's own check until 2.12's
  `sxml_collation_is_supported` exists); `resource` and `collection`
  through a resource table (`resource_table.c`, glossary
  `qt4_resource_table`): TEXT and BINARY by the resource's absolute URI
  (relative ones against the test-set file; TEXT gets the `encoding`
  attribute as the external encoding), else `file:///SUITE-PATH`;
  COLLECTION and URI_COLLECTION as SEQUENCE (document nodes, or their
  document URIs as xs:anyURI), `uri=""` is the default collection;
  collection sources with a `uri` are also available documents; a
  collection's resources are served like the environment's and their URIs
  follow the documents' in fn:uri-collection. Still skipped: collections
  with a `query` (3.13). Resources are not served to fn:doc.
- **Dependencies** now satisfied: feature simple-uca-fallback,
  non_empty_sequence_collection, arbitraryPrecisionDecimal;
  unicode-normalization-form NFC NFD NFKC NFKD (Phase 2 makes them work;
  the D95 denominator counts their cases anyway). Not collection-stability:
  its two cases ask for it to be absent.
- **Scope**: applicability also says whether a case is in the D95
  denominator (spec matches, no non-goal test set, no required non-goal
  feature; tables in `applicability.c`). Out-of-scope cases that apply
  still run, so pass counts stay comparable. `sxml-qt4 list` ends with
  "N test cases: A applicable, S skipped; D in scope" and marks
  "out of scope: ..."; `list --spec=XP40` gives 31,543 in scope,
  `--spec=XQ40` 41,174 (D95: about 31,500 and 41,200).
- **JSON report** (still version 1, members only added): test sets,
  totals and a new `areas` array (D94's prefixes) carry `in_scope` and
  `in_scope_passed`; `in_scope_passed / in_scope` is the gate rate.
  `tools/conformance-report.py` (1.27) may read them; it ignores them now.
- Measured (release): XP40 passed 23,146 to 24,451, no pass lost; not run
  18,448 to 16,321. The ten new "error" outcomes are UNSUPPORTED_FEATURE
  in newly run cases, not the driver.

### D109. try/catch in the evaluator (packet 3.6)
Recorded 2026-10-17, completing D101 for TRY_CATCH
(`src/evaluator_try.c`; XQuery 4.0, section 4.20).
- **One catching task** per TRY_CATCH, phases start, body, handler,
  finally. The try clause's value is read completely before it counts,
  so a lazy sequence's error is caught by the try that made it. Catch
  clauses are tried in order, the first whose name tests match wins
  (`*`, EQName, `p:*`/`Q{uri}*`, `*:local`); an error with no code in the
  record counts as err:FOER0000.
- **Error details** (glossary `error_details`): a clause's
  `error_details` slot is bound to `$err:map`, entries keyed by xs:string
  in table order: "code" (xs:QName, prefix kept only with a namespace),
  "description", "value", "module", "line-number", "column-number"; none
  for an empty value. `$err:stack-trace` and `$err:additional` are always
  `()` in sxml, so they have no entry. Lowering (3.7) reads each `$err:`
  variable as `map:get($slot, "local-name")` and `$err:map` is the slot.
  They are copied out of the record before anything else runs (D41).
- **Own error record**: when the caller passed none, the outermost try
  points the context's record at one in its task state while it runs and
  puts NULL back when it ends or unwinds; inner tries use it. Uncaught
  errors are still reported nowhere. Done inside the part because
  `src/evaluator.c` belongs to 2.6 in this wave; the machine may later
  keep its own record instead, with no visible change.
- **finally**: an error of the try clause that no clause matches, and an
  error of the catch clause, are caught only to run the finally clause;
  their details are kept (glossary `pending_error`) and raised again
  through the heap when it succeeds (the finally clause may raise and
  catch errors on the same heap). An error of the finally clause replaces
  them; a non-empty value is XQTY0153 (type error). Limits, memory and
  invalid arguments run no catch or finally clause.
- XPath 4.0 has no try/catch (checked in the 4.0 draft): nothing for 2.5.
  XSLT's xsl:try (4.x) can lower to the same kind.
- `tests/evaluator_machine_test.c` no longer expects UNSUPPORTED_FEATURE
  for TRY_CATCH (list `evaluated_kinds`, where 3.5 adds the constructors).

### D110. Node constructors in the evaluator (packet 3.5)
Recorded 2026-10-17. `src/evaluator_construct.c` evaluates the seven
constructor kinds and SHALLOW_COPY (D55, D101).
- **One tree per constructed node.** Each constructor makes a new arena
  tree in the item heap's memory that destroys itself with its last
  reference (as fn:json-to-xml); a nested constructor's node is a tree of
  its own that the enclosing constructor copies with
  `sxml_tree_builder_copy_node`. So a constructor's step only waits for
  its content value and then does leaf work (no C recursion, D71); copying
  costs one evaluation step per copied node, each content item one (D89).
  The arena tree gets the evaluation's error record: it reports only while
  events are sent, inside the step.
- **Content convention for lowering (3.9, 4.x).** A content expression of
  kind SEQUENCE lists the enclosed expressions and literal text of a direct
  constructor, one atom each, read directly (not pushed); atomic items are
  joined by spaces only within one atom (`<c>{1}{2}</c>` is "12", XQuery
  4.0, 4.12.1.3). Any other content expression (an ATOM, a LET...) is one
  enclosed expression: a computed constructor's whose value is a
  SEQUENCE must be bound to a slot first. Arrays are flattened with an
  explicit stack of content frames; JNodes add their atomized value;
  function items are XQTY0105.
- **Names.** The name atom holds an xs:QName (used as it is, prefix kept)
  or an xs:string, xs:untypedAtomic or xs:anyURI: trimmed, `Q{uri}local`,
  `Q{uri}prefix:local` or a lexical QName resolved with the unit's static
  namespaces (prefix 0 = default element namespace, elements only);
  XQDY0074 otherwise, XPTY0004 for another type or not exactly one item.
  XQDY0096 and XQDY0044 are checked here for every name; an attribute in a
  namespace without a prefix gets `xml` or `ns0`. PI targets XQDY0041 (any
  non-NCName string, as QT4 expects), XQDY0064, XQDY0026; comments
  XQDY0072; namespace prefixes XQDY0074, URIs XPTY0004 (not one string),
  XQDY0101 from the builder. Written namespace bindings (the
  `namespace_bindings` run) are sent after the start event: lowering
  should give a direct constructor's own declarations (enclosing ones are
  inherited through the copy while copy-namespaces is inherit).
- **Modes fixed for now**: construction strip (xs:untyped,
  xs:untypedAtomic), copy-namespaces preserve, inherit; ELEMENT and
  DOCUMENT follow XQuery (duplicate attributes XQDY0025, XQTY0024,
  XPTY0004 for attributes in a document), SHALLOW_COPY follows XSLT 4.0
  5.7.1 (later duplicates replace, XTDE0410, XTDE0420, XTDE0450,
  XTTE3180 for more than one item; element namespaces copied).
  construction_flags and name_namespaces select the other modes
  (D124).
- **Base URIs**: elements get the static base URI as their tree's base
  URI (`sxml_arena_tree_configuration.base_uri`, D98), documents through
  their start event; attributes, text, comments and PIs have none
  (QT4 fn-base-uri-5 to -7).
- **Deviation**: `text {""}` gives the empty sequence, not an empty text
  node: the arena tree drops empty text (D40). Inside a constructor the
  two are the same (the empty text node is deleted).

### D111. The query AST for XQuery and the parser part seam (packet 3.1)
Recorded 2026-10-17, extending D52 and D59. `src/xpath_ast_internal.h`.
- **Kinds**: 66 XQuery kinds appended to `sxml_ast_kind`, one per new
  visible node type of `grammars/xquery` (`SXML_AST_KIND_<NODE TYPE>`);
  `allowing_empty` is absorbed as flag ALLOWING_EMPTY. 24 fields appended
  (the grammar's, plus MODULE, which the front end gives the child of
  `module`, the one named child without a grammar field). Tokens in
  fields `mode`/`kind` become the new detail `keyword` (PRESERVE, STRIP,
  ORDERED, ..., LAX, STRICT); the decimal-format property name the detail
  `decimal_format_property` (`sxml/xpath.h`'s kind); the other XQuery
  tokens flags (FIXED, EXTERNAL, STABLE, DESCENDING, EMPTY_GREATEST,
  EMPTY_LEAST, ONLY, STATIC, PRESERVE_NAMESPACES, INHERIT_NAMESPACES).
  Front-end marks: BOUNDARY_WHITESPACE on direct element text,
  NAMESPACE_DECLARATION_ATTRIBUTE on `xmlns` attributes. Leaves: QNAME has
  a name payload, the direct text kinds, CDATA, comments, pragma contents
  and string constructor characters a decoded text, a direct PI the new
  payload `processing_instruction` (target, content). The header writes
  out AST and IR of five XQuery examples (group by, a tumbling window, a
  direct constructor with namespaces, try/catch/finally, a prolog
  function).
- **Parser part seam**: `sxml_ast_build_with_parser_part(request, part,
  &ast)` runs the one XPath walker (D59: iterative, A.3.2 and lookup key
  checks, depth bound) with a part's language and root node type, its
  sorted node type and field tables (consulted after XPath's), unnamed
  child fields, and three stateless hooks: `apply_token` (claims a token
  before the XPath rules), `describe_payload` (fills a payload before the
  XPath decoders, e.g. XQuery string literals with references) and
  `finish_node` (called once per node when it is complete, leaves at
  once; for XQST0118, XQST0040, boundary whitespace). Hooks reach the AST
  and source text through the opaque `sxml_ast_front_end`, add flags with
  `sxml_ast_add_flags`, and raise any err: code with
  `sxml_ast_front_end_raise` (the error record gets that code). Without a
  part the walk is unchanged: all 32,928 applicable XP40 queries build
  byte-identical ASTs. With a scratch part of only the tables, all 43,663
  valid XQ40 queries and the 145 parsable library modules build.
- **Who writes what**: 3.2 defines `sxml_xquery_build_ast` (declared now)
  and its part in `src/xquery_parse.c`; it needs `sxml_grammar_xquery`
  added to the `sxml` link line of the root `CMakeLists.txt` (one word,
  3.2 adds it). Parts are passed by each entry point, not registered by
  glob: the entry point chooses the grammar.
- `tests/xpath_parse_test.c`'s kind and field name tables grow with the
  enumerations (static asserts); 3.1 appended the XQuery names.
- Rejected: a parser registry by glob (one grammar per entry point, so
  nothing to discover); the XQuery tables in `xpath_parse.c` (the part
  owns its grammar's names, as lowering parts own their kinds); hook
  state pointers (everything a hook needs is in the AST).

### D112. The XSLT API (packet 4.0)
Recorded 2026-10-17. `include/sxml/xslt.h` is new, header only, after
`xquery.h` (D106); 4.15 implements it in `src/xslt.c`.
- **Stylesheet**: `sxml_xslt_stylesheet` (lifecycle quintet, like
  `sxml_xquery_query`) compiled from the principal module's bytes, or
  fetched through the resolver as an XSLT_MODULE (a URI reference) or an
  XSLT_PACKAGE (name and package-version), chosen by
  `sxml_xslt_principal_module_source`. Includes, imports and used
  packages always come from the resolver (XTSE0165, XTSE3000, XTSE0180).
  Static parameters are `sxml_variable_binding_view` values given at
  compile time (XTDE0050 is then a compile failure with DYNAMIC_ERROR).
  Of the shared static context only static_base_uri, the default
  collation and collation bindings are read; namespaces, variable
  declarations and decimal formats must be empty, since the stylesheet
  declares its own.
- **Output definitions**: `sxml_xslt_output_definition` = the D106
  parameters struct + `build_tree` (a serialization flag) +
  `has_serialization_method` (false: method chosen from the raw result,
  XSLT 4.0 26.1.2, which serialize.h cannot express). Read by name with
  `sxml_xslt_stylesheet_get_output_definition` (0 = unnamed); XSLT's
  omit-xml-declaration default is filled in as NO, indent stays DEFAULT
  and means yes for html/xhtml when sxml serializes.
- **Transformation**: one call, `sxml_xslt_stylesheet_transform`, primes
  and invokes (one execution scope per call; a primed object for several
  invocations is left out). The request embeds the shared dynamic
  context: context value = global context item (one item or absent),
  variable bindings = stylesheet parameters, trace sink = xsl:message
  (terminating events end in XTMM9000/XTMM9001 or the given code, D104).
  `sxml_xslt_invocation_kind` APPLY_TEMPLATES (0; initial match selection
  defaulting to the global context item, initial mode 0 = default mode,
  `selects_unnamed_mode` for the spec's note), CALL_TEMPLATE (name 0 =
  xsl:initial-template, focus on the global context item), CALL_FUNCTION
  (arguments by position, arity = count). Template and tunnel parameters
  are two binding views. Fields are grouped by size (no padding).
- **Delivery**: `sxml_xslt_result_delivery` DEFAULT (by build-tree), RAW,
  TREE (sequence normalization, base URI = result URI), SERIALIZED (to an
  `sxml_output_stream`); the principal result is the call's output
  sequence (empty when serialized) and its URI the base output URI.
- **Result documents**: `sxml_xslt_result_document_handler` with `open`
  (request: href, absolute URI against the base output URI, effective
  output definition, processor; the handler fills a target: delivery and
  output stream) and `close` (URI, value, `is_abandoned`), called once
  per successful open; XTDE1490 is checked before open, a failing open
  (D50 statuses) stops the transformation. The null handler evaluates and
  discards. Default implementation and test double:
  `sxml_xslt_result_document_collector` (quintet; keeps values or bytes
  by URI, `_get_result_document_count`, `_get_result_document`, `_clear`)
  for the 4.1 driver's assert-result-document and the WebAssembly host.
- Left out, each a later field or function: a primed transformer for
  several invocations, a context item for the initial template other than
  the global context item, initial function arguments by name, the
  stylesheet's parameter list, xsl:message output other than the trace
  sink, warnings, host extension functions and instructions, schema
  awareness and streaming (D97).

### D113. Lowering completions (packet 2.5)
Recorded 2026-10-17, completing D63's "Not lowered in Phase 1" (only the
computed node constructors are left, for Phase 3). `src/xpath_lower.c`,
`src/xpath_static.c`; IR kinds, the evaluator and public headers are
unchanged: each construct lowers to existing IR plus library calls,
found by name like fn:root (`find_builtin_function`, now with a
namespace); a missing one is UNSUPPORTED_FEATURE naming the gap.
- **for member / for key/value** (XPath 4.0, 4.13.1): the binding
  collection is a MAP over the arrays or maps calling array:split or
  map:entries (their argument coercion gives the type error and accepts
  JNodes), then the ordinary MAP with the position slot, so positions
  count across all arrays or maps. The member is `item?1`; key and
  value are map:keys and map:items of the single-entry map. XQST0089
  for equal key and value names and for a position variable named like
  a range variable.
- **Destructuring let** (4.13.2): the value coerced to the declared
  type (default item()*, or one array(*) / map(*); with a declared type
  also coerced to the single array or map), then fn:items-at(V, i),
  fn:subsequence(V, n) for the last of n, `V?i`, or `V?name`; a
  declared record type without the variable's field binds a RAISE of
  XPTY0004.
- **Dynamic node tests** (4.7.5.3-4): the selector is lowered with an
  absent focus (reading it raises XPDY0002) and coerced to
  xs:anyAtomicType*; the step uses `*` of the axis's principal node
  kind, then a FILTER keeps a node when some selector item is
  fn:atomic-equal to one of its keys: fn:jkey of a JNode, fn:node-name
  and fn:local-name of an XNode.
- **The item axis** (4.7.4): per origin, a CONDITIONAL on
  count(fn:jvalue(G)): two or more items, the child step; none, (); one,
  the child step from the array `[jvalue(G)]` (a map or array origin is
  a JTree root). Deviation: that JNode's parent is the wrapper array's
  JNode, not G, and it sorts after G's tree, so parent/ancestor identity
  and mixed document order differ from the specification (QT4
  JAxes-370..372, 384, 387, 388). The faithful form needs an axis value
  in the evaluator (Proposed).
- **Constructor functions** (4.5.1.3): `find_function` gives constructor
  entries the parameter `$value as xs:anyAtomicType? := .`, so keyword
  arguments (`xs:int(value := '1')`) name it and an arity-0 query finds
  the constructor with a parameterless signature: `xs:int()` casts the
  context value and `xs:int#0` is a closure capturing it (body CAST).
- **Method calls** (4.15.5): a link of the chain: MAP over M, the item
  coerced to map(*), `?N` looked up and dynamically called with the map
  first; arguments lowered in the body with the outer focus (as "=!>");
  a placeholder is XPST0003.
- **but with** (4.15.4): a chain link: A coerced to record(*), B to
  map(*), map:merge((A, B), {'duplicates': 'use-last'}). Deviation: the
  merged map is not coerced to A's record type (the IR cannot name a
  type known only at run time); a later coercion checks the fields.
- **jnode() selectors** (3.2.12): decimal and double literals, negative
  numbers ("-" and the digits, built in the type frame's scratch) and
  QName literals (the EQName; in no namespace the local name, the form
  the evaluator's cast reads).
- The free-variable analysis sees every variable of a destructuring let
  (the former four-candidate limit is gone).
- Measured (release, XP40): 23,698 to 24,206 passed, none lost; slice
  198/198. prod-ForClause.map 7 to 56, .member 0 to 33, prod-LetClause
  67 to 137, prod-MethodCall 0 to 12, misc-ConstructorKeywords 5 to 153,
  misc-FocusConstructors 5 to 153, prod-ButWithExpr 0 to 10 (the rest
  need fn:parts-of-dateTime). Dynamic node tests and the item axis wait
  for fn:jkey and fn:jvalue (2.17): with them prod-AxisStep.J goes 30 to
  98 in a private run. prod-NamedFunctionRef's 72 failures are missing
  library functions, not references.

### D114. The regex library part (packet 2.3)
Recorded 2026-10-17, after D101 made parts files of their own.
- `src/function_library_regex.c` holds every function of F&O 4.0,
  section 6.3: fn:matches, fn:replace and fn:tokenize move there from
  the string part (removal only; no wave-0 packet owned that file),
  plus fn:analyze-string, fn:matching-segments and fn:regex. Named
  groups reach results: `$<name>` in replacement strings (FORX0004
  when no group has the name), name keys in the groups maps of
  fn:replace and fn:matching-segments, `name` attributes in
  fn:analyze-string.
- fn:analyze-string builds a parentless untyped element in an arena
  tree, as fn:json-to-xml does; fn:group elements nest by range and by
  the regex's own nesting of parentheses (new `sxml_regex_group_parent`,
  so `(b)(x?)` and `(b(x?))` differ, QT4 analyzeString-017/-017a);
  lookahead groups follow as fn:lookahead-group.
- fn:regex returns a map (no static context knows the named record
  types yet, so `fn:compiled-regex-record` assert-types fail) whose
  functions are anonymous function items of hidden rows capturing
  pattern and flags; their signatures reuse the resolved parameter
  types of fn:matches#3 and fn:replace#4 from the evaluation's function
  table. Each call compiles again (no cache).
- `tests/function_library_test.c` (owned by 2.17 later, by nobody now)
  names the regex part in its fixture so its regex cases keep running;
  new cases and an allocation-failure loop over the new functions.

### D115. Evaluator: JNode selectors, jkey and jvalue, fold fixpoints (packet 2.6)
Recorded 2026-10-17. Sections are those of the XPath 4.0 editor's draft
vendored with QT4 (PR 2734 for selectors).
- **fn:jkey, fn:jvalue** move from 2.17 to 2.6: new part
  `src/function_library_jnode.c` (F&O 4.0, 16.1.2, 16.1.3; `$input as
  jnode()? := .`). Nearly every prod-PathExpr.J and prod-AxisStep.J case
  uses them; 2.17 no longer writes them. fn:jtree stays in
  `src/function_library_json.c`.
- **Selector interpretation** (4.7.3, 4.7.5.4; replaces D85's "not
  done"): in a path's MAP, a JNode N with a non-navigational body (glossary
  `navigational`, decided once per MAP from the IR: ".", STEP, a path MAP,
  DOCUMENT_ORDER, fn:root/TREAT of "/", FILTER of such, union, sequence,
  conditional, let; tables of 48 entries, beyond that navigational as in
  Phase 1) evaluates the body and selects the children of N whose jkey is
  atomic-equal to an item of its atomized value; a body that reads the
  focus is XPDY0002 there. Task phase bits of the MAP keep the analysis.
- **Name tests on JNodes**: an NCName matches string/anyURI/untypedAtomic
  keys, a prefixed name or wildcard matches xs:QName keys. The IR does not
  tell `a` from `Q{}a` (both match strings; JAxes-292 waits for lowering).
  The attribute and namespace axes from a JNode raise XPTY0004 (4.7.5.1).
- **Lookups** on a JNode use the items of its jvalue (each a map or array).
- **QName key tests** (D113's proposal, accepted by the orchestrator):
  a jnode() key test of type xs:QName given as `Q{uri}local` compares the
  key's expanded name (names the pool never saw match nothing); other
  texts are cast as before. Untested end to end: lowering still refuses
  prefixed QName literals in jnode() (QT4 sequence-type-61). The other
  D113 proposal, SXML_AXIS_ITEM, needs a tree.h addition and lowering
  together; not taken here.
- **Numeric predicates** (4.4.1): a value whose first item is numeric
  selects by "= position()" (any item), an integer range by its bounds;
  a later non-number is FORG0006 (was FORG0006 for every second item).
- **Maps and arrays as typed functions** (3.2.9, 3.2.10): when the type
  annotation does not decide it, a map matches function(K) as R for K a
  single atomic type and R allowing () when every value matches R; an
  array function(I) as R for I a single integer type when every member
  matches R.
- **Fold fixpoints** (glossary `fixpoint`): fn:fold-left/-right and
  array:fold-left/-right stop early when the function is an inline
  function (through coerced functions whose accumulator parameter and
  result are item()*) of body `if (C) then $acc else E` (or the mirror),
  C a value or general comparison of numeric constants, captured values
  and $acc, and C holds for the next accumulator: every later call would
  return it (QT4 fold-left/right-022/023). `sxml_evaluator_test_fixpoint`
  is declared in `src/evaluator_internal.h` (one additive declaration
  outside the packet's files; 2.0 is done).
- **Not done here**: `/` and `/x` from a JNode need lowering to treat as
  `document-node()|jnode()` (4.7.1.1; 6 PathExpr.J cases; lowering);
  fn:function-lookup of focus functions (position#0, last#0) needs the
  focus at the call, which a leaf row cannot see (lowering);
  assert-eq on a JNode result needs the driver to atomize (2.16);
  `count(reverse(1 to 10^11))` needs a descending range representation
  (substrate, 2.17); the O(1) comparison of a range with an integer
  (RangeExpr-408h, -410h) is ready but waits for `tests/xpath_test.c`
  (see "Proposed"). fn-function-lookup stays at 85%: its other 85
  failures are functions other packets add.

### D116. Collations and comparison options (packet 2.13)
Recorded 2026-10-17. Completes D97's collations in `src/collation.c`
(`src/collation_internal.h`), used through one resolver.
- **Four collations**: codepoint, HTML ASCII case-insensitive, and also
  the Unicode case-insensitive collation (F&O 4.0, section 5.2.5, which
  every processor must recognize; D97 predates it), and UCA URIs with
  fallback. fallback=no, and a fallback value other than yes/no, is not
  supported (FOCH0002); unknown keywords and values are ignored, the last
  value of a keyword wins, `maxVariable`, `caseFirst` and `caseLevel`
  also in their `max-variable` spellings.
- **UCA fallback** (glossary `uca_fallback`): every codepoint's full
  compatibility decomposition gives collation elements: a base character
  primary = its lower-case codepoint + 1 (codepoint order without case),
  secondary common, tertiary 1/2 for lower/upper (swapped by caseFirst=
  upper) + 2 for a compatibility variant; a combining mark (combining
  class > 0) only secondary = 2 + codepoint. Variable characters
  (whitespace, then P, Sm/Sk/So, Sc up to maxVariable) are shifted to a
  quaternary weight or blanked. Levels: primary, secondary (backwards
  reverses it), case (caseLevel), tertiary, quaternary (shifted only),
  identical (codepoints). lang, version, normalization, numeric and
  reorder are ignored. QT4 fn-collation, misc-UCACollation and fn-compare
  pass with it; the tables of advanced-uca-fallback stay out (D97).
- **Keys and matching**: a key lists each compared level's nonzero
  weights in three bytes, levels separated by three zero bytes, so keys
  compare as bytes (`fn:collation-key` returns them as xs:base64Binary)
  and strings compare by keys. Substring matching (contains, starts-with,
  ends-with, substring-before/-after/-before-last/-after-last) compares
  collation units and accepts matches at codepoint boundaries only; the
  codepoint collation stays a byte search. distinct-values,
  duplicate-values, all-equal and all-different hash collation keys.
- **One resolver** (`sxml_function_library_find_collation` /
  `_resolve_collation`, `src/function_library.c`): empty is the default
  collation, which is the codepoint collation until the static context's
  `default_collation_uri` and `collation_bindings` reach the evaluator
  (Proposed); a relative URI resolves against the static base URI (none:
  FOCH0002); FOCH0002 only for URIs sxml does not support. Bodies outside
  the string, sequence and deep-equal parts keep
  `sxml_function_library_check_collation`, which now also raises
  FOCH0002 for supported collations other than codepoint (Proposed:
  `sxml_function_library_compare_collated`).
- **`sxml_collation_is_supported`** (D104) is defined in
  `src/collation.c`, not `src/xpath.c`: the lookup lives there and 2.12
  has not run; 2.12 must not define it again. The QT4 driver's collation
  check calls it; its dependency table now also lists
  unicode-normalization-form FULLY-NORMALIZED (accepting 2.4's proposal).
- **fn:deep-equal 4.0** (`src/function_library_deep_equal.c`): every
  option. A resumable body (D75) for `items-equal` (asked before each item
  pair, attributes and single text nodes included; a leaf entry stays for
  array:index-of). "ordered": false and `unordered-elements` are
  unordered cursors on the explicit stack: each unit tries the unmatched
  candidates, a difference inside a trial truncates the stack to the
  cursor and tries the next (first fit). Function items: same item, or
  same body, arity, name and signature with deep-equal captured values;
  separately written inline functions are not compared for equivalence
  (Proposed). Without schemas typed-values and type-variety change
  nothing; debug writes nothing; options are coerced (a node or
  xs:untypedAtomic for a boolean).

### D117. The XSLT 4.0 test driver (packet 4.1)
Recorded 2026-10-17. `tests/xslt/driver/`, the developer tool
`sxml-xslt-test`, public API only.
- **A sibling, not a mode of sxml-qt4**: the XSLT catalog has its own
  namespace, schema and semantics (dependencies as elements in
  `<dependencies>`, stylesheets, invocations, `assert` over the result
  document), so its catalog, applicability and commands are new modules;
  it links `sxml_qt4_driver_library` for documents (a document loaded by
  `sxml_qt4_document_load` with its catalog namespace switched to
  `http://www.w3.org/2012/10/xslt-test-catalog`), the QT4 environment
  reader (sources, params, resources, collections, collations, schemas),
  suite paths, test lists, reasons and the run report (spec "XSLT40").
- **Reading**: a ref looks in the test set, then catalog.xml; every
  `<dependencies>` child is a dependency whose type is its local name.
  Names (params, initial template, mode, function) are xs:QNames: a
  prefix bound by the element, Q{uri}local as an EQName, an unprefixed
  name in no namespace, `#default`/`#unnamed` kept as keywords.
  initial-function gives CALL_FUNCTION, else initial-template
  CALL_TEMPLATE (one case also names a mode: ignored, listed), else
  APPLY_TEMPLATES. `<output>` of `<test>`, else of the environment.
- **Applicability**: spec tokens `XSLTnn` / `XSLTnn+` (case replaces set;
  no spec = every version, as D83); other dependencies match their whole
  value against a table (`xslt_applicability.c`): features
  higher_order_functions, namespace_axis, serialization,
  built_in_derived_types, HTML5, XPath_3.1 (XSLT 3.0's optional 3.1
  support), streaming-fallback; English numbering, AD/ISO calendars,
  every year_component_values phrase, the normalization forms, UTF-8,
  HTML 5, on-multiple-match recover, package_version_resolution
  highest_version (until 4.16 decides), enable_assertions, codepoint
  collation, any maximum_number_of_decimal_digits. Not: dynamic_evaluation,
  disabling_output_escaping, numbering combinations, the non-goals. Then
  the environment (source validation, XInclude, XML 1.1 sources, schemas,
  unknown collations) and posture-and-sweep tests. Scope (D95): spec
  matches, group (directory under tests/) not strm or sandp, no required
  non-goal feature. Measured: 13,867 cases resolve, 8,387 applicable,
  8,728 in scope (D95: about 8,700).
- **Plan** (`xslt_test_run.h`): principal module = first non-secondary
  module of `<test>`, else of the environment, else the
  defines-stylesheet source (16 cases); a package is RESOLVED_PACKAGE,
  a stylesheet TEXT with base URI file:///SUITE-PATH; other modules are
  served by the resolver; static params go to the stylesheet
  configuration, others are stylesheet parameters; role "." is the
  global context item; delivery SERIALIZED (serialize="yes"), RAW
  (tree="no"), TREE (tree="yes"), else DEFAULT; base output URI from
  output file=, else file:///TEST-SET-DIRECTORY/output/TEST-CASE/;
  result documents to `sxml_xslt_result_document_collector`, messages to
  the trace sink. Assertion semantics for the checks are in
  `xslt_assertion.h` (`assert` with the result document as context item,
  $RESULT-VAR bound for raw results).
- **Run**: not applicable is not-run "skip: ..."; assert-warning or
  assert-posture-and-sweep anywhere is not-run "unsupported: KIND"; an
  unknown or malformed assertion or no principal module is an error; all
  else is not-run "XSLT not implemented" until 4.15 implements xslt.h.
  4.15 (or a follow-up) adds the execution: a processor per case with the
  QT4 limits, the plan applied, the assertions checked.
- **Slice**: `tests/xslt/slice.txt`, 154 in-scope applicable cases spread
  over the twelve core sets of the Phase 4 gate; ctest `xslt/slice`
  against `tests/xslt/slice-baseline.txt` (0 until XSLT runs);
  `xslt/list_slice` keeps them resolving.

### D118. Patterns: front end, match expressions, priorities (packet 4.4)
Recorded 2026-10-17. `src/xslt_pattern.c`, `src/xslt_pattern_internal.h`
(new module header), after D29, D52, D103, D111.
- **Pattern AST**: 19 kinds appended to `sxml_ast_kind`, one per
  `..._pattern` node type of `grammars/xslt_pattern` (root PATTERN), the
  field PATTERN and the flag OPEN (map pattern `*`); everything inside
  predicates, node tests, names and constants stays the XPath kinds. Built
  by `sxml_xslt_pattern_build_ast` (declared in `xpath_ast_internal.h`
  beside the other front ends) through the D111 parser part seam. Every
  syntax error is XTSE0340 (6.3.1), as is an outermost function call not
  binding to fn:doc#1, id#1-2, element-with-id#1-2, key#2-3, root#0
  (prefixed names checked when the match expression is built). 4.4 made
  these additive edits to `src/xpath_ast_internal.h`,
  `tests/xpath_parse_test.c` (names) and the `sxml` link line of the root
  `CMakeLists.txt` (`sxml_grammar_xslt_pattern`), as D111 foresaw.
- **Match expression**: a pattern becomes an XPath AST (root XPATH) of
  xs:boolean over the item as context value, built by one loop over an
  explicit task list (D71) and lowered like any XPath expression through
  the embedding entry (D103); no new IR kind. Copied subtrees keep their
  source spans (the output AST starts with a copy of the pattern's text
  store). Private variables are in namespace
  `urn:x-sxml:pattern-variable`. XNode patterns are read right to left
  (6.3.2.3): a step is a self test on the item, the steps to its left a
  condition on its parent ("/") or an ancestor ("//"); a step with
  predicates is selected again from its origin so positions count as in
  the formal definition; leading steps use child-or-top,
  attribute-or-top, namespace-or-top. Patterns with intersect/except, a
  filter on a parenthesized pattern or an RHS parenthesized pattern beyond
  steps and unions use the formal definition (some ancestor-or-self
  candidate $x with the item in $x/(EE)). Simple, atomic, map, record,
  array and JNode patterns test the item directly (6.3.3-6.3.9; sequence
  patterns: count, then every item).
- **Errors in patterns (6.5)**: a match expression may raise; every
  evaluator of one (APPLY_TEMPLATES 4.8, xsl:number 4.11, grouping 4.9,
  keys 4.10) treats a dynamic or type error as "no match". current() in
  patterns is the item being matched; 4.10 must provide it (the match
  expression keeps current() as written).
- **Match function**: `sxml_xslt_pattern_lower_match_function` adds a
  top-level IR function (item()) as xs:boolean whose fixed focus is its
  parameter, for template rules (callers outside a lowering machine, no
  locals in scope); inline uses lower the match expression themselves.
- **Default priority**: `sxml_xslt_pattern_compute_default_priority`
  (6.4, incl. the 4.0 union minimum, element/attribute table, atomic
  0.5 - 2^-N by derivation depth, map, record, array, JNode rules,
  `##any` through a flag).
- Suite: of 19,441 pattern attributes in xslt40-test, 19,288 parse; all
  of them build a match expression; 18,916 lower with only the
  stylesheet's namespaces, 372 need what a stylesheet adds (XSLT
  functions current/key/id/accumulators, 145; schema or user types, 227).
- Rejected: evaluating root(.)//(EE) for every pattern (quadratic per
  match); a lowering part over pattern kinds (would need the core's node
  test resolution, which is private to `xpath_lower.c`).

### D119. Number formatting: one picture engine, decimal formats as settings (packet 2.9)
Recorded 2026-10-17. `src/format.c`, `src/format_internal.h`,
`src/function_library_format.c` (part `format`).
- **One engine** for fn:format-integer and fn:format-number pictures,
  open to fn:format-date (2.14: `sxml_integer_picture_parse_primary_format_token`
  and `sxml_format_integer`), xsl:number and xsl:decimal-format (4.10)
  and XQuery's decimal-format declaration (`sxml_decimal_format_apply`
  checks a format's properties: MALFORMED_INPUT for a wrong form or
  markers that are not distinct; callers map it to FODF1290,
  INVALID_ARGUMENT, XQST0097/8 or XTSE1300). Pictures are scanned by hand
  (D97) and parse into plain values that borrow the picture string; the
  grouping positions are rescanned while formatting, so parsing needs no
  memory.
- **Numbers as decimal text**: integers and decimals exactly, doubles and
  floats by their shortest decimal form (F&O 4.0, 4.9.5) after the
  percent or per-mille multiplication in their own type (overflow gives
  infinity); rounding half to even on the digits, any size. A mantissa
  that rounds up to 10^N keeps its exponent (QT4 numberformat304).
  QT4 fn-format-number-limits-007 expects Java's "4.9e-324"; sxml gives
  the shortest "5.0e-324".
- **format-integer**: digit patterns in any Nd family and radix 2 to 36,
  a to z bijective (to 2^64-1), Roman 1 to 3999, English words below
  10^21 (British "and"), English ordinals for digits and words; every
  other token, and numbers out of a sequence's range, use the token "1"
  (D97: no other languages or sequences). Title case keeps "and" and the
  part after a hyphen lower-case ("Twenty-one").
- **Decimal formats**: the part starts from the defaults and applies an
  options map (keys by atomic-equal string, QName keys in a namespace
  ignored, others XPTY0004; values coerced to xs:string). The static
  context's formats (D104) do not reach the evaluator yet, so every format
  name is FODF1280 (Proposed below).
- Measured (debug driver): fn-format-integer 13 to 87 passed (all 87 run),
  fn-format-number 20 to 252 of 286 run; 33 of the 34 failures use QT4
  decimal-format environments.

### D120. Value templates (packet 4.3)
Recorded 2026-10-17, after D97, D102 and D111.
- **Grammar** `grammars/value_template` inherits `grammars/xpath`:
  `value_template` (the XPATH root) with field body
  `value_template_body` (STRING_TEMPLATE), parts
  `value_template_fixed_part` (STRING_TEMPLATE_FIXED_PART) and the XPath
  `enclosed_expression`. Its own `src/scanner.c` wraps the shared query
  scanner and appends two tokens after the shared list (which is
  untouched): the fixed part (5.6: `{{`, `}}`; a backquote and "(:" are
  text) and a zero-width start token, so the body exists for a
  zero-length template.
- **Entry** `sxml_xslt_build_value_template_ast` in a new
  `src/xslt_value_template_internal.h` (the packet named no header; the
  module's own). A parser part (D111) with three node types and a
  describe_payload hook that decodes the template's own fixed parts; a
  fixed part of an XPath string template inside a variable part keeps
  the XPath decoder (told apart by the body's last child, the variable
  part being walked). The AST equals that of `` `...` `` but for spans.
- **Errors**: the grammar decides acceptance (D12). After XPST0003 a
  lexical scan (fixed parts, string literals, comments, nested brackets
  and string templates; nesting up to 64 in one word) finds XTSE0370 (an
  unescaped `}` in a fixed part) or XTSE0350 (a `{` never closed), with
  the location of that bracket; otherwise XPST0003 stays.
- **Effective value** is the string template's lowering (D102): each
  variable part atomized and joined with spaces. 5.7.2 (simple content)
  also merges adjacent text nodes and drops zero-length ones first, so
  `{text{'a'}, text{'b'}}` gives "a b", not "ab": left to 4.6, which
  lowers sequence constructors.
- **Every grammar joins the link line of sxml** (`grammars/CMakeLists.txt`,
  a call deferred to the end of the root directory), so 3.2 and 4.4 need
  no edit of the root `CMakeLists.txt` for xquery and xslt_pattern (an
  explicit entry there is harmless).
- **Check**: `tools/qt4-parse-check.py --spec XT40 --value-templates`
  (scope of D95; attribute value templates from the element syntax
  summaries, literal result elements and shadow attributes; text value
  templates under expand-text, xsl:note merged away): 12,244 value
  templates, 12,232 accepted, 0 false rejects, 11 "either" (the 5 of
  error-0350a/b, error-0370a and cvt-013 rejected); 1 uses optional
  record fields `d?`, dropped from the XPath 4.0 draft. Every valid one
  also builds through the C entry.


### D121. The XQuery front end (packet 3.2)
Recorded 2026-10-17, extending D29, D59 and D111. `src/xquery_parse.c`
defines `sxml_xquery_build_ast`: the XQuery parser part on the XPath
walker; `sxml_grammar_xquery` joins the `sxml` link line.
- **Tables and tokens**: the 66 node types and 23 fields XQuery adds; the
  module's child takes field MODULE. `mode` and `kind` tokens become the
  keyword detail, a decimal-format property name its detail, the other
  token fields flags. The `empty` token of order_specification is claimed
  before the XPath rules, which read `empty` as the empty-sequence
  occurrence.
- **Decoding** (describe_payload): XQuery string literals and
  BracedURILiterals replace references; direct element text also "{{"
  and "}}"; attribute value text also the doubled delimiter, read from the
  DIRECT_ATTRIBUTE_VALUE node (the first node containing the text on the
  parent chain of the last added node), then attribute value normalization
  (literal tab, line feed, carriage return to a space). Every decoded text
  (also CDATA, comments, PI content, pragma contents, string constructor
  characters and string template fixed parts) gets the end-of-line
  handling of A.3.3.1. A reference to a character outside XML 1.0 Char,
  or above U+10FFFF, is XQST0090. "(# r #)" has an empty PRAGMA_CONTENTS.
- **Checks** (finish_node): XQST0118 and XQST0040/XQST0071 on names as
  written (pairwise up to 32 attributes, else a heapsort of node indices
  in one work array from the AST's allocator); namespace declaration
  attributes are flagged, XQST0022 for an enclosed expression, XQST0070
  for the reserved bindings of 4.12.1.2, XQST0085 for a prefix bound to ""
  (XML Names 1.0, xgc: xml-version); BOUNDARY_WHITESPACE on text written
  as whitespace only with no CDATA section or text next to it; a version
  other than 1.0, 3.0, 3.1, 4.0 is XQST0031, an encoding that is no
  EncName XQST0087; a string field name in a record declaration annotated
  `%constructor` (unprefixed or Q{http://www.w3.org/2012/xquery}) is
  XPST0003, the third D29 false accept. A direct comment ending in "-" is
  already rejected by the scanner.
- **Left to later packets**: duplicate expanded names under different
  prefixes (XQST0040, lowering), the prolog's XQST0070 for namespace
  declarations, imports and default namespaces and its duplicate
  declarations (3.10), a prefixed `%constructor`.
- **Results** (a scratch harness over the case loader of
  `tools/qt4-parse-check.py`, which itself checks only the grammar): all
  43,663 valid XQ40 queries build, 70 of them with the static error their
  test expects (XQST0022 16, XQST0031 5, XQST0040 4, XQST0070 8, XQST0071
  9, XQST0085 5, XQST0087 4, XQST0090 13, XQST0118 6); the 145 parsable
  library modules build; all 772 XPST0003 queries fail statically.

### D120b. The XML and XHTML output methods (packet 3.3)
Recorded 2026-10-17 as a second D120 (4.3 has D120); renamed at the
Phase 2 gate (D151). `src/serialize_internal.h` splits the serializer
into parts: `src/serialize.c` keeps the output stream and buffer, the
parameter checks, the character pipeline, sequence normalization, the
adaptive method and the dispatch; `src/serialize_xml.c` has the XML and
XHTML methods and the node writer the adaptive method uses. Packet 3.4's
HTML, text, JSON and CSV parts build on the same internal interface.
- **Dispatch** (D106 follow-up): HTML, text, JSON and CSV return
  UNSUPPORTED_FEATURE until 3.4; parameters C can say but XDM cannot
  (enumerations out of range, ill-formed UTF-8, name 0, a character map
  codepoint twice or not an XML Char) are INVALID_ARGUMENT for every
  method; indent-unit, doctype-system, doctype-public and html-version
  values the spec forbids are SEPM0016.
- **Character pipeline** (Serialization 4.0, section 4): `sxml_serializer`
  writes markup, text, attribute values, URI attribute values and CDATA
  sections. Text and attribute values go through the character map
  (sorted once, binary search), then `sxml_unicode_normalize` (D107; the
  serialize.h form maps onto unicode_internal.h's, fully-normalized as NFC
  plus SERE0012 for a construct that starts with a composing character)
  on each unmapped segment, then escaping, line endings and the encoding
  into a 512-byte staging area. References are `&#xHEX;`; C0 and C1
  controls, NEL and LINE SEPARATOR are always references in text and
  attributes (also with version 1.0: XML 1.1's repertoire, QT4
  K2-Serialization-7, Serialization-xml-27 to -31); a character the
  encoding lacks is a reference there and SERE0008 in markup. UTF-16 is
  big endian; byte-order-mark writes EF BB BF or FE FF.
- **Sequence normalization** is a pull iterator
  (`sxml_sequence_normalizer`) that never builds the result tree: frames
  flatten arrays and JNode values, atomic items are cast to text joined
  by a space or the item separator, documents give their children, text
  is merged in a heap buffer. SEPM0004 (doctype-system or standalone with
  text or several elements at the top) runs the iterator once more first.
- **Namespaces**: each element's declarations are computed against a
  stack of written bindings: its in-scope namespaces (minus xml) plus what
  its own and its attributes' names need, declared where the written
  bindings differ, `xmlns=""` where a default goes out of scope, and
  `xmlns:p=""` only with undeclare-prefixes and version 1.1 (SEPM0010
  with 1.0). XHTML with HTML5 applies prefix normalization (XHTML, SVG,
  MathML elements unprefixed; their prefixes dropped unless an attribute
  of an ancestor-or-self uses them, Issue 2821). Canonical XML uses the
  same rule (it is C14N 1.1's namespace axis) and sorts declarations and
  attributes by heapsort (no recursion, O(n log n)).
- **Indentation** (5.1.4, 6.1.5): an element's content is indented only
  when it has element, comment or PI children and no text other than
  whitespace, its name is not in suppress-indentation (XHTML: also
  without regard to case), it has no xml:space="preserve", it is not a
  formatted or inline HTML element and has no inline child, and its
  parent is indented itself, so nothing below mixed or suppressed content
  moves. Whitespace-only text is dropped where whitespace is added.
  indent-unit defaults to two spaces; indent-attributes puts each further
  attribute on its own line. Newlines between top-level nodes and after
  the declaration only when indenting; none otherwise (QT4
  Serialization-xml-01, -xhtml-72).
- **XHTML** (section 6): html-version defaults to 5.0; minimized tags only
  for elements expected to be empty (`<br/>` with HTML5, `<br />`
  before), `<p></p>` otherwise; `<!DOCTYPE html>` before an html document
  element with HTML5 and no doctype-system; include-content-type (default
  yes) adds `<meta charset="ENC"/>` (HTML5) or the http-equiv form (media
  type text/html by default) as head's first child and drops existing
  charset or Content-Type meta children; escape-uri-attributes (default
  yes) applies URI escaping to the attributes of appendix D.
- **Canonical XML**: UTF-8, LF, no declaration, doctype, CDATA, character
  maps or indentation; start-end tag pairs; line feeds around nodes
  outside the document element; SERE0024 for top-level text, more or
  fewer than one element, or a relative namespace URI.
- **Adaptive nodes** (D82) are written by the same XML writer with the
  default parameters; a stored parentless namespace node (D98) is
  written from its name (the prefix) and content (the URI).
- Not done here: `sxml_serialization_parameter_reader` (declared by D106)
  has no definition yet; it belongs with fn:serialize (3.4). Not checked:
  SERE0005/SERE0006 (names and characters of the XML version), comments
  with "--" (SERE0003). The CLI's `--method` stays adaptive until 3.12.

### D122. Date and time functions (packet 2.8)
Recorded 2026-10-17. New part `src/function_library_temporal.c`
(test `function_library_temporal`): adjust-dateTime/date/time-to-
timezone, civil-timezone, days-in-month, parts-of-dateTime,
build-dateTime, seconds, duration-to-seconds, unix-dateTime and
parse-ietf-date. `src/operator_temporal.c` is unchanged.
- **Gregorian components** (PR 1481): the seven `*-from-dateTime` rows
  of `src/function_library_context.c` (2.12's file, edited because no
  other packet runs on it; two hunks) take the union of the eight
  Gregorian types and give the empty sequence for a component the type
  lacks (absent components are zero in the datum).
- **Shifts**: a dateTime or date moves by whole minutes through
  `sxml_operator_make_temporal_value` (D72: every int64_t year, FODT0001
  beyond); a time moves within its day. The implicit timezone and the
  clock come from the dynamic context, read once per evaluation (D88).
- **build-dateTime** declares `map(*)?`: the row text cannot spell the
  record's optional fields, so the body coerces each field by XPath 4.0,
  3.4.3: xs:untypedAtomic cast, an integral xs:decimal relabeled to
  xs:integer, xs:double/xs:float cast to xs:decimal (seconds only), an
  xs:duration without months relabeled to xs:dayTimeDuration; other
  types, extra keys or two items are XPTY0004. Then FODT0005 (field
  set), FORG0001 (ranges, gMonthDay against 1972), FODT0003 (timezone).
  parts-of-dateTime returns a plain map with the record's keys in order.
- **Seconds** keep nine fraction digits, truncated like casts (D36), so
  `seconds(3.14159265358979323846) eq xs:dayTimeDuration('PT3.14159265358979323846S')`;
  fn:seconds builds the duration from the decimal's digits (FODT0002
  beyond int64_t seconds), not by duration arithmetic, which rounds.
- **unix-dateTime**: milliseconds beyond uint64_t are FODT0001.
- **civil-timezone** (D97, no database): no place gives the implicit
  timezone (the default place is taken to be one whose civil timezone it
  is); "UTC", "Etc/UTC", "GMT", "Etc/GMT" give PT0S; any other place
  FODT0004.
- **parse-ietf-date**: hand-written scanner of the grammar of 9.10.1
  (D97), ASCII case-insensitive, longest names first; a tzoffset of one
  to four digits is H, HH, HMM, HHMM, with a colon minutes are 0 or 2
  digits; hour 24 only as 24:00:00 (next day); FORG0010 otherwise.
- **Open**: fn-parts-of-dateTime 8/20 (assert-type needs the named
  record type, Proposed); civil-timezone 4/9 (D97); days-in-month-006/007
  need fn:format-integer (2.9).

### D123. CI is disabled for now; gates are checked locally
Recorded 2026-10-17 at the owner's word ("CI is disabled, we don't need it at
this point"). Gate boxes that name CI are satisfied by the same checks run
locally (all presets, lint, slice, conformance runs, fuzzing). macOS, Windows
and MSVC stay unverified until CI returns; that remains a known risk.

### D124. IR extensions for try/catch and constructors (packet 3.6b)
Recorded 2026-10-17; accepts the IR proposals of D109 and D110
(`src/ir_internal.h`, `src/ir.c`, `src/ir_verify.c`).
- **Adders**: `sxml_ir_unit_add_name_tests(unit, sxml_ir_name_test_view,
  &list)`, `sxml_ir_unit_add_catch_clauses(unit, view, &list)` and
  `sxml_ir_unit_add_namespace_bindings(unit, view, &list)` work as
  `sxml_ir_unit_add_sort_keys`, with accessors `sxml_ir_unit_get_name_tests`,
  `_get_catch_clauses`, `_get_namespace_bindings`. A LOCAL_NAME_WILDCARD
  test's local name is interned in the item heap's name pool (pool text
  never moves), so callers may free theirs; an empty one is
  INVALID_ARGUMENT. The static namespaces are one run of the same array.
- **`sxml_ir_node_construction`** gains `name_namespaces` (a binding run;
  empty means the unit's static namespaces; lowering gives the static
  namespaces extended by enclosing direct constructors, for QT4
  Constr-compelem-compname) and `uint32_t construction_flags`
  (`SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES`, `_NO_INHERIT_NAMESPACES`,
  `_PRESERVES_TYPES`, `_XSLT_RULES`; 0 = construction strip,
  copy-namespaces preserve, inherit, XQuery rules). Lowering (3.9, 3.10)
  sets them per constructor from the prolog; XSLT (4.x) sets XSLT_RULES on
  literal result elements and xsl:element/xsl:document.
- **Evaluator** (`src/evaluator_construct.c`): flags pick the copy request
  of content nodes (no-preserve, no-inherit, keep annotations), xs:anyType
  for elements under PRESERVES_TYPES, XSLT rules (REPLACE duplicates, XTDE
  codes) for ELEMENT and DOCUMENT; SHALLOW_COPY drops the element's
  namespaces under NO_PRESERVE (copy-namespaces "no") and keeps its type
  under PRESERVES_TYPES. Names given as strings resolve with
  `name_namespaces`. `src/evaluator_try.c` reads clauses through the
  accessors.
- **Verifier**: TRY_CATCH needs a body and clauses or a finally body;
  each clause one or more name tests of a known kind, an error details
  slot of the function (bound in its handler only) and a handler. The
  constructor kinds and SHALLOW_COPY: fields the kind does not use must be
  empty (kinds as in the header), lists in range, known flags; a NONE name
  or value is allowed (the empty sequence: `attribute {()} {}` raises at
  run time, `namespace {()} {...}` is the default namespace). Only
  APPLY_TEMPLATES stays reserved.
- **Text form**: constructors print name, value, then `(namespaces p="u"
  ""="d")`, `(name-namespaces ...)` and the flag words on one line, then
  the content; TRY_CATCH prints body, `(catch (TESTS) $SLOT HANDLER)` per
  clause and `(finally BODY)` (golden text in `tests/ir_test.c`). Dumps of
  the other kinds are unchanged.
- Test units of `tests/evaluator_try_test.c` and
  `tests/evaluator_construct_test.c` use the adders; all of them pass the
  verifier (checked once in a scratch build, after giving
  `shallow_others` one content expression per copy: the IR is a tree).

### D125. fn:divide-decimals and the precision it supports (packet 2.7)
Recorded 2026-10-17. `sxml_decimal_divide_to_precision` (`src/decimal.c`),
part `numeric` row `divide-decimals`.
- **Method**: the divisor's power of ten is shifted by -precision (exact,
  no digits move) and libmpdec's truncating `mpd_qdivint` and `mpd_qrem`
  give quotient * 10^precision and the remainder value - quotient *
  divisor in one exact step each (F&O 4.0, section 4.3.1); the quotient
  has the sign of value / divisor, the remainder that of the value.
- **Precision range** (the rules' "adjusted to the nearest value
  supported"): a positive precision is lowered until quotient and
  remainder both fit `maximum_digit_count` (bounds from the adjusted
  exponents and the divisor's digits), to 0 at least; one below every
  digit of the quotient is raised to just below it (the quotient is 0
  either way). So `divide-decimals(10, 3, 100_000)` gives 9,997 decimal
  places at the default limit; an integer quotient wider than the limit
  is FOAR0002; a zero divisor FOAR0001.
- **Result**: a map with keys "quotient" and "remainder" (xs:decimal),
  row result type `record(quotient as xs:decimal, remainder as
  xs:decimal)`. It is not annotated with fn:division-record: no static
  context knows the F&O named record types yet (the Proposed item of D122; QT4
  divide-decimals-019 to -021 and fn-random-number-generator-35a to -37a
  wait for it).

### D126. Lowering XQuery expressions: FLWOR, typeswitch, switch, try/catch (packet 3.7)
Recorded 2026-10-17, completing D55 for these kinds. `src/xquery_lower.c`
is lowering part `xquery` (SXML_LANGUAGE_XQUERY only, D103); no IR kind
added. Contract in `src/xquery_lower_internal.h`.
- **FLWOR** replaces the core's FLWOR for XQuery. Clauses between two
  tuple stream clauses are a clause segment: for = MAP (position slot,
  ALLOWING_EMPTY), let = LET (destructuring as D113), where =
  CONDITIONAL, one frame per binding or clause. A segment ending at a
  tuple stream clause returns tuple arrays (one member per live slot
  variable, innermost of each name; constants and globals are not
  packed); then order by = SORT with each key lowered over the members
  unpacked from the SORT's focus, count = the position slot of the
  unpacking MAP, while = fn:take-while with a closure
  fn($tuple, $position) { boolean(C) } (no condition evaluated after the
  first false). The closure captures every visible variable the condition
  references (overcapture is harmless) and, only when lowering it read
  the focus, the FLWOR's focus parts (three slots kept after the
  captures, bound by LETs at the closure's start). A MAP over the result
  unpacks the members with LOOKUP before the next segment.
- **order by**: empty least unless written (the prolog default is
  3.10's); collation literals resolved against the static base URI;
  codepoint is NONE, html-ascii-case-insensitive and UCA URIs are kept
  (the evaluator's SORT still raises FOCH0002 for them, D116 Proposed),
  anything else is XQST0076. "stable" changes nothing (SORT is stable).
- **trace** clause: lowered (static errors stand) as the never-taken
  branch of a CONDITIONAL; trace output is implementation-defined and
  its errors must not fail the FLWOR (4.13.10).
- **typeswitch**: operand once; CONDITIONAL chain over INSTANCE_OF (a
  union case is a CONDITIONAL of the type tests); case variables bound to
  the operand. **switch**: switch value = coerce(fn:data(comparand),
  xs:anyAtomicType?) or true(); a case operand matches when its atomized
  value is empty and the switch value is, or some item is fn:deep-equal
  to it; each case's tests sit in the previous case's else branch (2.6.5).
- **try/catch**: TRY_CATCH (D109, D124); unprefixed error names use the
  default element namespace; `$err:` variables the handler references
  are LOOKUPs of the details slot, `$err:map` the slot, stack-trace and
  additional `()`.
- ordered/unordered: their content. Extension expressions: pragma names
  must resolve (XPST0081); every pragma is ignored; none and no
  expression is XQST0079. validate: XQST0075 (D97). String constructors:
  fn:string-join of the parts, interpolations string-join(E, " ").
- **Entry points** `sxml_xquery_lower` / `sxml_xquery_compile` (internal,
  for tests and drivers until 3.12): a main module without prolog
  declarations (UNSUPPORTED_FEATURE otherwise, 3.10). Group by and the
  windows are UNSUPPORTED_FEATURE; 3.8 adds them as tuple stream clauses
  in `finish_segment` (the tuple layout is in the FLWOR frame's locals).
- Measured with a scratch harness (the QT4 driver with the XPath API
  compiling through `sxml_xquery_compile`), XQ40: every failure left in
  prod-ForClause, LetClause, WhereClause, OrderByClause, CountClause,
  WhileClause, TypeswitchExpr, SwitchExpr, ExtensionExpr,
  StringConstructor is a node constructor (3.9) or a prolog (3.10), but
  for three (fn:message missing, `local:` undeclared, one driver
  assertion).

### D128. The HTML, text, JSON and CSV methods; fn:serialize (packet 3.4)
Recorded 2026-10-17. Every method runs on the 3.3 pipeline (D120b); no
second writer or escaper was added.
- **Escape styles**: `sxml_escape_style` grows HTML, NONE (text), JSON and
  CSV; a method picks one in `sxml_serializer_resolve_pipeline`, so
  character maps, normalization, line endings and the encoding apply to
  all of them in one place. A new character context RAW_TEXT (glossary
  `raw_text`) writes mapped, normalized, unescaped text (HTML script and
  style; SERE0008 for an unencodable character). Character maps now apply
  to every method but adaptive and the canonical forms (section 12).
- **HTML** is a mode of the XML node writer in `src/serialize_xml.c`
  (`is_html`, with `has_html_rules` for what XHTML and HTML share);
  `src/serialize_html.c` holds what HTML adds: void and HTML 4 empty
  elements (one table, also used by XHTML), boolean attributes (the HTML
  4.01 and HTML5 lists), raw text elements, processing instructions
  (`<?t d>` before HTML5, SERE0015 for ">"; `<!--?t d?-->` with HTML5).
  Requested version: html-version, else version, else 5.0. No-namespace
  elements are HTML elements (unknown names inline, as span); with HTML5
  also XHTML-namespace ones (unprefixed, prefix normalization of D120b);
  others are XML islands. Doctype `<!DOCTYPE html ...>`. SERE0014 for
  controls before HTML5.
- **Text and CSV** (`src/serialize_text.c`): text writes the string value
  of the result tree with escape style NONE. CSV takes a map (rows,
  columns) or a sequence of arrays; members atomize (one-member arrays
  unwrapped, nodes by string value), else SERE0025; a field is quoted when
  it, or a replacement of one of its mapped characters, holds the
  separator, the quote or a line end; separator equal to the quote or a
  line end is SEPM0016.
- **JSON** (`src/serialize_json.c`): frames on a heap stack (D71); nodes
  by a sub-serialization (`sxml_serializer_write_node_as_string`, which
  calls only the XML and text methods, so no cycle); doubles by their
  xs:double cast text, NaN null, infinities 1e9999 (SERE0020 when
  canonical); canonical: keys sorted by UTF-16 units (heapsort), numbers
  as ECMAScript from the shortest digits; SERE0022 checked by sorting
  only when some key is not string-like; indentation off with json-lines.
- **SERE0006** (left by D120b): XML and XHTML with version 1.0 (the
  default) raise it for C0 controls in text and attributes; 1.1 writes
  references. **SERE0003** for comments with "--" or a final "-" and PIs
  with "?>" (XML, XHTML). A character map may map any XML 1.1 Char
  (U+0001 included; serialize.h says XML 1.0's production, D120b already
  writes the 1.1 repertoire).
- **Parameter reader** (`src/serialize_parameters.c`): both forms decode
  each parameter into one value and assign it the same way. Two arenas:
  a read fills the spare one and swaps on success. Map: unknown string
  keys are XPTY0004 (F&O 4.0 option conventions, QT4 serialize-xml-107-40,
  deviating from serialize.h's "ignored"), a QName key in no namespace
  XPTY0004, in a namespace ignored; nodes atomize for character map
  values. Document: unknown output:* children, attributes other than
  value (character-map: character, map-string), non-whitespace text and
  children in no namespace are SEPM0017; two children with one name
  SEPM0019; element lists use the default namespace (the spec's example),
  method names do not. fn:serialize (`src/function_library_serialize.c`)
  skips the encoding phase: UTF-16 is written as UTF-8, ISO-8859-1 bytes
  decoded, no byte order mark.
- Not done: SERE0005 (names of the XML version), the text of nodes inside
  the JSON method beyond the four node methods, records omitting empty
  entries in JSON (no record marker on maps).

### D127. Lowering node constructors (packet 3.9)
Recorded 2026-10-17, completing D110 and D124 for XQuery.
`src/xquery_lower_construct.c` is lowering part `construct`
(SXML_LANGUAGE_XQUERY, D103) for the DIRECT_* and COMPUTED_* kinds; no IR
kind or header change.
- **Direct elements**: ELEMENT_CONSTRUCTOR whose content is a SEQUENCE of
  one atom per attribute and piece of content in order (D110); literal
  text and CDATA are xs:string constants; boundary whitespace is dropped
  unless the prolog says boundary-space preserve. Names are xs:QName
  constants (prefix kept) resolved while lowering (XPST0081); two
  attributes with one expanded name are XQST0040.
- **Namespace declaration attributes** extend the static context through
  the embedding entry (D103), not new machinery: the element's attributes
  and content are lowered with a copy of the static context whose
  bindings are its declarations followed by the current ones, so the core
  resolves names inside enclosed expressions (and nested constructors)
  with them. An element, direct or computed, sends as namespace bindings
  the declarations of its own and every enclosing direct constructor
  (innermost per prefix, found by walking the frame chain), so they are
  in scope whatever copy-namespaces says; a computed name inside one gets
  the extended bindings as name_namespaces. A computed namespace
  constructor binds no prefix statically.
- **Attribute values**: "" for none, the constant for text alone, the
  enclosed expression's atom alone (the evaluator joins with spaces);
  mixed parts are joined with `||`, each enclosed part as a
  TEXT_CONSTRUCTOR of its value (atomized and space-joined, empty gives
  ""), so no library call is needed.
- **Computed** constructors: a literal name is a constant (xs:QName for
  element and attribute, xs:string for PI target and namespace prefix),
  a name expression an atom; content is the atom of the enclosed
  expression (value, or an ATOM content expression for element and
  document). Direct comments and PIs are constructors of constants.
- **Prolog setters** (boundary-space, construction, copy-namespaces) are
  read from the module's PROLOG node and become construction_flags of
  element and document constructors; they take effect once 3.10 lets
  prologs through `sxml_xquery_lower` (D126 rejects them now).
- Results (scratch harness: xpath.c compiling with sxml_xquery_compile,
  as 3.7's): the 13 constructor test sets 0 to 932 of 1,165 passed, 59
  not run (dependencies); of the 174 failures and errors, 167 are queries
  with a prolog (3.10). The 7 others are not lowering's: URI whitespace of
  "Q{ _   _ }x" in the evaluator (2), `local:` not predeclared (3.10,
  3.12), `text {''}` (the D110 deviation) and a missing function (3).

### D129. Date formatting: the picture engine over format-integer (packet 2.14)
Recorded 2026-10-17, after D97, D119 and D122. `src/format_date.c` with
the module header `src/format_date_internal.h`, part
`src/function_library_format_date.c` (part `format_date`), test
`format_date`.
- **One engine**: a variable marker's first presentation modifier is a
  primary format token of the D119 engine; a minimum width widens the
  digit pattern (optional signs from the right, then prepended digits of
  the family) and parses it again; non-digit tokens are padded with
  spaces. Fractional seconds parse the reversed token (so `[f#99]` is
  FOFD1340) and write the fraction's digits directly with the effect of
  9.9.4.5's reversal (`[f,*-2]` of .006 is "0", as QT4 expects).
- **Scan**: three passes over the picture (syntax FOFD1340, then
  components FOFD1350, then writing), so a rejected picture writes
  nothing. Whitespace in a marker is dropped; the last comma starts the
  width; a lone "]" is FOFD1340.
- **Calendar arithmetic** on a year of the same 400-year cycle, so day of
  week, ISO week (W) and week in month (w, the month of the week's
  Thursday) hold for every int64_t year; W, w and F's numbers follow
  ISO 8601 in both calendars.
- **Calendars and languages** (D97): AD (default; year 0 is 1 BC, eras
  AD/BC) and ISO (absolute year, era "-"). Other designators and any
  name in a namespace format as AD after "[Calendar: AD]"; a name in no
  namespace that is no designator, or no EQName, is FOFD1340. A prefix is
  not resolved (no statically known namespaces at run time): any prefix
  counts as a namespace. A non-English $language gives "[Language: en]";
  the empty sequence or "" gives none (the default language is not read:
  English whatever the host names). $place is ignored.
- **Names**: English months and days, conventional day abbreviations
  (Tues, Weds, Thurs/Thur) before cutting at the maximum width; am/pm;
  AD, BC, ISO always in capitals. Title-case words capitalize after a
  hyphen ("Twenty-One", QT4 format-date-en125), unlike format-integer's
  "Twenty-one" (D119). Timezone names need a database (D97): [ZN] and
  other tokens use 01:01; ZZ gives military letters, "J" without a
  timezone.
- Measured (debug driver, XP40): fn-format-date 20 to 124 of 124 run,
  fn-format-time 19 to 94 of 94, fn-format-dateTime 5 to 80 of 80.

### D130. Elements to maps and back (packet 2.10)
Recorded 2026-10-17. `src/function_library_element_map.c` (part
`element_map`) implements fn:element-to-map-plan, fn:element-to-map and
fn:map-to-element (F&O 4.0, section 14.6).
- **No recursion, no new machinery** (D71). fn:element-to-map keeps one
  element slot per node of the subtree and walks the node indices twice:
  in document order it chooses layouts (a child is converted only when its
  parent's layout converts children, so a plan's `error` below an `xml`
  element is never reached), in reverse document order it builds values,
  whose children are then done. fn:element-to-map-plan walks the same
  ranges once. fn:map-to-element keeps a stack of frames and sends tree
  builder events to a new arena tree that destroys itself (D40, D110);
  the `xml` layout parses through the document loader and copies with
  `sxml_tree_builder_copy_node`. Grouping, duplicate keys and plan lookup
  use XDM maps (D37) as indices, nothing of its own.
- **Untyped input only**: sxml is not schema-aware, so the schema-based
  layouts (14.6.6) and the nilled forms on the element-to-map side never
  apply; xsi attributes are ignored, and plan analysis skips
  xsi:nil="true" elements as 14.6.4 says.
- **Choices the text leaves open**, following QT4: without a plan entry,
  attributes stay xs:untypedAtomic while simple element content gets the
  inferred type (14.6.3, `<a x="1">2</a>` gives `{"a":{"@x":"1",
  "#content":2}}`); a cast failure under a prescribed type is FOJS0010
  even with a fallback; a leading zero after an optional sign makes a
  string. In fn:map-to-element, matching a value against a layout looks
  one level down only (list members against the child name's candidates
  by form), so it needs no recursion; elements in a namespace from
  `Q{uri}` keys use the default namespace, attributes made-up prefixes
  `ns0`, `ns1`, ...; prefixes of `p:local` keys resolve against the static
  namespaces of the innermost unit (as the static base URI, D91).
- QT4: fn-element-to-map 179/181 run, fn-element-to-map-plan 20/20,
  fn-map-to-element 223/228. Open: element-to-map-101 and -507 assert
  `?a?…` on the context value, which the driver does not set
  (XPDY0002); map-to-element-014 and -021 expect XQuery's decoding of
  `&amp;` in string literals under XP40; -120 and -121 need fn:serialize.

### D131. Resources, the evaluation environment, lexical decimal formats and collations (packet 2.12)
Recorded 2026-10-17. Implements D104 in `src/processor.c`,
`src/processor_internal.h`, `src/xpath.c`, new part
`src/function_library_resource.c`; accepts three Proposed items (2.6's
range fast path, 2.9's decimal formats, 2.13's default collation).
- **Evaluation environment** (glossary): what library functions read from
  the host beyond fn:doc, carried by the processor's document loader of
  each evaluation: resolver (dynamic context's, else processor's), trace
  sink (same), default language ("en" when empty), environment variables.
  Natives reach it with `sxml_processor_get_evaluation_environment(dynamic_context)`
  / `sxml_processor_document_loader_from_dynamic_context`; NULL for a
  dynamic context the processor did not make (unit tests): nothing
  available, no variables, "en", trace discarded. The loader also
  retrieves TEXT/BINARY bytes (`_retrieve`/`_release`, BYTES only, else
  INVALID_ARGUMENT) and keeps collections per kind and absolute URI for
  the evaluation (fn:collection is deterministic; SEQUENCE only).
- **Text decoding** (F&O 4.0, 17.1.5, after bin:infer-encoding): candidate
  = resource's charset, else the encoding option; no candidate: BOM
  decides, else UTF-8 (bad bytes FOUT1200); a candidate skips a BOM that
  agrees ("UTF-16" without BOM is big-endian) and decodes a disagreeing
  one as characters (so it fails, FOUT1190); UTF-8, UTF-16LE/BE,
  ISO-8859-1, US-ASCII (the XML parser's names). Non-XML characters are
  FOUT1190; `fallback` gives U+FFFD; `normalize-newlines` maps CR LF and
  CR to LF; -lines always normalizes and rejects that key (XPTY0004,
  QT4 40-014); -available is false for FOUT1170/1190/1200. fn:json-doc is
  the text (any codepoint) handed to the fn:parse-json row, found by name
  in the XPath part list, with its options. No media-type handling.
- **fn:trace and fn:message** stay in the error part (the library tests
  list parts explicitly); both nondeterministic and context-dependent.
  `sxml_stream_trace_sink` writes `label: value` lines (adaptive, " ").
  `sxml_collation_is_supported` is 2.13's (D116).
- **Static context checks** (xpath.c): decimal formats (kinds once,
  `sxml_decimal_format_apply`, names distinct), collation bindings
  (absolute, not already known, target supported), default collation
  known; dynamic context: xs:language, distinct UTF-8 environment
  variables. Breaches are INVALID_ARGUMENT.
- **Lexical decimal formats and collations**: `sxml_ir_static_context`
  (decimal formats, default collation URI, bindings) is copied into the
  unit by `sxml_ir_unit_set_static_context`, set by lowering from the new
  `sxml_xpath_static_context.unit_static_context`;
  `sxml_dynamic_context_get_running_unit` gives the unit of the innermost
  IR task (as the static base URI). fn:format-number resolves names
  (xs:QName; string QName with the unit's static namespaces, EQName,
  NCName, spaces trimmed; FODF1280 otherwise), the named or unnamed
  format then the map's properties; the collation resolver tries the
  unit's bindings first and its default for "", fn:default-collation
  returns it.
- **Range fast path** (2.6's patch) in `src/evaluator.c`; the step-bound
  test uses a quantifier (a concatenation of a long range is not charged
  up front either: `0 = (1 to 10^11, 2)` runs unbounded; left as found).
- Measured (release, XP40): fn-unparsed-text 54 to 94/94 run,
  -lines 39 to 58/58, -available 7 to 57/57, unparsed-binary 16 to 21/21,
  json-doc 36 to 73/77 (4 need a fallback function), collection 18 to
  24/25, uri-collection 7/7, environment-variable 6 to 12/12,
  available-environment-variables 2 to 10/10, default-language 0 to 5/5,
  message 3 to 14/14, trace 31/31, format-number 252 (2.9) to 285/286.
  "Before" counts are at the packet's start (24fc6ac).

### D132. Packets start when their dependencies are committed (amends D96)
Recorded 2026-10-17 by the orchestrator, toward the owner's goal of working
XQuery and XSLT. A packet of a later phase may start as soon as every packet
in its "After" list is committed, whether or not it is marked "Early". The
phase gates still close each phase: a phase is done only when its gate review
passes, and gate reviews re-check the work of later-phase packets that
started early. First applied to 3.8, 3.10 and 3.13.

### D133. The built-in record types of F&O 4.0 (packet 2.6b)
Recorded 2026-10-17. Accepts the Proposed items of D122 and D125.
- **One table, declared once per registry**: `src/function_library_records.c`
  lists the 16 nominative record types of F&O 4.0, appendix C (glossary
  `builtin_record_type`), each as its fn local name and the text of a
  structural record type in the specification's field order and types
  (a field whose type allows the empty sequence is optional, D7;
  "xs:positive-integer" of fn:location-record read as
  xs:positiveInteger). `sxml_function_library_initialize` declares them
  in the type registry of its item heap before any row is resolved: all
  names first, so fields may name table types, recursion included, then
  each type's fields from its resolved text. A name the registry knows
  already is skipped, so libraries sharing a heap declare once. No
  globals; static analysis resolves `fn:dateTime-record` through the
  registry like any named type, and rows may name them.
- **Records are annotated**: `sxml_function_library_make_record` makes a
  record of a built-in type by name (`sxml_record_make`). fn:parts-of-dateTime,
  fn:divide-decimals, fn:random-number-generator (and ?next), fn:regex,
  fn:matching-segments and its captured groups return annotated records;
  their rows name the types. The function items of a generator and of a
  compiled regex take their signatures from the record's field types, so
  ?take is `fn(xs:nonNegativeInteger) as xs:double*` (take(-1) and
  take(()) are XPTY0004) and ?next is `fn() as
  fn:random-number-generator-record`; fn:regex no longer looks its
  functions up in the function table.
- **Coercion to a nominative record type** (XPath 4.0, section 3.4.2):
  the rule of a structural record type, the new record annotated with the
  nominative type (`src/evaluator_function.c`), so a map coerces to
  fn:division-record. Matching stays nominative: an unannotated map is no
  instance. fn:build-dateTime keeps `map(*)?` and its own coercion (D122).
- **idiv**: a zero divisor is FOAR0001 before NaN or an infinite dividend
  (FOAR0002), so `INF idiv 0` is FOAR0001 (F&O 4.0,
  op:numeric-integer-divide).
- Measured (release driver, XP40, passed of cases): fn-parts-of-dateTime
  8 to 20 of 20, fn-divide-decimals 19 to 21 of 21, fn-random-number-generator
  50 to 54 of 61, fn-regex 27 to 29 of 30, fn-matching-segments 37 to 38
  of 39, op-numeric-integer-divide 132 to 132 of 143 (wrong errors 2 to 0).

### D134. The QT4 driver for XQ40 (packet 3.13)
Recorded 2026-10-17. Extends D92, D94 and D108 in `tests/qt4/driver/`.
- **XQuery adapter (temporary)**: `--spec=XQ40` compiles the test's query
  as an XQuery main module through `xquery_adapter.c` (glossary
  `qt4_xquery_adapter`): `sxml_xquery_compile` (D126) and the evaluation
  of `src/xpath.c`, behind functions with the signatures of
  `include/sxml/xquery.h` named `sxml_qt4_xquery_*`. It is the one driver
  file with `src/` headers (a PRIVATE include line in the driver's
  CMakeLists). Packet 3.12 deletes it and drops `qt4_` from the calls.
  Until 3.10 a prolog gives UNSUPPORTED_FEATURE, an "error" outcome.
  Params, context items and collection queries stay XPath expressions.
- **declared="true"** (params and `$name` sources) under XQ40: bound in
  the dynamic context, not declared by the host (a host declaration of
  the prolog's name is XQST0049, D106). Under XP40 they are declared as
  before.
- **Modules**: the resource table answers XQUERY_MODULE with the file of
  a `<module>` of the test case whose uri is the target namespace: the one
  whose location equals the hint (as written or absolute), else the one
  whose file name is the hint's last segment, else the first; resource URI
  `file:///SUITE-PATH`. No longer a skip reason. Unused until 3.10/3.11.
- **Collection queries**: each `<query>` is evaluated (XPath, the test's
  contexts so far) and its value follows the collection's documents in
  fn:collection; no longer a skip reason.
- **Serialization assertions**: serialization-matches serializes the
  value with the query's parameters (the adapter gives XQuery's default,
  method xml; XPath runs use the same) through `sxml_serialize_sequence`
  and applies fn:matches with the flags (letters only) to the text;
  output that is not UTF-8 fails. assert-serialization-error judges an
  error raised by the evaluation, else by serializing, like `error`.
  assert-serialization (only in comments of the suite) stays unsupported.
  assert-xml is unchanged (nodes through the adaptive method, which
  writes nodes as the XML method). Feature `serialization` is satisfied.
- **Slices**: `qt4/xquery-slice` runs `tests/qt4/xquery-slice.txt`
  (`--spec=XQ40`, 186 XQ-only cases that pass now, up to four per test
  set with five or more such passes, spread evenly) against
  `tests/qt4/xquery-slice-baseline.txt` (186, only goes up);
  `run-slice-test.cmake` takes SLICE_FILE and SPEC. Golden `qt4/run_xquery`
  and new cases in run_pass, run_fail cover each new feature.
- **Report**: `tools/conformance-report.py` writes the D95 gate rate
  (in_scope_passed / in_scope) per spec and, for XQ40 with an XP40 run in
  the same directory, the XQ-only rate (run cases whose XP40 outcome is
  "skip: spec"). `run-qt4.sh --spec XQ40` needed no change.
- Measured (release, snapshot of c9a0cc6, one process per test set):
  XP40 29,029 passed before and after (no case changed). XQ40 47,313:
  33,374 passed (626 wrong error), 2,037 failed, 6,208 errors, 5,694 not
  run; gate rate 33,356/41,174 = 81.0 %; XQ-only 4,310/10,723 = 40.2 %;
  method-* 22/444 run. Errors: 5,969 prolog declarations (3.10), 179
  group by/windows (3.8); fails: 1,559 XPST0017 (Phase 2 functions not
  in that snapshot: fn:parse-csv, unparsed-text, json-doc, parse-uri,
  path, hash, collection, ...).

### D135. Prologs: the static context, declared functions and variables (packet 3.10)
Recorded 2026-10-17, completing D55 and D101 for XQuery main modules.
`src/xquery_static.c` (`src/xquery_static_internal.h`) reads the prolog
into one `sxml_xpath_static_context`; `src/xquery_prolog.c` lowers the
module and now holds `sxml_xquery_lower` / `sxml_xquery_compile` (moved
from `src/xquery_lower.c`); `src/evaluator_global.c` computes globals.
No new IR kind, no header change, no machinery beside the core's.
- **Static context**: namespace declarations and the default element
  namespace (prefix 0) first, then the host's bindings, then `local` and
  `output` (XQuery predeclares them); variables: the host's, then the
  prolog's (global n); a function table finds declared functions, then
  the host's table. Duplicates are found by sorting (XQST0033, XQST0049
  also against host declarations, XQST0034 for overlapping arity ranges);
  setters and the context value declaration at most once (XQST0068,
  0038, 0032, 0067, 0065, 0069, 0055, 0099); XQST0070, XQST0066,
  XQST0039, XQST0148, XQST0045 (function names and annotations in
  reserved namespaces, the XQuery namespace for unprefixed annotations;
  so `%updating` is rejected, D97), XQST0106/XQST0116, XQST0009 (schema
  import, D97), XQST0038 (unsupported default collation). External
  functions are XPST0017. Module imports and item type/record
  declarations are UNSUPPORTED_FEATURE for now.
- **Unprefixed function names** (5.14.2): the core resolves them to fn
  and looks names up without interning, so the prolog interns fn:L for
  every unprefixed call/reference name when it declares a no-namespace
  function or a default function namespace; the table then tries the
  declared no-namespace L (or L in the default function namespace, also
  among library functions: `pi()` under the math namespace) before fn:L.
  A written `fn:L` finds the same (Proposed: a default function namespace
  in the XPath static context).
- **Declared functions**: one IR function per arity of the arity range,
  added to the unit before any body is lowered (recursion and mutual
  recursion are STATIC_CALLs on the evaluator's stack, D75); the full
  arity holds the body with its result COERCEd; a shorter arity binds the
  missing defaults (coerced, no focus) and calls the full one; `:= .` is
  SXML_PARAMETER_DEFAULT_CONTEXT_VALUE, which the core supplies at the
  call. Bodies are level-0 functions with an absent focus.
- **Variables and the context value**: each value or default is an
  initializer IR function of arity 0, coerced to the declared type. When
  the prolog has initializers or a context value declaration, the unit
  gets the **context global** (name 0, the last global; glossary) and the
  query body and every variable initializer read the focus as the fixed
  focus of it (5.17); the main function's focus slot receives the host's
  context value, which the global part copies (coerced) into the context
  global unless a non-external declaration gives the value.
- **Computation order**: after lowering, the expression, atom and map
  entry runs each body and initializer added are scanned for globals read
  and declared functions called or referenced; an iterative walk from the
  query body orders the reached variables dependencies first; a cycle
  through a variable is XQDY0054, raised at compile time (5.16 allows
  static detection). Reached variables get, in that order, an initializer
  that calls their lowered one; the global part sorts the variables to
  compute by initializer index and runs each as a CALL on the machine (a
  pushed task turned into a CALL, as the machine's static calls are), so
  unreached variables are never computed (`declare variable $x := 1 div
  0; 2` is 2); BEING_COMPUTED still gives XQDY0054 for dynamic cycles.
- Rejected: computing on first read (needs every atom read in the
  machine to suspend: a change across `src/evaluator.c`); relowering in
  dependency order (two lowerings; the extra initializers cost one call).
- **Later additions** (same packet): named item types are aliases in the
  XPath static context (`named_item_types`, `sxml_xpath_find_named_type`,
  also for cast targets; resolved in rounds, so forward references work)
  with a cast as constructor function for generalized atomic types;
  records are nominative registry types (reused when an earlier
  compilation for the item heap declared the name) with a constructor
  IR function (a map of the fields, %static fields' defaults, coerced to
  the record); the QT4 tests call constructors of records without
  `%constructor`, so every record gets one. Decimal formats (XQST0111,
  XQST0114, XQST0097 for a value's form, XQST0098 otherwise) and the
  default collation go into `unit_static_context` (D131), which the
  module entry copies into the unit. `sxml_xpath_static_context` gets a
  function namespace rule (fn; a declared default; no namespace first,
  which the XQuery table falls back from to fn), replacing the interning
  workaround above; an empty namespace declaration binds the prefix to no
  namespace, which the core reads as undeclared (the lowerer accepts it);
  `xq` is predeclared; annotations of inline functions and function types
  are checked (XQST0045, XQST0125). Output declarations are checked by
  name (XQST0109, XQST0110); their values are 3.12's to read.

### D136. Node, sequence, string and type functions (packet 2.17)
Recorded 2026-10-17. F&O 4.0 sections are those of the current editor's
draft.
- **One options reader** (glossary `option_definition`):
  `sxml_function_library_read_options` (`src/function_library_options.c`,
  declared in `src/function_library_core_internal.h`) reads every map that
  follows the option parameter conventions (1.7): a definition is a name
  and a sequence type; values are coerced with `sxml_evaluator_coerce`;
  a string key that names no option is XPTY0004, an xs:QName key in a
  namespace is ignored. The string and JSON parts use it (their own
  readers are gone); the element-map, resource and serialization-parameter
  readers of other packets could move to it.
- **xnode()?** for fn:name, local-name, namespace-uri, node-name, base-uri,
  document-uri, nilled (as F&O 4.0 types them): a JNode is XPTY0004 (QT4
  PathExpr-J-461, -462).
- **Node functions** (`src/function_library_node.c`): siblings (the
  parent's child step); innermost/outermost in document order (a node's
  subtree follows it, so only the next member, or the last kept one, needs
  an ancestor walk); lang (caseless by full lower-case mapping); id,
  element-with-id and idref scan the target document for the is-id /
  is-idrefs nodes the parser marks (xml:id, DTD ID/IDREF/IDREFS),
  FODC0001 for a root that is not a document; path in every 4.0 form;
  location from the tree's `get_location` (`tree_internal.h`), as an
  fn:location-record: the processor's loader does not keep locations, so
  documents it parses give the empty sequence, which F&O allows; fn:doc
  accepts and ignores `$options`.
- **parse-xml-fragment** (`src/function_library_json.c`): the entity, after
  a UTF-8 byte order mark and a text declaration (which must have an
  encoding and no standalone, else FODC0006), is parsed by the loader as
  the content of an element of a document made around it, whose children
  are copied into a new document node; binary input is taken as UTF-8.
  **strip-space="all"** (both parse functions) copies through a tree
  builder that drops whitespace-only text outside xml:space="preserve".
  **system-properties** (17.1.9) lives in the same part: the 14 built-in
  properties, sxml not schema-aware and without the optional features.
- **Type part** `src/function_library_type.c` (19.1): type-of; schema-type,
  atomic-type-annotation and node-type-annotation make an
  fn:schema-type-record from the type registry; base-type, primitive-type,
  members and matches are anonymous function items over native rows that
  capture the type identifier as xs:integer; constructor is the type's
  constructor body (as xs:T#1), absent for xs:anyAtomicType,
  xs:anySimpleType and namespace-sensitive types; variety "mixed" for
  xs:anyType and xs:untyped; xs names carry the xs prefix.
- **String part**: pad-string, index-of-substring (codepoint matching).
- **Sequence part**: starts-/ends-/contains-subsequence as one resumable
  body (D75): one call of `$compare` per step; without it fn:deep-equal's
  leaf entry, no calls.
- **Collations** (D116's proposal for these bodies, accepted): fn:min,
  fn:max, fn:highest, fn:lowest, fn:sort-by, array:sort and array:sort-by
  resolve through `sxml_function_library_find_collation` /
  `_resolve_collation` and compare with `_compare_collated`; each sort key
  definition carries its collation (the default when a sort key record
  names none). `sxml_higher_order_check_collation` is gone;
  `sxml_function_library_check_collation` has no caller left (its
  definition in `src/function_library.c` may go). Order-by keys in the
  evaluator's SORT still accept only the codepoint collation.
- **Not done**: a descending range for `count(reverse(1 to 10^11))` (one
  QT4 case, RangeExpr-408g; needs a frozen-header addition, see
  "Proposed"); the zero comparison bug in `src/xdm_atomic.c` (Proposed).
  Survey (every fn/map/array/math name of the XP40 sets, function-lookup
  at arities 0 to 6): 26 functions missing, all of packets 2.11, 2.12,
  2.15, 3.11 or D97's non-goals (html-doc, parse-html, invisible-xml,
  xsd-validator, transform).

### D137. Group by and window clauses (packet 3.8)
Recorded 2026-10-17, completing D55 and D126 for these clauses. No IR
kind added; contract in `src/xquery_flwor_internal.h`: lowering in
`src/xquery_lower_window.c`, two internal native rows in
`src/xquery_flwor.c` in no function table (a STATIC_CALL targets the row:
`Q{}group-by#3`, `Q{}window#4`).
- **group by** stays a tuple stream clause. Its grouping specifications
  are bindings of the segment before it (a value is a let, 4.13.8's
  rewrite); after the last, each grouping variable is rebound to its
  grouping key: coerced to `xs:anyAtomicType?` (atomized, XPTY0004 for
  two items), and to a declared type again (so `as attribute(*)` is
  XPTY0004); XQST0094 when no variable of the FLWOR has the name. Tuple
  arrays before a group by pack every variable, constants too. The row
  takes the tuple arrays, the key member numbers and one collation URI
  per key ("" = default collation; XQST0076 for one `sxml_collation_find`
  does not know, relative URIs against the static base URI) and returns
  one tuple array per group, same layout, groups in the order of their
  first tuples: the first tuple's keys, every other member the
  concatenation of its values. Keys are equal when both empty or
  atomic-equal, strings under a non-codepoint collation by collation key,
  dates and times without timezone given the implicit one
  (fn:deep-equal); hashed as fn:distinct-values does.
- **Window clauses** are not tuple stream clauses (glossary): a clause of
  a segment like for (4.13.4.3), a MAP over the window arrays (glossary
  `window_array`) the window row returns; the body binds the declared
  variables by LOOKUP ($w with its type) and lowers the rest of the
  segment. XQST0103 for two variables of one name. The row's kind bits:
  sliding, has end, only end. Without end clause a tumbling window ends
  before the next start; tumbling `only end` without an end item stops.
- **Condition closures** (glossary): one lowering procedure for window
  start and end conditions (4 and 8 parameters named by the window
  variables; the row takes the effective boolean value) and the while
  clause (D126: tuple unpacked from parameter 1; its body is now
  `conditional(C, true(), false())`, not `fn:boolean(C)`), so capture
  analysis and focus capture exist once; `sxml_xquery_unpack_tuple` is
  shared.
- Measured (3.13's `--spec=XQ40`, release): prod-GroupByClause 35/40
  (not run 2; group-020 is a prolog, group-025 a UCA tailoring,
  use-case-groupby-Q4 an evaluator SORT over an empty input:
  `sxml_sort_finish` allocates 0 bytes, INVALID_ARGUMENT);
  prod-WindowClause 125/158 (19 not run, the 14 errors are prologs). The
  FLWOR sets are as D126 measured.

### D138. The JavaScript package and release 0.2.0 (packet 2.18)
Recorded 2026-10-17, completing D10 and D47. Node 22.22, WASI SDK 34.
- **Layout** (`bindings/javascript/`, glossary `javascript_package`,
  `javascript_shim`): the shim `sxml_javascript.c` (with its interface
  `sxml_javascript.h`, not a public header) uses only the public API, as
  the CLI does. It builds as a library in every preset, so `-Werror`,
  clang-tidy and ast-grep see it (`cmake/SxmlLint.cmake` globs
  `bindings/`); under WASI it links into `sxml.wasm`, a reactor module
  (`-mexec-model=reactor`, `export_name`, `--strip-all`). The package is
  hand-written TypeScript (`src/index.ts`, `src/wasi.ts`), erasable syntax
  only, so Node.js (22.18 and later) runs it and its tests directly;
  `tsc` 5.9.3, a devDependency, type-checks (ctest
  `javascript/typecheck` after `npm ci` there) and builds `dist/`.
  No runtime dependencies.
- **WASI**: the module imports only `clock_time_get`, `fd_write`,
  `fd_close` and `fd_seek` (sxml does no I/O; resources come through the
  resolver). The package supplies these itself in Node.js and browsers
  alike (real clocks; EBADF; ENOSYS for any other import), instead of
  `node:wasi` and a browser polyfill: one code path. So D65's fast-call
  workaround does not concern the package; `tools/run-wasi.mjs` is
  unchanged. Checked in headless Chromium (fetch, resolver, clock).
- **API**: `load()` gives an `Sxml`; `createProcessor({resolver})`;
  `Processor.parse`, `compile` (an `XPathExpression` with named
  external variables, `item()*`, NCName or `Q{uri}local`), `evaluate`,
  `value`, `serialize`, `dispose`; `Value.serialize`, `toString`
  (adaptive), `dispose`; failures throw `SxmlError` (status, code
  `prefix:local`, description, line, column). JavaScript data becomes XDM
  as fn:parse-json reads its JSON (a string is an xs:string), one
  general rule instead of per-type conversions; serialization options are
  fn:serialize's options map, read by `sxml_serialization_parameter_reader`
  (camelCase keys accepted). The resolver is a synchronous callback
  (`sxml_host.resolve` import) returning a string, bytes or nothing; an
  exception it throws is rethrown; it must not use its processor.
  XQuery arrives additively once `xquery.h` is implemented: a
  `Processor.compileQuery()` giving an expression object with the same
  `evaluate` and the query's serialization parameters as the default of
  `serialize`; resource kinds already include `xquery-module`.
- **Memory**: every package object owning C memory is a handle
  (glossary); `dispose()` releases it, a `FinalizationRegistry` releases
  what is never disposed. A processor's destruction releases its live
  values and expressions first, since they cannot outlive it in C. Each
  processor has a counting allocator: tests check that destruction leaves
  0 allocations and run the allocation-failure loop (D23) from JavaScript.
- **Size budget** (D93, glossary `size_budget`): at this commit the `sxml`
  CLI module is 1,820,106 bytes stripped, 629,585 gzipped (zlib -9);
  `sxml.wasm` 1,788,836 and 617,898. Phase 2 grew both past D93's 1.5 MB
  and 450 KB (from 1,213,261 and 358,020); `-flto -Oz` saves 2%, so the
  growth is code and tables, not flags. The budget becomes 2,000,000
  bytes stripped and 700,000 gzipped for both modules, checked by ctest
  `javascript/size_budget` (wasm preset), which prints the sizes. The
  owner may instead want optional parts as separate modules (PLAN.md,
  risks): see "Proposed".
- **npm name**: `sxml` is taken on npm (another project, 1.0.4); the
  package is `sxml-js` until the owner chooses.
- **Release 0.2.0**: `SXML_VERSION_*` 0.2.0, equal to `SXML_API_VERSION`;
  CMake project version 0.2.0; `CHANGELOG.md`; `docs/RELEASE.md` says how
  to cut a release. From 0.2.0 the public headers are stable (D47): a
  release adds declarations (minor before 1.0.0) and never changes one
  without a major (before 1.0.0, minor) version and a record. Tagging
  and publishing to npm are the owner's.

### D139. The stylesheet reader and static expressions (packet 4.5)
Recorded 2026-10-17, after D81, D102, D118, D120. `src/xslt_ast.c` (the
builder of D102), `src/xslt_read.c` with `src/xslt_read_internal.h`
(`sxml_xslt_read_stylesheet`), `src/xslt_static.c` with
`src/xslt_static_internal.h` (the static scope); the packet named no
headers, so each module has its own.
- **Walk**: each module is parsed by the XML parser into an arena tree
  with locations; one explicit stack of open elements (D71) holds the
  elements of every module being read, so an include or import pushes the
  new module's document frame and is read in place (D102's pre-order).
  Text runs are collected between elements (comments, PIs, xsl:note
  dropped, 3.11.1) and stripped by 3.11.1's rules.
- **Tables**: one row per XSLT element, generated from the 4.0 schema
  (attributes and their types, D102) and the element syntax summaries of
  the spec (required attributes, category, content model); one row per
  attribute (value syntax, keyword mask). Where the two disagree the
  schema wins (record-type has no constructor, field has default).
- **Static expressions** (9.7) run through the one front end, lowering
  and evaluator (sxml_xpath_lower, sxml_evaluator_evaluate) with a static
  scope: the static variables so far (later hides earlier) and a function
  table that puts system-property, available-system-properties,
  function-available, element-available and type-available (native rows
  of a small function library) in front of the processor's. 4.10 may
  move these rows to a library part for non-static XSLT expressions.
  Shadow attributes are value templates evaluated to a string (the AST is
  that of a string template, D120); the shadow's static context is the
  element's native or fixed namespaces with the parent's default element
  namespace and base URI. Values of static variables are coerced by
  binding them to a declared variable (XTTE0570, XTTE0590 for supplied
  parameters); a required static parameter without value is XTDE0050.
- **Modules** (3.10): hrefs are resolved against the element's static base
  URI and asked from the resolver as XSLT_MODULE; unavailable or not
  well-formed is XTSE0165. A reference to a module whose URI is on the path
  being read is ignored when every step of that path and the reference is
  an include (an include cycle, 4.0), else XTSE0210; an include of a URI
  already in the stylesheet level is ignored (included_module 0).
- **Node contents** (TEXT_NODE value templates, xsl:select) name their
  expression before it exists: the AST numbers expressions in order, so
  the reader gives the next index and hands the AST over after the node.
  `sxml_xslt_ast_add_node` marks a module simplified when its first node is
  a synthesized STYLESHEET (the header has no other writer for the flag).
- **Checks while reading**: XTSE0010 (placement, order, required
  attributes and children), XTSE0090, XTSE0805, XTSE0020 (values), XTSE0110,
  XTSE0120, XTSE0125, XTSE0130, XTSE0260, XTSE0280, XTSE0340/0350/0370 and
  XPST0003 from the front ends, XTSE0500, XTSE0530, XTSE0740, XTSE0808,
  XTSE0812, XTSE1430, XTSE0122 (fixed-namespaces by URI is not read),
  XTSE1660 (import-schema, strict/lax validation, schema types; D97).
  Errors keep their code and point at the attribute or element (module
  URI, line, column; an expression's own offset added on its first line).
- Left to lowering: duplicate declarations, XTSE3450 between static
  variables, the arity and ordering rules beyond leading children, the
  values static variables give non-static expressions (the AST does not
  keep them: lowering re-evaluates their select with the same scope).

### D140. URIs, QNames and hashes (packet 2.11)
Recorded 2026-10-17, after D88, D91, D101. New part `src/function_library_hash.c`
(fn:hash) over `src/hash.c` / `src/hash_internal.h`; rows added to the uri
part (decode-from-uri, parse-uri, build-uri) and the qname part
(parse-QName, resolve-QName).
- **Hashes**: MD5, SHA-1, SHA-256, BLAKE3 (the draft keeps it) and CRC-32
  as one call `sxml_hash_compute` (algorithm, message view) into a fixed
  32-octet `sxml_digest`: no allocation, no state; BLAKE3 merges chunk
  chaining values with a 54-entry stack (no recursion). The effective
  name is fn:upper-case(fn:normalize-space()) with Unicode case mapping,
  matched byte for byte; anything else is FOHA0001. No options are
  defined, so any unqualified key is XPTY0004. One step per 64 octets.
- **fn:parse-uri** follows F&O 4.0, 7.6.2 step by step; each regular
  expression is matched by hand over the octets (no regex per call). The
  parts are views into one working copy (backslashes made slashes) and one
  rewritten copy for a drive letter. Implementation-defined: known
  hierarchical schemes are file, ftp, http, https; a port that is not 1 to
  18 digits is left out (the field is xs:integer?); a zero-length authority
  (`////host`) is the empty sequence, as QT4 fn-parse-uri-048b/049b expect.
  query-parameters keeps keys in first-appearance order with all values;
  repeated keys are chained through an index map (O(n log n), not
  quadratic). The result is an annotated fn:uri-structure-record (D133).
- **Percent decoding** (glossary): one octet reader for decode-from-uri,
  parse-uri (URI decoding) and query parameters (form decoding, `+` as
  space); broken escapes take `%` and up to two octets (QT4 `%üü` is
  U+FFFD then `ü`); UTF-8 breaks off at the first octet outside Unicode's
  well-formed ranges; U+0000, U+FFFE, U+FFFF become U+FFFD.
- **fn:build-uri** reuses the escaping table of encode-for-uri (three new
  kinds: path segment, query parameter, fragment). Deviation: a URI
  without a scheme gets `//` before an authority (the spec text adds
  nothing, which would make `{'host': 'x', 'path': '/p'}` read back as a
  path); an empty query (from an empty map) adds no `?`.
- **fn:parse-QName** is marked USES_DYNAMIC_CONTEXT so no pass folds it:
  prefix:local is the xs:QName cast with the static namespaces of
  `sxml_dynamic_context_get_running_unit` (D131; FONS0004 from the cast,
  predeclared prefixes included); NCName and Q{uri}local are made directly;
  FOCA0002 for the rest. **fn:resolve-QName** looks the prefix up in the
  element's in-scope namespaces only (no predeclared fallback: `xs:x` on
  `<a/>` is FONS0004).
- Measured (release driver, XP40, passed of cases): fn-parse-uri 0 to
  74/74, fn-build-uri 0 to 48/48, fn-decode-from-uri 0 to 29/29,
  fn-parse-QName 6 (all wrong errors) to 19/19, fn-resolve-QName 4 to 8/8
  run (23 XQuery-only), fn-hash 1 to 54/54.

### D141. CSV parsing: fn:parse-csv, fn:csv-to-xml, fn:csv-doc (packet 2.15)
Recorded 2026-10-17, after D97, D128, D131, D133. New module `src/csv.c` /
`src/csv_internal.h` and part `src/function_library_csv.c`.
- **Scanner** (D97, by hand): `sxml_csv_read_token` gives one `csv_token`
  (field, row end, input end; glossary) per call and keeps its place in a
  `csv_cursor`, a plain value: no object, no allocation. A field is a view
  into the text; only a quoted field with a doubled quote or a CR needs
  `sxml_csv_decode_field`, into caller memory no longer than the field.
  Special characters are matched as whole UTF-8 sequences byte by byte.
- **Rules where F&O 4.0 is loose**: CR is a newline, so it cannot be a
  separator, quote or comment marker (FOCV0002, like LF); trimming removes
  U+0020 and U+0009 from unquoted fields and all four XML whitespace
  characters from header names; with trim-rows, a blank row is padded to
  the first row's width like any short row; select-columns beyond a
  header's end name the zero-length string; a selected position past
  int64_t selects nothing.
- **fn:csv-to-arrays** is not implemented: F&O 4.0 dropped it (issue 2961,
  absorbed into fn:parse-csv) and QT4 has no test set for it.
- **One parse, three functions**: fn:parse-csv builds columns, column
  index and rows once; the record's get is a function item over a static
  row (after fn:regex, D133) capturing rows and column index, signature
  from the record's field type. fn:csv-to-xml walks that result, as the
  specification defines it over fn:parse-csv. fn:csv-doc calls the
  fn:unparsed-text row (found by name, as fn:json-doc finds parse-json),
  so text decoding (D131) is written once.
- Measured (release driver, XP40, passed of cases; before at 3a3a3cb, all
  wrong errors): fn-parse-csv 35 to 107/124, fn-csv-to-xml 9 to 68/68 run,
  fn-csv-doc 4 to 20/20. The 17 parse-csv failures are assertions such as
  `?get(1, 1) = "one"` that read the result as the context value, which
  the driver does not bind (Proposed below); with it, 124/124.

### D142. Sequence constructors and node construction (packet 4.6)
Recorded 2026-10-17, after D102, D103, D110, D120, D124, D127, D139.
`src/xslt_lower.c`, `src/xslt_lower_construct.c`,
`src/xslt_simple_content.c` with `src/xslt_lower_internal.h`; no IR kind,
no evaluator change, no public header.
- **One machine**: XSLT instructions are lowering procedures pushed with
  `sxml_xpath_lowering_push_procedure` (D103) on a lowerer of language
  XSLT; every expression and value template goes through the embedding
  entry with an XPath static context made once per AST static context
  (namespaces, xpath-default-namespace as prefix 0, base URI, the
  processor's function table). The files define no lowering part (no
  XPath AST kind is XSLT's yet); instructions are found through
  `sxml_xslt_instruction_rule` views, one per file, listed in
  `src/xslt_lower.c` (4.7 to 4.14 add a view and one line).
- **Sequence constructor** = one atom per child, bound in the caller's
  block; its value is the SEQUENCE of them, given to constructors as one
  ATOM content (D110: one enclosed expression, so 5.7.1 joins adjacent
  atomic values across instructions). Variables a child pushes stay in
  scope until the constructor ends (4.7's xsl:variable). A constructor
  call may install a focus (FIXED for xsl:copy select, SLOTS for the
  for-each of xsl:array).
- **Literal result elements**: ELEMENT_CONSTRUCTOR with XSLT_RULES, an
  xs:QName constant, namespace bindings from the stylesheet tree's
  in-scope namespaces minus xml, the excluded namespaces (unless an alias
  outputs them) and alias literal URIs (11.1.3, 11.1.4; aliases resolved
  by import precedence, XTSE0810); xsl:inherit-namespaces="no" is
  NO_INHERIT. Attributes are ATTRIBUTE_CONSTRUCTORs of their AVT.
- **Value templates and simple content** (5.6, 5.7.2): string templates
  lowered here; each enclosed value goes through the internal native row
  `merge-text-nodes` (adjacent text nodes to one xs:untypedAtomic, else
  the value as it is) and fn:string-join. Fixes D120's deviation:
  `{//item/text()}` is "onetwo". The same path gives xsl:attribute,
  xsl:value-of, xsl:text, xsl:comment (hyphens fixed with fn:replace,
  11.8), xsl:processing-instruction (leading space, "?>", 11.6) and
  xsl:namespace (XTDE0930) their strings.
- **Names**: a name (and namespace) without enclosed expressions is
  resolved while lowering to an xs:QName constant; an invalid one becomes
  a RAISE of XTDE0820/0830/0850/0855/0860 when evaluated. A computed name
  is a string resolved by the evaluator with the instruction's in-scope
  namespaces as name_namespaces (without the default one for
  attributes), or fn:QName(namespace, name) (prefix dropped for "").
- **XSLT error codes** where the evaluator raises XQuery's: a TRY_CATCH
  around the constructor alone whose clauses RAISE the XSLT code
  (`sxml_xslt_lowering_bind_mapped_expression`): XQDY0137 to XTDE3365,
  XPTY0004 to XTTE3375 (maps), XQDY0074/0044/FOCA0002 to XTDE08x0
  (names), XQDY0041/0064 to XTDE0890, XQDY0074/0101 to XTDE0920/0925.
- **Others**: xsl:copy is SHALLOW_COPY with the content as its own body;
  xsl:copy-of a MAP that SHALLOW_COPYs elements and documents with
  `(@*, node())` as content and other nodes whole; xsl:map/xsl:record a
  MAP_CONSTRUCTOR (fields, then the content's maps as a merged entry),
  with [xsl:]duplicates map:merge; xsl:array array:of-members over
  JNodes (fn:jtree for non-JNodes), xsl:array-member fn:jtree; xsl:select
  its content expression; extension and unknown instructions their
  xsl:fallback children or XTDE1450.
- **Stylesheet AST** (accepts D139's Proposed item): appended
  SXML_XSLT_AST_KIND_RECORD and attribute kinds CONSTRUCTOR
  (record-type, BOOLEAN) and RECORD_FIELD (EXPRESSION); xsl:record reads
  attributes in no namespace as fields (no shadow or standard
  attributes), xsl:-prefixed ones as standard attributes and
  xsl:duplicates.
- **Entry** (until 4.8 and 4.15): `sxml_xslt_lower_initial_template`
  lowers one template's sequence constructor as the main function (focus
  = global context item), optionally inside a DOCUMENT constructor (the
  principal result tree), then runs the standard passes and seals.
- Deviations: xpath-default-namespace="##any" reads as none; attribute
  sets (4.12), xsl:type/validation ignored (D97); a zero-length text node
  is no node (D110).

### D143. Conformance sweep 1: computed constructors in XPath, the JSON fallback, descending ranges, function equivalence (packet 2.19)
Recorded 2026-10-17, after D95, D110, D127, D133, D136, D141. Accepts the
Proposed items "QT4 assertions see the result as the context value",
"Decimal zero against doubles", "A descending integer range" and
"Equivalence of function items" (orchestrator, packet 2.19).
- **Computed constructors are XPath 4.0** (section 4.13): the lowering
  part `construct` (D127) serves every language; direct constructors only
  parse in XQuery. 136 "error" outcomes of XP40 (prod-Comp*Constructor)
  become passes.
- **The fallback option** of fn:parse-json, fn:json-to-xml and fn:json-doc
  (F&O 4.0, 17.4.4 to 17.4.6): the three rows are resumable bodies (D75)
  of the json part; before the parse, the fallback is called once per
  codepoint that has an `unpermitted_escape` (`sxml_json_find_unpermitted_
  escape`, a lexical scan that pairs surrogates as parsing does), with the
  escape as written; its result is coerced to xs:anyAtomicType and its
  string value becomes the `json_replacement` the parse inserts for every
  escape of that codepoint (`sxml_json_parse_options.replacements`,
  U+FFFD without one). A fallback whose arity is not 1 is XPTY0004 when
  the options are read. fn:json-doc's row moves to the json part; the
  resource part keeps reading its text
  (`sxml_function_library_read_json_document`).
- **fn:generate-id of a JNode**: "j" tree, then "c" child index for the
  node and each ancestor below the root, read upwards: unique per JNode
  (JTree and path), the same for two cells of one JNode.
- **Decimal zero**: `sxml_double_write_exact_text` writes a zero of
  either sign as "0", so 0.0 eq 0e0 and the hash of 0.0 match.
- **A descending integer range** (additive, D99):
  `SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE` and
  `sxml_sequence_make_descending_integer_range` in `include/sxml/xdm.h`;
  the range cell with a second cell class. fn:reverse of a range in
  either order is O(1); count, get_item and iteration are O(1) per item;
  the general comparison of a range with one integer and the temporal
  check read either order. Fast paths that only know ascending ranges
  iterate a descending one.
- **Equivalent function items** (F&O 4.0, 1.9.5): deep-equal treats two
  IR functions of one unit as the same body when they have the same
  `static_form` (`sxml_ir_function_has_same_static_form`): the IR text
  form of each (D51 printer, explicit stack) without names, annotations
  and the atoms of the function's own captures, with every nested
  FUNCTION_DEFINITION printed as the function it defines; the captured
  values are compared by deep-equal as before. Slots are numbered by
  position, so renaming a variable does not change the form.
- **QT4 driver**: assertions see the result as the context value; a
  source that only has a uri and is not well-formed stays unavailable, so
  fn:doc raises FODC0002 instead of an "error" outcome.
- math:sin, math:cos and math:tan name their parameter $radians.
- **fn:last#0 and other focus-dependent named references** keep their
  name on the closure (XPath 4.0, 4.5.4), so function-name(fn:last#0) is
  fn:last and fn:last#0 differs from fn:position#0 under deep-equal.
- Measured (release, XP40, before at 62947af with the 3.10 prolog commit
  after it): passed 30,389 to 30,628 (+239, none lost), errors 187 to 2,
  in scope 96.3 % to 97.0 %; prod 96.6 to 98.1 %, fn 98.3 to 98.9 %, op
  99.7 to 99.8 %, misc 97.8 to 98.2 %; slice 198/198.
- Not done (open, for later sweeps): fn:function-lookup of a
  focus-dependent function (45 cases need the dynamic focus at the call),
  casts to array, map and record types (prod-Cast*Expr.composite, 82
  cases), counted regex repetition without expansion (cbcl-matches-004,
  -038: the last two "error" outcomes, LIMIT_EXCEEDED), partial
  application of a dynamic call with the wrong arity (xqhof8, xqhof9).

### D144. Templates and modes (packet 4.8)
Recorded 2026-10-17, after D55, D101, D118, D124, D139, D142.
`src/xslt_lower_template.c`, `src/evaluator_template.c`,
`tests/evaluator_template_test.c`; APPLY_TEMPLATES, the one reserved IR
extension point of XSLT, is accepted by the verifier. No other IR kind, no
public header.
- **IR** (`src/ir_internal.h`, the reserved part, completed):
  `sxml_ir_apply_templates` = select (NONE: the function's focus, for
  next-match and apply-imports), mode (0: the current mode), rule search
  (FROM_START, AFTER_CURRENT_RULE, IMPORTED_ONLY), and three atoms:
  `parameters` and `tunnel_parameters`, two maps from xs:QName to values,
  and `separator` (6.7). The by-name parameter binding list is gone (maps
  replace it). A template rule gains `lowest_imported_precedence` (the
  import subtree of its stylesheet level, for apply-imports, 6.11); a mode
  gains `built_in_template` and `fails_on_multiple_match`. Adders
  `sxml_ir_unit_add_template_rules`, `_add_modes`; accessors `_get_mode`,
  `_get_template_rules`; the text form prints search and maps. Verifier:
  modes in range, match functions of arity 1, template functions of
  arity 5.
- **Template function**: one IR function per xsl:template with five
  parameters: item()?, position, size (its focus slots) and the two
  parameter maps. Its body binds each xsl:param from map:contains/map:get
  (coerced, XTTE0590) or its default (select, content as a temporary tree
  without as, "" or () implicitly, XTDE0700 when mandatory), checks
  xsl:context-item (XTTE3090, XTTE0590), and coerces the result to `as`
  (TRY_CATCH mapping XPTY0004 to XTTE0505). A named template has a second
  function lowered with an absent focus, called where the caller's focus
  is absent (xsl:function, the initial template without a global context
  item: the main function tests the focus with a TRY_CATCH of XPDY0002),
  so reading `.` there raises XPDY0002 (10.1.2).
- **Instructions**: xsl:call-template is a STATIC_CALL (XTSE0650,
  XTSE0680 unless version < 2.0, XTSE0690); xsl:apply-templates,
  next-match, apply-imports are APPLY_TEMPLATES. Tunnel parameters
  (10.1.6): inside a template's function its received tunnel map (slot 5)
  with the tunnel xsl:with-param values put in (map:put), else the empty
  map. next-match and apply-imports pass the received non-tunnel map too
  when their effective version is 4.0 or more (6.11). Lexically outside a
  template body (another focus or function): XTDE0560 when evaluated.
  apply-templates without select: XNode child::node(), JNode child::*,
  map/array jtree(.)/child::*, else XTTE0510.
- **Modes** (6.9): the unnamed mode is mode 1; then every mode xsl:mode
  declares (XTSE0545 over all its attributes, XTSE0020 for a non-private
  unnamed mode, XTSE4005, XTSE4010 for enclosing modes) and every mode a
  template, apply-templates or default-mode names (XTSE3085 under
  declared-modes). Rules per mode (#all: every mode but enclosing ones)
  sorted by precedence, priority (explicit or D118's default) and
  declaration order (6.8), heapsort. XTSE0660, XTSE0080.
- **Built-in templates** (6.10) are template functions lowered from short
  XPath snippets and the existing kinds (APPLY_TEMPLATES in the current
  mode with the received maps, TEXT_CONSTRUCTOR, SHALLOW_COPY with
  copy-namespaces, RAISE XTDE0555), one per on-no-match used;
  shallow-copy-all reads as shallow-copy.
- **Evaluator** (`src/evaluator_template.c`): a part task per
  APPLY_TEMPLATES; each match function and template function is a CALL on
  the machine (no C recursion, D71: 100,000 levels tested); the task
  catches errors while a match function runs (6.5: no match); the
  dynamic context's current_mode and current_template_rule_index are set
  around each template call and restored on return and on unwinding;
  on-multiple-match="fail" checks the tied rules (XTDE0540).
- **Entry**: `sxml_xslt_lower_initial_template` lowers templates and modes,
  then the invocation (`sxml_xslt_invocation`): a call of the initial
  template (xsl:initial-template or a named one) with the global context
  item, or apply-templates to it in the initial mode (default mode of the
  principal module; XTDE0045 for an unknown one; XTDE0040 for an unknown
  initial template). A simplified module is applied in the unnamed mode.
- Left out: xsl:sort in apply-templates (4.9), current() and
  fn:apply-templates (4.10), mode `as`/`typed`, warnings, packages
  (xsl:override, visibility beyond XTSE0020), the 4.0 JNode shallow copy.
- Measured with a scratch harness (lower, evaluate, serialize; assertions
  through the XPath API) over the applicable in-scope cases of the sets of
  the packet: 316 of 615 pass (match 93/193, mode 70/162, tunnel 53/58,
  declared-modes 14/14, apply-templates 27/39, next-match 28/51,
  call-template 21/42, built-in-templates 4/5, template 5/6, apply-imports
  1/5, priority 0/40). Of the rest, 99 need global variables (4.7), 73
  other instructions, 59 stylesheet parameters (4.7, 4.15), 21 packages,
  18 XSLT functions (current, document, xsl:function: 4.10, 4.12), 11
  `~T` patterns (D139). Patterns with predicates select the step again
  from its parent (D118), so many sibling rules cost quadratic time
  (match-242: 60 s in a debug build).

### D145. Control flow and variables (packet 4.7)
Recorded 2026-10-17, after D71, D103, D109, D124, D126, D135, D142.
`src/xslt_lower_control.c` (rule view `sxml_xslt_instruction_rules_control`,
one line in `src/xslt_lower.c`'s list), `tests/xslt_lower_control_test.c`;
no IR kind, no evaluator change, no public header.
- **Reused kinds** (D126): xsl:if (with 4.0 then/else), choose and
  switch are CONDITIONAL chains, each later test lowered in the previous
  else branch; switch coerces the select value to one xs:anyAtomicType
  and compares each test value by "=" (8.3); xsl:for-each is a MAP whose
  body is the sequence constructor with a SLOTS focus, separator a text
  node guarded by position() lt last() (7.1.1); xsl:try is TRY_CATCH,
  errors tokens as name tests, the nine $err: variables pushed as LOOKUPs
  of the details slot (stack-trace and additional are ()), the try body
  the children before xsl:catch (xsl:catch gives () where it stands).
- **Variable bindings** (9.3, 9.4): one procedure for xsl:variable,
  iterate xsl:param and xsl:with-param, and global initializers: select,
  else content (a document node unless "as" or a disqualifying child),
  else "" (or () with "as"); "as" coerces with XTTE0570 (a TRY_CATCH
  mapping XPTY0004) for variables and the TEMPLATE_PARAMETER role
  (XTTE0590) for parameters. A local xsl:variable is a LET in the
  sequence constructor's block and a scope entry until it ends (D142).
- **xsl:iterate** (7.2) is fn:scan(input, state, closure): no recursion
  (D71), 100,000 items in a unit test. The state is an array [control,
  output, parameters...]; the closure (every visible variable captured,
  D126; the size captured only when the body reads last()) unpacks the
  parameters, runs the body with focus (item, position, size), and reads
  the last item of its value: an iteration marker {Q{XSLT}iterate:
  [0, (), values...]} from xsl:next-iteration or {..: [1, value]} from
  xsl:break gives the next state, anything else keeps the parameters.
  After a break the remaining items return the state without output. The
  result is the states' outputs, then xsl:on-completion (focus absent,
  final parameters) unless a break happened. XTSE3120 (tail position,
  checked on the AST), XTSE3130, XTSE0670, XTSE0580, XTSE3125.
- **8.5**: two internal native rows (as merge-text-nodes): `vacuous`
  and `remove-deemed-empty` (arrays flattened on an explicit stack).
  where-populated filters its content; a sequence constructor with
  on-empty/on-non-empty children (skipped by `lower_child`, a hook in
  `src/xslt_lower.c` pushes `sxml_xslt_lowering_call_conditional_content`
  at its end) is `if (vacuous(R)) then on-empty-or-R else (children with
  each on-non-empty's value in place)`, each lowered in its branch with
  the variables before it in scope; XTSE0010 for an on-empty not last.
- **xsl:message, xsl:assert** (23): the body is a document node of the
  select value and the content (TRY_CATCH: a body that cannot be built is
  an empty document, the transformation does not fail); the internal row
  `xslt-message` hands it to the trace sink as an XSLT_MESSAGE event with
  is_terminating and the code; terminate (an AVT, fixed or compared when
  evaluated) then raises fn:error(code, string(body), body). error-code:
  EQName or lexical QName, resolved while lowering, or by the row
  `error-code` with the instruction's namespaces as a map constant; an
  invalid one is the default (XTMM9000, XTMM9001). Assertions are always
  enabled.
- **Globals** (9.5, 9.11): `sxml_xslt_lowering_declare_global_variables`
  before the main function (one per name by import precedence, XTSE0630;
  declared in every static context) and `_add_global_variables` after:
  initializers of arity 0 with the context global (D135) as fixed focus,
  wrappers in dependency order (references in the declaration's
  expressions, explicit-stack DFS: XTDE0640 for a cycle, XPST0008 for a
  self-reference), a required xsl:param raises XTDE0050. The main
  function reserves its context slot when there are globals; callers pass
  absent global slots (as the XQuery tests). Static parameters are
  recomputed from their select (the AST keeps no static value).
- **Others**: xsl:fork is its content; xsl:evaluate its xsl:fallback
  children or XTDE3175 (sxml claims no dynamic evaluation, D117; a real
  one needs a nested evaluation, not done). xsl:for-each with xsl:sort is
  UNSUPPORTED_FEATURE until 4.9 (which can sort the input atom before the
  MAP). Deviations: D110's missing zero-length text nodes change
  on-empty-113/114 and message-0304; dependencies through functions and
  templates are not seen.
- Measured with a scratch harness (each case's initial template, or its
  `match="/"` template, through `sxml_xslt_lower_initial_template`; xml,
  error and string assertions): of 181 runnable cases of choose,
  for-each, iterate, try, switch, where-populated, on-empty, on-non-empty,
  message, evaluate, assert and decl-variable, 105 pass (before: none of
  these instructions lowered); most of the rest need apply-templates or
  template parameters (D144's driver path, 4.15), stylesheet functions
  (4.10, 4.12) or current-date in the harness, 52 have XPath assertions.

### D146. The XQuery API and the sxml xquery command (packet 3.12)
Recorded 2026-10-17, after D106, D134, D135. `include/sxml/xquery.h` is
implemented in `src/xquery.c`; no header changed.
- **One evaluation path**: `src/xpath.c` exports its context checks and
  the evaluation of a sealed unit through `src/xpath_api_internal.h`
  (`sxml_static_context_configuration_is_valid`,
  `sxml_processor_static_context`,
  `sxml_dynamic_context_configuration_is_valid`, `sxml_evaluate_unit`,
  glossary `unit_evaluation`); the XQuery API compiles with the same
  static context and evaluates through the same function. Globals are
  bound by name except the context global (name 0), which no binding
  reaches. The unit's static base URI (the host's or a base-uri
  declaration's) is the one evaluation uses.
- **Compiling**: `src/xquery.c` parses with `sxml_xquery_build_ast`
  (a library module is XPST0003, as the header says), compiles with the
  new `sxml_xquery_compile_module` (`src/xquery_lower_internal.h`:
  lower, passes, seal over a caller's AST; `sxml_xquery_compile` is now
  parse + that) and reads the output declarations from the AST it kept.
  Module import stays UNSUPPORTED_FEATURE until 3.11 (the resolver hook
  is the header's).
- **Output declarations**: option names resolve against the unit's static
  namespaces (`sxml_xpath_resolve_name`); declarations in the
  serialization namespace become `sxml_serialization_parameter_text`s
  (glossary) read by `sxml_serialization_parameter_reader_read_texts`
  (`src/serialize_internal.h`), one reader per query owning the views:
  the parameter document first, then the declarations over it, method xml
  when nothing names one. The textual decoding is the parameter
  document's (`src/serialize_parameters.c` now decodes a value attribute
  and a declaration with one function; QNames resolve with a binding view,
  first binding of a prefix wins), so xsl:output can use it in Phase 4.
  Errors are static at the declaration: a value a parameter does not
  permit SEPM0016 (SESU0007/SESU0011 for encodings and normalization
  forms), XQST0119 when the document cannot be loaded (processor's
  resolver, kind XML_DOCUMENT, resolved against the static base URI),
  XQST0115 when it is not a valid parameter document. XQST0108 waits for
  library modules (3.11).
- **QT4 driver**: the adapter (D134) is deleted; the driver calls
  `include/sxml/xquery.h` and has no `src/` include any more.
- **CLI**: `sxml xquery (QUERY-FILE | --expression=TEXT)`,
  `sxml serialize [--method] [--indent] FILE`, and `sxml xpath --method`
  with adaptive, xml, xhtml, html, text, json. `--param` declares and
  binds for XPath, only binds for XQuery (the query declares it
  external; a host declaration would make the prolog's XQST0049). The
  three commands share `apps/sxml/query_command.{c,h}` (glossary
  `query_run`). `--output`/`--force` stay planned: the CLI framework has
  no file writer.
- **WebAssembly**: the CLI module grows from 1.83 MB to 3.45 MB stripped
  (767 KB gzipped) with the XQuery grammar; `javascript/size_budget`
  allows 3,700,000 / 820,000 for the CLI (Proposed below). `sxml.wasm`
  (1.83 MB) is unchanged; `compileQuery()` in the JavaScript package is
  left out because it would link the same grammar into it.

### D147. Counted repetition with counters (packet 2.19b)
Recorded 2026-10-17, after D53, D71, D105. Closes D105's "Not done".
(Completed by the Phase 3 review, D161, from the text 2.19b left
uncommitted.)
- **When**: a quantifier whose copies would add more than
  `SXML_REGEX_LARGEST_EXPANSION` (256) instructions, each copy counted as
  at least one, compiles to a counted loop (glossary): the atom once,
  between `COUNTED_LOOP_START` (counter := 0), `COUNTED_LOOP_CHOOSE`, a
  jump out, `COUNTED_LOOP_BODY` (progress := position) and
  `COUNTED_LOOP_END` (counter + 1, back to CHOOSE). Smaller counts keep
  D53's copies, which are faster (a loop pays four instructions and
  three backtrack entries per iteration); x*, x+ and x? are unchanged.
  `a{2147483647}` is six instructions; `(?:a{1000}){1000}` and
  `(?:(?:){1000}){1000}` now compile (they were LIMIT_EXCEEDED).
- **Matching**: CHOOSE iterates below the minimum, offers an iteration
  as a choice below the maximum (greedy first, reluctant second, section
  6.1.4) and leaves at it. Counter and progress are register slots (as
  progress registers are), set with RESTORE_SLOT entries like SAVE, so
  backtracking into an earlier iteration finds its counter, lookaround
  keeps working (a positive assertion keeps restoring entries), and
  nothing recurses (D71). The parameters (minimum, maximum, unbounded,
  reluctant, the two slots) are in a `counted_loops` table of the
  compiled regex, shared like classes by the copies of an enclosing
  atom: copies run one after the other and every slot change is undone
  in order, so they can share slots.
- **Empty iterations**: above the minimum an iteration that consumed
  nothing fails, as CHECK_PROGRESS in D53's unbounded loops; below it,
  it stands for all the iterations still needed (the counter jumps to
  the minimum), since each would again match nothing at the same place.
  This keeps `(a?){2147483647}b` fast and finds the same matching
  segments; which substring a group inside captures then is
  implementation-dependent (F&O 4.0, section 6.1.5).
- **Quantities** above 4294967295 still saturate to it (cbcl-matches-004:
  `a{99999999999999999999999999}` is false, which the test allows).

### D148. Library modules and module import (packet 3.11)
Recorded 2026-10-17, after D106, D134, D135, D146. No header changed; no
IR kind added.
- **Module set** (`src/xquery_module.c`, `src/xquery_module_internal.h`;
  glossary `module_set`): a work list over the set from the main module.
  Each module import asks the processor's resolver (kind XQUERY_MODULE)
  once per location hint, the hint resolved against the importing
  module's static base URI (its base URI declaration over the host's base
  or its module URI), or once with an empty uri_reference when it has
  none and no module of the namespace is loaded. A module is identified by
  its module URI (glossary): the resource URI, else the absolute hint,
  else the hint, else the namespace; one met again is the module loaded
  before, which ends cycles and loads a module once per compilation. The
  module URI is interned in the name pool, so error records may name it
  as their source after compilation. Errors: XQST0088, XQST0047 (one
  namespace imported twice by a prolog), XQST0070 (prefix xml/xmlns,
  before loading), XQST0059 (no module for a hint or a hintless import, a
  main module, another namespace); a library module's XPST0003 is located
  in it.
- **Imports name namespaces** (5.12.2): a module imports every module of
  the set whose target namespace it imports, itself excepted, and sees
  their public declarations only (not what they import, not %private).
- **Static contexts**: one per module, read in the reading order (a
  depth-first walk, imported modules first; a cycle is broken where the
  walk meets it), so every declared function is an IR function from 2 on
  and every module's prolog variables are globals numbered after the
  host's in that order. `sxml_xpath_static_context.global_numbers`
  (internal, NULL = position + 1) maps a module's variables in scope (its
  own, then the imported public ones) to those globals. A library module
  starts from XQuery's default static context (no host namespaces or
  variables, its module URI as static base URI, the host's functions and
  collations); its module declaration and imports bind prefixes
  (XQST0033); public functions, variables, item types and records outside
  its target namespace are XQST0048; output declarations XQST0108; a
  context value with a value XQST0113. Imported public item types are
  aliases of the importer (read before it). Linking adds the imported
  public variables and makes the function table find imported public
  functions and constructors (after the module's own, before the
  host's); XQST0049 and XQST0034 are checked against them. Duplicate
  named types are now XQST0146 (5.19), as QT4 expects.
- **Lowering**: the whole set lowers into the main module's unit; each
  library module's functions and initializers with a lowerer of its own
  (its static context and AST). The dependency graph of D135 spans every
  module, so one walk orders all variables and finds XQDY0054 through
  imports; unreached library variables are never computed.
- **CLI**: `sxml xquery` imports modules from the local files the hints
  name (its resolver already served them); a static error in a library
  module names that module's file.
- **Not done** (each in Proposed below or a later packet):
  fn:load-xquery-module (needs the evaluator); the static base URI and
  source name of a library module at run time (cbcl-module-002,
  base-URI-modules-001, xqhof16, xqhof18, try-catch-map3: the unit has
  one of each); named item types across an import cycle (modules-43);
  every module of a namespace for a hintless import
  (modules-collide-var-001, -fn-001: one request gets one module, as the
  QT4 driver serves the first); a library module's context value type
  (contextDecl-050, -051, -054: not checked).
- Measured (release, all 737 XQ40 test sets): 39,541 to 39,698 passed,
  no case lost; prod-ModuleImport 23 to 112 of 141 (94 errors to 1;
  the rest XQ10-only, schema import or above), prod-AxisStep.J +24,
  prod-ItemTypeDecl +9, prod-CompNamespaceConstructor +6, fn-id +5,
  prod-NamedRecordTypeDecl +5, prod-ContextItemDecl +5. misc-HtmlTestSuite
  (1,379) now gets past its import and stops at fn:parse-html;
  fn-load-xquery-module (141) and misc-Subtyping (159) stay "not run".

### D149. Keys and XSLT functions (packet 4.10)
Recorded 2026-10-17, after D101, D103, D118, D139, D142, D144.
`src/xslt_key.c` with `src/xslt_key_internal.h`, `src/function_library_xslt.c`,
`tests/xslt_key_test.c`; no IR kind, no public header.
- **XSLT function table** (glossary): `sxml_xslt_declare_functions` puts one
  table in front of every static context of the stylesheet (before 4.12's
  components table): stylesheet functions lowered into the unit, then the
  rows of the XSLT-only library parts (D101: parts whose language set has
  XSLT and not XPath), then the processor's table. A stylesheet function
  becomes an IR function when an expression first names it;
  `sxml_xslt_lower_functions` lowers the bodies asked for (twice: before the
  templates are released, again after the global variables).
- **Library part `xslt`** (`sxml_function_library_xslt_languages =
  SXML_LANGUAGE_XSLT`): system-property, available-system-properties,
  function-available, element-available, type-available (moved from
  `src/xslt_static.c`, which now serves its expressions the first five rows,
  `sxml_xslt_static_functions`), current, document (4.0 options map or base
  node), unparsed-entity-uri and -public-id (the zero-length value: trees
  keep no unparsed entities). Names given as strings resolve against the
  running unit's static namespaces (D84): a static expression's own, a
  stylesheet's those of the invocation's template (D142), not each
  expression's (Proposed below).
- **fn:current**: a parameter default `SXML_PARAMETER_DEFAULT_CURRENT_ITEM`
  (`function_library_internal.h`); lowering reads focus part
  `CURRENT_ITEM`, the focus the innermost embedding started with
  (`sxml_xpath_lowerer.embedding_focus`, set in the embedding frame), so in a
  pattern it is the item matched (D118). Inside an inline function it is
  UNSUPPORTED_FEATURE (it would need a capture).
- **Entries taking the caller's context value themselves**: an entry whose
  `parameter_types` already cover the parameters the caller supplies is
  called as it is instead of the entry of the longer arity
  (`src/xpath_lower.c`, static calls and references). fn:key#2 is an IR
  function (key-name, key-value, context) distinct from fn:key#3.
- **Keys** (20.2): the declarations of one name are one key (XTSE1222 when
  collation or composite differ; XTSE1205 for use and content, or neither).
  Its build function is `map:build` over the tree's nodes and attributes
  with the use values of each matching declaration (match-N: the D118 match
  function in a TRY_CATCH giving false, 6.5; use-N: the attribute or
  sequence constructor with the node as fixed focus) as keys; fn:key
  dispatches on the resolved name and looks values up in the index that the
  internal row key-index returns. Key values: string-like as xs:string, or
  their collation key under a non-codepoint collation; numbers as
  xs:double (eq promotes, so 3.7 matches number('3.7')); a composite key is
  one string of tagged canonical texts. Result: `/.` (document order, no
  duplicates); fn:key#3 keeps descendants-or-self of $top; fn:key#2 raises
  XTDE1270 when the root is not a document node.
- **Evaluation memo** (glossary; `src/evaluator.c`, additive to
  `evaluator_internal.h`): entries (computation address and number, subject
  node by identity) to a value, released when the evaluation ends. key-index
  calls the build function on a miss (a resumable body, D75) and stores the
  map: each index is built once per evaluation and tree, lazily. 4.12
  (xsl:function cache) and 4.14 (accumulators) can use the same memo.
- **fn:copy-of** is xsl:copy-of's copy (`sxml_xslt_lowering_bind_copy_of`,
  exported from `src/xslt_lower_construct.c`); **fn:snapshot** an XPath text
  over fn:copy-of, fold-right and computed constructors (ancestors with
  their attributes and namespaces).
- **Patterns** (D118 amended): a step whose predicates are all
  non-positional (each a comparison, logical, instance-of, castable or
  quantified expression, a string or boolean literal, nodes, or a call of a
  listed boolean or string function, and none names position or last) is
  tested on the item with a singleton focus instead of being selected again
  from its origin (6.3.2.3 gives the same result). attr/match runs in 12 s
  in a debug build (match-242 was 60 s alone).
- Left to other packets: current-group, current-grouping-key, current-merge-*
  (4.9), regex-group (4.11), accumulator-before/-after (4.14),
  current-output-uri (4.13), xsl:evaluate (4.7: XTDE3175, D145),
  fn:apply-templates (needs the mode at run time; 5 cases), fn:transform
  (out of scope), function-available at run time (the dynamic context's
  table is the host's: Proposed).

### D150. Functions and declarations (packet 4.12)
Recorded 2026-10-17, after D101, D135, D139, D142, D144, D145.
`src/xslt_lower_declaration.c`, `src/xslt_whitespace.c` with
`src/xslt_whitespace_internal.h`, `tests/xslt_declaration_test.c`; hooks
in `src/xslt_lower.c`, `src/xslt_lower_construct.c`,
`src/xslt_lower_template.c`, `src/xslt_lower_control.c`,
`src/processor.c`. No IR kind, no evaluator change, no public header.
- **Stylesheet functions** (10.3), as XQuery's declared functions (D135):
  `sxml_xslt_declare_components` (before templates) adds one IR function
  per arity of each component's arity range and puts a function table in
  front of every static context's (so patterns, AVTs, globals and 4.10's
  table see them); bodies are lowered after the templates (they may call
  them) with an absent focus, parameters as slots 1.., the result coerced
  to `as` (TRY_CATCH mapping XPTY0004 to XTTE0780). A shorter arity binds
  the defaults of the left-out optional parameters (`required="no"`;
  select, content as a temporary tree without `as`, else ()) and calls
  the full arity; a default "." is SXML_PARAMETER_DEFAULT_CONTEXT_VALUE
  (the caller's focus, supplied by the core); other defaults see no focus.
  Eclipsed functions (higher precedence, containing arity range) and
  those given up under override-extension-function="no" have no IR
  function. A function in no namespace switches the static contexts to
  NO_NAMESPACE_FIRST, the table falls back to fn, and unprefixed call
  names are interned first (as XQuery). XTSE0080, 0580, 0740, 0760,
  0761, 0769, 0770, XTSE0020 (override vs override-extension-function).
  cache, new-each-time, streamability and visibility are ignored.
- **Attribute sets** (10.2): all declarations of one name (by
  precedence, then order) are one IR function of the focus (item()?,
  position, size, as a template function) plus an absent-focus variant;
  each body is the invocations of every declaration's
  use-attribute-sets, then its xsl:attribute children.
  `sxml_xslt_lowering_bind_attribute_sets` calls them with the current
  focus; literal result elements, xsl:element and xsl:copy (with the
  selected item's focus) put the result before their own attributes.
  XTSE0710, XTSE0720 (explicit-stack DFS).
- **xsl:decimal-format**: merged per name by import precedence (XTSE1290
  only for a conflict at the winning precedence, XTSE1300 from
  sxml_decimal_format_apply) into the unit's static context (D131),
  which XSLT sets here, once; a packet adding collations to the unit's
  static context must merge with it.
- **xsl:global-context-item** (3.4.9): checked in the main function before
  the invocation: XTDE3086 (required, none), XTTE0590 (as), XTSE3088 (a
  supplied item when use="absent"); XTSE3087, XTSE3089 statically.
  4.8's `bind_has_focus` is now `sxml_xslt_lowering_bind_has_focus`. Item
  types of `as` (ITEM_TYPE values) are resolved as sequence types (their
  AST root is SEQUENCE_TYPE): this also fixes xsl:context-item's `as`
  (context-item 12 to 29 passed).
- **Whitespace stripping** (4.2.2): a copy through a filtering tree builder
  (the specification's "copy of the source tree"), so parsed trees never
  change and nothing recurses; best match by precedence, default
  priority, declaration order; xml:space; XTSE0270. The host strips the
  global context item (`sxml_xslt_strip_document`); fn:doc and
  document() get stripped documents through a new **document
  preparation** of the processor's document loader
  (`sxml_processor_document_loader_configuration.document_preparation`,
  applied to documents from the resolver, not to available documents),
  which `sxml_xslt_whitespace_stripping_as_document_preparation` makes.
  4.15 must install both.
- **Globals (extends D145)**: initializers are lowered before the
  templates are released (a global's content may call a template: it
  crashed before); the computation order also follows the components an
  initializer reaches: a graph over declarations (globals read, functions
  named, templates called, every template rule for apply-templates,
  attribute sets used), whose strongly connected components (Tarjan, on
  an explicit stack) give the order. Cycles through components are left
  to the evaluator; D145's direct cycles stay static errors.
- **Host parameters** (D112): a supplied stylesheet parameter is a BOUND
  global slot of the unit, found by name (the evaluator coerces it, and
  its initializer does not run); static parameters also go to the reader.
  Nothing else was needed for the harness; 4.15 maps
  `sxml_xslt_stylesheet_transform`'s parameters the same way.
- Not done: xsl:character-map (4.13 owns serialization), memoization
  (`cache`, `new-each-time="no"`: function-1025/1026/1031), a parameter
  left out before a keyword argument gets () not its default (the core's
  rule for library functions), function-lookup of stylesheet functions
  at run time (the dynamic context's table is the host's).
- Measured with a scratch harness (lower, evaluate, serialize; XPath
  assertions; host parameters): function 13 to 98 of 124, attribute-set 1
  to 47/49, strip-space 8 to 25/26, global-context-item 0 to 10/12,
  context-item 11 to 29/32, param 14 to 31/31, variable 16 to 90/108,
  whitespace 20 to 24/27 (before: HEAD of 4.8, without 4.7),
  format-number 66/70; 4.8's eleven sets 474 of 614 (D144: 316 of 615).

### D151. Phase 2 gate review (packet 2.20)
Recorded 2026-10-17 by the review agent (D11), after D95, D97, D123,
D132. Checked locally (D123): `tools/check-main.sh` on a clean HEAD
(c7c0c35) builds and tests `release`, `debug` (ASan, UBSan), `debug-gcc`
and `wasm` (Node.js package tests and size budget included) and lints
clean, with no call cycle.
- **Numbers** (code of ed225c2, `release`, `tools/run-qt4.sh`): XP40
  30,630 of 47,313 passed, 0 errors, no crash, every test set finished;
  in scope (D95) 30,612 of 31,543, **97.0 %**; by area, in scope: fn
  95.5 %, op 99.2 %, prod 97.8 %, map 99.7 %, array 99.8 %, math 100 %,
  xs 99.5 %. Slice 198/198. XQ40 for the record: 39,541 passed, in scope
  96.0 %, XQ-only 83.9 %, 1,568 errors (all module import, 3.11 running;
  1,379 of them misc-HtmlTestSuite). Fuzzing, 300 s per target, all five
  at once, then the two XPath targets 300 s more: cast_fuzz 564,842
  runs, regex_fuzz 320,438, xml_parse_fuzz 29,846, xpath_compile_fuzz
  7,679, xpath_evaluate_fuzz 3,097 (fork mode, 12 to 16 runs a second
  on a loaded machine); no crash, leak, sanitizer report or timeout.
- **Functions**: every function of F&O 4.0's signatures (282 names, looked
  up at arities 0 to 7) is in the library but D97's non-goals (html-doc,
  parse-html, invisible-xml, xsd-validator, transform) and
  fn:load-xquery-module, which D97 gives to 3.11.
- **D71 through function pointers**: native bodies that call function
  items are resumable (D75); the function-body, generator and resolver
  operations re-enter the evaluator only through
  `sxml_evaluator_call_function_item`, which refuses a running context
  (`is_running`); part tasks return to the machine. The lowering machine
  has no such guard: see the follow-ups.
- **Merged** (the owner's principle of conceptual smallness): the
  options of fn:element-to-map, fn:map-to-element, fn:unparsed-text and
  fn:unparsed-text-lines are read by `sxml_function_library_read_options`
  (two private readers removed; an unknown key that is not a QName in a
  namespace is now XPTY0004 there too, F&O 4.0, section 1.7); one
  `SXML_FUNCTION_OPTION` macro for option tables instead of five; one
  calendar rule, `sxml_year_is_leap` and `sxml_days_in_month`
  (`src/cast_internal.h`), instead of four copies; the unused
  `sxml_function_library_check_collation` removed. XP40 and XQ40 outcomes
  are identical before and after, case by case.
- **Headers for 0.2.0** (D47, D104, D138): consistent and documented
  (one undocumented declaration, `sxml_sequence_get_representation`, got
  its comment). `xquery.h` and `xslt.h` are **provisional** until the
  gates of Phases 3 and 4: their implementations are not done, and a
  Proposed item already changes `xquery.h`'s contract. A minor release
  may change them, each change with a record (`version.h`,
  `docs/RELEASE.md`, `CHANGELOG.md`).
- **Proposed**, the Phase 2 items: collations in min/max,
  highest/lowest, sort-by and array:sort were done by D136; only the
  evaluator's SORT (order by with a collation, XQuery) still takes the
  codepoint collation alone, for Phase 3. The item axis (D113) is
  accepted: `SXML_AXIS_ITEM` appended to `sxml_axis` (additive, D99)
  with XPath 4.0, 4.7.4's STEP semantics, and the JNode key test reading
  the EQName form, for the next sweep. The XSLT driver's report name and
  areas (D117) are accepted for 4.17. The three are out of Proposed.
  Left there: the owner's (the two WebAssembly budgets, XQuery's
  omit-xml-declaration default) and Phase 3 design work (D135's prolog
  follow-ups, D148's three items), for the Phase 3 gate.
- **Numbering**: two records were D120; the 3.3 one is D120b now, with
  its references. D127 follows D128 (recorded in finishing order); kept.
- **Follow-ups** (not done here): fn:deep-equal still reads its
  twenty-odd options itself (raise `SXML_FUNCTION_OPTION_LIMIT` from 10
  and merge); two calendar arithmetics (`days_from_civil` in `src/format_date.c`, the
  cycle days of `src/operator_temporal.c`); the evaluator's navigation
  analysis finds fn:root by its local name (a native-function property
  would say it); the lowering machine (`run_machine` in
  `src/xpath_lower.c`) should refuse to run inside a step, as the
  evaluator does, since XQuery and XSLT lowering run procedures and
  embedded lowerings "outside a machine" by convention only and the
  call-cycle check cannot see through step pointers (a probe that aborts
  on a nested run passed all 193 `debug` tests at c7c0c35, so the guard
  breaks nothing; `src/xpath_lower.c` was being edited by a running
  packet); size products
  `count * sizeof(type)` are mostly unchecked, safe where the count
  mirrors an array already allocated (a lint rule could require
  `sxml_size_multiply` for the others).

### D152. Sorting and grouping (packet 4.9)
Recorded 2026-10-17, after D79, D116, D118, D136, D137, D142, D144, D145.
`src/xslt_lower_group.c`, `src/xslt_group.c` (`src/xslt_group_internal.h`),
`tests/xslt_group_test.c`; hooks in `src/xslt_lower.c`,
`src/xslt_lower_control.c` (xsl:for-each), `src/xslt_lower_template.c`
(xsl:apply-templates), `src/xpath_lower.c` (context functions),
`src/evaluator.c` (SORT). No IR kind, no public header.
- **Evaluator SORT** (fixes 3.10's Proposed (a) and D116/D136's last
  codepoint-only body): a descending key reverses the whole order, empty
  keys included (XQuery 4.0, 4.13.9: "empty greatest descending" puts ()
  first); each key's collation is resolved once per SORT through the one
  resolver (`sxml_function_library_find_collation`, FOCH0002 otherwise,
  also for an empty input) and strings compare with
  `sxml_function_library_compare_collated` (glossary `sort_key_rule`).
  `sxml_ir_sort_key` gains `order` (an xs:string atom, "ascending" or
  "descending", read when evaluated: an xsl:sort order AVT) and
  `is_composite` (XSLT 4.0, 13.1.2: a key is a sequence compared item by
  item, the shorter first; otherwise more than one item is XPTY0004 as
  before). The text form prints them; the verifier checks the atom.
- **xsl:sort** is one SORT key per xsl:sort, under for-each,
  apply-templates (the select value is sorted before the MAP or
  APPLY_TEMPLATES), perform-sort and for-each-group
  (`sxml_xslt_lowering_call_sort`). AVTs use the instruction's focus;
  data-type number/text convert the key (`data(K) ! number(.)`), a QName
  converts nothing; lang/case-order without collation give
  `UCA?fallback=yes;lang=L;caseFirst=C` (D116's fallback, lang ignored),
  none the codepoint collation. Fixed invalid values are XTSE0020,
  computed ones XTDE0030 when evaluated; XTSE1015, XTSE1017, XTSE1040;
  a TRY_CATCH maps XPTY0004 to XTDE1030 and FOCH0002 to XTDE1035.
  Stable is always.
- **xsl:for-each-group**: a MAP over the population makes one grouping
  pair per item ([item, keys] atomized, untyped as string, distinct for
  group-by; [item, matches] for patterns: the match expression lowered in
  place with the item as fixed focus, errors are no match, 6.5); the row
  `for-each-group($pairs, $method, $collation)` of `src/xslt_group.c`
  returns group arrays [current group, current grouping key]; group-by
  calls the XQuery group-by row (D137) over [item, key] tuples (composite
  keys one member per item, padded with empty members), so keys compare
  exactly as XQuery's; adjacent runs and pattern boundaries are the row's.
  xsl:sort sorts the group arrays with the initial item as context item,
  the group's position and count, and the current group bound. The body
  is a MAP over the groups whose focus is the initial item with the MAP's
  position and size slots. XTSE1080, XTSE1090, XTTE1100, XTDE1110.
- **xsl:merge** is the same parts: per merge source (for-each-item,
  for-each-source as `doc(resolve-uri(., base))`, or none) tuples
  [source number, item, keys...] (keys with a singleton focus), all
  tuples sorted stably by the first source's merge keys (SORT keys are
  tuple members, `tuple_key_offset`), runs of equal keys by the row
  (composite, the first key's collation), and the merge-action as the
  group body. XTSE2200, XTSE3190, XTSE3195, XTSE3200.
- **Context functions** (glossary): in XSLT, fn:current-group(),
  current-grouping-key(), current-merge-group(), current-merge-key(),
  current-merge-key-array() read the variable Q{XSLT}local-name the
  instruction put in scope (XTSE0080 keeps the namespace out of
  stylesheet variables); a variable bound to NONE (pattern grouping) or
  none in the current function raises XTDE1061/1071/3480/3510 when
  evaluated. `name#0` makes a closure over the value (4.0: it retains
  it). current-merge-group($source) is map:get of Q{XSLT}
  current-merge-sources. Lowering-wide XPath ASTs (short texts, match
  expressions) are kept until the lowering ends
  (`sxml_xslt_lowering_keep_ast` in `src/xslt_lower.c`, glossary
  `kept_ast`; 4.14 uses them too).
- Deviations: the current group is lexical, so a template called from
  the body sees none (XTDE1061; the spec makes it dynamic there);
  split-when and merge-when are UNSUPPORTED_FEATURE; sort keys of groups
  are evaluated with the group's position; merge inputs are always
  sorted (no XTDE2220, XTDE2210), equality under the first key's
  collation, a composite merge key is the keys' concatenation;
  current-merge-group with an unknown name gives () (not XTDE3490), with
  () an error.
- Measured with a scratch harness (each case's initial template or
  match="/", xml/error/string/file assertions; release of HEAD 2f0c640
  plus the packet): insn/sort 74 of 82 runnable cases (before: 0, xsl:sort
  was UNSUPPORTED), insn/for-each-group 79 of 86 (before: 0), insn/merge
  61 of 88 (before: 0); the rest need xsl:function, schemas, collections,
  xsl:number or accumulators, or are the deviations above (split-when,
  XTDE2210, XTDE2220, XTDE3490). QT4 XQ40: prod-EmptyOrderDecl 28 to 32,
  prod-OrderByClause 139, prod-GroupByClause 37, unchanged; ctest
  (debug, debug-gcc, wasm) and lint pass.

### D153. Numbering and analyze-string (packet 4.11)
Recorded 2026-10-18, after D105, D114, D118, D119, D142, D145, D149, D152.
`src/xslt_lower_number.c` (rule view `sxml_xslt_instruction_rules_number`,
one line in `src/xslt_lower.c`), `tests/xslt_number_test.c`; additive
edits to `src/format.c`, `src/function_library_format.c`,
`src/function_library_xslt.c`, `src/xpath_lower.c` and
`src/function_library_internal.h`; no IR kind, no evaluator change, no
public header.
- **xsl:number** (12) is 12.3's formal definition as XPath texts with
  their inputs as variables (D152's way): the selected node (XTTE0990,
  also for an absent focus, XTTE1000), the nodes count is tried on (the
  ancestors-or-self and their preceding siblings; for level="any" the
  preceding nodes and the ancestors-or-self), a FILTER of them by the
  count pattern's match expression (D118, inline, the item as fixed
  focus, an error is no match, 6.5; without count, a text for the
  selected node's kind and name), the same for from (on the
  ancestors-or-self, or the any-level candidates), then one text per
  level: matches-from's root case is `(from | root($s))[last()]`, a
  counted node's preceding siblings are the counted candidates with its
  parent before it. value (12.2) is `xs:integer(round(number(.)))`, exact
  for xs:decimal, XTDE0980; start-at (12.1) re-bases in a text
  (XTDE0030); XTSE0975 statically.
- **Formatting** is one internal row, format-place-marker
  (`src/xslt_number_internal.h`, defined beside fn:format-integer), over
  D119's engine: `src/format.c` splits the format into prefix, format
  tokens with their separators (". " before a number formatted by the
  first token) and suffix, and `sxml_integer_formatting` gained
  grouping-separator/grouping-size (a string every N digits of a decimal
  representation). An unparsable token is "1" (12.4). Ordinal: any value
  but "", no, 0, false. lang (xs:language or "") and letter-value
  (alphabetic, traditional) are checked (XTDE0030) and otherwise ignored:
  English only (D97). An empty place marker gives prefix and suffix
  (Saxon's behaviour; QT4 number-0806, -3205 expect it). Title-case words
  are "Twenty Two" here (Saxon's; number-0105, -2506), "Twenty-two" in
  fn:format-integer.
- **xsl:analyze-string** (17.1) is fn:matching-segments (one regex
  engine, D105, D114) of the select value coerced to `xs:string?`, the
  non-matching segments put between the matches by an XPath text
  (strings; matches stay fn:matching-segment-records), inside a TRY_CATCH
  mapping FORX0002/0001 to XTDE1140/1145; then a MAP over the segments
  whose body is a CONDITIONAL of the two children (select or sequence
  constructor) with the segment's string as context item in a slot of
  its own and the MAP's position and size. XTSE1130, XTSE1132, XTSE3185.
  compiled-regex is UNSUPPORTED_FEATURE.
- **Captured groups, a context variable** (glossary): a new parameter
  default `SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE` makes the caller pass
  the variable Q{XSLT}N (N the parameter's name) lexically in scope, or
  () (captured from an inline function only when captured; a function
  item of the shorter arity captures it, so regex-group#1 keeps the
  groups, 17.2.2). xsl:matching-substring has Q{XSLT}regex-groups =
  the segment's groups map with entry 0 (value, position, nr 0) first;
  xsl:non-matching-substring has (). fn:regex-groups and fn:regex-group
  (rows of the `xslt` part) read it; regex-group(N) also finds a named
  group by its nr (analyze-string-406, as XSLT 3.0 had it). 4.9's context
  functions (current-group and the others, D152) can use the same
  default instead of a second mechanism.
- Measured with a scratch harness (lower, evaluate, serialize; XPath
  assertions through `sxml xpath`) over the in-scope cases (D95; the
  combinations_for_numbering ones are not): insn-number 152/161,
  insn-analyze-string 56/62, misc-regex 64/65, fn-format-number 67/70
  (with 4.12's decimal formats); before the packet none of the
  instructions lowered. Of the rest: German words (6), Unicode digit
  sequences, the four dynamic-scope cases below, xsl:sort under
  xsl:number's callers, an attribute-set case.
- Deviations: the captured groups are lexical, so a template called from
  xsl:matching-substring sees none (17.2 says they pass through
  apply-templates and call-template; QT4 analyze-string-078 to -081): an
  implicit tunnel parameter would carry them (src/xslt_lower_template.c).
  A start-at known statically is checked when evaluated (XTDE0030, not
  XTSE0020, number-0109). German and other languages, the Unicode
  numbering sequences (circled digits, number-0819..0821, out of scope by
  combinations_for_numbering otherwise) and traditional letter values
  fall back to the token "1" or English.

### D154. XQuery conformance sweep 1 (packet 3.14)
Recorded 2026-10-18, after D71, D75, D95, D135, D148. No public header
changed; no IR kind added.
- **Global tables** (glossary `global_table`): the dynamic context keeps
  one table of global slots per unit (the evaluation's own first), and a
  task reads the table of its function's unit. The evaluator's
  restriction "only the evaluation's own unit reads globals" is gone; a
  unit with no table (a function item called after its evaluation ended)
  still reads none, so closures keep capturing globals (D55).
- **Unit evaluation** (glossary `unit_evaluation`): a resumable native
  step kind, `SXML_NATIVE_STEP_KIND_EVALUATE_UNIT`
  (`src/function_library_internal.h`), asks the machine to evaluate
  another sealed unit on its own task stack: a global table from the
  step's global values (coerced, XPTY0004), its initializers
  (`sxml_evaluator_start_global_initializers` now takes the table), its
  main function, and the next step gets the value, as after a CALL. The
  tables live until the evaluation (or `call_function_item`) ends. No C
  nesting (D71).
- **fn:load-xquery-module** (F&O 4.0, 18.2; `src/xquery_load_module.c`):
  XQuery's own function table, put after the host's by
  `sxml_xquery_compile_module` for one compilation, so only programs that
  compile XQuery link it (the JavaScript package's `sxml.wasm` does not,
  D146); fn:function-lookup does not see it (18.2 says the context is not
  changed; QT4 does not ask). Its first step writes the module record
  (glossary `module_record`) as a main module, `import module namespace
  sxml-loaded = "NS" at hints; map { "variables": map {QName(NS, v):
  $sxml-loaded:v, ...}, "functions": map {QName(NS, f): map {N:
  sxml-loaded:f#N, ...}} }`, as 18.1 words it ("as if by evaluating a
  named function reference Q#N"): the public names are read from the
  ASTs of a first pass that loads the import alone; the content option
  is served by a resolver in front of the evaluation's, as the module of
  the namespace asked for without hint, with the caller's static base URI
  as module URI. The second step returns the record the unit evaluation
  made. Variables' values bind globals by name (any module of the set)
  after an exact type match (FOQM0005, no conversion); context-value and
  context-item (both: FOQM0005). Errors: FOQM0001, FOQM0002 (the
  import's XQST0059), FOQM0003 (any other static error), FOQM0006
  (xquery-version above 4.0, no evaluation environment); dynamic errors
  of globals as they are. The QT4 driver claims feature
  fn-load-xquery-module for XQ40 runs only. Not done: trusted=false
  (FODC0016, 11 cases), a library module's context value type (D148).
- **QT4 driver**: an out-of-scope case (D95) that ends in XPST0017 or
  `SXML_STATUS_UNSUPPORTED_FEATURE` is "not run: needs the non-goal ...",
  so misc-HtmlTestSuite (1,379, fn:parse-html) and fn-parse-html are no
  longer failures; leaks, wrong results and other errors of such cases
  still count. A param's QName prefix is resolved on its element.
- **Arena trees**: blocks of 4 KiB instead of 64 KiB; each element
  constructor makes a tree, and 10,000 of them took 655 MB
  (fold-left-021 and slice-185 ended in the driver's memory limit).
- **Measured** (release, all 737 sets of each spec, code of 00ac981):
  XQ40 39,698 to 39,924 passed, in scope 96.4 % to 96.9 %, XQ-only
  85.3 % to 97.9 % (9,321 of 9,523; the non-goal cases no longer run),
  "error" outcomes 5 to 1 (modules-40, below), none lost but
  fn-system-properties-supports-dynamic-xquery-false (now not run, as
  the feature is claimed); fn-load-xquery-module 0 to 103 of 117 run,
  misc-Subtyping 0 to 109 of 110; method-* 97.5 %. XP40 unchanged:
  30,630 passed, in scope 97.0 %, 0 errors (32 out-of-scope failures now
  not run). Slice 198/198.
- **Not done** (Proposed below): closures reading globals at call time
  (D135 (c)); defaults that read the focus (D135 (b)); keyword arguments
  after an omitted parameter of a declared function (modules-40, the
  last "error"); a library module's static base URI and source name at
  run time; hintless split namespaces; ##any; function-lookup of
  declared functions. Each needs `src/xpath_lower.c`,
  `src/ir_internal.h` or `include/sxml/xquery.h`, which other packets
  were editing.

### D155. Accumulators (packet 4.14)
Recorded 2026-10-18, after D118, D144, D149, D150. `src/xslt_accumulator.c`
with `src/xslt_accumulator_internal.h`, five internal rows in
`src/function_library_xslt.c`, `tests/xslt_accumulator_test.c`; hooks in
`src/xslt_lower.c` (declare, lower, scope of the main function, release,
the rule view; field `accumulators` of `sxml_xslt_lowering`),
`src/xslt_lower_construct.c` (xsl:copy-of) and `src/xslt_key.c`
(fn:copy-of, fn:snapshot). No IR kind, no evaluator change, no public
header.
- **Lowering** (as templates, D144): each xsl:accumulator-rule is a match
  function (its pattern, D118, in a TRY_CATCH making every error false,
  6.5) and a rule function of ($node, $value) with the node as fixed focus
  and `$value` (no namespace) bound, its select or sequence constructor
  coerced to `as` (XPTY0004); initial-value the same over the root. One
  declaration per name, the highest import precedence (XTSE3350 at a tie);
  XTSE3355, XTSE3185, XTSE3300 (use-accumulators of xsl:mode and
  xsl:source-document: names of accumulators, none twice, #all alone).
  streamable is read and ignored.
- **Traversal** (19.4, no recursion, D71): per accumulator an IR function
  lowered from an XPath text (as src/xslt_key.c's, D149): a fold-left over
  `$root/descendant-or-self::node()` (attributes and namespaces are not
  visited) whose state map holds the value, an explicit stack of nodes
  whose end event is pending, and two maps from generate-id to the pre-
  and post-descent values; each node first closes the stack entries that
  are not its ancestors (end events, innermost first), then has its start
  event; the last rule in document order of the phase whose pattern
  matches gives the value. Depth is found in one step for a first child or
  a sibling, else by the ancestor axis.
- **Values** (19.3, lazily per tree): fn:accumulator-before/-after are IR
  functions of a function table put in front of every static context
  (after the XSLT function table, before 4.12's). Row accumulator-values
  builds once per evaluation, accumulator and root, through the evaluation
  memo (D149, reused: no new store); while building, the entry is the
  empty sequence, so a rule asking for its own accumulator raises
  XTDE3400; a failed build leaves a retry mark, so every later call raises
  the error again (19.5). XTDE3340 (accumulator-name: an xs:QName, or an
  EQName resolved as fn:key's names), XTTE3360.
- **Applicability** (19.2): row accumulator-scope records the applicable
  accumulators of a tree in the memo: the main function for the initial
  match selection (use-accumulators of the initial mode's xsl:mode; none
  without one), xsl:source-document for its document (with
  streamable="yes" only accumulators declared streamable); every other
  tree has all of them; accumulator-values raises XTDE3362 for one left
  out.
- **Copies** (19.10): fn:copy-of, fn:snapshot and xsl:copy-of with
  copy-accumulators="yes" link each copy to its original in the memo (row
  accumulator-link, only when the stylesheet declares an accumulator);
  the accumulator functions map a node under a linked copy to the node at
  its place in the original and read that tree's values.
- **xsl:source-document** (18.1, no streaming): fn:doc of href, the scope,
  the content with the document node as singleton focus (no packet owned
  it).
- Not done: streaming; XTDE3350 (an absent focus raises XPDY0002);
  xsl:merge-source use-accumulators (4.9's xsl:merge); a copy of a copy;
  19.5's functional dependency (one failing rule fails every call on that
  tree). Costs: a node deeper than a first child or a sibling pays its
  depth; a call on a linked copy pays the copy's size; memo entries are a
  list (many linked copies make lookups linear).

### D156. Output and result documents (packet 4.13)
Recorded 2026-10-18, after D106, D112, D120b, D128, D144, D145, D146, D150.
`src/xslt_output.c` with `src/xslt_output_internal.h`, the library part
`src/function_library_output.c` (XSLT only), `tests/xslt_output_test.c`;
hooks: one rule view in `src/xslt_lower.c`, declarations in
`src/xslt_lower_internal.h`, the tunnel mark in `src/xslt_lower_template.c`,
`result_writer` in the evaluation environment (`src/processor_internal.h`).
No IR kind, no evaluator change, no public header.
- **Stylesheet outputs** (glossary), compiled from the sealed AST and
  keeping nothing of it (4.15 makes them next to the unit): output
  definitions, named character maps, result document instructions.
  An xsl:output's attributes become serialization parameter texts (D146;
  booleans yes/no, names as EQNames, html-version as a decimal) merged
  over the definition's declarations (highest precedence wins, XTSE1560
  for two at that precedence unless a higher one follows; the name lists
  of cdata-section-elements and suppress-indentation are the union) and
  read by `sxml_serialization_parameter_reader_read_texts` over the
  parameter document (read at compile time through the resolver; a
  document that cannot be found is ignored, 26.1.5; its parameters win
  over the attributes, so texts it gives are dropped). build-tree is read
  here; omit-xml-declaration defaults to no; has_serialization_method
  when a text or the document gives method. Reader errors (SEPM0016,
  SESU0007, ...) become static errors at the declaration.
  `sxml_xslt_stylesheet_outputs_get_definition` is what
  `sxml_xslt_stylesheet_get_output_definition` returns.
- **Character maps** (26.1.6): the declaration of highest precedence per
  name (XTSE1580 for a tie there), expanded once each in post-order on an
  explicit stack (XTSE1590, XTSE1600), concatenations merged "later wins"
  by a heapsort into one sorted map; use-character-maps of an output
  definition concatenates in increasing precedence, then the parameter
  document's maps; of xsl:result-document over the definition's (26.2.4).
  `fn:character-map` (26.1.7) returns one as a map.
- **xsl:result-document** (26.2) is two internal rows around its content
  (`let $o := result-document-open(N, (values), temporary) let $raw := ...
  return result-document-close($o, $raw)`), N the AST node index naming
  its result document instruction, values its value templates in a fixed
  order. Open checks XTDE1480, resolves format (XTDE1460) and href against
  the base output URI, checks XTDE1490, merges the effective definition
  (the definition's texts with the instruction's over them, "" absent
  for doctype-*, "#absent" for item-separator, the instruction's
  parameter document over all, output-version as version) and makes the
  URI the current output URI. Close calls the handler's open, delivers
  (DEFAULT by build-tree, RAW, TREE, SERIALIZED) and close: the handler
  sees each result document when its raw result is complete, so open and
  close alternate as D112 says and a nested result document comes first.
  A document whose content failed is dropped (the handler never saw it).
- **Temporary output state** (26.2.6): lexically inside xsl:variable,
  param, with-param, function, key, sort, accumulator-rule, merge-key a
  RAISE of XTDE1480; dynamically, an xsl:apply-templates or
  call-template there puts Q{urn:x-sxml:output}temporary-output-state
  into the tunnel parameters it passes, and open tests the received map;
  a function call invocation sets the writer's flag.
- **Delivery** (shared by both results): `sxml_xslt_serialize_result`
  (the method given or chosen from the raw result, 26.1.2; indent DEFAULT
  is yes for html and xhtml), `sxml_xslt_build_result_tree` (the
  serializer's sequence normalizer into an arena tree, base URI the
  result's URI), `sxml_xslt_result_builds_tree`. The **result writer**
  (glossary) delivers the principal result after the evaluation
  (`sxml_xslt_result_writer_deliver_principal_result`: XTDE1490, an empty
  principal result with a result document of its URI discarded).
- **fn:current-output-uri**: the writer's current output URI (the
  innermost open result document, else the base output URI); absent in
  temporary output state (function call invocation) or without a base
  output URI. Not cleared in functions, global variables and patterns
  (26.3.1): a context variable (D153) bound by those lowerings would do
  it (current-output-uri-001, -005, -401).
- **Collector**: the public `sxml_xslt_result_document_collector_*` of
  xslt.h is implemented here (values or bytes by URI, the byte bound), so
  4.15 does not need to.
- Not done: XTDE1500 (reading a written URI), disable-output-escaping
  (ignored, as Serialization 4.0 allows), both parameter documents of
  xsl:output and xsl:result-document at once (the instruction's replaces
  the definition's), validation and type of xsl:result-document.
- Measured with a scratch harness (lower, evaluate with a result writer
  and a test handler, deliver; serialization assertions on the principal
  result serialized by its definition, assert-result-document by URI, a
  result document with the base output URI standing for an empty
  principal result): decl/output 235/244, decl/character-map 32/33,
  insn/result-document 129/149 (17 need accumulators or xsl:merge),
  insn/xsl-document 23/25, fn/current-output-uri 6/16 (the clearing
  above, initial functions, the harness's base output URI). Before: no
  xsl:result-document was lowered and xsl:output was not read.

### D157. The XSLT API, the XSLT driver's runs and sxml transform (packet 4.15)
Recorded 2026-10-18, after D112, D117, D139, D144, D145, D149, D150, D156.
`src/xslt.c` implements `include/sxml/xslt.h` (unchanged), imitating
`src/xquery.c`; no IR kind, no evaluator change, no public header.
- **Compile**: the principal module is the text or one resolver request
  (XSLT_MODULE: the reference resolved against the static base URI;
  XSLT_PACKAGE: name and package-version, "*" for none; XTSE0165 and
  XTSE3000 when not delivered, XTSE3000 for a principal package that is
  no xsl:package), read with the static parameters and the default
  collation (D139), lowered once as apply-templates in the default mode
  (every static error of the lowering is found here), its outputs
  compiled (D156), a function library over the XSLT parts made, the
  static parameters retained. The AST is kept: any other invocation
  (call-template, an initial mode, an initial match selection, a function
  call) lowers its own unit when a transformation asks (the main function
  is the invocation, D144). On failure an error location names the
  caller's name for the principal module (the AST that its module URIs
  borrowed from is gone); collation bindings of the host are not passed
  (the reader takes only a default collation).
- **Shared evaluation** (accepts 4.10's Proposed item, D149):
  `sxml_unit_evaluation` gains `extension` (`src/xpath_api_internal.h`):
  a function table for the dynamic context (the XSLT library parts, so
  function-available and fn:function-lookup see the XSLT functions), the
  document preparation (strip-space for fn:doc and document(), D150) and
  the result writer; XPath and XQuery pass NULL. Names given as strings
  still resolve against the unit's static namespaces (per-expression
  namespaces are left open).
- **Invocation globals** (glossary): the run-time values of an
  invocation are globals of the unit after the stylesheet's globals and
  the context global, named `Q{urn:x-sxml:invocation}template-parameters`,
  `tunnel-parameters` (maps from xs:QName, default the empty map; only
  when the lowering request sets `reads_invocation_parameters`, which
  src/xslt.c always does), `initial-match-selection` and
  `function-arguments` (an array; members coerced to the parameter types).
  src/xslt.c binds them by name like stylesheet parameters, so one
  mechanism carries everything the host supplies. Apply-templates
  without a selection or focus is XTDE0044; an initial function that is
  not visibility="public"/"final" (2.4.5: xsl:stylesheet functions are
  private) or has no IR function of that arity is XTDE0041.
- **Prime and run**: the global context item is stripped
  (`sxml_xslt_strip_document`); static parameters, then the request's
  stylesheet parameters, then the invocation values are bound by name
  (XTDE0050 is the initializer's, computed eagerly); a result writer per
  transformation (temporary output state for a function call) delivers
  the raw result. Changed in D156: a result document whose absolute URI
  is the base output URI (no href) is the principal result, delivered as
  the request asks (CLI stdout), not to the handler; XTDE1490 when the
  raw principal result is not empty.
- **Driver** (`tests/xslt/driver/xslt_execution.c`, D117's plan): a
  budgeted processor per case; the resolver is the QT4 resource table
  over a never-loaded QT4 "file catalog" with XSLT_MODULE served as
  XML_DOCUMENT and XSLT_PACKAGE as the matching <package>; params by
  XPath; no source, initial template or mode means call-template
  (xsl:initial-template); a serialization assertion without <output>
  serializes (the bytes parsed back for the other assertions); result
  documents to the collector, messages to a trace sink. Assertions
  (`sxml_xslt_test_check_assertions`): all-of/any-of/not on an explicit
  stack, assert-message (best over messages) and assert-result-document
  call the new `sxml_qt4_check_assertion_element` (the QT4 checks of one
  element), serialization-matches and assert-serialization on the
  serialized bytes as an xs:string (assert-serialization: equal after
  normalize-space, or deep-equal once parsed). Embedded stylesheets are
  not run. `tools/run-xslt.sh` (one process per test set, XT40/) and
  `tools/conformance-report.py` (XT40: areas by suite group, the twelve
  core sets, the XSLT slice; the QT4 report's JSON names its program,
  accepts 4.1's Proposed item).
- **CLI**: `sxml transform [--param]... [--initial-template] [--initial-mode]
  [--method] STYLESHEET [INPUT]` (docs/CLI.md); the stylesheet's base URI
  is `file:PATH` (the local resolver reads `file:` relative paths), so
  includes and fn:doc resolve against the stylesheet; --param values are
  passed as static parameters too; result documents with an href are
  not written; messages to stderr. The WebAssembly CLI's size budget is
  raised to 5,000,000 / 1,100,000 bytes (Proposed).
- Fixed on the way: allocation-failure paths in `src/xslt_lower_control.c`
  (a double release of the global arrays, arithmetic on NULL) and
  `src/xslt_lower_template.c` (`allocate_zeroed` leaves NULL on failure);
  the reader passes the XML parser's diagnostic of a module that is not
  well-formed.
- Measured (release, `tools/run-xslt.sh`, whole suite): in scope 7,585 of
  8,728 passed (86.9 %; D95 gate 70 %), 785 failed, 7 errors (memory and
  step limits: array-022, error-0640a/g, function-1031, json-to-xml-013,
  result-document-1501/1502), all 274 test sets finished. Core sets:
  apply-templates 97.4, mode 87.3, match 92.8, for-each-group 90.7,
  sort 100, key 97.1, number 57.5 (114 in-scope cases need
  combinations_for_numbering, such as circled digits, which format-integer
  lacks), choose 95.4, copy 88.4, value-of 53.3, result-document 94.0,
  function 89.5 %. Top causes: wrong results 460 (assert-xml 148, assert
  78), XPST0017 70 (packages, fn:transform, fn:apply-templates,
  map:for-key, stream-available), XTSE0340 49 (priority patterns),
  XPST0008/XTSE0650/XTSE0710 around packages (4.16), XPST0051 26
  (record and type aliases), XTDE1450 25 (call-template-ext). Not done:
  function-available and fn:function-lookup of key, accumulator-before/
  after and stylesheet functions at run time (they are unit functions of
  the lowering, not library rows), per-expression namespaces for names
  given as strings, collation bindings of the host, an implicit package
  (an xsl:stylesheet) as RESOLVED_PACKAGE (Proposed).

### D158. XQuery conformance sweep 2 (packet 3.14)
Recorded 2026-10-18, after D135, D148, D154. Closes D135 (b) to (g) and
D154's "Not done" but where said. No public header changed; no IR kind.
- **Default functions** (glossary `default_function`, D135 (b), (g)): a
  declared function's optional parameter whose default is not `:= .` is
  `SXML_PARAMETER_DEFAULT_EXPRESSION` with a `default_function_number`
  (`sxml_function_parameter`): an IR function of three parameters, the
  caller's context value, position and size, whose body is the default
  coerced to the parameter's type, lowered with those slots as its focus;
  its IR function's `focus` names the parts it reads (the lowering focus
  records `parts_read`). Static calls go to the full arity and call the
  default functions of the omitted parameters with their own focus
  (`sxml_xpath_lowerer_call_default_function`), so keyword arguments
  after omitted parameters work (modules-40) and `$x := string()` sees
  the caller's context value (5.18.3). A reference F#N whose omitted
  defaults read the focus is a closure that captures those parts and
  calls the defaults when called (so `$x := (., local:f#0)` ends); a
  reference made without a focus is the shorter arity's IR function,
  which calls the defaults with its absent focus. Defaults are lowered
  first in every module; one that calls a default not lowered yet passes
  all parts without reading them, and a fixpoint over the default
  functions' calls adds the parts they pass on.
- **Closures of XQuery globals** (D135 (c)): `find_free_variables`
  captures no global in XQuery (the unit's global table is read at call
  time, D154); the dependency walk ignores a variable's edge to itself
  (a read while it is computed is BEING_COMPUTED, XQDY0054). XPath and
  XSLT still capture (function items outlive their evaluation there).
- **fn:function-lookup** (D135 (d)): `sxml_evaluator_find_unit_function`
  finds a top-level named IR function of the calling task's unit (XQuery
  declared functions, XSLT stylesheet functions) before the host's
  table.
- **"##any"** (D135 (e)): `has_any_default_element_namespace` in the
  XPath static context; prefix 0 is bound to no namespace (constructed
  elements); an unprefixed name test or element test is `*:local`, an
  unprefixed type name is in xs (`sxml_xpath_resolved_name`'s
  `is_any_namespace`).
- **QT4 driver** (D135 (f)): assert-eq atomizes a single node.
- **IR modules** (glossary `ir_module`; D148's Proposed): a unit keeps
  one (source name, static base URI) per library module
  (`sxml_ir_unit_add_module`); every IR function a library module's
  lowering makes carries its `module_number`; the evaluator's static base
  URI and error locations read the function's module.
- **Trust** (F&O 4.0, 18.2): `trusted` (default false) marks the loaded
  unit untrusted (`sxml_ir_unit_set_untrusted`), as is every unit an
  untrusted call loads; fn:doc, unparsed-text(-lines), unparsed-binary,
  collection and uri-collection raise FODC0016 when the running unit is
  untrusted (`sxml_dynamic_context_check_trust`).
- **A library module's context value type** (D148): without a main
  module declaration, the first library module's type is the context
  global's (fn:load-xquery-module checks it, FOQM0005).
- **Function annotations**: IR functions carry their %annotations,
  copied into the unit (`sxml_ir_unit_add_function_annotations`,
  `sxml_xpath_lowerer_read_annotations`) for inline and declared
  functions; partial applications keep them (static and fn:partial-apply).
- Also: keyword placeholders take their written order (FunctionCall-416);
  an xs:untypedAtomic array lookup key is cast to xs:integer (4.7.3).
- **Not done**: hintless imports of a namespace split over modules (needs
  a resolver contract in `include/sxml/xquery.h`, kept as D148 says); a
  fixed default's one value for every call (function-decl-40-060); the
  focus of fn:function-lookup (XP and XQ, 47 cases); static-base-uri#0
  made in a library module and called elsewhere (xqhof16, 18); "fixed"
  default element namespaces; both main and library context value types
  at once (contextDecl-050, -051).
- **Measured** (release, all 737 sets of each spec; before at 01cf728,
  after at 5611dd2): XQ40 39,928 to 40,025 passed, in scope 96.9 % to
  97.2 %, XQ-only 9,325 to 9,404 of 9,523 (97.9 % to 98.8 %), failed 518
  to 422, "error" outcomes 1 to 0 (the Phase 3 gate's last open count);
  none lost; prod-FunctionDecl 211 to 227, fn-function-annotations 14 to
  23, fn-load-xquery-module 103 to 111, prod-DefaultNamespaceDecl 71 to
  78, app-Demos 4 to 11; method-* 97.5 %. XP40 30,630 to 30,646, in scope
  97.0 % to 97.1 %, 0 errors, none lost. Slice 198/198.

### D159. XSLT conformance sweep 1 (packet 4.17)
Recorded 2026-10-19, after D119, D149, D153, D155, D157.
- **Numbering sequences** in the shared picture engine (`src/format.c`,
  so fn:format-integer and xsl:number both have them): a token of one
  codepoint names an alphabet (glossary: a, A, Greek alpha with final
  sigma, Alpha without U+03A2), counted as before by bijective
  numeration, or a symbol sequence (glossary): one symbol per number over
  runs of codepoints, decimal outside them: circled digits 0 to 50,
  parenthesized and full-stop digits to 20 (DIGIT ZERO FULL STOP for 0),
  digit-comma 0 to 9, dingbat and double circled digits, circled and
  parenthesized ideographs, Aegean, Coptic epact, Rumi, Brahmi, Sinhala
  archaic, counting rod and Mende Kikakui numbers. The kinds
  LOWER_/UPPER_ALPHABETIC became ALPHABETIC and SYMBOLS with a
  `sequence_index`. The XSLT driver declares the 19 combinations.
- **Kept builds** (`src/function_library_xslt.c`): key-index and
  accumulator-values share one step (`step_kept_build`): the memo entry
  is the empty sequence while the build runs, so a key whose pattern or
  use calls fn:key for itself raises XTDE0640 (it ran out of memory),
  as accumulators raise XTDE3400.
- **Guarded matches**: the four copies of "an error while matching is no
  match" (keys, accumulators, group-starting-with/ending-with, xsl:number
  count/from) are `sxml_xslt_lowering_make_guarded_match`, whose clauses
  raise XTDE0640 and XTDE3400 again (6.5: circularity overrides).
- **Filters** (`src/evaluator.c`): a FILTER whose focus slots are all
  unused (the predicate does not depend on the focus) computes the
  predicate once and, for a single item or (), applies it as a constant
  predicate: `$s[$k]` takes one item instead of testing every position
  (it was quadratic: 20,000 accesses took 26 s, now 0.02 s).
- **xsl:analyze-string** takes its segments from the internal row
  analyze-segments (`src/xslt_number_internal.h`, defined in
  `src/function_library_regex.c`: fn:matching-segments with the gaps as
  strings) instead of fn:substring per gap, which is linear per call on
  non-ASCII text (result-document-1501/1502 ran out of steps).
- **fn:map-for-key** (20.2.3) as two stylesheet functions over the key
  index (XTDE1262 for composite or collated keys, XTDE1270);
  function-available sees the running unit's named functions and the fn
  functions lowering implements (key, map-for-key, copy-of, snapshot,
  accumulator-before/-after).
- The snippet-text helpers of `src/xslt_key.c` and
  `src/xslt_accumulator.c` are one module, `src/xslt_snippet_text.c`.
- **Measured** (release, whole XT40): in scope 7,585 (b67c93f) to 7,737
  of 8,728 with this packet alone (86.9 % to 88.6 %), and 7,879 (90.3 %)
  at 4cb75fd with packet 4.16's packages; errors 7 to 3; number 57.5 to
  97.4 %, result-document 94.0 to 95.3 %, map-for-key 0 to 6 of 8, error
  447 to 449. XP40 30,646 and XQ40 40,025 passed, unchanged; slice 198.
- **Not done** (causes found): value-of (53.3 %) needs zero-length text
  nodes (Proposed) and the cdata attribute; the "~T" type patterns of
  priority and type-alias (XTSE0340) are not in the 13 October 2026
  draft, which has atomic(), so they were left; the remaining errors are
  memory (json-to-xml-013, array-022: about 9 KB per small tree, each
  element constructor builds a tree of its own, and about 1.5 KB per JSON
  object) and xsl:function cache="yes" (function-1031); static-base-uri,
  base URIs of constructed nodes and names given as strings still read the
  unit's static context, not the expression's (Proposed); call-template-ext
  (extension instructions as template calls, 10.1.3) needs the reader to
  keep extension attributes as expressions; host collation bindings.

### D160. Packages (packet 4.16)
Recorded 2026-10-19, after D112, D117, D139, D148, D157. `src/xslt_package.c`
with `src/xslt_package_internal.h` (the package table, glossary); no IR
kind, no evaluator change; `include/sxml/xslt.h` changes one comment
(provisional, D151).
- **D148's design for XSLT**: a package is a set of modules whose
  components have a visibility. One AST holds every package: the reader
  asks the resolver for each xsl:use-package (kind XSLT_PACKAGE, the name
  as uri_reference and absolute_uri, the package-version or "*") and reads
  the package's principal module in place as a module of relation
  USED_PACKAGE, the top of a package (XTSE3000 when nothing is delivered,
  XTSE3005 for a package that uses itself, XTSE3008 for a use in an
  imported module). It is read again for every use, so each use has
  components of its own (3.4.5: a package used twice may be overridden
  twice), and parsed once per compilation (same name and range: the
  document is reused). The children of xsl:override are declarations,
  each with a static context of its own (where xsl:original is looked up).
  For precedence a USED_PACKAGE module counts as imported, so an
  override's template rules come before the used package's (3.4.6). One
  lowering lowers every package's declarations into one unit.
- **The table** is made first in the lowering: the package of each module
  and static context; per package, from the last to the first, one
  component instance per own winning declaration (visibility from
  xsl:expose and the visibility attribute, 3.4.5.1; an xsl:stylesheet's
  named templates and modes are public), per overriding declaration (its
  visibility, else private) and per accepted component (xsl:accept, else
  public/final become private; hidden ones are left out), each instance of
  the used package pointing at its successor in the user. **Binding**
  (3.4.5.5): the referring package's instance, then the successors; the
  last one's declaration is called. xsl:original binds to the overridden
  instance's declaration (3.4.5.4). Stylesheet parameters are public, as
  in XSLT 3.0 and the suite (4.0's 3.4.5.1 says private; the 3.4.5.2 rule
  for using packages says public).
- **Lowering**: functions (each static context's function table binds
  through its package; xsl:original in an overriding function), attribute
  sets (one per name and package; xsl:original in use-attribute-sets),
  global variables (one per name and package; each package's static
  contexts get a view of the names it binds with D148's `global_numbers`,
  plus $xsl:original in an overriding variable; a global no view binds, an
  overridden or hidden abstract one, gets no initializer), named templates
  (call-template, xsl:original), modes (a mode is a package and a name:
  each package has its own unnamed mode, a named mode binds like a
  component, #all is the template's package's modes, XTSE3085 per
  package), and the invocation: the top-level package's public or final
  initial template and function, an initial mode it declares public or
  final or does not declare. Abstract components' bodies raise XTDE3052.
  The existing checks between declarations (XTSE0630, XTSE0660, XTSE0770,
  XTSE0545, attribute set merging) apply within a package.
- **Static errors**: XTSE0020 (package-version of xsl:package or
  xsl:use-package), XTSE3003 (another name, a version out of range),
  XTSE3010, XTSE3020, XTSE3022, XTSE3025 (xsl:expose), XTSE3030,
  XTSE3032, XTSE3040, XTSE3051 (xsl:accept), XTSE3050, XTSE3055,
  XTSE3058, XTSE3060, XTSE3070 (arity ranges, parameter, result, variable
  and context item types identical by subtyping both ways, tunnel and
  required flags), XTSE3080.
- **Versions** (3.4.1): `sxml_xslt_package_version_matches` (versions,
  "*", lists, V.*, V+, to V, V to W). The resolver chooses the version;
  the driver (`tests/xslt/driver/xslt_package_version.c`) serves the
  highest one the range matches, settling D117's
  package_version_resolution as highest_version, and reads the name and
  version of a catalog <package> without uri= from its file.
- **D157's Proposed item accepted**: a RESOLVED_PACKAGE principal module
  may be an xsl:stylesheet or xsl:transform (3.4: an implicit package); a
  simplified module stays XTSE3000 (`src/xslt.c`, the xslt.h comment).
- **Not done**: keys, accumulators, decimal formats, namespace aliases,
  strip-space, outputs and named types are still stylesheet-wide, not per
  package (use-package-101, -104, override-misc-006); xsl:with-param of
  xsl:use-package and a used package's own static parameters;
  xsl:package-location (the resolver decides); XTSE3075; an abstract
  variable a view binds is computed eagerly, so XTDE3052 also when it is
  never read (accept-042, -043a: XSLT globals are all computed first).
- Also: a library package's global variables have no global context item
  (2.3.2, XPDY0002); the reader's `decide_role` no longer dereferences a
  missing rule (a lint finding).
- Measured (release, `tools/run-xslt.sh`, in-scope passed before -> after):
  decl/package 53 -> 68 of 72, override 30 -> 91 of 102, accept 33 -> 48
  of 50, expose 12 -> 42 of 42, attr/package-version 25 -> 33 of 34,
  use-package 13 -> 34 of 53 (34 of the 45 run: the 8 others need
  package_version_resolution lowest_version or unspecified, and one
  streaming); the six sets +150. Whole suite 7,585 -> 7,864 in scope
  (4.17's numbering included). No other test set lost a case.

### D161. Phase 3 gate review (packet 3.15)
Recorded 2026-10-19 by the review agent (D11), after D95, D123, D132,
D151. Checked locally (D123): `tools/check-main.sh` on a clean HEAD
(84f3178) builds `release`, `debug` (ASan, UBSan), `debug-gcc` and
`wasm`; every test passes but two of the XSLT driver
(`xslt/list_dependencies`, `xslt/run_json`): 4.17's numbering commit
50a3651 made "CIRCLED DIGIT ONE" a supported combination and the goldens
still skip it (Phase 4, 4.17's files; told to the orchestrator).
  The full lint reports three findings, none in Phase 3 code: a bare
  bool parameter in `tests/xslt/driver/xslt_assertion.c`, a byte pointer
  without length in `src/xslt.c` (4.15) and a null dereference the
  analyzer finds in `src/xslt_read.c` (4.16); no call cycle.
- **Numbers** (code of 50a3651, `release`, tools/run-qt4.sh and
  tools/run-xslt.sh, every test set finished, 0 crashes, no leak
  reported by the drivers' budgets): XP40 30,646 of 47,313 passed (as
  at the Phase 2 gate: no regression), 0 errors, in scope 30,628 of
  31,543, **97.1 %**. XQ40 40,025 passed, 0 errors, in scope 40,007 of
  41,174, **97.2 %**; XQ-only 9,404 of 9,523, **98.8 %**; method-* 433 of
  508 (85.2 %; 97.5 % of the 444 run). Slices: qt4/slice 198/198,
  qt4/xquery-slice 186/186 (ctest, all presets). XT40 for the record:
  7,693 of 13,867 passed, in scope 88.1 %, 7 errors (Phase 4).
- **Fuzzing**, 300 s per target, all six at once on a loaded machine:
  cast_fuzz 434,140 runs, regex_fuzz 174,843, xml_parse_fuzz 26,453,
  xpath_compile_fuzz 5,517, xpath_evaluate_fuzz about 2,100 and the new
  xquery_fuzz about 2,400 (fork mode); no crash, leak or sanitizer
  report. One xquery_fuzz timeout, a QT4 seed that loops 200,001 times
  building a map each time: slow by nature under ASan (P9), listed for
  triage only, as for xpath_evaluate_fuzz.
- **D71 through function pointers**: the evaluator's guard holds
  (D151); fn:load-xquery-module runs its module on the same machine
  (EVALUATE_UNIT, D154), compilation inside a native step runs a lowering
  machine of its own lowerer, which never evaluates. The lowering
  machine now refuses to run while one of the same lowerer runs
  (`is_running` in `sxml_xpath_lowerer`, SXML_STATUS_INVALID_ARGUMENT),
  D151's follow-up: every test passes, so no step of XQuery or XSLT
  lowering starts a machine. Serializers and tree builders call no
  evaluator.
- **Merged** (conceptual smallness): one in-place heapsort,
  `sxml_sort_in_place` (`src/sort_internal.h`), instead of the copies in
  `src/xquery_static.c` (prolog checks), `src/xquery_parse.c` (attribute
  names of a direct constructor), `src/evaluator_global.c` (initializer
  order) and `src/xslt_output.c` (character maps); one parameter document
  reader, `sxml_parameter_document_load` with
  `sxml_processor_compile_document_loader_initialize`
  (`src/processor_internal.h`), for XQuery's output:parameter-document and
  xsl:output's and xsl:result-document's parameter-document (two loaders
  and two document-element walks removed). With every change of this
  review, XP40 outcomes are identical case by case (release, every test
  set) and XQ40 gains five (fn-function-lookup-759, -760, -762, -763 and
  fn-system-properties-supports-dynamic-xquery, below), losing none:
  40,030 passed;
  XT40 differs only in the package sets that 4.16 changed meanwhile
  (accept, override, use-package, expose, ...), none in the output and
  serialization sets.
- **Fuzzing XQuery**: `tests/fuzz/xquery_fuzz.c` compiles, evaluates and
  serializes a main module through `xquery.h` alone, on a processor with
  small limits and a resolver table with one library module and one
  document; seeds are the QT4 queries and `.xq` files
  (`tools/fuzz-seeds.py`); it runs in fork mode like xpath_evaluate_fuzz
  (`tools/fuzz.sh`).
- **xquery.h is stable** (D151's rule): consistent with `xpath.h` and
  `xslt.h` (memory_requirements, initialize, finalize, create, destroy;
  configuration and request structs), every declaration documented,
  every function tested through the public API with an
  allocation-failure loop (`tests/xquery_test.c`), the CLI on it alone.
  From now on it changes as every public header does (D47). The one
  behaviour still open is the owner's: XQuery's omit-xml-declaration
  default (D146, Proposed); choosing "no" changes the header's comment
  with a record. `version.h`, `docs/RELEASE.md` and `CHANGELOG.md` say so.
- **Proposed, resolved here**: hintless imports of a namespace with
  several modules (D148): the contract is kept (XQuery 4.0, 5.12 leaves
  the set of modules implementation-defined; two QT4 cases fail). The
  prolog follow-ups (D135 (b'), (h); D158's "Not done") are accepted as
  designs for a later sweep, no owner decision: a fixed default is a
  default function without focus parts whose value the evaluation memo
  keeps; fn:function-lookup passes the call's focus as hidden arguments
  that may be absent. Left for the owner: the three WebAssembly budgets,
  XQuery's omit-xml-declaration default; for Phase 4: the XSLT
  functions at run time, D159's and D160's items.
- **Delayed features (D162)**: each is rejected or ignored in one place
  with the specification's error: XQuery's validate
  (XQST0075, `src/xquery_lower.c`), schema import (XQST0009,
  `src/xquery_static.c`), %updating (XQST0045, the reserved-namespace
  check of `src/xquery_static.c`), update and full-text syntax
  (XPST0003, the grammar), UCA with fallback=no (FOCH0002,
  `src/collation.c` alone; the evaluator and xsl:for-each-group pass its
  error on), named time zones (FODT0004, `src/function_library_temporal.c`),
  calendars and languages of dates (`src/format_date.c`: "[Calendar:
  AD]", "[Language: en]") and of numbers (English words and ordinals in
  `src/function_library_format.c`, for fn:format-integer and xsl:number
  alike), fn:parse-html, fn:html-doc, invisible XML, fn:transform,
  xsd-validator and EXPath (not in the library: XPST0017), DTD validation
  and XInclude options (FODC0013, the shared option reader), streaming
  (streamable is read only where a non-streaming processor must read it:
  xsl:mode's properties, accumulator scope; source documents are built
  whole). Fixed: fn:load-xquery-module was known to static calls only (XQuery's
  function table lived for one compilation), so fn:function-lookup did
  not find it and supports-dynamic-xquery of fn:system-properties said
  false; the query now keeps that table and gives it to its evaluations
  (the unit evaluation extension), and the property asks the dynamic
  context's table (XQ40 fn-system-properties-supports-dynamic-xquery
  passes; XPath still says false, rightly). Scattered or missing
  (XSLT, files in flight for 4.16 and 4.17, so listed, not merged): the
  processor's capabilities are two tables, fn:system-properties
  (`src/function_library_json.c`) and xsl:system-property
  (`src/function_library_xslt.c`), which should be one; XTSE1660 is
  raised for default-validation strict or lax in `src/xslt_read.c` but
  not for validation="strict"/"lax" or type attributes of instructions,
  and xsl:import-schema is read and ignored instead of raising XTSE1650
  (XSLT 4.0, 25: a non-schema-aware processor raises both, at the
  attribute reader, one place); backwards-compatible behaviour
  (version below 3.0) is not detected at all, where XSLT 4.0, 3.9 asks
  XTDE0160 of a processor without it.
- **Follow-ups** (not done here; the files are 4.16's and 4.17's): the
  XSLT lowering files keep their own copies of small helpers (a heapsort
  in `src/xslt_lower_declaration.c` and `src/xslt_lower_template.c`,
  `resolve_declared_type` three times, `slot_atom` nine times across
  XQuery and XSLT, `add_expression` seven); XML whitespace trimming is
  written about fifteen times (`trim_whitespace`, `trim_spaces`, ...);
  grouping keys compare in both `src/xquery_flwor.c` and
  `src/xslt_group.c`. Each is small; one shared helper each, at the
  Phase 4 gate at the latest.

### D162. D97's exclusions are delays, not non-goals (owner)
Recorded 2026-10-18 on the owner's word: "D97 is accepted only as a delay --
these features need to be supported ultimately." Amends D97 and the "Not in
year one" list of PLAN.md.
- D97 still fixes what Phases 2 to 4 deliver and what their gates count
  (D95); nothing in those phases changes.
- Everything D97 leaves out is owed later, not dropped: full UCA (DUCET,
  tailoring, numeric and case-order options; advanced-uca-fallback),
  fn:parse-html and fn:html-doc (an HTML5 tree builder), invisible XML,
  schema awareness (validate, XSLT validation and type attributes),
  streaming, the XQuery Update Facility, Full Text, EXPath, XPath 1.0
  compatibility mode and XSLT backwards-compatible behaviour,
  civil-timezone and Olson names (a timezone database), calendars other
  than AD and ISO and languages other than English in the formatting
  functions, and fn:transform.
- Schema awareness stays Phase 5. The rest becomes Phase 7 ("Deferred
  features" in PLAN.md), planned in `docs/tasks/phase-7.md` after the
  Phase 4 gate, one packet per feature, each with its QT4 or xslt40-test
  sets counted once it lands.
- Until then, code that rejects or ignores one of these features does it
  in one place each, with the error the specification gives a processor
  without the feature, so that adding it later replaces that place
  instead of unpicking special cases (the design principle in CLAUDE.md).
  Size-driven choices (UCA tables, a timezone database, HTML5) are
  revisited against the wasm budget then; a feature may be a build option.

### D163. XSLT conformance sweep 2 (packet 4.17)
Recorded 2026-10-19, after D99, D148, D159, D160, D161, D162.
- **Zero-length text nodes** (D159's Proposed item, accepted; additive to
  the frozen `include/sxml/tree.h`, D99): XDM 4.0, 7.5.7.1 lets a text
  node without a parent be empty ("text nodes are allowed to be empty
  only if they have no parents"). The builder contract: a parentless text
  whose events are all empty is a zero-length text node; inside a parent
  empty text still makes none, so the node disappears when it is added
  to content. TEXT_CONSTRUCTOR (XQuery `text {""}`, xsl:value-of,
  xsl:text) makes it; simple content drops zero-length text nodes first
  (XSLT 4.0, 5.7.2 step 1). D110's deviation is gone.
- **CDATA requests** (additive to tree.h): `sxml_text_event.
  is_cdata_section` and `sxml_node_description.is_cdata_section`
  (glossary `cdata_section`) carry XSLT 4.0, 11.4.3's cdata attribute of
  xsl:value-of and xsl:text (construction flag CDATA_SECTION; a computed
  value is one CONDITIONAL). A text node built from several events keeps
  the request when one has it (implementation-defined; value-of-012
  expects mixed nodes and fails). Copies keep it; the serializer writes
  such a text node as CDATA sections with the XML and XHTML methods.
- **Sized arena trees**: the first node segment holds 2 nodes, a growable
  tree without initial memory takes 768 bytes with its own allocation as
  the arena's first memory, location segments are made only when kept,
  the base URI is an exact allocation, the text store starts at 64 bytes.
  A constructed tree of a few nodes is about 1.3 KB (was 5 to 9 KB).
  JSON parsing reuses the key items of a parse (a cache of 32 keys).
  array-022 (209,579 small element trees) runs within the driver's
  512 MB.
- **The evaluation memo** is an open-addressing hash; subjects are the
  same when they are one node, atomic items of one type that are
  atomic-equal, or arrays whose members are, item by item.
  **xsl:function cache="yes"** (10.3.7; not for new-each-time="yes"): the
  full arity calls the internal row cached-call(number, [$p1, ...],
  body#n), body the sequence constructor's IR function; equal arguments
  give the first call's value (function-1031).
- **The static context of a call at run time** (D159's Proposed item,
  accepted, D149's names-as-strings item with it): one mechanism, the
  unit's modules (glossary `ir_module`, widened from D148): a module may
  carry its own decimal formats and collations and its own static
  namespaces besides its static base URI; STATIC_CALL and the node
  constructors gained `module_number`. XSLT lowering makes one module per
  AST static context (xml:base, namespaces, [xsl:]default-collation, the
  package's decimal formats, 3.4.8) and the XPath lowerer stamps the
  function calls of an expression with its context's module
  (`sxml_xpath_static_context.ir_module_number`); XSLT stamps the
  elements, documents and temporary trees it constructs. The evaluator's
  lexical getters (`sxml_dynamic_context_get_static_base_uri`, new
  `_get_static_context`, `_get_static_namespaces`) read the innermost
  IR task's expression module, then its function's, then the unit's.
  So fn:static-base-uri, resolve-uri#1, doc, base URIs of constructed
  nodes, default collations, names given as strings (fn:key,
  system-property, function-available, format-number) and decimal formats
  per package follow the expression. Not covered: fn:function-lookup of
  fn:key and the accumulator functions (lowered snippets, not natives),
  and XSLT's own library calls (they read the unit's).
- **Declarations local to a package** (D160's Proposed item, accepted;
  3.4.8): one rule, each named-declaration table is keyed by name and
  the package of the declaration, and a reference looks up its own
  package's (as attribute sets and modes already were): keys (the XSLT
  function table has one state per package, so fn:key and
  fn:map-for-key of package P see P's keys; the accumulator table keeps
  each static context's inner table instead of the first context's),
  namespace aliases (collected after the package table), decimal formats
  (through the modules above), character maps and output definitions
  (each package has its unnamed definition; xsl:result-document reads its
  package's). The output compilation, which runs without the package
  table, names a package by its top module
  (`sxml_xslt_package_top_module`, the same rule the table uses).
  Accumulators themselves are still stylesheet-wide.
- **D162 one-place rules** (from D161): XTDE0160 for an instruction whose
  effective version is below 2.0 (3.8, 27.4: XSLT 2.0 and 3.0 modes run
  as 4.0) in `lower_child`; XTSE1650 (xsl:import-schema) and XTSE1660
  ([xsl:]type, validation="strict", typed modes) in the reader's
  `reject_schema_awareness`; lax and preserve are taken as strip (27.2).
  One system property table (`src/system_property.c`, glossary
  `system_property`) serves fn:system-properties and the XSLT
  fn:system-property.
- **Merges**: `sxml_ir_slot_atom` (10 copies), `sxml_xml_text_trim_
  whitespace` (16), `sxml_xslt_lowering_resolve_declared_type` and
  `_expression_context` (3 and 2), the two remaining heapsorts onto
  `sxml_sort_in_place`, one boolean-attribute helper (yes, true, 1).
- Lint at HEAD fixed: invocation global names are byte views
  (`src/xslt.c`), the driver's serialization comparison returns its
  verdict, `decide_role` reads a RULE frame's rule once.
- **Measured** (release, whole suites, before at 9e7f5fc, after at the
  last commit of this packet): XT40 in scope 7,879 to 7,986 of 8,728
  (90.3 % to 91.5 %), errors 3 to 1; value-of 8 to 14 of 15 (53.3 % to
  93.3 %, every core set now 80 % or more), seqtor 61 to 85 of 92, text 9
  to 15, base-uri 32 to 49 of 52, collations 23 to 34, resolve-uri 15 to
  21, use-package 34 to 41 of 53, error 465 to 470, function 111 to 113;
  no test set lost a case. XP40 30,646 to 30,647 and XQ40 40,030 to
  40,034 passed, 0 errors; slices 198 and 186.
- **Not done**: json-to-xml-013 still runs out of the driver's 512 MB: it
  turns a 19 MB file into 209,579 documents that an xsl:for-each returns
  as one sequence before the enclosing element copies them, and then a
  tree of 2.7 million nodes (2 GB without a budget); it needs content
  that streams into the constructor (Proposed). XTDE3052 for an abstract
  variable is still raised when the globals are computed, read or not
  (accept-042, -043a: XSLT globals are all computed first, so a lazy
  error needs lazy globals). Accumulators are stylesheet-wide;
  fn:function-lookup does not find fn:key or the accumulator functions
  (they are lowered stylesheet functions, not rows).

### D164. Phase 5 starts now, alongside the end of Phase 4 (owner)
Recorded 2026-10-18 on the owner's choice ("Start Phase 5 now"), after they
asked for a way to work with XSD schemas in the `sxml` command. Amends D132
and PLAN.md's order: Phase 5 (XSD 1.1 validation and schema-aware typing)
no longer waits for the Phase 4 gate. Its packets are planned in
`docs/tasks/phase-5.md` by a design packet (5.1), which also vendors the
W3C XSD test suite (D13) and proposes the gate numbers. The Phase 4 gate
still closes Phase 4; Phase 5 work must not regress its counts. A `sxml
validate` command enters the CLI only once it works (D44).

### D165. Phase 5 design: schema.h, the substrate additions, the XSD suite (packet 5.1)
Recorded 2026-10-18. Plans Phase 5 (D164) in `docs/tasks/phase-5.md`;
completes D98's deferred "registering schema-derived atomic types" and
the Phase 5 notes of D32, D53, D58, D97 and D162.
- **Suite** (D13): W3C `xsdtests` at `7bc3365c652a322f3d762021b3879eb92dae7e30`
  in `third_party/xsdtests` (42,174 files, 54 MB), refreshed by
  `tools/update-test-suites.sh`, excluded from private snapshots and
  linked (`tools/private-build.sh`, `tools/check-main.sh`). Dropped, as no
  test reads them: the reports' HTML, CSS and images (`nistMeta/*.html`,
  25 MB) and CVS backup copies (`.#FILE.REVISION`). In scope: the tests
  of `suite.xml` applicable to an XSD 1.1 processor (version tokens 1.1,
  XML-1.0, XML-1.0-5e, Unicode_6.0.0, CTR-all-compile, full-xpath-in-CTA,
  comments-and-PIs-excluded), not queried or disputed, expecting valid
  or invalid: 41,538 (15,330 schema tests, 26,208 instance tests; NIST
  23,170, Microsoft 14,363, Sun 1,606, Saxonica 1,376, IBM 919, others
  104). The other token choices move the count by at most 47.
- **schema.h** (new, after `xslt.h`'s pattern): `sxml_schema` (quintet)
  compiled from `sxml_schema_document`s given as TEXT, NODE (a parsed
  document or an inline xs:schema element) or RESOLVED through the
  resolver; `sxml_schema_find_type`, `_find_element_declaration`,
  `_find_attribute_declaration`; `sxml_schema_validator` (quintet) with
  `_as_tree_builder` and `_get_outcome`; `sxml_schema_validate_node`;
  `sxml_schema_report_sink`. XSD 1.1 only (1.0 documents are read as
  1.1); schema location hints in instances are not followed (Part 1,
  4.3.2 allows it); an import that cannot be read is not an error by
  itself (4.2.6.2).
- **Validation is a tree builder filter**, not a tree walker: the
  validator receives builder events (from the XML parser while parsing,
  from `sxml_tree_builder_copy_node`, from construction) and sends them,
  annotated, to a result builder; the null builder makes it a pure check
  (`sxml validate` builds nothing). One mechanism serves the CLI, XQuery
  validate, XSLT validation, fn:doc validation and fn:xsd-validator, and
  the validated tree is an ordinary arena tree, since the events already
  carry type annotation, is-id, is-idrefs and nilled (`tree.h`, D39).
  Defaults and schema normalized attribute values are events too. A
  failed validation delivers no tree (its events are discarded).
- **Errors**: codes are XSD constraint names in no namespace
  (`cvc-complex-type.2.4`, `src-resolve`; Part 1, appendix C), static
  errors for compilation and dynamic errors for validation; XQuery and
  XSLT wrap them in their own codes, as XSLT rewrites XQDY0102 to
  XTDE0430. A report sink receives every error (the CLI shows all of
  them); the null sink stops at the first.
- **types.h** (frozen; additive, D99): `sxml_content_type_variety` and
  `sxml_complex_type` as the new union member `complex_type` of
  `sxml_type_description` (xs:anyType and xs:untyped MIXED;
  `src/types_builtin.c` now says so), `sxml_facet_kind`, `sxml_facet`,
  `sxml_facet_view` and the new last field `facets` of the description
  (empty for built-in and item types; `src/types.c` clears it when it
  normalizes an item type). Reason: "everything the registry knows about
  one type" must include what typed values (content variety, simple
  content type) and casting (facets) need. Facet values are text (the
  registry holds no items, as for JNode key constants), canonical in the
  base type, QNames as EQNames so no namespace context is needed.
  Assertion facets are not registry facets: they are XPath, kept by the
  schema, evaluated by validation and, in schema-aware expressions, after
  the cast, so `src/cast.c` never starts an evaluation (D71).
  Comments: the top of the header and `sxml_type_registry_find_type`.
- **Registration is internal**: `sxml_type_registry_register_schema_type`
  in `src/types_internal.h` (declared now, defined by 5.5), called only by
  schema compilation, which checks XSD's constraints first; a public
  function would let hosts register definitions that bypass them, and the
  registry would need a second copy of those checks. Schema types are
  never interned and are registered without entering the name index:
  names belong to a schema (`sxml_schema_find_type`), as D98 made named
  item types aliases of a module, so two schemas of one processor may
  define one name. Registered types outlive their schema (trees keep
  their annotations).
- **tree.h, xdm.h** (frozen; contract comments): a tree without
  `get_typed_value` gets typed values derived from string value and type
  annotation (simple types by casting with the element's namespaces,
  complex types by content type variety, nilled: none) in
  `sxml_node_make_typed_value`. The old text said such a tree holds only
  untyped nodes; for those nothing changes, so no tree that kept the
  contract changes behaviour. Reason: one derivation for every tree
  (arena and host trees alike) and nothing stored per node; the arena
  tree stays as it is.
- **processor.h** (with a record, D47): `SXML_RESOURCE_KIND_SCHEMA_DOCUMENT`
  at the end of the kinds (forms BYTES, DOCUMENT_NODE), using
  `target_namespace_uri` for the namespace; the QT4 driver's resolver
  lists it (`tests/qt4/driver/resource_table.c`).
- **Reuse, not parallel machinery**: schema documents through the XML
  parser and an arena tree (D12); facets and lexical spaces through
  `src/cast.c` alone; patterns through the regex engine with an XSD
  dialect flag (D53); assertions, type alternatives and identity
  constraints compiled with `xpath.h` and evaluated by the one evaluator
  on subtrees the validator records; schema-element(N) and
  schema-attribute(N) resolved statically to choices of the existing
  element(M, T) and attribute(N, T) tests over the substitution group,
  so matching and subtyping need no schema in the type registry (static
  analysis then no longer sets the `is_schema_declaration` flag of
  `sxml_node_test`, D58).
- **Integration** (5.11, 5.12), each replacing one rejection place
  (D161, D162): `sxml_static_context_configuration.in_scope_schemas` (a
  view of compiled schemas, last field, empty = not schema-aware) with a
  record by 5.11; XQuery import schema (XQST0009, `src/xquery_static.c`)
  and validate (XQST0075, `src/xquery_lower.c`); XPST0008 of
  schema-element in `src/xpath_static.c`; FODC0009 of the xsd-validation
  option (`src/function_library_json.c`); fn:xsd-validator (absent,
  XPST0017); xsl:import-schema (XTSE1650) and validation and type
  attributes (XTSE1660, `src/xslt_read.c`); the QT4 and XSLT drivers'
  feature lists.
- **Gate (proposed, for the owner)**: in-scope XSD 1.1 tests ≥ 90%, the
  datatype groups (NIST, MS-DataTypes, MS-Regex: 30,598) ≥ 95%, the
  structure groups (8,645) ≥ 80%, the XSD 1.1 groups (Saxonica, IBM:
  2,295) ≥ 70%; schema-aware QT4 XQ40 cases (about 1,100) ≥ 70%;
  schema-aware XSLT cases (about 690) ≥ 60%; the Phase 2 to 4 gates
  still held and no count dropped; no crash, leak or error outcome.
- **`sxml validate`** is designed in `phase-5.md` (synopsis, options,
  JSON shape, exit codes) and enters `docs/CLI.md` with packet 5.10
  (D44: only once it works).

### D166. Content that streams into a constructor; XSLT sweep 3 (packet 4.17)
Recorded 2026-10-18, after D71, D101, D110, D163. Accepts D163's Proposed
item "content that streams into a constructor", as one mechanism of the
shared evaluator and IR, used by XSLT and XQuery alike.
- **Receiving and delivering tasks** (glossary `receiving_task`,
  `delivering_task`; `src/evaluator_machine_internal.h`, "Delivering"):
  a document or element constructor (and SHALLOW_COPY of an element or
  document) whose content expression is not a D110 enclosed list opens
  its node, sets its part task's `receive` function and pushes the
  content as a delivering task (`sxml_evaluator_task.delivers_content`).
  When a delivering task finishes, the machine hands its value to the
  nearest receiving task below and the task below gets (). Delivering
  passes on only where a value reaches the receiver unchanged: LET bodies
  and conditional branches (they replace their task), a content LET's
  bound expression, each body of a MAP (not a path's MAP, whose body may
  select JNodes), the body of a function a delivering call enters (not
  through a coerced function item: its COERCE_RESULT task delivers), and
  the template calls of a delivering APPLY_TEMPLATES (which delivers its
  separators itself). A try/catch body never delivers, so no caught error
  leaves part of a content behind. The order and the rules of XSLT 4.0,
  5.7.1 and XQuery 4.0, 4.12.1.3 are those of the content sequence: the
  receiver adds each value as before (arrays flattened, atomic items
  joined within one enclosed expression, nodes copied, documents
  unwrapped, XTDE0410 and XQTY0024 from the same state).
- **Building into the receiver**: a constructor in a delivering task
  builds its node in the receiver's tree when that is what copying a tree
  of its own would give (`sxml_construction_open`): the same rules and
  construction flags and copy-namespaces preserve; no-inherit becomes the
  element start's `ignores_parent_namespaces`; a document's children
  become the content. Text, attribute, comment and processing-instruction
  constructors add their node directly (a namespace node keeps its own
  tree, whose builder checks XQDY0101); xsl:copy of another node delivers
  the node, which the receiver copies.
- **Content LETs** (glossary `content_let`, `sxml_ir_let.is_content`,
  printed `(let $n content ...)`): a LET whose variable only the result
  of its block reads, first and in order; in a delivering task its bound
  expression delivers too. Lowering marks them with
  `sxml_xpath_lowerer_mark_content_let` (the block's open LET made for the
  item, else marking stops unless the item is ()). The machine also
  evaluates `let $v := E return $v` as E in place.
- **Lowerings**: an XSLT literal result element, xsl:element,
  xsl:document and a temporary tree (9.4) compute their content in a block
  inside the constructor; a sequence constructor marks its children's LETs
  when it is a body or content (not with xsl:on-empty or
  xsl:on-non-empty, which read the atoms). XQuery computed element and
  document constructors do the same; a direct element constructor too,
  literal text as text constructors, unless two enclosed expressions are
  adjacent (`<c>{1}{2}</c>` stays a D110 enclosed list). Other temporary
  trees (with-param, message bodies) still take their content as a value.
- **Arena nodes**: content and in-scope namespaces share a union (a node
  has one or the other by kind), 72 to 56 bytes.
- **XSLT driver**: result documents are collected only for
  assert-result-document (else the null handler, which still checks
  them); the evaluation step budget is 4 x the QT4 driver's (D89), as
  json-to-xml-013 takes about 190 million steps.
- **Measured** (release, whole suites, before at 53bd400): json-to-xml-013
  peaks at about 410 MB (2.1 GB before) and passes; XT40 in scope 7,986 to 7,987 of 8,728
  (91.5 %), errors 1 to 0, no test set lost a case; XP40 30,647 and XQ40
  40,034 passed, 0 errors; slices 198 and 147. The driver change was
  committed with 457f530 (packet 5.3), which took the working tree's file.
- **Not done**: XTDE3052 when an abstract variable is read (accept-042,
  -043a): v1-proxy is a global that reads the abstract v1, and globals are
  computed before the evaluation (XQuery's too, D148: only unreferenced
  ones are skipped), so a lazy error needs globals computed on first read,
  which a step function's synchronous atom reads cannot do today
  (Proposed). A RAISE read in place of the abstract variable alone was
  tried and dropped: v1-proxy still raises when it is computed.

### D167. Schema components and compile stages (packet 5.2)
Recorded 2026-10-18. The internal design of Phase 5 (D165), in
`src/schema_internal.h`; golden `src/xslt_ast_internal.h`.
- **One store, index-addressed** (D24, D52): `sxml_schema_components`
  holds one table per component kind of XSD 1.1 Part 1, 2.2 (type
  definitions simple and complex together, element and attribute
  declarations, attribute uses, attribute groups, model group
  definitions, model groups, particles, schema wildcards, identity
  constraints, type alternatives, assertions, notations, facets) and per
  linking record (XPath records, namespace contexts, schema references,
  documents, list entries, namespace bindings, text bytes), entry 0 "none"
  in each. One generic append (`sxml_schema_components_add_entries`, by
  table) and edit, typed read accessors. Property records (content type,
  scope, value constraint, open content, type table) are fields. Built-in
  components first, with fixed indexes: type definitions 1 to 54 are the
  registry's built-ins by `sxml_builtin_type` (so a registered built-in
  and its component share a number), 55 the list type of
  xsi:schemaLocation, particles 1-2, model group 1, wildcards 1-2 for
  xs:anyType, attributes 1-4 the xsi: ones. xs:untyped, xs:untypedAtomic
  and xs:numeric have components but no XSD name.
- **Fields are "as read" or "property"**: the reader writes what the XML
  says, QName attributes as schema references (a table of symbol space,
  name, position); RESOLVE turns them into index fields beside them, and
  no later stage reads a reference. Children known when read are linked
  by index directly. Stages write the properties their comments name.
- **Six stages, one file each**, run in order by `sxml_schema_initialize`
  (5.4), each reading only what earlier ones wrote: READ (schema_read.c,
  schema_assemble.c), RESOLVE (schema_resolve.c), SIMPLE_TYPES (5.5),
  COMPLEX_TYPES (5.6, with schema_content_model.c), IDENTITY_CONSTRAINTS
  (5.8), ASSERTIONS (5.9); errors go through one function,
  `sxml_schema_compilation_report` (record gets the static description
  and the configured URI, the sink a detailed one); a stage that reported
  stops the pipeline after it. Stubs of compile stages check nothing and
  return OK (so 5.4 can measure schema tests alone); validation-time
  seams of unfinished features return UNSUPPORTED_FEATURE where used
  (D162), and the stubs handle the trivial case (no type table, no
  assertions, no identity constraints) so ordinary validation is not
  blocked by them.
- **`struct sxml_schema` is in the internal header** (processor, creating
  allocator, embedded components), so every `src/schema*.c` reaches the
  components; its functions stay 5.4's.
- **Content models** (5.6's plan): an opaque compiled model per complex
  type, in a block of the components (blocks never move, freed with the
  components, so no destroy hook): a deterministic Glushkov automaton,
  small maxOccurs expanded, large ones counted (as D147), UPA as a
  determinism check with element-over-wildcard, all groups as counted
  tables, open content as a matcher fallback, substitution groups as
  head transitions giving the member. The validator steps a cursor
  (`state` + caller-owned counter values) with start, step, can_end and
  list_expected.
- **Validator seams**, plain calls (D71): type alternatives and assertions
  (5.9), identity constraints (5.8) over a recorded subtree (an arena
  copy of the outermost element that needs one, with each element's
  governing declaration by node index), the ID table and the validator's
  report function (5.7), the derivation check (5.6) for xsi:type,
  substitution and type tables.
- **Sharing the header**: after 5.2, `src/schema_internal.h` is shared
  like a ledger: a packet adds what its files need as one small insertion
  in its own section, committed alone; it does not change other packets'
  fields. The text form writer is 5.4's (`src/schema_text.c`).

### D168. The XSD 1.1 test driver (packet 5.3)
Recorded 2026-10-18. `tests/xsd/driver/`, the developer tool
`sxml-xsd-test` (list, run), public API only, a sibling of sxml-qt4 and
sxml-xslt-test that links `sxml_qt4_driver_library`.
- **Shared, not copied** (the drivers' common mechanisms moved into the
  QT4 driver's library): the test budget (`budget.h`, from the copies in
  `test_run.c` and `xslt_execution.c`), the test selection over a test
  catalog interface (`selection.h`, from `xslt_selection.c`; the XSLT
  and XSD catalogs each adapt to it in a dozen lines), one slice-test
  script (`run-slice-test.cmake`, SPEC optional), documents in another
  suite's namespace and attributes in a namespace (`document.h`), report
  areas configurable (`report.h`, empty = D94's). The QT4 commands keep
  their own walking for now (a later cleanup can move them onto
  `selection.h`).
- **Names**: suite.xml names test sets only by href and testSet/@name
  repeats (Assert, CTA, TargetNS, VC, Zone), so a test set is named
  DIRECTORY-without-Meta "-" FILE-without-extension (`saxon-Assert`,
  `ms-DataTypes_w3c`, `nist-NISTXMLSchemaDatatypes`); the prefix is the
  contributor area of the report and the dashboard. A test is
  `GROUP/TEST` (unique per test set, xsts.xsd). `tools/run-xsd.sh`
  derives the same names from suite.xml with sed.
- **Scope, per element** (refines D165's count): xsts.xsd says the
  version tokens of testSet, testGroup and test each mean "run if any of
  them is supported", so each level must apply on its own; D165 or-ed
  the three levels together. Supported tokens and their classes as D165
  lists them; an unknown token is an unsupported class. Measured: 41,518
  in scope (15,329 schema tests, 26,189 instance tests; NIST 23,170,
  Microsoft 14,363, Sun 1,606, Saxonica 1,375, IBM 919, W3C WG 41,
  Oracle 26, Boeing 18), 20 fewer than D165's 41,538: 19 XSD 1.0 instance
  tests of `wgMeta/substitution-groups.testSet`, five of which name files
  that do not exist, and the 1.0 group of a Saxonica set. Gate groups:
  datatypes 30,598, structures 8,626, XSD 1.1 2,294. The introspection
  set is listed and not run; expected `indeterminate` or none applying,
  status queried or disputed: not run, out of scope.
- **Run**: each applicable test in a processor of its own (QT4 limits)
  through the test budget, its documents read (a missing one is an
  error); then not run, "XSD not implemented", until 5.4 and 5.7 fill
  the one marked place in `xsd_test_run.c` (plan in `xsd_test_run.h`:
  schema documents as TEXT with file:///SUITE-PATH, strict validation
  with a counting report sink, a group without a schema test validated
  against the built-in components alone, a group whose schema fails makes
  its instance tests fail). Report spec XSD11, program sxml-xsd-test.
- **Slice**: `tests/xsd/slice.txt`, 147 in-scope tests, one per test set
  plus the rest in proportion; baseline 0. A full release run takes 45 s
  (one process per test set; NIST, 23,170 tests in one file, is the
  longest).

### D169. Phase 4 gate review (packet 4.18)
Recorded 2026-10-19 by the review agent (D11), after D95, D123, D151,
D161, D166. Checked locally (D123): `tools/check-main.sh` on clean HEAD
(77bc5d5 before and 8d2aa06 after this review's changes) builds
`release`, `debug` (ASan, UBSan), `debug-gcc` and `wasm`, and every
test passes; the full lint at 8d2aa06 is clean (no call cycle). The
dashboard is regenerated at 8d2aa06.
- **Numbers** (release, tools/run-xslt.sh, run-qt4.sh XP40 and XQ40,
  run-xsd.sh, every test set finished, 0 crashes, no leak reported by
  the drivers' budgets, 0 "error" outcomes): at 77bc5d5 XT40 7,987 of
  8,728 in scope, **91.5 %** (gate 70 %); XP40 30,647 passed, in scope
  30,629 of 31,543, **97.1 %** (gate 90 %); XQ40 40,034 passed, in scope
  40,016 of 41,174, **97.2 %** (gate 85 %); no regression from 30,647 /
  40,034. Core sets (gate 80 % each): apply-templates 97.4 %, mode 88.5 %,
  match 93.8 %, for-each-group 90.7 %, sort 100 %, key 97.1 %, number
  97.4 %, choose 95.4 %, copy 89.0 %, value-of 93.3 %, result-document
  95.3 %, function 91.1 %. Slices: qt4/slice 198, qt4/xquery-slice 186,
  xslt/slice 147 (ctest, every preset). After this review's changes, case
  by case: XT40 gains try-002, -022 and -045 (7,990, 91.5 %), XP40 and
  XQ40 identical, nothing lost. The 24 sandp test sets the XSLT runner
  starts end at once with "unknown test set" (out of scope, D95).
- **Fuzzing**, 300 s per target, seven at once on a loaded machine (fork
  mode for the evaluating targets): cast_fuzz 1,068,415 runs, regex_fuzz
  461,213, xml_parse_fuzz 62,095, xpath_compile_fuzz 10,066,
  xpath_evaluate_fuzz about 10,000, xquery_fuzz about 9,600 and the new
  xslt_fuzz about 8,300; no crash, leak or sanitizer report. Timeouts
  for triage only (P9): one xquery_fuzz input and four xslt_fuzz inputs,
  all suite stylesheets slow by nature under ASan (a loop of 1,000,000,
  primes to 1,000, match="$nodes" over 2,000 nodes), each finishing in 5
  to 12 s alone. **xslt_fuzz** (`tests/fuzz/xslt_fuzz.c`, new): compiles
  the input through `xslt.h` and transforms it twice (templates applied
  to a small document, serialized, result documents collected as trees;
  xsl:initial-template called, raw result), small limits, a resolver
  table with one module and one document; seeds are the suite's `.xsl`
  files.
- **The XSLT driver's step budget** (D166, 4 x QT4, 400 million): a fair
  test limit, not a mask. json-to-xml-013 costs time linear in its input
  (13,000 to 209,579 cities: 1.5 s to 25 s, release), about 900 steps a
  city, 65 a node; a profile is flat (JSON parsing 23 %, coercion 10 %,
  construction 6 %, template dispatch 4 %), no pattern matching hot spot.
  What the fuzzing found instead: a pattern rooted at a variable
  (match="$a-nodes", 6.3.1) tests membership by a scan of the variable's
  value, so N nodes matched against M is N x M (1,000 nodes 0.6 s, 4,000
  9.4 s); within the budget for the suite's case, a design for a later
  sweep (a node set index by document order per global value), Proposed.
- **D71**: no call cycle (tools/check-call-cycles.py, 163 files); through
  function pointers: the evaluator's guard (`is_running`, D151) refuses a
  nested run on one dynamic context and the lowering machine's (D161) a
  nested lowering; resumable natives (fn:key's index, cached calls,
  accumulators) return CALL steps to the machine; D166's receive function
  only adds items to a construction and calls no machine; the result
  document handler, the trace sink and the resolver are host code, and a
  transformation one of them starts runs on a dynamic context of its
  own, as xslt.h allows; whitespace stripping is a tree builder filter.
- **Fixed**:
  - Error locations of XSLT: expressions are parsed from attribute text,
    and their AST spans stayed relative to that text, so a dynamic error
    or a static error found by lowering (XPST0008, XPST0017) said 1:5
    for an expression on line 4. The reader now places each AST in its
    module (`sxml_ast_move_source_spans`, with the rule its own
    relocate_error used, now shared). An error located in a unit lowered
    for one invocation (call-template, call-function, another mode) named
    its module with memory freed when the transformation ended (found by
    ASan in a new CLI test): the name now points to the stylesheet's
    module URI (status.h, D21).
  - err:XTMM9000 and err:XTMM9001, the default codes of xsl:message and
    xsl:assert, had no prefix and were shown Q{...}XTMM9000.
  - `sxml transform` with a relative STYLESHEET resolved "../" in an
    href to the file system's root (file:sub/x.xsl with ../b.xml is
    file:/b.xml by RFC 3986): the base URI now has the working directory
    (`sxml_platform_read_working_directory`, the invocation's
    `working_directory`; file:///C:/ for Windows). Error reports name the
    module file of a static error's location.
  - Merged: `sxml_xml_byte_is_whitespace` replaces thirteen private
    copies (casting, function library, XPath and XQuery static analysis,
    XQuery and XSLT lowering, constructors); one
    `sxml_xslt_lowering_add_expression` for seven copies.
  - `tests/xslt_test.c` covers the collector's caller-memory lifecycle,
    the last functions of xslt.h without a test.
- **Larger duplicates, listed** (one shared mechanism each, later): typed
  array allocation (count x size checked, then allocate) is a private
  helper in 19 files; group-adjacent and group-starting-with compare keys
  in `src/xslt_group.c` with their own equality, next to the FLWOR's
  `keys_are_equal` (group-by itself is shared); the window clause and
  group-starting-with/ending-with are two run detectors over one
  sequence. D161's follow-ups are done (sweep 2, D163: slot atoms, trim,
  declared types, heapsorts).
- **Delayed features (D162)**, one place each with the specification's
  error: xsl:import-schema XTSE1650 and validation or type attributes
  XTSE1660 (`src/xslt_read.c`), backwards-compatible behaviour XTDE0160
  (`src/xslt_lower.c`), streaming read only where a non-streaming
  processor must read it (xsl:mode, accumulators), xsl:evaluate XTDE3175
  (`src/xslt_lower_control.c`; dynamic evaluation is optional),
  extension instructions XTDE1450 (`src/xslt_lower.c`).
- **`sxml transform` matches docs/CLI.md**: every example of the page
  gives its output and exit code; the options, the usage errors, exit 3
  for a missing file and 1 for a stylesheet or input that is not
  well-formed behave as written.
- **xslt.h is stable** (D161's rule): consistent with `xpath.h` and
  `xquery.h`, every declaration documented, every function tested through
  the public API with an allocation-failure loop, the CLI on it alone.
  From now on it changes as every public header does (D47); the
  package-version function of Proposed would be an addition.
- **Proposed, resolved here**: matching package versions for host
  resolvers is accepted as a later addition to xslt.h (additive, no owner
  decision); the XSLT functions at run time and globals computed on first
  read are accepted as designs for a later sweep (the evaluator gains
  lazy globals as machine tasks, which also makes xslt.h's "evaluated
  when first read" exact for errors). New: membership of a pattern
  rooted at a variable (above). Left for the owner: the WebAssembly
  budgets (three items) and XQuery's omit-xml-declaration default.
- **Not done** (deferred, listed in the LOG): a dynamic error inside an
  included or imported module names the principal module with the
  included module's line (XSLT functions and templates carry no IR module
  number; the static contexts' IR modules have no source name);
  attribute-value normalization makes columns approximate in an
  expression that spans lines; XTDE3052 on reading an abstract variable
  (D166).

### D170. The schema document reader and assembly (packet 5.4)
Recorded 2026-10-18. READ and RESOLVE (D167) and `sxml_schema`'s quintet;
golden `src/xslt_read.c`, `src/xquery_module.c`.
- **One table of schema element roles** (`src/schema_read.c`): a role is
  what an element is where it appears (global or local element, group
  definition or reference, a facet, ...); its rule lists the unqualified
  attributes and required ones the schema for schema documents allows,
  the child slots its children fill in order (a slot: roles, required,
  repeated, ending the content), and its reader. Attributes of other
  namespaces than XSD's are allowed (openAttrs), `vc:` attributes decide
  conditional inclusion (4.2.2, src-cip). What breaks the schema for
  schema documents is reported with the role's representation constraint
  (src-element, src-ct, src-simple-type, ...), or the component's
  properties constraint where there is none (mg-props-correct,
  an-props-correct, n-props-correct, as-props-correct,
  sch-props-correct); equal `id` values are cvc-id.2.
- **The walk** is a loop over node indexes from the element read to its
  last descendant; an element's children get their roles when it is read,
  so an element without one (ignored, misplaced, in an annotation) is
  skipped with its subtree. Components are added in document order,
  linked to the parent's component at once; lists (particles, attribute
  uses, facets, ...) are written in a second pass, once, whole.
- **As read, for the later stages**: a simple type restriction has
  variety ATOMIC until RESOLVE gives it its base's; the xs:assertion
  facets of a simpleContent restriction join the type's assertions with
  its xs:assert children, in document order; defaultOpenContent is
  applied to complex content when appliesToEmpty or the type has a
  particle or complexContent (clause 6's emptiness needs COMPLEX_TYPES);
  an attribute use with ref and inheritable="false" takes the
  declaration's.
- **Assembly** (`src/schema_assemble.c`): a work list; a document is read
  once by absolute URI and the namespace its components get (a chameleon
  document included into two namespaces is read twice from one tree);
  the children of xs:redefine and xs:override are read after every
  document, replacing the name index entry of what they name (4.2.4,
  4.2.5); an override child with nothing to replace is not read; a
  component replaced twice is src-redefine or src-override; a redefined
  document that reaches the redefining one is src-redefine; a
  redefinition's base or self-reference names the original (src-redefine.5,
  6.1, 7.1; the original must be the redefined document's). Redefines
  and overrides are read innermost first (a chain replaces the inner
  redefinition) and a component replaced twice in one document is the
  error. A document the resolver does not deliver is no error (4.2.3
  clause 2.4, 4.2.6.2; schema.h says otherwise, Proposed); an unlocated
  import of the xml: namespace reads its known document (xml.xsd); an
  import without a location of a namespace already read reads nothing;
  an xs:schema element that conditional inclusion ignores is empty.
- **Not done**: src-redefine 6.2.2 and 7.2.2 (a group or attribute group
  redefined without a self-reference must restrict the original); the
  original of an override or redefinition stays a component (unnamed)
  and is compiled.
- **RESOLVE** (`src/schema_resolve.c`): references through the name index
  (src-resolve; src-resolve.4.2 for namespaces not imported, at READ),
  base, item and member types, restriction varieties, substitution group
  affiliations, element types through heads, members transitively, ref
  identity constraints; cycles of groups (mg-props-correct.2),
  derivations (ct-props-correct.3, st-props-correct.2) and substitution
  groups (e-props-correct.6), each a walk with an explicit stack.
  Attribute groups may refer to each other in a cycle (XSD 1.1, bug
  15795).
- **Text form** (`src/schema_text.c`): D167's lines for every table, the
  xs:schema element's namespace context always first, content models
  (5.6's writer) after the components.
- **Driver**: sxml-xsd-test compiles every schema test and every
  instance test's schema (resolver over the suite files); instance tests
  are not run until 5.7 ("XSD validation not implemented").
- **Measured** at 21c9531 with 5.5 and 5.6: schema tests 15,163 of
  15,329 in scope pass (98.9 %, gate of the packet 85 %); every schema
  document reads without a crash, leak or limit (10 particle tests reach
  the content model state limit in COMPLEX_TYPES); no reader error on a
  valid schema; the XSD slice 110 of 147.

### D171. Simple types and facets (packet 5.5)
Recorded 2026-10-18. Fills SIMPLE_TYPES (D167) and registration (D165).
- **One lexical engine.** Casting to a registered type is its whiteSpace
  facet (the nearest on its chain of registered bases, else the built-in
  one), the lexical mapping of its built-in ancestor (src/cast_lexical.c),
  then every facet of it and its registered bases (new src/cast_facet.c,
  part of the cast engine): steps by conjunction, the patterns of a step by
  disjunction. A failure is FORG0001 whose description starts with the
  constraint name (cvc-minInclusive-valid, ...), so validation (5.7) can
  report it with XSD's code. Facet values are read with the lexical mapping
  of the value's built-in type, never cast back through src/cast.c (D71).
  Equality is atomic-equal in that type (QNames by EQName); order is XSD's
  (`sxml_cast_order_values`: NaN incomparable, a date without a timezone
  compared at -14:00 and +14:00, durations at the four reference dateTimes
  of Part 2, E.3.3). Lists made by xs:list may be empty; a union's facets
  apply to the value its first accepting member gives, and no other member
  is tried after them.
- **Patterns**: `sxml_regex_flags.is_xsd_pattern` (D53's flag), the
  regex anchored to the whole value, ^ and $ ordinary, back-references,
  reluctant quantifiers, (? and (* groups and \b \B FORX0002. A pattern
  is compiled per check, in heap memory (open: a cache).
- **Registration** (`sxml_type_registry_register_schema_type`, src/types.c):
  no interning, no name index; facets copied, in the component's facet
  order, so a stage can pair registered (canonical) values with the
  components' fixed flags.
- **SIMPLE_TYPES** reads "simple" as "variety not COMPLEX" and replaces the
  variety of a restriction with its base's, so READ may write any simple
  variety for xs:restriction (comment at the stage's declaration). Order:
  an explicit stack over the type table; a type met again on it is
  st-props-correct.2. Facet values are made canonical (counts, keywords,
  enumeration values cast to the base, bounds read in the built-in type and
  compared with the base's bounds as Part 2, 4.3.7.4 to 4.3.10.4 say:
  incomparable values break no rule); the base's effective facets include
  the implicit ones of built-in types (whiteSpace, fractionDigits 0 of
  xs:integer, minLength 1 of the built-in lists, explicitTimezone of
  xs:dateTimeStamp). A failed type is not registered and the types made
  from it are skipped without a second report. Then value constraints of
  attribute declarations and uses are cast to their types (a-props-correct.2,
  au-props-correct.2). NOTATION enumeration values are read as QNames and
  must name notation declarations; xs:NOTATION itself (a declaration's
  type, a list item, a union member, a restriction without enumeration) is
  enumeration-required-notation. In the XSD dialect an unknown block name
  is all characters (Part 2, G.4.2.4) and an unescaped "-" never starts a
  range.
- **Measured** (release, schema tests only: instance tests wait for 5.7):
  NIST 3,953 of 3,953, MS-Regex 2,590 of 2,590, MS-DataTypes 2,240 of
  2,247, MS-SimpleType 322 of 334, Saxon Simple 65 of 65; the rest are the
  reader's (duplicate ids, include and redefine order, stZ033's chain of
  redefinitions) and complex types'. All XSD: 15,073 passed, 335 failed of
  the 15,408 run. XP40 30,647 and XQ40 40,034 unchanged, slice 198.

### D172. Complex types and content models (packet 5.6)
Recorded 2026-10-18. Fills COMPLEX_TYPES and the content model plan of
D167 (`src/schema_complex_type.c`, `src/schema_content_model.c`).
- **One automaton format.** A content model is states whose transitions
  are labelled by an element declaration (with the declarations
  substitutable for it, 3.3.6.3) or a schema wildcard and carry counter
  tests (a counter in a range) and counter operations (set to one, add
  one); final states may need tests. Sequences and choices compile
  through Glushkov's position automaton and a subset construction; an
  all group compiles to two states (before and after the first member)
  whose transitions count each member. The matcher, list_expected and the
  restriction walk step this one format.
- **Expansion, counters, determinism.** A particle with maxOccurs (or,
  unbounded, minOccurs) up to 16 is expanded into copies, a larger one
  keeps one copy under a counter; a compilation past the state limit is
  tried again counting every repetition above one. When the counters
  cannot choose between two transitions of one particle ((a{1,20}){1,20},
  (a+){1,100}: weakly but not strongly deterministic) the counted
  particles involved are expanded and the model compiled again, so the
  matcher never backtracks and needs one state and one value per counter.
  When that expansion passes the limit too (MS particlesZ035, Z036,
  maxOccurs in the thousands nested), the counters are kept and the
  transitions ordered (fewer operations first, so an inner repetition
  before an outer one); the matcher takes the first whose tests hold,
  exact unless an outer minimum needs the inner repetitions spread.
  Bounds: the configured states (and positions), 4 nodes, 32 edges or
  transitions and 64 list entries per state.
- **UPA as determinism** (cos-nonambig): two transitions of a state for
  different element particles sharing a name (also through substitution
  groups), or for different wildcard particles whose namespaces overlap,
  whose tests can hold together. An element particle beats a wildcard
  particle (3.8.6.4). Exclusive counter tests do not compete: (a{2}, a) is
  valid.
- **Substitution groups**: a head's transition lists the head and each
  member substitutable for it (blocking by the head's disallowed
  substitutions, its type's and the intermediate types' prohibited
  substitutions). Abstract declarations are listed too (D167 said not):
  the validator raises cvc-elt.2 for them, as for an abstract head.
- **Open content** is the matcher's fallback; suffix mode appends a suffix
  state that only its wildcard enters and keeps.
- **Restriction of content** (cos-content-act-restrict, 3.4.6.4) is a walk
  over pairs of configurations (state and counter values) of the two
  models: each move of the restriction, element by element (and each
  name a restriction's wildcard shares with the base's element
  transitions), must be a move of the base whose default binding subsumes
  the restriction's; names the restriction takes by an element are not
  its wildcard's. A base wildcard with no global declaration of the name
  takes the restriction's local element whatever its process contents
  (XSD 1.0's NSCompat; read literally 3.4.6.4 refuses it, against the
  suite: MS particlesJ*, ctG*). Past 65,536 configurations the derivation
  is accepted provisionally, as 3.4.6.3 allows.
- **The stage**: particles (p-props-correct), attribute groups (closures
  over cyclic references, 3.6.2.1, then written), complex types base
  first with an explicit stack (content type of 3.4.2.2 and 3.4.2.3.3 with
  the synthesized particles, simple content types through
  `sxml_schema_compile_simple_type`, attribute uses and wildcards with
  wildcard union and intersection, assertions; cos-ct-extends,
  derivation-ok-restriction, ct-props-correct; the content model;
  registration), xs:anyType's and xs:untyped's content models, then
  element declarations (e-props-correct 2, 4, 6, 7; value constraints cast
  to the simple type). Attribute restriction is checked with XSD 1.0's
  clauses (uses allowed, types derived, fixed values, required kept,
  wildcard subset, not weaker), not against xs:anyType.
- **Registration**: a complex type with a simple base is registered with
  base xs:anyType, since `sxml_type_registry_register_schema_type` takes
  only complex bases for complex types (Proposed).
- **Text form**: `sxml_schema_content_model_write_text` prints a model in
  D167's form, for 5.4's `sxml_schema_components_write_text`.
- **Not done**: type table equivalence (cos-element-consistent clause 4
  and the wildcard clause), the disallowed-names clauses of Wildcard
  Subset, cos-ct-extends 1.5 and 1.6 and derivation-ok-restriction.4
  (locally declared types), xs:assertion facets of simpleContent
  restrictions.
- **Measured** (schema tests only, with 5.4's uncommitted reader; the
  instance tests wait for 5.7): MS-Particles 851 of 853 run, ModelGroups
  377 of 391, Group 215 of 218, ComplexType 546 of 550, Wildcards 313 of
  315, Sun MGroup 40 of 40, CType 31 of 31, Saxon All 62 of 62, Open 49 of
  51, IBM allGroup 9 of 9, openContent 24 of 24. The rest are the reader's
  (duplicate ids, an all group with maxOccurs 0 accepted by XSD 1.1,
  openContent mode="none" with a wildcard, xs:redefine of a group) or
  disputed by the versions (particlesK006).

### D173. Validation and the typed tree (packet 5.7)
Recorded 2026-10-18. The schema validator of D165 and the seams of D167
(`src/schema_validate.c`, `src/schema_validate_value.c`,
`src/schema_validate_record.c`, `src/schema_validate_internal.h`); golden
`src/xslt_whitespace.c`, `src/arena_tree.c`.
- **A filtering tree builder.** An element's start is held back with its
  bindings and attributes (glossary pending_element) until its first
  content event or its end; then its governing type is chosen (its
  declaration's, after the type table seam; the configured governing type
  for the root; xsi:type when validly derived, cvc-elt.4; abstract
  declarations cvc-elt.2 and types cvc-type.2), xsi:nil applied
  (cvc-elt.3), and its start, bindings and assessed attributes go on. One
  validation frame per open element (glossary validation_frame) on a
  scratch array (D71). Each element is assessed strictly, laxly or skipped
  (glossary assessment); lax and skipped elements are xs:anyType, their
  unassessed attributes xs:untypedAtomic (XDM 4.0, 3.3.1.1).
- **Content**: children step the parent type's content model cursor (5.6;
  wildcards by process contents, open content by the matcher); an error in
  content is reported once and the rest of the element is assessed laxly.
  Simple content is collected and assessed at the end, then sent as one
  text event of its schema normalized value, or of the declaration's
  default or fixed value when the element is empty (cvc-elt.5); whitespace
  text in content that is not mixed is dropped (XDM 4.0, 7.5.7.4), and
  empty content allows no character children at all (3.4.4.2 clause 1.1).
  Element Declarations Consistent at validation (XSD 1.1, 3.4.4.2): an
  element a strict or lax wildcard matched must have a type derived from
  that of a like-named declaration (or substitution group member) of the
  content model or its base types; a skipped one is not checked.
- **Attributes**: an attribute use of the name (also of an xsi: name: a
  required xsi:type), else the four xsi: declarations, else the attribute
  wildcard by process contents (strict needs a global declaration,
  cvc-assess-attr); missing required uses cvc-complex-type.4; uses with a
  default or fixed value not present are added. Values are sent as their
  schema normalized values.
- **Simple values** go through the cast engine (D171): an invalid value is
  reported with the facet's cvc-*-valid name the cast puts first in its
  description, else cvc-datatype-valid.1.2.1; xs:anySimpleType and
  xs:anyAtomicType values are xs:untypedAtomic (also for value constraints
  in SIMPLE_TYPES and COMPLEX_TYPES, which reported au-props-correct.2,
  a-props-correct.2 and e-props-correct.2 for them); fixed values compare
  item by item (atomic-equal), mixed ones as strings; ENTITY values must
  name an unparsed entity of the document.
- **IDs** (cvc-id, XSD 1.1, 3.17.5.2): an ID identifies its attribute's
  element, or the parent of an element whose content is the ID (the
  validation root's content identifies nothing); one value may identify
  one element more than once; IDREFs are checked when the validation root
  ends. `sxml_schema_id_request` gained `element_number`.
- **The recorded subtree** (glossary recorded_subtree): while an element
  whose declaration has identity constraints or whose type has assertions
  is open, the annotated events also go to an arena tree through a
  counting builder, so each element's declaration is known by node index;
  the checks are deferred and run, in end order, through the seams of 5.8
  and 5.9 when the outermost recorded element ends. Type alternatives get
  a parentless copy of the element with its attributes as they appear and
  its in-scope namespaces; the attributes it inherits are 5.9's.
- **Typed values** are derived in `sxml_node_make_typed_value` (D165): a
  nilled element none, empty content the empty sequence, element-only
  content FOTY0012, mixed content and xs:anySimpleType xs:untypedAtomic,
  otherwise the string value cast with the element's (an attribute's
  parent's) in-scope namespaces.
- **`sxml_schema_validate_node`** copies the node through a validator into
  a new arena tree (`sxml_tree_builder_copy_node`, annotations dropped);
  a copy of a node that is no document keeps its base URI.
- **Driver** (`tests/xsd/driver/xsd_test_run.c`): instance tests validate
  while parsing; the instance's xsi:schemaLocation and
  xsi:noNamespaceSchemaLocation hints are followed by the driver, not the
  library (schema.h: hints are not followed; 4.3.2 allows either): the
  hinted documents are added as RESOLVED documents relative to the
  instance, and a hint that breaks the schema is not followed. The suite
  expects hints for 55 instance tests of groups without a schema test and
  for some with one.
- **schema.h** (provisional, comment only): a document the resolver does
  not deliver is no error for include, redefine and override either (XSD
  1.1, 4.2.3 clause 2.4), resolving 5.4's Proposed item.
- **Quick fix for 5.6**: ##definedSibling excludes the substitution group
  members of the content model's declarations (3.10.4.2).
- **Not done**: the attributes an element inherits (for type
  alternatives, 5.9); the XML 1.1 instances of saxon-XmlVersions (the
  parser reads XML 1.0); identity constraints and assertions stay
  UNSUPPORTED_FEATURE where used (D167, D162).
- **Measured** (release, at the packet's last commit): XSD in scope 40,755
  of 41,518 (98.2 %): instance tests 25,533 of 26,189 (97.5 %; NIST 100 %,
  Microsoft 95.0 %, Sun 93.9 %, IBM 65.3 %, Saxonica 74.5 %; 632 meet
  identity constraints, assertions or type alternatives and are not run),
  schema tests 15,222 of 15,329; gate groups: datatypes 100 %, structures
  95.6 %, XSD 1.1 83.3 %; the XSD slice 147 of 147. XP40 30,647, XQ40
  40,034 and XT40 7,990 unchanged.

### D174. Schema-aware XPath and XQuery (packet 5.11)
Recorded 2026-10-18. Replaces the rejection places of D97 and D162 for
XPath and XQuery (XPST0008 of schema-element, XQST0009, XQST0075, FODC0009
of fn:parse-xml, the driver's non-goal features); golden
`src/xquery_flwor.c` (an internal native row), `src/function_library_csv.c`.
- **xpath.h** (stable API, additive): `sxml_schema_view` and the last field
  `sxml_static_context_configuration.in_scope_schemas`, the in-scope
  schema definitions; empty is not schema-aware. Schemas must be the
  processor's and outlive what is compiled with them (validation reads
  them at run time). The view goes into `sxml_ir_static_context` (copied
  into the unit like the decimal formats, D131), so static analysis and
  run time read one field; `sxml_xpath_static_context.processor` gives the
  resolver for schema imports.
- **One module, `src/schema_scope.c`** (glossary schema_scope): lookups of
  types and global declarations across the in-scope schemas (built-in
  types first, then the first schema that has the name), substitution
  group members, constructor functions of schema simple types (used by
  static calls in `src/xpath_lower.c` and by fn:function-lookup), and
  `sxml_schema_scope_raise`, the one way a schema error becomes a language
  error: the language's code, "constraint: description".
- **schema-element(N), schema-attribute(N)** resolve statically to the
  choice of element(M, T) tests over N and the non-abstract, not blocked
  members of its substitution group (T the declared type, "?" when
  nillable), and to attribute(N, T); `is_schema_declaration` is no longer
  set. A union with a list member is not an item type (XPST0051).
- **import schema** (XQuery 4.0, 5.11): prefixes bind like namespace
  declarations ("default element namespace" counts for XQST0066);
  XQST0057, XQST0058; a namespace a host schema defines (or xs) is taken
  from there; the others compile into one schema of RESOLVED documents
  (hints, or the namespace alone; target namespace checked) kept by the
  processor until it is finalized (`sxml_processor_keep_schema`), as it
  keeps registered types: a unit owning schemas would make finalization
  a cycle (D71), since a schema's assertions and identity constraints are
  XPath units. When a document is not delivered,
  compilation is tried once more without the hints the resolver does not
  deliver (hints may be ignored). Errors: src-import and schema_reference
  XQST0059, sch-props-correct.2 XQST0035, others XQST0012. A main module
  without schemas gets the processor's one schema of the built-in
  components (`sxml_processor_get_builtin_schema`), so lax validation,
  xsi:type and validation by a built-in type always have one. A
  long-lived processor that compiles many importing queries keeps their
  schemas; caching them by documents is a later step.
- **validate** lowers to the internal row `validate($node, $mode,
  $type)` (`src/function_library_schema.c`): XQTY0030, XQDY0061, XQDY0084
  (strict, no declaration), XQDY0027 (invalid, constraint named), XQST0104
  statically; "type xs:untyped" copies without annotations. The schema is
  the in-scope one that declares the root's name or defines the type.
- **System properties**: fn:system-properties gives schema-aware and
  accepts-typed-data true; XSLT's is-schema-aware stays false (5.12).
- **fn:xsd-validator** (F&O 4.0, 17.2.5), a new library part: options
  validation-mode, type, return-typed-node, return-error-details
  (message and rule maps from a report sink); V validates with the
  in-scope schemas of its caller's static context (a function item holds
  no schema). The options schema, schema-location, target-namespace and
  use-xsi-schema-location are FODC0009 (one place): compiling a schema
  while a query runs needs the processor, which run time does not reach.
  The option limit of `function_library_core_internal.h` is 12.
- **xsd-validation of fn:parse-xml** validates the parsed document
  (FODC0008, FODC0014); fn:doc and fn:parse-xml-fragment keep FODC0009
  (the loader keeps one tree per URI; a fragment has no validation root).
- **QT4 driver**: schemaImport, schemaValidation and typedData are
  supported but stay outside the D95 denominators; environments with
  schemas compile them into one schema that is the in-scope schema of
  XPath and XQuery and validates `validation="strict|lax"` sources; the
  resolver serves SCHEMA_DOCUMENT by suite file, else by namespace. A
  schema-aware test case (glossary schema_aware_test) is counted in the
  report members `schema_aware`, `schema_aware_passed` and the dashboard
  area "schema-aware"; one that stops at SXML_STATUS_UNSUPPORTED_FEATURE
  (identity constraints, assertions: 5.8, 5.9) is not run. The fixtures'
  unsupported feature is now staticTyping.
- **Budget**: a schema-aware test case whose query runs out of the test
  budget (app-Demos itunes: a 10 MB source and a quadratic query, 10^8
  steps; app-CatalogCheck Catalog009: every test set loaded, 536 MB) fails
  instead of counting as an error of sxml.
- **Measured** (release, commit f43f747): schema-aware XQ40 1,231 of
  1,516 (81.2 %; the proposed gate is 70 %), XP40 279 of 315 (88.6 %);
  XP40 30,924 passed (was 30,647), XQ40 41,261 (was 40,034), both 0
  errors; D95 gate rates XP40 97.6 %, XQ40 97.9 %; slice 198; XT40 7,990
  and XSD11 41,452 unchanged. Six test cases that asked for these
  features to be absent no longer run (fn-system-properties,
  K2-ExternalVariablesWith-22a, ...), their satisfied twins do.
- **Not done**: the schema for the fn namespace
  (import schema of fn, json-to-xml validate=true, analyze-string
  validation), fn:doc's xsd-validation, schema documents compiled while
  a query runs (fn:xsd-validator's schema options), "fixed" default
  namespaces, is-id of unions and lists (5.7), complex types with a
  simple base (Proposed, 5.6), the pattern facet on canonical forms (5.5),
  fn:element-to-map layouts by type annotation.

### D175. Assertions and type alternatives (packet 5.9)
Recorded 2026-10-18. Fills ASSERTIONS and the seams of D167 for XSD 1.1
assertions and conditional type assignment (`src/schema_assertion.c`);
golden `src/schema_simple_type.c`.
- **One engine.** Tests are compiled with `xpath.h` (the XPath 4.0 front
  end reads XSD 1.1's XPath 2.0, a superset 3.13.6.2 allows) in the
  static context of XPath Valid: the record's namespace bindings (prefix 0
  and xml left out), its {default namespace} (xpathDefaultNamespace) as
  the default element namespace, its base URI when absolute, the
  codepoint collation; assertions declare `$value` as
  xs:anyAtomicType*, type alternatives nothing. A static or type error is
  as-props-correct.2 or ta-props-correct.2. Evaluation gives no available
  documents and collections and the empty default collection (a resolver
  of the file, 3.13.4.2); a dynamic or type error makes a test false; the
  result is taken as fn:boolean takes it
  (`sxml_evaluator_compute_effective_boolean_value`).
- **Assertions of complex types** run when the outermost recorded element
  ends, on an assertion copy (glossary assertion_copy): the element's
  recorded subtree copied into a tree of its own whose root is
  xs:anyType (3.13.4.1 clauses 1.2 and 1.3: partially validated, nothing
  outside it); `$value` is the typed value of simple content, else empty.
  The recorder drops comments and processing instructions (clause 1.2,
  the suite's comments-and-PIs-excluded). Each false one is
  cvc-assertion.
- **Assertion facets** (Part 2, 4.3.13.3) run with `$value` and no context
  item, for the type's {assertions}, a list's items against their item
  type, and a union value against its active basic member (glossary
  basic_member): the member the cast engine chose, which knows no
  assertions, and when its assertions fail each later basic member whose
  cast of the text succeeds (union items of a list: the chosen member
  only). One report, cvc-assertions-valid, and the value is invalid. The
  xs:assertion facets of a simpleContent restriction are now the
  synthesized simple content type's (`facet_assertions`, as read; 5.6
  gave them to the complex type).
- **Type alternatives** (3.12.4) are tested on the parentless element copy
  of D173 with the element's inherited attributes added (glossary
  inherited_attribute, 3.3.5.6): the validator keeps the inheritable
  attributes of the open elements (a use's {inheritable}, else the
  declaration's), with their values as they appeared, and the copy gets
  the innermost of each name the element does not have. The copies have
  the document's base URI. The first test that holds selects, else the
  default alternative, else the declared type.
- **Type tables compared** (3.8.6.3 equivalence, the minimum it requires:
  same expression, namespace bindings, default namespace, base URI and
  type, same default type), for cos-element-consistent and clause 4.6 of
  3.4.6.4 (`sxml_schema_type_tables_are_equivalent`).
- **Fixes in earlier stages**: an attribute reference without
  inheritable= takes its declaration's {inheritable} (`has_inheritable`,
  as read; RESOLVE took the declaration's when the reference said
  false); a restricted attribute use keeps its {inheritable} (3.4.6.4
  clause 5.4); xpathDefaultNamespace="##defaultNamespace" on xs:schema
  is resolved on each host element (3.13.2).
- **Not done**: namespace fixup of an inherited attribute whose prefix an
  inner element rebinds; xml:base of ancestors in the copies' base URI;
  value constraints and enumerations checked against assertions at
  compile time; casting to a type with assertions in schema-aware
  expressions (5.11).
- **Measured** (release): Saxon Assert 101 of 101, CTA 113 of 113, IBM
  assert 79 of 79, assertion 101 of 101, typeAlternatives 39 of 39,
  typeAlternativesMixed 15 of 15 (before: 188 of 448). XSD in scope
  41,037 of 41,518 (98.8 %, from 40,755); gate groups datatypes 100 %,
  structures 95.6 %, XSD 1.1 95.6 % (from 83.3 %); slice 147 of 147. No
  test set lost a test (ibm-vc, ibm-idIDREF, saxon-VC, saxon-Override and
  saxon-Wild gained some). Only schema code changed: XP40, XQ40 and XT40
  are not affected.

### D176. sxml validate (packet 5.10)
Recorded 2026-10-18. The command of the 5.10 design
(`docs/tasks/phase-5.md`), in `apps/sxml/command_validate.c`; golden
`apps/sxml/command_check.c`. Public API only (`schema.h`, `xml.h`).
- **One pass per FILE**: the XML parser feeds a validator whose result
  builder is the null builder; the processor and its local-file resolver
  are `query_command.h`'s (`SXML_PARAMETER_BINDING_NONE`). SCHEMA
  documents are TEXT documents with file URIs, so includes and imports
  resolve relative to the referring document; report locations are named
  back by `sxml_query_file_of_uri`, moved with `sxml_query_make_file_uri`
  from `command_transform.c` to `query_command.c` (one helper for both
  commands).
- **Schema location hints are not followed, and there is no flag for
  them.** The schema is what the command line names (D44: the command
  line fully determines behaviour; XSD 1.1 Part 1, 4.3.2 leaves hints to
  the processor; `schema.h` does not follow them either). A hinted
  document is named with another `--schema`. An explicit flag can be
  added later if the owner wants one; it would be a second, visible way
  to name schema documents, never a default.
- **Required options in the CLI framework**: `SXML_CLI_OPTION_AT_LEAST_ONCE`
  (`cli_command.h`) is a repeatable option the parser requires
  (`SXML_CLI_USAGE_ERROR_MISSING_OPTION`, "missing option '--schema'");
  help shows `--schema=SCHEMA [--schema=SCHEMA]...` and "(required;
  repeatable)", so the usage line and docs/CLI.md agree.
- **Reports**: text errors go to standard error as they arrive; JSON keeps
  a file's errors in an arena until its object is written, so `status`
  comes before `errors`. A description that starts with its constraint
  name (the facet errors of D173) is shown without it. In text, a file
  whose validity is not known gets one line on standard error. A FILE
  the parser rejects is reported as `sxml check` does; a failure of the
  validator (a limit, UNSUPPORTED_FEATURE of the 5.8 and 5.9 seams) is
  `failed`, exit 4.
- **Deviation from the design**: a document an include names that cannot
  be read is reported as src-resolve where its components are used, not
  src-include (D173, XSD 1.1 4.2.3 clause 2.4); docs/CLI.md says so.
- **WebAssembly**: the CLI module grows to 4,877,565 bytes stripped
  (1,126,567 gzipped); its ctest budget is raised to 5,500,000 /
  1,250,000 and the growth reported under "Proposed".
- **Measured**: the instance tests of `msData/particles`
  (ms-Particles_w3c, 514 run by both) give the same validity through
  `sxml validate` as through sxml-xsd-test, the 16 that need location
  hints with the hinted documents passed as `--schema`.

### D177. Identity constraints (packet 5.8)
Recorded 2026-10-18. IDENTITY_CONSTRAINTS and the identity seam of D167
(`src/schema_identity.c`, `tests/schema_identity_test.c`); golden
`src/schema_resolve.c`, `src/schema_validate_record.c`.
- **The subsets are checked on the AST, then compiled as XPath; no
  second path engine.** Each selector and field is parsed by the XPath
  4.0 front end (`sxml_xpath_build_ast`) and its AST checked against
  3.11.6.2 and 3.11.6.3: a union of paths of `.` and child NameTest steps
  (`child::` written or not), `.//` only first, `@` or `attribute::`
  only as the last step of a field, no predicates, NameTests QName, `*`
  and `p:*` (not `*:n` or `Q{}n`); otherwise c-selector-xpath or
  c-fields-xpath. Then it is compiled with `xpath.h` (XPath Valid,
  3.13.6.2): the record's in-scope namespaces without the default
  namespace and xml, xpathDefaultNamespace as the default element
  namespace. c-props-correct.1 (a keyref refers to a key or unique) and
  .2 (as many fields).
- **Evaluation on the recorded subtree** (D173), by the one evaluator:
  the selector with the declaring element as context node; elements
  without a governing declaration (skipped, or lax without one) are not
  targets and are ignored as field nodes, as are attributes nothing
  governed (xs:untypedAtomic); a field gives at most one node of a simple
  type or simple content (cvc-identity-constraint.3); a nilled element
  has no value; key sequences are the fields' typed values, derived from
  the annotations (D165). key 4.2.1 (a field without a value), 4.2.2 and
  4.2.3 (a nillable field element); unique 4.1; keyref 4.3.
- **XSD's equality** (Part 2, 2.2.3) is fn:atomic-equal of values of one
  primitive type, xs:untypedAtomic counted as xs:string: NaN equals
  itself (identity), 1 and 1.0 are one xs:decimal, xs:float 1 is not
  xs:decimal 1, xs:string is not xs:anyURI; field values compare item by
  item, so a value equals a list of one. A hash index over
  `sxml_atomic_item_hash` finds the candidates.
- **Node tables without state between calls**: the seam stays one plain
  call per element. A keyref evaluates its referenced key at its element
  and at each descendant whose declaration has it (3.11.5); a key
  sequence met with one node is in the table; one met with several nodes
  is resolved level by level up to the keyref's element with an explicit
  work list (D71): an element's own entry wins, children's entries that
  conflict are dropped. Keys of descendants are evaluated again for each
  keyref (no cache; measured fast enough on the suite).
- **Not done**: compiling an XPath record in its namespace context is
  written here and in `src/schema_assertion.c` (5.9, developed at the
  same time); one shared function is for a sweep.
- **Measured** (release): MS-IdentityConstraint 1,058 of 1,058 run (was
  788; the 48 invalid schemas now rejected), Sun IdConstrDefs 48 of 48,
  IBM identityConstraint 18 of 18, xpathDefaultNSonKeyKeyRefUnique 26 of
  26 (packet gate 80 %); 415 more XSD tests pass, none fewer; XSD in
  scope 41,170 of 41,518 before 5.9 landed, 41,452 (99.8 %) with it;
  gate groups 100 %, 99.5 %, 98.9 %;
  slice 147 of 147. No shared code changed: XP40, XQ40 and XT40 hold.

### D178. XSD and schema-aware conformance sweep 1 (packet 5.13)
Recorded 2026-10-18. Amends D174 (its QT4 outcome rules); resolves three
Proposed items (validation descriptions, locations of configured schema
documents, a simple base for a complex type).
- **Honest outcomes (amends D174).** The QT4 driver no longer counts a
  schema-aware test case that ends at the step or memory limit as "fail",
  nor one that stops at SXML_STATUS_UNSUPPORTED_FEATURE as "not run": D95's
  zero errors must not be met by reclassification. Identity constraints
  (D177) and assertions (D175) run. The two cases D174 moved are fixed at
  their causes:
  - app-CatalogCheck Catalog009 (536 MB): a group by packed every
    variable of the FLWOR into its tuples and concatenated each per group
    (XQuery 4.0, 4.13.8), here `$testsets`, 737 documents, once per
    dependency (18,564 tuples). A variable that no variable reference or
    grouping specification from the group by clause on names (the later
    clauses and the return clause; any reference of the name counts) is
    left out of the tuples: its grouped value is never read
    (`src/xquery_lower.c`). 147 MB; passes.
  - app-Demos itunes (10^8 steps): the query compares the keys of 6,597
    tracks of a 10 MB library once per artist, 367 million steps by D89's
    accounting. A FILTER whose predicate is a leaf (BINARY, UNARY, CAST,
    CASTABLE, INSTANCE_OF over atoms) that reads the focus computes it in
    the filter's own task, one step per item instead of two
    (`src/evaluator.c`), and the typed value of an xs:string node is its
    string value without a cast (`src/xdm_node.c`): 287 million steps.
    The QT4 driver's budget per evaluation is four times
    SXML_QT4_TEST_MAXIMUM_STEP_COUNT, as the XSLT driver's (D166, for a
    whole large input); itunes passes in 85 s (release).
- **Validation descriptions name what failed.** A description is a
  message template (glossary message_template,
  `src/schema_validate_value.c`): static text with placeholders for the element or attribute as written,
  a value (quoted, shortened past 60 bytes, one line), a type ("type
  'xs:decimal'", or "an anonymous type derived from ..."), the expected
  terms of the content model cursor (list_expected), and the facet the
  cast names with its value from the type or its nearest registered
  base. Every cvc-* report uses one: "element 'price' is not allowed
  here; expected 'title'", "element 'p:price' lacks the required
  attribute 'currency'", "element 'price' has the value '-1.005', which
  is not valid for type 'price': the value is below the minInclusive
  facet (...); the minInclusive facet is '0'", identity constraints with
  their element and constraint, assertions with their test. A template
  per site keeps the WebAssembly module in its budget (pieces built at
  run time did not: sxml.wasm 1,997,902 of 2,000,000 bytes). The
  description is made per report, so the first one goes to the error
  record through the item heap (sxml_item_heap_raise_error, D41).
- **Configured schema documents are located by the library**: one that is
  not well-formed is reported at the parser's line and column, one that is
  not a schema document at its document element, both with its URI as
  source name (`sxml_schema_report_request.document_uri`); `sxml validate`
  maps URIs to names only (standard input stays "-").
- **One XPath record compiler**: `sxml_schema_compile_xpath_record`
  (`src/schema_assertion.c`, XPath Valid, 3.13.6.2) for assertions, type
  alternatives, selectors and fields (D177's open item); selectors and
  fields now get the record's base URI too.
- **XSD**: src-redefine 6.2.2 and 7.2.2: a group or attribute group
  redefined without a self-reference gets two unnamed complex types, one
  of the original's content or attributes and a restriction of it of the
  redefinition's, so COMPLEX_TYPES checks the restriction with its own
  rules (cos-content-act-restrict; derivation-ok-restriction 2 to 4, as
  7.2.2 says) at the redefinition; a default open content with
  appliesToEmpty false is dropped when the explicit content type is empty
  (3.4.2.3.3 clause 6.1, `open_content_skips_empty`); the children of
  xs:override are read with the overridden document's defaults (4.2.5:
  they stand in D2'); a complex type with simple content is registered
  with its simple base (types.c accepts it, 3.4.1); "-0000" is year zero
  (Part 2, D.3.1; also QT4 cbcl-cast-gYearMonth-004a).
- **Not done**: is-id of union and list elements (the start event is sent
  before the value is known; it needs the start held back to the end of
  simple content); src-include for an include that cannot be read (XSD
  1.1, 4.2.3 makes it no error; D176's deviation stays); the assertion
  leftovers of D175 (no test of the suite needs them); the pattern facet
  on canonical forms was already done (`sxml_cast_check_item_facets`).
  Remaining XSD failures: XSD 1.0 rules XSD 1.1 dropped or changed
  (prohibited uses with fixed values, annotations), xs:NOTATION as a union
  member (D171), XML 1.1 instances, Element Declarations Consistent with
  wildcards and type tables (saxon wild078, 079, 081).
- **Also**: src-ct for mixed attributes of xs:complexType and
  xs:complexContent that disagree and for xs:openContent in a restriction
  without a particle (the schema for schema documents, XSD 1.1 bug
  16786); Wildcard Subset with its disallowed-names clauses (3.10.6.2
  clauses 2 to 4) in one function, `sxml_schema_wildcard_is_subset`, for
  content and attribute wildcards (two copies merged).
- **Measured** (release, every suite run with this code): XSD in scope
  41,483 of 41,518 (was 41,452; gate groups 100 %, 99.7 %, 99.6 %; no test
  set lost a test; slice 147); XP40 30,931 (was 30,924), XQ40 41,273 (was
  41,261), both 0 errors, schema-aware XQ40 1,236 of 1,516; XT40 7,992
  (was 7,990); QT4 slice 198.

### D179. Schema-aware XSLT (packet 5.12)
Recorded 2026-10-18. Replaces D162's two XSLT rejection places
(`reject_schema_awareness`'s XTSE1660 and XTSE1650 in `src/xslt_read.c`,
D163, D169) with D174's mechanisms; golden `src/xquery_static.c` (import
schema), `tests/xquery_schema_test.c`.
- **xsl:import-schema** (3.13) is a new module, `src/xslt_schema.c`
  (glossary schema_import), run after reading: the declarations of the
  sealed AST become the documents of one `sxml_schema_create` (the
  synthetic schema document): an inline xs:schema as a NODE document of
  the module's tree (base URI the declaration's), a schema-location as a
  RESOLVED document resolved against the declaration's static base URI
  with its namespace checked, a namespace alone as a RESOLVED document
  by namespace. Of one namespace only the declarations of the highest
  import precedence count; a namespace a host schema defines (or XSD's)
  is taken from there; a location or namespace the resolver does not
  deliver is dropped (3.13: not an error by itself), probed with
  `sxml_schema_scope_probe_document`, now shared with XQuery's import
  schema (one probe, `src/schema_scope.c`). The processor keeps the
  schema (`sxml_processor_keep_schema`, D174). XTSE0215 (inline schema
  with a schema-location or another namespace), XTSE0220 (any schema
  error, its constraint and description in the message). Schema roles
  (4.0) are not read: one unnamed role.
- **In-scope schemas**: the host's (`static_context.in_scope_schemas` of
  the stylesheet configuration, now read, `xslt.h` comment, additive),
  then the imported schema; neither gives the processor's built-in
  schema. They live in the AST (`sxml_xslt_ast_set_in_scope_schemas`),
  the lowering's base static context carries them into every module's
  XPath static context and IR module and the unit (D174's one field), so
  `as` types, schema-element(), casts, constructor functions and
  type-available see them. The host's schemas are what makes a source
  the host validated and the stylesheet's types the same types (schema
  types are never interned).
- **Validation is D174's validate row with XSLT's rules**: mode
  `SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES` (+4) and the descriptor
  `validate-items` (item()*): every item validated (documents, elements,
  attributes) or kept; XTTE1550, XTTE1512, XTTE1510, XTTE1515, XTTE1540,
  XTTE1555 (a document's cvc-id), XTTE1535. One lowering helper
  (`sxml_xslt_lowering_bind_validation`, `src/xslt_lower_construct.c`)
  reads [xsl:]validation, [xsl:]type or the default-validation in force
  and wraps the constructed nodes of xsl:element, xsl:attribute,
  xsl:copy, xsl:copy-of, xsl:document, xsl:result-document (the result
  tree's document node), xsl:source-document and literal result
  elements; preserve is the PRESERVES_TYPES construction flag (XQuery's
  construction mode), strip the default; XTSE1505 in the reader,
  XTSE1520 and XTSE1530 at lowering; an unprefixed [xsl:]type is in the
  default namespace for elements and types. Temporary trees (9.4) are
  built as preserve (25.2). The evaluator delivers a constructed node
  into a receiver that preserves types also when the node itself strips
  (the same tree either way), so variables keep the D166 streaming.
- **input-type-annotations="strip"** (4.1) is the document preparation
  of whitespace stripping (`src/xslt_whitespace.c`): the same copy,
  without annotations, for the global context item and documents fn:doc
  and document() load.
- **Typed modes** (6.9.4): a mode with typed="yes", "strict" or "lax"
  (or "no") tries one rule before all its template rules, of the
  highest precedence and priority, made from two XPath snippets: it
  matches an item that is not an XNode or an untyped (typed) element or
  attribute, and its template raises XTTE3100. Name tests are not read as
  schema-element() (strict, lax; XTSE3105 not raised).
- **fn:document**'s xsd-validation option (4.0) validates each document
  with fn:parse-xml's `sxml_schema_scope_validate_document` (D174).
- **is-schema-aware** is true; default-validation strict or lax is
  XTSE0020 (only preserve and strip are allowed). A schema error whose
  description already starts with its constraint name is raised without
  naming it twice (`sxml_schema_scope_raise`).
- **XSLT driver**: schema_aware is supported but stays a non-goal for
  scope (outside the D95 denominator, as QT4's schema features, D174);
  a test case requiring it, or whose environment has a schema or a
  validated source, is counted in the area "schema-aware". The
  environment's schemas compile into one schema that validates sources
  (strict, lax) and is the stylesheet's host schema; schema documents
  are served by suite file and namespace (`resource_table.h`). Assertions
  see the namespaces declared inside `<result>` too (an all-of may
  declare a prefix).
- **Not done**: typed modes' schema-element() reading of name tests
  (XTSE3105) and typed="~ItemType"; ID/IDREF
  is checked when an element (not a document) is validated, where XSLT
  leaves it to documents (the validator has no switch for it; validation
  1601, accumulator-073); xs:untypedAtomic as [xsl:]type; declarations
  of xml:lang and xml:space (no xml.xsd built in); xsl:copy of an element
  under preserve keeps its annotation instead of xs:anyType; the fn
  namespace schema (json-to-xml typed, D174); XTSE0265; schema roles.
- **Measured** (release, HEAD 8d6cad4 plus this packet): the schema-aware
  XT40 area 621 of 971 (64.0 %; feature schema_aware, a schema or a
  validated source); of the 946 that require schema_aware, the 679 that
  need no other non-goal (streaming 247, dynamic_evaluation 15, XSD_1.1,
  dtd, XML_1.1) pass 615 (90.6 %; the proposed gate is 60 %). XT40 8,642
  passed (was 7,990; gate rate 8,027 of 8,728 in scope, was 7,990), 560
  failed, no test case that passed before fails; XSLT slice 147 of 154;
  XP40 30,924 and XQ40 41,261 passed, 0 errors; slice 198. Six test cases
  that asked for schema_aware to be absent no longer run.
- **Six XT40 errors**, all in the schema-aware area and all a source the
  environment asks to validate that does not validate, reported rather
  than reclassified: as-3301, as-3401, match-208, match-209 give xs:ENTITY
  attributes the names of parsed entities (XSD 1.1 Part 2, 3.4.11: an
  ENTITY names an unparsed entity, so the sources are invalid);
  catalog-005b and catalog-009 compile XMLSchema.xsd, the schema for
  schemas, with the catalog's schema, which sxml rejects as
  sch-props-correct.2 against the built-in components (for 5.13's XSD
  sweep: whether a schema document for the XSD namespace may restate
  them).

### D180. Conformance sweep 2 (packet 5.13)
Recorded 2026-10-18. Clears D179's six XT40 errors at their causes;
resolves the Proposed item "xslt.h reads the host's in-scope schemas".
- **The schema for schema documents compiles beside the built-ins**
  (catalog-005b, catalog-009). XSD 1.1 Part 1 puts the built-in
  components in every schema (4.2.1, schema(D)), has every processor
  know them without reading (C.2.1, hard-coded schemas) and counts two
  top-level type definitions of one name as one component (3.4.6.5,
  the note on component identity); XMLSchema.xsd calls its xs:anyType
  "not the real urType, but as close an approximation as we can get".
  So a global type definition whose name a built-in type already has is
  that built-in component: the document's own definition is left
  unnamed, and sch-props-correct.2 does not hold against it. One place:
  `enter_global` (`src/schema_read.c`); other duplicates in the XSD
  namespace still conflict, and an xs:import of it was already ignored.
  With it, the built-in xml.xsd that stands in for an import of the xml:
  namespace the resolver does not deliver is used only when no document
  of that namespace is there (4.2.6.2: a processor may keep the first
  import of a namespace; `src/schema_assemble.c`): XMLSchema.xsd imports
  it from www.w3.org beside the environment's xml.xsd. catalog-009
  passes; catalog-005b fails on its own data (29 of the suite's
  stylesheets use XSLT 4.0 instructions and attributes, xsl:item-type,
  method="csv", line-ending, that the catalog's schema-for-xslt40.xsd
  does not have yet).
- **A source that does not validate is sxml's answer, not an error of
  the environment** (as-3301, as-3401, match-208, match-209). Their
  sources give xs:ENTITY attributes the names of parsed entities
  (`<!ENTITY eac "&#xE9;">`). XSD 1.1 Part 1, 3.16.4 String Valid clause
  2 requires a declared entity name, an unparsed entity of the document
  (Part 2, 3.4.11; Saxon's own XSD test saxonData/Id id017.n01 expects
  invalid without one). sxml's parser reads the internal subset and
  passes unparsed entities on (`src/xml.c`), so nothing of the DTD is
  missing: the right outcome needs no DTD support, and the test cases
  declare no dtd dependency, so "not run" has no ground in the catalog.
  The expected results are Saxon's, which does not make the check
  (report/results_XSLT3.0: pass; no other submission is schema-aware).
  Validation is part of the processor under test (the validator of
  xsl:import-schema and the validation attributes), so the XSLT driver
  judges an invalid source like any dynamic error of the transformation:
  the assertions decide (an expected error could pass), here "fail"
  (`sxml_xslt_test_judge_invalid_source`, `tests/xslt/driver/
  xslt_execution.c`). An environment schema that does not compile stays
  an error. The QT4 driver has no such case.
- **validate-items is a row of its own** (`call_validate_items`,
  `src/function_library_schema.c`): XSLT's rules are no longer a branch
  of XQuery's validate, so what links only XQuery's (the JavaScript
  module) does not carry them. sxml.wasm was 2,001,084 bytes stripped
  after 5.12, over its 2,000,000 budget; now 1,998,723 (696,023
  gzipped of 700,000).
- **xslt.h's in_scope_schemas comment is accepted** (D179's Proposed
  item): it documents what the stylesheet does with an existing field of
  xpath.h's static context (D174), is additive (a host that passed
  schemas before had them ignored), matches `src/xslt.c` and the
  lowering, and XSLT 4.0 allows it (F.2: an implementation may make
  schema components available beyond xsl:import-schema). It is what lets
  a host-validated source and the stylesheet share one schema's types.
- **Not done**: is-id of union and list elements (D178) is not cheap:
  the element's start event goes to the builders, with its attributes
  and namespace bindings, before its content; holding it to the end of
  simple content means holding those and any comments and processing
  instructions too, and tree.h is frozen (D99), so no later mark is
  possible. Left for a sweep that needs it (no test of the suites does).
- **Measured** (release, all four suites with this code at ae616ca):
  XT40 8,650 passed (was 8,649), 558 failed, 0 errors (was 6), gate rate
  8,029 of 8,728; schema-aware XT40 627 of 971; only the test sets as,
  catalog and match changed, none lost a test; XP40 30,931 and XQ40
  41,273, 0 errors; XSD in scope 41,483 of 41,518; slices 198, 147
  (XSLT, of 154) and 147 (XSD).

### D181. Verification in proportion; agent prompts in the repository (owner)
Recorded 2026-10-19 on the owner's word: re-running all builds and fuzzing
everything takes too long here; the owner will provision a machine for
that. Amends the done-when of the packet procedure (D123's local checks).
- A packet builds the `debug` preset (Clang, ASan and UBSan, `-Werror`),
  runs the unit and CLI tests it touches, keeps `clang-tidy` and the
  call-cycle check clean for its files, and runs the conformance test
  sets it affects with no fewer passes. `debug-gcc` and `wasm` only when a
  change is platform-sensitive or may grow `sxml.wasm` noticeably.
- Full suite runs, all presets, the full lint and fuzzing move to the
  owner's machine. Gate boxes that need them stay open, marked "deferred
  to the owner's machine (D181)"; `docs/CONFORMANCE.md` is regenerated only
  from full runs.
- The agent prompts live in `docs/agents/` (`BRIEF.md`, `PACKET.md`, and
  `QUEUE.md` for the running and next work), so any session can continue
  the work; commit messages carry the reasons and the numbers.
- The user-facing CLI reference is `apps/sxml/README.md`; `docs/CLI.md`
  keeps the design rules (D44).

### D182. Phase 5 gate review (packet 5.14)
Recorded 2026-10-19 by the review agent (D11), after D151, D161, D169
and D181. **The owner moved the confirming runs to a machine of their
own** (D181): the gate boxes that need full suite runs or fuzzing stay
open, marked
"deferred to the owner's machine (D181)"; the review's own runs below
met every number. Checked locally (D123): `tools/check-main.sh` on clean
HEAD 3b0812a (the review's code changes) builds `release`, `debug`
(ASan, UBSan), `debug-gcc` and `wasm`, and every test passes; its full
lint was stopped by the owner's change, but `tools/private-build.sh
--lint` over the same files was clean (clang-tidy, ast-grep, no call
cycle in 176 files). The dashboard is regenerated at 3b0812a.
- **Numbers** (release; run-qt4.sh XP40 and XQ40, run-xslt.sh,
  run-xsd.sh; every test set finished, 0 crashes, no leak reported by the
  drivers' budgets, 0 "error" outcomes in all four suites), at 02303a7
  and unchanged at 3b0812a, all 150,228 outcomes case by case: XSD in scope 41,483 of
  41,518, **99.9 %** (gate 90 %; the 41,538 of D165 became 41,518 with
  D168's per-element scope); datatypes 30,598 of 30,598, **100 %** (gate
  95 %), structures 8,600 of 8,626, **99.7 %** (gate 80 %), XSD 1.1
  2,285 of 2,294, **99.6 %** (gate 70 %). Schema-aware XQ40 1,236 of
  1,516, **81.5 %** (gate 70 %; XP40's 279 of 315, 88.6 %); schema-aware
  XT40 627 of 971, **64.6 %** (gate 60 %). XP40 in scope 30,783 of
  31,543, **97.6 %** (gate 90 %), XQ40 40,315 of 41,174, **97.9 %**
  (85 %), XT40 8,029 of 8,728, **92.0 %** (70 %), passed counts 30,931,
  41,273 and 8,650, none lower than at the Phase 4 gate (30,647, 40,034,
  7,990); the 12 core sets 88.5 % (mode) to 100 %. Slices: qt4/slice
  198, qt4/xquery-slice 186, xslt/slice 147, xsd slice 147 (ctest, every
  preset).
- **Fuzzing**, 300 s per target, eight at once on four cores (fork mode
  for the evaluating targets): cast_fuzz 1,790,230 runs, regex_fuzz
  656,221, xml_parse_fuzz 79,380, xpath_compile_fuzz 10,229,
  xpath_evaluate_fuzz about 8,300, xquery_fuzz about 9,300, xslt_fuzz
  about 10,500, schema_fuzz about 11,700; no crash, leak or sanitizer
  report. Timeouts for triage only (P9): one xquery_fuzz and three
  xslt_fuzz inputs, all suite texts slow by nature under ASan (a FLWOR
  over 200,001 maps, nested counts to 1,000, match="$nodeset" over
  1,000 nodes), 8 to 12 s alone. **schema_fuzz**
  (`tests/fuzz/schema_fuzz.c`, new): the input is a schema document, a
  zero byte and an instance; the schema is compiled from text with a
  resolver that serves nothing, and the instance validated strictly while
  parsed and laxly by `sxml_schema_validate_node`; seeds are the first
  schema document of each test group of the XSD suite, alone and with
  each instance of the group (`tools/fuzz-seeds.py`).
- **The step budgets are fair test limits, not masks.** QT4's 4 x 10^8
  per evaluation (D178) and XSLT's (D166, judged by D169) apply to the
  test drivers only, below the library's default of 10^9 (D89).
  app-Demos itunes is
  quadratic by its own text: `local:tracks-by-artist` filters all 6,597
  tracks once per artist, three times per artist (one-hit-wonder,
  years-by-artist, the title), and each find-key-string scans a track's
  keys; 287 million steps by D89's accounting, 72 s (release, this
  machine). Sampled stacks are flat (filters, atomization, general
  comparisons, the axis iterator), with no schema code among them: the
  schema-element() tests resolve statically to element tests (D174), so
  validation costs nothing per step. What it shows is a general
  evaluator item, a join that a processor with an index on
  `$seq[f(.) = $key]` would do in linear time, and about 4 million steps
  a second; listed, not a Phase 5 defect.
- **No driver reclassifies outcomes**: the QT4 driver's only rule left
  is D95's, for test cases outside scope that stop at a missing non-goal
  (never for schema-aware ones, D178); the XSLT driver counts
  SXML_STATUS_UNSUPPORTED_FEATURE as "fail" and judges an invalid source
  by the assertions (D180). The XSD driver still made a validation that
  meets SXML_STATUS_UNSUPPORTED_FEATURE "not run" (D168's rule from
  before 5.8 and 5.9): now "fail", reason "instance: unsupported
  feature", as the XSLT driver does. No test reached it (no schema code
  returns the status), so no number moves.
- **D71**: no call cycle (tools/check-call-cycles.py). Component graphs
  (type derivation chains, group and attribute group references,
  substitution groups, union members, redefine and override chains),
  content models (the particle tree, nested all groups, the
  restriction walk) and identity constraint tables are explicit work
  lists or stacks with a mark per component; schema finalization
  releases arenas and the compiled XPath units without calling back
  into the schema, since the processor keeps schemas that queries and
  stylesheets import (D174's avoided cycle). Through function pointers:
  the validator and the assertion copy are tree builder filters that
  pass events on and never call their source; an assertion, type
  alternative or identity constraint runs the evaluator on a dynamic
  context of its own, nested once at most, because their static contexts
  have no in-scope schemas (no `validate`, fn:xsd-validator refuses
  schemas beyond the in-scope ones) and their resolver serves nothing.
- **Fixed and merged** (conceptual smallness):
  - `sxml_xml_text_is_ncname`, `sxml_xml_text_is_whitespace` and
    `sxml_xml_text_next_token` (`src/xml_internal.h`) replace private
    copies: NCName checks in seven files (schema reader, cast's QName
    lexical form with its own copy of the name character table,
    xslt_read's ASCII approximation, which let any non-ASCII byte
    through, the XSLT lowering, fn:element-to-map, fn:id and
    fn:path, format-date's calendars), whitespace-only text in seven
    (schema reader and validator, deep-equal, serialization, XQuery's
    boundary whitespace, XSLT output and reading), token splitting in
    two; schema's `views_equal` (two), `sxml_trim` and `sxml_text_is`
    are `sxml_byte_view_equals` and `sxml_xml_text_trim_whitespace`; the
    content model's heapsort of keys is `sxml_sort_in_place`. sxml.wasm
    1,997,773 bytes (was 1,998,723).
  - `tests/schema_validate_test.c` has an allocation-failure loop over
    compilation (with an assertion, a key and a type alternative),
    validation while parsing and `sxml_schema_validate_node`, with leak
    checks: the last part of D161's rule schema.h lacked.
- **Larger duplicates, listed** (one shared mechanism each, later):
  the reader's own lexical parsers for xs:boolean and
  xs:nonNegativeInteger next to the cast engine's (and XSLT's yes/no
  reader); Gregorian day arithmetic in four places (cast_facet.c's
  reference dateTimes for duration order, format_date.c, processor.c's
  clock, operator_temporal.c's cycle days, which is XSD Part 2,
  appendix E too); compile reports that append a subject to their
  description built in three ways (reader and resolver through
  xml_buffer, the content model through report_text); bounded text
  writers for the text forms in four files (ir.c, xslt_ast.c,
  schema_text.c, schema_content_model.c); three private heapsorts in
  serialization (serialize.c, serialize_json.c, serialize_xml.c). D169's
  list stands (typed array allocation, group-adjacent keys, run
  detectors).
- **Delayed features (D162)**, one place each: XSLT's schema roles and
  typed="~ItemType" read nowhere (one unnamed role); is-id of union and
  list elements (D180); fn:xsd-validator's schema options refused in
  `call_xsd_validator` with its error. No D162 rejection of Phase 5's
  features is left in the language layers.
- **`sxml validate` matches docs/CLI.md**: every example of the page
  gives its output and exit code; the usage errors (no `--schema`, no
  FILE, a bad `--mode`, `-` twice) exit 2, a missing SCHEMA or FILE 3, a
  schema error and a FILE that is not well-formed 1. README is being
  reorganized and was left alone; its "still provisional" for schema.h
  is out of date after this record.
- **schema.h is stable** (D161's rule): consistent with `xpath.h`,
  `xquery.h` and `xslt.h` (quintets, configuration structs, error
  records, report sinks), every declaration documented, every function
  tested through the public API, now with an allocation-failure loop,
  and `sxml validate` on it alone. From now on it changes as every public
  header does (D47).
- **Proposed, resolved here**: the five items whose design was accepted
  or that need no owner (match="$var" membership, dynamic errors in
  included modules, package versions for host resolvers, the XSLT
  functions at run time, globals computed on first read) are accepted
  as designs for a later sweep and leave Proposed; they are listed in
  the LOG. Left for the owner: the WebAssembly budgets (four items;
  `sxml.wasm` is 1,997,773 of 2,000,000 bytes stripped and 695,343 of
  700,000 gzipped, the CLI 4,950,289 of 5,500,000 and 1,154,545 of
  1,250,000) and XQuery's omit-xml-declaration default.
- **Not done** (listed in the LOG): is-id of union and list elements;
  XSLT's ID/IDREF checks for elements as well as documents, schema roles,
  xs:untypedAtomic as a type, xml:lang declarations (D179); the XSD
  failures D178 lists (XSD 1.0 rules XSD 1.1 changed, NOTATION union
  members, XML 1.1 instances, Element Declarations Consistent with
  wildcards).

### D183. Phase 7 plan: one packet per deferred feature, optional features for size (packet 7.1)
Recorded 2026-10-19, after D95, D97, D154, D157, D162, D165, D166, D181.
Plans Phase 7 (D162) in `docs/tasks/phase-7.md`, as D165 planned Phase 5.
- **Packets** (numbers in wave order; "a, b" are packet groups):
  - 7.2: optional features and the size budget;
  - 7.3: xsl:evaluate;
  - 7.4: XSLT backwards-compatible behaviour and XPath 1.0 compatibility
    mode;
  - 7.5: disable-output-escaping;
  - 7.6a, b: streaming (streamability analysis, then streamed
    evaluation);
  - 7.7a, b, c: the Update Facility;
  - 7.8: EXPath Binary;
  - 7.9: EXPath File;
  - 7.10a, b: HTML5;
  - 7.11: fn:transform;
  - 7.12a, b, c: Full Text;
  - 7.13: XML 1.1 input;
  - 7.14a, b: DTD processing;
  - 7.15a, b: UCA;
  - 7.16: timezone database;
  - 7.17: calendars and languages;
  - 7.18: Invisible XML;
  - 7.19: review.

  7.3, 7.5, 7.13 and 7.14 are proposed for the owner (below). The order
  weighs the cases unlocked, user demand and risk:
  - 7.4 comes early because every `version="1.0"` stylesheet now raises
    XTDE0160 when evaluated;
  - Full Text is late because it is the riskiest;
  - UCA, timezones, languages and iXML come last: they are few cases
    and much data.
- **Counts** (not run at 3b0812a; first unmet dependency where the
  dashboard has it, else from the catalogs):
  - XT40: streaming 2,932, backwards compatibility 124, xsl:evaluate 83,
    dtd 78, XML 1.1 65, disable-output-escaping 32, numbering languages 14;
  - XQ40: HTML 1,401, Update 825, Full Text 685, Binary 673, XML 1.1 313,
    File 271, fn:transform 133, UCA 49 (+13), infoset-dtd 32, languages 24,
    Olson 8, iXML 10, XPath 1.0 mode 5;
  - xmlconf: 275 XML 1.1 cases and 265 with external entities.

  About 4,400 of XQ40's 5,505 not-run cases and 3,330 of XT40's 4,659.
  staticTyping, the xsl-stylesheet PI, HTML4 output, XInclude and
  remote_http stay unplanned (listed in the plan).
- **Mechanisms, no parallel machinery**:
  - xsl:evaluate and fn:transform compile at run time and run a unit
    evaluation (D154, D157); fn:transform is supplied by the XSLT layer,
    as fn:load-xquery-module is supplied by XQuery.
  - HTML5, iXML, XML 1.1 and DTD entities send tree builder events; HTML
    tree construction works on a working node table first, because the
    adoption agency moves built nodes.
  - DTD validation is schema validation against components built from
    the DTD.
  - Updates are applied by rebuilding trees through the tree builder, so
    `tree.h` stays frozen (node identity across an update is not
    observable within a snapshot).
  - Streaming is first a static streamability pass with unstreamed
    evaluation, which yields every result and static error; then a
    receiving task (D166) fed by the parser. The feature is claimed only
    then.
  - Full Text selections compile into a match program as regexes do
    (D53); Full Text goes in the XQuery grammar only.
  - The XPath 1.0 mode is a static-context flag that the lowering reads.
  - disable-output-escaping uses the JAXP processing-instruction pair,
    because text nodes cannot carry a mark.
- **Header changes** (additive, each with its record): `processor.h`
  (optional features, a file-system interface, resource kinds for Full
  Text, entities and optionally collation and timezone data), `xpath.h`
  (the 1.0 compatibility flag), `xquery.h` (updating queries), `xml.h`
  (DTD options). No substrate header changes.
- **Gates** (proposed): each feature has its own gate in its packet, from
  75 % (Full Text, languages) to 95 % (Binary, HTML5, the xmlconf
  cases). The Phase 7 gate is every accepted feature's gate, the earlier
  gates still held with no passed count dropped, no error outcome, the
  owner's size budgets, and fuzz targets for every new parser.
- **PLAN.md**: the Phase 7 row names the optional features and the four
  proposed additions.

### D184. Phase 7 scope, size and access choices (owner)
Recorded 2026-10-19 on the owner's answers to D183's questions.
- **Scope:** xsl:evaluate (7.3), disable-output-escaping (7.5), XML 1.1
  input (7.13) and DTD processing (7.14) join Phase 7.
- **Size:** link-time optional features through one additive
  `processor.h` view (one build, no `#if`), with large data (DUCET, CLDR
  rules, TZif files) supplied by the host through the resolver. Budgets:
  2,100,000 bytes for the core `sxml.wasm`, 3,000,000 for the full module.
  Packet 7.2 builds the mechanism and sets the budgets.
- **XQuery Update:** 3.0 first, then the 4.0 draft additions (7.7).
- **EXPath File in the CLI:** off unless `--allow-file-access` is given
  (D44); the library leaves file access to the host's resolver.
- Still open from D183: the gate numbers, adding the iXML tests and a
  sample of Unicode's CollationTest, and which languages, calendars and
  tailorings (7.17 proposes en, de, fr, it, es, nl; ISO, AD, BE, JE, OS, AH).

### D185. XSLT conformance sweep after the Phase 4 gate (packet 4.19)
Recorded 2026-10-19, after D29, D98, D152, D160, D169, D181. Verified as
D181 asks: the debug preset, the unit and CLI tests touched, clang-tidy
and the call-cycle check on the files changed, and the whole XT40 suite
with the release driver after each change, compared case by case (it
runs in about a minute here). **XT40 8,650 to 8,759 passed, 0 errors,
no case lost.**
- **Type patterns "~T" (priority, 34 failing): not implemented, by
  D29.** The project's draft (13 October 2026) has no type patterns:
  PR 2911 (14 September 2026) replaced them with atomic(), record(),
  map and array patterns, which sxml has; "~T" survives only in a few
  examples and in the typed attribute of xsl:mode. The suite's priority
  cases are written against the older draft, and its other expectations
  there (priority 1.0 for ".[p]", 0.25 for "{42}" and "array({'x'})")
  contradict 6.4 of the project's draft too. Nothing in priority changes.
- **Extension instructions call named templates** (10.1.3; call-template
  -ext 7 to 32 of 32): an extension instruction named after a template
  it binds to is xsl:call-template of it; each attribute is parsed while
  lowering, as a kept AST (D152, `sxml_xslt_lowering_parse_attribute`),
  as a value template for an xs:boolean or atomic parameter (boolean
  words; xs:untypedAtomic) or as an expression otherwise.
- **xsl:item-type and xsl:record-type** (5.4, D98; record 22 to 39,
  type-alias 13 to 25): `src/xslt_named_type.c` does what XQuery's
  declare type and declare record do: the static context's named item
  types (resolved in rounds; XPST0051, XTSE4035, XTSE4030) and
  nominative records in the type registry (XTSE4050). Both are
  components of a new package component kind, item type, so packages
  bind them by 3.4.5.5. xsl:field takes the draft's static and select.
  Delayed (D162): the record constructor function (5.4.2.2).
- **Available documents are source documents** (4.2.2; snapshot 16 to
  34, and merge, document): a transformation strips each available
  document once and hands the copies to fn:doc; a global context item
  that is one becomes its copy. fn:snapshot of a namespace node.
- **merge** (67 to 79; 29 more need streaming): the XSLT driver serves
  collections (also collection 0 to 5); XTSE1060, XTSE1070, XTSE3470,
  XTSE3500 for the current-group functions in patterns; XTDE2210 for
  fixed merge-key attributes that differ (raised when evaluated);
  XTTE2230 for incomparable merge keys. Left: XTDE3490 (map:get of an
  unknown source name, in `src/xpath_lower.c`), XTDE2220, computed
  XTDE2210, validation codes.
- **Errors in included modules** (D169's accepted design, done): each
  static context's IR module has its stylesheet module's URI as source
  name, and templates, stylesheet functions, attribute sets and global
  initializers take their element's IR module. Inline functions and
  xsl:iterate bodies keep module 0.
- **Duplicates** (D169's list): `sxml_allocator_allocate_array`
  (`src/memory_internal.h`) is the one array allocation helper; the XSLT
  copies use it, the others move when their files are next touched. The
  key equality of group-adjacent and group-starting-with stays: the
  FLWOR's keys_are_equal is in `src/xquery_flwor.c`, another packet's
  file at the time.
- **match="$var" membership: not done** (D169's accepted design stays
  open). The parts exist: the evaluation memo (D149) that keeps key
  indexes and cached calls, and a map from generate-id() to the node,
  built once by the cached-call row for a pattern rooted at a global
  variable, makes each test one lookup. The match expression is an XPath
  AST, so it needs a private function name it can call, or the match
  lowering must bind the index as a pattern variable; the rows live in
  `src/function_library_xslt.c`, another packet's file at the time.

### D187. The XQuery Update Facility 3.0 (packet 7.7a)
Recorded 2026-10-19, after D99, D137, D162, D183, D184. The owner chose
XQUF 3.0 first (D184): packet 7.7a designs it and does the 3.0
expressions and their application (the task file's 7.7a and 7.7b);
7.7b is now the 4.0 draft's additions and 7.7c fn:put and revalidation.
- **Grammar** (`grammars/xquery`, D12): XQUF 3.0, Appendix A: the
  revalidation declaration, "declare updating" (the compatibility
  annotation, accepted before the other annotations), insert, delete,
  replace (value of), rename, copy-modify, transform-with (at the arrow
  level, under XQuery 4.0's pipeline) and "invoke updating". The 17 new
  keywords stay names. Every query and module of the upd-* sets parses;
  the rest of XQ40 parses as before.
- **AST**: ten kinds, the fields modify and source, the keywords FIRST,
  LAST, INTO, AFTER, BEFORE, SKIP, the flag VALUE_OF.
- **Static rules** (XQUF 3.0, 2.2, 5.1 to 5.4) in `src/xquery_update.c`:
  one post-order walk over a module's AST by its links (no stack, D71)
  gives each expression a category, simple, updating or vacuous, and
  checks XUST0001, XUST0002 and XUST0028. %updating and %simple are
  accepted on functions and function items, XUST0032 on variables and
  XUST0033 twice: this replaces D162's place, the XQST0045 of
  `check_annotations` in `src/xquery_static.c`.
- **A pending update list is a sequence.** Each update primitive is a
  function item named upd:insertInto ... upd:rename whose body raises
  XUDY0038 when called; its captured values are the target, the
  parameter and the kind. So no item kind is added (xdm.h is frozen,
  D99), the evaluator keeps no update state, and merging the lists of
  the comma, FLWOR, conditional and function call expressions is
  concatenation (upd:mergeUpdates is a union); a try clause that fails
  loses its updates with its value. The static rules keep primitives
  where no XDM value is read.
- **Lowering** (the part `update`, `src/xquery_lower_update.c`): every
  update expression is a STATIC_CALL of an internal native row in no
  function table, as group by is (D137), which checks the operands
  (XUTY0004 to XUTY0013, XUDY0009, XUDY0023, XUDY0027, XUDY0029,
  XUDY0030, XUTY0022) and returns the primitives. replace value of takes
  its value through a TEXT_CONSTRUCTOR (5.4.3.2: "as in a text node
  constructor"). copy-modify is copy(E) per variable, the modify clause,
  then modify-copies, which applies the clause's list to the copies
  (XUDY0014) and rebinds the variables for the return clause.
- **Application** (`src/xquery_update_apply.c`, upd:applyUpdates): the
  compatibility checks (XUDY0015, XUDY0016, XUDY0017, XUDY0024), then
  each target tree is rebuilt through the tree builder in one pass over
  the old tree with every primitive of a node applied in the order of
  3.2.2; tree.h does not change (D183). Changed elements and their
  ancestors become untyped; a duplicate attribute is XUDY0021.
- **The host** (`include/sxml/xquery.h`, additive): an updating query is
  a unit whose main function carries %updating;
  sxml_xquery_query_is_updating says so, and the evaluation request's
  update sink receives each changed tree (its old and new roots) after
  the snapshot. How a change is made persistent is the host's
  (upd:applyUpdates: implementation-defined).
- **QT4 driver**: XQUpdate is supported; the tests of a test case run in
  order, each updating one's changed trees replace the sources they
  came from, and the last test's result is checked.
- **Done in 7.7a** (amended at the end of the packet): besides the
  above, transform-with (a MAP whose body copies the item and lowers the
  modify clause with the copy as fixed focus), the prolog's construction
  and copy-namespaces modes for copies and for the nodes insert and
  replace copy in, upd:removeType (only the primitives that change a type
  touch their node and its ancestors, which become xs:anyType when
  typed), upd:propagateNamespace under no-inherit, XUDY0021 also inside
  removed subtrees, and `sxml xquery` writing the context document as an
  updating query left it (no flag; the file is not changed).
- **upd-* (XQ40, release driver)**: before 0 of 825 run; after 733
  passed (10 of them with another error code), 24 failed,
  0 errors, 68 not run: 27 staticTyping, 40 revalidation lax and strict
  (XUST0026 until 7.7c), 1 that needs skip unsupported.
- **Left for 7.7b (the 4.0 draft)**: the updating return clause of
  copy-modify (id-transform-expr-014), XUDY0038 on a dynamic call of an
  updating function item (no test), the 4.0 draft's other additions.
  **For 7.7c**: fn:put (13 cases) and revalidation lax and strict (40,
  with revalidate-valtrans-ins-003). **Not ours**: copies made with
  construction strip keep is-id and is-idrefs (sxml_tree_builder_copy_node,
  src/tree.c: "the ID properties stay"), so 11 cases that ask for schema
  IDs to go fail (setToUntyped, transform-expr-045 to -048); 6
  propagateNamespace cases differ only in whitespace that assert-xml
  compares; valskip-repnode-006 revalidates a source with an IDREF.

### D188. disable-output-escaping: a text node mark (packet 7.5)
Recorded 2026-10-19, after D99, D163, D183, D184. The owner's prompt for
7.5 asked for a mark on text nodes as D163 added the CDATA request,
in place of D183's processing-instruction pair (glossary
`disable_output_escaping` replaces `output_escaping_marker`).
- **Substrate, additive** (`include/sxml/tree.h`, frozen by D99):
  `sxml_text_event.disables_output_escaping` and
  `sxml_node_description.disables_output_escaping`, false by default
  (designated initializers keep every caller as it was). Arena trees keep
  it in a node flag (the flags field is 16 bits, the node stays 56
  bytes); copies (`sxml_tree_builder_copy_node`) keep it.
- **Per character** (XSLT 4.0, 26.1.8: "an additional boolean property
  disable-escaping associated with every character in a text node"):
  inside a parent, text events that differ in the mark make separate
  text nodes, so `<out>` with "<" then an unescaped "&lt;" serializes as
  `&lt;&lt;` (doe-0182 to doe-0186). Such a final result tree has
  adjacent text nodes, a deviation from XDM 4.0, 6.7.1 limited to trees
  that use the deprecated feature; parentless text keeps one node.
- **Where it is set**: XSLT lowering of xsl:value-of and xsl:text with
  disable-output-escaping="yes" (construction flag
  DISABLES_OUTPUT_ESCAPING on TEXT_CONSTRUCTOR). The request is ignored
  in temporary output state (26.1.8), with 26.2.6's tracking of
  xsl:result-document: lexically inside xsl:variable and the other
  temporary containers the flag is not set; in a template that received
  the temporary-output-state tunnel parameter it is chosen at run time
  (`sxml_xslt_lowering_bind_received_output_state`, shared with
  xsl:result-document). Not covered: templates reached only through an
  initial function call (the writer's state), as for XTDE1480.
- **Where it is honoured**: the XML, XHTML and HTML methods write such
  text in the character context UNESCAPED_TEXT: mapped and normalized,
  not escaped, never in CDATA sections (26.1.8: CDATA "is treated as an
  alternative escaping mechanism"), a character the encoding cannot
  represent as a character reference. Canonical output escapes it; the
  text method has no escaping. Sequence normalization gives such a text
  node as a node, not merged, so it keeps the mark at the top level and
  in xsl:result-document's tree. Attributes, comments, processing
  instructions and namespaces atomize it and lose it.
- **Driver**: feature disabling_output_escaping is supported; the cases
  that need it unsatisfied are not run any more.

### D189. Optional features and the size budgets (packet 7.2)
Recorded 2026-10-19, after D71, D138, D162, D183, D184. Carries out the
owner's size choice (D184): link-time optional features through one
additive `processor.h` view, one build, no `#if`, large data from the
host through the resolver.
- **Public** (`include/sxml/processor.h`, additive):
  `sxml_optional_feature` (a handle to a static const definition, opaque
  to hosts), `sxml_optional_feature_view` as the last field of
  `sxml_processor_configuration` (zero means none, so every existing
  configuration is unchanged; copied at initialize; a NULL definition is
  INVALID_ARGUMENT), `sxml_test_optional_feature()` (named "test", adds
  nothing), `sxml_all_optional_features()` (every feature, for hosts that
  want them all), `sxml_optional_feature_get_name()` and
  `sxml_processor_has_optional_feature()`.
- **Internal** (`src/optional_feature_internal.h`, `src/optional_feature.c`):
  - one `sxml_optional_feature_kind` per feature; a definition holds its
    kind, name, a `static const` operations table (`const void *`, cast
    by kind) and an optional `finalize_state`;
  - the processor keeps one slot per kind (definition, state):
    `sxml_processor_find_optional_feature(processor, kind)` gives the
    table or NULL. NULL is the D162 place's answer: the specification's
    behaviour without the feature stays where it is. No global state; a
    feature never calls another feature or back through its caller (D71);
  - `sxml_processor_set_optional_feature_state` keeps per-processor state
    (tables built from feature data), finalized with the processor.
- **Feature data** (glossary `feature_data`, D184's option 4):
  `sxml_optional_feature_read_data` asks the processor's resolver for an
  absolute URI as a resource kind whose form is BYTES, lends the bytes to
  a reader, and gives them back. It goes through the compile document
  loader (`sxml_processor_compile_document_loader_initialize`), so URI
  resolution and form checks are the existing ones. The packets with
  data add their resource kinds to `processor.h` (DUCET `allkeys.txt`,
  CLDR rule text, TZif), each with its record. The test feature reads
  `urn:sxml:test-feature-data` (TEXT) once and keeps its byte count.
- **Hosts**: the sxml command and the QT4, XSLT and XSD drivers name
  `sxml_all_optional_features()` (one line each). JavaScript: `sxml.wasm`
  (core) and `sxml-full.wasm` (every feature) are two links of the same
  shim, which differ only in `sxml_javascript_core_features.c` and
  `sxml_javascript_full_features.c`. `Sxml.features` lists the module's
  features; `createProcessor({ features })` picks some (default all; an
  unknown name throws). Up to 32 features (one mask word).
- **Budgets** (`bindings/javascript/CMakeLists.txt`): core 2,100,000
  stripped / 735,000 gzipped, full 3,000,000 / 1,050,000 (the owner's
  stripped budgets; the gzipped ones keep D138's 7:20 ratio). CLI
  unchanged (5,500,000 / 1,250,000). `javascript/package_full` runs the
  package tests on the full module.
- **Sizes** at 2459034, before and after (stripped / gzipped):
  `sxml.wasm` 2,003,575 / 697,928 to 2,004,485 / 698,244 (+910 bytes:
  the shim's feature exports 430, the slots and checks 350, export names);
  `sxml-full.wasm` 2,005,513 / 698,677; the CLI 4,966,649 / 1,160,916 to
  4,967,809 / 1,161,287. HEAD was already 3,575 bytes over the old
  2,000,000 budget. The 910 bytes are the mechanism's fixed cost; no
  feature links into the core.
- **Existing parts not moved** (measured in `sxml.wasm` from the linker
  map, code and data): schema validation for fn:xsd-validator and
  friends 70,958; XQuery lowering parts linked through the lowering
  registry (D103) 59,879; XSLT-only library and evaluator parts 15,072;
  Unicode tables 146,058. The first is a candidate feature
  ("schema-aware"); the second and third are not features but registry
  links an XPath-only program does not need: splitting the registries per
  language would save about 75 KB without any API. All are owned by
  running packets (2.21, 4.19), so this packet changes none of them.
- **Later packets** (7.8 to 7.18) each: add a kind before
  `SXML_OPTIONAL_FEATURE_KIND_COUNT`; define the definition, operations
  table and exported `sxml_<name>_feature()` in their own files; add the
  definition to `sxml_all_optional_features`; make their D162 place ask
  `sxml_processor_find_optional_feature`; add a resource kind for any
  data and read it with `sxml_optional_feature_read_data`; extend the
  CLI's resolver to serve that data (for example `/usr/share/zoneinfo`)
  and the JavaScript resolver to forward the kind; check the full
  module's budget. A feature that gives access beyond the resolver
  (EXPath File, D184) stays out of `sxml_all_optional_features` and is
  named by the CLI only under `--allow-file-access`. The fuzz fixtures do
  not name features yet: the packet that adds a parser names its feature
  there.

### D190. XPath and XQuery conformance sweep (packet 2.21)
Recorded 2026-10-19, after D143, D158, D174, D113. Takes the "XPath and
XQuery" leftovers of `docs/agents/QUEUE.md`. No public header changed; no
IR expression kind added.
- **Composite casts** (glossary `composite_cast`; XPath 4.0, 4.19.2.4 to
  4.19.2.7): `cast as` and `castable as` an array, map or record type
  run the coercion frames with the cast rules
  (`sxml_coercion_request.applies_cast_rules`): an item that matches its
  item type stays; a generalized atomic, list or union type casts the
  atomized item with the static namespaces; arrays, maps and records
  rebuild their parts; a record cast makes a record with every field and
  drops other entries; duplicate keys are XQDY0137; anything else is
  XPTY0004. castable answers false for dynamic and type errors of the
  cast and of atomizing the operand (4.19.3). Lowering: XPST0080 for a
  choice that is not generalized atomic and for function or node types,
  XQST0052 in XQuery for a non-simple named type.
- **The caller's focus** (glossary `caller_focus`): a native row marked
  `SXML_NATIVE_FUNCTION_TAKES_CALLER_FOCUS` (only fn:function-lookup)
  gets the static call's context value, position and size as three
  extra arguments, dropped at run time when the context value is absent;
  its arities do not count them. fn:function-lookup binds a row that
  needs the focus: fn:position#0 and fn:last#0 return the captured part,
  a context-value default is the next arity partially applied (name
  kept). A dynamic call of fn:function-lookup passes no focus.
- **Paths over JNodes**: `SXML_IR_MAP_IS_AXIS_STEP` marks the item
  axis's MAP as navigational for the selector analysis (4.7.3); "/" is
  `fn:root(.) treat as (document-node() | jnode())` (4.7.1.1); XPTY0019
  and XPTY0020 are XPTY0004 (PR 2627); preceding:: of a JTree root is
  empty.
- **Contextual equality** of dates and times without timezone uses the
  implicit timezone in fn:distinct-values and fn:duplicate-values
  (`sxml_function_library_adjust_to_implicit_timezone`, shared with
  group by).
- **fn:doc's xsd-validation** validates the loaded document as
  fn:parse-xml does (D174); FODC0009 stays for fn:parse-xml-fragment.
- **QT4 driver**: assert-xml trims whitespace around inline expected XML
  that starts and ends with markup, as it did for files.
- **Measured** (release, full XP40 and XQ40 runs; before at 7d70d01):
  XP40 30,931 to 31,098 passed, wrong error 85 to 44; XQ40 41,273 to
  41,456, wrong error 123 to 70; none lost, 0 errors. XT40 at the same
  HEADs no set lost. sxml.wasm grew about 5.8 KB (inside D189's core
  budget).
- **Not done** (for later sweeps): (d) fn:serialize's canonical JSON
  cases call EXPath Binary's bin:unpack-double (27 cases, 7.8's module),
  serialize-xml-canonical-5 needs an external entity (7.14); (e) the
  built-in schema of the fn namespace (F&O 4.0, appendix D): an import
  of the namespace the resolver does not deliver could fall back to it
  as xml.xsd does in `src/schema_assemble.c`, but json-to-xml's
  validate, typed analyze-string results and their schema-element()
  tests also need it compiled once per processor and a validation
  against a given schema (about 25 cases); (f) the record model of the
  current draft (records annotated, record(*) only for records, a
  record matching only with all its fields, lookups of other keys
  XPTY0004: cast-record-901, recordTypeDecl-019..021, -072, -089);
  NamedRecordTypeDecl's module cases name `recordTypeDecl/` where the
  suite has `RecordTypeDecl/` (a case-sensitive file system finds
  none); the item axis's JNode for a singleton is a child of a new array
  (D113); NCName and Q{}N name tests are one in sxml_name_test (frozen);
  a declared function's "$a := ." found by fn:function-lookup; lax
  validation without any in-scope schema raises FODC0014
  (`src/function_library_schema.c`); use-xsi-schema-location.

### D195. XML 1.1 input (packet 7.13)
Recorded 2026-10-19, after D12, D81, D183, D184. Supersedes D81's "version
1.1 documents are parsed by the 1.0 rules".
- **Version**: read from the XML declaration of the decoded text before
  parsing (`sxml_xml_read_declared_version`, the pseudo-attribute reader
  that already finds the encoding name); `1.1` is XML 1.1, every other
  version, `1.x` included, and no declaration are XML 1.0 (XML 1.1,
  4.3.4; XML 1.0, 2.8).
- **A pre-pass, not scanner state** (deviation from 7.13's mechanism):
  for a 1.1 document the parser refuses a [2a] RestrictedChar written
  literally (XML 1.1, [1] document) and translates NEL, CR NEL and LINE
  SEPARATOR after the declaration into LF in its copy of the text (2.11;
  CR LINE SEPARATOR stays two line ends). Translation must come before
  tokenizing, because NEL is whitespace in 1.1 markup (a NEL between
  attributes), which the grammar would otherwise have to know
  per version in every whitespace and text token. The grammar stays the
  1.0 grammar for 7.14; NEL and LINE SEPARATOR in the declaration stay
  and are an ERROR, as 2.11 requires. Cost: a copy only for 1.1
  documents with such line ends; diagnostic positions after them are
  positions in the translated text (the byte offset can be off by one
  or two per translated character in UTF-8 input).
- **Characters and names**: character references may denote the C0
  controls in 1.1 (4.1, `sxml_xml_codepoint_is_version_1_1_char`);
  U+0000 never. Names need nothing: XML 1.0 Fifth Edition has 1.1's name
  characters. `xmlns:p=""` undeclares p in 1.1 (D81, unchanged).
- **Serialization**: version 1.1 already wrote the C0 and C1 controls as
  references and undeclare-prefixes (packets 3.3, 3.4); unchanged.
- **Drivers**: QT4 claims `xml-version 1.1` (sxml reads both versions);
  XT40 supports `XML_1.1` and xml-version="1.1" sources, and keeps it
  among D95's non-goals so the gate rates keep their meaning;
  assert-xml with xml-version="1.1" writes the result with version 1.1
  and parses both texts as 1.1 documents (`tests/qt4/driver/assertion.c`,
  shared). XSD: the suite names no XML-1.1 test; the XmlVersions set's
  1.1 documents now parse.
- **Query texts stay XML 1.0** (not this packet; Proposed below):
  XQuery's xgc: xml-version leaves the version of query lexing
  implementation-defined; sxml's is 1.0 (XQST0090 for `&#1;`, XQST0085
  for `xmlns:p=""` in a direct constructor, no NEL line ends, version
  1.0 as the default serialization version). QT4's 1.1 cases that need
  1.1 query lexing fail honestly; the 1.0 cases that need 1.0 lexing
  keep passing (the two sets conflict case by case).
- **Measured** (release drivers, before at 2459034 / after):
  - xmlconf XML 1.1 (`tests/xml_conformance_test.c` now selects the
    ibm and eduni xml-1.1 catalogs, eduni's NS 1.1 tests and the 1.1
    erratum test, Fifth Edition rules for EDITION; 271 cases): 202 to
    242 passed (valid 84/84, invalid 13/13, not-wf 140/169, error 5).
    All 29 failures are not-wf cases whose fault lies in an external
    entity or DTD, which sxml does not read (D81): 7.14's. Of the 242
    cases that need none, all pass. The 1.0 selection is unchanged
    (601/175/948).
  - QT4 (the 15 test sets with xml-version dependencies): XQ40 2,497 to
    2,798 passed, XP40 1,944 to 2,235; fn-graphemes 918 to 1,189 in
    both. Of XQ40's 313 1.1 cases 300 pass (95.8 %); the 13 that fail
    need 1.1 query lexing (above). No case that passed fails.
  - XT40: xml-version 0 to 40 of 42, namespace 206 to 221, catalog 5
    to 7, xml-to-json 112 to 115, no case lost. Left: xml-version-007
    (a value template), -016 and namespace-2606, -6202 (namespace
    undeclaration in XSLT, `src/xslt*`).
  - XSD saxon-XmlVersions 48 to 51 of 51.

### D191. Optional features add library functions (packet 7.10a)
Recorded 2026-10-19, after D45, D101, D162, D189. A feature whose D162
place is "the functions are absent" (XPST0017: fn:parse-html and
fn:html-doc, the EXPath Binary and File functions) must not link its
rows into a program that does not name it, so its rows cannot be a
library part of `src/function_library_*.c` (the registry links every
part).
- `sxml_optional_feature_definition` (src/optional_feature_internal.h)
  gains `library_functions`: a `const sxml_native_function_view *` of
  static const rows in the feature's own files, or NULL.
- The processor's function library serves every registered XPath part,
  then the `library_functions` of each configured feature in kind order
  (`sxml_processor_make_library_part_list` in src/processor.c). The
  list lives in allocator memory only while the library is initialized
  (the library does not keep it). XQuery and XSLT chain their own tables
  in front of the processor's, so they see the feature's functions too.
- Without the feature the functions are absent, as before: XPST0017 at
  compile time, false from fn:function-available, nothing from
  fn:function-lookup. A feature's file that holds rows names its view
  differently from `sxml_function_library_<part>`, so the CMake registry
  (cmake/SxmlParts.cmake) does not take it as a part.

### D199. DTD processing: external subset, external entities, validation (packets 7.14a, 7.14b)
Recorded 2026-10-19, after D12, D28, D44, D71, D81, D165, D183, D184, D195.
Replaces D81's "the external subset is not read" and "external parsed
entities are skipped in content".
- **Header, additive** (`include/sxml/xml.h`): an entity resolver
  (`sxml_xml_entity_resolver`: resolve and release over
  `sxml_xml_external_entity_request` → `sxml_xml_external_entity`), named
  per parse in `sxml_xml_parse_request.entity_resolver`. The null resolver
  (a zeroed request) reads nothing outside the input, as before. The
  request carries the purpose (external subset, parameter entity, general
  entity), name, system and public identifiers and the system identifier
  resolved against the base URI of the entity holding the declaration
  (4.2.2). `xml.h` cannot name `processor.h`'s resource resolver
  (processor.h includes xml.h), so the parser has its own small interface
  and the processor adapts its resolver to it.
- **Grammar** (D12, `grammars/xml`): the start rule also reads the
  external subset and external parsed entities, each after a marker the
  builder writes (U+0001 and a letter: no [2] Char, so a document never
  takes these alternatives, and the builder refuses one without a root).
  New: `external_subset` ([30], [31]), `external_parsed_entity` ([78]),
  `text_declaration` ([77]), `include_section` ([62]),
  `conditional_section` ([63], or a parameter-entity keyword),
  `ignored_section_contents` ([64], [65], scanner token `ignored_text`),
  entity values with parameter-entity references in external DTD text.
  The internal subset stays strict (WFC: PEs in Internal Subset in the
  grammar).
- **What the grammar cannot express is a loop with explicit stacks**
  (D71), as the task allowed: a declaration with a parameter-entity
  reference outside its literals matches its production only after
  expansion, so the scanner returns it as one `unexpanded_declaration`
  token; the builder replaces the references (4.4.8, one space on each
  side, the replacement texts' own references too, with quote state and an
  explicit stack of open entities in the parser's expansion array) and
  parses the result again as the external subset in a frame of its own.
  The same loop includes a reference in an entity value in the literal
  (4.4.5: character and parameter-entity references recognized again,
  general ones kept). A conditional section whose keyword is a reference
  is read as ignored text by the grammar; the builder reads keyword, '['
  and contents from the replacement text and the rest of the section (the
  '[' may come from the entity, a validity matter only) and parses an
  included section again. A missing '>' that comes from an entity
  (Proper Declaration/PE Nesting) ends the token at the next markup.
- **Reading**: the external subset after the internal subset (2.8),
  through a parameter entity named "[dtd]" (no Name, so unreachable);
  external parameter entities where referenced; external parsed entities
  in content (parsed after the entity marker, the tree cached per parse
  as an internal entity's is, frames of a new kind whose walk root is the
  entity). Each external entity is asked once per parse, decoded as a
  document is (Appendix F; its text declaration must agree with the
  bytes), its byte order mark dropped, U+0000 refused, line ends
  normalized in the copy (2.11); a 1.1 document's entities follow 1.1
  (restricted characters, NEL), and an entity of a 1.0 document must not
  declare 1.1 (XML 1.1, 4.3.4). An entity the resolver does not deliver is
  not read (4.4.3, 5.1: later entity and attribute-list declarations are
  skipped as for an unread parameter entity). Every external text counts
  against `maximum_entity_expansion_byte_count` at each inclusion.
- **WFC: Entity Declared** where it applies (no DTD, internal subset
  without references, standalone="yes") also refuses an entity declared
  in the external subset or in a parameter entity (4.1).
- **Measured** (`tests/xml_conformance_test.c`, now parsing every case
  with a file resolver; the 1.0 selection with ENTITIES other than "none"
  is a third group): standalone 1.0 unchanged 601/175/948; external 1.0
  0 run before, now valid 127/127, invalid 54/54 (well-formed), not-wf
  66/66, error 18; XML 1.1 not-wf 140/169 to 169/169 (D195's 29). The
  remaining parts (the processor's resource kind, trust-external, the
  drivers, the CLI, DTD validation) follow in this record.
- **Validation (7.14b) is schema validation** (no second content-model
  engine): `src/dtd.c` writes the DTD as XSD documents, one per
  namespace (names resolve through the root element's xmlns attributes
  and the DTD's xmlns defaults; content models become model groups,
  attribute types simple types: ID, IDREF(S), ENTITY(IES), NMTOKEN(S),
  NOTATION, enumerations; an element referred to but not declared gets a
  type that refuses it). `src/schema_from_dtd.c` compiles them and runs a
  schema validator without a result tree; the parser sends its events to
  it as well (a tee). The constraints about declarations themselves
  (Root Element Type, Standalone Document Declaration, the PE nesting
  ones, Unique Element Type Declaration, Unique Notation Name, One ID
  per Element Type, ID Attribute Default, Notation Declared, No
  Duplicate Types and Tokens, Entity Declared, Fixed Attribute Default)
  the parser checks and reports through the hook, code xml-validity. **Public, additive**: `xml.h`'s `sxml_xml_validator` hook
  (`sxml_xml_parse_request.validator`); `schema.h`'s
  `sxml_dtd_validator` (memory_requirements, initialize, finalize,
  create, destroy, as_xml_validator, get_outcome); `processor.h`'s
  resource kind `SXML_RESOURCE_KIND_EXTERNAL_ENTITY` (form BYTES) and
  `sxml_processor_entity_resolver`, which adapts the processor's resource
  resolver for a parse.
- **Tree**: element content whitespace (a declared element-only content
  model) makes no text node (XDM 4.0, 7.5.7.3); unparsed entities keep
  absolute system identifiers, so fn:unparsed-entity-uri and
  fn:unparsed-entity-public-id work (replaces D81's "unparsed entities
  are not reported").
- **fn:doc and fn:parse-xml** (F&O 4.0, 17.1.1 and 17.2.1): the loader
  parses with an entity resolver over the evaluation's resource resolver.
  trust-external defaults to false, so a document that refers to an
  external DTD or entity raises FODC0016 (the 4.0 incompatibility the
  specification names); dtd-validation=true validates and raises FODC0007
  (also for a document without a DTD, 2.8). FODC0013 stays for XInclude.
  XSLT's fn:document and stylesheet modules read external entities
  through the resolver. The CLI parses its input files with the null
  resolver, so it never reads an external entity (D44); a query reads one
  only through trust-external=true.
- **Measured**: DTD validation of xmlconf: standalone valid 594 of 594
  valid, invalid 173 of 173 invalid; external valid 127 of 127, invalid
  54 of 54. XT40 8,861 to 8,931 passed (release, whole suite, none lost);
  dtd cases 68 of 79 (86 %, under the 90 % gate): base-uri-051/051a/051b
  need an element base URI from the entity (tree.h; Proposed),
  unparsed-entity-05 to 08 need unparsed entities kept by xsl:copy-of
  (Proposed), error-1370a, 1380a and 1380b raise the wrong code without a
  focus, id-043 needs a parse-xml source in the driver. QT4 (release):
  fn-doc XQ40 69 to 73, XP40 63 to 67; fn-parse-xml XQ40 46 to 53, XP40
  35 to 42; none lost. Left for the QT4 driver (its files were others'
  work in progress): claiming infoset-dtd and dtd-validation-option,
  environment sources read with the entity resolver, and
  supports-dtd-validation true in `src/system_property.c` with the
  feature claim. Fuzzing: `tests/fuzz/dtd_fuzz.c`, 20 minutes, clean
  after one fix.

### D198. EXPath Binary (packet 7.8)
Recorded 2026-10-19, after D51, D99, D162, D163, D189, D191. The EXPath
Binary Module 4.0 is the optional feature `sxml_expath_binary_feature()`
("expath-binary").
- **Public, additive**: `include/sxml/processor.h` declares the feature
  function. `include/sxml/name.h` (frozen by D99) gains the well-known
  namespace `SXML_NAMESPACE_EXPATH_BINARY` = 13
  (http://expath.org/ns/binary), so rows name their namespace as the fn,
  map and math rows do. `SXML_NAMESPACE_WELL_KNOWN_COUNT` is now 14:
  interned URIs start one later, which no caller sees.
- **Rows**: 33 functions in `src/function_library_binary.c`, whose view is
  not named as a part, so they are the definition's `library_functions`
  (D191). The kind's operations table is that same view. Every binary
  parameter is `(xs:hexBinary | xs:base64Binary)`; results are
  xs:base64Binary (1.2). The 4.0 types are kept: `xs:unsignedByte` for
  octets and the `enum(...)` octet order, so bad values are XPTY0004 as
  4.0 says (appendix F), where the QT4 cases still expect
  bin:octet-out-of-range and bin:unknown-significance-order (26 wrong
  error codes, which count as passes).
- **Errors** are in the bin namespace, prefix bin, filled into the error
  record directly (static text, D21).
- **Text**: the decoder of fn:unparsed-text moves to `src/text_decoding.c`
  (`sxml_text_decoding_choose`, `sxml_text_decode`). Its encoding choice
  was already written after bin:infer-encoding, so bin:infer-encoding and
  bin:decode-string use it unchanged. bin:encode-string encodes UTF-8,
  UTF-16 (big-endian, no mark), UTF-16BE/LE, ISO-8859-1 and US-ASCII.
  Encoding names are classified by `sxml_xml_classify_encoding_name`.
- **Integers** beyond int64_t are packed and unpacked through their
  decimal text on base 10^9 limbs. bin:integer-too-large is raised past
  4,096 octets (9,865 digits, inside the heap's decimal limit).
- **System property**: the value kind DYNAMIC_XQUERY becomes
  KNOWN_FUNCTION, with a function name and arity in the row.
  supports-dynamic-xquery asks for fn:load-xquery-module#1 and
  supports-binary-library for bin:hex#1, so the property follows the
  configuration.
- **Hosts**: the feature is in `sxml_all_optional_features()`, so the sxml
  command, the drivers and `sxml-full.wasm` have it, and `sxml.wasm` does
  not. The QT4 driver supports the dependency feature binary, and bin-*
  is no longer a non-goal set.
- **Counts** (release driver, XP40 and XQ40 alike): bin-* 0 to 646 of
  646, of which 26 have the wrong error code; fn-serialize 200 to 224
  (XP40) and 258 to 282 (XQ40), the canonical-JSON cases that call
  bin:unpack-double; fn-system-properties 23 to 23 (the -false case is
  now not run, the true case passes).

### D196. xsl:evaluate (packet 7.3)
Recorded 2026-10-19, after D71, D154, D162, D163, D183, D184.
Replaces D162's one place for dynamic evaluation (the XTDE3175 raise in
`src/xslt_lower_control.c`): xsl:evaluate is available, so XTDE3175 is
never raised and the xsl:fallback children are not evaluated.
- **One mechanism** (D183): the instruction is a STATIC_CALL of an
  internal row (`src/xslt_evaluate.c`) whose resumable body compiles the
  target expression with `sxml_xpath_compile` and asks the machine for a
  unit evaluation (EVALUATE_UNIT, D154), as fn:load-xquery-module does;
  the second step gives its value. A nested xsl:evaluate (in a
  stylesheet function the target calls) is one more unit evaluation on
  the same machine: no C recursion (D71).
- **Static context** (10.5.1), read lexically from the call's module
  (the xsl:evaluate element's, D163): its static namespaces (prefix 0 is
  its xpath-default-namespace), or the in-scope namespaces of the
  namespace-context node (XTTE3170 unless one node; xml and
  undeclarations dropped); base-uri resolved against the instruction's
  base URI; its collations, default collation and decimal formats; its
  in-scope schemas only with schema-aware="yes"; the parameters as
  external variables of type item()*, xsl:with-param (lowered as for
  xsl:call-template) under with-params (XTTE3165 unless one map with
  xs:QName keys). trusted="no", the default, makes the unit untrusted
  (`sxml_ir_unit_set_untrusted`, FODC0016 for resources). schema-aware
  and trusted: XTSE0020 for a fixed non-boolean word, XTDE0030 for a
  computed one.
- **Functions** (10.5.1): the F&O library, then the stylesheet functions
  whose visibility is public or final, then fn:current (XSLT 4.0 allows
  it: the current item is the target's context item); no other
  XSLT-defined function. The stylesheet functions belong to the
  stylesheet's unit, so the lowering passes them as function items (a
  map from name to the references of its arities, bound through the
  instruction's function table), and the target's function table binds
  them with a new function entry kind, FUNCTION_ITEM
  (`src/function_library_internal.h`): a call lowers to a DYNAMIC_CALL
  of the item as a constant, a named reference to the item. Keyword
  arguments to them are not supported (the entry has no parameter
  names).
- **Dynamic context** (10.5.2): the context-item value is the context
  item; an empty one, or none, is no context (XPDY0002, as evaluate-025
  expects); more than one item is XTTE3210, XSLT 3.0's rule, which
  evaluate-026 and error-3210a expect (a deviation from the 4.0 draft,
  which makes the value the context value). Dynamic errors keep their codes; a static error
  of the target is XTDE3160 with the static error's description.
- **Counts** (release driver, whole XT40 against 9856281, case by case):
  8,899 to 8,973 passed, 0 errors, no case lost; evaluate 73 of 83 (9 of
  them with another error code), 91 % of the 80 run, 88 % of the 83;
  error-3175a and system-property-014b need the feature absent and are
  not run any more. The 90 % gate of 7.3 is not met yet: 3 cases wait for
  html-doc and parse-html (7.10), 2 for trust checks in json-doc and
  unparsed-text-available, 1 needs a UCA collation strength (7.15), 1
  (evaluate-008a) expects XPST0017 for a call use-when removes.
- **Not done**: keyword arguments to stylesheet functions; json-doc and
  unparsed-text-available do not check trust yet (evaluate-454/455,
  `src/function_library_resource.c` was another packet's file); html-doc
  and parse-html wait for 7.10.

### D201. HTML5 parsing: a hand-written tokenizer, tree construction on a working tree (packet 7.10a)
Recorded 2026-10-19, after D12, D53, D71, D162, D183, D184, D189, D191.
fn:parse-html and fn:html-doc (F&O 4.0, 17.3) as the optional feature
`sxml_html_parser_feature()` (name "html"), after the HTML Living
Standard of 16 October 2026, 13.2.
- **Not tree-sitter** (an exception to D12, for the reasons of D53):
  (1) the tokenizer is a state machine that tree construction switches
  (RCDATA, RAWTEXT, script data and PLAINTEXT after a start tag, CDATA
  sections only when the adjusted current node is foreign), so lexing
  depends on parse state a context-free grammar does not see; (2) HTML
  has no syntax errors: every input parses, by error recovery the
  standard defines exactly, whereas D12 makes any `ERROR` node a static
  error and tree-sitter's own recovery is not the standard's; (3) tree
  construction moves nodes already built (adoption agency, foster
  parenting), so a walk over a concrete syntax tree would be a second
  tree builder, not a replacement for one; (4) the input is run-time
  data (`parse-html($computed)`), and tree-sitter allocates with `malloc`
  outside every allocator and limit, which D12 accepts only for source
  text.
- **Files**: `src/html_tokenizer.c` (the 84 states of 13.2.5 as one loop
  over a state variable; tree construction pulls tokens, characters come
  in runs); `src/html_tree_construction.c` (the 21 insertion modes and
  the rules for foreign content as a dispatch loop of steps, so "process
  the token using the rules for" is no call cycle, D71); `src/html.c`
  (the walk that sends the working tree to an arena tree, the XDM
  mapping, the feature); `src/html_encoding.c` and the generated
  `src/html_encoding_tables.c` (`tools/generate-html-encodings.py`);
  `src/function_library_html.c` (the two rows, D191's library functions,
  not a registered part); `src/html_internal.h`.
- **Working tree** (glossary `html_working_tree`): node, attribute and
  text-segment tables in allocator memory; a text node is a chain of
  segments so that later characters join it; the walk to the tree
  builder follows parent links (no stack needed). The named character
  references are fn:char's table (`src/unicode_tables.c`) plus the 106
  names that also match without ";": no second entity table.
- **The standard as of now**: processing instructions (13.2.5.72 to .76:
  `<?target data?>` is a PI, `<?xml ...?>` a comment), no "in select"
  insertion modes, `search` special, `select` a scope boundary. F&O 4.0's
  note that the HTML parser makes no PIs predates that change.
- **Not done, by the standard's own switches**: scripting is Disabled
  (`noscript` content is parsed; 13.2.4.5), no fragment case, no
  declarative shadow roots, no sanitizer, no content patching (template
  `for`); `selectedcontent` is filled by DOM steps of the select
  element, not by the parser, so it stays empty.
- **XDM mapping** (F&O 4.0, 17.3.1; HTML, 13.2.9): XHTML, SVG and
  MathML namespaces; SVG and MathML names in their case; xlink: and xml:
  attributes in their namespaces; xmlns attributes dropped; names XML
  does not allow escaped as "U" and six hex digits; comments get a space
  inside "--" and after a final "-"; U+000C becomes a space and other
  characters XML does not allow U+FFFD; an `id` that is an NCName is an
  ID; a template's contents are its children. Base URI: the static base
  URI (fn:parse-html), the resource's URI (fn:html-doc, also its
  document URI).
- **Input** (13.2.3): a string is UTF-8 with a known encoding, a leading
  U+FEFF stripped (F&O 4.0); octets: byte order mark, then the encoding
  option (every label of the Encoding Standard; its 28 single-byte
  encodings, UTF-8, UTF-16 and x-user-defined decode, a multi-byte or
  unknown label is FODC0011), then step 9's autodetection: well-formed
  UTF-8, else windows-1252 (step 10). No `<meta>` prescan yet (7.10b).
- **fail-on-error**: parsing stops at the first parse error (13.2.2
  allows it), a decoding error included: FODC0011. Unknown options and
  html-version, method or include-template-content are XPTY0004.
- **Counts** (release driver): fn-parse-html 0 to 22 of 22 (XQ40 and
  XP40), fn-html-doc 0 to 9 of 10 (html-doc-010 expects FODC0011 for
  `<a:tag>`, which no rule of 13.2 makes a parse error),
  misc-HtmlTestSuite 0 to 1,378 of 1,379 (XQ40; html-test-1281 expects
  jsoup's comment for `<?processing data?>`), misc-BuiltInKeywords +2
  (the result type `document-node(*:html)?`). The sets are no non-goals
  of the QT4 driver any more. Outside the suite, the html5lib tests of
  web-platform-tests run in a scratch harness: tree construction 1,747
  of 1,753 (`#script-on` and fragment cases left out; the others are
  `selectedcontent` and harness limits), tokenizer 7,022 of 7,049 (the
  rest harness limits and F&O's U+FEFF rule).
- **Sizes** (stripped / gzipped): `sxml.wasm` 2,078,210 / 722,803 before
  and after (no HTML code links into the core); `sxml-full.wasm`
  2,103,528 / 733,247 to 2,182,635 / 762,413 (+79,107; budget
  3,000,000); the `sxml` CLI module 5,848,173 / 1,239,159 to
  5,927,297 / 1,267,818. The CLI module was over its 5,500,000-byte
  budget already (Proposed below).
- **Tested**: `tests/html_test.c` (tokens, trees, the input stream, both
  functions with and without the feature, allocation-failure loops),
  `tests/fuzz/html_parse_fuzz.c` (11,000 runs seeded from the suite,
  no finding), two `sxml xpath` end-to-end tests.
- **Left for 7.10b**: the `<meta>` prescan of 13.2.3.2 (step 5) and
  13.2.3.4 (changing the encoding while parsing) for octets; vendoring the
  html5lib .dat and tokenizer tests (web-platform-tests
  `html/syntax/parsing/resources`, html5lib-tests `tokenizer`) as a unit
  test with the scratch harness's format; the multi-byte decoders
  (Shift_JIS, EUC-JP, ISO-2022-JP, GBK, gb18030, Big5, EUC-KR) or a
  decision that they stay FODC0011; source locations for dm:location;
  XHTML (the XML syntax) recognition, which F&O 4.0 asks for with
  "should"; the CLI's relative URIs in `sxml xpath` and `sxml xquery`
  (no static base URI, so fn:html-doc and fn:unparsed-text of a relative
  name fail there); html-doc-010 and html-test-1281 stay failing unless
  the suite changes.

### D202. The CLI's WebAssembly budget is 7,000,000 / 1,500,000 bytes (owner)
Recorded 2026-10-19 on the owner's choice. The CLI module grew to 5,927,297
bytes stripped and 1,267,818 gzipped (the XQuery Update grammar, D187, and
the HTML parser, D201), over D176's 5,500,000 / 1,250,000. The CLI's
WebAssembly build runs sxml in Node, where size matters less than for
`sxml.wasm`, so its budget rises to 7,000,000 / 1,500,000 with room for the
rest of Phase 7. The `sxml.wasm` budgets of D184 and D189 stay.

### D197. XSLT backwards compatible behaviour and XPath 1.0 compatibility mode (packet 7.4)
Recorded 2026-10-19, after D97, D162, D183, D184. Replaces D162's place
for XSLT 1.0 behaviour (`asks_xslt_1_behaviour` and its XTDE0160 in
`src/xslt_lower.c`).
- **One flag, no second evaluator**: XPath 1.0 compatibility mode is
  `has_xpath_1_compatibility_mode` of the XPath static context
  (`src/xpath_static_internal.h`) and, additively, the last field of
  `sxml_static_context_configuration` (`xpath.h`; XPath hosts only,
  XQuery has no such mode and ignores it). The lowering reads it and
  inserts the 1.0 conversions as STATIC_CALLs of internal native rows in
  no function table (`src/xpath_compatibility.c`, golden file
  `src/xslt_simple_content.c`); the evaluator does not know the mode.
  - Arguments of static calls to built-in functions (XPath 4.0, 3.4.1):
    the first item; then fn:string for xs:string and xs:string?, fn:number
    for xs:double and xs:double? (the optional forms keep ()); fn:concat
    takes xs:string($v[1]) per argument (F&O 4.0). Not for partial
    applications, dynamic calls, stylesheet functions (XSLT 4.0, 10.3.2)
    or the lowering's own calls.
  - Arithmetic and unary operands (4.9): atomized, the first item, () as
    NaN, xs:boolean, xs:string, xs:decimal, xs:float and xs:untypedAtomic
    by fn:number. The operands of "to" keep their first items (4.8.2
    coerces them).
  - General comparisons (4.11.2): one row does the mode's rules: the
    xs:boolean rule (EBV of the other operand), fn:number of every item
    for <, <=, >, >=; for = and != a pair with a numeric item compares as
    doubles, other pairs by the mode-false rules. It calls the existing
    `sxml_operator_make_general_comparison` on the converted parts.
- **XSLT**: the static context of every element whose effective version
  is below 2.0 has the mode (5.3.1, 3.8.1; 2.0 and 3.0 behaviour is
  4.0's). The 1.0 rules of the instructions, each where the instruction
  is lowered, through `sxml_xslt_lowering_has_xslt_1_behaviour` and
  `sxml_xslt_lowering_bind_first_atom` (fn:head(fn:data(.))):
  xsl:value-of with select and no separator (11.4.2), attribute value
  templates (5.6.1; not text value templates), xsl:number value (12.2:
  first item, () as NaN, an unconvertible value formatted as "NaN"
  between the format's prefix and suffix), xsl:sort keys (13.1.2, first
  item after data-type), keys in backwards compatible mode (20.2.2:
  use values and fn:key's argument cast to xs:string), and an unknown
  extension function bound to a fallback error function, XTDE1425 when
  called (3.8.1, 24.1.2; function-available stays false).
  supports-backwards-compatibility is yes.
- **xs:numeric is not xs:double** (XPath 4.0, 3.4.1 names xs:double only;
  F&O 4.0's changes list says strings are no longer accepted where the
  type became xs:numeric): fn:format-number, fn:round, fn:floor and
  fn:abs do not convert strings or () by fn:number in the mode. The
  suites expect the older conversion: XT40 format-number 014, 035,
  038-041, backwards-022, version-021, xpath-compat-0302 and 0401, and
  QT4 numberformat15, 38, 41, 42 fail. Not reclassified (D178).
- **Not done**: the expressions of a text value template in a 1.0
  element are still lowered with the mode (3.8.1 asks for it off; only
  expand-text="yes" on a 1.0 element meets it); the default output
  method xml for a 1.0 stylesheet is also used for an explicit
  xsl:result-document (backwards-019b); exsl:node-set, which DocBook's
  1.0 stylesheets call (docbook-001 raises XTDE1425; it needs a
  function namespace sxml_well_known_namespace lacks); docbook-002 meets
  an XPST0003 in an included module.
- **Drivers**: XT40 supports backwards_compatibility and keeps it among
  D95's non-goals, as XML_1.1 (D195); QT4 supports xpath-1.0-compatibility
  for XPath only and sets the mode for the test cases that ask for it
  (assertions stay mode false), also kept out of the denominator. The
  three XT40 cases that expect XTDE0160 (and three more asking for the
  feature unsatisfied) are not run now.
- **Numbers** (release, XT40 whole, case by case): 8,687 to 8,787
  passed at 3dc364a; 111 of the 124 cases (89.5 %, gate 80 %); QT4
  xpath-1.0-compatibility 1 of 5 (generate-id-018; the other four are
  the xs:numeric cases above). Unit test `tests/xslt_compatibility_test.c`.

### D205. fn:put, revalidation, untyped copies and the 4.0 return clause (packets 7.7b, 7.7c)
Recorded 2026-10-19, after D99, D165, D173, D184, D187. The rest of the
XQuery Update Facility after 7.7a: fn:put and revalidation (7.7c), the
4.0 draft's updating return clause (7.7b), and copies that drop schema
identity.
- **fn:put** (XQUF 3.0, 6.1; `src/xquery_update.c`): a row of XQuery's
  function table (`src/xquery_load_module.c`, beside
  fn:load-xquery-module), `fn:put($node, $uri, $params := ())`, whose call
  the static rules treat as updating (XUST0001 as for any update). Its
  value is one update primitive `upd:put`, the node as target and the URI
  as parameter: FOUP0001 for a node that is not a document or element
  (6.1 leaves other kinds implementation-defined), FOUP0002 for a $uri
  with a character no URI or IRI reference has or that does not resolve
  against the static base URI to an absolute URI. Application checks
  XUDY0031 (two puts, one URI) and XUDY0037 (a put in a copy-modify's
  modify clause) and changes no tree.
- **The host** (`include/sxml/xquery.h`, additive): the update sink gets
  `receive_put_document`, called after every changed tree (3.2.2: upd:put
  last) with `sxml_xquery_put_document` (the absolute URI, the node as the
  other updates left it, the $params). The library never writes it
  (D184); NULL makes a query that calls fn:put fail with FOUP0002 before
  any tree is handed over. The node of a put on the root of a changed
  tree is the new root; a put on a node inside a changed tree gets the
  node as it was (node identity across a rebuild is not kept, D187).
- **Revalidation** (3.1, 3.2.4 upd:revalidate): XUST0026 is gone; lax
  and strict are supported. Each rebuilt tree that a type-changing
  primitive marked (upd:removeType reaches its root) is validated as
  "validate lax|strict { $top }" does, with sxml_schema_validate_node
  (D173) and the in-scope schemas (the schema declaring the root element;
  for lax the first without one; XQDY0061, XQDY0084, XQDY0027), and its
  validated copy is the updated tree. The mode is the main module's for
  the query and the module's for copy-modify (modify-copies takes it as a
  third argument). sxml's initial revalidation mode stays skip: the
  static context table of 3.1 lets an implementation overwrite lax.
- **Copies** (5.6 step 1.2; XQuery 3.1, 3.9.1.3): a copy-modify copy is
  untyped whatever the construction mode, and copies made untyped drop
  is-id and is-idrefs except for xml:id attributes. `tree.h` (frozen,
  D99) gets one additive field, `sxml_copy_request.strips_identity`;
  false, the old behaviour, is what XSLT needs (validation="strip" and
  input-type-annotations="strip" keep is-id, copy-5034 and
  strip-type-annotations-021). The update application sets it for its
  copies. XQuery's own node constructors under construction strip still
  keep is-id (`src/evaluator_construct.c` has no language flag): left.
- **The updating return clause** (XQUF 4.0 draft, copy modify): a
  copy-modify whose return clause is updating is updating, its pending
  update list the return clause's; 3.0's XUST0001 there is gone.
- **QT4 driver**: revalidation lax and strict are supported; what
  fn:put stores is serialized, parsed again and added to the test case's
  available documents at its URI (no file is written), and assertions
  see the available documents.
- **Left**: XUDY0038 for a simple dynamic call of an updating function
  item (no QT4 test; the one place is the evaluator's dynamic call); two
  cases where validating an element root checks IDREFs against its IDs
  (cvc-id.1) and the tests expect no such check
  (removeType-valskip-repnode-006, revalidate-valtrans-ins-002); six
  propagateNamespace cases whose expected XML has other whitespace than
  their queries make under boundary-space preserve (the suite's error:
  assert-xml compares canonical forms, kept strict); 27 staticTyping.

### D200. EXPath File (packet 7.9)
Recorded 2026-10-19, after D71, D99, D162, D184, D189, D191, D198. The
EXPath File Module 4.0 is the optional feature `sxml_expath_file_feature()`
("expath-file") over a host interface, the file system.
- **Public, additive**: `include/sxml/processor.h` gains the file system
  (`sxml_file_system`: an operations table with the directory, path and
  line separators and ten operations, describe, read, write, list,
  create_directory, remove, move, create_temporary, get_directory and
  canonicalize, each over request structs) and the configuration field
  `file_system`, next to the resolver and the clock. `name.h` (frozen by
  D99) gains `SXML_NAMESPACE_EXPATH_FILE` = 14
  (http://expath.org/ns/file); `SXML_NAMESPACE_WELL_KNOWN_COUNT` is 15.
- **Opt-in twice** (D184): the host names the feature and gives a file
  system. `sxml_all_optional_features()` leaves the feature out, so
  naming every feature never grants file access. Without the feature the
  functions are absent (XPST0017); with it and the null file system,
  every function that needs files raises file:io-error.
- **Rows**: 37 functions in `src/function_library_file.c`, the
  definition's `library_functions` (D191). Paths are made absolute against
  CURRENT and normalized lexically in the library, so the host sees only
  absolute paths; directory paths end with the separator. Text goes
  through `src/text_decoding.c` (decoding moved there by D198; the encoder
  of bin:encode-string moves there too). Errors are in the file
  namespace, filled into the error record (static text). file:move onto an
  existing directory moves into it, overwriting a file of the same name,
  as file:copy does.
- **No recursion** (D71): copy, delete, size and list of a directory walk
  it with a work list; file:descendants is a resumable body that asks the
  evaluator to call its filter and recurse predicates.
- **System property**: supports-file-library is a KNOWN_FUNCTION row
  (D198) for file:exists#1.
- **CLI**: `apps/sxml/platform_file_system.c` implements the interface
  over POSIX (realpath is a copy under WASI). `sxml xpath` and
  `sxml xquery` name the feature and pass the file system only with
  `--allow-file-access`; they bind the prefixes bin and file always.
  Windows is not implemented: initialization reports
  SXML_STATUS_UNSUPPORTED_FEATURE, and the CLI runs without files.
- **QT4 driver**: names the feature for a test case with a sandpit or the
  dependency feature expath-file. A sandpit is copied through the host
  file system to a fresh temporary root (root/sandpit is CURRENT,
  root/temp is TEMPORARY), served by a wrapping file system, and deleted
  after the case. expath-file is no longer a non-goal feature, nor
  expath-* a non-goal set, and sandpits are no longer unsupported.
- **Counts** (release driver): expath-file not run to 270 of 270 (XQ40)
  and 267 of 267 (XP40, 3 not run, XQuery-only); fn-system-properties 23
  to 23 (supports-file-library passes; the -false case is not run).
- **Size**: about 66 KB of native code (x86-64, release), above the
  20–30 KB estimate, none of it in `sxml.wasm` or `sxml-full.wasm`, which
  do not name the feature. The interface and the encoder move cost the
  modules +164 bytes (`sxml.wasm` 2,109,988 to 2,110,152 stripped at
  531c5b6 and 10ca66e; `sxml-full.wasm` 2,214,413 to 2,214,593). The core
  was over its 2,100,000 bytes before this packet; the full module's
  3,553,063 at 1e91f97 is fn:transform's (D203).

### D206. XSLT 1.0 follow-ups: EXSLT common, text value templates, result documents (packet 7.4b)
Recorded 2026-10-19, after D197 and D99. Follows up D197's "Not done".
- **name.h (frozen, additive, D99)**: `SXML_NAMESPACE_EXSLT_COMMON = 15`
  for http://exslt.org/common, `SXML_NAMESPACE_WELL_KNOWN_COUNT` 16.
  Needed because a function row names its namespace by a well-known
  constant (`sxml_native_function.function_namespace`); the list only
  grows and no value changes.
- **EXSLT common**: `src/function_library_exslt.c`, an XSLT-only library
  part (as `output`, D149) with exsl:node-set (returns its argument: a
  temporary tree is already a document node in XSLT 2.0 and later) and
  exsl:object-type ("string", "number", "boolean", "node-set" for nodes
  and the empty sequence, "external" otherwise). function-available
  reports them. No other EXSLT module: DocBook tests the others
  (dyn:evaluate, set:leading, date:date-time, str:*) with
  function-available first, and no XT40 case needs them.
- **exsl:document** in an extension namespace (24.2) is read as
  xsl:result-document (`src/xslt_read.c`): its attributes are a subset of
  xsl:result-document's (26.2), all attribute value templates; an
  unprefixed version attribute reads as the standard attribute, as on any
  XSLT element. element-available('exsl:document') is true. DocBook's
  chunker writes its secondary files through it (docbook-001 raised
  XTMM9000 "Can't make chunks" without it). Outside an extension
  namespace it stays a literal result element.
- **docbook-002's XPST0003** was fo/autoidx.xsl's xsl:key: its use
  attribute names entities of the external parameter entity
  common/entities.ent, which the module reader did not retrieve, so the
  expression lost its first argument. 7.14 (D199, 7a0c96a) now reads a
  module's external entities through the resolver; nothing else was
  needed.
- **Text value templates** in an element with XSLT 1.0 behaviour are
  lowered without XPath 1.0 compatibility mode (XSLT 4.0, 3.8.1): the
  value template frame lowers its expressions in a copy of the element's
  static context with the mode off.
- **xsl:result-document** whose result is the principal result (no href)
  chooses its default method as an explicit result document: xhtml for an
  XHTML html element, also in a 1.0 stylesheet (XSLT 4.0, 26.1).
- **Numbers** (release, XT40 whole suite, case by case, against 46cc929):
  8,940 to 8,943 passed: docbook-001, docbook-002, backwards-019b; no
  other case changed. Unit tests in `tests/xslt_compatibility_test.c`.
- **Size** (wasm preset, MinSizeRel objects): `src/xpath_compatibility.c`
  is 6,024 bytes of code and 1,133 of data before linking, all in
  `sxml.wasm`, because the XPath lowering calls it; the EXSLT part is 961
  bytes of code and 285 of data, also linked into the core through the
  part registry. At 01a3993 `sxml.wasm` is 2,294,051 bytes stripped
  (801,300 gzipped), over its 2,200,000 / 770,000 budget; these two files
  are about 8.4 KB of that.

### D207. JavaScript module budgets raised: core 2,200,000, full 4,000,000 (owner)
Recorded 2026-10-19 on the owner's choices. Amends D184 and D189.
- The core `sxml.wasm` crept to 2,120,223 bytes stripped over D184's
  2,100,000 (small additions of several packets; XPath 1.0 compatibility
  mode, D197, added 9,776). The owner chose to raise the budget, not to move
  schema validation out of the core: 2,200,000 stripped, 770,000 gzipped.
- fn:transform (packet 7.11) links the XSLT processor into
  `sxml-full.wasm`, now 3,553,063 bytes. The owner chose to keep it there and
  raise the full budget to 4,000,000 stripped, 1,400,000 gzipped.
- The gzipped limits keep D138's ratio of 0.35.

### D208. Budgets follow features; do not block on the owner (owner)
Recorded 2026-10-19 on the owner's word: "don't block on me, you can keep
raising the budget -- this is for people who want a full blown sxml
processor but not hundreds of megabytes to get java".
- The size budgets of the full module and the CLI module rise as features
  land; a packet that goes over raises the budget in the same change, says
  so in its commit message and its D-record, and measures the cost. The
  core `sxml.wasm` should stay lean: large features go behind D189's
  optional features, and the core budget rises only for small, general
  additions.
- Open questions that are not irreversible take the proposed default and
  are recorded, so work never waits: D183's Phase 7 gate numbers; adding
  the iXML tests and a sample of Unicode's CollationTest as vendored test
  data (D13); the languages en, de, fr, it, es, nl, the calendars ISO, AD,
  BE, JE, OS, AH and CLDR's standard tailorings for those languages; the
  XPath 4.0 §3.4.1 reading of number conversion in compatibility mode
  (D197); a processor option for the XML version of query text (D195),
  defaulting to 1.0.
- Still left to the owner, because they are outward-facing or legal: the
  license, tagging and publishing releases, and the npm package name.

### D203. fn:transform (packet 7.11)
Recorded 2026-10-19, after D71, D154, D157, D183, D189, D191, D196, D207.
Replaces D162's place for fn:transform (the function was absent,
XPST0017; supports-dynamic-xslt false).
- **Optional feature**: `sxml_xslt_transform_feature()` ("xslt-transform",
  declared in `include/sxml/xslt.h` as the packet's option 2), whose
  library_functions hold the one row (D191); in
  `sxml_all_optional_features()`. A host that names no XSLT and not the
  feature links no XSLT; `sxml-full.wasm` links it (D207).
- **One mechanism** (D183): the row's resumable body
  (`src/xslt_transform_function.c`, golden file
  `src/xquery_load_module.c`) reads the 33 options of F&O 4.0, 18.3
  (the option limit of the option conventions is 40 now), compiles the
  stylesheet with `xslt.h`'s compiler, primes the transformation and
  asks the machine for EVALUATE_UNIT; the next step delivers the
  principal result and collects the result documents (the built-in
  collector) into the result map; one step per result runs post-process.
  `src/xslt.c` splits `sxml_xslt_stylesheet_transform` into prime,
  describe, run and deliver, so a machine transformation
  (`src/xslt_transform_internal.h`) is the same priming with the
  evaluation left to the machine. No evaluator runs nested in C (D71);
  compiling evaluates static expressions on an evaluator of their own,
  where fn:transform has no evaluation environment and raises FOXT0004,
  so the compiler is not re-entered.
- **Extension per unit evaluation**: `sxml_evaluation_request.extension`
  is kept in the unit's global table (a unit evaluation that gives none,
  such as xsl:evaluate's, inherits the asking code's);
  `sxml_dynamic_context_get_unit_extension` gives the running code's.
  The result writer is found there first, so xsl:result-document and
  current-output-uri of the inner transformation reach its own writer;
  xsl:message honours enable-messages false. The body drops the
  extension before it releases the transformation, so function items
  that outlive it (transform-003) see none.
- **Options**: exactly one of stylesheet-location/-node/-text,
  package-location/-node/-text and package-name (FOXT0002); a node is
  serialized and compiled as text with its base URI (or
  stylesheet-base-uri); a location is read as fn:doc reads it (the
  caller's trust and documents; FOXT0002 when it cannot be read); a
  package name is a RESOLVED_PACKAGE. The invocation is the first of
  initial-function (with function-params), initial-template, a source
  (source-node, source-location, initial-match-selection; at most one),
  else xsl:initial-template; initial-mode with a call is FOXT0002,
  xsl:unnamed selects the unnamed mode. The global context item is
  global-context-item, else the root of source-node, else the
  source-location document. Parameter maps need xs:QName keys
  (XPTY0004). delivery-format document is TREE, absent is DEFAULT
  (build-tree), raw, serialized (UTF-8, no byte order mark, as
  fn:serialize), file FOXT0004, others FOXT0002. serialization-params
  override the unnamed xsl:output, the list parameters joined.
  xslt-version above 4.0 and requested-properties that the XSLT system
  property table does not give (xsl:version ignored) are FOXT0001.
  trusted false (the default) makes the unit untrusted. A stylesheet
  that is not well-formed is FOXT0003; static and type errors keep their
  codes as dynamic errors.
- **Result map**: "output" or the base output URI, resolved, for the
  principal result (left out when it is empty beside result documents,
  unless a result document became it, 26.2.2); each result document by
  its absolute URI.
- **Drivers**: QT4 supports fn-transform-XSLT and fn-transform-XSLT30
  (a 4.0 processor serves 3.0) and serves XSLT_MODULE as suite files;
  fn-transform is no non-goal set any more.
- **Counts** (release drivers): QT4 fn-transform XQ40 0 to 129 of the
  133 run (137, 4 not run), XP40 126 of 130; gate 80 % met. XT40 8,861
  to 8,870 passed, none lost, 0 errors; transform 0 to 9 of 12
  (transform-009 now reaches an assertion the driver does not check).
- **Size**: `sxml.wasm` 2,119,912 to 2,120,239 (+327, the extension
  lookups); `sxml-full.wasm` 2,224,369 to 3,554,353 (+1,329,984, XSLT).
- **Not done**: fn:transform in a static expression (transform-004:
  static evaluation has no evaluation environment); enable-assertions
  false (assertions stay enabled, fn-transform-91); a separate execution
  scope (cache false still shares current-dateTime, fn-transform-27);
  the inner transformation shares the caller's document loader (fn:doc
  is not stripped as its xsl:strip-space asks), available documents and
  trace sink, and fn:function-lookup sees the caller's table; xslt-version
  2.0 behaviour (82e); the QT4 driver gives a query file no base URI of
  its own (fn-transform-94); ~record patterns (transform-401, XSLT).

### D209. The timezone database (packet 7.16)
Recorded 2026-10-19, after D97, D162, D183, D184, D189, D191, D208.
Replaces D97's "no tz database" and the FODT0004 place of D162 in
fn:civil-timezone.
- **Data from the host, TZif**: the optional feature
  `sxml_timezone_database_feature()` ("timezone-database",
  `src/timezone.c`, `src/timezone_internal.h`) reads zones as the host
  has them: one TZif file per zone (RFC 8536, versions 1 to 4), asked of
  the resolver as the new `SXML_RESOURCE_KIND_TIMEZONE_DATA` (additive in
  `processor.h`) under `urn:sxml:timezone-data:` + a zoneinfo path (a zone
  name, or `zone1970.tab`, else `zone.tab`, for country codes). No
  compact form and no generator: TZif is the standard format, every
  Unix host has it in `/usr/share/zoneinfo`, and one reader serves both
  hosts and the vendored data. sxml checks a name before it asks (ASCII
  letters, digits, `_ - +`, parts between single `/`), so a resolver can
  map the path onto a directory. A zone is read at its first use and kept
  in the processor's feature state; a zone that is not delivered is asked
  again next time (no negative cache, so a query cannot grow the state
  with names that do not exist).
- **Vendored**: tzdata 2026e under `third_party/tzdata` (D13): the
  sources, and `zoneinfo/` compiled by `zic -b slim` with the zone
  tables, 599 files, 237 KB; `tools/update-tzdata.sh` moves it. The QT4
  driver serves it (`../tzdata/zoneinfo/` from the catalog), the unit
  tests read it from the source tree; the sxml command serves the host's
  `/usr/share/zoneinfo`; JavaScript hosts answer the kind
  `'timezone-data'`. Nothing is compiled into sxml.
- **Lookup** (RFC 8536, 3.2): type 0 before the first transition, the
  last transition's type, the footer's TZ string (POSIX XBD 8.3 with RFC
  8536's 167-hour and negative times) from the last transition on. Leap
  second records are skipped (xs:dateTime has no leap seconds). Years
  beyond ±10^9 get no answer.
- **fn:civil-timezone** (F&O 4.0, 9.7.4): no place is the implicit
  timezone, the UTC names stay PT0S without the database; any other place
  is a zone name, looked up at $value's instant ($value without a
  timezone takes the implicit timezone); FODT0004 without the feature,
  for an unknown zone, or a year out of range. A country code is not a
  zone here (the rules say "IANA timezone name").
- **$place of the formatting functions** (9.9.4.8): a zone name adjusts a
  value with a timezone to the zone's offset at its instant and gives
  [ZN] its abbreviation; a time alone takes the zone's standard time ("only
  possible where the value includes a date"). A country code (two ASCII
  letters, any case) does not adjust; [ZN] names the first zone of the
  country, in zone1970.tab's order, with the value's offset at that
  instant (2018-07-01 at -05:00 in "us" is America/Chicago's CDT). An
  abbreviation that does not start with a letter ("-03") is no name, and
  an offset with seconds (local mean time) is no xs:dateTime timezone:
  the fallback format 01:01 is kept, as for an unknown place and no
  place (format-time-030).
- **Numbers** (release drivers, against 97aedba): fn-civil-timezone 4 to
  9 of 9 (XP40 and XQ40); with the dependency olson-timezone turned on,
  fn-format-dateTime XQ40 83 to 93 of 94 run (not run 9 to 1), XP40 80 to
  88 (not run 14 to 6): all 8 olson-timezone cases and 025c/025e;
  fn-format-time XQ40 96 to 97 (025c); app-spec-examples XQ40 1,479 to
  1,480 and XP40 1,346 to 1,347 (fo-test-fn-civil-timezone-003);
  fn-format-date and misc-BuiltInKeywords unchanged; XT40 format-date and
  format-date-en unchanged. The packet's gate (all 14 cases) is met.
- **Size** (wasm preset): `sxml-full.wasm` 3,564,851 to 3,575,555 bytes
  (+10,704; gzipped 1,085,933 to 1,090,270), within its 4,000,000 budget;
  `sxml.wasm` 2,294,067 to 2,295,933 (+1,866: the two D162 places and the
  value adjustment, which ask for the feature), already over its
  2,200,000 budget before (D206); the CLI module 6,510,188 to 6,521,732.
  No budget changes.
- **Not done**: the dynamic context has no default place (a host setting
  would be an additive header change); without one, no place keeps the
  implicit timezone and [ZN] the fallback format.

### D210. DTD follow-ups: unparsed entities on copies, entity base URIs, sxml check with the DTD, the drivers (packet 7.14c)
Recorded 2026-10-19, after D44, D99, D178, D199. Resolves the three
Proposed items of 7.14: the base URI of an element from an external
entity, unparsed entities kept by copies, and `sxml check` with the DTD.
- **Substrate, additive** (`include/sxml/tree.h`, frozen by D99; every
  zero value keeps the behaviour before):
  - `sxml_tree_operations.get_unparsed_entity(state, entity_number,
    entity)`, optional: the unparsed entities in the order of their
    declarations, SXML_STATUS_INDEX_OUT_OF_RANGE past the last;
  - `sxml_tree_builder_copy_unparsed_entities(builder, tree)`: sends them
    right after start_document. `sxml_tree_builder_copy_node` calls it for
    a document node, and so does SHALLOW_COPY of a document node that is
    not copied into enclosing content (`src/evaluator_construct.c`): XSLT
    4.0, 11.9.1 (xsl:copy) and 11.9.2 (xsl:copy-of) copy the
    unparsed-entities property, and fn:copy-of and fn:snapshot copy as
    xsl:copy-of does. Temporary trees, xsl:document and the XQuery document
    constructor still have none (XSLT 4.0 sets the property to empty);
  - `sxml_element_start_event.entity_base_uri` and the optional
    `sxml_tree_operations.get_entity_base_uri(state, element, base_uri)`:
    the entity base URI (glossary `entity_base_uri`) of an element at the
    top of an external parsed entity. The XML parser sets it from the
    entity's frame (an element one deeper than the document's element
    depth when the frame was pushed); the arena tree keeps it in a short
    list; dm:base-uri stops its walk of xml:base attributes at the first
    element that has one and resolves against it instead of the tree's
    base URI (XML 1.0, 4.2.2; XML Base, 4.2; XDM 4.0, dm:base-uri).
- **Copies do not carry entity base URIs**: XSLT 4.0, 11.9.2 gives a
  copied descendant its new parent's base URI ("these differences are
  lost when the subtree is copied"). Copies that stand for the same
  document (xsl:strip-space, validating a parsed document) lose them too;
  left as it is.
- **XTDE1370 and XTDE1380**: each function now raises its own code (XSLT
  4.0, 20.5.2, 20.5.3; error-1380b). Without a focus both raise XPDY0002,
  as the two sections say for an omitted $doc; error-1370a and 1380a
  expect the XSLT 2.0 codes and stay wrong-error passes (D178).
- **`sxml check --external-entities` and `--validate-dtd`** (D44: one
  intent per flag, nothing fetched that was not asked for). The first
  reads the external subset and external entities as local files, through
  the query commands' local-file resolver and
  `sxml_processor_entity_resolver`, relative to the file that declares
  them. The second validates with `sxml_dtd_validator` and reads nothing
  by itself. Its null report sink stops at the first error of each file,
  reported as `FILE:LINE:COLUMN: CODE: message`; the JSON statuses are
  `valid` and `invalid`, the latter with `code`. Golden file
  `apps/sxml/command_validate.c`; CLI tests `check_external_*` and
  `check_validate_dtd*` over `tests/cli/input/dtd/`.
- **Drivers**: XT40 serves a source given only by a select expression,
  evaluated without a focus (id-043). QT4 claims infoset-dtd and
  dtd-validation-option and parses environment sources with the entity
  resolver (external DTDs are suite files). supports-dtd-validation and
  supports-dtd-attribute-typing are true (F&O 4.0, 17.1.9: fn:doc and
  fn:parse-xml raise an error for an invalid document, and the tree
  has is-id and is-idrefs from the DTD).
- **Measured** (release drivers): XT40 8,944 to 8,952 passed, none lost
  (163 wrong-error, 0 errors); dtd cases 68 to 76 of 79 (96 %, gate 90 %
  met). The 3 left: base-uri-051a (fn:doc without trust-external raises
  FODC0016, as F&O 4.0 asks), error-1370a and 1380a (wrong error, above).
  QT4 XQ40 fn-id 47 to 61 of 61, fn-idref 34 to 51 of 54 (the 3 need
  schema-typed IDREFs), fn-system-properties 22 to 23, app-FunctxFunctx
  626 to 627; XP40 fn-id 17 to 30, fn-idref 10 to 24, fn-system-properties
  22 to 23; fn-doc and fn-parse-xml unchanged (XQ40 73 and 53, XP40 67
  and 42). XQ40 whole suite 44,835 to 45,000 passed (with 7.11's driver claims, c857ce3), failures 336 to 336. supports-dtd-validation and supports-dtd-attribute-typing are true regardless of the optional feature dtd-validation (D211); they should follow it, which needs the feature in reach of fn:system-properties. xmlconf unchanged (D199's counts).
- **clang-tidy**: `src/processor.c`'s multi-level pointer conversion is an
  explicit cast now.

### D212. XSLT streamability analysis (packet 7.6a)
Recorded 2026-10-19, after D97, D162, D166, D183, D184. Replaces D97's
"streamable="yes" runs unstreamed" by the analysis of XSLT 4.0, chapter
19 (whose rules are XSLT 3.0's chapter 19); evaluation stays unstreamed
until 7.6b.
- **Where**: `src/xslt_streamability.c`, called once from
  sxml_xslt_compile after lowering, over the sealed stylesheet AST and
  the XPath ASTs of its expressions, patterns and value templates (the
  same trees lowering reads; no tree of its own). Not over the IR:
  chapter 19 classifies by syntax production, and lowering rewrites
  paths, `//` and value templates. A frame machine with explicit stacks
  in allocator memory, no recursion (D71); a construct needing more than
  100,000 frames is uncertain.
- **What**: each construct's posture, sweep and U-type (glossary
  `posture`, `sweep`, `u_type`): the general rules (19.8.1) with operand
  usages, choice operand groups and higher-order operands; every
  instruction of 19.8.4; stylesheet functions by category with streaming
  parameters (19.8.5); attribute sets (19.8.6); value templates
  (19.8.7); the XPath expressions of 19.8.8 (paths with scanning
  expressions, axis steps with rule 4, filters, unions, for and
  quantified expressions, maps, lookups, dynamic calls, inline functions
  and named references); the table of 19.8.9 and its special functions
  (accumulator-after and -before, current, current-group, last, root,
  outermost, the higher-order functions refined by an inline function's
  signature); motionless patterns (19.8.10). Verdicts: template rules
  of streamable modes, by import precedence of xsl:mode (6.6.4);
  xsl:source-document (18.1); functions with a category; attribute sets
  (10.2.3); accumulators, their values type-adjusted by the declared
  type and capturing rules (4.0, 19.9) grounded (18.2.9); merge sources
  (15.4). The first not guaranteed-streamable is XTSE3430 at the
  construct (19.10).
- **Uncertain**: where sxml has no rule (an extension function, a
  function the 19.8.9 table does not list, keyword arguments, a dynamic
  call without a visible signature, a binding sxml cannot trace, XPath
  4.0 constructs chapter 19 does not know), the assessment is uncertain
  unless the operand is grounded, and an uncertain verdict is never an
  error: sxml rejects nothing on a guess (19.10 lets a processor stream
  more than the rules guarantee).
- **Beyond the letter of chapter 19, as the XT40 cases expect**: E//T
  is E/descendant::T when T's predicates are plainly non-positional; "."
  is a pattern step (./descendant::a/b scans); xsl:copy select is
  assessed as xsl:for-each over the selection (19.8.4.12 would make the
  content higher-order); xsl:fork with at most one consuming prong takes
  the general rules (one prong may return streamed nodes); current-group()
  of a grounded population is grounded and motionless anywhere, and of a
  group around a streamed xsl:source-document grounded inside it;
  intersect and except with a motionless right operand keep the left
  operand's posture; one for or some binding over streamed nodes is
  S ! R with the variable as "."; a reference to the streaming parameter
  inside a higher-order operand is free-ranging (19.8.4.30, note); an
  instruction with XSLT 1.0 behaviour is roaming; xsl:map with
  duplicates (4.0) takes the general rules; group-starting-with and
  group-ending-with must be motionless patterns.
- **Driver**: the XSLT driver claims the feature streaming and no
  longer streaming-fallback, which needs an option sxml does not have
  (Proposed); the feature and the strm group stay outside the D95
  denominator. system-property-013 (streaming satisfied="false") is
  therefore not applicable.
- **Measured** (release driver, whole XT40, ed905be and 8a8fd58): passed
  8,952 to 11,792. The 2,936 cases that need the feature streaming, not
  run before, 2,841 passed (2,681 with the feature claimed and no
  analysis, at f12ac89); 154 of the 156 that expect XTSE3430 pass; the
  strm group 2,503 of 2,564 (97.6 %). Every other case as before
  (system-property-013 now not applicable: 8,952 to 8,951).
- **Not done, left to 7.6b**: streamed evaluation itself (the receiving
  task fed by the parser, D166); the two XTSE3430 cases the analysis
  misses (sf-filter-001, marked _WRONG in the suite; si-fork-953, sorted
  groups in xsl:fork, which 19.8.4.19 allows); supports-streaming;
  the fallback option. 7.6b can read the verdicts: a guaranteed-
  streamable mode or source document is what it may stream, and the
  postures name what each construct selects.

### D213. Full UCA collations over the host's DUCET (packet 7.15a)
Recorded 2026-10-19, after D97, D116, D162, D183, D184, D189, D191, D208.
Replaces D97's UCA fallback as the behaviour of a UCA collation URI when
the host serves the DUCET; the fallback stays for hosts that do not.
- **Public** (`include/sxml/processor.h`, additive): the optional feature
  `sxml_uca_collation_feature()` ("uca-collation", in
  `sxml_all_optional_features`) and the resource kind
  `SXML_RESOURCE_KIND_COLLATION_DATA` (form BYTES): the feature asks for
  "http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt" once per
  processor, at the first UCA collation, and keeps the table (also "no
  data") in its slot. Nothing is compiled into the core (D184, D189):
  `sxml.wasm` gains only the resolution path (below).
- **Internal**: `src/collation_uca.c` reads allkeys.txt (UTS #10, 9.1)
  in two passes into sorted arrays (elements, single-codepoint entries,
  contractions) and computes implicit weights (10.1, with the file's
  @implicitweights and Unicode 16.0's Unified_Ideograph). The elements
  of a string: NFD with canonical ordering, the longest contiguous match,
  discontiguous contractions (S2.1.1 to S2.1.3), variable weighting
  (shifted, blanked; maxVariable groups from the general categories of
  the characters a primary starts), caseFirst and caseLevel from the
  tertiary weights of UTS #10's tertiary table (upper: 08 to 0C, 0E, 11,
  12, 1D), numeric=yes (UTS #35, 3.7: a digit run weighs as its count
  of significant digits, then its digits, in the 255 weights after digit
  zero's primary, primaries being shifted left 8 bits), the identical
  level over NFD (7.4). normalization=yes|no both normalize (the same
  result for every FCD string). `src/collation.c` keeps one element
  array for every collation (keys, comparison and matching are shared);
  `sxml_collation_resolve` is the one place a UCA URI becomes the
  feature's collation (D162's place: without the feature or data, the
  fallback, and fallback=no is not supported).
- **fallback=no**: supported only with the table and when every
  parameter is honoured: lang empty, root, und, en or en-XX (CLDR has no
  tailoring for these; others are 7.15b), no reorder (7.15b), version
  equal to the table's @version, known keywords and values. Otherwise
  FOCH0002 dynamically and XQST0038 for `declare default collation`
  (now checked with the processor). `sxml_collation_is_supported`
  (xpath.h) has no processor and still says false for fallback=no.
- **Default collation in comparisons**: value and general comparisons
  of strings and the sort keys of order by and xsl:sort without a
  collation now use the static context's default collation (XPath 4.0,
  4.11.1 and 4.11.2; XQuery 4.0, order by), through the one resolver;
  before, they used the codepoint collation whatever the default was,
  which the 4 XQST0038 "wrong error" passes had hidden.
- **Hosts**: the QT4 driver serves the vendored copy
  (`third_party/unicode-collation/`, D13, D208) and claims
  advanced-uca-fallback and non_unicode_codepoint_collation (no longer
  non-goals); the sxml command embeds allkeys.txt without comments
  (generated at build time by `apps/sxml/embed-collation-data.cmake`) and
  serves it; the JavaScript package forwards 'collation-data' to the
  host's resolver and otherwise serves `LoadOptions.collationData`,
  default the allkeys.txt it ships next to the modules (false: none).
  The XSLT driver names the feature but serves no data yet (fallback).
- **Tests**: `tests/collation_uca_test.c` sorts a sample of Unicode's
  CollationTest (every 20th line of the _SHORT files, NON_IGNORABLE
  10,314 lines and SHIFTED 11,391, both in order; the full _SHORT files,
  206,286 and 227,801 lines, also pass, not vendored), the parameters,
  matching, malformed data and an allocation-failure loop.
- **Measured** (release drivers, the sets that use collations): QT4
  advanced-uca-fallback 49 of 49 pass (XQ40 and XP40), not run before;
  non_unicode_codepoint_collation 13 of 13 (XQ40), 5 of 5 applicable
  (XP40). XQ40 passes 3,896 to 3,961 over fn-compare, fn-contains,
  fn-starts-with, fn-ends-with, the four fn-substring sets,
  fn-collation-key, fn-deep-equal, prod-GroupByClause,
  prod-DefaultCollationDecl (12 of 12, no longer 4 of them by a wrong
  error), app-spec-examples and the other collation sets; XP40 3,301 to
  3,358; no case that passed fails. XT40 collations 35 to 39 of 43
  (the default collation in comparisons and sorts), sort, merge,
  for-each-group, avt, function, choose, error, maps unchanged.
- **Sizes** (stripped / gzipped, at 8a8fd58 and with the feature):
  `sxml.wasm` 2,131,618 / 742,974 to 2,134,058 / 743,745 (+2,440: the
  resolution path, the element arrays, the default collation in
  comparisons); `sxml-full.wasm` 3,653,848 / 1,136,201 to 3,666,394 /
  1,140,763 (+12,546, the feature's code); the CLI 6,156,220 /
  1,351,350 to 7,303,201 / 1,562,302 (+1,146,981, nearly all the
  embedded DUCET text). The CLI budget rises to 7,500,000 / 1,650,000
  (D208); the JavaScript budgets hold. The package ships allkeys.txt
  (2,285,767 bytes) as a file beside the modules.
- **Left for 7.15b**: tailorings (lang other than the root languages,
  reorder) from CLDR rules, the XSLT driver serving the DUCET (with lang
  sorts), CLDR's root order where it differs from the DUCET.

### D211. DTD validation is an optional feature; the core size audit (packet 7.2b)
Recorded 2026-10-19, after D162, D189, D199, D207, D208. The core
`sxml.wasm` was 2,294,051 bytes stripped (801,273 gzipped) at 7fac033,
over D207's 2,200,000 / 770,000, so `javascript/size_budget` failed.
- **What grew it** (linker maps of the core module, code and data, from
  8d19cbf, D207's 2,121,086 / 739,785): 4869814 (fn:doc and fn:parse-xml
  dtd-validation, D199) +167,419. The processor's document loader called
  sxml_dtd_validator_create, which compiles the schema documents written
  from the DTD, so the whole XSD schema compiler became reachable:
  schema_read 55,036, schema_content_model +27,304, schema_simple_type
  19,178, schema_complex_type +18,546, schema_resolve 10,019,
  schema_assemble 8,556, schema_components +4,972, schema_assertion
  +3,725, schema_identity +2,457, schema_from_dtd 1,707, data +11,552.
  ffccefa (fn:put and revalidation, D205) +4,267: xquery_update_apply
  +3,043, the rest of XQuery Update through the lowering registry (D103).
  01a3993 (EXSLT, D206) +1,279: function_library_exslt 1,080. a7e7e38 and
  eb1e397: 0. No HTML or other feature was linked by mistake; XPath 1.0
  compatibility mode was already in D207's number.
- **Moved**: DTD validation becomes `sxml_dtd_validation_feature()`
  (`include/sxml/schema.h`, additive; name "dtd-validation"; kind
  SXML_OPTIONAL_FEATURE_KIND_DTD_VALIDATION; definition and operations
  table in `src/schema_from_dtd.c`, its type in `src/dtd_internal.h`).
  The loader (`src/processor.c`, sxml_loader_parse_with_dtd) reaches the
  validator only through the table. Without the feature the
  dtd-validation option raises FODC0013, F&O 4.0's error for a processor
  without a parser for the requested options (17.1.1, 17.2.1; its note
  names web browsers): the option's single D162 place.
  `sxml_all_optional_features()` includes it, so the sxml command, the
  QT4, XSLT and XSD drivers and `sxml-full.wasm` keep DTD validation.
  The public sxml_dtd_validator API is unchanged: a host that calls it
  links it.
- **Kept in the core**: reading the DTD, internal and external entities,
  attribute defaults and normalization, is-id and is-idrefs (src/xml.c,
  src/xml_entity.c); `src/dtd.c` (9.5 KB, the DTD written as schema
  documents) stays linked, because the parser calls it through the
  validator interface of the frozen `xml.h`. Schema validation for
  fn:xsd-validator stays (D207's choice), as do the XQuery lowering
  parts of the registry (D189: about 75 KB, a registry split, not a
  feature).
- **System properties**: supports-dtd-validation and
  supports-dtd-attribute-typing (F&O 4.0, 17.1.9; true since D210) follow
  the feature: a value kind OPTIONAL_FEATURE in
  `src/system_property_internal.h`, answered by
  sxml_processor_find_optional_feature, as the KNOWN_FUNCTION properties
  follow their features' functions.
- **Sizes** (stripped / gzipped -9): `sxml.wasm` 2,294,051 / 801,273 to
  2,128,754 / 741,943, under 2,200,000 / 770,000 with no budget raised
  (7,668 bytes over D207's module: XQuery Update 3,624, the loader's
  DTD and trust-external code 1,536, EXSLT 1,080); 2,131,859 / 742,980
  at 384a2e8 with 7.16's timezones. `sxml-full.wasm` 3,577,133 /
  1,108,707, the CLI module 6,079,521 / 1,323,861.
- **Counts** (release drivers, all features configured; the same before
  and after): QT4 fn-doc XP40 67, XQ40 73 of 75; fn-parse-xml XP40 42,
  XQ40 53 of 57; fn-doc-available 34 of 47; fn-system-properties 23 of
  35; fn-id XP40 30, XQ40 61 of 61; fn-idref XP40 24, XQ40 51 of 54;
  XT40 document 83 of 99. No XSD code changed, so no XSD set was run.
- **Resolved Proposed item**: "xs:numeric parameters in XPath 1.0
  compatibility mode" (7.4b, D197) is decided by D208: sxml follows
  XPath 4.0, 3.4.1, which applies fn:number in the mode only to xs:double
  and xs:double? parameters. The 14 cases (QT4 numberformat15, 38, 41,
  42; XT40 format-number 014, 035, 038, 039, 040, 041, backwards-022,
  version-021, xpath-compat-0302, 0401) stay failures, not reclassified
  (D178).

### D214. XQuery and XPath Full Text 3.0 (packet 7.12a)
Recorded 2026-10-19, after D12, D53, D71, D162, D183, D187, D189, D208.
Amended as the packet goes on.
- **Grammar** (`grammars/xquery` only, D12): Full Text 3.0, Appendix A,
  with FTContainsExpr as the operand of ComparisonExpr above XQuery 4.0's
  OtherwiseExpr. Its 43 keywords are not reserved words: tree-sitter's
  keyword extraction returns one only where the parse state expects it,
  so no name position changes. (Reserved, as the XQuery keywords are, they
  grew the compiled parser by 1.5 MB; contextual, with the choice rules
  inlined, by 500 KB, in the CLI only.) One GLR conflict, after a
  thesaurus URI ("at least ... levels" against "at start").
- **AST**: 29 kinds, 16 fields, 15 keywords, four flags; a selection is
  a tree of FT_SELECTION, FT_OR, FT_AND, FT_MILD_NOT, FT_UNARY_NOT,
  FT_PRIMARY_WITH_OPTIONS and FT_WORDS nodes with the positional filters
  as children of their FT_SELECTION.
- XPath's Full Text is not implemented: the QT4 cases are XQuery only,
  and the XPath grammar and `sxml.wasm` stay as they are.

### D215. Invisible XML: fn:invisible-xml over an Earley parser (packet 7.18)
Recorded 2026-10-19, after D12, D13, D53, D71, D162, D183, D189, D191,
D208. Amended as the packet goes on.
- **No tree-sitter for user grammars** (D12 gives way, as D53 did for
  regex): an iXML grammar is run-time data, the argument of
  fn:invisible-xml, and its language is any context-free language, so it
  needs a general parser built while the query runs (Earley, as the
  packet says). The iXML notation itself could be a tree-sitter grammar,
  but then sxml would carry two parsers for one job; instead the grammar
  of iXML grammars is compiled in as a constant table of its XML form, so
  the one Earley parser reads grammars and inputs alike.

### D216. JavaScript bindings for XQuery, XSLT and XSD validation (packet js-1)
Recorded 2026-10-19, after D10, D138, D187, D189, D205, D207, D208.
The package (`bindings/javascript`) covered XPath only.
- **Mechanism**: D189's two links. The full module links four more shim
  files (`sxml_javascript_full.c`, `_xquery.c`, `_xslt.c`, `_schema.c`);
  the core module links none, so `sxml.wasm` is byte for byte what it
  was. The package tells the modules apart by the exports
  (`sxml_javascript_query_compile` present or not), not by a feature:
  XQuery, XSLT and XSD are whole front ends, not optional features of a
  processor. The handles and helpers both links share moved to
  `sxml_javascript_internal.h`, still `static` (an inline hint on all of
  them made the core 1,609 bytes larger).
- **API** (`src/index.ts`), in the XPath style, compile once, run many
  times: `Processor.compileQuery(text, { namespaces, baseUri })` gives an
  `XQuery` (`evaluate({ context, variables, updates, messages })` to a
  Value, `serialize(value)` with the prolog's output declarations, `run()`
  both, `isUpdating`); `Processor.compileStylesheet(text, { baseUri,
  staticParameters })` gives a `Stylesheet` (`transform({ source,
  initialMatchSelection, initialMode, initialTemplate, initialFunction,
  args, parameters, templateParameters, tunnelParameters, baseOutputUri,
  delivery, resultDocumentDelivery, serialization, messages })` to `{
  output, value, resultDocuments }`); `Processor.compileSchema(documents,
  { baseUri })` gives a `Schema` (`validate(text or node, { mode, type,
  uri })` to `{ validity, reports, document }`). Errors are SxmlError
  (now with `sourceName`); a schema that does not compile throws
  `SchemaError` with every report. Library modules, included modules,
  packages and schema documents come through the resolver.
- **Side results** (glossary `side_result`): what a call delivers besides
  its result (updated trees and fn:put documents of the update sink,
  result documents from the built-in collector, trace output, schema
  reports) is kept by the handle until its next call and written into the
  request; the package adopts every value at once, then hands them to the
  `updates` sink and `messages` callback after the call, also when it
  failed. No host import is added: callbacks never run inside sxml.
- **Defaults taken** (D208): invalid input is a result, not an
  exception; a transformation without `source` and `initialMatchSelection`
  calls xsl:initial-template, as `sxml transform` does; `serialization`
  replaces the unnamed xsl:output's parameters, method included (the
  parameter reader cannot merge); fn:put without `putDocument` raises
  FOUP0002 (xquery.h); text is validated in one pass by the XML parser
  without external entities (the null entity resolver), so reports have
  lines and columns, a node by copy (sxml_schema_validate_node's steps,
  keeping the validator for the not-known outcome that function does not
  give); xsl:message as the text method writes it, fn:trace and
  fn:message by the adaptive method.
- **Sizes** (stripped / gzip -9): `sxml.wasm` 2,134,058 / 730,518
  unchanged; `sxml-full.wasm` 3,666,545 / 1,124,343 to 6,570,723 /
  1,328,474, almost all the XQuery compiler and its grammar. The full
  budget rises to 7,000,000 / 2,450,000 (D208; D207's ratio of 0.35).
- **Tests**: `test/full.test.ts`, 24 tests under Node 22, as
  `javascript/languages_full`; `javascript/languages` runs it on the core
  module, where it checks that the three are refused. The existing
  package tests pass on both modules.

### D217. The sxml command writes files: --output, --result-directory, --allow-put (packet cli-2)
Recorded 2026-10-19, after D44, D200, D205. Closes the CLI gaps D44 left
planned ("no files written without --output") and the queue's cli items.
- **One mechanism** (`apps/cli_framework/cli_command.h`): the invocation
  gets an `sxml_cli_file_writer` beside the file reader (find_kind,
  create_directory, begin, commit, abandon), implemented by the platform
  layer's new `apps/sxml/platform_file_writer.c`. A write goes to a
  temporary file `.NAME.sxml-N.tmp` beside the target, made with O_EXCL
  (fopen "x" on Windows), and is moved into place on commit: rename when
  replacing, else link (EEXIST refuses a file that appeared meanwhile)
  then unlink, falling back to a second look and rename where link is
  refused (WASI, file systems without hard links); MoveFileEx on Windows.
  The query commands keep their files in `sxml_query_run`;
  `sxml_query_run_finish(run, exit_code)` commits them all on success and
  abandons them all otherwise, so a failing command changes no file.
- **--output=FILE (-o FILE) and --force** on xpath, xquery, transform and
  serialize: FILE gets the bytes standard output would get. An existing
  FILE without --force is refused before the work starts (exit 3, "'FILE'
  exists; give --force to replace it"); `-` or an empty name is a usage
  error, as is --force with no option that writes. For transform, FILE's
  URI is the base output URI unless --result-directory is given.
- **--result-directory=DIR** (transform; XSLT 4.0, 2.4.6 and 26.2): the
  explicit option the orchestrator proposed, not "below --output's
  directory" (D44's planned row), because one option naming one file must
  not write others (one intent, one action). DIR must exist; its
  normalized file URI is the base output URI; a result document whose
  absolute URI, percent-decoded, is not a file below it (dot segments,
  an escaped "/", another directory or scheme) is refused with exit 3;
  directories below DIR are made. Without the option result documents
  with an href are reported on standard error and dropped, as before but
  no longer silently.
- **--allow-put** (xquery; XQUF 3.0, 6.1; D205): an explicit flag, not
  part of --allow-file-access, which grants the EXPath File feature. With
  it the update sink gets receive_put_document: the node is serialized
  with fn:put's $params (`sxml_serialization_parameter_reader`) to the
  local file its `file:` URI names (resolved against the static base URI,
  the working directory since 7.10b); another scheme exits 3. Without it
  fn:put still raises FOUP0002.
- **--method=csv** in every command's choices (Serialization 4.0, CSV
  output method); the query commands add no final line feed after it,
  since every record ends with one.
- **Option hints**: an unknown long option with no close match among the
  command's own names the commands that have it ("it is an option of 'sxml
  xquery'"), in `cli_help.c`; close matches keep "did you mean".
- **Tests**: `sxml_add_cli_test(... WORKSPACE FILES ... EXISTING ...)`
  runs in an empty directory and compares every file written (changed
  inputs included) with `expected/<name>.files`, which catches a
  temporary file left behind. CLI tests 161 to 203.
- **Left**: Windows paths of the file writer are written but not built or
  run here; the permissions of a replaced file are not kept (the new file
  has the default mode); fn:put does not make missing directories.

### D218. HTML follow-ups: encoding sniffing, every legacy decoder, the html5lib tests, XHTML (packet 7.10b)
Recorded 2026-10-19, after D13, D71, D189, D201, D208. Closes D201's list
for 7.10b.
- **Determining the encoding** (HTML, 13.2.3.2): octets without a byte
  order mark or encoding option are prescanned (the first 1024 bytes,
  the standard's "get an attribute", then "get an XML encoding"), then
  step 9 (well-formed UTF-8 is UTF-8), then windows-1252; the confidence
  is then tentative. `sxml_html_prepare_input` reports the encoding and
  the confidence.
- **Changing the encoding while parsing** (13.2.3.4): a meta element in
  "in head" (also reached from "in body") whose charset, or whose
  http-equiv Content-Type content (HTML, 2.5.3), names another encoding
  while the confidence is tentative stops tree construction, and the
  document is parsed again from memory with that encoding, certain: at
  most two passes. The optional change of decoder on the fly is not
  done. With fail-on-error and a tentative encoding a pass does not stop
  at its first parse error, so the errors of a discarded pass never
  count.
- **Every encoding** (Encoding Standard, 10 to 13): gb18030 (also GBK's
  decoder), Big5, EUC-JP, ISO-2022-JP, Shift_JIS and EUC-KR decode, one
  loop each in `src/html_encoding.c`; "restore to ioQueue" moves the
  offset back over the bytes just read. The indexes are generated into
  `src/html_encoding_tables.c` by `tools/generate-html-encodings.py`
  (dense uint16 tables by pointer; Big5's 1,713 plane 2 code points as a
  sorted pointer list; the 207 gb18030 ranges). No label is FODC0011 any
  more; an unknown label still is.
- **XHTML** (F&O 4.0, fn:parse-html, a "should"): without a media type
  the input says what it is. When the document element after the prolog
  is an unprefixed `html` with `xmlns="http://www.w3.org/1999/xhtml"` and
  the decoded input is well-formed XML, `sxml_processor_parse_document`
  parses it (the XMLDocument mapping of 17.3.1); otherwise it is HTML.
  No external DTD is read, so `&nbsp;` and the like make it HTML. A
  prefixed XHTML root is parsed as HTML.
- **Source locations**: not done. The arena tree keeps dm:location only
  with `keeps_locations`, and the processor builds every document without
  it (`src/processor.c`, XML documents too), so HTML locations would be
  dropped. When the processor keeps locations, the tokenizer records the
  line and column of each token's start and the walk of `src/html.c`
  puts them on the events, in that one place (D162).
- **The html5lib tests are vendored** (D13, D208's default):
  `third_party/html5lib-tests/` holds web-platform-tests'
  `html/syntax/parsing/resources` (6df7a6ed, BSD) and html5lib-tests'
  `tokenizer/` and `encoding/` (c777c408, MIT), pinned in
  `third_party/html5lib-tests.commit`; 2.4 MB. `tests/html5lib_test.c`
  runs them: tree construction 1,749 of 1,753 (the 4 are selectedcontent,
  filled by DOM steps of option and select, not by the parser; listed as
  known failures), 220 fragment and script-on cases left out; tokenizer
  7,045 of 7,045 (4 with lone surrogates left out); encoding 82 of 82.
  Whether there is a parse error agrees in every tokenizer case, and is
  found in every tree case that lists one (fail-on-error depends on it).
- **CLI**: `sxml xpath` and `sxml xquery` have the working directory's
  file URI as static base URI, so html-doc and unparsed-text of a
  relative name work (folded into cli-2's 430379b).
- **Counts** (release driver, before and after): fn-parse-html 22/22
  (XQ40 and XP40), fn-html-doc 9/10, misc-HtmlTestSuite 1,378/1,379
  (XQ40). html-doc-010 and html-test-1281 stay as D201 says.
- **Sizes** (stripped / gzipped, wasm): `sxml.wasm` 2,134,058 / 743,746
  before and after; `sxml-full.wasm` 6,570,723 / 1,347,198 to 6,755,529 /
  1,497,292 (+184,806 / +150,094, nearly all the indexes; budget
  7,000,000 / 2,450,000); the CLI module 7,817,964 / 1,593,193 to
  8,002,794 / 1,740,606 (budget 9,000,000 / 2,000,000). No budget
  changes.

## Proposed

- **A streamability option and a streamed source in `xslt.h`** (packet
  7.6a, D212; for 7.6b and the owner of `xslt.h`): XSLT 4.0, 19.10 asks
  a streaming processor to offer, at user option, XTSE3430 or processing
  without streaming for a construct declared streamable that is not
  guaranteed-streamable. sxml always raises XTSE3430, so the XT40
  streaming-fallback cases (7 with merge-097sf) are not run. Proposed:
  an additive last field of `sxml_xslt_stylesheet_configuration`, for
  example `bool falls_back_to_unstreamed`, read by
  sxml_xslt_check_streamability, with 7.6b's streamed principal source.

- **The CLI's WebAssembly budget after the Phase 7 features** (packet
  7.10a, D201; for the owner): the `sxml` CLI module names every optional
  feature, so each feature grows it. Before 7.10a it was 5,848,173
  bytes stripped, already over the 5,500,000 budget (1,239,159 gzipped
  of 1,250,000); HTML parsing makes it 5,927,297 / 1,267,818, now also
  over the gzipped budget, so `javascript/size_budget` fails. Choose:
  raise the CLI budget (for example to 6,500,000 / 1,400,000, leaving
  room for 7.11 to 7.18), or link fewer features into the CLI's
  WebAssembly build.

- **XQuery's XML version** (packet 7.13, D195; for the owner of
  `xquery.h` and `processor.h`): query texts follow XML 1.0 (line ends,
  XQST0090 for `&#1;`, XQST0085 for `xmlns:p=""` in direct constructors,
  version 1.0 as the default serialization version). XQuery 4.0 (A.2,
  xgc: xml-version) leaves it implementation-defined and recommends
  1.1's control-character rules with a 1.0 mode. QT4 has 13 XQ40 cases
  that need 1.1 lexing and about as many 1.0 cases that need 1.0
  lexing. Choose: keep 1.0; switch to 1.1; or add a processor option
  for the XML version (one additive header field; the QT4 driver then
  sets it per case).

- **Phase 7: four more features and the size of optional parts** (packet
  7.1, D183; for the owner). The details are in `docs/tasks/phase-7.md`,
  under "Proposed for the owner", "Size" and "Questions for the owner".
  - *Features no phase covers yet*: xsl:evaluate (83 XT40 cases, 7.3),
    disable-output-escaping (32 XT40, 7.5), XML 1.1 input (313 XQ40, 65
    XT40, 275 xmlconf; 7.13) and DTD processing (32 XQ40, 78 XT40, 265
    xmlconf; 7.14). Proposed: they join Phase 7.
  - *Size*: `sxml.wasm` has 1,277 bytes left. The XPath-reachable
    features would add an estimated 0.5 to 0.8 MB. Choose one of:
    1. raise the budget per feature;
    2. optional features chosen at link time through the processor
       configuration (one additive `processor.h` change; `sxml.wasm`
       unchanged, plus a larger module that names the features);
    3. CMake options with stub files;
    4. large data (DUCET, CLDR rules, the timezone database) supplied by
       the host in its standard formats through the resolver.

    Recommended: 2 with 4, a 2,100,000-byte core budget and a
    3,000,000-byte full module.
  - *Also*: the per-feature gate numbers; vendoring the iXML community
    tests and a sample of Unicode's CollationTest; the languages,
    calendars and tailorings (proposed: en, de, fr, it, es, nl; ISO, AD,
    BE, JE, OS, AH); XQUF 3.0 only, or also the 4.0 draft; EXPath File off
    in the CLI unless `--allow-file-access` is given.

- **The CLI's WebAssembly budget after sxml validate** (packet 5.10,
  D176; for the owner, with the 4.15 item below): linking schema
  compilation and validation makes the stripped `sxml` CLI module
  4,877,565 bytes (1,126,567 gzipped), over 4.15's 1,100,000 gzipped;
  5.10 set the ctest budget to 5,500,000 / 1,250,000, leaving room for
  5.8 and 5.9. `sxml.wasm` was unchanged then (1,916,341 / 663,954). At
  the Phase 5 review (D182) the CLI is 4,950,289 / 1,154,545 and
  `sxml.wasm` 1,997,773 / 695,343 of 2,000,000 / 700,000. Choose:
  confirm, or split the optional front ends into modules.

- **The CLI's WebAssembly budget after sxml transform** (packet 4.15,
  D157; for the owner, with the budget items below): linking XSLT (its
  lowering and the pattern and value template grammars) makes the
  stripped `sxml` CLI module 4,609,397 bytes (1,026,010 gzipped); 4.15 set
  its ctest budget to 5,000,000 / 1,100,000. `sxml.wasm` is unchanged
  (no XSLT in the JavaScript package yet). Choose: confirm, or split the
  XSLT front ends into an optional module.

- **XQuery's default for omit-xml-declaration** (packet 3.12, D146; for
  the owner of `include/sxml/xquery.h`): the header gives every
  undeclared parameter serialize.h's default, so omit-xml-declaration is
  yes; Serialization 4.0 leaves it implementation-defined for XQuery and
  QT4 expects a declaration (K2-Serialization-22, -23, Serialization-xml-01:
  standalone or item-separator declared alone). Proposed: XQuery's default
  is no (the declaration is written), in the header's comment and in
  `src/xquery.c`.

- **The CLI's WebAssembly budget after sxml xquery** (packet 3.12, D146;
  for the owner, with the budget item below): linking the XQuery grammar
  makes the stripped `sxml` CLI module 3,450,339 bytes (767,283 gzipped).
  3.12 set its ctest budget to 3,700,000 / 820,000 and kept `sxml.wasm`
  at 2,000,000 / 700,000, so `compileQuery()` is not in the JavaScript
  package yet. Choose: confirm, or split the XQuery grammar into an
  optional module (then compileQuery() and a lower CLI budget follow).

- **The WebAssembly size budget** (packet 2.18, D138; for the owner):
  Phase 2 grew the stripped `sxml` CLI module from 1.21 MB to 1.82 MB
  and its gzipped size from 358 KB to 630 KB, past D93's 1.5 MB / 450 KB.
  2.18 set the ctest budget (`javascript/size_budget`) to 2,000,000
  bytes stripped and 700,000 gzipped for the CLI and `sxml.wasm`, so it
  holds today with about 10% headroom. Choose: confirm that budget, or
  ask for optional parts (XQuery and XSLT grammars, collation and Unicode
  tables) as separate modules (PLAN.md, risks) and keep a lower budget
  for the core. At the Phase 2 gate (D151) `sxml.wasm` is
  1,865,152 bytes stripped and 647,721 gzipped: 7 % left.

