# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–4 closed by agent review (D11, D151, D161, D169).
- Phase 5 (XSD 1.1, schema awareness) closed by agent review (D182); four
  gate boxes wait for full runs and fuzzing on the owner's machine (D181).
  All public headers are stable.
- Passes: XP40 31,098, XQ40 42,490, XT40 8,931+, XSD in scope 41,483 of
  41,518. Zero error outcomes in every suite.
- Optional features (D189). Budgets (D202, D207): core `sxml.wasm`
  2,200,000 / 770,000 (2,131,859 after 7.2b moved DTD validation behind a
  feature, D211), `sxml-full.wasm` 4,000,000 / 1,400,000 (3,577,133), CLI
  module 7,500,000 / 1,650,000 (7,303,201 with the embedded DUCET). XQuery lowering parts (about
  75 KB) stay in the core until the part registry is split by language.
- EXPath Binary (D198) 646 of 646 and File (D200) 270 of 270 in XQ40; File
  needs `--allow-file-access` in the CLI and has no Windows implementation.

## Running

- 7.12a XQuery/XPath Full Text, part 1.
- JavaScript bindings for XQuery, XSLT and validation (js-1).
- 7.6b XSLT streamed evaluation.
- 7.15b CLDR tailorings.
- 7.18 Invisible XML.

## Next

- Phase 7 (`docs/tasks/phase-7.md`, D183, D184): 7.10b HTML follow-ups; Full Text, UCA, timezones, calendars and languages, iXML.
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

XSLT (after 4.19, D185; XT40 8,931+):
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
xs:double (D208: follow the spec). 7.4b (D206): exsl:node-set,
exsl:object-type, exsl:document; docbook-001/002 pass; docbook-004 fails
with FODC0002 in xsl:source-document.

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

DTD (7.14a/b done, D199): xmlconf external-entity cases all pass, XML 1.1
not-wf 169 of 169; XT40 dtd 76 of 79 after 7.14c (D210); `sxml check
--external-entities` and `--validate-dtd`. Left: supports-dtd-validation
should follow D211's dtd-validation feature (needs the processor's feature
set in fn:system-properties); error-1370a/1380a expect XSLT 2.0 codes.

XQuery Update (7.7a-c done, D187, D205): upd-* 787 of 825 (98.6 % of 798
in scope). Left: XUDY0038 for dynamic calls of updating function items (the
evaluator's dynamic call); six propagateNamespace cases are a test-suite
whitespace error; two cases assume no cvc-id.1 IDREF check on parentless
elements (schema owner); XQuery constructors under construction strip
still keep is-id (evaluator_construct.c cannot tell XQuery from XSLT);
fn:put in `sxml xquery` under `--allow-file-access`.

fn:transform (7.11, D203): QT4 fn-transform XQ40 129 of 133 run, XP40 126
of 130; XT40 transform 9 of 12. Left: fn:transform in static expressions
(FOXT0004), enable-assertions false, the inner run shares the caller's
scope/loader/available documents/trace sink, XSLT 2.0 behaviour for
xslt-version 2.0, the QT4 driver's base URI for query files, `~record`
patterns.

Streaming analysis (7.6a, D212): strm/ 2,503 of 2,564 (evaluation still
unstreamed); XTSE3430 154 of 156. XT40 8,952 -> 11,792 with the streaming
feature claimed.

UCA (7.15a, D213): advanced-uca-fallback 49 of 49 (XQ40 and XP40),
non_unicode_codepoint_collation 13 of 13; DUCET 16.0.0 vendored, served by
the host; the default collation now applies to comparisons and order by.
CLI budget 7,500,000 / 1,650,000 (D208).

Timezones (7.16, D209): fn-civil-timezone 9 of 9, the 8 olson-timezone
cases pass; tzdata 2026e vendored, served by the host as TZif. Left: a
default place in the dynamic context (additive header change); the CLI
reads `/usr/share/zoneinfo`.

XML 1.1 (after 7.13, D195):
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

Only the outward-facing ones (D208): the license (no LICENSE file; the
JavaScript package says MIT), tagging and publishing releases, the npm
name. Everything else takes the proposed default and is recorded (D208):
budgets rise with features (keep the core lean), Phase 7 gates as in D183,
vendored iXML and CollationTest data, the listed languages and calendars,
XPath 4.0's number conversion in compatibility mode, an XML-version option
for query text defaulting to 1.0. CI stays off until the owner's machine.
