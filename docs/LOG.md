# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

- js-2 done (D294): streamed output and input in the JavaScript API.
  The host shim gains an output writer (chunks of 64 KiB from a reused
  buffer, result documents between open and close events) and byte
  readers; the package gains serializeTo, runTo, TransformOptions.output,
  resultDocumentOutput and streamedSource, chunk sources from the
  resolver, and textSink, writableStreamSink, nodeStreamSink and
  outputReadableStream. 108.7 MB of output: module memory 749 MB to
  481 MB and JavaScript heap 113 MB to 4 MB with a tree; with a
  streamed source and a streamable mode 387 MB to 11 MB. New
  javascript/streams (19 cases, a 100 MB output under 64 MB). Found: a
  streamed source with method text grows memory quadratically.

- book-4 done (D292): the codegen chapter on trees. XQuery builds a
  tree of the TypeScript program in a small vocabulary of our own
  (generated/program.xml), an XSLT printer (print.xsl) writes the
  files, one template rule per element and the layout in one place;
  braces.xsl prints the same tree in a second style; check.xq parses
  the printed code with sxml:parse-code and checks client, server and
  runtime against each other. New section codegen-printer.md. 46
  examples match; tsc --strict passes. No library bug found.

- 7.25 done (D293): tree-sitter's WebAssembly language files in the
  JavaScript package. Sxml.loadCodeLanguage links a file as `tree-sitter
  build --wasm` writes it (a dylink.0 side module) into the running
  sxml.wasm, which now exports its growable function table, its stack
  pointer and tree-sitter's 25 scanner C functions; src/linker.ts reads
  dylink.0, reserves memory with sxml's malloc and table slots, runs the
  relocations and registers the language with
  createProcessor({ codeLanguages }). javascript/code_languages 12/12
  (JSON, Python and CSS built from the vendored grammars, no network;
  loaded Python and CSS equal the built-in ones); the JavaScript chapter
  5/5. sxml.wasm 13,448,586 bytes (+7,713 code and data; budget 14 MB).

- 7.24 done (D290): Invisible XML for large inputs. Regular
  nonterminals whose XML is their text are compiled once into automata
  that count paths (ambiguity kept) and read in one step, every end a
  completion; item sets only where items are, origins set numbers,
  positions UTF-8 byte offsets (no codepoint array); the parse tree
  without hidden nodes, with text runs. Log 1 MB 0.20 s / 90 MB ->
  0.06 s / 21 MB (84 -> 10 MB per further MB); 10 MB 2.0 s / 831 MB ->
  0.6 s / 124 MB; 100 MB 6.2 s / 1.17 GB within the step limit; csv,
  calc, markdown, right recursion 1.9 to 3.4 times less memory. iXML
  suite 886/907, QT4 fn-invisible-xml 16/16 unchanged; fuzz 600 s clean.
  The ambiguous sum misses D283's 2 s here before and after (3.7, 3.1 s).

- report-1 done: docs/CONFORMANCE.md regenerated from whole runs at
  e74d4ad (release, GCC). XP40 32,815 of 32,841 run, XQ40 46,158 of
  46,232 (74 failed), XT40 12,268 of 12,518 (250 failed), XSD11 41,599
  of 41,610 (11 failed), torture slice 198/198, 0 errors in each; iXML
  886 of 907, tree-sitter 1,410 of 1,438 (1 error: a corpus test with no
  divider). Equal to the QUEUE counts. The book's appendix and README
  figures updated. c982bf6 did not build with GCC (-Wshadow in
  src/source_code.c), fixed by 7.23's c1a88af.

- 7.23 done (D288): more source code languages. TypeScript, TSX,
  Markdown and its inline grammar built in; the locals query (a
  reference takes its definition's highlight) and the injections query
  (Markdown code blocks and paragraphs, HTML script and style parsed with
  their languages, an explicit stack of trees); a public registration
  API (sxml_source_code_language, source_code_languages,
  sxml_source_code_check_language, ABI 13 to 15); `--code-language` loads
  a grammar's shared library in the CLI. tree-sitter/suite 630/639 to
  1,410/1,438, highlights 187/187. sxml.wasm 13.4 MB (budget 14 MB).
  Delayed: tree-sitter .wasm language files in the JavaScript package
  (sxml.wasm is no PIC main module), the native wasm store (needs a JIT
  runtime), the host shim.

- 2.28 done (D287): XPath/XQuery leftovers. Syntax errors at the token
  tree-sitter could not take (a string constructor's `{ }` was 1:1);
  run-time closures for context-dependent functions (xqhof18/19);
  function items called after their evaluation read fresh globals
  (D154's limit); focus-free defaults computed once (function-decl-
  40-060); an item type round for cyclic imports (modules-43); the
  "context node, if any" default for 4.30's map-for-key-005. XQ40
  46,154 to 46,158 passed, XP40 32,815 and XT40 12,268 unchanged, none
  lost. A fresh HEAD run found no new cluster.

- 7.21 done (D283): fast Invisible XML. Book first (part "Parsing Text
  with Invisible XML", five chapters, 52 examples, all match), then
  tests/bench/run-ixml-bench.py whose --check holds the book's numbers,
  then the parser: lookahead sets, Leo items, codepoint classes, grammar
  simplification, items and nodes in blocks, errors that say where.
  1 MB inputs 0.18 to 0.53 s and 91 to 204 MB (were 1.1 to 3.2 s and
  372 to 1,460 MB); right recursion linear (was quadratic, failing at
  250 KB). iXML suite 886/907 and QT4 iXML sets unchanged.

- 4.30 done (D289): XSLT real gaps, XT40 12,261 to 12,268 passed (99
  wrong error), 257 to 250 failed, 0 errors. split-when through
  fn:partition and one closure mechanism shared with xsl:iterate;
  new-each-time="no" cached; keys compared by fn:atomic-equal (key-004
  lost by the draft's 20.2.2 note); adaptive character maps; XTSE4045,
  XTSE3070 for new-each-time, XTSE0165 for an included package;
  fn:document validates before stripping. Left with sizes (D289):
  map-for-key-005 (xpath_lower.c), copy-4308, function-lookup and
  accumulators per package, per-package strip-space, schema roles,
  streaming and namespace undeclarations (sketches in D289).

- 7.22 done (D285): source code as XML. sxml:parse-code($source,
  $language, $options) and sxml:code-languages() (namespace urn:sxml,
  feature "source-code", every build): seven vendored tree-sitter
  grammars (JSON, JavaScript with JSX, Python, C, CSS, HTML, Go) turn
  code into a document of its syntax tree, with the captures of each
  grammar's highlights query as highlight attributes and optional
  line/column positions; string() of the document is the source. Book
  first (Chapter "Source Code as XML", 18 examples, all match), then the
  grammars' tests (tree-sitter/suite: corpus 452 of 452, highlights 178
  of 187; 9 need locals or injections), then the implementation.
  sxml.wasm 9,693,235 / 2,006,198 (+1,990,621 since its baseline,
  budget 10,000,000 / 2,800,000); CLI module 11,289,043 / 2,288,933
  (budget 11,500,000 / 2,500,000). QT4/XT40/XSD not rerun (no change to
  their paths).

- 2.27 done (D286): XPath/XQuery sweep. XInclude for fn:doc,
  fn:doc-available, fn:parse-xml and fn:document (one loop with an
  explicit stack, inclusion loops, xml:base and xml:lang fixup, text
  includes, fallbacks; FODC0016 without trust-external; no xpointer);
  the Japanese ideographic numerals (U+4E00) of fn:format-integer;
  element() selects no JNode; fixed default element namespaces;
  format-number's decimal format name with a constructor's namespaces
  (eqname-007); XQST0034 for schema constructors; strip-space "all"
  after validation; build-dateTime from nodes; collection documents
  through fn:doc; xml-to-json's top escaped-key. XP40 32,809 to 32,815
  passed (31 to 26 failed; this packet +4, 5.16 +2), XQ40 46,138 to
  46,154 (90 to 78; this packet +12), XT40 12,261 unchanged, no case
  lost. Left: xqhof18/19, modules-43, function-decl-40-060, D281's
  record cases (the draft disagrees with the tests).

- book-3 done (D282): Chapter 12 of the book, "Project: Generating Code
  from a Schema", four sections. An XSD schema and an API description
  (validated with sxml validate) become TypeScript types with runtime
  checks from the facets, a fetch client and a Node.js handler (XQuery
  string constructors, fn:put), and an OpenAPI 3.1 document (XSLT);
  the generated code runs with Node.js, and a diff shows whether the
  kept files are current. 30 examples, all match. No library change.

- 5.16 done (D278): schema location hints. schema.h gains
  `sxml_schema_configuration.schema_location_hint_node` (additive):
  an instance's xsi:schemaLocation and xsi:noNamespaceSchemaLocation
  hints join the assembly through the resolver, only for namespaces the
  schema lacks. fn:xsd-validator, fn:doc, fn:doc-available,
  fn:parse-xml and fn:document honour use-xsi-schema-location (no more
  FODC0009); `sxml validate --use-schema-location` (book and README
  updated); xlink.xsd vendored, the 92 introspection tests run and
  pass. XSD11 41,507 to 41,599 passed (11 failed, unchanged); QT4
  schema-aware XQ40 11,691 to 11,696; XT40 schema-aware 4,084 to 4,087;
  none lost. CONFORMANCE.md not regenerated.

- 4.29 done (D281): XSLT sweep, XT40 12,250 to 12,261 passed (104 to
  99 wrong error), 268 to 257 failed, 0 errors, none lost. A failed
  cast in an XSLT binding's coercion is its type error (XTTE0570,
  XTTE0590, XTTE0505; a new IR coercion role); xsl:iterate in a global
  reads globals when it runs; validated copy-accumulators copies;
  run-time XTTE0945 in globals; fn:xml-to-json indent; deep-copy keeps
  types; fixed-namespaces document URIs; function-available of the
  grouping functions; simple content keeps whitespace. Left, with
  sizes: D281. CONFORMANCE.md not regenerated.

- 2.26 done (D280): XPath/XQuery sweep. XP40 32,797 to 32,809 passed
  (36 to 31 failed, 14,480 to 14,473 not run), XQ40 46,101 to 46,138
  (107 to 90 failed, 1,105 to 1,085 not run; xml-to-json-057 is
  4.29's); XT40 unchanged by it; no case lost. Q{}N and p:N select
  QName keys of JNodes (an IR step flag; sxml_name_test stays frozen);
  an import without hints loads every module a resolver lists
  (SEQUENCE answer, processor.h and xquery.h, additive); fn:function-lookup
  finds named atomic types' constructors; deep-equal and collation
  options by the option conventions; fn:slice at the int64 limits;
  fn:op under the default collation; XQST0134; adaptive INF/NaN as
  fn:string (book example updated); the driver runs moduleImport and
  six format-integer sequences, assert-eq takes [42]. D276's xs:numeric
  in 1.0 mode not done: the draft keeps xs:double only (D197). Left:
  xqhof18, eqname-007, modules-43; cheap not-run features in D280.
  The assert-eq commit's message counts array-for-each-pair-420, which
  fn:op's commit fixed.

- 5.13 sweep 3 done (D277): XSD11 41,486 to 41,507 of 41,518 in scope
  (32 to 11 failed), none lost. XSD 1.1 rules added: src-attribute.5,
  src-redefine.1, simple attribute types, required uses kept by
  restrictions, simple content restricting xs:anySimpleType, fixed values
  compared as values, Element Declarations Consistent with wildcards,
  wildcard unions in restrictions, xml:lang. The 11 left are XSD 1.0
  rules or disputed expectations. After a cvc-complex-type.2.4
  rejection the validator recovers and checks the rest of the content
  (book and CLI README updated). Schema-aware QT4 (XQ40) and XT40 sets
  unchanged. Not added: xsi:schemaLocation in sxml validate (the
  library has no hints). The introspection set passes with a local
  xlink.xsd.

- 4.28 done (D276): XSLT sweep, XT40 12,239 to 12,250 passed (107 to
  104 wrong error), 279 to 268 failed, 0 errors, none lost. Errors an
  instruction raises name its module and line ($err:line-number, CLI
  "file.xsl:L:C"; book and CLI tests updated); xsl:next-match in
  attribute sets; XTTE0600 for explicit parameter defaults, XTSE3520;
  XTSE3088, XTSE1210, XTSE0500, XTSE0085, xsl:select's fallback order.
  Found for the XPath sweep: 1.0 compatibility treats xs:numeric
  parameters as xs:double (8 cases). CONFORMANCE.md not regenerated.

- 2.25 done (D275): XPath/XQuery sweep. XP40 32,793 to 32,797 passed
  (40 to 36 failed), XQ40 46,070 to 46,101 (138 to 107 failed, 49 to
  44 wrong errors), XT40 +13 from this packet; no case lost. "//E1/E2"
  is ((//E1)/E2): later steps no longer run once per descendant (4.26's
  find). XSLT inline functions read globals when called; comparisons
  use their own element's default collation; a library module has its
  own static context (decimal formats, collation, schemas, namespaces);
  load-xquery-module binds external variables only, checks
  vendor-options and refuses untrusted content's imports;
  context-dependent references keep their module; lax xml:id is an ID;
  XML 1.1 query text as a processor option (processor.h, additive),
  which the QT4 driver sets for xml-version 1.1 tests. Left: D275's
  "Not done".

- 4.27 done (D274): XSLT sweep, XT40 12,210 to 12,226 passed (115 to
  107 wrong error), 308 to 292 failed, 0 errors, none lost. CDATA
  requests make text nodes of their own in final output (D188's way),
  adjacent CDATA sections join; static XTSE0020 for fixed start-at,
  non-top-level static and item types with "?"; XTSE1220, XTSE0620,
  XTSE3022 orders; keys over namespace nodes; strip-space "none" skips
  xsl:strip-space; XTTE0950 (XQTY0086's check) and fn:copy-of
  preserving types; XML 1.1 characters in value templates;
  Q{uri}prefix:local names. CONFORMANCE.md not regenerated.

- 4.26 done (D273): XSLT sweep, XT40 12,191 to 12,209 passed (118
  wrong error), 327 to 309 failed, 0 errors. fn:stream-available by a
  streamed open of the document's head; fn:document's options map as
  fn:doc's (3.0 forms keep reading external DTDs, a recorded
  deviation); untrusted fn:unparsed-text-available and fn:json-doc;
  globals in scope in their own declarations; the XSLT driver builds a
  tree unless tree="no"; FODC0002 names the URI (CLI and book);
  library packages' xsl:global-context-item; resolve-uri and "##".
  Found for 2.25: "//" lowered right-nested (E//S/T/f() repeats S/T's
  nodes). CONFORMANCE.md not regenerated (another packet's changes).

- 2.24 done (D263): XPath/XQuery sweep. XP40 32,716 to 32,731 passed
  (55 to 40 failed, 32 to 30 wrong errors), XQ40 46,017 to 46,070 (191
  to 138, 53 to 49), about eight of them from 4.25, 4.26 and cli-2; no
  case lost. Library context value types; fn:string(.) defaults of
  string-length and normalize-space; QName casts with the namespaces of
  their place (direct constructors, XSLT elements); is-id of list and
  union elements through an additive optional tree builder operation
  (tree.h); nested direct constructors keep only their own namespaces;
  variable cycles found when they run; named item type, EQName and
  function type checks. Static typing (dropped by XQuery 4.0) estimated
  at two or three packets, proposed not to do. CONFORMANCE.md not
  regenerated (another packet's uncommitted changes). Left: D263's
  "Not done".

- size-3 done (D272): every build has everything. fn:load-xquery-module
  is a library part for every language, so XSLT and XPath have it (QT4
  XP40 fn-load-xquery-module 0 to 56, fn-function-lookup 657 to 661;
  XT40 load-xquery-module wrong errors 4 to 1). JavaScript ships one
  module, sxml.wasm, 7,701,002 bytes (the full module's size; budget
  8,000,000 kept); ./sxml-full.wasm is an alias. The CLI and sxml_host
  already linked every feature. BRIEF's size rule rewritten.

- cli-2 done (D271): the CLI bugs book-2 found. A relative
  collection() raises FODC0002 (the resolver gave a file's bytes for a
  collection: exit 4); a dynamic error in a library module names the
  module's file (the query was released before the report); xsi:type
  and QName values no longer resolve an undeclared xs: (cvc-elt.4.1;
  defaults read in the schema's namespaces); fn:trace and fn:message
  go to standard error in xpath, xquery and transform; the
  --context-item document has a document-uri. XSD11 41,486 unchanged,
  XQ40 45,996 to 46,013 (one from this packet), XT40 12,191; book
  397 examples match. Left: XTTE errors of XSLT validation have no
  line (the lowering's spans).

- 4.25 done (D262): XSLT sweep, XT40 12,157 to 12,187 passed (120 to
  118 wrong error), 361 to 331 failed, 0 errors, none lost. Captured
  groups and the cleared output URI pass through template calls (one
  mechanism: the tunnel parameters as received context variables);
  fn:apply-templates; XTDE1270 for fn:key; ##any as default element
  namespace in XPath prologs and xpath-default-namespace; run-time
  QName casts in XSLT without the predeclared prefixes (D261's
  leftover); name checks of system-property and the *-available
  functions; XTDE0030 for computed boolean attributes; override
  checks of modes. Recorded as the draft's: FORG0001 in coercions,
  XTTE0600, Sequence JNode copies, XPath 1.0 numbers. CONFORMANCE.md
  not regenerated (another packet's uncommitted changes).

- book-2 done (D270): the book's XQuery part grows from 5 to 11
  sections (grouping and windows, types and errors, inputs and
  external variables, serialization, XQuery 4.0, a report project)
  and the XSD part from 2 to 9 (simple and complex types, derivation,
  namespaces, keys, XSD 1.1, reading errors, deeper schema-aware
  XQuery and XSLT). Every example checked against a release build.
  Found, not fixed (QUEUE): relative collection() exits 4 with a
  status name; dynamic errors in library modules carry the main
  module's file name; xsi:type resolves an undeclared xs prefix; no
  fn:trace output in the CLI.

- perf-4 done (D259): the three things the book found. The book's join
  (2,000 orders) 12.9 to 0.045 s: "E//T[P]" becomes descendant::T[P]
  (the descendant pass), untyped keys answer numeric probes (the number
  view), probes that are no constant are indexed first. Early exits:
  item demands (the demand pass) stop MAPs, FILTERs, axis steps and
  traversals after the items exists, empty, head, boolean, `[k]` read;
  some/every run over the input of a filter or loop they read (the
  fusion pass); 10^7 items 2 to 6 s to 0.004 s. The default step bound
  is 10^8 (8 s, was 75 s; 7 to 21 million steps a second); drivers keep
  their bounds. xmark-all +1 % instructions; XQ40 45,964 and XT40 12,036
  passed, 0 outcomes changed. Book: performance, C (195 allocations),
  limits appendix.

- 4.24 done (D261): XSLT stylesheets no longer see XPath's predeclared
  prefixes; as XSLT 4.0 5.3.1 says, a stylesheet knows its applicable
  static namespaces (native or fixed-namespaces) and xml. A behaviour
  change for users: undeclared xs:, map:, math: in a stylesheet raise
  XPST0081 (expressions, as types) or XTSE0280 (names). The book's
  page.xsl and customers.xsl, five unit-test files and the bindings'
  tests declare theirs; CHANGELOG says so. XT40 12,156 to 12,157 passed
  (namespace-6202), 361 failed, 0 errors, no case lost.

- 4.23 done (D260): an XSLT sweep by root-cause clusters. assert-xml
  ignores whitespace around a result document; enclosing modes (6.9.6);
  XTSE0545 for name lists and types; initial-mode eligibility; xsl:copy
  of JNodes (11.9.1.2) with keyed parentless roots; XTDE0430/0440/0835/
  0865; static variables' consistency (XTSE3450); fn:current-output-uri
  cleared in functions, globals and patterns, current-output-uri#0
  captured; fn:stream-available; xsl:sequence/xsl:select as=. XT40
  12,081 to 12,156 passed (+75, 120 wrong error), 362 failed, 0 errors,
  no case lost; CONFORMANCE XT40 section and README regenerated. Next:
  captured groups through templates, absent-focus codes, XSLT without
  XPath's predeclared prefixes (a decision).

- 2.23 done (D258): an XPath/XQuery sweep by root-cause clusters. JNodes
  in `{ E }` map constructors; the item axis's JNode with G as its parent
  (D113's deviation gone); fn:xsd-validator compiles its schema,
  schema-location and target-namespace options per call through the
  builtin-schemas feature; fn:function-lookup gives a declared function's
  `$a := .` its caller's focus; "||" over sequences; partial-application
  names and the dynamic arity check; deep-equal ignores declared
  functions' names; pure unions without facets; XQTY0086; pattern facets
  by XSD's canonical doubles; fn:resolve-uri's FORG0002. Release whole
  runs: XP40 32,689 to 32,716 passed (82 to 55 failed, wrong errors 34 to
  32), XQ40 45,969 to 46,017 (239 to 191, 55 to 53), none lost;
  CONFORMANCE.md's XP40 and XQ40 sections regenerated at 7849df6. Next:
  D258's "Not done" (QUEUE).

- book-1 done (D257): the sxml book in `docs/book/`, an mdBook of 35
  pages: Getting Started, XPath, XQuery and XSLT in full (five, four and
  four sections), XSD, other formats, text, files, C (five programs),
  JavaScript, performance, four appendices. 254 examples, all checked by
  `tools/check-book-examples.sh` against a release build of HEAD
  (`mise run book:check`); `mise run book` builds it. README links it.

- dist-1 done (D256): `mise run dist` builds the `sxml` command as
  seven archives (`build/dist/`, SHA256SUMS): x86_64/aarch64 Linux
  (static musl), macOS 11 and Windows (MinGW-w64, UCRT), cross-compiled
  with `zig cc` (`cmake/toolchains/zig.cmake`), and wasm32-wasi. All
  build with `-Werror` unchanged. Program sizes: x86_64-linux 9,820,840,
  aarch64-linux 9,326,752, x86_64-macos 9,710,431, aarch64-macos
  9,393,656, x86_64-windows 9,855,488, aarch64-windows 9,228,800,
  sxml.wasm 9,224,585; archives 1.96 to 2.54 MB. `mise run dist:verify`
  ran x86_64-linux, aarch64-linux (qemu-user), x86_64-windows (wine)
  and wasm32-wasi (Node.js). Owner: run the macOS and Windows-on-ARM
  programs, signing and notarization, publishing. README "Install".

- 6.3 paused (D251, D255): the Android build. `src/allocator.c` uses
  `posix_memalign` below API 28 and `src/processor.c` uses `clock_gettime`
  below API 29. There are four NDK presets (android-24, MinSizeRel),
  plus `tools/build-android.sh` and `tools/fetch-binding-toolchains.sh`
  (pinned, SHA-256). All four ABIs build every target with `-Werror`.
  `libsxml_kotlin.so` stripped / gzip -9: arm64-v8a 7,558,944 /
  1,859,972, armeabi-v7a 6,591,092 / 1,747,590, x86_64 7,789,056 /
  1,928,181, x86 7,599,348 / 1,980,055. LOAD 0x4000 on the 64-bit ABIs.
  No device or emulator here (no KVM): that is the owner's.
  `bindings/kotlin/DESIGN.md` is the resume plan for 6.3, 6.6, 6.10 and
  6.11.

- 4.22 done (D252): XSLT on the newest draft. Nothing moved: the XSLT,
  XPath, XQuery and F&O 4.0 editor's drafts are still those of 13 October
  2026 (qtspecs 72f2229) and xslt40-test's newest commit is the vendored
  91328fb. The draft's pattern grammar has no "~T", so the priority set's
  type patterns stay XTSE0340: 57 cases are class (b), the draft wins
  (53 "~T", 4 default priorities of 6.4.2/6.4.4/6.4.7, match-080 and
  -239). sxml's default priorities match the draft in every form checked.
  New: typed="~T" on xsl:mode (6.9.4, XTTE3100). XT40 11,951 + 130
  wrong error of 12,518 run, 0 errors (CONFORMANCE XT40 section
  regenerated). Next: refresh when qtspecs or xslt40-test move.

- 6.5 done (D250): the Kotlin full surface. `dev.tinyactors.sxml` gains
  `compileQuery` (`XQuery`, `UpdateSink`, `UpdatedTree`, `PutDocument`,
  `Message`), `compileStylesheet` (`Stylesheet.transform`, `InitialMode`,
  `ResultDelivery`, `TransformResult`) and `compileSchema`
  (`Schema.validate`, `ValidationResult`, `SchemaException`), over seven
  new JNI entry points and the host shim's side results. The DUCET is a
  class path resource copied by the build. The host shim refuses U+0000,
  U+FFFE and U+FFFF and checks UTF-8 in `sxml_host_string`. ctest
  `kotlin/jvm` (debug, ASan, -Xcheck:jni) passes 65 tests with 0 leaks
  (was 36), with LeakSanitizer now on (JVM frames suppressed); 25 by the
  names of `full.test.ts`. `bindings/kotlin/README.md` and the README's
  Kotlin section. Next: 6.6 (the AAR).

- 7.20 done (D249): a sweep of 7.11's open cases and the larger
  duplicates. XT40 11,945 to 11,951 passed (transform-009 and five
  result-document cases: the driver now checks assert-serialization
  inside assert-result-document); XP40, XQ40 and XSD unchanged case for
  case; 0 errors. transform-004 cannot pass under D71 (a static
  fn:transform would re-enter the compiler); transform-401 waits for
  4.22's "~T" patterns (D245). Merged: one decoder with an error rule
  (XML, text decoding, HTML), Full Text through the feature data path,
  `sxml_allocator_reserve_array`, the schema reader's boolean and digits
  through the cast engine, Gregorian cycle days in `calendar.c`, compile
  reports built in one place, one text form writer, the serialization
  heapsorts as `sxml_sort_in_place`. `sxml.wasm` 2,089,167 (guard +345).

- 6.8 done (D248): the Swift full surface. Module `Sxml` gains
  `compileQuery` (`XQuery`, `UpdateSink`, `UpdatedTree`, `PutDocument`,
  `Message`), `compileStylesheet` (`Stylesheet.transform`, `InitialMode`,
  `ResultDelivery`, `TransformResult`) and `compileSchema` (`Schema.validate`,
  `ValidationResult`, `SchemaError`), over the host shim's side results.
  `CollationData.bundled` serves the DUCET from a SwiftPM resource, a copy
  of `third_party/unicode-collation/allkeys.txt` with the same git blob
  (SwiftPM copies a symbolic link as a dangling link). ctest `swift/linux`
  (debug, ASan) passes 55 tests with 0 leaks (was 27): 24 by the names
  of `full.test.ts`, the full allocation-failure loop among them, an ARC
  test, and 3 resource data tests (DUCET, tzdata, CLDR). There is a Swift
  section in README.md, and a new `bindings/swift/README.md` with real
  output. Next: 6.9 on the owner's Mac.

- 6.4 done (D247): the Kotlin core on the JVM. `bindings/kotlin/`
  (package `dev.tinyactors.sxml`) over `libsxml_kotlin.so`: the library,
  the host shim and one JNI glue file, exporting `Java_*` and
  `JNI_OnLoad` only. Processor, parse, compile, evaluate, value,
  serialize, the resolver and errors. AutoCloseable with a PhantomReference
  cleaner (no `java.lang.ref.Cleaner`: Android API 33). Every call holds
  the processor's lock. ctest `kotlin/jvm` (kotlinc, `-Xcheck:jni`, ASan
  preloaded under debug; skipped without JNI, kotlinc or java) passes 36
  cases with 0 leaks: 26 by the names of `package.test.ts`, the failure
  loop among them, plus close, cleaner and thread tests. Kotlin 2.2.20,
  JDK 21. Next: 6.5 (XQuery, XSLT, XSD), then 6.6 (the AAR).

- 6.7 done (D241): the Swift core. `Package.swift` at the root and
  `bindings/swift/` (module `Sxml`) import the host shim through the module
  map `CSxmlHost`, with no Swift-specific C. Processor, parse, compile,
  evaluate, value, serialize, the resolver and errors; ARC, and no type
  is Sendable. ctest `swift/linux` (merged `libsxml_host.a`, `swift test`,
  ASan under debug; skipped without swift) passes 27 Swift Testing tests
  with 0 leaks: 24 by the names of `package.test.ts`, 3 ARC tests and the
  allocation-failure loop among them. Swift 6.2 (swift-6.2-RELEASE) on
  Ubuntu 24.04. Next: 6.8 (XQuery, XSLT, XSD, the DUCET resource); 6.9 on
  the owner's Mac.

- 6.2 done (D240): the JavaScript shim is the host shim in
  `bindings/host/` (`sxml_host_*`, target `sxml_host`). The resolver is a
  state and a function, and the request names its processor. The
  JavaScript export names are kept by three thin export files, and
  `src/index.ts` is unchanged. All javascript/* tests pass under wasm. The
  new `host_shim` unit test (7 cases, the allocation-failure loop) passes
  under debug with 0 leaks. `sxml.wasm` 2,091,192 (+388), `sxml-full.wasm`
  7,637,765 (+561). Next: 6.4 (Kotlin) and 6.7 (Swift) link `sxml_host`.

- 6.1 done (D238): the Phase 6 plan in `docs/tasks/phase-6.md`. One host
  shim (the JavaScript shim moved to `bindings/host/`) under JavaScript,
  Kotlin (JNI glue only) and Swift (module map, no C). APIs mirror the
  JavaScript package. Ownership: AutoCloseable and Cleaner in Kotlin, ARC
  in Swift. Probed here: NDK r28c builds the library for all four ABIs
  at API 29 (arm64-v8a 7,693,024 bytes stripped). At API 24 only
  `aligned_alloc` and `timespec_get` are missing (6.3 adds fallbacks).
  Kotlin/JNI on JDK 21, an AAR with Gradle and AGP, and Swift 6.2 on
  Linux over the shim all work. Not here: an emulator (no KVM), iOS,
  macOS and device CI (owner). Packets 6.2 to 6.8 and 6.11 run here.

- 7.19 done (D237): the Phase 7 review and gate. Whole release runs at
  9c5b1ba, 0 error outcomes: XP40 32,689 (99.5 %), XQ40 45,969 (99.5 %),
  XT40 12,075 (94.8 %, core sets 88.5 % to 100 %), XSD in scope 41,486
  of 41,518; schema-aware XQ40 92.2 %, XT40 95.6 %; CONFORMANCE.md
  regenerated. Every packet meets its gate but 7.11's "all 12 XT40
  transform cases" (9; 7.4's QT4 part as D208 reads it). Sizes under
  budget: core 2,090,746, full 7,637,162, CLI 9,231,791. Fixed: an
  undefined pointer offset on empty input in `src/xml_input.c` (fuzzing);
  the iXML suite's 4 errors (inputs no xs:string holds, now not run);
  EXPath File's four-parameter functions and clang-tidy findings; Full
  Text's copy of `sxml_xml_buffer`; citations of D192 and D222. Added
  `ixml_fuzz` and `full_text_fuzz`, seeds for them and html_parse_fuzz.
  Proposed resolved: XQuery's XML version, the Phase 7 and budget items
  (D184, D189, D202, D207, D208), omit-xml-declaration (a design, D208).
  Not done: 7.11's three cases; the larger duplicates of D237 (three
  text decoders, Full Text's own resource reading, private growable
  arrays); fuzzing at length and the full lint on the owner's machine
  (D181). For the owner (D208): the license, tagging and publishing
  releases, and the npm package name.

- 7.6g done (D236): the last streaming items. xsl:merge with streamed
  sources is the IR kind MERGE: one cursor per anchor document, keyed
  k-way stepping through the grounded merge's own sort and grouping,
  XTDE2220 as input is read; capture="yes" end rules read subtrees the
  stream builder tees into full windows. Two 38.3 MB documents merged
  by `sxml transform`: peak resident 1,055.6 MB to 11.0 MB. XT40 11,943
  to 11,945 passed, none lost; 27 merge cases and 5 capture cases now
  stream. Left: merge sources with validation, type, accumulators or
  predicates (grounded, same results).

- 7.12d done (D235): XPath Full Text (Appendix B) through
  grammars/xpath_full_text, which only the feature full-text links (in
  grammars/xpath the productions would add 195 KB to the core); the
  productions written once (grammars/xpath/full_text.js), Full Text's
  parser tables once (src/full_text_parse.c, a parser part extension of
  XQuery's part and the feature's), the lowering and the feature shared
  with XQuery, score variables in XPath's for. `sxml.wasm` +1,215 bytes;
  the full module and the CLI +502 KB, their budgets raised to 8,000,000
  and 9,500,000. XP40 32,655 and XQ40 45,914 passed, unchanged case by
  case; the ft-* cases are XQuery only, and the 48 written as XPath
  expressions pass through the XPath API. The JavaScript package binds
  bin wherever expath-binary is present, as the sxml command does.

- 4.21 done (D234): XT40 11,913 to 11,943 passed, 0 errors, one wrong
  error now a failure (document-4066); QT4 XQ40 unchanged case by case.
  The hoist pass leaves indexed FILTERs alone (a hoisted LET raised an
  error the predicate never ran); fn:current-merge-group(()) and
  XTDE3490; globals computed on first read through one IR kind,
  GLOBAL_READ, and a last pass (XTDE3052 of unread abstract variables,
  XPDY0002 of unread globals, XTDE0640 in XSLT, the initializer's error
  not caught around the read, required parameters still checked);
  typed modes read leading element names as schema-element(), XTSE3105.

- docs-2 done: README.md rewritten as a plain overview (standards with
  versions, the modules and their sizes at e4ea880, streaming, HTML, iXML
  and Full Text examples run on a release build, features by module,
  conformance, status); apps/sxml/README.md and the check and program help
  texts brought up to date (74 console commands match exactly);
  bindings/javascript/README.md covers the full module. CONFORMANCE.md
  regenerated from whole release runs at e4ea880: XP40 32,689, XQ40
  45,969, XT40 12,053, XSD in scope 41,486 of 41,518, 0 errors.

- 7.6f (a) done (D233): incremental bytes. A byte reader resource form
  (`processor.h`, additive): a streamed document is decoded and parsed
  as its bytes arrive through a text window (`src/xml_input.c`: UTF-8,
  UTF-16, ISO-8859-1, XML 1.1 line ends, a part at a time, the text
  before the current segment given up); the CLI's resolver delivers
  files so. `sxml transform` on 95.6 MB, peak resident: streamed
  identity 133.9 MB to 11.0 MB, for-each 134.5 MB to 11.0 MB; 974 MB in
  10.9 MB. XT40 11,918 and XQ40 45,914 passed, unchanged case by case.
  Left: xsl:merge with streamed inputs and capture="yes" end rules (both
  need IR and evaluator files 4.21 holds; designs in D233).

- perf-3 done (D232): XML parsing speed within D12. The profile showed
  tree-sitter's cost per token and node, so the grammar makes fewer:
  '<a>', '<a/>', '</a>' and attributes without references are single
  tokens and nodes without children, the content items of a segment are
  a left-recursive rule tree-sitter does not rebalance; the walk reads
  fields by number and node types by symbol. Parse-only 14.9 to 25.2
  MB/s (100 MB: 14.7 to 25.5), the tree 13.4 to 21.3 MB/s; 10 MB check
  6.71 to 3.90 x 10^9 instructions; XMark-All 9.87 to 8.88 (1.17 to
  1.08 s), parsing now 16 % of it. QT4 and XT40 unchanged case by case.
  Left: tree-sitter's per-character lexer and per-token stack.

- 4.20 done (D230): XSLT conformance sweep, XT40 11,826 to 11,912
  passed, none lost, 0 errors (whole suite, release, case by case after
  each change). xsl:merge checks its input order (XTDE2220, by the
  stable sort), computed key attributes (XTDE2210) and validates
  for-each-source documents; [before 1] collation resets and the
  vendored Swedish collation; record constructors synthesized as the
  implicit xsl:function; element validation without ID/IDREF,
  xs:untypedAtomic as a type, XTTE1545, XTSE0265; the codes the
  specification gives (XTSE0550, 0580, 0710, 1295, 1570, 3440, 3460,
  XTDE0830, 0860, 0905, 0920, 1360, XTTE0945); inline functions and
  xsl:iterate bodies in their module; string casts to xs:QName with the
  expression's namespaces; fn:transform's enable-assertions; the XSLT
  driver's assertions see the environment's schema and decode
  ISO-8859-1 and UTF-16 output. Left: lazy globals (XTDE3052, designed
  in D230, needs the evaluator), typed modes, XTDE3490, xml-version-007,
  namespace-2606/-6202, docbook-004.

- 7.6e done (D231): the last streamed shapes. Accumulators computed
  while streaming (each event a STREAM takes calls the accumulator
  functions, full windows get descend-K; fn:accumulator-before and -after
  read the values of streamed nodes; rules' errors raised when a value is
  read; dependent rules wait a round); xsl:fork prongs streamed in passes
  of their own over the document; the principal result of a streamed
  principal source serialized as it is made (a DOCUMENT with
  PRINCIPAL_RESULT over the main body, a streamed XML serialization).
  `sxml transform` on 95.6 MB: identity 525 MB to 134 MB peak resident,
  an accumulator over the records more than 4.1 GB (stopped) to 135 MB;
  what is left is the input the CLI holds. XT40 12,031 to 12,048, none
  lost. Left: xsl:merge with streamed inputs (a new evaluator part), the
  byte reader resource form (waits for the parser files perf-3 holds),
  capture="yes" end rules.

- perf-2 done (D229): the second round of evaluator performance,
  profile first (callgrind). Numbers to doubles by Clinger's fast path
  (strtod was 6 % of XMark-All); leaves computed at their push, charged
  their step; item lists, type matcher and coercion frames inline; one
  XNode atomizes without a builder, its string value validated once;
  one atomic item matches its own annotation directly; inline integers
  compare directly; match="$var"'s type check of $V memoized; numeric
  filter index keys (exact comparisons share nearest doubles); the hoist
  pass takes quantified expressions. Instructions: xmark-all 12.29 to
  9.84 x 10^9 (1.55 to 1.20 s), match="$var" 9.30 to 3.91, numeric-join
  (new) 4.98 to 2.54, recursion 8.28 to 7.23, itunes 3.36 to 2.97 s.
  QT4 XQ40 and XT40 unchanged case by case at each milestone. Left:
  parsing is a quarter of XMark-All; the cast pipeline's layers; the
  arena describe_node; hoisting from path MAPs and sort keys.
- 7.6d done (D228): a streamed principal source. `xslt.h` (additive):
  `streamed_source_uri` in the transformation request and
  `sxml_xslt_stylesheet_streams_source`; the invocation opens the URI
  with STREAM OPENS_DOCUMENT and applies the initial mode to the open
  document node. Template rules of a streamed mode are streamed bodies or
  run inside a grounding stream (STREAM axis self, the node's full
  window); the built-in rules stream the children, their values content
  LETs; apply-templates without select and child-step apply points use
  shallow windows; fn:avg folds; the IR printer writes self, fold and
  apply focus. `sxml transform` streams an INPUT file when the initial
  mode streams: 100 MB, "/" with a for-each, 673.2 MB to 267.3 MB
  resident; template rules 658.8 MB (tree) to 267.3 MB. The XSLT driver
  streams the suite's streaming="true" sources. XT40 none lost
  (streamable-116 now passes). Left: accumulators while streaming,
  xsl:fork, xsl:merge with streamed inputs, a principal result held whole.

- size-2 done (D227): per-language part lists. CMake writes, beside
  each part registry, one unit per language with the parts serving it
  (`sxml_function_library_<language>_parts()`,
  `sxml_xpath_lowering_<language>_parts()`,
  `sxml_evaluator_<language>_parts()`); each language's entry point
  names its lists, and a unit carries its language's evaluator parts, so
  a module links only its languages' parts. The core `sxml.wasm` drops
  XQuery's lowering (with xquery_flwor and the update primitives),
  XSLT's library parts, evaluator_stream and evaluator_template:
  2,215,004 / 774,905 (over budget after perf-1's passes) to 2,094,214 /
  731,897 at 5385a0e, and 2,101,275 / 734,951 to 2,077,439 / 725,473 at
  5664da8. The full module, CLI and native builds keep every part. QT4
  XP40, XQ40 and XT40 whole release runs unchanged at both steps (5664da8:
  32,655, 45,914, 11,860 passed). Baseline refreshed.

- perf-1 done (D224): evaluator performance with general mechanisms.
  A benchmark set (`tests/bench/run-bench.py`: XMark, joins, paths,
  recursion, group by, order by, xsl:key, match="$var", app-Demos
  itunes); axis walks that describe each node once; filter indexes (an
  IR pass gives a FILTER whose predicate needs "=", "eq" or "is" a
  key and a probe, a for clause's where becomes such a FILTER, the index
  lives in the evaluation memo); a path's step from all its origins at
  once; the hoist pass for loop-invariant LETs. Processor seconds:
  xmark-all 6.73 to 1.52, itunes 80.6 to 3.4, Q8 1.19 to 0.28, Q11 2.22
  to 0.79. QT4 XQ40 45,886 and XT40 11,826 passed, unchanged case by
  case after each change. Steps per step unchanged; counts fall where
  work is skipped.
- 7.6c done (D223): more streamed shapes in xsl:source-document
  streamable="yes". A streamed path (a downward path, perhaps ending in
  @a) in the select of value-of, copy-of, sequence, for-each,
  apply-templates, iterate or for-each-group streams in place through a
  new lowering override of the XPath lowerer; count/exists/empty read only
  the chain's counter, sum/min/max fold node by node (IR `fold`), other
  atomizing functions get one value per node, the rest full windows.
  Template rules of a guaranteed-streamable mode run on each node as it
  passes (IR `apply_templates.focus`). Unused accumulators no longer stop
  streaming; a D219 bug lost text after empty child elements. XT40 no case
  changed; streamed source documents 164 to 986; 57 nodes get templates
  one by one, 600,075 fold. 2.4 MB under 1 MB: count 186 KB, sum 223 KB,
  apply-templates 233 KB. Left: streamed accumulators, fork, merge, a
  streamed principal source.

- health-3 done (D225): `sxml.wasm` back under budget, 2,352,234 /
  823,283 to 2,180,960 / 762,788 (budget 2,200,000 / 770,000, not
  raised). Per-commit linker maps since ae7ef0b: c008c7c (lax validation
  without in-scope schemas) made the XSD schema compiler reachable from
  XPath again (+171,516), and 820231e's fn: schema a second way. The
  processor's built-in schemas for XPath's functions are now the optional
  feature `sxml_builtin_schemas_feature()` ("builtin-schemas", in
  `sxml_all_optional_features`). Guard: the modules link with a map, and
  `javascript/size_budget` prints the largest growths over
  `test/size-baseline/sxml.txt` (twenty when over budget). The full
  module's budget rises to 7,500,000 / 2,625,000. Also: debug-gcc's
  -Wconversion in bin:pack-integer (only under UBSan), WASI preopens
  /usr/share/zoneinfo, and the two validate CLI goldens follow D222.
  Counts unchanged (QT4 fn-json-to-xml, fn-analyze-string, fn-parse-xml,
  fn-doc, fn-xml-to-json, fn-xsd-validator, prod-ValidateExpr,
  prod-SchemaImport, fn-element-to-map, app-spec-examples; XT40
  json-to-xml, analyze-string, import-schema, xml-to-json, validation).
  Left: evaluator_stream (14,565) is linked into the core through the
  evaluator part registry although only XSLT lowers to STREAM.

- 2.22 done (D226): XPath and XQuery leftovers. The fn: namespace's
  schema (F&O 4.0, appendix D) for imports and, compiled once per
  processor, for json-to-xml validate=true and typed analyze-string
  results; lax validation without schemas; fn:element-to-map's
  schema-based conversion (220 of 220); deep-equal's typed-values and
  type-variety; doc-available with options; schema-type of schema
  types; the draft's record model (a plain map is no record; but with
  keeps the record type; a record called with another key is
  XPTY0004); XPath 4.0 namespace declarations' XQST0033/XQST0070;
  XQST0039; record and decimal-key lookups. XP40 +56, XQ40 +98 cases,
  2 honest losses each (doc-available xinclude and
  use-xsi-schema-location). Open: the item axis's JNode parent,
  fn:xsd-validator's schema options, `{ E }` over JNodes.

- xml-mem done (D222): the XML parser reads a document in document
  segments and holds one segment's tree-sitter tree at a time: the head
  (prolog and the document element's start tag), then content segments
  of about 16 KB cut by a byte scan between content items
  (`src/xml_segment.c`), parsed by two new start alternatives of
  `grammars/xml` after builder markers; the open elements' start tags
  are written before each segment so the scanner still checks end tags.
  Parsing 100 MB: 3,789 MB to 98 MB resident (the input is 97 MB), CPU
  25.0 s to 19.7 s; 10 MB: 387 MB to 13 MB. The incremental parse
  resumes across segments; the unit test parses 4 MB under a 256 KB
  allocator limit with tree-sitter under 2 MB. xmlconf, QT4 and XT40
  unchanged case by case. A document with two byte order marks is now
  refused (correctly). Open: `sxml transform` does not stream (no
  resolver delivering bytes); large prologs and external entities are
  parsed whole.

- 7.12b and 7.12c done (D214 amended): Full Text's match options and
  scores. Stemming: the Snowball algorithms of the D208 languages,
  compiled by `tools/generate-snowball-stemmers.py` from the vendored
  .sbl files (`third_party/snowball`, BSD) for one stemming machine
  (`src/full_text_stem.c`); every snowball-data word stems as Snowball
  3.1.1 does. Stop word lists and thesauri by URI are read while
  compiling through the new resource kinds FULL_TEXT_STOP_WORDS and
  FULL_TEXT_THESAURUS (`processor.h`, additive), with relationships,
  levels and the defaults; the QT4 driver serves the catalog's files
  (fullText-defaults supported). Scores come from the matches and their
  weights (w / (w + 1 + log2(1 + n))), for let score and for score.
  Ideographs and hiragana are one-character words (UAX #29), so zh and
  ja are accepted. ft-* 624 to 672 of 685, XQ40 45,839 to 45,868; the
  13 left: 9 expected-result whitespace, 2 stemmer-dependent, 2
  fullText-units. Open: `sxml.wasm` is over its 2,200,000 budget at HEAD
  (2,328,807 before this packet; +2 KB here).

- 7.17 done (D221): languages and calendars in the formatting
  functions, as the optional feature `sxml_localization_feature()`
  ("localization") over CLDR 47 files the host serves
  (`urn:sxml:locale-data:` + main/, rbnf/, supplemental/). One CLDR
  module (`src/cldr.c`) now serves the collation tailorings too: tags,
  locale chains, LDML files as tables of distinguished paths, lookups
  with inheritance and aliases. Month, day, era and am/pm names in en,
  de, fr, it, es, nl and regions; the calendars BE, JE (eras by their
  dates), OS (Julian) and AH (tabular Islamic); words and ordinals by
  CLDR's spell-out rules (RBNF, explicit stack) for format-integer,
  xsl:number and [Dwo], rule sets by name or by ending. QT4
  fn-format-date 125 to 137, fn-format-integer 87 to 94,
  fn-default-language 5 to 6; XT40 number 265 to 274 of 274; the 38
  named cases all pass, none lost. Vendored main/, rbnf/ and
  supplemental files (4.1 MB); the CLI embeds the names (+366 KB).
  Open: `sxml.wasm` was over its budget before this packet (2,342,948
  of 2,200,000; +4,920 here).

- 7.6b done (D219): streamed evaluation of xsl:source-document
  streamable="yes" on the shared machine: an incremental parse of the
  XML parser feeds a stream builder (`src/evaluator_stream.c`) that keeps
  the open elements and their attributes; the new IR kind STREAM selects
  one step's nodes as their events pass, into shallow or full stream
  windows; `src/xslt_lower_stream.c` plans bodies of motionless
  instructions and one xsl:for-each over a downward path (7.6a's sweeps).
  Allocator peak 232 KB for 2.4 MB, 8.3 MB and 106 MB documents (the
  unit test streams 2.4 MB under a 1 MB limit; unstreamed fails); the
  process still holds tree-sitter's syntax tree of the whole input
  (3.98 GB RSS at 106 MB). `xslt.h`: falls_back_to_unstreamed (19.10);
  supports-streaming true. XT40 11,797 to 11,804, none lost; 164
  xsl:source-document executions stream. Not streamed yet: accumulators,
  apply-templates and streamable modes, iterate, fork, merge, aggregates.

- 7.18 done (D215): Invisible XML. fn:invisible-xml as the optional
  feature `sxml_invisible_xml_feature()` ("invisible-xml"): one grammar
  compiler over the XML form (tree builder events, or the constant table
  of the grammar of grammars, so ixml notation is parsed by the same
  Earley parser), an Earley parser whose items keep their first
  derivation and mark a second (ambiguity), and construction with marks,
  renaming, insertions, ixml:state and D02 to D07. QT4 fn-invisible-xml
  16 of 16 (was 6, wrong errors), now in the D95 scope. The iXML
  community suite is vendored from Codeberg (`third_party/ixml-tests`,
  2.4 MB) and run by an XQuery driver through `sxml xquery`
  (`tests/ixml/`): 886 of 907, every grammar test, 711 of 715 cases with
  a correct grammar; 4 errors are inputs with C0 controls an xs:string
  cannot hold. `sxml-full.wasm` +39,704 bytes, `sxml.wasm` +4. Open:
  right recursion is quadratic (no Leo items).

- 7.15b done (D220): UCA collations order by CLDR's root collation
  (`allkeys_CLDR.txt`, asked for before the DUCET) and honour `lang`
  through the CLDR collation of the language, read from its LDML file at
  first use and compiled into a tailoring over the root
  (`src/collation_tailoring.c`: resets, relations, starred ranges,
  extensions, settings, `[reorder]`, `[import]`; `-u-co-` types;
  locale lookup by truncation), and `reorder` over 170 reorder groups
  generated from CLDR (`src/collation_tables.c`). CLDR 47 subset vendored
  in `third_party/cldr` (root, en, en_US, en_US_POSIX, de, de_AT, es, fr,
  fr_CA, it, nl). The QT4 and XSLT drivers and the sxml command serve it;
  xsl:sort lang works. QT4 collation sets unchanged (no case lost; fr-CA
  now by its tailoring), XT40 collation and sort sets unchanged.
  `sxml.wasm` +375 bytes, `sxml-full.wasm` +25,126, the CLI +37,303, all
  within budget. Not supported (not honoured, root order): `[before n]`,
  special reset positions, prefixes, `[suppressContractions]`. The
  JavaScript package still ships the DUCET only.

- 7.12a done (D214): XQuery and XPath Full Text 3.0 as the optional
  feature full-text. Grammar in `grammars/xquery` (43 contextual
  keywords, +500 KB of parser), AST, static rules, a tokenizer, the
  match program (an XDM array of postfix text and operands) run over
  AllMatches rows, every connective, FTWords variant and positional
  filter, case, diacritics, wildcards, explicit stop words, score
  variables. QT4 ft-* 0 to 624 of 685 (91 %; 0 errors); the rest is
  thesauri and stop word lists by URI (29) and stemming (21), 7.12b/c.
  `sxml.wasm` +2 KB; every budget holds.
- 7.10b done (D218): HTML input sniffs its encoding as browsers do
  (BOM, the encoding option, the `<meta>` prescan, then UTF-8 or
  windows-1252, tentative) and parses again when a later meta element
  changes it; every legacy multi-byte encoding decodes (gb18030/GBK,
  Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR); XHTML input is parsed
  as XML. The html5lib tests are vendored (`third_party/html5lib-tests`)
  and run by `tests/html5lib_test.c`: tree construction 1,749 of 1,753
  (4 selectedcontent), tokenizer 7,045 of 7,045, encoding 82 of 82. QT4
  unchanged (fn-parse-html 22/22, fn-html-doc 9/10, misc-HtmlTestSuite
  1,378/1,379). `sxml-full.wasm` +184,806 bytes stripped (the indexes),
  core unchanged. Source locations wait for the processor to keep them.
- cli-2 done (D217): the sxml command writes files, only where its
  command line says. `--output=FILE` (`-o`) and `--force` on xpath,
  xquery, transform and serialize; `sxml transform --result-directory=DIR`
  writes result documents with an href below DIR (the base output URI);
  `sxml xquery --allow-put` lets fn:put store documents. Every file goes
  through a temporary file and is committed only when the command
  succeeds (new `sxml_cli_file_writer`, `apps/sxml/platform_file_writer.c`).
  Also `--method=csv`, and an unknown option names the commands that
  have it. Folded in 7.10b's static base URI for the query commands. CLI
  tests 161 to 203 (debug preset, ASan and UBSan); wasm builds.
- js-1 done (D216): the JavaScript package's full module,
  `sxml-full.wasm`, has XQuery 4.0 with the Update Facility
  (`Processor.compileQuery`: external variables, output declarations,
  updated trees and fn:put documents through an update sink), XSLT 4.0
  (`compileStylesheet`: parameters, initial mode, template or function,
  principal result serialized, raw or as a tree, result documents,
  messages) and XSD 1.1 validation (`compileSchema`: every report with
  its location, the validity, the typed copy). Four shim files linked
  into the full module only (D189's two links): `sxml.wasm` 2,134,058
  bytes, unchanged; `sxml-full.wasm` 3,666,545 to 6,570,723 (the XQuery
  compiler and grammar; budget 7,000,000 / 2,450,000). 24 Node tests.
  Open: `javascript/size_budget` fails on the CLI module at HEAD
  (7,803,988 bytes, budget 7,500,000), and the `debug-gcc` library does
  not build (`src/function_library_binary.c:1303`, -Wconversion); neither
  is this packet's.

- 7.15a done (D213): full UCA collations as the optional feature
  `sxml_uca_collation_feature()` ("uca-collation") over the DUCET the
  host serves (`SXML_RESOURCE_KIND_COLLATION_DATA`), read once per
  processor by `src/collation_uca.c`: contractions (also discontiguous),
  implicit weights, shifted and blanked, maxVariable, caseFirst,
  caseLevel, numeric, backwards, the identical level over NFD;
  fallback=no only with the table. Comparisons and sorts now use the
  default collation. Unicode's DUCET and CollationTest samples vendored
  in `third_party/unicode-collation/`; both samples (and the full _SHORT
  files) sort in order. QT4 advanced-uca-fallback 49 of 49,
  non_unicode_codepoint_collation 13 of 13; XQ40 +65, XP40 +57 passes in
  the collation sets; XT40 collations 35 to 39. The CLI embeds the DUCET
  (budget 7,500,000 / 1,650,000); the JavaScript package ships it.
  Tailorings (lang, reorder) are 7.15b.

- 7.6a done (D212): XSLT streamability analysis (XSLT 4.0, 19) in
  `src/xslt_streamability.c`, a frame machine over the stylesheet AST
  and its XPath ASTs, run by sxml_xslt_compile: postures, sweeps,
  U-types, the verdicts of 6.6.4, 10.2.3, 15.4, 18.1, 18.2.9 and
  19.8.5, XTSE3430 for a construct declared streamable that is not
  guaranteed-streamable; uncertain verdicts are never errors.
  Evaluation stays unstreamed; supports-streaming false. The XSLT
  driver claims streaming (not streaming-fallback). XT40 passed 8,952
  to 11,792: streaming cases 2,841 of 2,936, strm 2,503 of 2,564, 154
  of 156 XTSE3430 cases. Proposed: a fallback option in `xslt.h` for
  7.6b.

- 7.2b done (D211): the core size audit. `sxml.wasm` was 2,294,051
  bytes stripped, over 2,200,000: fn:doc's dtd-validation (4869814) made
  the whole schema compiler reachable (+167 KB); XQuery Update +4.3 KB,
  EXSLT +1.3 KB. DTD validation is now the optional feature
  `sxml_dtd_validation_feature()` ("dtd-validation"; FODC0013 without
  it), in `sxml_all_optional_features()`. `sxml.wasm` 2,128,754 /
  741,943, no budget raised; `sxml-full.wasm` 3,565,200. QT4 fn-doc,
  fn-parse-xml, fn-doc-available, fn-system-properties and XT40
  document unchanged. The xs:numeric Proposed item is resolved by D208.

- 7.16 done (D209): the timezone database, an optional feature over
  TZif files the resolver serves (`SXML_RESOURCE_KIND_TIMEZONE_DATA`,
  `urn:sxml:timezone-data:PATH`); tzdata 2026e vendored in
  `third_party/tzdata` (`tools/update-tzdata.sh`), served by the QT4
  driver; the sxml command reads `/usr/share/zoneinfo`.
  fn:civil-timezone for zone names; $place adjusts to a zone and names
  [ZN], a country code names [ZN]. fn-civil-timezone 4 to 9 of 9,
  olson-timezone turned on: fn-format-dateTime XQ40 83 to 93, XP40 80 to
  88; format-time and app-spec-examples +1 each; XT40 format-date
  unchanged. Full module +10.7 KB, core +1.9 KB.

- 7.14c done (D210): the DTD follow-ups. `tree.h` gains (additive,
  D99) unparsed entities listed by number and copied with a document
  node (xsl:copy, xsl:copy-of, fn:copy-of, fn:snapshot), and an entity
  base URI for elements at the top of an external entity (dm:base-uri).
  XTDE1380 has its own code. `sxml check --external-entities` and
  `--validate-dtd` are added. The QT4 driver claims infoset-dtd and
  dtd-validation-option, and supports-dtd-validation is true. XT40
  8,944 to 8,952, dtd 68 to 76 of 79 (gate met); QT4 XQ40 fn-id 61/61,
  fn-idref 51/54. Left: base-uri-051a (FODC0016 is right), the
  system properties should follow D211's feature.

- 7.4b done (D206): the XSLT 1.0 follow-ups. EXSLT common
  (`src/function_library_exslt.c`, XSLT only): exsl:node-set returns
  its argument, exsl:object-type; exsl:document in an extension
  namespace is read as xsl:result-document. name.h gains
  SXML_NAMESPACE_EXSLT_COMMON (additive, D99). Text value templates in
  1.0 elements are lowered without XPath 1.0 compatibility mode; an
  explicit xsl:result-document chooses xhtml also in a 1.0 stylesheet.
  docbook-002's XPST0003 came from an unread external parameter entity,
  fixed by 7.14. XT40 8,940 to 8,943 (docbook-001, docbook-002,
  backwards-019b). `xpath_compatibility.c` is about 7 KB of the core
  module. Proposed for the owner: fn:number for xs:numeric parameters
  in 1.0 mode (14 cases).

- 7.7b and 7.7c done (D205): fn:put as XQuery's updating function,
  its documents handed to the update sink's new receive_put_document
  after the changed trees (the library writes nothing; the QT4 driver
  makes them available documents); revalidation lax and strict through
  sxml_schema_validate_node on the rebuilt trees; copy-modify copies
  untyped and without schema IDs (additive `strips_identity` in
  `tree.h`, XSLT keeps its IDs); the 4.0 draft's updating return
  clause. upd-* 733 to 787 of 825; XQ40 and XT40 otherwise unchanged.
  Left: XUDY0038 on simple dynamic calls, 6 propagateNamespace cases
  with other whitespace in their expected XML, 2 IDREF checks on
  element validation, XQuery constructors under strip still keep IDs.

- 7.11 done (D203): fn:transform as the optional feature
  `sxml_xslt_transform_feature()` ("xslt-transform", in every-feature
  hosts and `sxml-full.wasm`). Its resumable body compiles the
  stylesheet with `xslt.h`'s compiler and runs the transformation as a
  unit evaluation on the caller's machine (no nested run, D71); a unit
  evaluation now carries its language's extension in its global table,
  so the inner transformation's result documents reach its own writer.
  QT4 fn-transform 0 to 129 of 133 run (XQ40), 126 of 130 (XP40); XT40
  8,861 to 8,870, transform 0 to 9 of 12, none lost. `sxml.wasm` +327
  bytes; `sxml-full.wasm` +1,329,984 (3,554,353, D207's 4,000,000).
  Left: static fn:transform (transform-004), enable-assertions false, a
  separate execution scope and document loader for the inner run.

- 7.14 done (D199): DTD processing. The external subset, external
  parameter and parsed entities (through an entity resolver in `xml.h`;
  the processor adapts its resource resolver, new resource kind
  EXTERNAL_ENTITY), conditional sections and parameter-entity expansion
  (grammar in `grammars/xml`, the rest one explicit-stack loop, D71).
  Validation is schema validation: the DTD becomes XSD documents run
  through the schema validator as the document is parsed
  (`sxml_dtd_validator`, `schema.h`). fn:doc and fn:parse-xml:
  dtd-validation (FODC0007) and trust-external (default false,
  FODC0016). xmlconf entity cases 0 to 265 run (valid 127/127, invalid
  54/54, not-wf 66/66), XML 1.1 not-wf 140 to 169 of 169; validation
  finds 594/594 valid and 173/173 invalid standalone cases. XT40 8,861 to
  8,931, dtd 68 of 79 (86 %, gate 90 %); QT4 fn-doc +4, fn-parse-xml +7
  in each language; none lost. Left: the QT4 driver's infoset-dtd and
  dtd-validation-option claims with supports-dtd-validation (files
  busy), three Proposed items (element base URI, unparsed entities on
  copy, an `sxml check` flag).

- 7.9 done (D200): EXPath File 4.0 as the optional feature
  `sxml_expath_file_feature()` (37 file: functions) over a new host
  interface, `sxml_file_system` in the processor configuration; library
  code never touches files. Not in `sxml_all_optional_features()` nor in
  either JavaScript module (+164 bytes for the interface). The sxml
  command implements it over POSIX (`apps/sxml/platform_file_system.c`;
  Windows not yet) and grants it only with `--allow-file-access`; the QT4
  driver copies each sandpit to a temporary directory. expath-file not
  run to 270 of 270 (XQ40) and 267 of 267 (XP40, 3 XQuery-only);
  supports-file-library follows the configuration.

- 7.4 done (D197): XSLT backwards compatible behaviour and XPath 1.0
  compatibility mode. The XTDE0160 place is gone; elements whose
  effective version is below 2.0 compile with the mode, a flag of the
  XPath static context (also a last field of xpath.h's static context
  configuration) that the lowering reads to insert the 1.0 conversions
  as internal native rows (`src/xpath_compatibility.c`): arguments of
  built-in functions, arithmetic and range operands, general
  comparisons. XSLT's 1.0 rules for xsl:value-of, attribute value
  templates, xsl:number, xsl:sort, keys and XTDE1425 extension
  functions; supports-backwards-compatibility yes. XT40 8,687 to 8,787
  passed (whole suite, case by case), 111 of the 124 backwards cases;
  QT4 xpath-1.0-compatibility 1 of 5. The 10 XT40 and 4 QT4 cases left
  expect fn:number for xs:numeric parameters, which XPath 4.0, 3.4.1 no
  longer gives (D197). Not done: text value templates of 1.0 elements,
  xml method for 1.0 result documents, exsl:node-set (DocBook).

- 7.10a done (D201, D191): HTML5 parsing, fn:parse-html and fn:html-doc,
  as the optional feature `sxml_html_parser_feature()` ("html"). A
  hand-written tokenizer (the 84 states of the current Living Standard,
  processing instructions included) and tree construction on a working
  tree (21 insertion modes, adoption agency, foster parenting, foreign
  content), sent to an arena tree with the XDM mapping of F&O 4.0,
  17.3.1; WHATWG decoders for octets (28 single-byte encodings, UTF-8,
  UTF-16). misc-HtmlTestSuite 0 to 1,378 of 1,379, fn-parse-html 0 to
  22 of 22, fn-html-doc 0 to 9 of 10 (XQ40; XP40 31 of 32), and
  misc-BuiltInKeywords +2; the sets are no driver non-goals any more.
  html5lib tree tests 1,747 of 1,753 in a scratch harness. `sxml.wasm`
  unchanged (2,078,210), `sxml-full.wasm` +79,107 bytes (2,182,635).
  The CLI module was over its 5,500,000-byte budget before (5,848,173)
  and is now also over its gzipped one (1,267,818 of 1,250,000): for the
  owner (D201 Proposed). Next: 7.10b (meta prescan, the html5lib tests
  as a unit test, the rest of D201's list).

- 7.3 done (D196): xsl:evaluate. The instruction calls a row whose
  resumable body compiles the target with the XPath compiler and runs it
  as a unit evaluation (D154), nested without C recursion; static
  context from the instruction's module or namespace-context, parameters
  from xsl:with-param and with-params, public stylesheet functions as
  function items (new function entry kind FUNCTION_ITEM), fn:current,
  trusted. XT40 8,899 to 8,973 passed, 0 errors, none lost; evaluate 73
  of 83 (91 % of the 80 run). Gate not met yet: HTML (7.10), trust checks
  of json-doc and unparsed-text-available, UCA.

- 7.7a done (D187): the XQuery Update Facility 3.0. Grammar (all 1,159
  upd-* query texts parse), static rules in one stackless AST walk
  (XUST0001/0002/0028; %updating replaces D162's XQST0045 place), the
  pending update list as a sequence of update primitives (function items
  with an inert body: no new item kind, no evaluator state), a lowering
  part of STATIC_CALLs, application by rebuilding trees through the tree
  builder (tree.h unchanged), copy-modify and transform-with, the
  additive xquery.h update sink, `sxml xquery` writing the updated
  context document, and the QT4 driver's update test cases. upd-* 0 to
  733 of 825 passed (10 wrong error), 24 failed, 0 errors, 68 not run
  (staticTyping, revalidation lax/strict); the rest of XQ40 unchanged
  case by case. Next: 7.7b (4.0 draft additions), 7.7c (fn:put,
  revalidation).

- 7.8 done (D198): EXPath Binary 4.0 as the optional feature
  `sxml_expath_binary_feature()` (33 bin: functions; D191's library
  functions, so `sxml.wasm` does not link them). bin-* 0 to 646 of 646 in
  XP40 and XQ40 (26 of them wrong error codes that 4.0's types make
  XPTY0004); fn-serialize +24 in each (canonical JSON via
  bin:unpack-double); supports-binary-library follows the configuration.
  fn:unparsed-text's decoder is shared (`src/text_decoding.c`).

- 7.13 done (D195): XML 1.1 input. The version is read from the XML
  declaration; 1.1 documents refuse restricted characters, translate NEL
  and LINE SEPARATOR (a pre-pass; the grammar is unchanged for 7.14) and
  allow C0 character references. xmlconf XML 1.1 202 to 242 of 271 (the
  29 left need external entities: 7.14); QT4 claims xml-version 1.1
  (XQ40 300 of 313, fn-graphemes 918 to 1,189); XT40 XML_1.1 supported,
  xml-version 0 to 40 of 42, namespace 206 to 221; XSD XmlVersions 48 to
  51 of 51. No passed case lost. For the owner: XQuery's own XML version
  (D195 Proposed).

- 7.5 done (D188): disable-output-escaping. Text nodes carry a mark
  (`disables_output_escaping`, additive to `tree.h`, as D163's CDATA
  request); text that differs in it stays a node of its own; ignored in
  temporary output state and when cdata is present; the XML, XHTML and
  HTML methods write it unescaped, never in CDATA. XT40 8,759 to 8,784
  passed (disable-output-escaping 25 of 25 run, bug 4), 0 errors, none
  lost; 5 cases that need the feature absent are not run any more.

- 2.21 done (D190): XPath and XQuery sweep. Casts and castable to
  array, map and record types (the coercion frames with cast rules);
  fn:function-lookup binds context-dependent functions to the caller's
  focus (three extra arguments of a static call); the item axis is
  navigational, "/" reaches a JTree root, XPTY0019/XPTY0020 are
  XPTY0004; distinct-values with the implicit timezone; fn:doc's
  xsd-validation; the QT4 driver's assert-xml trims inline expected XML.
  Release, full runs from 7d70d01: XP40 30,931 to 31,098, XQ40 41,273 to
  41,456, none lost, 0 errors; XT40 no set lost. Also fixed GCC's build
  of tests/optional_feature_test.c. Not done: the fn namespace's
  built-in schema, the current draft's record model, EXPath Binary's
  serialize cases (D190).

- 7.2 done (D189): optional features. `processor.h` gains
  `sxml_optional_feature` and a feature view in the processor
  configuration; the D162 place of each feature asks the processor for
  it by kind; feature data comes from the resolver. The CLI and drivers
  name every feature. JavaScript ships `sxml.wasm` (core, no feature)
  and `sxml-full.wasm` (all); budgets 2,100,000 and 3,000,000 bytes
  (D184). `sxml.wasm` 2,003,575 to 2,004,485 bytes (+910, the
  mechanism's fixed cost). Not moved, measured: schema validation 71 KB,
  XQuery lowering parts 60 KB and XSLT parts 15 KB in the XPath-only
  module (registry links, about 75 KB to save). 7.8 to 7.18 may start.

- 4.19 done (D185): XSLT sweep after the Phase 4 gate. XT40 8,650 to
  8,759 passed (0 errors, nothing lost; whole suite, release, case by
  case after each change). Extension instructions call named templates
  (call-template-ext 7 to 32 of 32); xsl:item-type and xsl:record-type
  (record 22 to 39, type-alias 13 to 25; the record constructor is
  delayed); available documents are stripped source documents and
  fn:snapshot copies namespace nodes (snapshot 16 to 34); the XSLT
  driver serves collections (collection 0 to 5); merge 67 to 79 with
  XTSE1060/1070/3470/3500 in patterns, XTDE2210 and XTTE2230; errors in
  included modules name their module; one array allocation helper. Not
  done: the "~T" patterns of priority (dropped by the project's draft,
  D29: 34 cases stay failing), the match="$var" index (design in D185),
  XTDE3490 and the group key equality (other packets' files).

- CLI docs split (D181): the user reference of the sxml command moved to
  `apps/sxml/README.md`, every example re-run on a release build;
  `docs/CLI.md` keeps the design rules, planned commands and the
  implementation map. README links it and states the status after the
  Phase 5 gate (all public headers stable, D182).
- 7.1 done (D183): Phase 7 is planned in `docs/tasks/phase-7.md`, with
  one packet or packet group per deferred feature, each with its
  specifications, unlocked cases, D162 place, header changes, owned files,
  size and a proposed gate. The plan would unlock about 4,400 of XQ40's
  5,505 not-run cases, 3,330 of XT40's 4,659 and 540 xmlconf cases;
  the largest are streaming (2,932), HTML5 (1,401), the Update Facility
  (825), Full Text (685) and Binary (673). Wave 1 can start now: 7.3
  xsl:evaluate, 7.4 backwards compatibility, 7.5
  disable-output-escaping. Waiting on the owner (D183 Proposed): the
  four new features, the size option (`sxml.wasm` has 1,277 bytes
  left; recommended are link-time optional features with large data
  supplied by the host), and the gate numbers.

- 5.14 done (D182): the Phase 5 review. The gate boxes that need full
  runs or fuzzing are deferred to the owner's machine (owner's change);
  the review's own runs, at 02303a7 and again at 3b0812a: XSD in scope 41,483 of 41,518
  (99.9 %; datatypes 100 %, structures 99.7 %, XSD 1.1 99.6 %),
  schema-aware XQ40 1,236 of 1,516 (81.5 %), schema-aware XT40 627 of
  971 (64.6 %), XP40 97.6 %, XQ40 97.9 %, XT40 92.0 % (core sets 88.5 %
  or more); 0 errors and 0 crashes in all four suites; slices 198, 186,
  147, 147. Eight fuzz targets 300 s each, with the new schema_fuzz
  (schema and instance, seeded from the XSD suite): no crash or leak;
  four timeouts, all suite inputs slow by nature under ASan. The step
  budgets are fair (itunes is quadratic by its text, no schema hot spot).
  Fixed: the XSD driver counted an unsupported feature as "not run".
  Merged: one NCName check, one whitespace-only test, one token splitter
  (fifteen files), the schema code's copies of view equality, trimming
  and heapsort. schema.h has an allocation-failure loop and is stable.
  Five accepted Proposed designs moved to the open questions below.
  README was left alone (being reorganized): its "schema.h, still
  provisional" line is out of date.

- 5.13 sweep 2 done (D180): XT40 has 0 errors (was 6). The schema for
  schema documents compiles beside the built-in types (a restated
  built-in type is the built-in component), and the stand-in xml.xsd is
  used only when no document of the xml: namespace is there (catalog-009
  passes; catalog-005b fails on the catalog's outdated XSLT 4.0 schema).
  The XSLT driver judges a source that does not validate as the
  transformation's error: as-3301, as-3401, match-208 and match-209 name
  parsed entities in xs:ENTITY attributes (XSD 1.1 Part 1, 3.16.4) and
  fail. validate-items has its own row, which brings sxml.wasm back under
  its budget (1,998,723 bytes; 5.12 left it at 2,001,084). xslt.h's
  in_scope_schemas comment accepted; README status lines updated. XT40
  8,650 passed, schema-aware 627/971; XP40 30,931, XQ40 41,273, XSD
  41,483, all 0 errors; slices 198, 147, 147. Not done: is-id of union
  and list elements (not cheap, D180).

- 5.12 done (D179): schema-aware XSLT. xsl:import-schema (inline
  xs:schema, schema-location, namespace; XTSE0215, XTSE0220) compiles in
  `src/xslt_schema.c` into one kept schema after the host's in-scope
  schemas (xslt.h now reads them); the validation and type attributes of
  xsl:element, xsl:attribute, xsl:copy, xsl:copy-of, xsl:document,
  xsl:result-document, xsl:source-document and literal result elements go
  through D174's validate row with XSLT's error codes; default-validation,
  temporary trees as preserve, input-type-annotations="strip" (the
  whitespace-stripping copy), typed modes (XTTE3100), fn:document's
  xsd-validation, is-schema-aware. The XSLT driver validates sources and
  counts the "schema-aware" area. Schema-aware XT40 621/971; 615 of the
  679 schema_aware cases without another non-goal (90.6 %). XT40 8,642
  passed (was 7,990), 6 errors (invalid sources and XMLSchema.xsd, D179);
  XP40 30,924, XQ40 41,261, 0 errors; slices 198 and 147. Not done:
  ID/IDREF only for documents, xs:untypedAtomic types, xml:lang
  declarations, schema roles.

- 5.13 sweep 1 done (D178, amends D174): the QT4 driver's D174 rules
  (limits as "fail", UNSUPPORTED_FEATURE as "not run") are gone; their two
  cases are fixed at the cause: a group by keeps only the variables read
  after it (Catalog009, 536 MB to 147 MB), a FILTER computes a leaf
  predicate in its own task and xs:string nodes skip the cast for their
  typed value (itunes, 367 to 287 million steps; the QT4 step budget is
  4 x 10^8 as XSLT's). Validation errors name the element or attribute,
  the value, the type, the facet and the expected elements (message
  templates); configured schema documents that are not well-formed or not
  schema documents are located by the library. One XPath record compiler
  for assertions and identity constraints. XSD: src-redefine 6.2.2 and
  7.2.2, default open content and empty content, override defaults, simple
  bases of complex types in the registry, year "-0000", src-ct, wildcard
  subset with disallowed names. XSD in scope 41,483 of 41,518 (was
  41,452), XP40 30,931, XQ40 41,273 (0 errors), XT40 7,992. Not
  done: is-id of union and list elements, src-include for an unreadable
  include, D175's assertion leftovers.

- 5.11 done (D174): schema-aware XPath and XQuery. xpath.h gained
  `in_scope_schemas` (the in-scope schema definitions, carried into the
  unit's IR static context); `src/schema_scope.c` looks types,
  declarations, substitution groups and schema-type constructor functions
  up across them and raises schema errors with the language's codes.
  schema-element/schema-attribute resolve to element(M, T)/attribute(N,
  T) choices; `import schema` compiles one schema the processor keeps
  (host schemas reused, unreadable hints dropped); `validate` lowers to an
  internal row over sxml_schema_validate_node; fn:xsd-validator (in-scope
  schemas only); fn:parse-xml's xsd-validation. The QT4 driver serves and
  compiles environment schemas, validates sources and counts the
  "schema-aware" area. Schema-aware XQ40 1,231/1,516 (81.2 %), XP40
  279/315; XP40 30,924, XQ40 41,261, 0 errors; XT40, XSD11 unchanged.
  Not done: the fn-namespace schema (json-to-xml validate, analyze-string),
  fn:doc validation, run-time schema compilation for fn:xsd-validator's
  schema options, fixed default namespaces.

- 5.8 done (D177): identity constraints (`src/schema_identity.c`).
  Selectors and fields are checked against XSD's XPath subsets on the
  XPath front end's AST (c-selector-xpath, c-fields-xpath), compiled with
  `xpath.h` (xpathDefaultNamespace as default element namespace) and run
  by the one evaluator on 5.7's recorded subtree; unique, key and keyref
  (cvc-identity-constraint.3, 4.1, 4.2.1 to 4.2.3, 4.3) with key
  sequences compared by fn:atomic-equal within one primitive type, and
  keyrefs against node tables carried up from descendants (3.11.5);
  c-props-correct for keyrefs. MS-IdentityConstraint 1,058 of 1,058 run
  (was 788), Sun IdConstrDefs 48/48, IBM identityConstraint 18/18,
  xpathDefaultNSonKeyKeyRefUnique 26/26; XSD in scope 41,452 of 41,518
  with 5.9; slice 147/147. No shared code changed.

- 5.9 done (D175): XSD 1.1 assertions and type alternatives
  (`src/schema_assertion.c`). Tests compiled with `xpath.h` in XPath
  Valid's static context and run by the one evaluator; complex type
  assertions on an assertion copy of the recorded subtree (root
  xs:anyType, no comments or processing instructions, `$value` of simple
  content); assertion facets with `$value`, also for list items and a
  union's active basic member; simpleContent assertion facets on the
  simple content type; type alternatives with inherited attributes and
  the document's base URI; type tables compared (cos-element-consistent,
  restriction). Fixes: inheritable= of attribute references and
  restrictions, ##defaultNamespace per host element. Saxon Assert and
  CTA, IBM assert, assertion, typeAlternatives, typeAlternativesMixed 448
  of 448 (from 188); XSD in scope 41,037 of 41,518 (from 40,755), XSD 1.1
  gate group 95.6 %, slice 147/147. Not done: namespace fixup of
  inherited attributes, xml:base in the copies, value constraints against
  assertions at compile time.

- 5.10 done (D176): `sxml validate --schema=SCHEMA [--schema=SCHEMA]...
  [--mode=strict|lax] [--format=text|json] FILE...` validates documents
  against the XSD 1.1 schema the SCHEMA documents form (includes and
  imports as local files relative to the referring document), each FILE
  in one pass with the parser feeding the validator; every error as
  `FILE:LINE:COLUMN: CONSTRAINT: description`, JSON reports as the 5.10
  design gives them, exit codes as `sxml check`. Location hints are not
  followed (no flag). The CLI framework gained required repeatable
  options (`SXML_CLI_OPTION_AT_LEAST_ONCE`); the file URI helpers of
  `sxml transform` moved to `query_command.c`. 26 golden tests, docs/CLI.md
  section and README example. msData/particles: the 514 instance tests
  give the same validity as sxml-xsd-test (hinted documents passed as
  `--schema`). No library change: XP40, XQ40, XT40 and XSD counts
  unchanged. Proposed: locations of errors about a SCHEMA itself, and
  validation descriptions that name what failed.

- 5.7 done (D173): validation and the typed tree. The schema validator
  is a filtering tree builder (`src/schema_validate.c`): an element's
  start is held back until its attributes are known (xsi:type, xsi:nil,
  abstract declarations and types), children step 5.6's content model
  cursor (wildcards strict, lax and skip, lax assessment, one report per
  error in content, Element Declarations Consistent at validation),
  attributes by uses, xsi: declarations and attribute wildcards, defaults
  and fixed values added and compared, simple values through the cast
  engine with the facets' cvc-* names, schema normalized values sent on,
  IDs and IDREFs as XSD 1.1 binds them (`src/schema_validate_value.c`);
  the subtree recorder for 5.8 and 5.9 (`src/schema_validate_record.c`);
  `sxml_schema_validate_node`; typed values derived from annotations in
  `sxml_node_make_typed_value`. sxml-xsd-test validates instance tests
  and follows the instances' location hints. XSD in scope 40,755 of
  41,518 (instance tests 25,533 of 26,189: NIST 100 %, MS 95.0 %, Sun
  93.9 %; 632 wait for identity constraints, assertions and type
  alternatives), gate groups 100 %, 95.6 %, 83.3 %, slice 147/147. Quick
  fixes for 5.5 and 5.6: xs:anySimpleType value constraints
  (au-props-correct.2 and the others), ##definedSibling with substitution
  groups. schema.h's comment on undelivered documents fixed (5.4's
  Proposed item). XP40 30,647, XQ40 40,034, XT40 7,990 unchanged.

- 5.4 done (D170): schema documents read and assembled. READ walks each
  document's arena tree (no recursion) with one table of schema element
  roles: the schema for schema documents' attributes and child slots and
  the component each is read into, reported with the src-* constraints;
  conditional inclusion, duplicate ids, the assembly through the resolver
  (read once, chameleon inclusion, xs:redefine and xs:override last,
  innermost first, the xml: namespace known). RESOLVE: references,
  base/item/member types, substitution groups, element types, cycles.
  The text form (`src/schema_text.c`) with 5.6's content models;
  `sxml_schema`'s quintet and find functions. sxml-xsd-test compiles every
  schema: schema tests 15,163 of 15,329 in scope (98.9 %, packet gate
  85 %), no crash, leak or limit in READ, slice 0 to 110; instance tests
  wait for 5.7. Proposed: schema.h on documents the resolver does not
  deliver (XSD 1.1 makes them no error).

- 5.5 done (D171): simple types and facets. Casting to a registered
  schema type is its whiteSpace facet, the built-in lexical mapping, then
  every facet of it and its registered bases (new `src/cast_facet.c`, part
  of the cast engine; FORG0001 whose description starts with the
  constraint name); the regex engine's XSD dialect (`is_xsd_pattern`);
  `sxml_type_registry_register_schema_type`; SIMPLE_TYPES in derivation
  order on an explicit stack, with facet applicability, canonical facet
  values, the valid-restriction and consistency rules (built-in types'
  implicit facets included), NOTATION rules and attribute value
  constraints. Schema tests (release): NIST 3,953/3,953, MS-Regex
  2,590/2,590, MS-DataTypes 2,240/2,247, MS-SimpleType 322/334, Saxon
  Simple 65/65 (the failures left are the reader's and complex types');
  instance tests wait for 5.7. XP40 30,647, XQ40 40,034, slice 198:
  unchanged.

- 5.6 done (D172): complex types and content models. COMPLEX_TYPES
  (`src/schema_complex_type.c`) derives content types, attribute uses and
  wildcards (union, intersection, cyclic attribute groups) and assertions
  base first, checks cos-ct-extends, derivation-ok-restriction,
  ct-props-correct, p-props-correct and e-props-correct 2, 4, 6, 7,
  registers each type and answers derivation questions. Content models
  (`src/schema_content_model.c`): Glushkov positions plus subset
  construction, small repetitions expanded, large ones counted (counter
  ambiguity expanded and recompiled), UPA as determinism, all groups as
  counted transitions, open content and substitution groups in the
  matcher, restriction of content as a walk over configuration pairs, a
  text form. Unit tests: D167's three schemas and own cases. Schema tests
  of its groups with 5.4's reader (instance tests wait for 5.7): MS
  Particles 851/853, ModelGroups 377/391, Group 215/218, ComplexType
  546/550, Wildcards 313/315, Sun MGroup 40/40, CType 31/31, Saxon All
  62/62, Open 49/51, IBM allGroup 9/9, openContent 24/24 (the rest
  mostly the reader's). Proposed: a simple base for complex types in the
  registry.

- 4.18 done (D169): Phase 4 gate review (D11). **XSLT works**: XT40 in
  scope 7,990 of 8,728, **91.5 %** (gate 70 %), every core set 88.5 % or
  more (gate 80 %); XP40 30,647 (97.1 %) and XQ40 40,034 (97.2 %), no
  regression; 0 crashes, leaks or errors; slices 198, 186, 147. Fuzzing
  300 s per target, seven targets with the new `xslt_fuzz`: no crash,
  leak or sanitizer report (timeouts of slow suite inputs only). The
  XSLT driver's 400 M step budget is a fair limit (json-to-xml-013 is
  linear, about 900 steps a city); match="$var" is quadratic (Proposed).
  Fixed: XSLT error locations (expression ASTs placed in their module;
  a dangling source name after call-template and call-function runs),
  err:XTMM9000's prefix, `sxml transform`'s "../" with a relative
  stylesheet (the working directory in its base URI); merged thirteen
  whitespace-byte copies and seven add_expression copies. xslt.h is
  stable. Every gate box ticked (agent review).

- 5.3 done (D168): the XSD 1.1 test driver `sxml-xsd-test`
  (`tests/xsd/driver/`): reads `third_party/xsdtests/suite.xml` and its
  test sets (named after their files, `saxon-Assert`; tests
  `GROUP/TEST`), decides each test by its version tokens (per element,
  per token class), status and expected outcome, and runs it in a
  processor of its own through the test budget, reading its documents.
  41,518 tests in scope (D165 counted 41,538 over the union of the three
  levels' tokens), all resolve, all "not run: XSD not implemented" until
  5.4 and 5.7. The QT4, XSLT and XSD drivers now share the test budget,
  the test selection over a test-catalog interface, the slice-test
  script, catalog namespaces and report areas. `tools/run-xsd.sh` runs
  the suite in 45 s (release); dashboard section XSD11 with contributor
  areas and the proposed gate groups (datatypes 30,598, structures
  8,626, XSD 1.1 2,294). Slice 147 tests, baseline 0. XP40 unchanged
  (30,647 passed, 0 errors), torture slice 198/198.

- 4.17 sweep 3 done (D166): content streams into constructors. A
  document or element constructor evaluates its content as a delivering
  task, so values are added as they are made, and a constructor in
  content builds into its receiver's tree instead of making a tree to
  copy; content LETs let every instruction of a sequence constructor
  stream; XSLT literal result elements, xsl:element, xsl:document,
  temporary trees and XQuery computed and direct constructors compute
  their content inside the constructor. Arena nodes 72 to 56 bytes. The
  XSLT driver keeps result documents only for assert-result-document and
  allows 4 x the QT4 step budget. json-to-xml-013 passes (about 410 MB,
  was 2.1 GB): XT40 7,987 of 8,728 in scope, **0 errors** (gate item "no
  error outcome" met), no set lost a case; XP40 30,647, XQ40 40,034, 0
  errors; slices 198, 147. Open: XTDE3052 on reading an abstract variable
  needs globals computed on first read (Proposed).

- 5.2 done (D167): the schema components and compile stages in
  `src/schema_internal.h`: one index-addressed store (a table per
  component kind of XSD 1.1 Part 1, 2.2, plus references, lists, texts,
  namespace contexts and XPath records), "as read" fields with QName
  references that RESOLVE turns into indexes, built-in components at
  fixed indexes (types 1-54 = the registry's built-ins), six stages in
  six files (read, resolve, simple types, complex types, identity
  constraints, assertions), the content-model automaton plan (Glushkov
  with counters, UPA as determinism, all groups as counted tables), and
  the validator's seams; three schemas (purchase order, recursive
  substitution group, XSD 1.1 with assertions, alternatives, open
  content) written out in text form. `src/schema_components.c` (store,
  name index, blocks, built-ins, reports) with
  `tests/schema_components_test.c`; stubs for 5.5 to 5.9. Nothing
  validates yet; conformance unchanged (no code path of XPath, XQuery or
  XSLT touched).

- 5.1 done (D165): Phase 5 planned in `docs/tasks/phase-5.md` (14
  packets, waves, gate proposal, the `sxml validate` design). New
  `include/sxml/schema.h`: compiled schemas, a schema validator that is a
  tree builder filter (parser, tree copy or construction in, annotated
  events out), `sxml_schema_validate_node`, XSD constraint names as error
  codes, a report sink. Substrate additions with D165: facets and complex
  content types in `types.h` descriptions, typed values derived from
  annotations (`tree.h`, `xdm.h` comments), resource kind
  SCHEMA_DOCUMENT (`processor.h`), internal schema type registration
  declared in `src/types_internal.h`. W3C XSD suite vendored in
  `third_party/xsdtests` (54 MB): 41,538 tests in scope for XSD 1.1.
  Nothing implemented yet; all ctests pass under debug, debug-gcc, wasm.

- 4.17 sweep 2 done (D163): XSLT conformance. Zero-length parentless
  text nodes (XDM 4.0, 7.5.7.1) and CDATA requests (the cdata attribute,
  11.4.3) in the tree builder contract (tree.h, additive); sized arena
  trees (a small constructed tree 1.3 KB, was 5 to 9 KB) and a JSON key
  cache; a hashed evaluation memo and xsl:function cache="yes"; the static
  context of a call at run time (unit modules with base URI, namespaces,
  collation and decimal formats, named by STATIC_CALL and constructors);
  keys, decimal formats, namespace aliases, character maps and output
  definitions local to a package; XTDE0160, XTSE1650 and XTSE1660 in one
  place each and one system property table (D161's D162 items); merged
  helpers (slot atoms, 16 whitespace trims, declared types, two
  heapsorts); lint at HEAD fixed. Release, whole XT40: in scope 7,879 to
  7,986 of 8,728 (90.3 % to 91.5 %), errors 3 to 1 (json-to-xml-013,
  Proposed: content that streams into a constructor); value-of 53.3 % to
  93.3 %, so every core set is at 80 % or more. XP40 30,647, XQ40 40,034
  passed, 0 errors; slices 198, 186; XSLT slice 147 of 154.

- Docs pass (owner's request): `README.md` is the project's front page
  (what sxml does, where it runs, quick start, a C and a JavaScript
  example, conformance, design, status); `docs/CLI.md` is a user
  reference with one section per command, true outputs and exit codes,
  the D44 rules kept under "Design". `sxml` help: plain one-line
  summaries, short paragraphs ending in the exit status, examples on the
  same sample files; the program help gains a description and examples.
  Every example was run on a release build. No LICENSE file exists yet.

- 3.15 done (D161): Phase 3 gate review (D11). **XQuery works**: XQ40 in
  scope 97.2 %, XQ-only 98.8 %, method-* 433 of 508, 0 errors, no crash
  or leak; XP40 30,646 passed (unchanged), in scope 97.1 %; slices
  198/198 and 186/186 under every preset, run locally (D123). Fuzzed six
  targets 300 s each, the new `tests/fuzz/xquery_fuzz.c` among them;
  nothing found. Fixed: the lowering machine refuses to run inside one of
  its own steps (D71, D151's follow-up); one heapsort
  (`src/sort_internal.h`) for four copies; one parameter document reader
  for XQuery and XSLT; fn:load-xquery-module in the run-time function
  table (fn:function-lookup, supports-dynamic-xquery: XQ40 +5, 40,030).
  `xquery.h` is stable. D162 check: the delayed features are each in one
  place except three XSLT ones (two capability tables, XTSE1650/XTSE1660,
  XTDE0160), listed in D161 for Phase 4. Left at HEAD for Phase 4 code:
  three lint findings (`src/xslt.c`, `src/xslt_read.c`,
  `tests/xslt/driver/xslt_assertion.c`), being fixed separately.

- 4.17 sweep 1 done (D159): XSLT conformance. Numbering alphabets and
  symbol sequences (circled, parenthesized, full-stop digits, dingbats,
  ideographs, historic numbers, Greek letters) in the shared picture
  engine, for fn:format-integer and xsl:number; key and accumulator
  builds share one kept-build step (a self-dependent key is XTDE0640, it
  ran out of memory); one guarded-match helper replaces four copies and
  lets circularities through (6.5); a focus-independent predicate is
  computed once (`$s[$k]` was quadratic); xsl:analyze-string takes its
  gaps from the regex search (internal row analyze-segments) instead of
  fn:substring; fn:map-for-key; function-available sees stylesheet
  functions; one snippet-text module for keys and accumulators. Release,
  whole XT40: in scope 7,585 to 7,737 of 8,728 (86.9 % to 88.6 %), errors
  7 to 3; number 57.5 to 97.4 %, result-document 94.0 to 95.3 %. XP40
  30,646 and XQ40 40,025 passed (unchanged), qt4 slice 198. Proposed:
  empty parentless text nodes (value-of, seqtor), the static context of a
  call at run time (base URIs, names given as strings).

- 3.15 claimed (D161): the Phase 3 gate review (D11). Gate numbers in
  `docs/CONFORMANCE.md`, review fixes in small hunks of Phase 3 files,
  the ledgers, `CHANGELOG.md`, the gate boxes of `docs/tasks/phase-3.md`.

- 4.16 done (D160): XSLT packages, with D148's design: one AST holds
  every package (xsl:use-package reads the used package in place, once
  per use, parsed once), one lowering lowers them all, and the package
  table (`src/xslt_package.c`) binds each reference: component instances
  per package with their visibility (xsl:expose, xsl:accept), overriding
  instances with xsl:original, successors in the using packages. The
  lookups of functions, attribute sets, globals (per-package views),
  named templates and modes (each package its own unnamed mode) bind
  through it; abstract components raise XTDE3052; the XTSE30xx checks
  including signatures (XTSE3070); version ranges, the driver serves the
  highest matching version; an xsl:stylesheet is an implicit principal
  package (D157's Proposed item). Release, in scope: package 53 -> 68/72,
  override 30 -> 91/102, accept 33 -> 48/50, expose 12 -> 42/42,
  package-version 25 -> 33/34, use-package 13 -> 34/53 (34/45 run);
  XT40 7,585 -> 7,864. Still stylesheet-wide: keys, decimal formats,
  aliases, character maps, outputs (Proposed).

- 4.17 sweep 1 claimed (D159): XSLT conformance. Numbering sequences in
  the shared picture engine, the run-time function table (key,
  accumulators, stylesheet functions), names given as strings against
  each expression's namespaces, host collations, priority patterns,
  record and type aliases, missing functions, the "error" outcomes.
  Files: `src/format.c`, `src/xslt*.c` (not `src/xslt_package.c`),
  `src/function_library_*.c` (small hunks), `tests/xslt*`,
  `docs/CONFORMANCE.md`.

- 4.15 done (D157): XSLT works through the public API and the CLI.
  `src/xslt.c` implements `include/sxml/xslt.h` on the shared unit
  evaluation (now with an extension: XSLT function table, strip-space
  document preparation, result writer); the host's template and tunnel
  parameters, initial match selection and initial function arguments are
  invocation globals bound by name; a result document without href is the
  principal result. `sxml transform STYLESHEET [INPUT]` with --param,
  --initial-template, --initial-mode, --method (21 golden tests);
  `tests/xslt_test.c` runs 26 transformations and an allocation-failure
  loop (which found three failure-path bugs in the XSLT lowering, fixed).
  The driver executes cases (`xslt_execution.c`, assertions over the QT4
  checks), `tools/run-xslt.sh` and XT40 in `tools/conformance-report.py`.
  Release run of the whole suite: in scope 7,585 of 8,728 passed
  (**86.9 %**, gate 70 %), 785 failed, 7 errors (memory and step limits),
  no test set crashed; core sets apply-templates 97.4, mode 87.3, match
  92.8, for-each-group 90.7, sort 100, key 97.1, number 57.5 (114
  in-scope cases need numbering combinations such as circled digits),
  choose 95.4, copy 88.4, value-of 53.3 (7 wrong results), result-document
  94.0, function 89.5 %. Slice 0 to 140 of 154; XP40 30,646 passed (the
  dashboard's 30,630), qt4 slice 198. `debug`, `debug-gcc` 227/227,
  `wasm` 229/229, lint clean. CLI wasm budget raised (Proposed).

- 4.14 done (D155): accumulators. xsl:accumulator rules are match
  functions (D118) and rule functions (as templates, D144); per
  accumulator a traversal function from an XPath text, a fold-left over
  the tree with an explicit stack (no recursion), giving maps of pre- and
  post-descent values; fn:accumulator-before/-after build them lazily per
  tree through the evaluation memo (D149: XTDE3400 for a cycle, errors
  raised again on each call). use-accumulators of the initial mode and of
  xsl:source-document (now lowered, without streaming) restrict a tree
  (XTDE3362); copies by fn:copy-of, fn:snapshot and xsl:copy-of
  copy-accumulators="yes" keep the original's values. Scratch harness
  (release): decl/accumulator 4 to 68 of 69 (accumulator-053 only fails
  in the harness: a text-method assertion), misc/error 355 to 370,
  attr/mode 109 to 121, insn/copy 124 to 126, attr/capture 0 to 1; no
  case went from pass to fail. Not done: streaming, merge-source
  use-accumulators, XTDE3350.

- 3.14 sweep 2 done (D158): XQuery conformance. Default functions
  (glossary): a declared function's default is an IR function over the
  caller's focus, called by static calls at the full arity (keyword
  arguments after omitted ones, modules-40) and by focus-capturing
  closures for F#N; XQuery closures read globals at call time (self-
  referencing variables); fn:function-lookup finds the unit's functions;
  "##any"; IR modules (glossary) give library functions their base URI
  and source name; trusted=false gives FODC0016; function annotations on
  IR functions; keyword placeholders in written order; untyped array
  keys; QT4 assert-eq atomizes. XQ40 39,928 to 40,025 (in scope 96.9 % to
  97.2 %), XQ-only 9,325 to 9,404 of 9,523 (98.8 %), errors 1 to 0, none
  lost; XP40 30,630 to 30,646 (97.1 %), 0 errors; slice 198/198.
  CONFORMANCE.md regenerated at 5611dd2.

- 4.11 done (D153): numbering and analyze-string. `src/xslt_lower_number.c`:
  xsl:number as 12.3's formal definition in XPath texts (count/from
  patterns as inline match-expression FILTERs, an error is no match),
  value (XTDE0980), start-at, formatted by one internal row
  (format-place-marker) over fn:format-integer's engine (format tokens,
  separators, prefix/suffix and grouping added to `src/format.c`);
  xsl:analyze-string over fn:matching-segments with the gaps put in,
  XTDE1140/1145 mapped. fn:regex-group and fn:regex-groups read the
  current captured groups as a context variable: a new parameter default
  `SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE` (lexical; templates called from
  matching-substring see none). Harness (in scope): insn/number 0 to
  152/161, analyze-string 0 to 56/62, misc/regex 64/65, format-number
  27 to 67/70 (with 4.12). XP40 30,630 unchanged, slice 198/198.

- 4.9 done (D152): sorting, grouping, merging. xsl:sort is one SORT key
  per xsl:sort (composite keys, order and collation computed when
  evaluated; data-type, lang/case-order as a UCA collation) under
  for-each, apply-templates, perform-sort and for-each-group;
  xsl:for-each-group makes grouping pairs and calls the new row of
  `src/xslt_group.c` (group-by through XQuery's group-by row, D137), then
  a MAP over group arrays; xsl:merge on the same parts. current-group and
  the other context functions are lexical variables in the XSLT
  namespace (`src/xpath_lower.c`). Evaluator SORT: descending reverses
  the empty order (3.10's Proposed (a)), any supported collation (D116's
  last codepoint-only body). Harness: insn/sort 74/82, for-each-group
  79/86, merge 61/88 (needs xsl:function, schemas, collections); QT4
  prod-EmptyOrderDecl 28 to 32, other order/group sets unchanged. Not
  done: split-when, merge-when, XTDE2210/2220/3490, a dynamic current
  group in called templates.

- 4.13 done (D156): output and result documents. xsl:output merged into
  output definitions (XTSE1560) and read by the serialization parameter
  reader over the parameter document, as XQuery's output declarations;
  xsl:character-map expanded on an explicit stack (XTSE1580/1590/1600) and
  fn:character-map; xsl:result-document as two internal rows around its
  content (href, format, value templates over the definition, XTDE1460,
  XTDE1490, XTDE1480 lexically and through a tunnel mark), the result
  writer (principal result delivery, current output URI) and the public
  result document collector, both for 4.15 to call. Files:
  `src/xslt_output.c`, `src/xslt_output_internal.h`,
  `src/function_library_output.c`, `tests/xslt_output_test.c`. Scratch
  harness: decl/output 235/244, character-map 32/33, result-document
  129/149, xsl-document 23/25, current-output-uri 6/16 (before: none
  lowered). Not done: XTDE1500, clearing current-output-uri in functions,
  globals and patterns.

- 4.12 done (D150): xsl:function as IR functions (one per arity, a
  function table in front of every static context, optional parameters
  and their defaults, eclipsing, override-extension-function, functions
  in no namespace, XTTE0780), attribute sets (one IR function of the
  focus per name; literal result elements, xsl:element, xsl:copy;
  XTSE0710/0720), xsl:decimal-format into the unit's static context,
  xsl:global-context-item, whitespace stripping of source documents (a
  filtering copy; fn:doc through a new document preparation of the
  document loader), and on 4.7's globals: initializers may call
  templates, and the order follows the functions, templates and
  attribute sets they reach (strongly connected components). Scratch
  harness: function 98/124, attribute-set 47/49, strip-space 25/26,
  global-context-item 10/12, context-item 29/32, param 31/31, variable
  90/108, whitespace 24/27, format-number 66/70; 4.8's sets 474/614
  (was 316/615). Not done: character maps (4.13), memoization (cache).

- 3.14 sweep 1 done (D154): XQ40. fn:load-xquery-module without C
  nesting: global tables per unit in the dynamic context and a unit
  evaluation native step (`src/evaluator.c`, `src/evaluator_global.c`);
  the row in `src/xquery_load_module.c`, after the host's table for XQuery
  compilations only; the module record is a generated main module of
  named function references (18.1). QT4 driver: out-of-scope cases that
  stop at a missing feature are not run (misc-HtmlTestSuite), a param's
  prefix is its element's; arena tree blocks 4 KiB (655 MB for 10,000
  constructed elements). QT4 (release, 00ac981): XQ40 39,698 to 39,928,
  in scope 96.4 % to 96.9 %, XQ-only 85.3 % to 97.9 %, errors 5 to 1
  (modules-40); fn-load-xquery-module 103/117 run, misc-Subtyping 109/110;
  method-* 97.5 %; XP40 30,630 (97.0 %, 0 errors) and slice 198/198
  unchanged. Left in Proposed: closures reading globals at call time and
  focus-reading defaults (`src/xpath_lower.c`), keyword arguments after an
  omitted declared default, a library module's base URI at run time.

- 4.10 done (D149): keys and XSLT functions. `src/function_library_xslt.c`,
  the XSLT-only library part (system-property and the other static-expression
  functions moved from `src/xslt_static.c`, current, document with the 4.0
  options map, unparsed-entity-uri/-public-id); `src/xslt_key.c`, the XSLT
  function table in front of every static context: fn:key#2/#3 lowered from
  XPath texts over the xsl:key declarations (map:build index per key and
  tree, built lazily once per evaluation through the new evaluation memo of
  the dynamic context; composite keys, collations, numbers as eq compares
  them), fn:copy-of, fn:snapshot. current() is a parameter default the
  lowerer reads from the embedding's focus (patterns: the item matched).
  Patterns whose predicates never count positions no longer select the step
  again (attr/match 12 s in debug; match-242 was 60 s alone). Scratch
  harness (lower, evaluate, serialize; assertions through `sxml xpath`):
  fn sets key 75/91, document 64/98, system-property 20/23, snapshot 7/19,
  function-available 4/10, copy-of 3/8, type-available 1/2, current 2/2 of
  applicable (before: 4 of 290 with the harness's first checker, before
  4.7). Left: xsl:function (4.12), xsl:sort (4.9), stylesheet parameters,
  fn:apply-templates, current-output-uri; runtime function table (Proposed).

- 2.20 done (D151): Phase 2 gate review (D11), checked locally (D123).
  Clean HEAD c7c0c35: `release`, `debug`, `debug-gcc`, `wasm` (Node.js
  package, size budget) and lint pass, no call cycle. QT4 (release, code
  ed225c2): XP40 30,630 passed, 0 errors, in scope 97.0 %, every area
  above 95 % (fn 95.5, op 99.2, prod 97.8, map 99.7, array 99.8, math
  100, xs 99.5); slice 198/198; XQ40 39,541, in scope 96.0 %, XQ-only
  83.9 %, 1,568 errors (module import). Fuzzing, 300 s per target and
  300 s more for the XPath ones: no finding. Merged: one options reader
  for element maps and text resources, one option-table macro, one
  leap-year and days-in-month rule (four copies), the unused
  codepoint-only collation check removed; outcomes identical case by
  case. `xquery.h` and `xslt.h` provisional until their gates; the second
  D120 is D120b; Proposed keeps the owner's items and Phase 3 design
  work. Gate boxes ticked but the tag. Follow-ups in D151 (lowering
  re-entry guard, deep-equal's options, calendar arithmetic); open
  questions at the end of this file.

- 3.11 done (D148): library modules and module import. `src/xquery_module.c`
  loads the module set through the resolver (one request per location
  hint, an empty one for none; module URIs end cycles, a module loads
  once); every module gets its own static context, read imports first,
  with its variables as globals of the one unit
  (`sxml_xpath_static_context.global_numbers`); imports name namespaces
  (5.12.2), %private hides; XQST0033, 0034, 0047, 0048, 0049, 0059, 0070,
  0088, 0108, 0113, XQDY0054 across modules. `sxml xquery` imports local
  files. XQ40 (release, all sets) 39,541 to 39,698 passed, none lost;
  prod-ModuleImport 23 to 112 of 141. Not done: fn:load-xquery-module
  (needs cross-unit globals in the evaluator, Proposed), a library
  module's base URI and source name at run time (Proposed).

- 4.7 done (D145): control flow and variables lowered into kinds XQuery
  already uses: xsl:if (then/else), choose, switch (CONDITIONAL chains),
  for-each (MAP, separator), iterate as fn:scan over a closure (no
  recursion; next-iteration/break as an iteration marker, on-completion,
  XTSE3120 checked), local xsl:variable, try/catch with the $err:
  variables, where-populated, on-empty/on-non-empty (internal rows
  vacuous and remove-deemed-empty), message and assert (XSLT_MESSAGE
  trace events, error-code resolved), fork, evaluate (fallback or
  XTDE3175), global variables and parameters (dependency order,
  XTDE0640, XTSE0630, XTDE0050). Scratch harness over the twelve sets:
  105 of 181 runnable cases pass (before: none of the instructions
  lowered); the rest wait for template parameters, functions or XPath
  assertions. debug, debug-gcc, wasm, lint clean (HEAD's xpath_static
  failure is 2.19's pending test update).

- 2.19b done (D147): counted repetition whose copies would pass 256
  instructions runs as a counted loop (the atom once, a counter slot on
  the backtrack stack's restoring entries), so `a{2147483647}` is six
  instructions; small counts keep their copies. QT4 XP40 (release)
  30,628 to 30,630 passed, errors 2 to 0 (cbcl-matches-004, -038, the
  last "error" outcomes); fn-matches 198 to 200, fn-matches.re 1,082,
  fn-replace 187, fn-tokenize 71, fn-analyze-string 30,
  fn-matching-segments 38, fn-regex 29 unchanged; slice 198/198; debug,
  debug-gcc, wasm pass, lint clean.
- 4.8 done (D144): templates and modes. APPLY_TEMPLATES, XSLT's one IR
  extension point, completed (parameter maps, rule search, separator;
  modes with ordered rules and a built-in template) and evaluated by
  `src/evaluator_template.c` as one part task whose match and template
  calls run on the machine (errors while matching are no match; 100,000
  levels of apply-templates tested). `src/xslt_lower_template.c`:
  template functions with xsl:param and context-item, call-template
  (STATIC_CALL; absent-focus variants), apply-templates, next-match,
  apply-imports, tunnel parameters, modes, conflict resolution, built-in
  templates from XPath snippets, the invocation. Scratch harness over the
  packet's sets: 316 of 615 applicable cases pass (before: none ran);
  most of the rest wait for 4.7 (globals, parameters), 4.9-4.12, packages.
  XP40 30,628 and slice 198/198 unchanged; debug, debug-gcc, wasm, lint
  clean.

- 3.12 done (D146): "working XQuery". `include/sxml/xquery.h` in
  `src/xquery.c`, compiled with the XPath API's static context and
  evaluated by its unit evaluation (`src/xpath_api_internal.h`); output
  declarations read as text by the serialization parameter reader over
  the parameter document (SEPM0016, XQST0115, XQST0119 static). The QT4
  driver uses the public header (adapter deleted). CLI: `sxml xquery
  (QUERY-FILE | --expression)`, `sxml serialize`, `sxml xpath --method`
  (adaptive, xml, xhtml, html, text, json). QT4 XQ40 (release, before =
  a13b17f with the adapter): passed 39,174 to 39,539; gate 95.1 % to
  96.0 % (39,521/41,174); XQ-only 81.0 % to 83.9 %; method-* passed 147
  to 433 of 444 run; errors 1,574 to 1,570 (1,379 misc-HtmlTestSuite, 94
  module imports). XP40 30,628 (unchanged), slice 198/198, xquery-slice
  186. debug, release, debug-gcc, wasm pass. The wasm CLI is 3.45 MB
  stripped with the XQuery grammar; budget raised for the CLI only
  (Proposed). D95's XQ40 numbers are met.

- 2.19 sweep 1 done (D143): computed constructors lowered in XPath 4.0
  (the construct part serves every language, 136 "error" outcomes gone);
  the JSON fallback option (parse-json, json-to-xml, json-doc as resumable
  bodies; json-doc's row moved to the json part); generate-id of JNodes;
  0.0 eq 0e0; a descending integer range (additive to xdm.h, D99) so
  reverse(1 to 10^11) is O(1); deep-equal of IR functions by static form
  (the text printer with nested definitions inlined); named focus
  closures (fn:last#0 has a name); math:sin/cos/tan $radians; QT4 driver:
  result as the assertions' context value, malformed uri-only sources
  unavailable. XP40 (release) 30,389 to 30,628 passed (+239, none lost),
  errors 187 to 2 (cbcl-matches-004/-038, regex expansion limit), in
  scope 96.3 % to 97.0 %; prod 96.6 to 98.1 %, fn 98.3 to 98.9 %; slice
  198/198. docs/CONFORMANCE.md regenerated.
- 3.10 done (D135): prologs. `src/xquery_static.c` reads the prolog into
  one XPath static context (namespaces, `local`/`output`/`xq`, default
  namespaces with a function namespace rule, setters, base URI, default
  collation and decimal formats into the unit's static context, output
  declaration names, named item types as aliases, nominative records with
  constructors, declared functions as IR functions per arity, variables
  as globals); `src/xquery_prolog.c` lowers bodies, defaults,
  initializers and the query body (fixed focus on the context global),
  orders globals by a static dependency walk (XQDY0054) and holds
  `sxml_xquery_lower`/`_compile`; `src/evaluator_global.c` computes them
  as calls on the machine. QT4 XQ40 (release; before, every prolog query
  was an "error"): VarDecl 138/142 run, VarDecl.external 95/97,
  FunctionDecl 210/227, Annotation 87/88, ItemTypeDecl 65/78 and
  NamedRecordTypeDecl 77/92 (9 each need module import, 3.11),
  DecimalFormatDecl 45/46, DefaultNamespaceDecl 71/80, NamespaceDecl
  52/54, ContextItemDecl 34/43 (8 module imports), ContextValueDecl 13/13,
  OptionDecl.serialization 32/38, EmptyOrderDecl 28/32 (SORT, Proposed);
  setter sets 100%. XP40 30,389 (unchanged), slice 198/198; debug,
  debug-gcc, wasm pass, lint clean. Follow-ups under "Proposed" (D135).

- 4.6 done (D142): sequence constructors and node construction lowered
  into the shared IR by the one lowering machine (procedures pushed per
  instruction, XPath through the embedding entry): literal result
  elements (namespace nodes, exclusion, xsl:namespace-alias), value
  templates with 5.7.2 simple content (fixes D120's deviation: adjacent
  text nodes merged), xsl:sequence, select, element, attribute, text,
  value-of, comment, processing-instruction, namespace, document, copy,
  copy-of, map, map-entry, record (new AST kind, D139's Proposed item),
  array, array-member, fallback; XSLT error codes by TRY_CATCH mapping.
  No IR kind, evaluator or public header change. Scratch harness over
  lre, element, attribute, text, value-of, copy, construct-node,
  sequence, array, xsl-record (436 applicable): 78 pass, 2 fail (DTD
  entities), 356 need 4.7/4.8/4.10/4.12. debug, debug-gcc, wasm,
  clang-tidy, ast-grep and the call-cycle check clean; XP40 untouched.

- 2.15 done (D141): fn:parse-csv, fn:csv-to-xml, fn:csv-doc. `src/csv.c`
  is a hand-written scanner (D97): one token per call, a plain cursor, no
  allocation; `src/function_library_csv.c` builds the record (get as a
  function item over captured rows and column index, D133), csv-to-xml
  walks that result, csv-doc goes through the fn:unparsed-text row (D131).
  fn:csv-to-arrays was dropped from F&O 4.0 (issue 2961) and is left out.
  XP40 (release) before to after: fn-parse-csv 35 (wrong errors) to
  107/124, fn-csv-to-xml 9 to 68/68 run, fn-csv-doc 4 to 20/20. Full run:
  XP40 30,389 passed, slice 198/198. The other 17 parse-csv cases need the
  driver to bind the result as the context value of assertions (Proposed;
  124/124 with it, +20 over XP40). HEAD's QT4 driver still does not build
  (xquery_adapter.c of 3.13); measured with a local patch.
- 2.18 done (D138): JavaScript package `bindings/javascript/` (npm name
  `sxml-js` for now; `sxml` is taken): C shim over the public API, linked
  into the reactor module `sxml.wasm`; hand-written TypeScript (parse,
  compile/evaluate XPath with variables, namespaces and context,
  serialize with fn:serialize options, resolver callback, SxmlError,
  dispose + FinalizationRegistry), own minimal WASI imports for Node.js
  and browsers (checked in headless Chromium). ctest javascript/package
  (21 tests: leak checks, allocation-failure loop), javascript/typecheck
  (after npm ci), javascript/size_budget. Version 0.2.0, CHANGELOG.md,
  docs/RELEASE.md; not tagged or published (owner). Size: CLI module
  1.82 MB stripped / 630 KB gzip, past D93's budget; budget set to 2.0 MB
  / 700 KB, see "Proposed". No library change: XP40 and slice 198/198
  unchanged; debug, debug-gcc, wasm, lint pass.

- 2.11 done (D140): fn:hash (MD5, SHA-1, SHA-256, BLAKE3, CRC-32 in
  `src/hash.c`, a new hash part), fn:decode-from-uri, fn:parse-uri and
  fn:build-uri (uri part; one percent decoder, one escaping table),
  fn:parse-QName and fn:resolve-QName (qname part). XP40 (release) per set
  before to after: fn-parse-uri 0 to 74/74, fn-build-uri 0 to 48/48,
  fn-decode-from-uri 0 to 29/29, fn-parse-QName 6 (wrong errors) to 19/19,
  fn-resolve-QName 4 to 8/8 run (23 XQuery-only), fn-hash 1 to 54/54. Full
  run with these: XP40 29,766 passed, slice 198/198, no set below the
  committed dashboard. Seen at HEAD 9f5168d, not 2.11's: the QT4 driver
  does not build (xquery_adapter.c of 3.13 against 2.12's processor
  changes: mpdecimal.h include path, document loader configuration).
- 4.5 done (D139): the stylesheet reader. `src/xslt_ast.c` (builder,
  import precedence, text form), `src/xslt_read.c`
  (`sxml_xslt_read_stylesheet`: modules through the XML parser, one
  explicit stack over all modules, element and attribute tables from the
  4.0 schema and syntax summaries, static contexts, 3.11.1 stripping,
  simplified modules, include/import with 4.0's include-cycle rule, XTSE
  checks), `src/xslt_static.c` (use-when, static variables and shadow
  attributes through the one evaluator, XSLT's five static functions).
  Scratch harness over the 8,712 in-scope xslt40-test cases: 6,933 read,
  348 fail with an expected code, 1,323 read but expect a later
  (dynamic or lowering) error, 108 wrong: 55 `~T` type patterns the
  pattern grammar lacks, 17 xsl:record (no AST kind), 36 single causes.
  Sets: use-when 97/102, static 39/48 (6 later errors), shadow 10/10,
  include 21/23 (embedded modules), import 41/41, forwards 23/23. No
  sanitizer report over the whole run; debug, debug-gcc, wasm,
  clang-tidy and ast-grep clean on the new files. XP40 untouched (no
  shared file changed).

- 2.17 done (D136): node, sequence, string and type functions. Node part:
  siblings, innermost, outermost, nilled, lang, id, element-with-id,
  idref, path (every 4.0 option, JNode keys), location; the name functions
  take xnode()?; fn:doc takes fn:parse-xml's options. String part:
  pad-string, index-of-substring. Sequence part: starts-/ends-/contains-
  subsequence (resumable). JSON part: parse-xml-fragment, strip-space=
  "all" (a stripping tree builder), system-properties. New type part:
  type-of, schema-type, atomic-/node-type-annotation (schema-type
  records over the registry). One options reader for every options map
  (`src/function_library_options.c`). Collations in min/max, highest/
  lowest, sort-by, array:sort(-by). XP40 (release, same machine) 28,476
  to 29,593 with the others' work since; per set before to after:
  fn-pad-string 2/22 to 22/22, fn-index-of-substring 1 to 21/21,
  fn-siblings 3 to 18/18, fn-innermost 8 to 57/58, fn-outermost 8 to 55/55,
  fn-nilled 5 to 14/14, fn-lang 6 to 38/38, fn-id 7 to 17/17, fn-idref 7 to
  10/10, fn-location 1 to 5/5, fn-path 6 to 60/60, fn-doc 42 to 54/60,
  fn-parse-xml 27 to 31/42, fn-parse-xml-fragment 9 to 36/39,
  fn-system-properties 1 to 23/23, fn-type-of 0 to 31/31, fn-schema-type 0
  to 9/9, fn-*-type-annotation 0 to 4/4, fn-starts-/ends-/contains-
  subsequence 12 to 95/95, fn-highest 29 to 30/30, fn-lowest 29 to 31/31,
  fn-sort-by 50 to 56/56, array-sort 27 to 29/29, array-sort-by 43 to
  48/48. Lost: fn-function-lookup-350/354/358 (function-lookup of the
  focus-dependent id#1, element-with-id#1, idref#1 is XPDY0002, D115's open
  lowering item; they passed only while the functions were missing).
  Survey: missing functions are 2.15's (CSV), load-xquery-module,
  transform and D97's non-goals. Slice 198/198; debug, debug-gcc, wasm
  pass; lint clean. Proposed: the decimal-zero comparison fix in
  `src/xdm_atomic.c` (`0.0 eq 0e0` is false), a descending range.

- 3.8 done (D137): group by and window clauses. Grouping specifications
  are segment bindings, keys rebound atomized (XQST0094, XPTY0004), then
  the internal group-by row over tuple arrays (atomic-equal keys,
  collation keys, implicit timezone); window clauses are segment clauses
  like for, a MAP over the window row's window arrays; one
  condition-closure procedure serves window conditions and while (D126's
  closure code left `src/xquery_lower.c`). XQ40 (release, 3.13's driver):
  prod-GroupByClause 0 to 35/40, prod-WindowClause 0 to 125/158 (all
  errors prologs); For, Let, Where, OrderBy, Count, While unchanged. XP40
  29,472 passed, slice 198/198. debug, debug-gcc, wasm (new tests),
  clang-tidy clean. Open: `sxml_sort_finish` (src/evaluator.c) fails with
  INVALID_ARGUMENT on an empty input (use-case-groupby-Q4).

- 2.12 done (D131): resources and the evaluation environment. New part
  `resource` (unparsed-text, -lines, -available, unparsed-binary,
  json-doc, collection, uri-collection, environment variables,
  default-language); fn:trace and fn:message reach the trace sink,
  `sxml_stream_trace_sink`. Static decimal formats and collations are
  checked and copied into the IR unit and read lexically (accepting
  2.9's and 2.13's proposals), plus 2.6's O(1) range comparison. XP40
  sets (release, before at 24fc6ac): unparsed-text 54 to 94/94,
  -lines 39 to 58/58, -available 7 to 57/57, unparsed-binary 16 to
  21/21, json-doc 36 to 73/77, collection 18 to 24/25,
  environment-variable 6 to 12/12, available-environment-variables 2 to
  10/10, default-language 0 to 5/5, message 3 to 14/14, format-number
  252 to 285/286. XP40 28,791 of 30,992 run (CONFORMANCE.md 23,146);
  slice 198/198; debug, debug-gcc, wasm, clang-tidy clean.

- 3.13 done (D134): `sxml-qt4 run --spec=XQ40` runs XQuery main
  modules through a temporary adapter (`tests/qt4/driver/xquery_adapter.c`,
  the signatures of xquery.h over the internal `sxml_xquery_compile`;
  packet 3.12 replaces it). `<module>` files served for XQUERY_MODULE,
  declared="true" params bound without a host declaration, collection
  queries evaluated, serialization-matches and assert-serialization-error
  through the serializer, feature serialization satisfied; the
  conformance report gives the D95 gate rate and the XQ-only rate. First
  XQ40 numbers (release, one process per test set): 33,374 of 47,313
  passed, gate 33,356/41,174 = 81.0 %, XQ-only 4,310/10,723 run =
  40.2 %; 5,969 errors are prologs (3.10), 179 group by (3.8). XP40
  29,029 before and after. New ctest `qt4/xquery-slice` (186 XQ-only
  cases, baseline 186) and golden `qt4/run_xquery`; debug, debug-gcc,
  wasm pass.

- 2.10 done (D130): elements to maps and back, part `element_map`.
  fn:element-to-map chooses layouts in document order and builds values
  in reverse document order over per-node element slots; plans group by
  name with XDM maps as indices; fn:map-to-element on a frame stack into
  a self-destroying arena tree, xml layout through the document loader.
  fn-element-to-map 30 to 179/181 run, fn-element-to-map-plan 0 to
  20/20, fn-map-to-element 75 (wrong errors) to 224/228. XP40 passed
  28,856 of 30,992 run in the run of this commit; slice 198/198; debug,
  debug-gcc, wasm, release 151/151, lint clean.

- 2.14 done (D129): date formatting. `src/format_date.c` (module
  header `src/format_date_internal.h`) scans date pictures by hand and
  numbers components through the D119 fn:format-integer engine;
  `src/function_library_format_date.c` adds fn:format-dateTime,
  fn:format-date and fn:format-time. Every component and modifier,
  width modifiers, fractional seconds, timezones (numeric, military,
  GMT), English names, calendars AD and ISO, fallback markers
  `[Calendar: AD]` and `[Language: en]` (D97). XP40 (debug driver):
  fn-format-date 20 to 124/124, fn-format-time 19 to 94/94,
  fn-format-dateTime 5 to 80/80 (all run cases pass); XP40 (release)
  28,534 passed, slice 198/198; debug, debug-gcc, wasm, lint clean. Test
  `format_date`: engine and API cases, allocation-failure loops.

- 2.6b done (D133): the 16 built-in record types of F&O 4.0, appendix C
  (`src/function_library_records.c`), declared once per type registry
  when the function library initializes; bodies return annotated records
  (`sxml_function_library_make_record`), generator and compiled-regex
  function items take their field types as signatures; coercion to a
  nominative record type; ?take is xs:nonNegativeInteger; INF idiv 0 is
  FOAR0001. fn-parts-of-dateTime 8 to 20/20, fn-divide-decimals 19 to
  21/21, fn-random-number-generator 50 to 54/54 run, fn-regex 27 to
  29/30 (-007 needs XPTY0004 for a dynamic call on ()),
  fn-matching-segments 37 to 38/38 run, op-numeric-integer-divide wrong
  errors 2 to 0. XP40 29,029 passed, slice 198/198; debug and debug-gcc
  pass (HEAD's tests/xslt_pattern_test.c fails -Wshadow under GCC, not
  this packet).

- 3.7 done (D126): XQuery lowering part `xquery` (`src/xquery_lower.c`,
  `src/xquery_lower_internal.h`): FLWOR as clause segments (MAP, LET,
  CONDITIONAL) with order by, count and while over tuple arrays (SORT,
  the unpacking MAP's position, fn:take-while with a closure), trace,
  typeswitch, switch, try/catch, ordered/unordered, pragmas, validate
  (XQST0075), string constructors; internal `sxml_xquery_compile`.
  Scratch harness (XQ40, 0 before): For 139/212, Let 158/189, Where
  68/85, OrderBy 133/210, Count 13/13, While 69/83, Typeswitch 80/88,
  Switch 34/58, TryCatch 190/212; the rest need prologs (3.10) or group
  by (3.8). XP40 27,842 (not touched: XQuery only), slice 198/198;
  debug 150/150, debug-gcc and wasm build and pass, lint clean.

- 3.4 done (D128): HTML, text, JSON and CSV methods and fn:serialize on
  the 3.3 pipeline (escape styles, a raw-text context; HTML a mode of the
  XML node writer, `src/serialize_html.c` for void elements, boolean
  attributes, raw text, PIs; JSON frames on a heap stack, nodes by
  sub-serialization, RFC 8785 canonical form; CSV from maps or arrays);
  `sxml_serialization_parameter_reader` for maps and parameter documents
  (two arenas); SERE0006 and SERE0003 added. fn-serialize 48 to 165/194
  (32 left need expath binary, parse-csv, unparsed-text); method-* are
  XQ-only (unit cases until 3.10/3.13). XP40 otherwise unchanged (the
  driver's adaptive path is untouched); slice 198/198; debug, debug-gcc,
  wasm serialize tests pass, lint clean on the packet's files.

- 3.9 done (D127): constructor lowering, part `construct` in
  `src/xquery_lower_construct.c`. Direct elements: SEQUENCE content of one
  atom per attribute and piece of content; namespace declaration
  attributes lowered as an embedding with an extended static context;
  in-scope declarations sent by nested elements; XQST0040. Computed
  constructors with literal or computed names. Constr sets 0 to 932/1,165
  (scratch harness); all remaining run failures but 7 need prologs (3.10).

- 2.13 done (D116): collations and comparison options. `src/collation.c`:
  codepoint, Unicode and HTML ASCII case-insensitive, UCA with fallback
  (UTS #10 levels over weights from Unicode properties: strength,
  alternate, maxVariable, caseFirst, caseLevel, backwards), keys and
  substring matching by collation units; `sxml_collation_is_supported`
  (the QT4 driver uses it; FULLY-NORMALIZED accepted). One resolver for
  every collation argument; fn:collation, -available, -key; collation-
  aware compare, index-of, distinct/duplicate-values, all-equal/-different,
  sort, contains-token, substring functions. fn:deep-equal: every 4.0
  option, items-equal as a resumable body, unordered comparisons by
  backtracking trials. XP40 passed 26,170 to 26,510, none lost; FOCH0002
  failures 280 to 12; fn-compare 188 to 221/225, fn-duplicate-values
  111/113, fn-deep-equal 327 to 395/449 (function-item equivalence and
  default collation Proposed), fn-collation 0 to 89/89, fn-collation-key
  0 to 35/35, misc-UCACollation 70 to 87/87. Slice 198/198; debug,
  debug-gcc, wasm 143/143, lint clean.

- 4.4 done (D118), Early packet of Phase 4: XSLT 4.0 patterns in
  `src/xslt_pattern.c` with the module header
  `src/xslt_pattern_internal.h`. Front end `sxml_xslt_pattern_build_ast`
  on the D111 parser part seam (19 pattern kinds, field PATTERN, flag
  OPEN appended to the AST; XTSE0340 for syntax and forbidden outermost
  calls). Patterns become match expressions (XPath ASTs of xs:boolean,
  read right to left as 6.3.2.3 describes, the formal definition for
  intersect/except and filtered parenthesized patterns) lowered through
  the embedding entry; match functions for template rules; default
  priorities (6.4, all 4.0 forms). Tests: 51 patterns against a fixed
  document by table, 43 item cases (atomic, map, array, sequence
  patterns), 60 priorities, 14 static errors, match function, allocation
  failures, 3,000 nested parentheses. Suite: 19,441 pattern attributes,
  19,288 parse, all build a match expression, 18,916 lower with the
  stylesheet's namespaces alone (the rest need XSLT functions or schema
  types). `debug`, `debug-gcc`, `wasm` all pass, lint clean; slice
  198/198; XPath behaviour untouched (kinds only appended).

- 2.7 done (D125): fn:divide-decimals over a new decimal operation
  (divisor exponent shifted, libmpdec divint/rem, precision fitted to the
  digit limit). Release XP40 23,146 (docs/CONFORMANCE.md) to 26,975 with
  the others' work since; fn-divide-decimals 4 to 19 of 21; math-*,
  fn-round, fn-round-half-to-even, fn-parse-integer, op-numeric-* already
  at 100% of run cases. Slice 198/198; `debug`, `debug-gcc`, `wasm`
  142/142; lint clean. Open (Proposed): F&O named record types
  (divide-decimals-019 to -021, random-number-generator-35a to -37a);
  take(-1) as XPTY0004 needs a unit-test change.
- 3.6b done (D124): IR extensions accepted from D109/D110.
  `sxml_ir_unit_add_name_tests`, `_add_catch_clauses`,
  `_add_namespace_bindings` and accessors; `construction_flags`
  (no-preserve, no-inherit, preserves-types, xslt-rules) and
  `name_namespaces` on `sxml_ir_node_construction`, honoured by
  `src/evaluator_construct.c`; the verifier checks TRY_CATCH and the
  constructor kinds (only APPLY_TEMPLATES stays reserved); the text form
  prints clauses, bindings and flags (other dumps unchanged). 4 new IR
  cases, 10 new constructor-mode cases (also under allocation failure);
  the try and constructor tests use the adders. `debug`, `debug-gcc`,
  `wasm` 143/143, lint clean, slice 198/198 (release). Release
  `evaluator_construct_test` passes at HEAD with gcc and clang -O3, -O2
  ASan+UBSan and valgrind: the reported release failure did not
  reproduce.

- 2.8 done (D122): date and time functions in the new part
  `src/function_library_temporal.c`: adjust-dateTime/date/time-to-timezone,
  build-dateTime (record fields coerced by the body), parts-of-dateTime,
  days-in-month, seconds, duration-to-seconds, unix-dateTime,
  civil-timezone (no tz database: implicit timezone, UTC names, else
  FODT0004) and parse-ietf-date (hand scanner); the `*-from-dateTime`
  rows take every Gregorian type. XP40 26,678 to 27,185 passed in a
  release snapshot, none lost; slice 198/198. fn-adjust-* 21 to 133 of
  133, build-dateTime 49 to 90/90, parse-ietf-date 37 to 105/105,
  seconds 6 to 24/24, duration-to-seconds 4 to 18/18, unix-dateTime 4 to
  22/22, days-in-month 4 to 16/18, the seven `*-from-dateTime` sets now
  all pass, parts-of-dateTime 4 to 8/20 (needs F&O named record types,
  Proposed), civil-timezone 1 to 4/9. `debug`, `debug-gcc`, `wasm`
  pass, lint clean.

- 2.9 done (D119): number formatting. One picture engine
  (`src/format.c`, `src/format_internal.h`) for fn:format-integer (digit
  patterns in any Nd family, radix 2 to 36, letters, Roman, English
  words and ordinals, fallback to "1") and fn:format-number (sub-pictures,
  exponents, percent and per-mille, marker:rendition properties, exact
  half-even rounding on digit text); decimal formats checked by
  `sxml_decimal_format_apply` for 2.14, 3.x and 4.10. Part `format` with
  options maps. fn-format-integer 13 to 87 passed (all run),
  fn-format-number 20 to 252 of 286 run; 33 failures wait for static
  decimal formats in the evaluator (Proposed). XP40 27,308 passed in the
  snapshot, no set lost; slice 198/198; `debug`, `debug-gcc`, `wasm`
  126/126 (the `evaluator` failure under `debug` is HEAD's), lint clean.

- 3.3 done (D120b): XML and XHTML output methods. Serializer split into
  `src/serialize.c` (pipeline: character maps, normalization through
  `sxml_unicode_normalize`, escaping, line endings, UTF-8/UTF-16/US-ASCII/
  ISO-8859-1 with references; sequence normalizer; adaptive; dispatch,
  other methods UNSUPPORTED_FEATURE) and `src/serialize_xml.c`
  (declaration, doctype, namespaces against written bindings with
  undeclarations, indentation with suppress-indentation, indent-unit,
  indent-attributes, CDATA sections, XHTML void elements, HTML5 doctype,
  prefix normalization, content-type meta, URI escaping, Canonical XML;
  adaptive nodes incl. stored namespace nodes, D98). 13 new unit cases
  modelled on method-xml/method-xhtml, allocation-failure loop, 20,000
  nested elements. `debug`, `debug-gcc`, `wasm` all pass, lint and
  call-cycle check clean; slice 198/198; XP40 26,951 passed before and
  after (release runs of the snapshot at HEAD, no test changed). Open:
  the parameter reader of D106 (3.4), SERE0005/0006.
- 3.2 done (D121): XQuery front end. `sxml_xquery_build_ast` in
  `src/xquery_parse.c`, the XQuery parser part on the XPath walker:
  tables, keyword tokens, XQuery decoding (references with XQST0090,
  doubled delimiters and braces, attribute value normalization,
  end-of-line handling) and the D29 builder checks (XQST0118, XQST0040,
  XQST0071, XQST0022, XQST0070, XQST0085, boundary whitespace, XQST0031,
  XQST0087, %constructor record fields). XQ40 front end: before (scratch
  part, 3.1) 43,663 valid build with no checks; after 43,663 valid and
  145 modules build, 70 with their expected static error, 772/772
  XPST0003 rejected, 0 false accepts. XP40 26,951 passed at HEAD (no
  XPath path changed; CONFORMANCE.md 23,146), slice 198/198. `debug`,
  `debug-gcc`, `wasm` 126/126, lint clean.

- 2.6 done (D115): evaluator, paths, references. fn:jkey/fn:jvalue in
  new part `src/function_library_jnode.c` (moved from 2.17); selector
  interpretation of non-navigational steps from JNodes (XPDY0002 when
  focus-dependent), QName keys for prefixed names and wildcards (and
  EQName jnode() key tests, D113's proposal), attribute/namespace axes
  from JNodes XPTY0004, lookups through jvalue; numeric predicates of
  several numbers; maps/arrays as typed functions; fold fixpoint early
  exit; drop-while over a range stays a range. Release XP40, same
  driver: 23,698 to 23,910 passed before drop-while (1 former wrong error
  lost: PathExpr-J-461, fn:name on JNodes); prod-PathExpr.J 71 to 160 of
  175, prod-AxisStep.J 30 to 64 of 127, fn-fold-left 19 to 21/21,
  fold-right 22 to 24/24, fn-drop-while 17 to 18/18, prod-Predicate 188
  to 199/199, fn-jkey 4 to 14, fn-jvalue 4 to 15, fn-jtree 11 to 29,
  fn-function-lookup 499 to 500 of 587 (rest: missing functions; focus
  functions need lowering). Slice 198/198; `debug`, `debug-gcc`, `wasm`
  125/125; lint clean. Open: O(1) range comparison (RangeExpr-408h/410h)
  waits for `tests/xpath_test.c` (Proposed); reverse of a range (2.17);
  "/" on JNodes (lowering treat type); driver assert-eq on JNodes.
- 4.3 done (D120): value templates. `grammars/value_template`
  (inherits xpath; own scanner adds the fixed part and a zero-width
  start token), `sxml_xslt_build_value_template_ast` to an XPATH root
  with a STRING_TEMPLATE body through the parser part seam, XTSE0350 and
  XTSE0370 from a lexical scan after a syntax error. Every grammar now
  joins the sxml link line (`grammars/CMakeLists.txt`).
  `qt4-parse-check.py --spec XT40 --value-templates`: 12,244 in scope,
  12,232 accepted, 0 false rejects (one uses dropped `record(d?)`).
  `debug`, `debug-gcc`, `wasm` 126/126, lint clean; slice 198/198; XP40
  26,678 passed with and without the packet (sxml-qt4 links none of it).
- 4.1 done (D117): the XSLT 4.0 test driver `sxml-xslt-test`
  (`tests/xslt/driver/`), a sibling of sxml-qt4 that links its library.
  `list` resolves every case of `third_party/xslt40-test` (13,867, no
  problem; 8,387 applicable, 8,728 in the D95 scope) and shows its plan:
  principal stylesheet, package or embedded stylesheet, secondary
  modules, static and stylesheet parameters, global context item,
  invocation, delivery, assertion kinds. `run` reports through the QT4
  report: skip, "unsupported: assert-warning", errors for malformed
  cases, else "XSLT not implemented" until 4.15. Slice
  `tests/xslt/slice.txt` (154 cases over the twelve core sets), baseline
  0. Proposed: report name and XSLT areas in the QT4 report. `debug`,
  `debug-gcc`, `wasm` (xslt and qt4 tests), lint clean on the new files;
  debug ctest 141/141; no library or QT4-driver change, so XP40 and the
  qt4 slice (198) are untouched.

- 3.5 done (D110): node constructors in the evaluator. Seven
  constructor kinds and SHALLOW_COPY, one self-destroying arena tree per
  node, nested nodes copied; content convention for lowering (a SEQUENCE
  content lists enclosed expressions); names, PI, comment and namespace
  errors; strip, preserve/inherit fixed until the IR carries modes
  (proposed with a namespace-binding adder and verifier support).
  Deviation: `text {""}` is the empty sequence. 62 hand-built IR cases
  with allocation-failure loops, 20,000-deep arrays flattened. `debug`,
  `debug-gcc`, `wasm` 121/121, lint clean; slice 198/198; XP40 23,698
  passed before and after (release run of the snapshot at HEAD).
- 2.5 done (D113): lowering completions. for member and for key/value
  (array:split / map:entries collections, positions across all arrays
  or maps, XQST0089), destructuring let in its three forms, dynamic node
  tests (absent-focus selector, atomic-equal against node-name,
  local-name or jkey), the item axis (child step or a wrapper array;
  parent and order deviate, Proposed), constructor functions with a
  `$value := .` parameter (keyword arguments, `xs:T()`, `xs:T#0`),
  method calls, but with (map:merge, no coercion to the record type),
  jnode() selectors for decimals, doubles, negatives and QNames. Only
  computed node constructors stay unlowered. XP40 23,698 to 24,206
  passed, none lost; slice 198/198; prod-ForClause.map 7 to 56,
  .member 0 to 33, prod-LetClause 67 to 137, prod-MethodCall 0 to 12,
  misc-ConstructorKeywords and misc-FocusConstructors 5 to 153 each,
  prod-ButWithExpr 0 to 10 (needs parts-of-dateTime), prod-AxisStep.J
  30 (98 once fn:jkey/fn:jvalue land, 2.17). prod-NamedFunctionRef's 72
  failures are missing library functions. `debug`, `debug-gcc`, `wasm`
  119/119, lint clean.

- 3.1 done (D111): query AST and parser part seam. 66 XQuery kinds and
  24 fields in `src/xpath_ast_internal.h` (keyword detail, XQuery flags,
  boundary whitespace and namespace declaration attribute marks, PI
  payload), AST and IR of five XQuery examples written out;
  `sxml_ast_build_with_parser_part` runs the XPath walker with a part's
  grammar, tables and hooks (apply_token, describe_payload, finish_node,
  `sxml_ast_front_end_raise` for any err: code); `sxml_xquery_build_ast`
  declared for 3.2, which adds `sxml_grammar_xquery` to the link line.
  All 32,928 XP40 queries build byte-identical ASTs; a scratch part with
  only the tables builds all 43,663 valid XQ40 queries and 145 library
  modules. XP40 23,698 passed (CONFORMANCE.md 23,146; no AST changed),
  slice 198/198. `debug`, `debug-gcc`, `wasm` 119/119, lint clean.

- 2.4 done (D107): Unicode 16.0 tables and functions. `src/unicode.c`
  (UAX #15 NFC/NFD/NFKC/NFKD and FULLY-NORMALIZED; UAX #29 extended
  grapheme clusters with GB9c; HTML character reference names) over
  `src/unicode_tables.c` from `tools/generate-unicode-tables.py`
  (separate from the regex tables: no property in both);
  `src/function_library_unicode.c`: fn:char, fn:characters,
  fn:graphemes, fn:normalize-unicode. All 79,860 NormalizationTest.txt
  checks (plus part 1 identities) and all 1,093 GraphemeBreakTest.txt
  lines pass in a scratch probe; 13 unit cases with an
  allocation-failure loop. QT4 (release): fn-graphemes 0 to 918 of 918,
  fn-normalize-unicode 11 to 39 of 41 (the two left are "FULLY-NORMALIZED
  unsupported" variants the driver still runs; 44 of 44 once it declares
  the form), fn-char 24 to 47 of 48 (char-001 needs fn:json-doc),
  fn-characters 1 to 8 of 8; XP40 24,704 passed on 79ae2d0 plus 2.4, no
  test set lost; slice 198/198. `wasm` grows by about 143 KB (136 KB
  data, mostly decompositions and the 2,125 names). `debug`, `debug-gcc`,
  `wasm`, `release` 121/121, lint clean.
- 2.3 done (D105, D114): the regex engine covers the F&O 4.0 dialect
  (\p/\P categories and blocks, \i \c, \b \B, lookahead/lookbehind in
  both spellings, named groups and \k<name>, XSD 1.1 hyphens, case-blind
  matching through the fn:lower-case/upper-case tables plus 94 extra
  pairs), no recursion; tables regenerated from Unicode 16.0 (about 25
  KB). New part `src/function_library_regex.c` with fn:matches,
  fn:replace ($<name>), fn:tokenize (moved from the string part),
  fn:analyze-string, fn:matching-segments, fn:regex. Before/after
  (release): fn-matches.re 580/1082 -> 1082, fn-matches 155/200 -> 198,
  fn-replace 166/187 -> 185, fn-tokenize 67/71 -> 68, fn-analyze-string
  0/30 -> 29, fn-matching-segments 0/38 -> 36, fn-regex 0/30 -> 25;
  XP40 23,146 -> 23,793 (dashboard run 09e1fed vs private run on
  HEAD plus these files); FORX0002 561 -> 0; slice 198/198. `debug`,
  `debug-gcc`, `wasm`, `release` all pass, lint clean. Open:
  `a{2147483647}` is LIMIT_EXCEEDED (counted repetition is expanded);
  named record types for fn:regex/fn:matching-segments.

- 2.5 done (D113): lowering completions. for member and for key/value
  (array:split / map:entries collections, positions across all arrays
  or maps, XQST0089), destructuring let in its three forms, dynamic node
  tests (absent-focus selector, atomic-equal against node-name,
  local-name or jkey), the item axis (child step or a wrapper array;
  parent and order deviate, Proposed), constructor functions with a
  `$value := .` parameter (keyword arguments, `xs:T()`, `xs:T#0`),
  method calls, but with (map:merge, no coercion to the record type),
  jnode() selectors for decimals, doubles, negatives and QNames. Only
  computed node constructors stay unlowered. XP40 23,698 to 24,206
  passed, none lost; slice 198/198; prod-ForClause.map 7 to 56,
  .member 0 to 33, prod-LetClause 67 to 137, prod-MethodCall 0 to 12,
  misc-ConstructorKeywords and misc-FocusConstructors 5 to 153 each,
  prod-ButWithExpr 0 to 10 (needs parts-of-dateTime), prod-AxisStep.J
  30 (98 once fn:jkey/fn:jvalue land, 2.17). prod-NamedFunctionRef's 72
  failures are missing library functions. `debug`, `debug-gcc`, `wasm`
  119/119, lint clean.

- 4.0 done (D112): the XSLT API, header-only. `xslt.h`:
  `sxml_xslt_stylesheet` (quintet) from the principal module's bytes or
  through the resolver (XSLT_MODULE, XSLT_PACKAGE), static parameters at
  compile time; `sxml_xslt_output_definition` (D106 parameters plus
  build-tree and "no method given"); `sxml_xslt_stylesheet_transform`
  with the shared dynamic context (global context item, stylesheet
  parameters, xsl:message through the trace sink), apply-templates,
  call-template and function call invocations, raw/tree/serialized
  delivery, a result document handler (open/close) and its default, the
  `sxml_xslt_result_document_collector`. `debug`, `debug-gcc`, `wasm`
  119/119 (slice included), lint clean; no source changed, so XP40
  counts cannot move.

- 3.6 done (D109), Early packet of Phase 3: TRY_CATCH in
  `src/evaluator_try.c` as one catching task: name tests and wildcards,
  first matching clause, `$err:map` bound to the clause's slot (keys
  code, description, value, module, line-number, column-number; no
  entry when empty), finally (XQTY0153; its error replaces the first;
  unmatched and catch-clause errors raised again after it), lazy values
  read inside the try, an own error record when the caller passes none.
  17 unit cases over hand-built IR (resumable native calls, limits,
  allocation-failure loops). Proposed: IR adders for catch clauses and
  name tests, verifier support. `debug`, `debug-gcc`, `wasm` 120/120,
  lint clean; slice 198/198; XP40 23,698 passed in the snapshot (XPath
  cannot produce TRY_CATCH, so unchanged by this packet).

- 3.0 done (D106): the XQuery and serialization API, header-only.
  `xquery.h`: `sxml_xquery_query` (quintet) compiled with the shared
  static context, library modules through the XQUERY_MODULE resolver
  kind, evaluation with the shared dynamic context, the prolog's
  serialization parameters returned by value. `serialize.h`: methods
  xml, xhtml, html, text, json, csv; one struct with every
  Serialization 4.0 parameter (tri-state `sxml_serialization_flag`,
  encodings UTF-8/UTF-16/US-ASCII/ISO-8859-1, character maps, element
  name lists); `sxml_serialization_parameter_reader` for
  output:serialization-parameters elements and option maps. `debug`,
  `debug-gcc`, `wasm` 119/119 (slice included), lint clean; no source
  changed, so XP40 counts cannot move.

- 2.16 done (D108): QT4 driver for Phase 2. Environments decimal-format
  (D104 properties, prefixed names), collation (D97 URIs run, default
  sets the default collation), resource and collection (new resource
  table: TEXT/BINARY by URI with the resource's encoding, COLLECTION and
  URI_COLLECTION as sequences, default collection) are applied; only
  collections with a query wait (3.13). Applicability reports the D95
  scope: `list --spec=XP40` 31,543 in scope, `--spec=XQ40` 41,174. JSON
  report gains `areas` and `in_scope`/`in_scope_passed` counts (format
  version 1, additive). New dependencies satisfied: simple-uca-fallback,
  non_empty_sequence_collection, arbitraryPrecisionDecimal, NFC/NFD/NFKC/
  NFKD. Goldens `run_environments`, `run_environments_json`,
  `list_environments`; unit tests for scope and the resource table.
  XP40 23,146 to 24,451 passed (none lost), not run 18,448 to 16,321;
  fn-format-number 20 to 27 passed (not run 72 to 22),
  fn-unparsed-text 40 to 54, -lines 23 to 39, -available 6 to 7,
  fn-collection 10 to 18, fn-json-doc 7 to 36, misc-BuiltInKeywords 0 to
  206; slice 198/198. `debug`, `debug-gcc`, `wasm` 122/122, lint clean.
- 2.1 done (D103): lowering seam. `src/xpath_lower_internal.h` exports
  the lowerer (lifecycle quintet), its building blocks and the frame
  machine; frames point at procedure descriptors, so core and part
  procedures run alike. Lowering parts (`src/<name>_lower[_<part>].c`
  defining `sxml_xpath_lowering_part_<part>`) are registered by
  `cmake/SxmlParts.cmake` (none yet); a part's rule comes before the core
  for its language. Embedding entry (from a step or from outside) lowers
  an XPath AST in the caller's scope, focus and the AST's static context;
  `sxml_xpath_lower` uses it. Probe parts and kinds in `tests/ir_test.c`
  (7 new cases with an allocation-failure loop). Slice dumps unchanged;
  `debug`, `debug-gcc`, `wasm` 119/119, lint clean; slice 198/198; XP40
  23,146 passed before and after (release run of the snapshot).
- 2.0 done (D101): part registries by glob. CMake generates the library
  part list (per language: XPath, XQuery, XSLT) and the evaluator part
  list; processor and fuzz fixture take the XPath list. Machine API in
  `src/evaluator_machine_internal.h` (part tasks, catching tasks, global
  initializer hook); stubs construct, try, template, global return
  UNSUPPORTED_FEATURE. Probes verified a new part, an XSLT-only part and a
  catching part need no other edit. `debug`, `debug-gcc`, `wasm` 119/119,
  lint clean (89 files, no cycle); slice 198/198; XP40 unchanged: 23,146
  passed, 5,209 failed, 510 errors (before and after).

- 2.2 done (D104): the public API for Phase 2, header-only. Static
  context: decimal formats (name, properties as text with 4.0's
  marker:rendition form), default collation URI, collation bindings,
  `sxml_collation_is_supported`. Dynamic context: default language,
  environment variables, trace sink. Resources: kinds TEXT, BINARY,
  COLLECTION, URI_COLLECTION, XQUERY_MODULE, XSLT_MODULE, XSLT_PACKAGE,
  form SEQUENCE, request fields for encoding, default collection, module
  namespace and package version. `sxml_trace_sink` for fn:trace,
  fn:message, xsl:message, with `sxml_stream_trace_sink`. version.h:
  `SXML_VERSION_ENCODE`, `SXML_VERSION_NUMBER`, `SXML_API_VERSION`
  (0.2.0). 2.12 defines the two new functions. `debug`, `debug-gcc`,
  `wasm` 117/117, lint clean; XP40 23,146 passed before and after, slice
  198/198.

- 4.2 done (D102), Early packet of Phase 4: the stylesheet AST,
  `src/xslt_ast_internal.h` (header only). One AST per stylesheet:
  modules (pre-order, import precedence computed at seal), one node kind
  per element of the XSLT 4.0 schema plus literal result elements, text
  nodes, extension, unknown and data elements; attribute kinds per
  schema attribute with values read into typed forms (expressions,
  patterns, value templates and types as their own `sxml_ast`); a
  resolved static context per element; declaration list; text form.
  for-each-group-001 and a template rule, function and import written
  out. Builder functions go to 4.5 (`src/xslt_ast.c`). Header compiles
  alone under clang, gcc and wasi-sdk with the project flags and is
  clang-tidy clean; `debug`, `debug-gcc`, `wasm` 117/117, lint clean.
  No translation unit includes it yet, so XP40 and the slice are
  unchanged (198).

- 1.29 done, Phase 1 gate review (D98-D100). Rubric pass over all public
  headers and the core modules; no writable globals, no allocation or
  printing outside their places, no recursion through function pointers
  (call-cycle check now also links the QT4 driver: 85 files, no cycle).
  Substrate fixed before the freeze: base URI of parentless roots
  (`sxml_arena_tree_configuration.base_uri`), parentless namespace nodes
  stored as a tree's root; then frozen (D99). Local gate on `09e1fed`:
  `debug`, `debug-gcc`, `release`, `wasm` 117/117 ctest, `fuzz` 5/5,
  `tools/lint.sh` clean, slice 198/198 (sanitizers and `wasm`); five fuzz
  targets 300 s each, clean; dashboard regenerated: XP40 23,146 passed
  (800 wrong error), 5,209 failed, 510 errors, 18,448 not run (up from
  21,443 by 1.23b). `wasm` CLI 1,268,420 stripped / 385,760 gzip, inside
  the D93 budget; 1.23b added 55 KB stripped without logging it.
  **Blocked**: CI refuses every job for billing since 2026-10-16; the CI
  gate boxes stay open and macOS, Windows, MSVC are unverified.

- 1.28 done (D93): WebAssembly slice. `tools/run-wasi.mjs` preopens the
  source tree as well as ".", so `xml_conformance` runs under WASI
  (601/175/948) instead of skipping; `qt4/slice` 198/198 under `wasm`
  (117/117 ctest). Sizes, stripped / gzip: `sxml` CLI 1,213,261 / 358,020,
  `sxml-qt4` 1,264,250 / 376,528; `libsxml.a` 947,910. Slice: native
  release ~0.95 s, `wasm` ~1.9 s. Budget: CLI < 1.5 MB stripped, < 450 KB
  gzip; not yet enforced in CI.
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

## Open questions at the Phase 1 gate (packet 1.29)

For the owner. Only the first blocks a gate box.

1. **CI billing.** GitHub Actions refuses every job ("recent account
   payments have failed or your spending limit needs to be increased").
   Restore billing (or move CI elsewhere) so the two CI gate boxes can be
   ticked and macOS, Windows and MSVC are built again; until then those
   platforms are unverified since the Phase 0 gate.
2. **Phase 2 may start with the CI boxes open?** The review ran every check
   locally (D100); CLAUDE.md is the orchestrator's to move to Phase 2.
3. **Substrate freeze (D99).** Accept the rule: additive changes with a
   record, no changes to existing declarations without reopening D99.
4. **The 510 "error" outcomes of XP40** are mostly unlowered constructs
   (UNSUPPORTED_FEATURE, 493); 8 hit LIMIT_EXCEEDED and 7 the 512 MiB
   budget of the driver. D95 asks for zero at the Phase 2 gate; the 15
   resource cases need a look in Phase 2 (2.19 sweeps).
5. **Size budget in CI.** D93's budget is not enforced by a test; 1.23b
   grew the `wasm` CLI by 55 KB unnoticed. Add the size check to CMake or
   `ci.yml` in Phase 2 (2.0 owns the root CMakeLists.txt).

## Open questions at the Phase 2 gate (packet 2.20)

For the owner. Only the first is a gate box.

1. **Tag 0.2.0.** Every other box is ticked (D151). `CHANGELOG.md` says
   "unreleased"; `docs/RELEASE.md` lists the steps (tag, npm name
   `sxml-js` while `sxml` is taken on npm).
2. **WebAssembly budgets** (Proposed): `sxml.wasm` is 1,865,152 bytes
   stripped and 647,721 gzipped against 2,000,000 / 700,000 (7 % left);
   the CLI with XQuery 3,480,962 / 780,893 against 3,700,000 / 820,000.
   Phases 3 and 4 are likely to pass them: confirm higher budgets, or ask for
   optional modules (XQuery and XSLT grammars, Unicode and collation
   tables).
3. **XQuery's omit-xml-declaration default** (Proposed, D146): no
   (write the declaration, as QT4 expects) or keep yes.
4. **Provisional headers** (D151): `xquery.h` and `xslt.h` may change in
   a minor release until the Phase 3 and 4 gates. Accept, or declare them
   stable now.
5. **CI** (D123): still local only; macOS, Windows and MSVC have not been
   built since the Phase 0 gate.

## Open questions at the Phase 3 gate (packet 3.15)

For the owner. Every gate box is ticked (D161).

1. **0.2.0 and XQuery.** 0.2.0 is still untagged; `CHANGELOG.md` lists
   XQuery under "Unreleased". Tag 0.2.0 at the Phase 2 commit, or fold
   XQuery into 0.2.0.
2. **WebAssembly budgets** (Proposed): `sxml.wasm` is 1,900,448 bytes
   stripped and 659,284 gzipped against 2,000,000 / 700,000 (5 % left);
   the CLI with XQuery and XSLT 4,624,576 / 1,032,045 against 5,000,000 /
   1,100,000. `compileQuery()` is still not in the JavaScript package.
   Confirm the budgets, or ask for optional modules.
3. **XQuery's omit-xml-declaration default** (Proposed, D146): no (write
   the declaration, as QT4 expects) or keep yes. The only behaviour of
   the now stable `xquery.h` still open.
4. **CI** (D123): still local only; macOS, Windows and MSVC unbuilt since
   the Phase 0 gate.

## Open questions at the Phase 4 gate (packet 4.18)

For the owner. Every gate box is ticked (D169).

1. **0.2.0, XQuery and XSLT.** 0.2.0 is still untagged; `CHANGELOG.md`
   lists XQuery and XSLT under "Unreleased". Tag 0.2.0 at the Phase 2
   commit, or fold both into one release.
2. **WebAssembly budgets** (Proposed, three items): the CLI with XQuery
   and XSLT against its 5,000,000 / 1,100,000 budget, `sxml.wasm` against
   2,000,000 / 700,000, and `compileQuery()` and XSLT still not in the
   JavaScript package. Confirm the budgets, or ask for optional modules.
3. **XQuery's omit-xml-declaration default** (Proposed, D146): no (write
   the declaration, as QT4 expects) or keep yes.
4. **Phase 7 plan** (D162): `docs/tasks/phase-7.md` is due after this
   gate, one packet per delayed feature.
5. **CI** (D123): still local only; macOS, Windows and MSVC have not been
   built since the Phase 0 gate (the CLI's new working-directory read is
   `_getcwd` there, untested).

## Open questions at the Phase 5 gate (packet 5.14)

For the owner. Four gate boxes wait for the full runs and fuzzing on the
owner's machine (D181); the review's runs met every number, and schema
validation works.

0. **Confirming runs** (D182): on the owner's machine, `tools/check-main.sh`
   (with the full lint, cut off here), the four suites (`tools/run-qt4.sh`
   XP40 and XQ40, `run-xslt.sh`, `run-xsd.sh`, then
   `tools/conformance-report.py`) and `tools/fuzz.sh --seconds 300`,
   which now includes schema_fuzz; then tick the four boxes.

1. **0.2.0 and Phases 3 to 5.** 0.2.0 is still untagged; `CHANGELOG.md`
   lists XQuery, XSLT and XSD under "Unreleased". Tag 0.2.0 at the
   Phase 2 commit, or fold them into one release.
2. **WebAssembly budgets** (Proposed, four items): `sxml.wasm` is
   1,997,773 bytes stripped and 695,343 gzipped against 2,000,000 /
   700,000 (0.1 % and 0.7 % left: the next feature in the JavaScript
   package breaks it); the CLI with XSD is 4,950,289 / 1,154,545 against
   D176's 5,500,000 / 1,250,000. `compileQuery()`, XSLT and schemas are
   still not in the JavaScript package. Confirm the budgets, or ask for
   optional modules.
3. **XQuery's omit-xml-declaration default** (Proposed, D146): no (write
   the declaration, as QT4 expects) or keep yes.
4. **Accepted designs for later sweeps** (D169, D182; out of Proposed):
   match="$var" membership by a node index; IR modules that carry their
   stylesheet module's URI, for errors in included modules; exporting
   the package-version match from xslt.h; the XSLT functions in the
   run-time function table; globals computed on first read (XTDE3052).
5. **Not done in Phase 5** (D178 to D182): is-id of union and list
   elements; XSLT's ID/IDREF for elements, schema roles, typed modes'
   schema-element() names, xs:untypedAtomic as a type, xml:lang
   declarations; the remaining XSD failures (XSD 1.0 rules 1.1 changed,
   NOTATION union members, XML 1.1 instances, EDC with wildcards).
6. **Performance** (D182): about 4 million evaluation steps a second
   (release) and no index for `$seq[f(.) = $key]`; QT4's itunes takes
   72 s. A general evaluator item, not a schema one.
7. **Phase 6 and 7 plans** (D162): no `docs/tasks/phase-6.md` or
   `phase-7.md` yet; the next phase is the owner's choice.
8. **CI** (D123): still local only; macOS, Windows and MSVC unbuilt.

