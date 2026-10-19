# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–4 closed by agent review (D11, D151, D161, D169).
- Phase 5 (XSD 1.1, schema awareness) closed by agent review (D182); four
  gate boxes wait for full runs and fuzzing on the owner's machine (D181).
  All public headers are stable.
- Passes: XP40 31,098, XQ40 42,490, XT40 8,973, XSD in scope 41,483 of
  41,518. Zero error outcomes in every suite.
- Optional features (D189). Budgets (D202, D207): core `sxml.wasm`
  2,200,000 / 770,000 (2,120,223 now), `sxml-full.wasm` 4,000,000 /
  1,400,000 (3,553,063 with fn:transform), CLI module 7,000,000 / 1,500,000.
- EXPath Binary (D198) 646 of 646 and File (D200) 270 of 270 in XQ40; File
  needs `--allow-file-access` in the CLI and has no Windows implementation.

## Running

- 7.4b XSLT 1.0 follow-ups: exsl:node-set, docbook-002, TVTs in 1.0 elements, result-document xhtml.
- 7.6a XSLT streamability analysis.
- 7.11 fn:transform (after 7.3).
- 7.7b/7.7c XQuery Update 4.0 additions, fn:put, revalidation (after 7.7a, D187: upd-* 733 of 825).
- 7.14 DTD processing (after 7.13, D195).

## Next

- Phase 7 (`docs/tasks/phase-7.md`, D183, D184): 7.6b streamed
  evaluation after 7.6a; 7.10b HTML follow-ups; Full Text, UCA, timezones, calendars and languages, iXML.
- JavaScript bindings for XQuery, XSLT and validation (they cover XPath only).
- `sxml` CLI gaps: `--output`, writing result documents, `--method=csv`,
  "did you mean" for options of a command.

## Leftovers no packet owns yet

XPath and XQuery (after 2.21, D190; XP40 31,098, XQ40 41,456):
- Built-in schema for the `fn` namespace, compiled once per processor
  (json-to-xml validation, typed analyze-string results; about 25 cases).
- The current draft's record model: maps are records only when annotated
  (cast-record-901, recordTypeDecl-019/072/089).
- NCName vs `Q{}N` name tests (sxml_name_test is frozen); the item axis's
  JNode parent (D113); a declared function's `$a := .` via function-lookup.
- Lax validation with no in-scope schema raises FODC0014 (the planner in
  `src/function_library_schema.c` treats it as strict).
- fn:serialize canonical-JSON cases (27) need `bin:unpack-double` (7.8).
- A processor keeps every imported schema until it is destroyed (D174).

XSLT (after 4.19, D185; XT40 8,973):
- `priority` stays at 6/40: the suite expects "~T" patterns the project's
  draft (13 Oct 2026) replaced, and other expectations contradict 6.4 (D29).
- `match="$var"` membership index: designed in D185, needs
  `src/function_library_xslt.c`.
- XTDE3490 (needs `src/xpath_lower.c`); XTDE2220 and XTDE2210 with computed
  attributes in merge; merge 049-052 validation codes; collection-006;
  snapshot-0101c; the record constructor function (record-026/027).
- Errors in inline functions and xsl:iterate bodies of an included module
  still name the main stylesheet's file.
- XTDE3052 needs globals computed on first read.
- Typed-array allocation: XSLT files use `sxml_allocator_allocate_array`
  (`src/memory_internal.h`); move the other files when next touched. Key
  equality of group-adjacent/group-starting-with should reuse the FLWOR's
  `keys_are_equal` (`src/xquery_flwor.c`).
- Schema-aware leftovers (D179): XTSE3105, typed modes reading name tests
  as `schema-element()`, ID/IDREF skipping for elements, `xs:untypedAtomic`
  as a type attribute, `xsl:copy` with preserve, schema roles, XTSE0265.

XSLT backwards compatibility (7.4, D197): backwards_compatibility 111 of
124; QT4 xpath-1.0-compatibility 1 of 5. 14 cases expect fn:number-style
conversion for xs:numeric parameters, which XPath 4.0 §3.4.1 restricts to
xs:double (owner to confirm).

xsl:evaluate (7.3, D196: 73 of 83, gate 90 %): 3 need parse-html (7.10),
2 need json-doc/unparsed-text-available trust checks, 1 a UCA strength
(7.15); keyword arguments to stylesheet functions from the target
expression. disable-output-escaping (7.5, D188): 25 of 25 run.

HTML (7.10a done, D201: misc-HtmlTestSuite 1,378 of 1,379, fn-parse-html
22/22, fn-html-doc 9/10). 7.10b: the `<meta>` encoding prescan and
mid-parse encoding change, vendored html5lib tests as a unit test,
multi-byte decoders (or FODC0011), source locations, XHTML input, and the
CLI's missing static base URI (relative `html-doc`/`unparsed-text` in
`sxml xpath`/`xquery`).

XML 1.1 (after 7.13, D195; xmlconf 1.1 242 of 271):
- 29 xmlconf cases need external entities or DTDs (7.14).
- XT40 xml-version-007/-016 and namespace-2606/-6202 fail in `src/xslt*`.
- Owner: 13 XQ40 cases need query text lexed by XML 1.1 rules; keep 1.0,
  switch to 1.1, or a processor option (Proposed in DECISIONS.md).

XSD:
- is-id for union and list elements (needs the element's flags after its
  content; tree.h is frozen, D180).
- 35 failures: XSD 1.0-only rules, `xs:NOTATION` as a union member,
  XML 1.1 instances, wildcard consistency with type tables.

Larger duplicates (D182): the schema reader's own boolean and integer
parsers, Gregorian day arithmetic in four places, three ways of building
compile-report text, four text writers, three private heapsorts in
serialization.

Performance: the evaluator runs about 4 million steps a second and has no
index for joins (app-Demos itunes is quadratic in its own text, 72 s).

Platform and tooling:
- macOS, Windows and MSVC have never been built; `_getcwd` is untested.
- CI is disabled (D123).
- The QT4 driver's `list`/`run` still have their own selection walk;
  `selection.h` is shared by the XSLT and XSD drivers.

## Owner decisions open

Listed at the end of `docs/LOG.md`: the 0.2.0 tag, XQuery's
omit-xml-declaration default, the license (no LICENSE file; the JavaScript
package says MIT), CI, and from D183 the Phase 7 gate numbers, the iXML and
CollationTest test data, and the languages, calendars and tailorings.
Decided in D184: the four extra features join Phase 7, link-time optional
features with host-supplied data (2.1 MB core, 3.0 MB full), Update 3.0
then 4.0, EXPath File behind `--allow-file-access`.
