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
- **7.15b Tailoring**: `lang` and `reorder` through CLDR rules compiled at
  first use into a tailoring (glossary `tailoring`). The rules are a
  built-in set for a few languages, or come through the resolver
  (option 4). Owner question 5 names the set.
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

### 7.19 Phase 7 review (review)
After every packet the owner accepted. Owns the gate boxes. A separate
agent (D11) runs the rubric and re-checks each "done when" and the size
budgets. Open items go under "Proposed".

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

- [ ] Each feature meets its packet's gate above under `release`.
- [ ] XP40 ≥ 90 %, XQ40 ≥ 85 % and XT40 ≥ 70 % still; the passed counts
  of XP40, XQ40, XT40 and XSD have not dropped; the schema-aware gates of
  Phase 5 still hold; all ctest slices are at their baselines.
- [ ] No in-scope test crashes, leaks or ends in an "error" outcome,
  including the new suites (xmlconf 1.1 and entity cases, and the iXML
  suite if vendored).
- [ ] `sxml.wasm` and the CLI module are within the budgets the owner set
  in answer to D183's size item; the size test checks every module
  variant.
- [ ] The fuzz targets cover each new parser (HTML, iXML, DTD, XML 1.1,
  Full Text selections), run on the owner's machine (D181).

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
