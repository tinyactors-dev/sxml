# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–4 closed by agent review (D11, D151, D161, D169).
- Phase 5 (XSD 1.1, schema awareness) closed by agent review (D182); four
  gate boxes wait for full runs and fuzzing on the owner's machine (D181).
  All public headers are stable.
- Passes: XP40 30,931, XQ40 41,273, XT40 8,759, XSD in scope 41,483 of
  41,518. Zero error outcomes in every suite.
- Optional features (D189): `sxml.wasm` (core) 2,004,485 of 2,100,000
  bytes, `sxml-full.wasm` 2,005,513 of 3,000,000. Gzipped budgets 735,000 /
  1,050,000 were set at D138's ratio (owner to confirm). Schema validation
  (71 KB) and language-specific lowering parts (75 KB) could leave the core.

## Running

- XPath/XQuery conformance sweep (2.21).
- 7.4 XSLT backwards compatibility and XPath 1.0 mode.
- 7.3 xsl:evaluate and 7.5 disable-output-escaping.
- 7.8 EXPath Binary and 7.9 EXPath File (one agent).
- 7.10a HTML5 tokenizer and tree builder.
- 7.7a XQuery Update Facility 3.0.
- 7.13 XML 1.1 input.

## Next

- Phase 7 (`docs/tasks/phase-7.md`, D183, D184): 7.6a streaming analysis
  after 7.4; after 7.13, 7.14
  DTD; after 7.3, 7.11 fn:transform;
  then Full Text, UCA, timezones, calendars and languages, iXML.
- JavaScript bindings for XQuery, XSLT and validation (they cover XPath only).
- `sxml` CLI gaps: `--output`, writing result documents, `--method=csv`,
  "did you mean" for options of a command.

## Leftovers no packet owns yet

XPath and XQuery:
- Casts to array, map and record types (about 82 cases).
- `fn:function-lookup` results do not get the caller's focus (about 47).
- `fn:serialize` option edge cases (about 25); path steps over JNodes (23).
- Built-in schema for the `fn` namespace (json-to-xml and analyze-string
  validation, about 25); `fn:doc` with xsd-validation; schema loading inside
  `fn:xsd-validator`.
- A processor keeps every imported schema until it is destroyed (D174).

XSLT (after 4.19, D185; XT40 8,759):
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
