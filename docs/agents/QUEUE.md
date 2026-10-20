# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–5 and 7 closed by agent review (D11, D151, D161, D169, D182,
  D237); fuzzing at length and the full lint wait for the owner's machine
  (D181). 7.11's gate (all 12 XT40 transform cases) is open: 10 pass;
transform-004 needs a suspendable compiler (D71), transform-401 waits on
4.22's newest-draft patterns (D249).
- Passes (release, whole suites at e74d4ad, report-1, docs/CONFORMANCE.md):
  XP40 32,815 (26 failed), XQ40 46,158 (74), XT40 12,268 (250), XSD11
  41,599 of 41,610 run (11), iXML 886 of 907 (21 not run), tree-sitter
  1,410 of 1,438 (8 failed, 1 corpus error). Zero errors in the W3C
  suites. Gate rates: XP40 99.8 %, XQ40 99.8 %, XT40 96.7 %, XSD 100.0 %.
  tools/conformance-report.py has no iXML or tree-sitter sections yet.
- One build with everything (D272, size-3): every shipped build has every
  language and optional feature; the JavaScript package ships one
  `sxml.wasm` (7,701,002 / 1,713,241 bytes; budget 8,000,000 / 2,800,000;
  `./sxml-full.wasm` is an alias). CLI budget 9,500,000 / 2,100,000. The
  size test compares the module's linker map with a baseline and prints
  the top growths (D225). fn:load-xquery-module serves every language.
- EXPath Binary (D198) 646 of 646 and File (D200) 270 of 270 in XQ40; File
  needs `--allow-file-access` in the CLI and has no Windows implementation.

## Running

Priorities (D255): 1. conformance, 2. a mise task building the CLI for
every supported platform, 3. the user-facing book. Phase 6 is paused
(bindings/kotlin/DESIGN.md, bindings/swift/DESIGN.md).


- 7.25: tree-sitter .wasm language files in the JavaScript package (a JS
  dynamic linker; sxml.wasm shares memory and table; owner, 2026-10-20).
- book-4: the codegen chapter rebuilt around a TypeScript tree and an XSLT
  printer (owner, 2026-10-20).
- js-2: streaming output (and input) in the JavaScript API (owner).
- 4.31: XSLT namespace undeclarations in the tree, per-package items,
  copy-4308 (D289).
  (Book-first pages carry <!-- status: draft -->; check-book-examples.sh
  skips them unless --drafts.)

## Next

- Phase 6 (D238, `docs/tasks/phase-6.md`), packets that run here:
  - wave 1, together: 6.2 host shim (`bindings/host/`; JavaScript
    tests and sizes unchanged); 6.3 Android build (four ABIs at API 24,
    the two fallbacks in `src/allocator.c` and `src/processor.c`,
    presets, `tools/fetch-binding-toolchains.sh`);
  - wave 2, after 6.2 and 6.3: 6.4 Kotlin core (JNI, `kotlin/jvm`) and
    6.7 Swift core (`swift/linux`);
  - wave 3: 6.5 Kotlin full, after 6.4; 6.8 Swift full, after 6.7;
  - wave 4: 6.6 Android AAR (assemble only), after 6.3 and 6.5;
  - wave 5: 6.10 the device CI workflow (written, disabled) and 6.11
    the bindings' documentation.

  Owner: 6.9 Apple builds and iOS tests; the device runs of 6.6 and
  6.10.
- More XPath/XQuery and XSLT sweeps on the leftovers below; perf-4 on casts
  and path-MAP hoisting.

## Owner's machine (Phase 6)

- 6.9: the XCFramework (`cmake/apple.cmake`, `tools/build-xcframework.sh`,
  `libtool -static`), a `binaryTarget` for `CSxmlHost`, `swift test
  --sanitize=address --disable-xctest` on macOS, `xcodebuild test` on an iOS
  simulator and a device, and `Bundle.module` finding `allkeys.txt` (D248).
- Android emulator and device runs (6.6, D238): `tools/fetch-binding-toolchains.sh
  ndk`, `tools/build-android.sh`, then connectedAndroidTest on x86_64 API 24
  and 35 and one arm64 device (D251). Owner choices: which host libraries
  sxml-jvm ships (proposed linux-x86_64 and macOS arm64); keep the 32-bit
  ABIs or not.

## Leftovers no packet owns yet

Kotlin (6.4/6.5 done, D247, D250): 65 tests on the JVM with -Xcheck:jni and
0 leaks; the host shim now refuses U+0000, U+FFFE, U+FFFF. 6.6 (AAR) after
6.3; Kotlin's SchemaException subclasses SxmlException, Swift's SchemaError
does not (6.12 to compare).

Swift (6.7/6.8 done, D241, D248): 55 tests on Linux; the bundled DUCET is a
copy of third_party/unicode-collation/allkeys.txt that must be updated with
it.

XPath and XQuery (2.28, D287): syntax errors reported at the inner ERROR
node (string constructors no longer 1:1); run-time closures carry module
and focus (one mechanism for XQuery and XSLT); D154's limit lifted (a
fresh global table after the evaluation ends); focus-free defaults
computed once; item types across module cycles. XQ40 46,154 -> 46,158 (74
failed). What is left is kept on purpose (D286) or singletons.

XPath and XQuery (2.27, D286): XInclude for fn:doc, fn:parse-xml and
fn:document (no xpointer); Japanese ideographic numerals; element() no
longer selects JNodes. XP40 32,809 -> 32,815 (26 failed), XQ40 46,138 ->
46,154 (78 failed). Left: xqhof18/19 (run-time closures carrying module
and focus), modules-43 (types imported in a cycle), function-decl-40-060,
record-003/-005/-016 (draft rejects surplus entries), fn-string-length-26
(contradicts XDM 7.5.2.4), 4 XP40 cases relying on XQuery-only character
references.

XPath and XQuery (2.26, D280): XP40 32,797 -> 32,809 (31 failed), XQ40
46,101 -> 46,138 (90 failed). A resolver may answer a module import with
a SEQUENCE of locations (processor.h, xquery.h contract widened). D197
stands (xs:numeric under 1.0 compatibility: the draft limits fn:number
conversion to xs:double). Left: xqhof18/19 (run-time function-lookup
items' static context), eqname-007, modules-43. Cheap not-run features:
fn:doc use-xsi-schema-location (to 5.16), CJK numerals, XInclude.

XPath and XQuery (2.25, D275): XP40 32,793 -> 32,797, XQ40 46,070 ->
46,101 passed (107 failed); XT40 +13 (`//` lowering, inline functions read
globals at call time, per-element default collation). XML 1.1 query text
(processor.h `reads_query_text_as_xml_1_1`, additive; the QT4 driver sets
it, the CLI keeps 1.0). Left: Q{}N name tests on JNodes, named item type
constructors and function items via fn:function-lookup (xqhof18/19),
eqname-007, two modules of one namespace without a location hint,
serialization singletons, IDREF at an element validation root (kept as
cvc-id).

XPath and XQuery (2.24, D263): XP40 32,716 -> 32,731, XQ40 46,017 ->
46,070 passed. tree.h gained the optional set_element_identity operation
(additive, D99). Static typing: XQuery 4.0 dropped the feature (PR 1344);
about 4,000-6,000 lines for 3.1's rules; default (D208): not done. Left:
XQuery's XML 1.1 lexing (13), whitespace-only assert-xml, Q{}N name tests
on JNodes, named item type constructors found by fn:function-lookup at
run time, xml:id under lax validation, IDREF checks at a validation root,
serialization singletons.

Older (after 2.23, D258; XP40 32,716, XQ40 46,017 passed):
- XSLT's own expressions have no Full Text (they use grammars/xpath; D235).
- NCName vs `Q{}N` name tests (sxml_name_test is frozen).
- A library module's context value type checked against the context value
  (XQuery 4.0, 5.17; contextDecl-050/051/064/066).
- fn:string-length and fn:normalize-space: an explicit argument is atomized,
  the default is fn:string(.) (string-length-23, normalize-space-24).
- Casts to xs:QName with an enclosing direct constructor's namespaces (CAST
  uses the unit's static namespaces, D84; 4 cases); `$e:code` with xmlns:e
  on the same element (try-catch-err-code-variable-13).
- is-id of list and union ID types (fn-element-with-id-4/5); fn:xsd-
  validator's use-xsi-schema-location (FODC0009).
- The suite's `recordTypeDecl/` module paths (the directory is
  `RecordTypeDecl/`; 11 cases, a suite error; the owner reports it upstream).
- fn:serialize canonical-JSON cases (27) need `bin:unpack-double` (7.8).
- A processor keeps every imported schema until it is destroyed (D174).

XSLT (after 4.20, D230; XT40 11,912): merge 107/120, error 474/580,
validation 51/68, namespace 217/224, normalize-unicode 18/18. Left:
XTDE3490; merge-097 (Saxon collection URI); xml-version-007 (value-template
grammar rejects XML 1.1 controls); namespace-2606/-6202; docbook-004;
fn-transform-27/82e/94; xsl:copy of JNodes (needs an xdm.h addition);
XTDE0430/0440; 4.22 (D252): the newest drafts (13 Oct 2026, qtspecs 72f2229) and
xslt40-test (91328fb) are what sxml already had; 57 suite cases contradict
the draft (53 "~T" type patterns that PR 2911 removed, 4 priorities,
match-080/239) and the draft wins (D245; the owner confirmed on
2026-10-20: no "~T" extension). typed="~T" on xsl:mode added.
Refresh both together when qtspecs or the suite move. 4.21 (D234): indexed FILTERs
are not hoisted from; current-merge-group(()) per 15.5.1; globals computed
on first read (GLOBAL_READ); typed modes and XTSE3105; XT40 11,943 at its
last run. Left: document-4066 (fn:document's schema validation does not
raise FODC0002); a global's side effects now happen at its first read.

XSLT (4.30, D289): XT40 12,261 -> 12,268 passed, 250 failed; key-004 lost
to the draft (20.2.2 note: keys compare by fn:atomic-equal). split-when
via fn:partition; one XSLT closure mechanism; new-each-time="no" cached;
adaptive character maps. Left: map-for-key-005 (to 2.28), copy-4308,
function-lookup and accumulators per package, per-package strip-space for
fn:document, schema roles kept apart (import-schema-216), merge-when.
Streaming: about 20 single causes (~a week). Namespace undeclarations: an
additive tree.h binding with an empty URI (4.31).

XSLT (4.29, D281): XT40 12,250 -> 12,261 passed (99 wrong error), 257
failed. Of what is left: 112 the draft decides, 42 suite or environment,
about 130 failed and 72 wrong error real gaps. Small: xsl:evaluate codes
(12), package/override static codes (~20), serialization (~10),
map-for-key with empty $top, copy-4308. Medium: schema-aware validation
codes (~20), record coercion (12, to 2.27), D274's document items,
new-each-time="no", split-when/merge-when, function-lookup visibility per
package. Large: streaming (~20), namespace undeclarations in the tree
(namespace-2606). self::element() selecting JNodes to 2.27.

XSLT (4.28, D276): XT40 12,239 -> 12,250 passed, 268 failed. XSLT
instruction errors carry module and line (D271's XTTE gap closed). Left:
FORG0001 in parameter/variable coercion casts (4), XTTE0945 when the focus
is absent only at run time (2), an attribute set inside xsl:for-each of a
rule still sees the rule; xs:numeric under 1.0 compatibility given to 2.26
(check against D197). About 17 cases the draft decides.

XSLT (4.27, D274): XT40 12,210 -> 12,226 by its own changes; 12,239
passed, 279 failed at 92a8a8c with 2.25's first commits. Left:
xsl:next-match in attribute sets (2), per-package strip-space for
fn:document (document-2401), fragment identifiers in xsl:source-document
(docbook-003/004), strip-space "none" returning XSLT's stripped copy of an
available document, XTTE line numbers (D271), namespace-2606. Draft wins:
key-070, error-0150b/c/e, iterate-006, maps-910, iterate-024.

XSLT (4.26, D273): XT40 12,191 -> 12,209 passed, 309 failed. Given to
2.25: `//` lowered right-nested in xpath_lower.c gives duplicate nodes
(5 cases), XSLT inline functions capture globals by value (2). Left: CDATA
text merging with neighbours (4), a fixed invalid start-at as static
XTSE0020 (2), key patterns matching namespace nodes, fn:document with
strip-space "none". fn:document's trust-external default false only with
an options map (6 older cases need external DTDs).

XSLT (4.25, D262): XT40 12,157 -> 12,187 passed. Left: stream-available-
004/005 (a streamed open of the resource); choose-0106, type-0150 and
collations-0121..0124 (casts and comparisons read the function's module,
not the inner element's static context; 2.24's add_cast_namespaces covers
XQuery only); fn:load-xquery-module in XSLT would link XQuery into XSLT
builds (D189); D260's single cases. Kept as the draft decides: FORG0001 in
coercions, no XTTE0600, sequence JNode copies, format-number under 1.0
compatibility, XSD 1.1 regex syntax, XPDY0002 for unparsed-entity-*.

size-3 (D272): fn:load-xquery-module in XPath: XP40 0 -> 56 of 60 run (021,
040, 051, 067 fail); XT40 load-xquery-module-001 raises FOQM0002 per F&O
4.0 where the suite expects XQST0059 or FOQM0006.

XSLT prefixes (4.24, D261): a run-time cast of a string to xs:QName still
falls back on XPath's predeclared prefixes in every language (the evaluator
does not know the language).

XSLT backwards compatibility (7.4, D197): backwards_compatibility 111 of
124; QT4 xpath-1.0-compatibility 1 of 5. 14 cases expect fn:number-style
conversion for xs:numeric parameters, which XPath 4.0 §3.4.1 restricts to
xs:double (D208: follow the spec). 7.4b (D206): exsl:node-set,
exsl:object-type, exsl:document; docbook-001/002 pass; docbook-004 fails
with FODC0002 in xsl:source-document.

xsl:evaluate (7.3, D196: 73 of 83, gate 90 %): 3 need parse-html (7.10),
2 need json-doc/unparsed-text-available trust checks, 1 a UCA strength
(7.15); keyword arguments to stylesheet functions from the target
expression. disable-output-escaping (7.5, D188): 25 of 25 run.

HTML (7.10a/b done, D201, D218): misc-HtmlTestSuite 1,378 of 1,379,
fn-parse-html 22/22, fn-html-doc 9/10; html5lib tree construction 1,749 of
1,753, tokenizer 7,045 of 7,045, encoding 82 of 82; all WHATWG multi-byte
decoders; XHTML input. Left: source locations (the processor builds every
tree without them), a prefixed XHTML root, XHTML that needs an external DTD.

XML 1.1 (after 7.13, D195; D275): query text by XML 1.1 rules is a
processor option; XT40 namespace-2606/-6202 still fail in `src/xslt*`.

XSD (5.13 sweep 3, D277): in scope 41,486 -> 41,507 of 41,518. The 11
left: XSD 1.0 rules (3), circular attribute groups the 1.1 Rec allows (1),
xs:NOTATION used directly (3, w3c/xsdtests issue 12), instance tests of
indeterminate schemas (4). Validation continues after a rejected element
(content recovery). Not run: 92 introspection tests (all pass with
xlink.xsd), 57 "queried", 50 version 1.0. The library does not follow
schema location hints (5.16).
5.16 (D278): schema location hints in the library
(schema_location_hint_node, additive in schema.h), fn:xsd-validator and
fn:doc/parse-xml/document's use-xsi-schema-location, `sxml validate
--use-schema-location`; xlink.xsd vendored (third_party/w3c-xlink): XSD
41,507 -> 41,599 passed, 92 of 92 introspection tests. XQuery validate and
XSLT validation attributes still ignore hints (their specs allow it).

Duplicates merged in 7.20 (D249): one decoder with an error rule, Full Text
on the shared feature-data path, sxml_allocator_reserve_array, and D182's
five cheap ones. Left: about 30 private array growers, XSLT's yes/no
reader, a second UTF-8 encoder, cldr's byte_text. (Earlier, D182: the schema reader's own boolean and integer
parsers, Gregorian day arithmetic in four places, three ways of building
compile-report text, four text writers, three private heapsorts in
serialization.

Performance (perf-1, D224): benchmark set in tests/bench; itunes 80.6 s ->
3.4 s, XMark-All 6.73 -> 1.52 s; filter indexes for =/eq/is joins (also
match="$var"); one-pass axis walks; loop-invariant hoisting. perf-2 (D229):
XMark-All 1.55 -> 1.20 s, match="$var" 1.02 -> 0.45 s, numeric joins 2x;
fast number casts, leaves computed in place, inline small lists, numeric
index keys, quantifier hoisting. perf-3 (D232): XML parsing 15 -> 25 MB/s
(tree 13 -> 21 MB/s), XMark-All -10 %; tags and plain attributes are
single tokens. Left: casts cost about 1,100 instructions; hoisting of path
MAPs and sort keys; the task struct copied on each push.

Distribution (dist-1, D256): `mise run dist` builds the CLI for x86_64 and
aarch64 Linux (static musl), macOS and Windows, and wasm32-wasi, as
archives with SHA256SUMS. Ran here: both Linux (aarch64 under qemu),
x86_64 Windows (wine), WASI. Owner: run the macOS and aarch64 Windows
programs, signing and notarization, publishing. Windows has no
/usr/share/zoneinfo, so the CLI there needs embedded or shipped tz data.

The book (book-1, D257): `docs/book/`, every chapter written; Getting
Started, XPath, XQuery and XSLT are complete, the others shorter.
`tools/check-book-examples.sh` re-runs its examples; a change to CLI
output the book shows updates the book. Left: chapters for a published
npm package and release archives once they exist, more on XSLT 4.0's new
instructions, and schema-aware XSLT output validation.

perf-4 (D259): the book's join 12.9 s -> 0.045 s (descendant pass, the
filter index's number view, probe order); early exit through item demands
and quantifier fusion (10^7 items: seconds -> 4 ms); default step bound
10^8 (about 8 s). Left: two filters in a row index only the first;
FILTER/MAP over MAP not fused; demands do not enter function bodies;
if/boolean read one extra item.

book-2 (D270): XQuery part 5 -> 11 sections, XSD part 2 -> 9; 387
examples checked. cli-2 (D271) fixed its four CLI bugs (collection ->
FODC0002, library-module error file names, undeclared xsi:type prefixes
-> cvc-elt.4.1 with defaults read in the schema's namespaces, fn:trace
and fn:message to standard error) and gave --context-item documents a
document URI. Left: XTTE errors from XSLT validation have no line number
(src/xslt_lower*.c gives no source span); content after a
cvc-complex-type.2.4 rejection is not checked; the CLI ignores
xsi:schemaLocation.

book-3 (D282): Chapter 12, a code-generation project (XSD and an API
description to TypeScript with XQuery, OpenAPI with XSLT, run with
Node.js). Left: a syntax error inside a string constructor's enclosed
expression is reported at 1:1 of the module, not at its own place.

7.22 (D285): sxml:parse-code and sxml:code-languages (namespace urn:sxml;
name.h SXML_NAMESPACE_SXML and processor.h sxml_source_code_feature(),
additive); json, javascript (JSX), python, c, css, html, go vendored with
highlights; tree-sitter/suite 630 of 639 (corpus 452/452, highlights
178/187: 7 need locals, 2 injections). sxml.wasm 9,693,235 / 2,006,198
(budget 10,000,000 / 2,800,000), CLI 11,289,043 / 2,288,933 (budget
11,500,000 / 2,500,000). Owner (2026-10-20): TypeScript/TSX and Markdown
built in, a registration API, run-time language files (7.23).
7.23 (D288): typescript, tsx, markdown, markdown_inline built in; locals
and injections (8 deep); public sxml_source_code_language and
source_code_languages (tree-sitter ABI 13-15); CLI --code-language=LIBRARY
(dlopen/LoadLibraryW in the platform layer). tree-sitter/suite 1,410 of
1,438 (highlights 187/187; 8 markdown_inline extensions not in upstream's
default parser, 1 upstream spec.txt error). sxml.wasm 13,425,285 /
2,411,765 (budget 14,000,000), CLI 15,026,404 (15,500,000 / 2,900,000).
Delayed: tree-sitter .wasm language files in JavaScript (sxml.wasm cannot
link modules at run time), the native wasm store (wasmtime), the host
shim and bindings (Phase 6 paused), injection.combined.

7.21 (D283): Earley with lookahead, Leo items, compiled classes, grammar
simplification. 1 MB: log 1.12 -> 0.18 s, csv 1.25 -> 0.28 s, calc 2.14 ->
0.53 s, markdown 3.21 -> 0.30 s, right recursion fails -> 0.30 s; linear
(4x input, 3.8-4.2x time); memory 90-200 bytes per character (all sets
kept). Ambiguous inputs stay cubic. Suite 886 of 907 (21 not run: Unicode
versions, C0 controls).

7.24 (D290): regular rules compiled to token automata (every end a
completion; ambiguity still counted); item sets only where items are;
no tree nodes for hidden parts; parse-only arrays freed before read-out.
1 MB log 0.196 s / 90 MB -> 0.059 s / 21 MB; 100 MB log 6.2 s / 1.17 GB
(was over the step limit). Parser's own memory under 2 MB per MB; the
rest is the XML tree. Left: the ambiguous sum (3.1 s against D283's 2 s
bound on this machine; owner's machine to judge).

Platform and tooling:
- macOS, Windows and MSVC have never been built; `_getcwd` is untested.
- CI is disabled (D123).
- The QT4 driver's `list`/`run` still have their own selection walk;
  `selection.h` is shared by the XSLT and XSD drivers.

## Owner decisions open

Decided in D239: MIT license, npm `@tinyactors/sxml`, the move to the
tinyactors-dev organization (URLs change with the move), Kotlin
`dev.tinyactors.sxml`. Still the owner's: tagging and publishing releases,
the copyright holder's name if not "The sxml authors", and when to move
the repository. Everything else takes the proposed default (D208).
