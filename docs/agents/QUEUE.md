# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–4 closed by agent review (D11, D151, D161, D169).
- Phase 5 (XSD 1.1, schema awareness) closed by agent review (D182); four
  gate boxes wait for full runs and fuzzing on the owner's machine (D181).
  All public headers are stable.
- Passes: XP40 30,931, XQ40 41,273, XT40 8,650, XSD in scope 41,483 of
  41,518. Zero error outcomes in every suite.
- `sxml.wasm` is 1,997,773 of its 2,000,000-byte budget: the next feature
  that links into it breaks the size test (owner decision open).

## Running

- CLI docs split: the user reference moves to `apps/sxml/README.md`.
- XPath/XQuery conformance sweep (2.21).
- XSLT conformance sweep (4.19).
- Phase 7 plan (7.1).

## Next

- Phase 7 packets once 7.1 has written `docs/tasks/phase-7.md`.
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

XSLT:
- `priority` (34): "~T" type patterns from a newer 4.0 draft.
- `call-template-ext` (25): extension instructions as template calls.
- Named record and type declarations (`record`, `type-alias`, 41).
- `merge` (20), `snapshot` (19).
- `match="$var"` membership is a linear scan (quadratic); needs an index.
- Errors inside an included module name the main stylesheet's file.
- XTDE3052 needs globals computed on first read.
- Larger duplicates: typed-array allocation in 19 files; key equality of
  group-adjacent and group-starting-with.
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

Listed at the end of `docs/LOG.md`: the 0.2.0 tag, the wasm budgets (or
optional features), XQuery's omit-xml-declaration default, the license
(no LICENSE file; the JavaScript package says MIT), CI, and whether
`xsl:evaluate`, DTD processing, XML 1.1 and disable-output-escaping join
Phase 7 (7.1 proposes it).
