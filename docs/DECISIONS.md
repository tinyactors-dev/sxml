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

### D120. The XML and XHTML output methods (packet 3.3)
Recorded 2026-10-17. `src/serialize_internal.h` splits the serializer
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
Recorded 2026-10-17. Every method runs on the 3.3 pipeline (D120); no
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
  also XHTML-namespace ones (unprefixed, prefix normalization of D120);
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
- **SERE0006** (left by D120): XML and XHTML with version 1.0 (the
  default) raise it for C0 controls in text and attributes; 1.1 writes
  references. **SERE0003** for comments with "--" or a final "-" and PIs
  with "?>" (XML, XHTML). A character map may map any XML 1.1 Char
  (U+0001 included; serialize.h says XML 1.0's production, D120 already
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
Recorded 2026-10-17, after D71, D103, D109, D124, D126, D142.
Reserved here; the record is completed when the packet is ticked.

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
Recorded 2026-10-17, after D53, D71, D105.
Reserved here; the record is completed when the packet is ticked.

### D148. Library modules and module import (packet 3.11)
Recorded 2026-10-17, after D106, D134, D135, D146.
Reserved here; the record is completed when the packet is ticked.

## Proposed

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

- **Prolog follow-ups** (packet 3.10, D135). (a) For the owner of the
  evaluator's SORT (`src/evaluator.c`): "empty greatest" with
  "descending" puts empty keys last (and "empty least" first); XQuery
  4.0, 4.13.9 orders empty as least/greatest before the direction
  applies (QT4 emptyorderdecl-10, -11, -18, -19). (b) Parameter defaults
  that read the focus (`$x := string()`) must see the caller's focus, and
  fixed defaults (`$x := <a/>`) are one value for every call (5.18.3;
  function-decl-40-040 to -060): proposed, lower a default at the call
  site when it reads the focus, and a fixed one as a hidden global.
  (c) A global whose own closure reads it (`declare variable $f := fn {
  ... $f(...) }`) fails with XQDY0054, because closures capture globals
  when made (D55); reading a global from an IR function of the same unit
  at call time would fix it (vardeclerr-3a, K-InternalVariablesWith-50,
  -52). (d) `fn:function-lookup` does not see declared functions (the
  dynamic context's table is the processor's). (e) `declare default
  element namespace "##any"` (5.14.1) is not supported. (f) For the QT4
  driver (3.13): `assert-eq` should atomize the result (default_namespace-023,
  -024, namespaceDecl-26).

- **The WebAssembly size budget** (packet 2.18, D138; for the owner):
  Phase 2 grew the stripped `sxml` CLI module from 1.21 MB to 1.82 MB
  and its gzipped size from 358 KB to 630 KB, past D93's 1.5 MB / 450 KB.
  2.18 set the ctest budget (`javascript/size_budget`) to 2,000,000
  bytes stripped and 700,000 gzipped for the CLI and `sxml.wasm`, so it
  holds today with about 10% headroom. Choose: confirm that budget, or
  ask for optional parts (XQuery and XSLT grammars, collation and Unicode
  tables) as separate modules (PLAN.md, risks) and keep a lower budget
  for the core.

- **The item axis in the evaluator** (packet 2.5, D113; for the owner of
  `src/evaluator_path.c` and `include/sxml/tree.h`): `item::` is lowered
  over the child axis and a wrapper array, so the singleton case's JNode
  has the wrong parent and document order. Proposed: append
  `SXML_AXIS_ITEM` to `sxml_axis` (additive, D99) with the STEP semantics
  of XPath 4.0, 4.7.4 (a JNode with ·jkey· 1, ·jvalue· jvalue(G) and
  ·jparent· G, made once per G so `is` holds); lowering then emits one
  STEP. Also: the JNode key test casts `key_lexical_form` without
  namespaces, so a QName key in a namespace (given as an EQName,
  `include/sxml/types.h`) never matches; the cast should read the EQName
  form (QT4 sequence-type-61, `jnode(#xml:space)`).

- **Collations in the other comparing bodies** (packet 2.13, D116; for
  the owners of `src/function_library_numeric.c`,
  `src/function_library_higher_order.c`, `src/function_library_array.c`
  and the evaluator's SORT): fn:min/max, fn:highest/lowest, fn:sort-by,
  array:sort and order-by keys with a collation still accept only the
  codepoint collation (FOCH0002 otherwise, through
  `sxml_function_library_check_collation` or their own checks). They
  should resolve with `sxml_function_library_resolve_collation` and
  compare with `sxml_function_library_compare_collated`
  (`src/function_library_core_internal.h`).

- **Report name and areas for the XSLT driver** (packet 4.1, D117; for
  the owner of `tests/qt4/driver/report.c`): the JSON report says
  `"report": "sxml-qt4 run"` and sums QT4 areas (fn, op, ...), so
  `sxml-xslt-test run --format=json` reports area "other" for most sets.
  Proposed (additive): `program_name` in `sxml_qt4_report_configuration`
  (empty = "sxml-qt4") and an optional area function there, so the XSLT
  driver can report its test-set groups (attr, decl, expr, fn, insn,
  misc, type) for the 4.17 dashboard.

