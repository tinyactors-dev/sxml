<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Work queue

Kept by the orchestrator. Newest state first; each change is its own commit.
Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–5 and 7 closed by agent review (D11, D151, D161, D169, D182, D237);
  fuzzing at length and the full lint wait for the owner's machine (D181).
  7.11's gate (all 12 XT40 transform cases) is open: 10 pass; transform-004
  needs a suspendable compiler (D71), transform-401 waits on 4.22's
  newest-draft patterns (D249).
- Passes (release, whole suites at e74d4ad, report-1, docs/CONFORMANCE.md):
  XP40 32,815 (26 failed), XQ40 46,158 (74), XT40 12,268 (250), XSD11 41,599 of
  41,610 run (11), iXML 886 of 907 (21 not run), tree-sitter 1,410 of 1,438 (8
  failed, 1 corpus error). Zero errors in the W3C suites. Gate rates: XP40 99.8
  %, XQ40 99.8 %, XT40 96.7 %, XSD 100.0 %. tools/conformance-report.py has no
  iXML or tree-sitter sections yet.
- One build with everything (D272, size-3): every shipped build has every
  language and optional feature; the JavaScript package ships one `sxml.wasm`
  (7,701,002 / 1,713,241 bytes; budget 8,000,000 / 2,800,000;
  `./sxml-full.wasm` is an alias). CLI budget 9,500,000 / 2,100,000. The size
  test compares the module's linker map with a baseline and prints the top
  growths (D225). fn:load-xquery-module serves every language.
- EXPath Binary (D198) 646 of 646 and File (D200) 270 of 270 in XQ40; File
  needs `--allow-file-access` in the CLI and has no Windows implementation.

## Running

Priorities (D255): 1. conformance, 2. a mise task building the CLI for every
supported platform, 3. the user-facing book. Phase 6 is paused
(bindings/kotlin/DESIGN.md, bindings/swift/DESIGN.md).

(Book-first pages carry <!-- status: draft -->; check-book-examples.sh skips
them unless --drafts.)

## Next

- perf-5 done (D309): slices of a stored sequence share their items; 100 walks
  over 780 elements 10.2 / 2.6 / 3.0 s to 0.11 s for `[position() > 1]` /
  `tail` / `subsequence`, QT4 XP40, XQ40 and XT40 unchanged case by case.
  SchXslt's expand stage of EN 16931: 8 min 56 s before perf-5 and perf-6; 7
  min 38 s unpatched now (its own re-sorts); 14.8 s with SchXslt's sort-once
  fix (was 1 min 39 s), output identical. Left: slices of concatenations and
  lazy sequences copy once; `fn:remove($s, 1)` and `$s[position() != 1]` still
  loop; the memo keeps one matched vector per item type.
- The step limit: the patched expand stage needs more than the default 10^8
  steps (it stops after 2 s); the CLI has no way to raise it. A CLI option is
  for the owner ("## Proposed" when written up).
- Also open under "Proposed" in DECISIONS.md: source locations for parsed input
  documents.
- perf-6 done (D308): a regex cache per evaluation; compiling was under 1 % of
  SchXslt's expand stage. Left: `tests/bench/regex-reuse.xq` is not yet in
  `run-bench.py` (perf-5 holds that file).
- perf-7 done (D310): faster regex matching. With perf-5, perf-6 and perf-7,
  SchXslt's expand stage with its sort-once fix takes 10.5 s (75.3 x 10^9
  instructions; was 14.8 s, and 1 min 39 s before perf-5). Left: an error
  record not cleared after a caught FORX0002, so a later LIMIT_EXCEEDED in the
  same query prints the old description.
- 7.30 done (D317): 514 cases, 2025-only cases checked independently. 7.29 done
  (D318): source locations, AUTOMATIC by default; left: the JavaScript
  `locations` option is written and tested but was not built (no WASI SDK in
  the session), and under AUTOMATIC a program that only raises errors naming
  nodes keeps no locations (D313's list has no fn:error). 7.28 (Schematron) can
  start: 7.27, 7.29, 7.30 and cli-4 are done.
- The owner's decisions of 2026-10-28 are D313: Schematron's open points,
  source locations (packet 7.29, after perf-8), no step limit in the sxml
  command (packet cli-4, runs now), SchXslt's fix not offered upstream.
- 7.27 (design, for the owner's review) and then 7.28: Schematron as a front
  end that builds the XSLT AST (owner, 2026-10-28). Starts after perf-8 and
  perf-9; the e-invoicing work can use the official pre-compiled EN 16931
  stylesheets until then.
- perf-8 done (D316, coercion 33 % to 18 % of the profile) and perf-9 done
  (D311, regex search 12.9 % to 0.4 %). SchXslt's expand stage of EN 16931:
  7.7–8.5 s with its sort-once fix, 4 min 51 s unpatched (8 min 56 s this
  morning); output identical.
- perf-10 done (D319): relabelled casts, choices of one item; SchXslt's expand
  stage 52.19 to 47.78 x 10^9 instructions, about 6.7 s.
- fix-4 done (D321): choices try every alternative (XPath 4.0, 3.4.2); a caught
  error no longer leaves its description in the record. Left: code that drops
  an error outside a catching task is not audited.
- perf-11 done (D322): about 1 %; the rest of atomization's cost is in the
  atomic cell (allocation 4.3 %, a second UTF-8 check 2.5 %, type lookup 1 %),
  proposed in D322.
- 7.28 done (D320, D323): Schematron natively, 7.30's suite 463 of 463
  applicable (3 listed deviations), EN 16931 UBL 1,133 of 1,133 from the `.sch`
  sources, compile 0.53–0.59 s. Waiting for the owner (Proposed in DECISIONS):
  EN 16931 CII (a parameter named twice) and XRechnung (pattern variables used
  schema-wide, `let` after assertions, an undeclared `xs` prefix) are not
  correct Schematron and are refused. Left: with `--schema`, findings run on
  the validated copy and lose line numbers; a dynamic error in an expression
  rule context has no location; the JavaScript API was not built (no WASI SDK).
- perf-12 done (D324): compile passes linear; invoice 2.68 to 2.08 x 10^9
  instructions (0.29–0.37 s).
- perf-13 done (D325): component graph linear, value templates without braces,
  the Schematron builder parses once; `validate --schematron` on an invoice
  about 0.41 s (was 0.59–0.73).
- perf-14 done (D326): the Schematron walk visits only the node kinds its rules
  can match; invoice validation 1.21 to 0.48 x 10^9 instructions (the official
  stylesheet: 0.55), about 0.3 s.
- perf-15 done (D327): rules indexed by kind and name; invoice transform
  0.21-0.23 s, `validate --schematron` 0.24 s, many-rules benchmark 7.0 to 0.46
  s.
- fix-5 closed (D328): XSLT 4.0, 6.4.1 gives a union pattern the minimum of its
  alternatives' default priorities (a change from 3.0); sxml already did; a
  test added.
- perf-16 done (D330, the IR passes walk each body once) and perf-17 done
  (D329, fewer grammar reductions, each XPath text parsed once per
  compilation). `validate --schematron` on one EN 16931 invoice: 1.12 x 10^9
  instructions, about 0.20 s; the pre-compiled stylesheet about 0.18 s. Left
  for later packets: the demand pass (0.10), owners made in four passes,
  verify, the global-read pass (D330); sxml.wasm not built in this session (no
  WASI SDK), its size budget unchecked after perf-17's tables (+about 206 KB).
- fix-6 done (the count drifted with later packets, stable per build; the book
  shows 194). book-30 done: the Schematron part (seven chapters on the Vane
  story), published to sxml.tinyactors.dev from efd5d4ddc on 2026-10-29.
- fix-7 done (D331): F&O's prefixes (`xs`, `fn`, `math`, `map`, `array`, `err`)
  resolve without `ns` (binding Annexes H to L); every finding names its
  document (text, JSON, C, JavaScript); real rule sets' known deviations are
  warnings (`schematron:compatibility`), so EN 16931 CII and XRechnung compile
  from their sources and run in ctest. Left: a pattern let defined only in a
  pattern the phase leaves out is not shared (XPST0008; not against the
  standard, which scopes pattern variables to their pattern, 3.23, and SchXslt
  does the same); the differential against SchXslt runs in ctest only with
  SXML_SCHEMATRON_DIFFERENTIAL=ON (minutes per rule set); the JavaScript API
  typechecked only.
- wasm-1 (build and test the JavaScript package; wasi-sdk-34 now at
  /opt/wasi-sdk) runs now.
- Conformance baselines differ by setup: 7.29 (D318) measured XP40 32,815, XQ40
  46,158, XT40 12,283 before and after; perf-8, perf-10 and fix-4 measured
  32,785, 46,114, 12,185 before and after on the same code. Each run is
  consistent, so no packet changed an outcome, but the setups differ (cause
  unverified; XP40's 32,815 equals the owner's report). Compare only against a
  base measured in the same setup and session, and name the setup (environment
  variables, drivers' options) in the record.
- Leftovers without a packet: the error record is not cleared after a caught
  FORX0002, so a later LIMIT_EXCEEDED prints the old description (perf-7);
  `c.md`'s memory example prints 197 allocations where the book shows 195 (also
  before cli-4); an exceeded limit exits 4, the status for internal errors
  (cli-4; the owner chose exit 1 for unsupported Schematron constructs, D315's
  revision).
- Sorting was measured and is not a problem: 780 elements sort in about 1.3 ms
  (`xsl:sort`) or 1.8 ms (`fn:sort`). SchXslt's 7 minutes were its own 800,000
  re-sorts; the fix (sort once in `schxslt:replace-params`) belongs upstream,
  for the owner to offer.
- 7.26 done (D307): tree-sitter:parse and tree-sitter:languages in
  urn:sxml:tree-sitter (name.h 17, additive), errors
  tree-sitter:unknown-language and tree-sitter:invalid-query; sxml:token and
  sxml:node stay in urn:sxml. tree-sitter/suite 1,410 of 1,438 unchanged; book
  examples match. Left: the paused Kotlin and Swift bindings bind neither
  prefix.
- The book on Vane is done (book-5..book-21, book-13): 18 chapters, 882
  examples, mdbook build clean. Conformance appendix numbers date from 20
  October (a new report run would refresh them).
- The book on the one-service Vane is done (book-22..book-29, D303): 18
  chapters, six Vane projects (codegen generates the SDKs), 884 examples,
  mdbook build clean, 217 internal links resolve. Leftover: the book check
  needs WASI_SDK_PREFIX exported for the examples that build tree-sitter
  language modules; `mise run book:check` does not set it yet (dist.sh shows
  how: `mise where github:WebAssembly/wasi-sdk`). For book-13: a separate `$
  echo $?` line always prints 0 (each command runs in a fresh shell); join it
  to its command (book-17 fixed its chapters).
- fix-2 done: invalid identity-constraint fields are validation errors (XSD
  3.11.4 cl. 4); global xsl:param values coerced to `as` (XTTE0590). For the
  chapter packets: xsd-typed-data.md can use typed params again,
  xsd-errors/xsd-identity the invalid-anchor example, traces drop the --param
  workaround.
- fix-3 done (D302): TVTs in xsl:text stream (was a silent 0); xsl:iterate and
  xsl:for-each-group (adjacent/starting/ending) stream in about 11 MB;
  count(//x) below the root is XTSE3430 per 12.9.21; child paths from the root
  indexed (21.9 s -> 0.30 s); number-parser is not in F&O 4.0 (number-format
  is).
- fix-1 done (D300): CLI help examples; the record and map:keys-where reports
  follow the drafts (not bugs).
- Phase 6 (D238, `docs/tasks/phase-6.md`), packets that run here:

  - wave 1, together: 6.2 host shim (`bindings/host/`; JavaScript tests and
    sizes unchanged); 6.3 Android build (four ABIs at API 24, the two fallbacks
    in `src/allocator.c` and `src/processor.c`, presets,
    `tools/fetch-binding-toolchains.sh`);
  - wave 2, after 6.2 and 6.3: 6.4 Kotlin core (JNI, `kotlin/jvm`) and 6.7
    Swift core (`swift/linux`);
  - wave 3: 6.5 Kotlin full, after 6.4; 6.8 Swift full, after 6.7;
  - wave 4: 6.6 Android AAR (assemble only), after 6.3 and 6.5;
  - wave 5: 6.10 the device CI workflow (written, disabled) and 6.11 the
    bindings' documentation.

  Owner: 6.9 Apple builds and iOS tests; the device runs of 6.6 and 6.10.
- More XPath/XQuery and XSLT sweeps on the leftovers below; perf-4 on casts and
  path-MAP hoisting.

## Owner's machine (Phase 6)

- 6.9: the XCFramework (`cmake/apple.cmake`, `tools/build-xcframework.sh`,
  `libtool -static`), a `binaryTarget` for `CSxmlHost`, `swift test
  --sanitize=address --disable-xctest` on macOS, `xcodebuild test` on an iOS
  simulator and a device, and `Bundle.module` finding `allkeys.txt` (D248).
- Android emulator and device runs (6.6, D238):
  `tools/fetch-binding-toolchains.sh ndk`, `tools/build-android.sh`, then
  connectedAndroidTest on x86_64 API 24 and 35 and one arm64 device (D251).
  Owner choices: which host libraries sxml-jvm ships (proposed linux-x86_64 and
  macOS arm64); keep the 32-bit ABIs or not.

## Leftovers no packet owns yet

Kotlin (6.4/6.5 done, D247, D250): 65 tests on the JVM with -Xcheck:jni and 0
leaks; the host shim now refuses U+0000, U+FFFE, U+FFFF. 6.6 (AAR) after 6.3;
Kotlin's SchemaException subclasses SxmlException, Swift's SchemaError does not
(6.12 to compare).

Swift (6.7/6.8 done, D241, D248): 55 tests on Linux; the bundled DUCET is a
copy of third_party/unicode-collation/allkeys.txt that must be updated with it.

XPath and XQuery (2.28, D287): syntax errors reported at the inner ERROR node
(string constructors no longer 1:1); run-time closures carry module and focus
(one mechanism for XQuery and XSLT); D154's limit lifted (a fresh global table
after the evaluation ends); focus-free defaults computed once; item types
across module cycles. XQ40 46,154 -> 46,158 (74 failed). What is left is kept
on purpose (D286) or singletons.

XPath and XQuery (2.27, D286): XInclude for fn:doc, fn:parse-xml and
fn:document (no xpointer); Japanese ideographic numerals; element() no longer
selects JNodes. XP40 32,809 -> 32,815 (26 failed), XQ40 46,138 -> 46,154 (78
failed). Left: xqhof18/19 (run-time closures carrying module and focus),
modules-43 (types imported in a cycle), function-decl-40-060,
record-003/-005/-016 (draft rejects surplus entries), fn-string-length-26
(contradicts XDM 7.5.2.4), 4 XP40 cases relying on XQuery-only character
references.

XPath and XQuery (2.26, D280): XP40 32,797 -> 32,809 (31 failed), XQ40 46,101
-> 46,138 (90 failed). A resolver may answer a module import with a SEQUENCE of
locations (processor.h, xquery.h contract widened). D197 stands (xs:numeric
under 1.0 compatibility: the draft limits fn:number conversion to xs:double).
Left: xqhof18/19 (run-time function-lookup items' static context), eqname-007,
modules-43. Cheap not-run features: fn:doc use-xsi-schema-location (to 5.16),
CJK numerals, XInclude.

XPath and XQuery (2.25, D275): XP40 32,793 -> 32,797, XQ40 46,070 -> 46,101
passed (107 failed); XT40 +13 (`//` lowering, inline functions read globals at
call time, per-element default collation). XML 1.1 query text (processor.h
`reads_query_text_as_xml_1_1`, additive; the QT4 driver sets it, the CLI keeps
1.0). Left: Q{}N name tests on JNodes, named item type constructors and
function items via fn:function-lookup (xqhof18/19), eqname-007, two modules of
one namespace without a location hint, serialization singletons, IDREF at an
element validation root (kept as cvc-id).

XPath and XQuery (2.24, D263): XP40 32,716 -> 32,731, XQ40 46,017 -> 46,070
passed. tree.h gained the optional set_element_identity operation (additive,
D99). Static typing: XQuery 4.0 dropped the feature (PR 1344); about
4,000-6,000 lines for 3.1's rules; default (D208): not done. Left: XQuery's XML
1.1 lexing (13), whitespace-only assert-xml, Q{}N name tests on JNodes, named
item type constructors found by fn:function-lookup at run time, xml:id under
lax validation, IDREF checks at a validation root, serialization singletons.

Older (after 2.23, D258; XP40 32,716, XQ40 46,017 passed):

- XSLT's own expressions have no Full Text (they use grammars/xpath; D235).
- NCName vs `Q{}N` name tests (sxml_name_test is frozen).
- A library module's context value type checked against the context value
  (XQuery 4.0, 5.17; contextDecl-050/051/064/066).
- fn:string-length and fn:normalize-space: an explicit argument is atomized,
  the default is fn:string(.) (string-length-23, normalize-space-24).
- Casts to xs:QName with an enclosing direct constructor's namespaces (CAST
  uses the unit's static namespaces, D84; 4 cases); `$e:code` with xmlns:e on
  the same element (try-catch-err-code-variable-13).
- is-id of list and union ID types (fn-element-with-id-4/5); fn:xsd-validator's
  use-xsi-schema-location (FODC0009).
- The suite's `recordTypeDecl/` module paths (the directory is
  `RecordTypeDecl/`; 11 cases, a suite error; the owner reports it upstream).
- fn:serialize canonical-JSON cases (27) need `bin:unpack-double` (7.8).
- A processor keeps every imported schema until it is destroyed (D174).

XSLT (after 4.20, D230; XT40 11,912): merge 107/120, error 474/580, validation
51/68, namespace 217/224, normalize-unicode 18/18. Left: XTDE3490; merge-097
(Saxon collection URI); xml-version-007 (value-template grammar rejects XML 1.1
controls); namespace-2606/-6202; docbook-004; fn-transform-27/82e/94; xsl:copy
of JNodes (needs an xdm.h addition); XTDE0430/0440; 4.22 (D252): the newest
drafts (13 Oct 2026, qtspecs 72f2229) and xslt40-test (91328fb) are what sxml
already had; 57 suite cases contradict the draft (53 "~T" type patterns that PR
2911 removed, 4 priorities, match-080/239) and the draft wins (D245; the owner
confirmed on 2026-10-20: no "~T" extension). typed="~T" on xsl:mode added.
Refresh both together when qtspecs or the suite move. 4.21 (D234): indexed
FILTERs are not hoisted from; current-merge-group(()) per 15.5.1; globals
computed on first read (GLOBAL_READ); typed modes and XTSE3105; XT40 11,943 at
its last run. Left: document-4066 (fn:document's schema validation does not
raise FODC0002); a global's side effects now happen at its first read.

XSLT (4.32, D296): XT40 12,273 -> 12,283 passed, 235 failed. XTSE3155,
fn:filter's streamability, fn:snapshot keeps base URI and unparsed entities,
document-node(element(N, T)) tests the annotation, input-type-annotations after
validation. The newest draft moves streamability into a separate XSLT Streaming
4.0 specification. Of 54 streaming failures: 9 fixed, 37 decided by the draft
or the suite, 8 left (accumulator-061, capture-206, six merge cases). Streamed
snapshot over 13 MB: 8.5 MB resident.

XSLT (4.31, D291): XT40 12,268 -> 12,273 passed, 245 failed. copy-4308,
map-for-key-005, accumulators and fn:function-lookup per package, document-2401
(strip-space per package; one document per URI and preparation). Namespace
undeclarations need no tree.h change (D69 already stores them; namespace-2606's
expected file is indented, a suite issue). Left: function-lookup-005 (the
packages' use relation), import-schema-216 (schema roles), merge-when,
streaming (4.32).

XSLT (4.30, D289): XT40 12,261 -> 12,268 passed, 250 failed; key-004 lost to
the draft (20.2.2 note: keys compare by fn:atomic-equal). split-when via
fn:partition; one XSLT closure mechanism; new-each-time="no" cached; adaptive
character maps. Left: map-for-key-005 (to 2.28), copy-4308, function-lookup and
accumulators per package, per-package strip-space for fn:document, schema roles
kept apart (import-schema-216), merge-when. Streaming: about 20 single causes
(~a week). Namespace undeclarations: an additive tree.h binding with an empty
URI (4.31).

XSLT (4.29, D281): XT40 12,250 -> 12,261 passed (99 wrong error), 257 failed.
Of what is left: 112 the draft decides, 42 suite or environment, about 130
failed and 72 wrong error real gaps. Small: xsl:evaluate codes (12),
package/override static codes (~20), serialization (~10), map-for-key with
empty $top, copy-4308. Medium: schema-aware validation codes (~20), record
coercion (12, to 2.27), D274's document items, new-each-time="no",
split-when/merge-when, function-lookup visibility per package. Large: streaming
(~20), namespace undeclarations in the tree (namespace-2606). self::element()
selecting JNodes to 2.27.

XSLT (4.28, D276): XT40 12,239 -> 12,250 passed, 268 failed. XSLT instruction
errors carry module and line (D271's XTTE gap closed). Left: FORG0001 in
parameter/variable coercion casts (4), XTTE0945 when the focus is absent only
at run time (2), an attribute set inside xsl:for-each of a rule still sees the
rule; xs:numeric under 1.0 compatibility given to 2.26 (check against D197).
About 17 cases the draft decides.

XSLT (4.27, D274): XT40 12,210 -> 12,226 by its own changes; 12,239 passed, 279
failed at 92a8a8c with 2.25's first commits. Left: xsl:next-match in attribute
sets (2), per-package strip-space for fn:document (document-2401), fragment
identifiers in xsl:source-document (docbook-003/004), strip-space "none"
returning XSLT's stripped copy of an available document, XTTE line numbers
(D271), namespace-2606. Draft wins: key-070, error-0150b/c/e, iterate-006,
maps-910, iterate-024.

XSLT (4.26, D273): XT40 12,191 -> 12,209 passed, 309 failed. Given to 2.25:
`//` lowered right-nested in xpath_lower.c gives duplicate nodes (5 cases),
XSLT inline functions capture globals by value (2). Left: CDATA text merging
with neighbours (4), a fixed invalid start-at as static XTSE0020 (2), key
patterns matching namespace nodes, fn:document with strip-space "none".
fn:document's trust-external default false only with an options map (6 older
cases need external DTDs).

XSLT (4.25, D262): XT40 12,157 -> 12,187 passed. Left: stream-available-
004/005 (a streamed open of the resource); choose-0106, type-0150 and
collations-0121..0124 (casts and comparisons read the function's module, not
the inner element's static context; 2.24's add_cast_namespaces covers XQuery
only); fn:load-xquery-module in XSLT would link XQuery into XSLT builds (D189);
D260's single cases. Kept as the draft decides: FORG0001 in coercions, no
XTTE0600, sequence JNode copies, format-number under 1.0 compatibility, XSD 1.1
regex syntax, XPDY0002 for unparsed-entity-\*.

size-3 (D272): fn:load-xquery-module in XPath: XP40 0 -> 56 of 60 run (021,
040, 051, 067 fail); XT40 load-xquery-module-001 raises FOQM0002 per F&O 4.0
where the suite expects XQST0059 or FOQM0006.

XSLT prefixes (4.24, D261): a run-time cast of a string to xs:QName still falls
back on XPath's predeclared prefixes in every language (the evaluator does not
know the language).

XSLT backwards compatibility (7.4, D197): backwards_compatibility 111 of 124;
QT4 xpath-1.0-compatibility 1 of 5. 14 cases expect fn:number-style conversion
for xs:numeric parameters, which XPath 4.0 §3.4.1 restricts to xs:double (D208:
follow the spec). 7.4b (D206): exsl:node-set, exsl:object-type, exsl:document;
docbook-001/002 pass; docbook-004 fails with FODC0002 in xsl:source-document.

xsl:evaluate (7.3, D196: 73 of 83, gate 90 %): 3 need parse-html (7.10), 2 need
json-doc/unparsed-text-available trust checks, 1 a UCA strength (7.15); keyword
arguments to stylesheet functions from the target expression.
disable-output-escaping (7.5, D188): 25 of 25 run.

HTML (7.10a/b done, D201, D218): misc-HtmlTestSuite 1,378 of 1,379,
fn-parse-html 22/22, fn-html-doc 9/10; html5lib tree construction 1,749 of
1,753, tokenizer 7,045 of 7,045, encoding 82 of 82; all WHATWG multi-byte
decoders; XHTML input. Left: source locations (the processor builds every tree
without them), a prefixed XHTML root, XHTML that needs an external DTD.

XML 1.1 (after 7.13, D195; D275): query text by XML 1.1 rules is a processor
option; XT40 namespace-2606/-6202 still fail in `src/xslt*`.

XSD (5.13 sweep 3, D277): in scope 41,486 -> 41,507 of 41,518. The 11 left: XSD
1.0 rules (3), circular attribute groups the 1.1 Rec allows (1), xs:NOTATION
used directly (3, w3c/xsdtests issue 12), instance tests of indeterminate
schemas (4). Validation continues after a rejected element (content recovery).
Not run: 92 introspection tests (all pass with xlink.xsd), 57 "queried", 50
version 1.0. The library does not follow schema location hints (5.16). 5.16
(D278): schema location hints in the library (schema_location_hint_node,
additive in schema.h), fn:xsd-validator and fn:doc/parse-xml/document's
use-xsi-schema-location, `sxml validate --use-schema-location`; xlink.xsd
vendored (third_party/w3c-xlink): XSD 41,507 -> 41,599 passed, 92 of 92
introspection tests. XQuery validate and XSLT validation attributes still
ignore hints (their specs allow it).

Duplicates merged in 7.20 (D249): one decoder with an error rule, Full Text on
the shared feature-data path, sxml_allocator_reserve_array, and D182's five
cheap ones. Left: about 30 private array growers, XSLT's yes/no reader, a
second UTF-8 encoder, cldr's byte_text. (Earlier, D182: the schema reader's own
boolean and integer parsers, Gregorian day arithmetic in four places, three
ways of building compile-report text, four text writers, three private
heapsorts in serialization.

Performance (perf-1, D224): benchmark set in tests/bench; itunes 80.6 s -> 3.4
s, XMark-All 6.73 -> 1.52 s; filter indexes for =/eq/is joins (also
match="$var"); one-pass axis walks; loop-invariant hoisting. perf-2 (D229):
XMark-All 1.55 -> 1.20 s, match="$var" 1.02 -> 0.45 s, numeric joins 2x; fast
number casts, leaves computed in place, inline small lists, numeric index keys,
quantifier hoisting. perf-3 (D232): XML parsing 15 -> 25 MB/s (tree 13 -> 21
MB/s), XMark-All -10 %; tags and plain attributes are single tokens. Left:
casts cost about 1,100 instructions; hoisting of path MAPs and sort keys; the
task struct copied on each push.

Distribution (dist-1, D256): `mise run dist` builds the CLI for x86_64 and
aarch64 Linux (static musl), macOS and Windows, and wasm32-wasi, as archives
with SHA256SUMS. Ran here: both Linux (aarch64 under qemu), x86_64 Windows
(wine), WASI. Owner: run the macOS and aarch64 Windows programs, signing and
notarization, publishing. Windows has no /usr/share/zoneinfo, so the CLI there
needs embedded or shipped tz data.

The book (book-1, D257): `docs/book/`, every chapter written; Getting Started,
XPath, XQuery and XSLT are complete, the others shorter.
`tools/check-book-examples.sh` re-runs its examples; a change to CLI output the
book shows updates the book. Left: chapters for a published npm package and
release archives once they exist, more on XSLT 4.0's new instructions, and
schema-aware XSLT output validation.

perf-4 (D259): the book's join 12.9 s -> 0.045 s (descendant pass, the filter
index's number view, probe order); early exit through item demands and
quantifier fusion (10^7 items: seconds -> 4 ms); default step bound 10^8 (about
8 s). Left: two filters in a row index only the first; FILTER/MAP over MAP not
fused; demands do not enter function bodies; if/boolean read one extra item.

book-2 (D270): XQuery part 5 -> 11 sections, XSD part 2 -> 9; 387 examples
checked. cli-2 (D271) fixed its four CLI bugs (collection -> FODC0002,
library-module error file names, undeclared xsi:type prefixes -> cvc-elt.4.1
with defaults read in the schema's namespaces, fn:trace and fn:message to
standard error) and gave --context-item documents a document URI. Left: XTTE
errors from XSLT validation have no line number (src/xslt_lower\*.c gives no
source span); content after a cvc-complex-type.2.4 rejection is not checked;
the CLI ignores xsi:schemaLocation.

book-3 (D282): Chapter 12, a code-generation project (XSD and an API
description to TypeScript with XQuery, OpenAPI with XSLT, run with Node.js).
Left: a syntax error inside a string constructor's enclosed expression is
reported at 1:1 of the module, not at its own place.

7.22 (D285): sxml:parse-code and sxml:code-languages (namespace urn:sxml;
name.h SXML_NAMESPACE_SXML and processor.h sxml_source_code_feature(),
additive); json, javascript (JSX), python, c, css, html, go vendored with
highlights; tree-sitter/suite 630 of 639 (corpus 452/452, highlights 178/187: 7
need locals, 2 injections). sxml.wasm 9,693,235 / 2,006,198 (budget 10,000,000
/ 2,800,000), CLI 11,289,043 / 2,288,933 (budget 11,500,000 / 2,500,000). Owner
(2026-10-20): TypeScript/TSX and Markdown built in, a registration API,
run-time language files (7.23). 7.23 (D288): typescript, tsx, markdown,
markdown_inline built in; locals and injections (8 deep); public
sxml_source_code_language and source_code_languages (tree-sitter ABI 13-15);
CLI --code-language=LIBRARY (dlopen/LoadLibraryW in the platform layer).
tree-sitter/suite 1,410 of 1,438 (highlights 187/187; 8 markdown_inline
extensions not in upstream's default parser, 1 upstream spec.txt error).
sxml.wasm 13,425,285 / 2,411,765 (budget 14,000,000), CLI 15,026,404
(15,500,000 / 2,900,000). Delayed: tree-sitter .wasm language files in
JavaScript (sxml.wasm cannot link modules at run time), the native wasm store
(wasmtime), the host shim and bindings (Phase 6 paused), injection.combined.

7.21 (D283): Earley with lookahead, Leo items, compiled classes, grammar
simplification. 1 MB: log 1.12 -> 0.18 s, csv 1.25 -> 0.28 s, calc 2.14 -> 0.53
s, markdown 3.21 -> 0.30 s, right recursion fails -> 0.30 s; linear (4x input,
3.8-4.2x time); memory 90-200 bytes per character (all sets kept). Ambiguous
inputs stay cubic. Suite 886 of 907 (21 not run: Unicode versions, C0
controls).

7.24 (D290): regular rules compiled to token automata (every end a completion;
ambiguity still counted); item sets only where items are; no tree nodes for
hidden parts; parse-only arrays freed before read-out. 1 MB log 0.196 s / 90 MB
-> 0.059 s / 21 MB; 100 MB log 6.2 s / 1.17 GB (was over the step limit).
Parser's own memory under 2 MB per MB; the rest is the XML tree. Left: the
ambiguous sum (3.1 s against D283's 2 s bound on this machine; owner's machine
to judge).

7.25 (D293): Sxml.loadCodeLanguage loads tree-sitter .wasm language files
(src/linker.ts reads dylink.0; sxml.wasm exports a growable table,
__stack_pointer and tree-sitter's 25 wasm-stdlib functions); createProcessor({
codeLanguages }). +7,713 bytes. Not supported: pre-2021 emscripten dylink,
files needing other side modules, scanner imports outside tree-sitter's list. A
loaded language's memory is never freed. The Swift initializer's new field is
not compiled here.

book-4 (D292): the codegen chapter builds a tree of the TypeScript program
(XQuery, a vocabulary of our own) and prints it with an XSLT printer (one rule
per element, layout in one place, a second style by import); check.xq parses
the printed code with sxml:parse-code. 46 examples match, tsc --strict passes.

js-2 (D294): serializeTo, XQuery.runTo, transform output / resultDocumentOutput
/ streamedSource, chunked sinks (textSink, writableStreamSink, nodeStreamSink,
outputReadableStream), chunk sources for doc($uri). 108.7 MB output: 749 MB
module / 113 MB JS heap -> 481 / 4 (the tree); streamed source 10.6 / 14.6 MB.
Processor.parse still takes whole text (no public incremental parse, D233).
Sinks cannot apply backpressure (synchronous evaluation). Swift memberwise call
not compiled here.

stream-1 (D295): long text runs grow by doubling (arena_tree.c); method=text
serialized while streamed; xml writes long text in 64 KiB parts. 20,000 x 1 KB
text: out of memory -> 11.0 MB, 0.55 s. A streamed element grants 500 steps;
createProcessor({ maximumEvaluationStepCount }). 93 MB streamed identity: CLI
23.2 s / 11 MB, Node 52.6 s / 10.7 MB. Left: no streamed writer for html (a
tree, now linear).

stream-2 (D297): html and xhtml serialized while streamed (indent=no): 29.8 ->
10.9 MB for 20,000 x 1 KB; indent=yes (XSLT's html default) and XHTML with
doctype-system/standalone/canonical keep the tree. cli-3 (D298):
fn:collection/fn:uri-collection over local directories (select, recurse,
on-error), --collection; XML documents only.

Platform and tooling:

- macOS, Windows and MSVC have never been built; `_getcwd` is untested.
- CI is disabled (D123).
- The QT4 driver's `list`/`run` still have their own selection walk;
  `selection.h` is shared by the XSLT and XSD drivers.

## Owner decisions open

Decided in D239: MIT license, npm `@tinyactors/sxml`, the move to the
tinyactors-dev organization (URLs change with the move), Kotlin
`dev.tinyactors.sxml`. Still the owner's: tagging and publishing releases, the
copyright holder's name if not "The sxml authors", and when to move the
repository. Everything else takes the proposed default (D208).
