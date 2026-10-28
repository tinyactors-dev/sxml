# Phase 7: Deferred features

Goal (PLAN.md, D162): everything D97 left out of Phases 2 to 4 except
schema awareness (Phase 5): full UCA collations, fn:parse-html and
fn:html-doc, Invisible XML, XSLT streaming, the XQuery Update Facility,
Full Text, EXPath Binary and File, XPath 1.0 compatibility mode and XSLT
backwards-compatible behaviour, a timezone database, other calendars and
languages in the formatting functions, and fn:transform. The owner has
said every deferred feature must be supported eventually (D162). Four
features no phase covers yet are proposed below for the owner. Design:
D183. Rules for parallel packets are those of `docs/tasks/phase-2.md`
(D45, D96, D132: a packet starts when every packet in its "After" list is
committed), plus the shared-table rule below. Phase 7 must not regress the
counts of Phases 2 to 5.

## The design in one page (D183)

- **Each feature replaces its one rejection place** (D162). Every packet
  below names the place where sxml rejects or ignores the feature today,
  with the error the specification gives a processor without it. The
  packet replaces that place. It does not add a second path beside it.
  When the feature is in, the packet also removes the feature from the
  drivers' non-goal lists (`tests/qt4/driver/applicability.c`,
  `tests/xslt/driver/xslt_applicability.c`) and sets its system property
  (`src/system_property.c`).
- **One IR, one evaluator, one tree builder.** No feature adds an
  evaluator or a tree kind:
  - Dynamic evaluation (xsl:evaluate) and fn:transform compile at run
    time and ask the machine for a unit evaluation, as
    fn:load-xquery-module does (glossary `unit_evaluation`, D154).
  - Parsers for other notations (HTML5, Invisible XML, DTD-driven
    entities, XML 1.1) send tree builder events (`tree.h`), as the XML
    parser does. The result is an ordinary arena tree.
  - DTD validation is schema validation. The DTD's declarations become
    schema components (`src/schema_internal.h`), and the validator (a
    tree builder filter, D165) checks them.
  - Updates do not change trees. A pending update list is applied by
    copying the affected trees through the tree builder with the changes
    made, so `tree.h` stays frozen (D99).
  - Streaming consumes the parser's builder events as a receiving task
    (D166). Before that, streamability analysis is a static pass over the
    XSLT AST.
  - Full Text compiles a selection into a match program, as the regex
    engine compiles a pattern (D53). The program is run by a loop, not by
    recursion (D71).
- **No substrate change is planned.** `status`, `view`, `memory`,
  `string`, `name`, `types`, `tree`, `xdm` and `export` stay as they are.
  The planned header changes are additive and each needs a decision
  record:
  - `processor.h`: optional features (7.2), a file-system interface
    (7.9) and new resource kinds (7.10, 7.12, 7.15, 7.16, 7.18);
  - `xpath.h`: the XPath 1.0 compatibility flag (7.4);
  - `xquery.h`: updating queries (7.7);
  - `xml.h`: DTD options (7.14).
- **Size** (below): `sxml.wasm` is 1,998,723 of 2,000,000 bytes. Every
  feature that XPath can reach would link into it. Packet 7.2 carries out
  the owner's choice, and features that would grow `sxml.wasm` wait for
  7.2.

## Not run today, by feature

The counts come from `docs/CONFORMANCE.md` at 3b0812a, and from the
catalogs where the dashboard shows only the 15 largest reasons. "First"
means "the first dependency not met", the dashboard's count. "Any" counts
every case that names the dependency. XQ40 and XP40 share most QT4 cases,
so their counts are not added together.

| Feature | QT4 (XQ40 / XP40) | XT40 | Other suites |
| --- | --: | --: | --- |
| XSLT streaming | – | 2,932 first (2,564 in `tests/strm/`) | – |
| HTML5 (fn:parse-html, fn:html-doc) | 1,401 / 22 (misc-HtmlTestSuite 1,379 XQ only; fn-parse-html 14; fn-html-doc 8) | – | – |
| XQuery Update Facility | 825 (upd-*) / – | – | – |
| Full Text | 685 (ft-*) / – | – | – |
| EXPath Binary | 673 / 673 | – | – |
| XML 1.1 input (proposed) | 313 / 291 | 65 | xmlconf 275 (84 valid, 13 invalid, 173 not-wf, 5 error) |
| EXPath File | 271 / 268 | – | – |
| fn:transform | 133 (21 of them XSLT 3.0) / 129 | 12 in `transform` (9 fail) | – |
| Backwards compatibility | 5 / 5 (xpath-1.0-compatibility) | 124 first | – |
| DTD processing (proposed) | 32 / 27 first (infoset-dtd), plus 7 / 6 FODC0013 failures | 78 first (87 any) | xmlconf 265 with external entities; the 175 invalid cases are checked for well-formedness only |
| xsl:evaluate (proposed) | – | 83, plus 5 XTDE3175 failures | – |
| UCA collations | 49 advanced-uca-fallback; 13 / 5 non_unicode_codepoint_collation; 4 XQST0038 failures | – | Unicode CollationTest (not vendored) |
| Calendars and languages | 24 (language de 16, it 2, fr 1, fr-CA 1; CLDR 4) | 14 (numbering de 6, it 1, fr 1; ICU_spellout 3; ordinals 3) | – |
| disable-output-escaping (proposed) | – | 32 first (33 any) | – |
| Timezone database | 8 olson-timezone, plus 6 FODT0004 failures | – | – |
| Invisible XML | 10 / 10 (of the 16 in fn-invisible-xml) | – | the iXML community suite (not vendored) |

In total these features would unlock about 4,400 of XQ40's 5,505
not-run cases, about 3,330 of XT40's 4,659, and 540 xmlconf cases. Other
not-run features are not planned. They are listed so the owner sees
them: staticTyping (77 XQ40, 48 XP40), xsl-stylesheet-processing-instruction
(23 XT40), HTML4 output (3), schema-location-hint (3), XInclude (2), and
remote_http (2).

When a packet lands, its cases enter the D95 denominators. A feature that
passes below the current rate lowers XP40's, XQ40's or XT40's gate rate
even though no pass is lost. This is why each feature has a gate of its
own, below.

## Size: optional build parts (owner decision, D183 Proposed)

The JavaScript package links XPath only, so the following features cost
nothing in `sxml.wasm`: XSLT- and XQuery-only features (streaming, the
Update Facility, Full Text in the XQuery grammar, xsl:evaluate,
disable-output-escaping, XSLT backwards compatibility). They do grow the
`sxml` CLI module, which is 4,877,565 of 5,500,000 bytes. The other
features are reachable from XPath and would link into `sxml.wasm`. The
estimates below (to be measured by 7.2) are of stripped bytes. Measured
references: linking XSLT added 1,159,058 bytes to the CLI module (D157),
and linking schema validation added 268,168 bytes (D176).

| Feature | `sxml.wasm` (estimate) | CLI module (estimate) |
| --- | --: | --: |
| UCA: DUCET 16.0 tables, algorithm, options | 170–280 KB | same |
| UCA tailorings (CLDR rules, a few languages) | 10–40 KB | same |
| HTML5 parser (with 2,231 named character references) | 100–180 KB | same |
| Timezone database, all zones, compact | 60–120 KB (TZif reader alone: about 5 KB) | same |
| Invisible XML (Earley parser, iXML grammar) | 40–70 KB | same |
| EXPath Binary | 30–50 KB | same |
| EXPath File | 20–30 KB (no use in a browser) | same |
| DTD processing | 30–60 KB | same |
| Calendars and languages | 5–15 KB plus 3–6 KB per language | same |
| XPath 1.0 compatibility mode | 5–10 KB | same |
| XML 1.1 input | 2–8 KB | same |
| fn:transform | 0 if XSLT stays out of the package; else about 1.16 MB | 10–20 KB |
| Streaming / Update / Full Text / evaluate / d-o-e / XSLT compat | 0 | 150–250 / 60–100 / 150–250 / 5 / 3 / 15–25 KB |

If every XPath-reachable feature were linked, `sxml.wasm` would grow by
about 0.5 to 0.8 MB, to about 2.6 MB. The options:

1. **Raise the budget per feature as it lands.** This costs no code.
   Every browser user downloads every table, used or not, and the budget
   ends near 2.8 MB stripped (about 0.9 MB gzipped).
2. **Optional features chosen at link time** (recommended, with 4). One
   public mechanism: the processor configuration takes a view of
   `sxml_optional_feature` values (`processor.h`, additive, a record).
   Each feature has an exported function that returns one, for example
   `sxml_uca_collation_feature()`, `sxml_html_parser_feature()` or
   `sxml_xslt_transform_feature()`. The core reaches a feature only
   through that value's operations table. Without the value, the
   feature's D162 place stays the behaviour (FOCH0002, XPST0017,
   FODT0004), so a program that never names a feature does not link it.
   - What stays the same: one build, every feature tested in every
     build, and no `#if`.
   - The JavaScript package ships `sxml.wasm` as today, plus a larger
     module (or a few) that names the features. Its API takes a feature
     list.
   - Costs: one `processor.h` addition; an operations table per feature
     (about 1 KB each); behaviour that depends on the host's
     configuration (the drivers and the CLI name every feature).
   - The core still needs headroom for core changes (DTD, XML 1.1, the
     compatibility mode). The proposal is 2,100,000 bytes for the core
     and 3,000,000 for the full module.
3. **CMake options** (`SXML_WITH_UCA` and similar). At compile time, stub
   files hold the rejection places in place of the excluded sources.
   - Advantage: no header change.
   - Costs: at least two configurations to build and test (minimal and
     full); a binary's features are fixed when it is built; and two
     sxml binaries of one version behave differently.
4. **Large data as host resources** (adds to 2 or 3). sxml compiles in
   readers for standard formats, and the host supplies the data through
   the resource resolver:
   - DUCET as `allkeys.txt` (UTS #10 format) and tailorings as CLDR rule
     text (UTS #35, Part 5);
   - the timezone database as TZif files (RFC 8536): the CLI reads
     `/usr/share/zoneinfo`, and the JavaScript package fetches files on
     demand.

   A compiled built-in copy can be its own optional feature, for hosts
   that want no files. Costs:
   - new resource kinds (`processor.h`, additive);
   - parsing at first use (`allkeys.txt` is 1.9 MB of text; loading it
     takes an estimated 20 to 60 ms);
   - correctness that depends on the version of the host's data (tests
     pin it);
   - no help for features that are mostly code (HTML5, iXML, Binary).

## Parallelism plan

| Wave | Packets (run together) | Why first |
| --- | --- | --- |
| 1 | 7.2 optional features (after the owner's choice), 7.3 xsl:evaluate, 7.4 backwards compatibility, 7.5 disable-output-escaping | Small, low risk, no `sxml.wasm` cost (7.4's XPath flag aside). 7.4 matters most to users: today every `version="1.0"` stylesheet raises XTDE0160 when an instruction of it is evaluated |
| 2 | 7.6 XSLT streaming (a, b), 7.7 Update Facility (a, b, c) | The two largest sets (2,932 and 825); XSLT and XQuery only |
| 3 | 7.8 EXPath Binary, 7.9 EXPath File, 7.10 HTML5 (a, b), 7.11 fn:transform | Large sets or high demand, after 7.2 |
| 4 | 7.12 Full Text (a, b, c), 7.13 XML 1.1 input, 7.14 DTD processing (a, b) | Highest risk (Full Text); 7.13 and 7.14 share the XML parser, so 7.14 follows 7.13 |
| 5 | 7.15 UCA (a, b), 7.16 timezone database, 7.17 calendars and languages, 7.18 Invisible XML | Few cases and much data; 7.17 follows 7.16 (both use `src/format_date.c`) |
| 6 | 7.19 review (gate) | |

Dependencies ("After"):
- 7.2 depends on the owner's size decision.
- 7.3, 7.4 and 7.5 can start now.
- 7.6b after 7.6a; 7.7b after 7.7a; 7.7c after 7.7b.
- After 7.2: 7.8, 7.9, 7.10a, 7.11, 7.13, 7.14a, 7.15a, 7.16, 7.17 and
  7.18, which all grow `sxml.wasm` (option 1 means "after the owner
  raises the budget").
- 7.10b after 7.10a.
- 7.12b after 7.12a; 7.12c after 7.12b.
- 7.14a after 7.13; 7.14b after 7.14a.
- 7.15b after 7.15a.
- 7.17 after 7.16 and after 7.15b (both use CLDR data).
- 7.19 after everything the owner accepted.

The critical path is 7.6a, 7.6b and 7.19. The proposed packets (7.3,
7.5, 7.13, 7.14) start only when the owner accepts them.

**Shared tables.** Each packet changes only its own entries in the
drivers' dependency and non-goal tables and in `src/system_property.c`,
as single hunks staged ledger-style (BRIEF.md). The CLI grows only when a
feature works (BRIEF.md, D44).

## Packets

Each packet gives: the specifications; the cases it unlocks; the
rejection place it replaces and the mechanism it extends; header changes;
dependencies ("After"); what it owns; size; and its gate (proposed, for
the owner). "Owns" lists new files, plus the existing files the packet
changes beyond single shared-table entries.

### 7.1 Phase 7 plan (design)
After the Phase 4 gate (D162). Owns this file, D183, PLAN.md's Phase 7
row and the glossary terms of D183.
**Done** 2026-10-19 (D183).

### 7.2 Optional features and the size budget (design, build)
After the owner's size decision (D183 Proposed).
- **Specifications**: none. This is an engineering mechanism.
- **Owns**: `include/sxml/processor.h` (option 2: `sxml_optional_feature`,
  `sxml_optional_feature_view` as the last field of
  `sxml_processor_configuration`, and the record), new
  `src/optional_feature.c` and `src/optional_feature_internal.h`,
  `bindings/javascript/` (module variants, the feature list in the API,
  the budgets in `CMakeLists.txt`), and the CLI's and drivers' processor
  setup (single hunks).
- **Header change**: under option 2, an additive change to `processor.h`;
  under option 3, none (CMake options and stub files); under option 4,
  new resource kinds, added by the packets that need them.
- **Mechanism**: a feature's operations table is `static const`. The core
  finds it by kind in the processor (no global state) and never calls
  one feature from another (D71). The D162 place of each feature becomes
  "this feature is not configured".
- **Done when**: the mechanism exists, with one feature-free test
  feature; the size test checks both modules; and the size of
  `sxml.wasm` is measured before and after (it must not grow).
**Done** 2026-10-19 (D189): `sxml.wasm` 2,003,575 to 2,004,485 bytes
(the mechanism's own 910 bytes; no feature in the core),
`sxml-full.wasm` 2,005,513; budgets 2,100,000 and 3,000,000.

### 7.2b Core size audit (build)
After 7.14b and D208. The core `sxml.wasm` was 2,294,051 bytes stripped,
over D207's 2,200,000 / 770,000.
- **Owns**: the DTD validation feature (`include/sxml/schema.h`,
  additive; `src/schema_from_dtd.c`, `src/dtd_internal.h`), the loader's
  dtd-validation place in `src/processor.c`, single hunks in
  `src/optional_feature*.c/h`, `bindings/javascript/CMakeLists.txt` and
  `tests/xpath_test.c`.
- **Done when**: per-object and per-commit growth measured; large
  separable parts behind D189 features with their D162 place; the core
  under budget, or a minimal raise recorded (D208); the touched QT4 and
  XT40 sets unchanged with all features; the xs:numeric Proposed item
  resolved by D208.
**Done** 2026-10-19 (D211): DTD validation is the optional feature
"dtd-validation" (FODC0013 without it); `sxml.wasm` 2,294,051 / 801,273
to 2,128,754 / 741,943, no budget raised; counts unchanged.

### 7.2c Per-language part lists (build; packet size-2)
After 7.2b and D225. The part registries were global, so the core
`sxml.wasm` linked XQuery's lowering parts (about 75 KB, D189) and the
XSLT-only library and evaluator parts (D225).
- **Owns**: `cmake/SxmlParts.cmake`, the registry lines of
  `CMakeLists.txt`, the registry declarations in
  `src/function_library_parts_internal.h`, `src/xpath_lower_internal.h`
  and `src/evaluator_machine_internal.h`, single hunks where each
  language names its lists, `tests/part_registry_test.c`, the size
  baseline.
- **Done when**: one mechanism makes a module link only the parts of the
  languages it offers; the core drops the XQuery-only and XSLT-only parts
  while the full module, the CLI and native builds keep them; whole
  release QT4 and XT40 runs unchanged; D225's baseline refreshed.
**Done** 2026-10-19 (D227): one list per language and registry,
generated by the glob and named by the language's entry point;
units carry their language's evaluator parts; `sxml.wasm` 2,215,004 /
774,905 to 2,094,214 / 731,897 (lowering and library, at 5385a0e) and
2,101,275 / 734,951 to 2,077,439 / 725,473 (evaluator_stream and
evaluator_template, at 5664da8); QT4 and XT40 unchanged.

### 7.4 XSLT backwards-compatible behaviour and XPath 1.0 compatibility mode (implementation)
- **Specifications**:
  - XPath 4.0, 2.2.1 (XPath 1.0 compatibility mode in the static
    context) and the rules that depend on it: general comparisons,
    arithmetic, and the coercion rules (first item, fn:number and fn:string conversions);
  - F&O 4.0, fn:format-number in 1.0 mode;
  - XSLT 4.0, 3.8 "Backwards Compatible Processing" and the 1.0 rules it
    names (xsl:value-of and attribute value templates take the first
    item, xsl:number, sort keys, xsl:call-template and parameter
    conversions); XTDE0160 is no longer raised.
- **Unlocks**: XT40 backwards_compatibility 124 (backwards 47,
  xpath-compat 18, xslt-compat 13, and others) and QT4
  xpath-1.0-compatibility 5. It also unlocks every real-world
  `version="1.0"` stylesheet.
- **Replaces**: `asks_xslt_1_behaviour` and its XTDE0160 raise in
  `src/xslt_lower.c` (D162's place), and
  supports-backwards-compatibility.
- **Mechanism**: a flag of the static context that the XPath lowering
  reads. It inserts the 1.0 conversions as ordinary IR (the first item,
  then fn:number or fn:string), so the evaluator does not change. XSLT
  sets the flag for elements whose effective version is below 2.0.
- **Header**: `xpath.h`, additive: a last field of
  `sxml_static_context_configuration` (for hosts and the QT4 driver),
  with a record. `xslt.h`: none.
- **After**: nothing.
- **Owns**: `src/xpath_lower.c` (the conversions), `src/xslt_lower.c`
  (the XTDE0160 place), `src/xslt_lower_construct.c` (value-of and
  attribute value templates), `src/xslt_lower_number.c`, and new
  `tests/xslt_compatibility_test.c`.
- **Size**: 5–10 KB in `sxml.wasm`, so it waits for 7.2 or for a budget
  raise; the XSLT part is in the CLI only.
- **Gate**: ≥ 80 % of the 124 and all 5 QT4 cases.

**Done** 2026-10-19 (D197), gate met for XT40, not for QT4: XT40 111 of
the 124 (89.5 %); QT4 1 of 5, the four others and ten XT40 cases expect
fn:number conversion for xs:numeric parameters (fn:format-number,
fn:round, fn:floor), which XPath 4.0, 3.4.1 gives only to xs:double.
`xpath_compatibility.c` is in the core module (a few KB).

**7.4b** (D206, follow-ups to 7.4): exsl:node-set and exsl:object-type
(EXSLT common, a new well-known namespace), exsl:document as
xsl:result-document, docbook-002's XPST0003, text value templates in 1.0
elements without the mode (3.8.1), xsl:result-document's xhtml default in
a 1.0 stylesheet (backwards-019b), the size of `xpath_compatibility.c` in
`sxml.wasm`; the xs:numeric question put to the owner. **Done**
2026-10-19: XT40 8,940 to 8,943 (docbook-001, docbook-002 and
backwards-019b pass); `xpath_compatibility.c` is about 7.2 KB, the EXSLT
part 1.2 KB of the core module.

### 7.6 XSLT streaming (design and implementation, two packets)
- **Specifications**: XSLT 4.0, 18 "Streaming" (xsl:source-document
  streamable="yes", xsl:merge with streamable sources, xsl:fork,
  streamable modes and accumulators, burst-mode functions such as
  fn:snapshot and fn:copy-of) and 19 "Streamability" (postures, sweeps,
  the streamability rules of each construct, guaranteed streamability,
  XTSE3430); XSLT 4.0, 27 (the streaming conformance feature).
- **Unlocks**: XT40 streaming 2,932 (`tests/strm/` 2,564: sf-, si-, sx-
  sets; `streamable` 139; `streaming-fallback` 6, today run unstreamed).
- **Replaces**: today streamable="yes" runs unstreamed, as a
  non-streaming processor may (`src/xslt_read.c`, `src/xslt_accumulator.c`,
  supports-streaming false, the driver's streaming-fallback).
- **7.6a Streamability analysis**:
  - a static pass over the XSLT AST and the lowered XPath that gives each
    construct its posture and sweep (glossary `posture`, `sweep`) by
    XSLT 4.0, 19, and raises XTSE3430 where a construct declared
    streamable is not guaranteed-streamable;
  - streamed constructs still run unstreamed, which the suite cannot
    tell apart: every result, plus the static errors;
  - the drivers count streaming cases;
  - supports-streaming stays false until 7.6b.
  - Owns: new `src/xslt_streamability.c` and
    `src/xslt_streamability_internal.h`.
  - **Done** 2026-10-19 (D212): XT40 streaming cases 2,841 of 2,936
    (strm 2,503 of 2,564), 154 of 156 XTSE3430 cases; no pass lost but
    system-property-013, now not applicable.
- **7.6b Streamed evaluation**:
  - the XML parser feeds builder events to a receiving task (D166) in
    place of an arena builder;
  - the lowering of a guaranteed-streamable mode or source document
    selects nodes as their events pass: a streamed node is a window of
    its ancestors, its attributes and its own events, held in allocator
    memory;
  - grounded operands (`fn:copy-of`, `fn:snapshot`) build small trees
    through the tree builder;
  - memory is bounded by the depth and the grounded parts, not by the
    document;
  - claims the feature (supports-streaming true).
  - Owns: new `src/evaluator_stream.c` and
    `src/xslt_lower_stream.c`, the receiving side in
    `src/evaluator_construct.c` (single hunks agreed with the evaluator's
    owners), and a memory test of a 1 GB generated document.
  - **Done** 2026-10-19 (D219): xsl:source-document bodies of
    motionless instructions and one xsl:for-each over a downward path
    stream (STREAM, `src/evaluator_stream.c`, an incremental parse);
    allocator peak 232 KB at 2.4 MB to 106 MB, but tree-sitter's syntax
    tree of the whole input remains (3.98 GB at 106 MB, so no 1 GB test);
    the fallback option in `xslt.h`; supports-streaming true. XT40 11,797
    to 11,804, none lost. Left: accumulators, streamable modes and
    apply-templates, iterate, fork, merge, aggregates, the principal
    source, incremental tokenizing of content.
- **7.6c More streamed shapes** (after 7.6b; D223): reuse the STREAM
  machinery, 7.6a's sweeps and the shared evaluator for, in order,
  apply-templates and streamable modes with position(); accumulators;
  aggregates over a streamed path; attribute steps at the end of a path;
  xsl:iterate, xsl:fork, xsl:merge; a streamed principal source. Count
  the source-document executions and mode runs that stream.
  - Owns: `src/xslt_lower_stream.c`, `src/evaluator_stream.c`,
    `tests/evaluator_stream_test.c`; single hunks in `src/xpath_lower.c`
    (the lowering override), `src/xslt_streamability.c` (mode verdicts),
    `src/xslt_lower_template.c`, `src/evaluator_template.c` and the IR.
  - **Done** 2026-10-19 (D223): streamed paths in value-of, copy-of,
    sequence, for-each, apply-templates, iterate and for-each-group
    selects (counted, folded, atomized or full windows; a last attribute
    step); template rules applied node by node with their position;
    declared but unused accumulators no longer stop streaming; a D219
    text bug fixed. XT40 no case changed; streamed source documents 164
    to 986; 57 nodes get templates one by one, 600,075 fold. Left:
    accumulators computed while streaming, xsl:fork, xsl:merge, the
    streamed principal source (no `xslt.h` field), template rules with
    stream points of their own.
- **7.6d The remaining streamed shapes** (after 7.6c; D228), in order of
  value: (a) a streamed principal source, an additive `xslt.h` field with
  a record, used by `sxml transform`, with the CLI's memory at 100 MB
  before and after; (b) accumulators computed while streaming; (c)
  xsl:fork, then xsl:merge with streamed inputs; (d) template rules with
  stream points of their own; (e) avg folding; (f) the IR printer's new
  fields.
  - Owns: `src/xslt_lower_stream.c`, `src/evaluator_stream.c`,
    `tests/evaluator_stream_test.c`, `include/sxml/xslt.h` (the field),
    `apps/sxml/command_transform.c`; single hunks in `src/xslt.c`,
    `src/xslt_lower.c`, `src/xslt_lower_template.c`,
    `src/xslt_streamability.c`, the IR and the XSLT driver.
  - **Done** 2026-10-19 (D228): (a) `streamed_source_uri` and
    `sxml_xslt_stylesheet_streams_source`; `sxml transform` streams an
    INPUT file when the initial mode streams: "/" with a for-each over a
    100 MB document 673.2 MB to 267.3 MB resident, template rules 658.8 MB
    (tree) to 267.3 MB; the driver streams the suite's streaming="true"
    sources. (d) template rules of streamed modes run on open nodes,
    streamed or grounded (STREAM axis self); built-in rules, apply-
    templates without select and child-step apply points stream. (e) avg
    folds as sum and count. (f) done. XT40 none lost. Left: (b)
    accumulators while streaming, (c) xsl:fork and xsl:merge, and a
    principal result held whole until it is serialized (the identity
    keeps memory proportional to its output).
- **7.6e The last streamed shapes** (after 7.6d; D231), in order: (a)
  accumulators computed while streaming: pre-descent and post-descent
  rules, fn:accumulator-before and -after on streamed nodes, the
  streamability of accumulator rules (18.2.9); (b) xsl:fork, then
  xsl:merge with streamed inputs; (c) the principal result delivered as
  it is produced, through the result writer, so a streamed identity runs
  in bounded memory end to end; (d) a resolver form that delivers bytes
  incrementally (additive in `processor.h`, with a record), so `sxml
  transform` does not hold its input. Peak resident memory of `sxml
  transform` on a generated 100 MB document before and after.
  - Owns: `src/evaluator_stream.c`, `src/xslt_lower_stream.c`,
    `src/xslt_accumulator.c`, `tests/evaluator_stream_test.c`; single
    hunks in `src/function_library_xslt.c` (the row), the IR
    (`sxml_ir_stream_accumulators`), `src/evaluator_internal.h`,
    `src/xslt_streamability.c`, `src/xslt_lower_template.c`,
    `src/xslt_lower_control.c` (xsl:fork), `src/xslt.c`, the serializer,
    the result writer, the XML parser's input, `include/sxml/processor.h`
    (the byte reader) and `apps/sxml/command_transform.c`.
  - **Done** (D231): (a), (b) xsl:fork and (c); `sxml transform`
    identity on 95.6 MB 525 MB to 134 MB peak resident, accumulators
    more than 4.1 GB to 135 MB; XT40 12,031 to 12,048, none lost. Left:
    xsl:merge with streamed inputs (a new evaluator part) and (d), whose
    parser change waits for `src/xml.c`, `src/xml_segment.c` and
    `src/xml_encoding.c` (perf-3); the design is in D231.
- **7.6f The last streaming items** (after 7.6e and perf-3; D233): (a)
  incremental bytes as D231 designed them: the additive resource form
  SXML_RESOURCE_FORM_BYTE_READER in `processor.h`, the incremental parse
  decoding its input a window at a time (UTF-16 and ISO-8859-1 too), the
  CLI's resolver delivering files that way, peak resident memory of
  `sxml transform` on a streamed identity before and after, the
  JavaScript host unchanged; (b) xsl:merge with streamed inputs, a new
  evaluator part reading several documents in step; (c) capture="yes"
  end rules while streaming, if cheap.
  - Owns: new `src/xml_input.c`; `src/xml.c`, `src/xml_encoding.c`,
    `src/xml_internal.h`, `include/sxml/processor.h` (the form),
    `src/processor.c`, `src/evaluator_stream.c`, `tests/xml_test.c`,
    the CLI's file reader and resolver.
  - **Done** 2026-10-20 (D233): (a) the byte reader form and the text
    window (`src/xml_input.c`); `sxml transform` on 95.6 MB, streamed
    identity 133.9 MB to 11.0 MB peak resident, for-each 134.5 MB to
    11.0 MB, 974 MB in 10.9 MB; the JavaScript host unchanged (BYTES);
    XT40 11,918 and XQ40 45,914 passed, unchanged case by case. Left:
    (b) xsl:merge with streamed inputs and (c) capture="yes" end rules,
    designed in D233; both need a new IR form or function and an
    evaluator part in files packet 4.21 held uncommitted
    (`src/ir_internal.h`, `src/ir.c`, `src/ir_verify.c`,
    `src/evaluator.c`, `src/evaluator_machine_internal.h`).
- **7.6g The last two streaming items** (after 7.6f and 4.21; D233,
  D236): (a) xsl:merge with streamed inputs, a MERGE part with one cursor
  per anchor document, keyed k-way stepping and XTDE2220 for input out of
  order, reusing the STREAM machinery, the merge key rules and the
  evaluator's resumable tasks; (b) capture="yes" end rules, a capture
  function with the stream builder copying the element's events into a
  full window. Keep GLOBAL_READ and the global-read pass working (D234).
  - Owns: `src/evaluator_stream.c`, `src/xslt_lower_group.c` (the
    merge), `src/xslt_lower_stream.c`, `src/xslt_accumulator.c`,
    `tests/evaluator_stream_test.c`; single hunks in the IR
    (`src/ir_internal.h`, `src/ir.c`, `src/ir_verify.c`, the passes),
    `src/evaluator.c` and `src/xquery_prolog.c`.
  - **Done** 2026-10-20 (D236): MERGE and its merge streams; tees for
    capturing end rules. Two 38.3 MB documents merged: peak resident
    1,055.6 MB to 11.0 MB. XT40 11,943 to 11,945, none lost; 27 merge
    and 5 capture cases stream. Left: merge sources with validation,
    type, accumulators or predicates (grounded).
- **Header**: possibly `xslt.h`, additive: a streamed principal source
  given as bytes or a parser, with a record. The resolver's BYTES form
  already serves xsl:source-document.
- **After**: 7.6b after 7.6a.
- **Size**: 0 in `sxml.wasm`; 150–250 KB in the CLI.
- **Gate**: ≥ 85 % of the in-scope streaming cases; XT40 passes not
  dropped; the memory test's peak is under 64 MB.

### 7.7 XQuery Update Facility 3.0 and 4.0 (design and implementation, three packets)
- **Specifications**: XQuery Update Facility 3.0:
  - 2 (extensions to the processing model: updating expressions,
    pending update lists, snapshots);
  - 3 (the insert, delete, replace, replace value of, rename and copy
    modify (transform) expressions, %updating functions, fn:put,
    revalidation declarations);
  - 4 (update primitives and upd:applyUpdates, upd:mergeUpdates,
    upd:revalidate);
  - its error codes (XUST, XUDY, XUTY).

  Also the XQuery Update Facility 4.0 community draft where the QT4
  tests follow it (it is not yet a W3C draft: owner question 6).
- **Unlocks**: XQ40 upd-* 825 cases, including revalidation (strict 21,
  lax 19, skip 34); upd-StaticTyping's 27 also need staticTyping, so they
  stay out.
- **Replaces**: two places today. `src/xquery_static.c` rejects %updating
  as a reserved-namespace annotation (XQST0045), and the update
  expressions are not in `grammars/xquery` (XPST0003).
- **7.7a Design**:
  - the AST kinds, and the IR's update primitives: an updating
    expression's value is a pending update list, carried as an internal
    item kind of the evaluator, not an XDM item;
  - how XQuery returns updated documents to the host: `xquery.h`,
    additive, an updating query and an update sink that receives each
    changed document as a new tree, plus fn:put's targets;
  - the `sxml xquery` design for updates (a flag that writes the changed
    documents);
  - the D-record.
- **7.7b Updating expressions and application**:
  - the grammar (insert, delete, replace, rename, the copy-modify
    expression and `%updating`);
  - static rules (XUST0001, XUST0002) in `src/xquery_static.c`;
  - lowering of every updating expression into primitive constructors;
  - merging and the compatibility checks (XUDY0015, XUDY0016, XUDY0017,
    XUDY0024 namespace conflicts);
  - application by rebuilding each target tree through the tree builder
    with the primitives applied in the order of XQUF 3.0, upd:applyUpdates;
  - the copy-modify expression copies with `sxml_tree_builder_copy_node`
    and applies to the copy.

  Node identity across an update cannot be observed inside one snapshot.
  Hosts get the new trees (XQUF 3.0, upd:applyUpdates: how the changes are made
  persistent is implementation-defined).
- **7.7c fn:put and revalidation**: fn:put through the update sink;
  revalidation by the schema validator over the rebuilt tree
  (`schema.h`, D165).
**7.7a done** 2026-10-19 (D187), with the 3.0 part of 7.7b as the owner's
order asked (D184: 3.0 first): grammar, static rules, every 3.0
expression including transform-with and invoke updating, application,
the xquery.h update sink, the QT4 driver and `sxml xquery`. upd-* 0 to
733 of 825 passed (10 wrong error), 24 failed, 0 errors, 68 not run; the
gate's 85 % of 798 is 679. **7.7b** is now the 4.0 draft's additions
(the updating return clause of copy-modify, id-transform-expr-014;
XUDY0038 for dynamic calls of updating function items), **7.7c** fn:put
(13 cases) and revalidation lax and strict (40 cases, XUST0026 today).
Not in this packet: copies with construction strip keep is-id and
is-idrefs (src/tree.c), which 11 cases need gone.
**7.7b and 7.7c done** 2026-10-19 (D205): fn:put through the update
sink's receive_put_document (13 of 13), revalidation lax and strict
(XUST0026 gone), copy-modify copies untyped and without schema IDs
(`sxml_copy_request.strips_identity`, additive in `tree.h`), the 4.0
draft's updating return clause, setToUntyped for content going into an
untyped node. upd-* 733 to 787 of 825 (98.6 % of the 798 in scope); left:
6 propagateNamespace cases whose expected whitespace differs from what
their queries make, 2 that expect no IDREF check when an element is
validated, 27 staticTyping, 3 for processors without a revalidation
mode; XUDY0038 for simple dynamic calls of updating function items (no
test).
- **Header**: `xquery.h`, additive (7.7a). `tree.h`: none.
- **After**: 7.7b after 7.7a; 7.7c after 7.7b.
- **Owns**: `grammars/xquery` (the update productions), new
  `src/xquery_update.c` and `src/xquery_update_internal.h`, new
  `src/evaluator_update.c`, and the update hunks of `src/xquery_static.c`
  and `src/xquery_lower.c`.
- **Size**: 0 in `sxml.wasm` (XQuery is not in it); 60–100 KB plus the
  grammar's growth in the CLI.
- **Gate**: ≥ 85 % of the 825 (less the 27 static-typing cases).

### 7.8 EXPath Binary (implementation)
- **Specifications**: EXPath Binary Module 4.0, as the QT4 bin- sets test
  it (namespace `http://expath.org/ns/binary`; the bin:
  functions and their errors bin:differing-length-arguments,
  bin:index-out-of-range, bin:negative-size, bin:octet-out-of-range,
  bin:non-numeric-character, bin:unknown-encoding,
  bin:conversion-error, bin:unknown-significance-order).
- **Unlocks**: 673 (XP40 and XQ40), in 39 bin- sets.
- **Replaces**: the functions are absent (XPST0017), and
  supports-binary-library is false.
- **Mechanism**: a function-library part of its own over xs:base64Binary
  items (`src/function_library_binary.c`). Encodings go through the
  existing decoders (`src/xml_encoding.c`, D81), and number packing
  through the decimal and double code of `src/cast.c`.
- **Header**: none (with option 2: one feature function).
- **After**: 7.2.
- **Owns**: new `src/function_library_binary.c`.
- **Size**: 30–50 KB.
- **Gate**: ≥ 95 % of the 673.
**Done** 2026-10-19 (D198): bin-* 646 of 646 in XP40 and XQ40 (26 wrong
error codes, XPTY0004 by the 4.0 types); fn-serialize +24 in each; the
673 also counted fn-serialize's and fn-system-properties' binary cases.

### 7.9 EXPath File (design and implementation)
- **Specifications**: EXPath File Module 4.0, as the QT4 expath-file set
  tests it (namespace `http://expath.org/ns/file`; file:exists, file:list,
  file:read-text, file:write, file:create-dir, file:delete,
  file:path-to-uri, and the others; the file: errors).
- **Unlocks**: 271 XQ40 and 268 XP40 (expath-file 270, plus 1).
- **Replaces**: the functions are absent (XPST0017), and
  supports-file-library is false.
- **Mechanism**: library code never touches the file system. A host
  interface does, next to the resolver and the clock: a file system with
  an operations table (read, write, append, list, stat, create, delete,
  move; each with at most three parameters, using request structs). The
  functions are nondeterministic, and evaluation order is the
  evaluator's. Off by default: the null file system makes every function
  raise file:io-error.
- **Header**: `processor.h`, additive: `sxml_file_system` in the
  processor configuration, with a record.
- **CLI**: implements the interface over POSIX and Windows. It is off
  unless `--allow-file-access` is given (owner question 7).
- **After**: 7.2 (in a browser the feature is never linked).
- **Owns**: new `src/function_library_file.c`, and the CLI's platform
  file (single hunks).
- **Size**: 20–30 KB, none in the JavaScript package.
- **Gate**: ≥ 90 % of the 271.
**Done** 2026-10-19 (D200): expath-file 270 of 270 (XQ40) and 267 of
267 (XP40; 3 cases are XQuery-only); fn-system-properties' file case
passes. About 66 KB native, none in the JavaScript modules. The CLI's
Windows file system is not written yet.

### 7.10 HTML5 parsing: fn:parse-html and fn:html-doc (implementation, two packets)
- **Specifications**:
  - F&O 4.0, fn:parse-html and fn:html-doc (the options: method,
    html-version, encoding, include-template-content; the XHTML namespace
    result);
  - HTML Living Standard (WHATWG), 13.2 "Parsing HTML documents"
    (encoding sniffing 13.2.3, tokenization 13.2.5, tree construction
    13.2.6) and 13.5 "Named character references".
- **Unlocks**: XQ40 1,401 (misc-HtmlTestSuite 1,379, from the
  web-platform-tests HTML suite; fn-parse-html 14; fn-html-doc 8) and
  XP40 22.
- **Replaces**: the functions are absent (XPST0017). Also the drivers'
  non-goal test sets misc-HtmlTestSuite, fn-parse-html and fn-html-doc.
- **7.10a Tokenizer**: the tokenizer's 80 states as a loop over a state
  variable; the named-reference table as a sorted array; encoding
  sniffing through `src/xml_encoding.c`.
- **7.10b Tree construction**:
  - the insertion modes, the stack of open elements, active formatting
    elements, the adoption agency algorithm and foster parenting;
  - these move nodes already built, so construction works on an HTML
    working tree (glossary `html_working_tree`): a node table in
    allocator memory that is walked with an explicit stack, and sent to
    the tree builder once the document ends;
  - the result is an ordinary arena tree in the XHTML namespace (F&O 4.0,
    fn:parse-html);
  - fn:html-doc reads bytes through the resolver (BINARY, so sniffing
    applies).
- **Header**: none. The XML parser's grammar is not used: D12 does not
  apply to HTML, whose tokenizer is specified as a state machine and
  whose input never fails. The D53 reasoning applies, recorded in the
  D-record of 7.10a.
- **After**: 7.2; 7.10b after 7.10a.
- **Owns**: new `src/html.c`, `src/html_tokenizer.c`,
  `src/html_tree_construction.c`, `src/html_internal.h`,
  `src/html_entity_table.c` (generated by a new
  `tools/generate-html-entities.py` from WHATWG's `entities.json`), and
  new `src/function_library_html.c`.
- **Size**: 100–180 KB.
- **Gate**: ≥ 95 % of misc-HtmlTestSuite and ≥ 90 % of fn-parse-html and
  fn-html-doc.
**7.10a done** 2026-10-19 (D201): the tokenizer and tree construction both,
so the gate is met: misc-HtmlTestSuite 1,378 of 1,379, fn-parse-html 22 of
22, fn-html-doc 9 of 10 (XQ40). 7.10b: the list at the end of D201.
**7.10b done** 2026-10-19 (D218): the meta prescan and the change of
encoding while parsing, every legacy multi-byte decoder, XHTML parsed as
XML, the html5lib tests vendored and run by `tests/html5lib_test.c`
(tree construction 1,749 of 1,753, tokenizer 7,045 of 7,045, encoding 82
of 82), the CLI's static base URI; source locations wait for the
processor to keep them.

### 7.11 fn:transform (implementation)
- **Specifications**: F&O 4.0, fn:transform (the options map:
  stylesheet-location, stylesheet-node, stylesheet-text, package-name,
  initial-mode, initial-template, initial-function, source-node,
  global-context-item, stylesheet-params, static-params,
  delivery-format, post-process, requested-properties, and others; the
  result map of principal and secondary results; FOXT0001 to FOXT0006);
  XSLT 4.0, 2.3 (initiating a transformation).
- **Unlocks**: XQ40 133 (fn-transform-XSLT; 21 of them ask for XSLT 3.0,
  which a 4.0 processor serves), XP40 129, and the XT40 `transform` set
  (12 cases, 9 failing).
- **Replaces**: the function is absent (XPST0017); supports-dynamic-xslt
  is false.
- **Mechanism**: the XSLT layer supplies the function, as XQuery
  supplies fn:load-xquery-module (`src/xquery_load_module.c`, D154). Its
  body compiles the stylesheet with `xslt.h`'s compiler, then runs the
  transformation as a unit evaluation with XSLT's
  `unit_evaluation_extension` (D157). Result documents are collected
  into the result map (`result_delivery` raw or as trees). No nested run
  happens (D71).
- **Header**: with option 2, `xslt.h` exports
  `sxml_xslt_transform_feature()`, so XPath and XQuery hosts that never
  name it do not link XSLT. Otherwise none (the XSLT and XQuery
  processors install it as a layer table).
- **After**: 7.2.
- **Owns**: new `src/xslt_transform_function.c`.
- **Size**: 10–20 KB in the CLI; 0 in `sxml.wasm` unless the package
  names the feature (then about 1.16 MB, measured as XSLT's cost in
  D157).
- **Gate**: ≥ 80 % of the 133 and all 12 XT40 cases.
**Done** 2026-10-19 (D203): QT4 fn-transform 129 of the 133 run (XQ40;
XP40 126 of 130), gate met; XT40 transform 9 of 12, not all: a static
fn:transform (transform-004), a ~record pattern (transform-401) and an
assertion the driver does not check (transform-009). The rest is D203's
"Not done".

### 7.12 XQuery and XPath Full Text 3.0 (design and implementation, three packets)
- **Specifications**: XQuery and XPath Full Text 3.0 (W3C
  Recommendation, 2015):
  - 2 (the grammar: FTContainsExpr, FTSelection, FTPosFilter,
    FTMatchOptions, FTScoreVar);
  - 3 and 4 (semantics through the AllMatches model, glossary
    `all_matches`);
  - 5 (conformance: the optional features, such as ignore option, units,
    defaults, extension selections, and scoring);
  - Unicode UAX #29 (word boundaries) for the default tokenizer.
- **Unlocks**: XQ40 ft-* 685, including fullText-ignore 14,
  fullText-units 10 and fullText-defaults 4. The QT4 cases are XQuery
  only.
- **Replaces**: `contains text` is not in the grammar today (XPST0003),
  and the driver lists fullText* as a non-goal.
- **7.12a Grammar and AST**: the Full Text productions in
  `grammars/xquery` only (XPath hosts and `sxml.wasm` do not pay for
  them); the AST kinds.
- **7.12b Matching**:
  - a full-text selection (glossary `full_text_selection`) compiles into
    a match program, as a regex compiles (D53): words, any/all/phrase,
    ftor, ftand, mild not and not, and the positional filters (ordered,
    window, distance, scope, content), evaluated over token positions by
    a loop with an explicit stack;
  - the tokenizer: UAX #29 words, sentences and paragraphs, over the
    existing Unicode tables;
  - the score variables of for and let.
- **7.12c Match options**:
  - case and diacritics, through the case and normalization tables;
  - wildcards, through the regex engine;
  - language;
  - stop words and thesaurus, through the resolver: new resource kinds
    FULL_TEXT_STOP_WORDS and FULL_TEXT_THESAURUS (the QT4 catalog
    supplies them as files);
  - stemming: a built-in English stemmer (Snowball English, BSD), with
    other languages as implementation-defined fallbacks.
- **Header**: `processor.h`, additive (the two resource kinds); `xquery.h`
  none.
- **After**: 7.12b after 7.12a; 7.12c after 7.12b.
- **Owns**: `grammars/xquery` (the Full Text productions), and new
  `src/full_text.c`, `src/full_text_internal.h`, `src/full_text_match.c`,
  `src/full_text_tokenize.c`, `src/xquery_lower_full_text.c`.
- **Size**: 0 in `sxml.wasm`; 150–250 KB in the CLI (most of it grammar
  tables).
- **Gate**: ≥ 75 % of the 685. Scoring and thesaurus results are partly
  implementation-dependent.
**7.12a done** 2026-10-19 (D214): the grammar, AST, static rules,
tokenizer, the match program over AllMatches rows with every
connective, FTWords variant and positional filter, case, diacritics,
wildcards and explicit stop words, score variables, as the optional
feature full-text; QT4 ft-* 624 of 685 pass (91 %), gate met.
`sxml.wasm` +2 KB, the CLI and `sxml-full.wasm` +540 KB (grammar
500 KB). 7.12b/c: stemming, stop word lists and thesauri by URI
(the two resource kinds; fullText-defaults), scores from the matches.
**7.12b and 7.12c done** 2026-10-19 (D214, amended): Snowball stemmers
for en, de, fr, it, es and nl, interpreted from the vendored .sbl files
(all 210,829 snowball-data words stem as Snowball 3.1.1 does); stop
word lists and thesauri by URI through the resolver (the two resource
kinds of `processor.h`, FTST0008 and FTST0018, relationships and
levels, the defaults; fullText-defaults); scores from the matches and
their weights (let score and for score); zh and ja by UAX #29's
one-character words. QT4 ft-* 624 to 672 of 685 (98.1 %), XQ40 45,839
to 45,868; the 13 left are expected-result whitespace (9),
stemmer-dependent (2) and fullText-units (2). `sxml.wasm` +2 KB (it was
over its budget before), `sxml-full.wasm` and the CLI +19 KB.
- **7.12d XPath Full Text; EXPath prefixes in the JavaScript package**
  (after 7.12c): (1) the Full Text productions for XPath (Appendix B),
  with XQuery's contextual keywords, sharing XQuery's lowering and the
  feature full-text; without the feature the syntax stays XPST0003 in
  one place; the core `sxml.wasm` grows by a few KB at most, or a
  cleaner split is found (D208); the QT4 ft-* cases that run under XP40
  measured. (2) The package binds the EXPath prefixes wherever the
  feature is present, as the CLI does; its README without the
  `namespaces` workaround.
  - Owns: new `grammars/xpath/full_text.js`, `grammars/xpath_full_text`,
    `src/full_text_parse.c`; `grammars/xquery/grammar.js`,
    `src/xquery_parse.c`, `src/xpath_parse.c`, `src/xpath_ast_internal.h`
    (the parser part), single hunks in `src/xpath_lower.c`,
    `src/full_text.c`, `src/full_text_internal.h`,
    `src/xquery_lower_full_text.c`, `tests/qt4/driver/applicability.c`;
    `bindings/javascript/src/index.ts`, its README and tests, the size
    budgets.
  - **Done** 2026-10-20 (D235): grammars/xpath_full_text, a parser of
    its own that only the feature links (the productions in
    grammars/xpath would have added 194,561 bytes to the core); the
    productions once in grammars/xpath/full_text.js, Full Text's parser
    tables once in src/full_text_parse.c (a parser part extension);
    score variables in XPath's for. `sxml.wasm` +1,215 bytes; the full
    module and the CLI +502 KB (budgets raised to 8,000,000 / 2,800,000
    and 9,500,000 / 2,100,000). XP40 32,655 and XQ40 45,914 passed,
    unchanged case by case; no ft-* case runs under XP40 (all XQ10+):
    through the XPath API the 48 written as XPath pass, the other 637
    are XQuery syntax. The package binds bin with expath-binary. Left:
    the comment of `sxml_full_text_feature()` in `xquery.h` (Proposed).

### 7.15 Full UCA collations (implementation, two packets)
- **Specifications**: F&O 4.0, 5.2.4 "The Unicode Collation Algorithm"
  (the URI parameters fallback, lang, version, strength, maxVariable,
  alternate, backwards, normalization, caseLevel, caseFirst, numeric,
  reorder; FOCH0002); UTS #10 (UCA, version 16.0, matching sxml's Unicode
  16.0.0 tables) with its DUCET; UTS #35, Part 5 (CLDR collation: the
  root collation, tailoring rule syntax, the case and numeric options).
- **Unlocks**:
  - QT4 advanced-uca-fallback 49 (fn-compare, fn-contains,
    fn-starts-with, fn-substring-after and others);
  - non_unicode_codepoint_collation 13 XQ40 / 5 XP40;
  - the 4 XQST0038 failures;
  - XSLT sorts with `lang`;
  - for the unit tests, Unicode's CollationTest files (owner question 4).
- **Replaces**: in `src/collation.c`, the UCA fallback and the FOCH0002 of
  fallback=no; the driver's advanced-uca-fallback.
- **7.15a DUCET and the algorithm**:
  - a collation element table (glossary `collation_element_table`)
    generated from `allkeys.txt` by `tools/generate-unicode-tables.py`,
    with implicit weights for Han and unassigned code points computed,
    not stored;
  - contractions and expansions, variable weighting, and the parameters
    strength, alternate, maxVariable, backwards, caseLevel, caseFirst,
    numeric and normalization;
  - the existing sort-key form (`fn:collation-key`) and substring
    matching by collation units (F&O 4.0, 5.2) over the new elements;
  - the fallback becomes the table's behaviour.
  - **Done** 2026-10-19 (D213): the table is read at first use from
    the DUCET the host serves (option 4; no generated tables, so
    `tools/generate-unicode-tables.py` is unchanged); QT4
    advanced-uca-fallback 49 of 49 and non_unicode_codepoint_collation
    13 of 13 (XQ40) and 5 of 5 (XP40), the XQST0038 cases pass;
    CollationTest samples (non-ignorable and shifted) pass in
    `tests/collation_uca_test.c`. The gate is met. Left for 7.15b:
    tailorings (lang, reorder) and the XSLT driver serving the DUCET.
- **7.15b Tailoring**: `lang` and `reorder` through CLDR rules compiled at
  first use into a tailoring (glossary `tailoring`). The rules are a
  built-in set for a few languages, or come through the resolver
  (option 4). Owner question 5 names the set.
  - **Done** 2026-10-19 (D220): option 4 over one resource kind
    (COLLATION_DATA with `urn:sxml:collation-data:` + a CLDR path, no
    COLLATION_TAILORING kind): the CLDR root (`allkeys_CLDR.txt`) and the
    LDML files of en, de, fr, it, es, nl and their regions (CLDR 47,
    `third_party/cldr`), compiled at first use by
    `src/collation_tailoring.c`; reorder over the groups of the generated
    `src/collation_tables.c` (`tools/generate-collation-tables.py`; the
    DUCET part of `tools/generate-unicode-tables.py` stays unused). The
    QT4 and XSLT drivers and the sxml command serve the data; xsl:sort
    lang works. No QT4 or XT40 case lost. Left: `[before n]`, special
    reset positions, prefixes and `[suppressContractions]` (root's
    search, eor and emoji types), the JavaScript package's data.
- **Header**: none (the collation URIs are already public). With option
  4, `processor.h` gets the resource kinds COLLATION_DATA and
  COLLATION_TAILORING.
- **After**: 7.2; 7.15b after 7.15a.
- **Owns**: `src/collation.c`, `src/collation_internal.h`, new
  `src/collation_uca.c`, `src/collation_tailoring.c` and
  `src/collation_tables.c` (generated), and
  `tools/generate-unicode-tables.py` (the DUCET part).
- **Size**: 170–280 KB with built-in tables; about 15 KB of code with
  option 4.
- **Gate**: ≥ 95 % of the 49 and ≥ 90 % of the others; CollationTest
  (non-ignorable and shifted) passes in a unit test.

### 7.16 A timezone database (implementation)
- **Specifications**:
  - F&O 4.0, fn:civil-timezone (places as IANA zone names, glossary
    `zone_name`; FODT0004);
  - fn:format-dateTime, fn:format-date and fn:format-time, the $place
    argument (an IANA name or an ISO 3166 country code: the
    timezone names of [ZN] and the offset used);
  - the IANA Time Zone Database (tzdata; public domain);
  - RFC 8536 (TZif), if the data comes from the host.
- **Unlocks**: QT4 olson-timezone 8 (fn-format-dateTime) and the 6
  FODT0004 failures (fn-civil-timezone). It also lets users format dates
  for real places.
- **Replaces**: `src/function_library_temporal.c`'s FODT0004 for every
  place other than UTC, and the unused $place of
  `src/function_library_format_date.c` (D97's notes there).
- **Mechanism**: a timezone database (glossary `timezone_database`)
  answers, for a zone name and an instant, the offset and abbreviation in
  effect (transitions, then the POSIX TZ rule of the TZif footer for later
  instants). Country codes map to zones through `zone1970.tab`. The data
  is either a compact table generated by a new
  `tools/generate-timezone-table.py` from tzdata, or TZif files from the
  host (option 4: the CLI reads `/usr/share/zoneinfo`).
- **Header**: none, or `processor.h` additive (the resource kind
  TIMEZONE_DATA) with option 4.
- **After**: 7.2.
- **Owns**: new `src/timezone.c`, `src/timezone_internal.h` and
  `src/timezone_table.c` (generated); the place hunks of
  `src/function_library_temporal.c`, `src/function_library_format_date.c`
  and `src/format_date.c`.
- **Size**: 60–120 KB built in; about 5 KB with option 4.
- **Gate**: all 14 cases.
**Done** 2026-10-19 (D209), gate met: option 4 (TZif from the host;
tzdata 2026e vendored in `third_party/tzdata` for the drivers and tests,
no generated table). fn-civil-timezone 9 of 9, the 8 olson-timezone cases
of fn-format-dateTime pass (XQ40 93 of 94 run). `sxml-full.wasm` +10.7 KB,
`sxml.wasm` +1.9 KB (the D162 places). Not done: a default place in the
dynamic context.

### 7.17 Calendars and languages in the formatting functions (implementation)
- **Specifications**:
  - F&O 4.0, 9.9.4.8 (the calendars: AD, ISO, AH, AME, AM, AP, AS, BE,
    CB, CE, CL, CS, EE, FE, JE, KE, KY, ME, MS, NS, OS, RS, SE, SH, SS,
    TE, VE, VS) and the language argument of the date formatting
    functions (month, day and era names);
  - F&O 4.0, fn:format-integer (words 'w', ordinals 'o', the language);
  - XSLT 4.0, xsl:number (lang, ordinal);
  - CLDR (Unicode License v3) for names, ordinals and spell-out rules.
- **Unlocks**:
  - QT4: language de 16, it 2, fr 1, default-language fr-CA 1,
    fn-format-integer-CLDR 4;
  - XT40: languages_for_numbering de 6, it 1 and fr 1, ICU_spellout 3,
    ordinal_scheme_name 3;
  - other cases in fn-format-date (13 not run) and fn-format-integer
    (15 not run).
- **Replaces**: in `src/format_date.c` and `src/function_library_format_date.c`,
  the fallback markers `[Language: en]` and `[Calendar: AD]` (D97); and
  the English-only words of `src/format.c`.
- **Mechanism**:
  - a language table (glossary `language_table`) per language: month
    and day names and abbreviations, eras, ordinal suffixes and
    spell-out rules, as data the existing engines read. The proposed set
    is en, de, fr, it, es and nl (owner question 5);
  - calendar conversion as one step before formatting. The arithmetic
    calendars are BE, JE (eras over Gregorian), OS (Julian) and AH
    (tabular Islamic). The others keep the fallback marker, which F&O
    allows.
- **Header**: none.
- **After**: 7.16 (shared `src/format_date.c`) and 7.15b (shared CLDR
  data and tooling).
- **Owns**: new `src/language_tables.c` (generated by a new
  `tools/generate-language-tables.py`) and `src/calendar.c`; the language
  hunks of `src/format.c` and `src/format_date.c`.
- **Size**: 5–15 KB plus 3–6 KB per language.
- **Gate**: ≥ 75 % of the 38 named cases, and no format-date or
  format-integer case lost.
**Done** 2026-10-19 (D221), gate met: the 38 named cases all run and
pass (QT4 language de 16, it 2, fr 1, default-language fr-CA 1, of them
fn-format-integer-CLDR 4; XT40 languages_for_numbering de 6, it 1, fr 1,
ICU_spellout 3, ordinal_scheme_name 3); fn-format-date XQ40 137 of 138
(not run 13 to 1), fn-format-integer 94 (not run 15 to 8: numbering
sequences), none lost. CLDR files from the host (D220's way, no generated
tables): the optional feature localization, `src/cldr.c` shared with the
collation tailorings, `src/language_table.c`, `src/spell_out.c` (RBNF),
`src/calendar.c` (BE, JE, OS, AH). `sxml.wasm` +4,920 bytes,
`sxml-full.wasm` +32,103, the CLI +366,194 (embedded names).

### 7.18 Invisible XML (implementation)
- **Specifications**: Invisible XML Specification 1.0 (invisiblexml.org,
  2022: the ixml grammar, marks and insertions, ambiguity reported as
  `ixml:state="ambiguous"`, failures as `ixml:state="failed"`); F&O 4.0,
  fn:invisible-xml (the grammar argument, options fail-on-error; the
  result function; FOIX0001, FOIX0002).
- **Unlocks**: QT4 fn-invisible-xml 10 (of 16); and the iXML community
  test suite, if vendored (owner question 4: several hundred grammar and
  parse tests).
- **Replaces**: the function is absent (XPST0017), and
  supports-invisible-xml is false.
- **Mechanism**:
  - one Earley parser (glossary `parse_forest`) over a compiled iXML
    grammar (glossary `ixml_grammar`), with explicit item sets in
    allocator memory;
  - the iXML grammar of iXML itself is compiled in as constant tables,
    so the same engine reads grammars and inputs;
  - the first parse tree is serialized as builder events with marks
    applied.

  Grammars are run-time data, so D12 gives way to D53's reasoning,
  recorded in the packet's D-record.
- **Header**: none.
- **After**: 7.2.
- **Owns**: new `src/ixml.c`, `src/ixml_internal.h`, `src/ixml_earley.c`
  and `src/function_library_ixml.c`; the suite under
  `third_party/ixml-tests` if the owner agrees.
- **Size**: 40–70 KB.
- **Gate**: all 16 QT4 cases; ≥ 90 % of the suite's correct-grammar tests.
**Done** 2026-10-19 (D215), gate met: QT4 fn-invisible-xml 16 of 16
(XP40, XQ40); the iXML community suite (vendored, `third_party/ixml-tests`)
886 of 907, test cases with a correct grammar 711 of 715 (99.4 %), grammar
tests 142 of 142. `sxml-full.wasm` +39,704 bytes (stripped); `sxml.wasm`
unchanged but for 4 bytes.

### 7.19 Phase 7 review (review)
After every packet the owner accepted. Owns the gate boxes. A separate
agent (D11) runs the rubric and re-checks each "done when" and the size
budgets. Open items go under "Proposed".
**Done** 2026-10-20 (D237): the gate below, with whole release runs of
the four suites; three gate boxes ticked by agent review, one open for
7.11's XT40 cases, the fuzzing box deferred to the owner's machine
(D181). Fixed: an undefined pointer offset found by fuzzing
(`src/xml_input.c`), the iXML driver's four error outcomes, EXPath
File's four-parameter functions and its clang-tidy findings, Full Text's
copy of the XML buffer, two stale record citations; added the fuzz
targets `ixml_fuzz` and `full_text_fuzz`.

### 7.20 Sweep: 7.11's open cases and the larger duplicates (implementation)
- **Cases**: XT40 transform-004 (a static fn:transform, FOXT0004),
  transform-401 (a `~record` pattern), transform-009 (an assertion the
  XT40 driver did not check). Close 7.11's gate, or record exactly why a
  case cannot pass under the project's draft.
- **Duplicates** (D237, D182): the three UTF-8 and UTF-16 decoders; Full
  Text reading through the resolver itself; the private growable arrays;
  the schema reader's boolean and integer parsers; Gregorian day
  arithmetic; compile-report text; the text form writers; the
  serialization heapsorts.
- **Size**: the core stays under budget (the D225 guard).
**Done** 2026-10-20 (D249): XT40 transform 10 of 12. transform-009
passes, and so do five result-document cases: the driver checks
assert-serialization inside assert-result-document on the serialized
result document. transform-004 cannot pass while D71 holds: compiling a
static fn:transform re-enters the compiler. transform-401 needs the
newest draft's "~T" patterns: D245 targets that draft, and packet 4.22
brings them. Merged: one decoder with an error rule, Full Text through
`sxml_optional_feature_read_data`, `sxml_allocator_reserve_array`, the
cast engine's boolean and digits, cycle days in `src/calendar.c`, compile
reports built in one place, `src/text_form.c`, and `sxml_sort_in_place`
for the serialization sorts. XT40 11,945 to 11,951 passed; XP40, XQ40
and XSD unchanged; `sxml.wasm` 2,089,167 (guard +345).

## Bindings

### js-1 JavaScript bindings for XQuery, XSLT and XSD validation (implementation)
After 7.2 (D189), D207 and D208. The package covered XPath only.
- **Specifications**: XQuery 4.0 (5 Modules and Prologs, 5.16, 5.22),
  the XQuery Update Facility 3.0 (2.2, 3.2.2, 6.1), XSLT 4.0 (2.4, 23,
  26), XSD 1.1 Part 1 (4.2, 5.2, appendix C), through the stable
  `xquery.h`, `xslt.h` and `schema.h`.
- **Mechanism**: D189's two links: shim files linked into
  `sxml-full.wasm` only; the package finds them by their exports. No
  header changes.
- **Owns**: `bindings/javascript/` (the new `sxml_javascript_full.c`,
  `_xquery.c`, `_xslt.c`, `_schema.c`, `_internal.h`, `test/full.test.ts`;
  `src/index.ts`, `sxml_javascript.c/h`, `CMakeLists.txt`, `README.md`),
  the JavaScript section of the top-level `README.md`.
- **Done when**: compile once, run many times for the three languages,
  with results as values or serialized text and errors with codes and
  locations; XQuery's external variables, output declarations and update
  results through the update sink; XSLT's parameters, initial mode,
  template or function, principal and secondary results; XSD reports;
  TypeScript types, Node 22 tests on the full module, README examples
  with real output; `sxml.wasm` unchanged.
**Done** 2026-10-19 (D216): `Processor.compileQuery`,
`compileStylesheet`, `compileSchema`; 24 tests (`javascript/languages`,
`javascript/languages_full`); `sxml.wasm` 2,134,058 bytes, unchanged;
`sxml-full.wasm` 3,666,545 to 6,570,723 (budget 7,000,000 / 2,450,000).

### cli-2 The sxml command's gaps: written files, csv, option hints (implementation)
After D44, D200, D205. The command wrote nothing but EXPath File output.
- **Specifications**: XSLT 4.0 (2.4.6 base output URI, 26.2 result
  documents), the XQuery Update Facility 3.0 (6.1 fn:put),
  Serialization 4.0 (CSV output method); the rules of `docs/CLI.md`
  (D44: one intent, one action; files only where the command line names
  them; `--force` to replace).
- **Mechanism**: one file writer interface for the commands
  (`sxml_cli_file_writer`, beside the file reader), temporary file and
  move on commit; the query run commits or abandons all its files at
  the end. No library header changes.
- **Owns**: `apps/` (new `apps/sxml/platform_file_writer.c`),
  `tests/cli/`, `apps/sxml/README.md`, `docs/CLI.md`.
- **Done when**: `--output` and `--force` on xpath, xquery, transform
  and serialize; result documents with an href under an explicit
  option; fn:put under an explicit flag; `--method=csv`; hints for a
  command's misspelled options; CLI tests with goldens, help and README
  with real output.
**Done** 2026-10-19 (D217): `--output=FILE`/`-o`, `--force`,
`sxml transform --result-directory=DIR`, `sxml xquery --allow-put`,
`--method=csv`, other commands' options named; CLI tests 161 to 203.

## Performance

### perf-1 Evaluator performance (implementation)
After D182 (the evaluator runs about 4 million steps a second and has
no index for joins), D178 (QT4 app-Demos itunes is quadratic by its own
text, 72 s) and D185 (`match="$var"` membership is a linear scan).
- **Mechanism**: general mechanisms of the one IR and evaluator, no
  language-specific machinery: a benchmark set (`tests/bench/`), axis
  steps that describe each node once, and filter indexes: an IR pass
  (the optimizer seam, D46) gives a FILTER whose predicate needs
  "key = probe", "eq" or "is" a filter index, a for clause's where
  becomes such a FILTER, and the evaluator keeps the index in the
  evaluation memo (D149).
- **Owns**: `tests/bench/`, `src/tree_axis.c`, `src/evaluator_path.c`,
  `src/evaluator_index.c`, `src/ir_loop_passes.c`; the FILTER parts of
  `src/evaluator.c`, `src/ir.c`, `src/ir_verify.c`, `src/ir_internal.h`.
- **Done when**: the baseline and the numbers after each change in a
  D-record and the commit messages; no QT4 XQ40 or XT40 outcome
  changes; D71, the memory limits and honest step counts kept.
**Done** 2026-10-19 (D224): `tests/bench/`; axis walks; filter indexes
(the filter-index pass, `src/evaluator_index.c`); a path's step from all
origins at once; the hoist pass. xmark-all 6.73 to 1.52 processor
seconds, app-Demos itunes 80.6 to 3.4; QT4 XQ40 and XT40 unchanged case
by case.

### perf-2 Evaluator performance, second round (implementation)
After D224 (perf-1: its benchmarks, and what it left: describe_node 15 %
and the machine loop 12 % of XMark-All, atomization, reference counts and
small sequences, numeric index keys, hoisting of path MAPs, quantified
expressions and sorts, the `$V[. instance of gnode()]` check per
`match="$var"` test).
- **Mechanism**: profile first (callgrind), then general mechanisms of
  the one IR and evaluator for the hot spots; D223's rule (no IR pass
  moves STREAM bindings), D71 and honest step counts kept.
- **Owns**: `src/evaluator.c`, `src/evaluator_function.c`,
  `src/evaluator_path.c`, `src/evaluator_index.c`, `src/ir_loop_passes.c`,
  `src/tree_axis.c`, `src/arena_tree.c`, `src/tree.c`, `src/cast*.c`,
  `src/decimal.c`, `src/xdm*.c`, `tests/bench/`.
- **Done when**: each change measured before and after, the numbers in
  its commit and in a D-record; no QT4 XQ40 or XT40 outcome changes.
**Done** 2026-10-19 (D229): Clinger's fast path for numbers to doubles;
leaves in place; inline item lists and matcher frames; single-node
atomization; the memoized match="$var" type check; numeric index keys;
quantified hoisting. xmark-all 12.29 to 9.84 x 10^9 instructions (1.55
to 1.20 s), match="$var" 9.30 to 3.91; QT4 XQ40 and XT40 unchanged case
by case.

### xml-mem Bounded memory for XML parsing (design and implementation)
After D12 (tree-sitter parses XML, the owner's choice), D199 (DTD and
entities) and D219 (parsing 106 MB took 3.98 GB resident, nearly all
tree-sitter's syntax tree of the whole input).
- **Specifications**: XML 1.0 (Fifth Edition) [1], [14], [22], [27],
  [39] to [44], 2.11, 4.3.3; 3 WFC: Element Type Match.
- **Mechanism**: keep tree-sitter and its grammar; parse a document in
  bounded segments cut at markup boundaries, walk each segment's tree
  and free it before the next. No header changes.
- **Owns**: `src/xml.c`, `src/xml_segment.c`, `src/xml_internal.h`,
  `grammars/xml/`, `tests/xml_test.c`.
- **Done when**: memory proportional to the depth and the largest item,
  not the input; error positions, DTD and entities as D199; the
  incremental parse of D219 pauses and resumes; a unit test parses a
  large generated document under a small allocator limit; resident
  memory and speed before and after at 10 MB and 100 MB.
**Done** 2026-10-19 (D222): the head and content segments of 16 KB;
100 MB parses in 98 MB resident (3,789 MB before; the input is 97 MB),
CPU 25.0 s to 19.7 s; xmlconf, QT4 and XT40 unchanged case by case.

### perf-3 XML parsing speed (implementation)
After D12 (tree-sitter parses XML, the owner's choice), D222 (segmented
parsing) and D224, D229 (the benchmarks; parsing a quarter of
XMark-All).
- **Mechanism**: profile parsing alone (callgrind, 10 MB and 100 MB);
  fewer tokens and nodes in the grammar and its scanner, a cheaper walk;
  tree-sitter, the segments and the incremental events kept.
- **Owns**: `grammars/xml/`, `src/xml.c`, `src/xml_segment.c`,
  `src/xml_encoding.c`; the validation loop of `src/string.c`.
- **Done when**: MB/s before and after for parse-only and the tree, and
  XMark-All, in a D-record; xmlconf, QT4 and XT40 unchanged case by case.
**Done** 2026-10-19 (D232): tags and attributes without references as
single tokens and childless nodes, content items without rebalancing,
fields by number, node types by symbol. Parse-only 14.9 to 25.2 MB/s,
the tree 13.4 to 21.3 MB/s (6.71 to 3.90 x 10^9 instructions at 10 MB);
XMark-All 9.87 to 8.88; QT4 and XT40 unchanged case by case.

### perf-4 Laziness, joins and the step limit (implementation)
After D224, D229, D232 and D257 (writing the book's performance chapter
found three things): `head(filter(1 to 1e9, ...))` and `exists((1 to
1e9)[. = 999])` read the whole range; the chapter's join of 2,000
orders takes 12 s although D224's filter index exists for it; the
default step limit takes about 75 s to trigger.
- **Mechanism**: one general mechanism for stopping early (head,
  exists, empty, `[1]`, some/every, boolean()), on the one IR and
  evaluator, with D71, honest step counts and the errors XPath 4.0,
  2.4.3 lets be skipped; the filter index extended in general to the
  join's shape; the step rate measured and the default limit decided
  without touching the drivers' budgets (D166, D178).
- **Owns**: `src/evaluator.c`, `src/evaluator_index.c`,
  `src/ir_loop_passes.c`, `src/ir.c` (the pass list, the text form),
  `src/ir_internal.h` and `src/ir_pass_internal.h` (their LET and pass
  parts), the traversal of `src/function_library_higher_order.c`,
  `tests/bench/`, `tests/ir_loop_passes_test.c`,
  `docs/book/src/performance.md` and its examples.
- **Done when**: each change measured before and after, in a D-record
  and the commit messages; no QT4 XQ40 or XT40 outcome changes, case by
  case; the book's chapter true (D257).
**Done** 2026-10-20 (D259): descendant paths, the number view and
non-constant probes first (the book's join 12.9 to 0.045 s); item
demands and quantified fusion (`exists`, `head(filter(...))`, `[1]`,
some/every over 10^7 items 2 to 6 s to 0.004 s); the default step bound
10^8 (8 s instead of 75 s; the drivers' bounds unchanged); xmark-all +1 %
instructions; QT4 XQ40 and XT40 unchanged case by case.

### perf-5 Slices without copies (implementation)
After D34 (sequence representations), D143 (reverse of a range in O(1))
and D259 (item demands). Running SchXslt 1.10.1 on sxml to compile the
EN 16931 Schematron rules (2026-10-28) found that recursing over the
rest of a sequence is quadratic: `make_span` in
`src/function_library_sequence.c` copies every item after the start into
a new vector unless the input is an integer range, so `fn:tail`,
`fn:subsequence` and `$s[position() > 1]` cost the length of the rest.
Measured (release, x86_64 Xeon 2.1 GHz): one recursive walk over 780
elements takes 106 ms with `$items[position() > 1]`, 28 ms with
`tail($items)`, 32 ms with `subsequence($items, 2)`. SchXslt's
`schxslt:replace-params` walks 780 parameters this way for each of 1,042
attributes: about 110 s of its expand stage (8 min 56 s in all, 1 min
39 s once its own per-level re-sort is removed; about 9 s of that is
`fn:replace`). The compiled rules themselves pass all 1,133 checks of
the EN 16931 UBL unit tests.
- **Mechanism**: general mechanisms of the one evaluator, no
  Schematron-specific machinery: a slice of a stored sequence (a vector,
  or another slice) shares the stored items instead of copying them, so
  `fn:tail`, `fn:subsequence`, `fn:slice`, `fn:head` and `fn:trunk` of
  one are O(1); `$s[position() > N]`, `$s[position() >= N]` and
  `$s[position() = N to M]` with N and M fixed for the filter take the
  same path (an IR pass, or the evaluator's FILTER fast path). Prefer a
  representation internal to `src/xdm_sequence.c` (a slice cell
  referring to its base cell); a new public
  `sxml_sequence_representation` is an additive change to a frozen
  header (D99) and needs its own decision record first. Reference
  counts keep the base alive while a slice exists; a small slice of a
  large base may be copied when the base would otherwise outlive it
  (state the rule and its threshold in the D-record). "Slice" already
  names QT4 test lists in `docs/GLOSSARY.md` (`xquery_slice`); add the
  new term there first under a name of its own (for example
  `sequence_slice`).
- **Owns**: `src/xdm_sequence.c`, `src/function_library_sequence.c`,
  the FILTER parts of `src/evaluator.c` and `src/ir_loop_passes.c`,
  `tests/bench/` (a new `recursion-tail.xq` with the three forms),
  `tests/xdm_sequence_test.c`.
- **Done when**: the three forms walk 780 items in time linear in the
  length (before and after in the D-record and the commit messages);
  SchXslt's expand stage of EN 16931, unpatched, measured before and
  after; no QT4 XP40, XQ40 or XT40 outcome changes, case by case; D71,
  the memory limits and honest step counts kept (a slice step counts
  one).
**Done** 2026-10-28 (D309): sequence slices (a cell over a vector,
reported as a vector; spans of at most 16 items or under an eighth of
the base copied), positional filters in the evaluator's FILTER, and a
matched-base memo for typed parameters; 100 walks over 780 elements
10.23, 2.55, 3.03 s to 0.11 s each, linear; SchXslt's expand stage 552
to 458 s; QT4 XP40, XQ40 and XT40 whole: 0 outcomes differ.

### perf-6 Compile each regex once (implementation)
After D53, D105 and D114 (the regex engine and its library part) and
the SchXslt measurements of perf-5. `compile_pattern` in
`src/function_library_regex.c` parses and compiles the pattern and
creates a matcher on every call of `fn:matches`, `fn:replace`,
`fn:tokenize` and `fn:analyze-string`, also when the pattern and flags
are literals. Measured (release, x86_64 Xeon 2.1 GHz): 20,000 calls of
`replace($s, "(\W*)\$AMOUNT(\W*)", "$1x$2")` take 0.156 s, against
0.054 s for the literal pattern `"\$AMOUNT"`; SchXslt's expand stage of
EN 16931 makes about 810,000 `fn:replace` calls with about 1,050
distinct patterns (about 9 s).
- **Mechanism**: one general mechanism, no Schematron-specific
  machinery: a compiled regex (with its parsed flags) is reused for
  every call with the same pattern text and flags. Prefer a bounded
  cache owned by the evaluation (or the processor, if its lifetime and
  limits fit better), keyed by pattern and flags, its memory from the
  allocator and counted against the limits; state the bound and the
  eviction rule in the D-record. Where the pattern and flags are literals
  the lowering may compile once instead; one mechanism, not two, if the
  cache already gives the speed. Errors (FORX0001, FORX0002) and the
  regex step limit behave as before, per call. New terms go into the
  glossary first.
- **Owns**: `src/function_library_regex.c`, `src/regex.c`,
  `src/regex_internal.h`, `tests/function_library_regex_test.c` (or the
  regex tests that exist), `tests/bench/` (a new `regex-reuse.xq`). Not
  `src/evaluator.c`, which perf-5 holds: if the cache needs a field
  there, write the need under "## Proposed" and use what the dynamic
  context already offers.
- **Done when**: the benchmark before and after in the D-record and the
  commit messages; SchXslt's expand stage of EN 16931 measured; no QT4
  XP40, XQ40 or XT40 outcome changes, case by case; the regex suites
  unchanged; D71, the memory limits and honest step counts kept.
**Done** 2026-10-28 (D308): a regex cache per evaluation in the
evaluation memo (4,096 entries, 8 MiB, least recently used unused entry
evicted); 20,000 replace calls 0.103 to 0.065 s, bench-real.xsl 0.550 to
0.488 s, SchXslt's expand stage 555 to 551 s (compiling under 1 %); QT4
and XT40 regex sets unchanged case by case.

### perf-7 Faster regex matching (implementation)
After D53, D105 (the backtracking matcher) and D308 (perf-6: compiling
is now under 5 % of a regex-heavy workload; matching is the rest).
Measured (release, x86_64 Xeon 2.1 GHz): 200,000 calls of
`matches("abc_def ghi " || ., "\w+\s\w+\s\d+")` take 0.80 s, and 0.71 s
with explicit ranges `[a-z_]+ [a-z]+ [0-9]+`, against 0.12 s for the same
loop with `contains`: about 3 microseconds a match of a 15-character
string, whatever the character classes.
- **Mechanism**: profile first (callgrind on `tests/bench/regex-reuse.xq`
  and a new matching benchmark), then general mechanisms of the one regex
  engine for the hot spots. Candidates to confirm or reject with numbers:
  the matcher created and released on every call (D308 kept matchers per
  call; a matcher could live with its cache entry while no call uses
  it); the setup and the backtrack stack of each match; a literal prefix
  or a required first character to skip start positions; ASCII fast
  paths for class tests in `src/regex_unicode.c` (generated: change
  `tools/generate-regex-tables.py` and regenerate, never edit it).
  Errors, the regex step limit and step counts stay as before.
- **Owns**: `src/regex.c`, `src/regex_internal.h`, `src/regex_unicode.c`,
  `tools/generate-regex-tables.py`, `src/function_library_regex.c`,
  `tests/regex_test.c`, `tests/bench/regex-*.xq`.
- **Done when**: each change measured before and after (instructions and
  seconds) in a D-record and the commit messages; the QT4 XP40 and XQ40
  regex sets and the XT40 regex sets unchanged case by case; D71, the
  memory limits and honest step counts kept.
**Done** 2026-10-28 (D310): ASCII member sets and an ASCII category
table, a start filter, idle matchers in the regex cache, character
repetitions; 200,000 `matches` 0.382 to 0.197 s (366.9 to 200.3 x 10^6
instructions at 20,000), regex-match.xq 1.069 to 0.652 s, regex-reuse.xq
0.250 to 0.146 s; QT4 and XT40 regex sets unchanged case by case.

### perf-8 Cheap coercion of function arguments (implementation)
After D85 (coercion), D281 (XSLT coercion casts), D309 and D310. A
callgrind profile of SchXslt's expand stage of EN 16931 (with
SchXslt's sort-once fix, the step bound raised in a scratch build;
75.3 x 10^9 instructions, 10.5 s) puts 33 % in `sxml_evaluator_coerce`,
called 8.1 million times from `sxml_make_leaf_value`: about 3,000
instructions a coercion. Each one creates and finishes a sequence
builder (4.1 million), atomizes (2.7 million), casts (1.4 million,
mostly xs:untypedAtomic attribute values to xs:string) and matches the
sequence type (3.4 million), even when the argument is one item that
already has the required type.
- **Mechanism**: profile first, then general fast paths in the one
  coercion (XPath 4.0, 3.4.3 "Coercion Rules"; XSLT 4.0 and XQuery 4.0
  use the same): an argument that already matches the required type
  passes through untouched, without a builder; a single atomic item
  needs no builder either; the result of a type match is not computed
  twice. No change of outcome, error code or step count beyond what the
  D-record states and justifies.
- **Owns**: `src/evaluator_function.c`, `src/evaluator.c` (its
  `sxml_make_leaf_value` and the call paths into coercion),
  `src/function_library_type.c`, `tests/bench/` (a new
  `coercion.xq`), `tests/evaluator_test.c` for new cases.
- **Done when**: each change measured before and after (instructions
  and seconds) in a D-record and the commit messages, on the new
  benchmark and on xmark-all; no QT4 XP40, XQ40 or XT40 outcome changes,
  case by case, over the whole suites (coercion is everywhere); D71, the
  memory limits and honest step counts kept.
**Done** 2026-10-28 (D316): single-item shortcuts in the one matcher
and the one coercion: no item or one item matched by the item rules
without frames, one item coerced to an atomic type through the atomic
coercion rules once, without the engine, its builders or a second
match; the frames out of the structs. SchXslt's expand stage 75.27 to
61.68 x 10^9 instructions (coercion 24.88 to 11.21), coercion.xq (new)
3.775 to 2.847, xmark-all 8.737 to 8.735; QT4 XP40, XQ40 and XT40
whole: 0 outcomes differ, case by case.

### perf-9 Required literals in regex search (implementation)
After D310 (perf-7). The same profile puts 12.9 % in
`sxml_regex_matcher_scan` under `fn:replace`. SchXslt's patterns have
the shape `(\W*)\$name(\W*)`: they can match the empty string before a
`$`, so perf-7's start filter skips nothing and every position is
attempted, though no match exists unless the literal `$name` occurs.
- **Mechanism**: at compile time, find a literal that every match must
  contain (a required literal, as other engines keep a "required
  character" or "must-have string"); at search time, find its next
  occurrence first and attempt only start positions from which a match
  could reach it, and give up at once when it does not occur. One
  mechanism for `matches`, `replace`, `tokenize`, `analyze-string` and
  `fn:regex`'s functions; errors, step limits and step counts as D310
  settled (a skipped position counts one step).
- **Owns**: `src/regex.c`, `src/regex_internal.h`,
  `src/function_library_regex.c`, `tests/regex_test.c`,
  `tests/bench/regex-*.xq`.
- **Done when**: each change measured before and after (instructions
  and seconds) in a D-record and the commit messages, with SchXslt's
  expand stage; the QT4 XP40 and XQ40 regex sets and the XT40 regex
  sets unchanged case by case, and a random comparison of
  `fn:analyze-string` before and after as perf-7 did.
**Done** 2026-10-28 (D311): a required literal per compiled regex
(the longest CODEPOINT run every way to MATCH passes through) and the
read set of what precedes it; the scan finds the literal first and
skips the positions that cannot reach it, all of them when it does not
occur. SchXslt's expand stage 75.3 to 65.8 x 10^9 instructions;
regex-literal.xq 0.458 to 0.186 s; regex sets and 18,000 random
regexes unchanged.

### perf-10 Casts that keep the string, and the coercions perf-8 left (implementation)
After D316 (perf-8). With perf-5 to perf-9, SchXslt's expand stage of
EN 16931 (sort-once fix, no step bound in the CLI since D314) takes
7.7–8.5 s; unpatched SchXslt takes 4 min 51 s (8 min 56 s this
morning). D316 left three costs: a cast of xs:untypedAtomic to
xs:string costs about 1,600 instructions, though the result is the
same string (XPath 4.0, 19.1 casting from xs:untypedAtomic; F&O 4.0,
19.1.1); about 0.7 million single items against unions, choices and
enumerations still run the coercion engine; about 0.7 million
multi-item values still run the full matcher, some twice when a slice
base fails D309's whole-base match and is matched again item by item.
- **Mechanism**: profile first (callgrind on SchXslt's expand stage
  and `tests/bench/coercion.xq`); a cast between string-like types
  whose lexical form does not change (xs:untypedAtomic, xs:string, and
  the derived types whose facets the value already satisfies) relabels
  the item instead of rebuilding it; then the next largest of D316's
  remaining costs, measured. Outcomes, error codes and step counts
  unchanged.
- **Owns**: `src/cast.c` and its internal header, `src/evaluator_function.c`
  (the matcher and the coercion engine), `tests/cast_test.c`,
  `tests/evaluator_test.c`, `tests/bench/`.
- **Done when**: each change measured before and after (instructions
  and seconds) in a D-record and the commit messages, on SchXslt's
  expand stage, coercion.xq and xmark-all; QT4 XP40, XQ40 and XT40 run
  whole with no outcome changes, case by case.
**Done** 2026-10-28 (D319): relabelled casts (a cast that keeps the
text is the source's cell under the target's annotation), one item
against a choice kept by its first matching alternative, untyped items
cast without a match. SchXslt's expand stage 52.19 to 47.78 x 10^9
instructions (coercion 11.14 to 6.63), coercion.xq 2.845 to 2.716,
xmark-all 8.735 to 8.746; QT4 XP40, XQ40 and XT40 whole: 0 outcomes
differ, case by case.

### fix-4 Choice coercion and a stale error description (implementation)
After D316 and D319, which found the first, and D310 (perf-7), which
found the second.
- **Choice coercion**: a function parameter of type
  `(element(a) | xs:string)` given `xs:untypedAtomic("v")` raises
  XPTY0004 (`sxml xpath 'let $f := function($x as (element(a) |
  xs:string)) { $x } return $f(xs:untypedAtomic("v"))'`). XPath 4.0,
  3.4.2 (coercion rules) tries the alternatives of a choice in order,
  so the untyped value is cast to xs:string. The coercion engine counts
  the unchanged item as the first alternative's success (D319).
- **Stale description**: after a `try`/`catch` catches FORX0002, a later
  LIMIT_EXCEEDED in the same query printed the caught error's
  description (D310): the error record is not cleared. Reproduce first
  (the sxml command has no step bound since D314; use a regex step
  bound, a memory limit, or the C API with a small step bound); if it
  cannot be reproduced, record why.
- **Owns**: `src/evaluator_function.c`, `tests/evaluator_test.c`, and the
  file that clears the error record after a caught error (find it;
  not `src/xslt*.c` or `src/schematron*.c`, which 7.28 holds).
- **Done when**: both fixed with tests, each citing its section; QT4
  XP40, XQ40 and XT40 whole: no outcome lost case by case (gains
  listed); the SchXslt expand stage of EN 16931 unchanged in output and
  not slower.

**Done** 2026-10-28 (D321): a CHOICE attempt succeeds only when its
result matches the alternative; a catching task clears the error record
once it handles the error. QT4 XP40, XQ40, XT40 whole: 0 outcomes
differ; SchXslt expand unchanged in output and instructions.

### perf-11 Cheaper node atomization (implementation)
After D316 and D319. In SchXslt's expand stage of EN 16931 (47.78 x 10^9
instructions after perf-10), node atomization in coercion is the
largest coercion cost: 3.19 x 10^9 (6.7 %), in `src/xdm_node.c`.
- **Mechanism**: profile first (callgrind on the expand stage,
  `tests/bench/coercion.xq` and xmark-all); a general fast path for the
  common case (an attribute or a text-only element of an untyped tree:
  one xs:untypedAtomic from the string value, without building a
  sequence), and whatever else the profile shows in atomization (XPath
  4.0, 2.4.2 atomization; XDM 4.0, typed-value).
- **Owns**: `src/xdm_node.c`, its internal header, the tree's typed
  value and string value paths (`src/arena_tree.c` only for those),
  `tests/` for them, `tests/bench/`.
- **Done when**: before and after in a D-record and the commit messages
  (instructions and seconds) on the expand stage, coercion.xq and
  xmark-all; QT4 XP40, XQ40 and XT40 whole: no outcome changes, case by
  case.
**Done** 2026-10-28 (D322): a typed value describes its node once;
text walks step over attributes by count. SchXslt's expand stage
47.78 to 47.45 x 10^9 instructions, coercion.xq 2.716 to 2.695,
xmark-all 8.747 to 8.643; QT4 XP40, XQ40 and XT40 whole: 0 outcomes
differ, case by case. The atomic cell's costs (70 % of what is left)
are outside the packet's files: proposed in D322.

### perf-12 Compiling large stylesheets (implementation)
After D224, D259 and D322. Validating one EN 16931 invoice with the
official pre-compiled stylesheet (900 KB of XSLT, from
ConnectingEurope/eInvoicing-EN16931) costs 2.68 x 10^9 instructions,
about 0.5 s, and 79 % of it is compiling the stylesheet: reading 41 %
(tree-sitter's XML parse 23 %), XPath parsing 25 %, IR passes 32 %;
the validation itself 20 %. Two passes dominate on their own:
`sxml_ir_shorten_descendant_paths` 12.5 % (self) and
`sxml_ir_read_globals_lazily` 8.6 % (self), which suggests work that
grows with the square of the unit (each template or global revisits the
whole unit). Every `sxml transform` and, after 7.28, every Schematron
compile pays this.
- **Mechanism**: profile first (callgrind on the invoice run above and
  on a stylesheet of growing size made by repeating templates, to show
  the growth); make each pass linear (or n log n) in the unit's size
  with the same result; then the next largest compile cost the profile
  shows in the files below. The IR after the passes must be the same:
  compare the IR text form (`src/ir.c`'s printer) before and after on
  the EN 16931 stylesheet and the XT40 suite's stylesheets.
- **Owns**: `src/ir_loop_passes.c`, `src/ir_global_read.c`, `src/ir.c`
  (the pass list and helpers), `src/ir_internal.h` and
  `src/ir_pass_internal.h` (their pass parts), `src/xpath_parse.c`,
  `src/xpath_lower.c`, `tests/ir_loop_passes_test.c`, `tests/bench/`.
  Not `src/xslt*.c` (7.28).
- **Done when**: each change measured before and after (instructions and
  seconds) in a D-record and the commit messages, on the invoice run, a
  growth series showing linear passes, xmark-all and SchXslt's expand
  stage; the IR text form identical; QT4 XP40, XQ40 and XT40 whole,
  base and change built and run in the same setup: no outcome changes,
  case by case.
**Done** 2026-10-28 (D324): descendant and global-read passes and the
lowerer's declaration check linear; fusion skips functions without a
QUANTIFIED; AST kinds kept per grammar symbol. Invoice 2.680 to 2.076
x 10^9 instructions; growth N = 128 57.1 to 6.2; IR text identical;
XP40, XQ40, XT40 0 outcomes differ.

### perf-13 Compiling XSLT and Schematron: the component graph, value templates, the Schematron front end (implementation)
After D320, D323 and D324 (perf-12, whose Proposed entry names the first
two). With perf-12, validating one EN 16931 invoice with the
pre-compiled stylesheet takes 0.29–0.37 s (2.08 x 10^9 instructions);
validating it with `sxml validate --schematron` from the `.sch` sources
takes 0.59–0.73 s, the Schematron compile 0.53–0.59 s (D323).
- **Known costs**: XSLT's component graph grows with the square of the
  stylesheet (`add_named_edges` in `src/xslt_lower_control.c`,
  `sxml_xslt_package_bind`); value templates are parsed even when they
  contain no "{" (6.3 % of the invoice).
- **Mechanism**: profile first (callgrind on both invoice runs and on
  perf-12's growth series); make the component graph linear in the
  stylesheet with the same result; a value template without "{" or "}"
  is its text (XSLT 4.0, 5.6.1); then the largest costs the profile of
  the Schematron compile shows in the files below (reading the `.sch`,
  expanding abstract patterns, building the AST). The IR text form after
  the passes stays identical; compiled Schematron gives the same
  findings.
- **Owns**: `src/xslt_lower*.c`, `src/xslt_read.c`, `src/xslt_package*`
  and their internal headers, `src/schematron*`, `tests/xslt_*`,
  `tests/schematron_test.c`, `tests/bench/`.
- **Done when**: each change measured before and after (instructions and
  seconds) in a D-record and the commit messages, on both invoice runs,
  the growth series, xmark-all and the XT40 stylesheets' compile; IR text
  identical on EN 16931 and the XT40 units; Schematron: the 7.30 suite as
  in D323 and EN 16931 UBL 1,133 of 1,133 from the sources; QT4 XP40,
  XQ40 and XT40 whole, base and change in the same setup: no outcome
  changes, case by case.

### 7.21 Fast Invisible XML (book, tests, implementation)
After D215 (7.18: the Earley parser, right recursion quadratic, every
item a step). The owner: high performance; book first, then
conformance and benchmarks, then the implementation.
- **Owns**: `docs/book/src/ixml*.md`, `docs/book/examples/ixml/`,
  `tests/bench/run-ixml-bench.py`, `tests/bench/ixml/`, `src/ixml.c`,
  `src/ixml_earley.c`, `src/ixml_tables.c`, `src/ixml_internal.h`,
  `src/function_library_ixml.c`, `tests/ixml_test.c`.
- **Done when**: the book's performance chapter passes
  (`run-ixml-bench.py --check` and its examples); no iXML suite or QT4
  iXML case lost; D71, allocator memory, honest steps.
**Done** 2026-10-20 (D283): the part "Parsing Text with Invisible XML";
the benchmark suite; lookahead sets, Leo items, codepoint classes,
grammar simplification. 1 MB in 0.18 to 0.53 s (was 1.1 to 3.2 s),
right recursion linear; suite 886/907 and QT4 unchanged.

### 7.22 Source code as XML: tree-sitter trees with highlights (design and implementation)
- **Order** (owner): the book first, then the grammars' own tests, then
  the implementation.
- **Mechanism**: tree-sitter grammars vendored with their highlights
  queries and tests (third_party/tree-sitter-grammars, D13); one walk
  of a TSTreeCursor with an explicit stack (D71) into tree builder
  events; the highlights query through ts_query_* with the text
  predicates evaluated by sxml; the step limit through the progress
  callbacks.
- **API**: sxml:parse-code and sxml:code-languages in urn:sxml, an
  optional feature in every build (D189, D191, D272); no new CLI
  command (D44).
- **Owns**: src/source_code.c, src/source_code_internal.h,
  src/source_code_languages.c, src/function_library_source_code.c,
  tools/vendor-tree-sitter-grammar.sh,
  tools/generate-source-code-languages.py, tests/tree-sitter/,
  tests/source_code_test.c, docs/book/src/source-code.md.
- **Done when**: the chapter's examples match; the grammars' corpus
  and highlight tests run with a baseline; the size growth reported.
**Done** 2026-10-20 (D285): the chapter (18 examples match); seven
grammars (JSON, JavaScript, Python, C, CSS, HTML, Go); tree-sitter/suite
630 of 639 (corpus 452 of 452, highlights 178 of 187: locals 7,
injections 2); sxml.wasm +1,990,621 to 9,693,235 bytes, budgets raised.

### 7.23 More source code languages, a registration API, run-time languages (design and implementation)
- **Owner** (2026-10-20, answering D285's proposal): TypeScript, TSX and
  Markdown built in; a public API to register tree-sitter languages; new
  languages at run time from tree-sitter language files, loaded by the
  host. Order: the book, the tests, the implementation.
- **Owns**: 7.22's files, src/source_code_check.c,
  apps/sxml/code_languages.c/.h, apps/sxml/platform_library_loader.c,
  the library loader of apps/cli_framework, the source code part of
  include/sxml/processor.h.
- **Done when**: the chapter's examples match; the new grammars' tests
  run with a baseline; a test loads a language from a library it builds;
  the sizes reported and the budgets raised.
**Done** 2026-10-20 (D288): the chapter (32 examples match); eleven
languages; locals and injections; the registration API and
`--code-language`; tree-sitter/suite 1,410 of 1,438 (highlights 187 of
187); sxml.wasm 13,425,285 bytes, budgets 14,000,000 and the CLI
15,500,000 / 2,900,000. Delayed: .wasm language files in the JavaScript
package, the native wasm store, the host shim.

### 7.24 Invisible XML for large inputs (book, tests, implementation)
After D283 (7.21: about 3 to 6 MB/s and linear, but 80 to 200 bytes of
memory for each input character). The owner: usable on tens to hundreds
of MB; the book, the tests, then the implementation; one parser (D215).
- **Owns**: 7.21's files and `src/ixml_tokens.c`.
- **Done when**: the book's performance chapter states the targets and
  passes (`run-ixml-bench.py --check`, with the 100 MB log under
  `--large`, and its examples); regular nonterminals compiled once into
  automata, ambiguity kept; memory bounded; no iXML suite or QT4 iXML
  case lost; D71, allocator memory, honest steps, no header change.
**Done** 2026-10-20 (D290): regular tokens with path-counting automata,
sparse item sets over UTF-8 byte positions, a parse tree without hidden
nodes and with text runs. Log 1 MB 0.20 s / 90 MB to 0.06 s / 21 MB
(84 to 10 MB per further MB); 10 MB 2.0 s / 831 MB to 0.6 s / 124 MB;
100 MB 6.2 s / 1.17 GB within the default step limit; suite 886/907 and
QT4 fn-invisible-xml 16/16 unchanged.

### 7.25 tree-sitter WebAssembly language files in the JavaScript package (book, tests, implementation)
After D288's delayed (a). The owner (2026-10-20): an easy fix; the
book, the tests, then the implementation.
- **Owns**: `bindings/javascript` (the shim's code language exports,
  `src/linker.ts`, `src/index.ts`'s code languages, the test and the
  language modules' build), the host shim's `source_code_languages`.
- **Done when**: a section of the JavaScript chapter passes; a Node.js
  test loads grammars built from the vendored sources with the pinned
  WASI SDK (one with an external scanner), parses and checks highlights;
  the size test passes.
**Done** 2026-10-20 (D293): the module linker and loadCodeLanguage /
codeLanguages; javascript/code_languages 12 of 12 (JSON, Python, CSS;
refusals); the chapter 5 of 5; sxml.wasm 13,448,586 / 2,420,734 bytes,
+7,713 code and data, budget unchanged. Not supported: emscripten's
pre-2021 `dylink` section, needed side modules, scanner imports outside
tree-sitter's list.

### 7.26 tree-sitter functions in a tree-sitter namespace (design and implementation)
After D285, D288, D293 and D306. The owner (2026-10-22): expose
tree-sitter as `tree-sitter:`, not as `sxml:code-*`. Today
`sxml:parse-code` and `sxml:code-languages` live in `urn:sxml` (D285).
- **Design**: a well-known namespace for the tree-sitter functions,
  bound to the prefix `tree-sitter` as `bin` and `file` are (name.h,
  additive after D99, as D206 and D285 added theirs); the functions'
  names in it (proposed: `tree-sitter:parse` and
  `tree-sitter:languages`, same signatures and options); where the
  unknown-language error goes. `sxml:parse-code` and
  `sxml:code-languages` are removed, not kept as aliases. Recorded as a
  decision that amends D285's API paragraph; the glossary updated.
- **Owns**: 7.22's, 7.23's and 7.25's files, the namespace part of
  include/sxml/name.h and the comments of include/sxml that name the
  functions, the prefix bindings of apps/sxml, bindings/host and
  bindings/javascript, every test and book page that calls the functions
  (source-code.md, ixml-performance.md, the codegen chapter's check.xq,
  the examples under docs/book/examples).
- **Done when**: the CLI, XQuery prologs and stylesheets reach the
  functions through the `tree-sitter` prefix; tree-sitter/suite
  unchanged (1,410 of 1,438); the book's examples match; no
  `sxml:parse-code` or `sxml:code-languages` left in code, tests or
  docs (decision records excepted).

**Done** 2026-10-22 (D307): `tree-sitter:parse` and
`tree-sitter:languages` in `urn:sxml:tree-sitter` (name.h
SXML_NAMESPACE_TREE_SITTER = 17), errors `tree-sitter:unknown-language`
and `tree-sitter:invalid-query`; the sxml command and the JavaScript
package bind `tree-sitter`; tree-sitter/suite 1,410 of 1,438, unchanged;
the book's affected examples match. The old names remain only in
records (DECISIONS, LOG, QUEUE's history, the 0.2.0 CHANGELOG section,
the 7.22 and 7.26 entries here).

### 7.27 Schematron as a front end to the XSLT AST (design)
The owner accepted Schematron on 2026-10-28, as a front end beside XSLT
and XQuery, not by generating XSLT text. The owner settled the open
points in D313 (edition 2025, bindings, outputs, exit status, order
with XSD, SchXslt as a test reference only): the design follows them.
No phase covered it (PLAN.md
names XSD only): the packet's decision record brings it into Phase 7.
Evidence from 2026-10-28 (see D309, D310 and the queue):
- the official pre-compiled EN 16931 stylesheets pass all 1,133 checks
  of the EN 16931 UBL unit tests on sxml;
- SchXslt 1.10.1 running on sxml compiles the EN 16931 sources into a
  stylesheet that also passes 1,133 of 1,133, but its text pipeline
  costs 10.5 s for EN 16931 (8 min 56 s before perf-5 to perf-7 and
  without SchXslt's sort-once fix) and needs more than the default step
  bound: the cost is in generating XSLT text, not in Schematron;
- XRechnung's rules embed XSLT (`xsl:function` for `xr:checkIBAN`), as
  the `xslt2` and `xslt3` query bindings allow;
- findings today are SVRL with an XPath for a location, and errors in
  a rule point into the generated stylesheet, not the `.sch`.
- **Whom it serves, first**: the maintainer of a rule set (errors at
  `rules.sch:LINE:COLUMN`) and the person reading a finding (the
  document's line and column, with the locations proposal under
  "## Proposed" in DECISIONS.md); then embedders (C, JavaScript) who
  validate many documents without bundling a third-party stylesheet.
  Who pays: a reader and an expansion pass in C, one public header, a
  CLI option, the JavaScript API, the book; nothing per event of other
  languages.
- **Design** (decision record, glossary terms first, public header):
  - *The path*: the `.sch` document is parsed by the processor's XML
    parser (with locations kept); a Schematron reader builds the XSLT
    AST (`src/xslt_ast_internal.h`: `sxml_xslt_ast_add_node`,
    `_add_attribute`, `_append_child`), as `src/xslt_read.c` does for
    a stylesheet, with each node's source location in the `.sch`; the
    existing XSLT lowering makes IR. No IR kind, no evaluator change,
    no generated XSLT text. Lowering Schematron to IR directly is
    rejected: it would duplicate modes, patterns and priorities.
  - *The mapping*, each with its section of ISO/IEC 19757-3 (the
    packet names the edition it implements, after checking the
    current one): pattern to mode; rule to template rule matching its
    `context`, rule order to priority (the first matching rule of a
    pattern fires); `let` to a variable; `assert` and `report` to a
    condition that adds a finding; `value-of`, `name`, diagnostics,
    properties, `role` and `flag`; phases selecting modes; `ns`
    declarations; `include`, `extends` and abstract patterns expanded
    while reading, in one linear pass (each placeholder replaced once,
    longest name first, without a regex per parameter); `documents`.
    Foreign `xsl:function`, `xsl:key`, `xsl:include` and `xsl:import`
    children of `sch:schema` under the `xslt2` and `xslt3` bindings
    become the same AST nodes `src/xslt_read.c` makes; other bindings
    are rejected in one place with the error the standard gives, or
    mapped if the decision record shows they are the same language.
  - *Findings*: a public `sxml_schematron_report` modelled on
    `sxml_schema_report` (`include/sxml/schema.h`): the assert or
    report id, its role and flag, the message text, the rule's
    location in the `.sch` and the node's location in the document;
    delivered to a report sink like the XSD validator's. SVRL is one
    output form, made from the reports, not the engine's data.
  - *API* (`include/sxml/schematron.h`, new, additive; at most three
    parameters; the object lifecycle of CLAUDE.md): compile a
    Schematron schema once (with phase and parameters), validate many
    documents or nodes with it; typed (validated) input allowed.
  - *CLI* (`docs/CLI.md`, D44: no new command): `sxml validate
    --schematron=FILE [--phase=NAME]`, combinable with `--schema`; text
    and `--format=json` reports as for XSD, the same exit statuses;
    `--format=svrl` if the decision record keeps it.
  - *JavaScript*: the same compile-once, validate-many shape in the
    package.
  - *Out of scope*: streaming validation, Schematron Quick Fix, any
    output other than reports and SVRL.
- **Owns**: `include/sxml/schematron.h` (new), the decision record,
  `docs/GLOSSARY.md` terms, and the implementation packet 7.28's text
  (its files, gates and order). The CLI design is written in the
  decision record: `docs/CLI.md` is cli-4's while 7.27 runs, and 7.28
  updates its validate section.
- **Done when**: the owner has reviewed the header and the decision
  record; 7.28 is written with its file list and gates.

**Designed** 2026-10-28 (D315), for the owner's review:
`include/sxml/schematron.h` (compiles with the project's flags under
clang and gcc, implemented by nothing yet), D315, the glossary's
Schematron terms, 7.28 below. Not ticked: the owner's review is the
gate.

**Revised** 2026-10-28 after the owner's review (D315, points A to D;
D313 point 2 amended: no queryBinding, `xslt` and `xpath` are read as
XSLT 4.0 and XPath 4.0) and against the text of ISO/IEC 19757-3:2025:
every construct of the 2025 edition designed with its clause (group,
library, rules, extends at the top, schema and abstract-pattern params,
let/@as, visit-each, severity, dynamic flag/role/severity, phase
from/when and `#ANY`, schematronEdition); compile errors through a
report sink; exit status 1 for an unsupported binding. The header,
D315, the glossary and 7.28 below follow.

**Done** 2026-10-28 (D315, accepted by the owner): the owner answered
the revision's eight choices (D315, "The owner's answers": severity
decides when present; `xslt4`/`xpath4` as 4.0; rule contexts are XSLT
patterns under every binding; no text value templates; schema
parameters are compile-time text; rule lets once per context node;
XSLT's compiler gets a report sink in 7.28; any `schematronEdition`
accepted and reported). The header, the glossary and 7.28 follow.

### 7.28 Schematron as a front end to the XSLT AST (implementation)
After the owner's review of 7.27 (D315, D313), after 7.29 (the
locations a validation keeps; both touch `apps/sxml/`), after 7.30's
suite exists (`tests/schematron/`, gate 6) and cli-4 (which holds
`apps/sxml/` and `docs/CLI.md`). Implements
`include/sxml/schematron.h` as D315 (revised) designs it, against
ISO/IEC 19757-3:2025 (the owner's copy; cite clauses, copy no text); a
change to the header needs a record first.
- **Owns**:
  - new: `src/schematron_internal.h` (the index, the table of
    assertions and rules, the layout of a finding item),
    `src/schematron_read.c` (documents, `sch:include`, `sch:extends
    href` and `sch:library`, the index, the query binding: D315's one
    place for the bindings sxml does not read, every error to the
    compile report sink),
    `src/schematron_expand.c` (the placeholder scan over the instance's
    and the schema's parameters, defaults, expansion segments),
    `src/schematron_build.c` (the stylesheet AST: variables and their
    scopes, groups, visit-each, from, `#ANY`, dynamic attribute
    values), `src/schematron.c` (the public API: compile, describe,
    validate, reports), `src/schematron_svrl.c` (SVRL from reports and
    the static table, Annex D);
  - changed: `src/xslt.c` and `src/xslt_transform_internal.h` (compile
    a given AST: the part of `sxml_xslt_compile` after reading; a
    compile report sink through reading and static analysis, D315
    "Compile errors"), `include/sxml/xslt.h` (additive:
    `sxml_xslt_configuration.report_sink`, `sxml_schema_report_sink`
    reused; a record of its own), `src/xslt_read.c`'s and the static
    analysis' error paths that go on after an error,
    `apps/sxml/command_transform.c` and its tests (every compile error
    on its own line), the JavaScript `compileStylesheet` (`reports` on
    the error) and the book where it shows a stylesheet error,
    `src/xslt_read.c` and `src/xslt_read_internal.h` (read one foreign
    XSLT declaration into a given AST under a given parent, with extra
    namespace bindings);
  - tests: `tests/schematron_test.c` (reading, placeholders, each
    construct of the 2025 edition, error codes with their locations,
    several errors from one compile, the lifecycle under the counting
    allocator), small `.sch` fixtures in
    `tests/schematron/`, and a driver `tests/schematron/driver/`
    (`sxml-schematron-test`, not shipped, as the XSD driver: the VEFA
    unit tests of EN 16931 and the differential test);
  - vendored test data in `third_party/`, pinned in
    `tools/update-test-suites.sh` (D13), none of it shipped: EN 16931's
    validation artefacts (ConnectingEurope/eInvoicing-EN16931, EUPL
    1.2: `src/ubl/schematron`, `src/cii/schematron`, `src/test`),
    XRechnung's Schematron (itplr-kosit/xrechnung-schematron v2.6.0,
    Apache 2.0: `src/validation/schematron` and its test instances),
    SchXslt 1.10.1 (MIT, its `2.0/` directory; D313, item 6);
  - `apps/sxml/command_validate.c`, its tests in `tests/cli/`, and
    `docs/CLI.md`'s validate section (D315, "The CLI");
  - the JavaScript package's Schematron API
    (`bindings/javascript/src/index.ts`, `sxml_javascript_exports.c`,
    `sxml_javascript_full_exports.c`, its tests; D315, "JavaScript");
  - a book chapter (`docs/book/src/schematron.md` and SUMMARY.md), with
    real output;
  - glossary terms it adds, LOG, its record.
- **Order**, a commit after each step:
  1. `src/xslt.c`: compile a given AST (no change in behaviour).
  1b. The XSLT compile report sink: `xslt.h`, reading and static
     analysis go on after an error, `sxml transform` and JavaScript
     report them all. XT40 unchanged case by case (the null sink keeps
     today's first-error behaviour).
  2. `src/xslt_read.c`: read one foreign declaration into a given AST.
     XT40 unchanged case by case after steps 1 and 2.
  3. Read, inclusions (include, extends href, library), index and
     query binding (case ignored; none, xslt and xpath as 4.0),
     every error to the report sink, with unit tests.
  4. Placeholders: the scan (whole name token, the instance's table
     then the schema's, abstract defaults, values not scanned again)
     and expansion segments (error offsets), with unit tests.
  5. Build, compile and validate the third-edition constructs, with
     fixtures; variables and their scopes (D315, "Variables"); reports
     with both locations.
  5b. The 2025 constructs, a fixture each: group, rules, visit-each
     (rule lets once per context node), severity and the
     error-finding rule (severity decides when present), dynamic flag,
     role and severity (variable references, AVTs), let/@as, phase
     from, when with `#ANY`, schematronEdition (any value, reported),
     the message context (6.3), expression rule contexts (with
     from, with documents, under `#ANY`).
  6. Vendor EN 16931, XRechnung and SchXslt; the driver; gates 1 and 2.
  7. SVRL; the differential test (gate 3).
  8. The CLI (`--schematron-param`, `--phase=#ANY`). 9. JavaScript.
  10. The book chapter.
- **Gates**:
  1. *EN 16931*: the UBL unit tests, 1,133 of 1,133 checks, and the
     CII unit tests, 9 of 9, pass with the rules compiled from the
     `.sch` sources (`EN16931-UBL-validation.sch`,
     `EN16931-CII-validation.sch`), counted as on 2026-10-28 (each
     test's rules in scope must or must not fire, with their counts).
  2. *Compile time*: compiling `EN16931-UBL-validation.sch` takes well
     under a second in a release build (measured, with peak memory, and
     reported beside SchXslt's 10.5 s), within the library's default
     step bound (10^8 steps; compiling evaluates only static
     expressions), and each unit test validates within it too.
  3. *Differential*: for the EN 16931 UBL and CII rule sets and
     XRechnung's UBL and CII rule sets, over their test instances, the
     set of (pattern, assertion id, kind, subject path) in sxml's
     reports equals the set of (id, kind, location) in the SVRL that
     SchXslt 1.10.1 running on sxml makes from the same `.sch`, and the
     fired rules (context, node) agree. A difference is fixed or
     explained in the record (a value that SchXslt scans again, a
     placeholder SchXslt matched by prefix, D315), never reclassified
     (D178). SchXslt has no 2025 constructs: gate 6 covers them.
  4. *Errors in the `.sch`*: each `schematron:*` code, and a syntax
     error in a test, in a rule context, in a `sch:param` value (an
     instance's, a schema's, a configured one) and in a foreign
     `xsl:function`, has a test of its file, line and column; one
     `.sch` with errors of several kinds gives all of them in one
     compile, in the C API, the CLI and JavaScript; a dynamic error in
     a test names its assertion's line.
  5. *Locations of findings*: the instance's line and column in text,
     JSON and JavaScript reports, on an EN 16931 test instance.
  6. *The 2025 edition*: 7.30's suite (`tests/schematron/`) passes,
     100 % of its applicable cases, run by the driver; each construct
     of step 5b, each Annex B constraint and each query binding name
     (cases mixed; xslt4 and xpath4 read; xquery3 and an unknown name
     refused with exit status 1) is among them, reports and SVRL
     (active-group, severity, visit-each, schematronEdition) included.
     The driver keeps a list of deliberate deviations, each with its
     record: `edition-other-value` (D315 8b, best-effort validation of
     other editions) and the cases that depend on reading the `xslt`
     binding and relative xpath contexts as sxml does (D313, D315); a
     deviation outside that list is a failure.
  6b. *Expression rule contexts*: 7.30's cases whose rule contexts are
     expressions but not patterns (`(//item)[1]`, `../x`, under xpath
     and xslt bindings, with `from` and `documents`) pass; a context
     that is a pattern compiles as before (no global, no intersect).
  7. *Compile errors of XSLT*: a stylesheet with errors in several
     declarations gives each through the sink, in C, `sxml transform`
     (one line each, exit 1) and JavaScript; the null sink gives the
     first as before; XT40 unchanged case by case.
- **Done when**: the gates pass; the C, CLI and JavaScript APIs are
  tested end to end (text, JSON and SVRL; `--schema` with
  `--schematron`; exit statuses 0, 1 (an unsupported binding among
  them), 2, 3 and 4); the book chapter's
  examples match (`tools/check-book-examples.sh`); clang-tidy and the
  call-cycle check are clean; QT4 and XT40 unchanged; the `sxml.wasm`
  and CLI size budgets raised in the same change if needed (D208),
  with the growth reported.

**Done** 2026-10-28 (D323): steps 1 to 10. The suite passes 463 of
463 applicable normative cases with 3 listed deviations
(`edition-other-value`, `binding-xpath2-no-maps`,
`error-context-is-expression`); EN 16931 UBL 1,133 of 1,133, compile
0.53 s within the default step bound; the differential agrees on all
four rule sets (CII and XRechnung patched); XT40 unchanged. Open, for
the owner: the EN 16931 CII and XRechnung sources are not correct
Schematron as read (D323, "Proposed"), so ctest runs the UBL unit tests
only; the JavaScript additions are not built (no WASI SDK).

### 7.29 Source locations for parsed input documents (design and implementation)
After D87, D313 and perf-8 (which holds `src/evaluator.c` and
`src/evaluator_function.c`). D313 settles the behaviour: a processor
setting `AUTOMATIC` (the zero value), `ALWAYS`, `NEVER`; under
`AUTOMATIC` documents a host parses keep locations, and where sxml
parses for a program the compiled program decides (it calls
`fn:location`, can reach functions dynamically, or is a validation);
dynamic errors that name an input node print its line and column;
`--locations=` in the sxml command, the same three values in
JavaScript. Today `sxml_processor_parse_document` sets
`keeps_locations = false` (`src/processor.c`), so `fn:location` is the
empty sequence for every input document; the arena tree and the parser
already support locations.
- **Design**: the field in `sxml_processor_configuration`
  (`include/sxml/processor.h`, additive after D99, with its decision
  record); one flag per compiled unit (XPath, XQuery, XSLT) that the
  lowering sets; where `fn:doc`, `fn:parse-xml` and `fn:collection` read
  it; `xpath` and `xquery` in the sxml command compile before they parse
  `--context-item`; which error records gain the location.
- **Owns**: `include/sxml/processor.h` (the new field), `src/processor.c`,
  the parts of the lowering that set the flag, the call sites of
  `fn:doc`, `fn:parse-xml` and `fn:collection`, `apps/sxml/query_command.c`,
  `apps/sxml/command_xpath.c`, `apps/sxml/command_xquery.c`,
  `apps/sxml/command_transform.c` (option only), `docs/CLI.md`, the
  JavaScript package's option, tests.
- **Done when**: `location(.)` gives line and column for the context
  item, `fn:doc` and a stylesheet's input under `AUTOMATIC` in a
  program that calls it, and nothing is kept for one that cannot ask
  (memory measured both ways on the 26 MB document of D313); errors
  print locations; QT4 and XT40 unchanged case by case; the book's
  examples match.

**Done** 2026-10-28 (D318): `sxml_source_location_policy` and
`sxml_node_get_location` (additive); a flag per IR unit set where
expressions are added, the processor's location demand, copies that
keep locations; `--locations` in xpath, xquery and transform, which
compile first; errors name a node's line; `createProcessor({ locations
})` (not built here: no WASI SDK). 173 MB without fn:location, 249 MB
with, on the 26 MB document; XP40, XQ40, XT40 identical case by case.

### cli-4 No step limit in the sxml command (implementation)
After D313 (the owner: the sxml command sets no evaluation step limit).
Today the command leaves `maximum_evaluation_step_count` at 0, the
library's default of 10^8 steps (D89, D259), about 2 s of work after
perf-5 to perf-7; a long batch job such as a Schematron compile cannot
finish, and there is no option to raise it. The library's default stays
for embedders and the JavaScript package; the drivers keep their own
bounds (D166, D178).
- **Mechanism**: the command's processor configuration asks for no
  step bound. If the header has no way to say "no bound" (0 means the
  default), use the largest value and say so in the decision record,
  or propose a named constant under "## Proposed" if that reads better
  (PACKET.md step 4).
- **Owns**: `apps/sxml/` (the processor configuration only),
  the CLI tests that rely on the step limit, `docs/CLI.md`, and the
  book's pages that describe the command stopping: `performance.md`
  ("The step limit"), `appendix-differences.md`, `ixml-performance.md`,
  `javascript.md` (which keeps the default, but cites the chapter).
- **Done when**: a command that ran into the bound runs to its end (an
  example that took more than 10^8 steps); the book's examples match
  (`tools/check-book-examples.sh`); the CLI tests pass.
**Done** 2026-10-28 (D314): the command's processor configuration sets
the step bound to UINT64_MAX (0 would mean the default); the CLI test
`xpath_no_step_limit` runs a comparison charged 4 x 10^8 steps to its
end; the book's step-limit pages and docs/CLI.md updated.

### 7.30 A Schematron conformance suite (tests)
The owner (2026-10-28): the standard (ISO/IEC 19757-3:2025, bought by
the owner) cannot be committed, so sxml writes a comprehensive
conformance suite of its own from it. No public Schematron conformance
suite of that scope is known; check (SchXslt's own tests, schematron.com,
the Schematron community's repositories) and record what exists and its
licence before writing; reuse with attribution what a licence allows.
- **Whom it serves**: 7.28 (its gate), every later change to Schematron
  in sxml, and other implementations if the owner publishes it. Who
  pays: test files and a coverage list; no code in the library.
- **The standard's text is data, not repository content**: cite clause
  numbers, paraphrase in a few words; never copy sentences, examples or
  schemas from it. The extracted text is in the orchestrator's
  scratchpad (`iso-19757-3/standard.txt`), never committed.
- **Layout** (`tests/schematron/`, imitating the QT4 catalog of
  `third_party/qt4tests`: a catalog, test sets, test cases):
  - `catalog.xml` naming the test sets; one test set per clause group
    (schema structure, patterns and rules, assertions and reports,
    `let` and parameters, abstract patterns and `extends`, inclusion,
    phases and `from`, diagnostics and properties, `name` and
    `value-of`, `group`, `library` and `rules`, `visit-each`,
    `severity`/`role`/`flag` and their dynamic evaluation, query
    bindings, `documents`, `schematronEdition`, SVRL, static errors);
  - each test case: the clause it covers, a schema (inline or a file),
    one or more instance documents, an optional phase and parameters,
    and the expected result, written neutrally (no sxml API, no sxml
    error codes): the set of findings (kind assert or report, id, the
    location as a node path, role, flag, severity, the message text
    after `value-of` and `name`), or "the schema is in error" (with the
    clause that makes it so), or "implementation-defined" with the
    clause that allows it;
  - `coverage.xml`: every normative requirement of the standard (each
    "shall" and each definition that constrains behaviour), as clause
    number plus a short paraphrase, with the test cases covering it;
    a requirement without a test is listed as such.
- **Cross-checks**: every case using only 3rd-edition constructs runs
  through SchXslt 1.10.1 on sxml (compile with the release CLI, which
  now has no step limit; D314) and must agree, or the disagreement is
  recorded with the clause that decides it. 2025-only cases are checked
  by a second, independent agent that derives each expected result from
  the clause without seeing the first one's answer.
- **Owns**: `tests/schematron/` (new), a README there (format, licence,
  how to run the cross-check), a glossary term if needed, a D-record.
  Not the driver: 7.28 writes the driver that runs the suite against
  sxml.
- **Done when**: `coverage.xml` lists every normative requirement of
  clauses 4 to 7 and the annexes that are normative, each with at least
  one test (or a recorded reason); all cases are well-formed and
  validate against a schema for the catalog format; the SchXslt
  cross-check agrees on every 3rd-edition case or each difference is
  recorded; the independent check of the 2025-only cases is recorded;
  7.28's gates name the suite (pass rate 100 % of applicable cases).
**Done** 2026-10-28 (D317): 514 cases in 18 sets (`tests/schematron/`),
139 requirements in `coverage.xml` (131 with cases, 8 with reasons), an
edge-case table of construct by dimension, the ambiguities of the
standard in the README; SchXslt cross-check of the 369 2020 cases (281
agree, the rest decided by recorded clauses); all 145 2025-only cases
checked independently in two rounds, none changed.

## Proposed for the owner

No phase covers these four features yet (D183 Proposed). The owner has
said every deferred feature must be supported in the end, but D97 and
D162 do not name these four, so they need the owner's word to join the
phase. Their packet numbers keep their places in the waves above. They
start only when the owner accepts them.

### 7.3 xsl:evaluate (implementation; proposed, D183)
- **Specifications**: XSLT 4.0, 10.5 "Dynamic XPath Evaluation" (the
  xpath, context-item, with-params, namespace-context, base-uri,
  schema-aware and as attributes; XTDE3160, XTDE3165, XTDE3170, XTDE3175).
- **Unlocks**: XT40 `evaluate` 83 cases (feature dynamic_evaluation), and
  the 5 XTDE3175 failures.
- **Replaces**: the XTDE3175 raise in `src/xslt_lower_control.c`
  (`evaluate_locals`), and supports-dynamic-evaluation.
- **Mechanism**: compile the XPath text at run time with the XPath front
  end. Its static context is built from the instruction's attributes and
  `xsl:with-param`s; the parameters are external variables. Then ask the
  machine for a unit evaluation (D154), whose value is the instruction's
  value. No new IR kind is needed beyond what fn:load-xquery-module
  uses.
- **Header**: none.
- **After**: nothing.
- **Owns**: `src/xslt_lower_control.c` (the evaluate procedure) and new
  `src/xslt_evaluate.c`.
- **Size**: about 5 KB, in the CLI only.
- **Gate**: ≥ 90 % of the 83.
**Done** 2026-10-19 (D196): evaluate 73 of 83 (91 % of the 80 run, 88 %
of the 83); XT40 8,899 to 8,973 passed, 0 errors, none lost. The gate
waits for 7.10 (html-doc, parse-html: 3 cases) and trust checks in
json-doc and unparsed-text-available (2 cases).

### 7.5 disable-output-escaping (implementation; proposed, D183)
- **Specifications**: XSLT 4.0, 26.2 "Disabling Output Escaping"
  (deprecated, optional, and its permission to drop the marker when the
  result is not serialized); Serialization 4.0, 2 (the phases).
- **Unlocks**: XT40 disabling_output_escaping 32 (disable-output-escaping
  28, bug 4, and others).
- **Replaces**: today the attribute is read and ignored
  (`src/xslt_read.c`); the place where it is ignored is the place.
- **Mechanism** (changed by D188, on the owner's prompt): a text node
  mark, as D163's CDATA request, additive to `tree.h`
  (`disables_output_escaping`); the XML, XHTML and HTML methods write
  marked text unescaped. The JAXP processing-instruction pair is not
  used.
- **Header**: none; a comment in `serialize.h`.
- **After**: nothing.
- **Owns**: the d-o-e branches in `src/xslt_lower_construct.c`, and
  `src/serialize_xml.c` and `src/serialize_html.c` (the pair).
- **Size**: about 3 KB, in the CLI only (the serializer is in
  `sxml.wasm`: under 1 KB there).
- **Gate**: ≥ 90 % of the 32.
**Done** 2026-10-19 (D188): disable-output-escaping 25 of 25 run pass
(7 not run: 3 need streaming, 1 is XSLT 2.0 only, 3 need the feature
absent), bug 4 more; XT40 8,759 to 8,784 passed, 0 errors, none lost
(5 cases that need the feature absent are not run any more).

### 7.13 XML 1.1 input (implementation; proposed, D183)
- **Specifications**: XML 1.1 (Second Edition), 2.2 (Char, RestrictedChar:
  C0 controls only as character references, C1 controls as references),
  2.11 (line ends with NEL and LINE SEPARATOR), 4.3.4 (version), 2.13
  (normalization checking, optional); Namespaces in XML 1.1
  (undeclaring prefixes, already done, D81); Serialization 4.0, the
  version parameter (1.1 output, where character references are
  required).
- **Unlocks**:
  - QT4 xml-version 1.1: 313 XQ40, 291 XP40;
  - XT40 XML_1.1 65 (xml-version 42, namespace 16, and others);
  - xmlconf 275 (the ibm and eduni xml-1.1 catalogs; today
    `tests/xml_conformance_test.c` does not select them);
  - a few XSD instance failures (D180's leftovers).
- **Replaces**: XML 1.1 documents are parsed by the 1.0 rules
  (`src/xml.c`, D81's place).
- **Mechanism**: the XML grammar's external scanner takes the version
  from the declaration as scanner state, and checks Char and line ends
  by version. The tree builder is unchanged. fn:parse-xml and the
  drivers state the version. The serializer's version 1.1 writes
  references for restricted characters.
- **Header**: none (a comment in `xml.h`).
- **After**: 7.2 (it grows the core by a few KB).
- **Owns**: `grammars/xml` (scanner), `src/xml.c`,
  `tests/xml_conformance_test.c` (the selection), and
  `src/serialize_xml.c` (version 1.1).
- **Size**: 2–8 KB.
- **Gate**: ≥ 95 % of the 275 xmlconf cases; ≥ 90 % of the QT4 and XT40
  cases.

**Done** 2026-10-19 (D195), gate partly met: xmlconf 242 of 271 XML 1.1
cases (89.3 %; every case without external entities passes, the 29
failures need external entities or DTDs, which is 7.14's); QT4 XQ40 300
of 313 (95.8 %; the 13 need XQuery's own 1.1 lexing, D195 Proposed);
XT40 xml-version 40 of 42, 65 XML_1.1 cases now run. The grammar is
unchanged: a 1.1 pre-pass in `src/xml.c` instead of scanner state.

### 7.14 DTD processing: entities, defaults, validation (design and implementation, two packets; proposed, D183)
- **Specifications**: XML 1.0 (Fifth Edition):
  - 2.8 (the external subset and its conditional sections), 4.1 to 4.4
    (parameter entities in the external subset, external parsed
    entities, text declarations), 3.2 to 3.4 (element type, attribute
    list and conditional section declarations), 3.3.2 (attribute
    defaults), 3.3.3 (attribute value normalization by declared type);
  - validity constraints (2.8 VC: Root Element Type, 3 VC: Element
    Valid, ID, IDREF, Entity Name, Notation, Enumeration, Fixed
    Attribute Default);
  - F&O 4.0, fn:doc and fn:parse-xml's dtd-validation option
    (FODC0013 today);
  - XDM 4.0, 6 (the infoset mapping: is-id, unparsed entities, attribute
    types).
- **Unlocks**: QT4 infoset-dtd 32 XQ40 and 27 XP40 (fn-id, fn-idref),
  and the 7 FODC0013 failures; XT40 dtd 78 (id 42, unparsed-entity-uri
  20, and others); xmlconf 265 cases with external entities (127 valid,
  54 invalid, 66 not-wf, 18 error); and validity checking of the 175
  invalid cases.
- **Replaces**: two places today. External entities are skipped in
  content and the external subset is not read (`src/xml.c`, D81). The
  dtd-validation option raises FODC0013
  (`src/function_library_json.c`); supports-dtd-validation and
  supports-dtd-attribute-typing are false.
- **7.14a Entities and defaults**:
  - the external subset and parameter-entity references between
    declarations: a grammar change to `grammars/xml` (D28 left the
    external subset without a grammar), with text declarations;
  - external entities read through the resolver (a new resource kind,
    EXTERNAL_ENTITY, form BYTES);
  - attribute defaults and normalization sent as builder events (as the
    XSD validator sends them, D165);
  - entity limits stay those of `xml.h`.
- **7.14b Validation**:
  - the DTD's declarations become schema components (content models as
    model groups; attribute types as simple types: ID, IDREF(S),
    ENTITY, NMTOKEN(S), NOTATION, enumerations);
  - one validator, the schema validator, checks the document as it is
    parsed; XML's validity constraints map to its errors, and the DTD's
    own codes are kept in the description;
  - no second content-model engine.
- **7.14c Follow-ups** (D199's open items, D210): the QT4 driver claims
  infoset-dtd and dtd-validation-option and reads environment sources
  through the entity resolver, with supports-dtd-validation true; `sxml
  check --external-entities` and `--validate-dtd` (D44); XT40's dtd
  failures: XTDE1370/1380 codes, a parse-xml source in the driver (id-043),
  unparsed entities kept by xsl:copy and xsl:copy-of
  (unparsed-entity-05 to 08), an element base URI from external entities
  (base-uri-051, 051b; additive `tree.h`, D99); the clang-tidy finding in
  `src/processor.c`.
- **Header**: `xml.h`, additive (options for the external subset,
  external entities and DTD validation, and the validator hook), and
  `processor.h` (the resource kind), with records.
- **After**: 7.13 (both use `grammars/xml` and `src/xml.c`); 7.14b after
  7.14a.
- **Owns**: `grammars/xml`, `src/xml.c`, `src/xml_entity.c`, new
  `src/dtd.c`, `src/dtd_internal.h` and `src/schema_from_dtd.c`, the
  FODC0013 place, and `tests/xml_conformance_test.c` (the entity
  selection).
- **Size**: 30–60 KB.
- **Gate**: ≥ 95 % of the 265 xmlconf entity cases and of the 175 invalid
  cases checked as invalid; ≥ 90 % of the QT4 and XT40 dtd cases.

**Done** 2026-10-19, 7.14a and 7.14b (D199): xmlconf entity cases 265 of
265 run as expected (valid 127/127, invalid 54/54, not-wf 66/66, 18
error), DTD validation of the invalid cases 173 of 173 standalone and 54
of 54 external; QT4 fn-doc and fn-parse-xml gained 11 XQ40 and 11 XP40;
XT40 dtd 68 of 79 (86 %, under the gate), left to 7.14c.

**Done** 2026-10-19, 7.14c (D210), gate met: XT40 dtd 76 of 79 (96 %;
whole suite 8,944 to 8,952, none lost); QT4 infoset-dtd sets XQ40 fn-id
61 of 61, fn-idref 51 of 54, app-FunctxFunctx 627 of 627, XP40 fn-id 30,
fn-idref 24 (all run cases but 3 schema-typed fn-idref ones); xmlconf
unchanged.

## Gate (proposed in D183; the owner may change the numbers)

Reviewed by packet 7.19 (D237) on whole release runs at 9c5b1ba.

- [ ] Each feature meets its packet's gate above under `release`: every
  one but two, see the table below. Not met: 7.11's "all 12 XT40
  `transform` cases" (9 pass, 2 fail, 1 not run: a static fn:transform,
  a ~record pattern, an assertion the driver does not check; D203; after
  7.20, 10 pass: the static fn:transform needs a compiler that does not
  re-enter itself (D71), the ~record pattern packet 4.22's newest-draft
  patterns (D245); D249). Met
  as D208 reads it: 7.4's "all 5 QT4 cases" (1 passes; the other 4
  expect fn:number conversion of xs:numeric parameters, which XPath 4.0,
  3.4.1 gives only to xs:double, and D208 follows the specification).
- [x] XP40 ≥ 90 %, XQ40 ≥ 85 % and XT40 ≥ 70 % still; the passed counts
  of XP40, XQ40, XT40 and XSD have not dropped; the schema-aware gates of
  Phase 5 still hold; all ctest slices are at their baselines (agent
  review): XP40 99.5 %, XQ40 99.5 %, XT40 94.8 % (core sets 88.5 % to
  100 %); passed 32,689, 45,969, 12,075 and 41,486 (D182: 30,931,
  41,273, 8,650, 41,483); XSD in scope 99.9 %, datatypes 100 %,
  structures 99.7 %, XSD 1.1 99.7 %; schema-aware XQ40 92.2 %, XT40
  95.6 %; slices 198/198, XSLT 149 of 154 (baseline), XSD 147/147.
- [x] No in-scope test crashes, leaks or ends in an "error" outcome,
  including the new suites (xmlconf 1.1 and entity cases, and the iXML
  suite if vendored) (agent review): 0 errors in XP40, XQ40, XT40 and
  XSD; xmlconf, html5lib and the iXML suite pass their whole runs (the
  iXML suite's 4 errors were inputs no xs:string holds, now not run with
  that reason, D237).
- [x] `sxml.wasm` and the CLI module are within the budgets the owner set
  in answer to D183's size item; the size test checks every module
  variant (agent review): `sxml.wasm` 2,090,746 / 730,902 of 2,200,000 /
  770,000, `sxml-full.wasm` 7,637,162 / 1,690,683 of 8,000,000 /
  2,800,000, the CLI 9,231,791 / 1,973,839 of 9,500,000 / 2,100,000; the
  D225 guard names +1,978 bytes since its baseline (the IR's
  GLOBAL_READ, 4.21).
- [ ] The fuzz targets cover each new parser (HTML, iXML, DTD, XML 1.1,
  Full Text selections), run on the owner's machine (D181): the targets
  exist (agent review: `ixml_fuzz` and `full_text_fuzz` added, XML 1.1
  through `xml_parse_fuzz`), two-minute runs here found one bug, fixed
  (D237); the full runs are deferred to the owner's machine (D181).

| Packet | Its gate or done-when | Fresh numbers (9c5b1ba, release) | |
| --- | --- | --- | --- |
| 7.2, 7.2b, 7.2c (size-2), health-3 | core under budget, one part list per language, the size guard | sizes above; the core links no feature definition, the guard prints its top growths | met |
| 7.3 | ≥ 90 % of 83 | evaluate 77 of 83 (92.8 %) | met |
| 7.4, 7.4b | ≥ 80 % of the 124; all 5 QT4 | backwards_compatibility 115 of 131 (87.8 %); QT4 1 of 5 | met as D208 reads it |
| 7.5 | ≥ 90 % of 32 | disable_output_escaping 32 of 32 run (9 more need the feature absent, XSLT 2.0 or streaming fallback) | met |
| 7.6a–g | ≥ 85 % of the streaming cases; XT40 passes kept; peak under 64 MB | streaming 2,882 of 2,938 (98.1 %); strm 2,521 of 2,564; 1,000,000 orders (74.7 MB) summed streamed in 11.0 MB peak resident | met |
| 7.7a–c | ≥ 85 % of 798 | upd-* 787 of 798 (98.6 %; 27 staticTyping not run) | met |
| 7.8 | ≥ 95 % of 673 | binary 673 of 674 (XQ40), bin-* 672 of 672 (XP40, XQ40) | met |
| 7.9 | ≥ 90 % of 271 | expath-file 270 of 270 (XQ40), 267 of 267 (XP40) | met |
| 7.10a, 7.10b | ≥ 95 % HtmlTestSuite; ≥ 90 % parse-html, html-doc | 1,378 of 1,379; 22 of 22; 9 of 10; html5lib 1,749 / 7,045 / 82 | met |
| 7.11 | ≥ 80 % of 133; all 12 XT40 | fn-transform 130 of 137 (XQ40), 127 of 137 (XP40); XT40 transform 9 of 12, 10 after 7.20 (D249: 004 needs a resumable compiler under D71, 401 the "~T" patterns of 4.22) | not met (XT40) |
| 7.12a–d | ≥ 75 % of 685 | ft-* 672 of 685 (98.1 %); XP40 unchanged | met |
| 7.13 | ≥ 95 % of the xmlconf 1.1 cases; ≥ 90 % QT4 and XT40 | xmlconf 1.1 271 of 271 (valid 84, invalid 13, not-wf 169, error 5); QT4 300 + 1 of 313 (96.2 %); XT40 XML_1.1 62 of 65 run | met |
| 7.14a–c | ≥ 95 % of the entity and invalid cases; ≥ 90 % QT4 and XT40 dtd | xmlconf entities 265 of 265, invalid 175 of 175, DTD validation 173 + 54 of 227; XT40 dtd 84 of 87 (96.6 %); QT4 infoset-dtd 33 of 34 | met |
| 7.15a, 7.15b | ≥ 95 % of 49; ≥ 90 % others; CollationTest | 49 of 49; 13 of 13 (XQ40), 5 of 5 run (XP40); CollationTest samples 10,314 and 11,391 lines in order | met |
| 7.16 | all 14 | olson-timezone 8 of 8, fn-civil-timezone 9 of 9 | met |
| 7.17 | ≥ 75 % of 38; none lost | QT4 20 of 20; XT40 9 of 9 (the cases naming the 14 dependencies); fn-format-date 137 of 137 run, fn-format-integer 94 of 94 run | met |
| 7.18 | all 16 QT4; ≥ 90 % of the correct-grammar tests | 16 of 16 (XP40, XQ40); suite 886 of 907, correct-grammar 711 of 711 run, grammar tests 142 of 142 | met |
| js-1 | the three languages in the package; `sxml.wasm` unchanged | the `javascript/*` tests pass (wasm preset, 383 of 383) | met |
| cli-2 | written files, csv, hints | CLI tests pass (release, 379 of 379) | met |
| perf-1, perf-2, perf-3 | numbers recorded; outcomes unchanged | xmark-all 1.10 s, itunes 2.29 s, parse 0.16 s (processor seconds, release); suite counts as at e4ea880 but XT40's gains | met |
| xml-mem | memory by depth, not input | the 74.7 MB run above in 11.0 MB; the allocator-limit unit test passes | met |

## Questions for the owner (D183 Proposed)

1. Do xsl:evaluate, DTD processing, XML 1.1 input and
   disable-output-escaping join Phase 7 as 7.3, 7.14, 7.13 and 7.5?
2. Size: which option (1 to 4) do you choose, and what budgets?
3. Are the per-feature gate numbers above acceptable?
4. New test data: vendor the iXML community tests (`third_party/ixml-tests`)
   and Unicode's CollationTest (about 30 MB unpacked; a sampled subset
   would be about 1 MB)? xmlconf is already vendored.
5. Which languages and calendars should be supported, and which UCA
   tailorings built in? The proposal is en, de, fr, it, es and nl; the
   calendars ISO, AD, BE, JE, OS and AH; and tailorings for the same
   languages.
6. Should the Update Facility follow XQUF 3.0 only, or also the 4.0
   community draft that some QT4 cases use?
7. EXPath File in the CLI: should it be off unless `--allow-file-access`
   is given?
