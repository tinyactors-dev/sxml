# Work queue

Kept by the orchestrator. Newest state first; each change is its own
commit. Numbers are release-build passes from `docs/CONFORMANCE.md`.

## State (2026-10-19)

- Phases 0–5 and 7 closed by agent review (D11, D151, D161, D169, D182,
  D237); fuzzing at length and the full lint wait for the owner's machine
  (D181). 7.11's gate (all 12 XT40 transform cases) is open: 9 pass.
- Passes (release, whole suites at 9c5b1ba): XP40 32,689 (99.5 %), XQ40
  45,969 (99.5 %), XT40 12,075 (94.8 %), XSD in scope 41,486 of 41,518.
  Zero error outcomes in every suite.
- Optional features (D189). Budgets (D202, D207, D208): core `sxml.wasm`
  2,200,000 / 770,000 (2,077,439 / 725,473 after size-2, D227: each module
  links only its languages' parts), `sxml-full.wasm` 8,000,000 /
  2,800,000, CLI 9,500,000 / 2,100,000 (D235: XPath Full Text grammar). The size test compares the core's
  linker map with a baseline and prints the top growths (D225).
- EXPath Binary (D198) 646 of 646 and File (D200) 270 of 270 in XQ40; File
  needs `--allow-file-access` in the CLI and has no Windows implementation.

## Running

- 6.2: the host shim (bindings/host/), shared by JavaScript, Kotlin and Swift.
- 7.20: sweep (7.11's transform-004/401/009, the larger duplicates of D237).

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

## Leftovers no packet owns yet

XPath and XQuery (after 2.22, D226):
- XSLT's own expressions have no Full Text (they use grammars/xpath; D235).
- `{ E }` map constructors over JNodes (MapConstructor-500..511).
- NCName vs `Q{}N` name tests (sxml_name_test is frozen); the item axis's
  JNode parent (D113); a declared function's `$a := .` via function-lookup.
- fn:xsd-validator's schema, schema-location, target-namespace options.
- fn:serialize canonical-JSON cases (27) need `bin:unpack-double` (7.8).
- A processor keeps every imported schema until it is destroyed (D174).

XSLT (after 4.20, D230; XT40 11,912): merge 107/120, error 474/580,
validation 51/68, namespace 217/224, normalize-unicode 18/18. Left:
XTDE3490; merge-097 (Saxon collection URI); xml-version-007 (value-template
grammar rejects XML 1.1 controls); namespace-2606/-6202; docbook-004;
fn-transform-27/82e/94; xsl:copy of JNodes (needs an xdm.h addition);
XTDE0430/0440; priority stays 6/40 (D29). 4.21 (D234): indexed FILTERs
are not hoisted from; current-merge-group(()) per 15.5.1; globals computed
on first read (GLOBAL_READ); typed modes and XTSE3105; XT40 11,943 at its
last run. Left: document-4066 (fn:document's schema validation does not
raise FODC0002); a global's side effects now happen at its first read.

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

XML 1.1 (after 7.13, D195):
- XT40 xml-version-007/-016 and namespace-2606/-6202 fail in `src/xslt*`.
- Owner: 13 XQ40 cases need query text lexed by XML 1.1 rules; keep 1.0,
  switch to 1.1, or a processor option (Proposed in DECISIONS.md).

XSD:
- is-id for union and list elements (needs the element's flags after its
  content; tree.h is frozen, D180).
- 35 failures: XSD 1.0-only rules, `xs:NOTATION` as a union member,
  XML 1.1 instances, wildcard consistency with type tables.

Larger duplicates (D237): three UTF-8/UTF-16 decoders with different
error rules; Full Text reads stop words and thesauri through the resolver
itself; private growable arrays in four files. (D182): the schema reader's own boolean and integer
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
