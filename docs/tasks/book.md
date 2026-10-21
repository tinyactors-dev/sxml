# The book around five projects (plan, packet book-5)

Internal: this file is not part of the book. It plans the rewrite of
`docs/book` that the owner asked for on 2026-10-21 (D299):

> restructure the content of the book around useful examples instead of
> the current ones: 1. keeping structured specifications for projects
> using XML (think IETF-style RFCs, without needing an LLM to edit large
> text), 2. creating and transforming SVG (e.g. a diagramming tool),
> 3. annotating SCXML files with data schemas and XHTML for
> self-describing machines, 4. using the tree-sitter bridge to create a
> diff of call chains, 5. recording OTel traces before and after a change
> and rendering a trace diff for changes in cardinality and communication
> participants.

The book keeps D257's rules: The Rust Programming Language's style
(teach by running small, real examples; no phases, packets or decision
numbers in the text), every console block checked by
`tools/check-book-examples.sh`, `<!-- examples: NAME -->`,
`<!-- requires: javascript -->` and `<!-- status: draft -->` as today.
The library/loans data goes away everywhere except Chapter 18 (codegen),
which is already a project and stays as it is.

## 1. One story, five projects

All five projects belong to one small, made-up system, **Latch**: door
locks in a building, a controller, and the services behind it. The reader
meets one domain and sees the projects feed each other:

| Project | What the reader builds | Data (committed by book-5) |
| --- | --- | --- |
| spec | The Latch protocol as an Internet-Draft in XML: indexes of requirements and terms, checks, targeted edits, HTML and RFC-style text | `examples/spec/` |
| svg | A small diagramming tool: a graph description, a computed layout, SVG written by XQuery and XSLT, SVG transformed | `examples/svg/` |
| scxml | The lock as an SCXML machine whose data and events carry XSD types and whose states carry XHTML documentation; checks and generated documentation | `examples/scxml/` |
| call-chains | The lock service's TypeScript before and after a change, its call graph, and the diff of its call chains | `examples/call-chains/` |
| traces | OTLP JSON traces of the same change before and after, and the diff of cardinalities and participants, rendered as HTML and SVG | `examples/traces/` |

The links between projects are real and small: the machine's
`latch:implements` attributes name the draft's requirement anchors; the
machine's numbers (3 attempts, 10 s, 300 s) are the draft's; the code
change of call-chains is the change the traces record; the diagram
vocabulary of svg draws the services, the machine, the call-chain diff and
the trace diff. The book says once, early (introduction), that the
projects share a story, and each project chapter can be read on its own.

## 2. The shared data

Everything below is committed and checked with a release build of HEAD
(`tools/private-build.sh SCRATCH/build-book-5 --preset release --`).
These files are the *seed* of each project directory.

### spec: an Internet-Draft in a subset of xml2rfc v3

`spec/latch.xml` (draft -01) and `spec/latch-00.xml` (the earlier draft),
`spec/latch.xsd`.

- **Vocabulary**: a subset of xml2rfc version 3 (RFC 7991 and the RFC
  Editor's current v3 grammar), no namespace, xml2rfc's element names and
  meanings: `rfc` (version, category, ipr, submissionType, docName),
  `front` (title, seriesInfo, author with organization and address/email,
  date, area, keyword, abstract), `middle` with nested `section` (anchor,
  `name`, `t`, `dl`/`dt`/`dd`, `sourcecode`), inline `xref`, `bcp14`,
  `em`, `strong`, `tt`, `cref`, and `back` with nested `references` and
  `reference`. Chosen over an own vocabulary because it is what IETF
  authors write, xml2rfc can render the file, and the book can say so.
- **Requirements** are paragraphs `t` with an `anchor` holding `bcp14`
  keywords (11 in -01: MUST 5, MUST NOT 1, SHOULD 2, SHOULD NOT 1, MAY 2;
  plus one unanchored SHOULD in Security
  Considerations that `cref` issue-2 flags, deliberately: the
  "add a requirement" edit gives it an anchor). Anchors are names given
  once in the order written (`req-1`…`req-11`, not in document order:
  -01 inserted req-9, req-10, req-11 into existing sections); the
  renderer numbers sections and requirements by position. Terms are the
  `dt`s of section 1.1 with anchors `term-*`, used through
  `<xref target="term-…">text</xref>`. References: RFC 2119, 5234, 8174
  (normative), RFC 6238 (informative, new in -01). One ABNF
  `sourcecode` (for the iXML task). Two `cref` editorial notes.
- **-00 vs -01**: -01 adds req-9 (TOTP, with the RFC 6238 reference),
  req-10 (status rate) and req-11 (lockout report), changes req-7
  ("five" to "three" rejected codes) and req-8 ("one minute" to "five
  minutes"), and the dates and names. Anchors of unchanged requirements
  are the same, so a diff by anchor works.
- **latch.xsd** (XSD 1.1, `vc:minVersion="1.1"`): named simple types
  (`anchor`, `draft-name` with a pattern, `keyword` enumerating BCP 14's
  eleven levels with an annotation, `month`, `day`), mixed content
  (`text`), a group of inline elements, `xs:simpleContent` extensions
  (`title`, `sourcecode`), recursive `section` and `references`, an
  `xs:assert` (docName equals the Internet-Draft seriesInfo), `xs:unique`
  over all anchors (not `xs:key`: most paragraphs have none) and an
  `xs:keyref` from `xref/@target`.

### svg: a diagram description and its drawing

`svg/diagram.xml`, `svg/diagram.svg`.

- **Vocabulary** (own, no namespace): `diagram` (title) with `node` (id,
  label, optional kind="store") and `edge` (from, to, optional label).
  The svg packet adds an optional `class` on node and edge (copied to the
  SVG group, so a diff can colour `added`, `removed`, `changed`).
- **diagram.svg** is the drawing the svg chapter's code must produce,
  made with this layout (a sketch in XQuery made it; the svg packet
  writes the book's version and may regenerate the file if it changes a
  rule, saying so):
  rank of a node = breadth-first distance from the nodes without
  incoming edges (cycles allowed; for this DAG equal to the longest
  path); one row per rank, top to bottom, nodes in document order,
  each row centred; boxes 140 x 40, gaps 40 across and 60 down, padding
  20, so width = 2·20 + n·140 + (n−1)·40 for the widest row; edges are
  lines from the bottom centre of the source to the top centre of the
  target with an arrow marker, the label at the midpoint (+6, +4);
  services `rect rx="6"`, stores `rx="20"`; a `style` element with five
  rules; ids `node-ID`, `edge-FROM-TO`; edges drawn before nodes. The
  file is the serialization with `indent=yes` of the SVG namespace as the
  default namespace.

### scxml: a door lock that describes itself

`scxml/lock.scxml`, `scxml/scxml.xsd`, `scxml/latch-data.xsd`.

- **Machine** (SCXML 1.0 namespace, `datamodel="ecmascript"`): states
  locked (initial), checking, unlocked, lockout, jammed; transitions with
  events, `cond`, `target`, executable `assign`, `send`, `log`.
  Self-description, in the namespace `urn:example:latch`:
  `latch:events` declares each event (`latch:event name`, optional
  `latch:payload` naming a payload element of latch-data.xsd, and an
  XHTML `p`); each `data` has `latch:type` naming a simple type; states
  and transitions have `latch:implements` with requirement anchors of
  spec/latch.xml; each state has an XHTML `div`.
- **Deliberate flaws**, valid against the schemas, found by the
  schema-aware checks of Chapter 7: `jammed` is unreachable (no
  transition targets it), `door.opened` is handled but not declared,
  `bolt.jammed` is declared but neither handled nor sent, and
  `status.response` is only sent (a check must count `send/@event` as
  used, or report it as outgoing).
- **scxml.xsd**: a subset of the Recommendation's schema (said in its
  comment: one target per transition, so a keyref can check it; four
  executable elements), `xs:import` of latch-data.xsd, lax `xs:any` and
  `xs:anyAttribute` `##other`, two `xs:assert`s citing SCXML 1.0 3.5
  and 5.3.1, `xs:key` state-id with keyrefs for targets and initial.
- **latch-data.xsd**: simple types with facets (attempt-count,
  attempt-limit ≤ 10, seconds ≤ 3600, door-id and code patterns matching
  the draft's ABNF, state enumeration), payload elements `credentials`
  and `status` (with an `xs:assert` tying `retry-after` to lockout),
  global attributes `type`, `payload`, `implements` (an `xs:list`), the
  `events` element with `xs:unique` on names, and `latch:value` of type
  `xs:anySimpleType` so that a query can check a data value against its
  type with `xsi:type` (verified: `validate { <latch:value
  xsi:type="latch:attempt-limit">30</latch:value> }` raises
  cvc-maxInclusive-valid).

### call-chains: a TypeScript service, before and after

`call-chains/before/{server,doors,registry,cache,http}.ts`,
`call-chains/after/{server,doors,registry,audit,http}.ts` (TypeScript,
built in; namespace imports `import * as registry from "./registry.ts"`
make callees module-qualified).

The change, in `doors.ts`: `listDoors` called `registry.getDoors()` once;
now it calls `registry.getDoorIds()` and then `registry.getDoor(id)` in a
`for … of` loop (an N+1; a static check sees the call inside a loop);
`unlockDoor` no longer reads `cache.getDoor` (cache.ts is gone) and now
calls `audit.record` (audit.ts is new). server.ts and http.ts are
unchanged; the calls of `start` are inside an arrow function, so the call
graph attributes calls to the enclosing named function.

### traces: OTLP JSON before and after

`traces/before.json` (16 spans), `traces/after.json` (30 spans), each an
OTLP `ExportTraceServiceRequest` in JSON (the OTLP/HTTP JSON encoding:
hex trace and span ids, kind as an integer, times as decimal strings,
`intValue` as a string), grouped as exporters write them: resourceSpans
per service (service.name, service.version, deployment.environment.name),
scopeSpans per instrumentation library (http, pg, redis). Two traces per
file, `GET /doors` and `POST /doors/{id}/unlock`, attributes after the
stable HTTP and database semantic conventions (http.request.method,
http.route, url.template, server.address, db.system.name,
db.operation.name, db.collection.name, db.query.text). Participants come
from client spans: the service of the server span whose parent is the
client span, else `server.address` (redis and postgresql have no server
spans). The diff (checked with a sketch query):

```
GET /doors: lock-registry -> postgresql  1 -> 5
GET /doors: lock-service -> lock-registry  1 -> 5
POST /doors/{id}/unlock: audit-service -> postgresql  0 -> 1
POST /doors/{id}/unlock: lock-service -> audit-service  0 -> 1
POST /doors/{id}/unlock: lock-service -> door-cache  1 -> 0
```

lock-service's version goes from 2.3.0 to 2.4.0; the four doors are
front, back, garage, office.

### Verification (release build of HEAD, run in each directory)

```console
$ cd spec && sxml validate --schema=latch.xsd latch.xml latch-00.xml; echo $?
0
$ sxml xpath --context-item=latch.xml 'count(//t[bcp14]), count(//t[bcp14][not(@anchor)]), count(//xref), count(//dt)'
12
1
11
4
$ cd ../scxml && sxml validate --schema=scxml.xsd lock.scxml; echo $?
0
$ cd ../svg && sxml check diagram.xml diagram.svg; echo $?
0
$ cd ../call-chains && sxml xpath --allow-file-access 'for $version in ("before", "after") for $file in sort(file:list($version)) let $tree := sxml:parse-code(file:read-text($version || "/" || $file), "typescript") return `{$version}/{$file}: {count($tree//function_declaration)} functions, {count($tree//(ERROR | *[@missing]))} errors`'
"before/cache.ts: 1 functions, 0 errors"
"before/doors.ts: 4 functions, 0 errors"
"before/http.ts: 1 functions, 0 errors"
"before/registry.ts: 3 functions, 0 errors"
"before/server.ts: 2 functions, 0 errors"
"after/audit.ts: 1 functions, 0 errors"
"after/doors.ts: 4 functions, 0 errors"
"after/http.ts: 1 functions, 0 errors"
"after/registry.ts: 3 functions, 0 errors"
"after/server.ts: 2 functions, 0 errors"
$ cd ../traces && sxml xpath '("before.json", "after.json") ! `{.}: {count(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*)} spans of {count(distinct-values(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*?traceId))} traces in {string-join(json-doc(.)?resourceSpans?*?resource?attributes?*[?key = "service.name"]?value?stringValue, ", ")}`'
"before.json: 16 spans of 2 traces in api-gateway, lock-service, lock-registry"
"after.json: 30 spans of 2 traces in api-gateway, lock-service, lock-registry, audit-service"
```

Also tried, so the chapters can rely on them: a broken copy of
latch.xml gives cvc-enumeration-valid (bcp14 MIGHT), cvc-assertion,
cvc-identity-constraint.4.1 (an anchor twice) and 4.3 (an xref to no
anchor); a broken lock.scxml gives 4.1 on a duplicate event, cvc-assertion
on `data` with expr and src, and 4.3 on a missing target; a schema-aware
query (`import schema` of both schemas, `validate { doc("lock.scxml") }`)
finds the four flaws above, and `sum(@expr)` on the validated machine
fails with FORG0006 because `expr` is typed xs:string (a typed-data
lesson in itself).

## 3. Rules for the rewrite packets

1. **Project directories are example directories.** A chapter file that
   uses a project names that project's directory:
   `<!-- examples: spec -->`. The reader runs every command of a project
   from one directory, like a real repository. Chapters that use no
   project keep their own directory (getting-started, ixml, source-code,
   c, javascript, codegen).
2. **Seed files are frozen during the rewrite.** The files of section 2
   are read-only for every packet. A packet that needs a change writes it
   under "Proposed data changes" at the end of this file and goes on
   without it; the integration packet decides. (The svg packet may
   regenerate `diagram.svg` from its own code, as said above; it is the
   only exception, and it then runs every chapter that shows that file.)
3. **New files in a shared directory** are owned by the packet that adds
   them. Before adding a name, `git ls-files docs/book/examples/DIR` and
   the "Files" list of the other packets below; never take a name another
   packet lists. Never list a project directory's root in an example
   (`file:list(".")`, `ls`): other packets add files there. A packet that
   needs a listing makes a subdirectory of its own.
4. **Cross-project files** are relative symlinks to seed files (or to a
   file another packet has committed, which is then an order constraint),
   committed in the using directory by the using packet, e.g.
   `scxml/latch.xml -> ../spec/latch.xml`. `check-book-examples.sh`
   copies directories with `shutil.copytree`, which follows symlinks, and
   `{{#include}}` should name the target's real path.
5. **SUMMARY.md, introduction.md, foreword.md and the appendices** belong
   to the integration packet (book-13). A rewrite packet checks its files
   with `tools/check-book-examples.sh BUILD file.md…` (which does not need
   SUMMARY) and lists any new glossary term for book-13 in its final
   report. New chapter files are not in SUMMARY until book-13; mdBook
   ignores them meanwhile. Run `mdbook build` only after book-13.
6. **Cross references**: refer to other chapters by title with a link
   (`[Transforming with XSLT](xslt.md)`), not by number: the numbers move.
   book-13 replaces the remaining "Chapter N" texts (37 today).
7. **Old example directories** are deleted by the packet that takes over
   their chapters, in the commit that stops using them (see each packet).
8. **Every feature in the coverage table (section 6) is taught where
   the table says**, with at least the depth the old chapter had; a
   packet that cannot (an sxml limit), keeps the old text's point with
   the new data and says so in its report.
9. Each packet: release build of a HEAD snapshot (one at a time, deleted
   at the end), the check on its chapter files, a LOG entry, a decision
   record only for choices the next packets need, commit and push at
   each finished chapter. The book never mentions packets, phases or
   decisions.

## 4. The new SUMMARY.md

```markdown
# Summary

[Foreword](foreword.md)
[Introduction](introduction.md)

- [Getting Started](getting-started.md)
    - [Installation](installation.md)
    - [Hello, XPath](hello-xpath.md)
    - [The sxml Command](the-sxml-command.md)
- [Finding Things with XPath](xpath.md)
    - [Paths, Steps and Predicates](xpath-paths.md)
    - [Values and Types](xpath-values.md)
    - [Functions and Operators](xpath-functions.md)
    - [Maps, Arrays and Records](xpath-maps-arrays.md)
    - [Functions as Values](xpath-higher-order.md)
- [Writing Programs with XQuery](xquery.md)
    - [FLWOR Expressions](xquery-flwor.md)
    - [Grouping](xquery-grouping.md)
    - [Building XML](xquery-constructors.md)
    - [Conditions, Types and Errors](xquery-types-errors.md)
    - [Functions and Modules](xquery-modules.md)
    - [Documents, Parameters and the Context](xquery-inputs.md)
    - [Writing Results](xquery-serialization.md)
    - [Updating Documents](xquery-updates.md)
    - [What's New in XQuery 4.0](xquery-4.md)
    - [Project: Editing a Specification Without Rewriting It](xquery-project.md)
- [Transforming with XSLT](xslt.md)
    - [Templates and Modes](xslt-templates.md)
    - [Keys, Numbers and Grouping](xslt-keys-grouping.md)
    - [Many Outputs, Functions and Packages](xslt-packages.md)
    - [Project: Publishing the Specification](xslt-project.md)
- [Project: Drawing Diagrams with SVG](svg.md)
    - [Computing a Layout](svg-layout.md)
    - [Writing SVG with XQuery](svg-xquery.md)
    - [Drawing with XSLT](svg-xslt.md)
    - [Transforming SVG](svg-transform.md)
- [Validating with XSD](xsd.md)
    - [Simple Types and Facets](xsd-simple-types.md)
    - [Complex Types and Content Models](xsd-complex-types.md)
    - [Deriving Types](xsd-derivation.md)
    - [Namespaces and Larger Schemas](xsd-namespaces.md)
    - [Keys and References](xsd-identity.md)
    - [XSD 1.1 Features](xsd-11.md)
    - [Reading Validation Errors](xsd-errors.md)
    - [Typed Data and Schema-Aware Queries](xsd-typed-data.md)
- [Project: Self-Describing State Machines](scxml.md)
    - [Checking What the Schema Cannot](scxml-checks.md)
    - [Documenting the Machine in XHTML](scxml-xhtml.md)
    - [Drawing the Machine](scxml-drawing.md)
- [Parsing Text with Invisible XML](ixml.md)
    - [Writing Grammars](ixml-grammars.md)
    - [Real Tasks](ixml-tasks.md)
    - [When a Parse Fails](ixml-errors.md)
    - [Invisible XML Performance](ixml-performance.md)
- [JSON, CSV and HTML](formats.md)
- [Source Code as XML](source-code.md)
- [Project: A Diff of Call Chains](call-chains.md)
    - [A Call Graph from the Code](call-chains-graph.md)
    - [Comparing Two Versions](call-chains-diff.md)
- [Project: Comparing Traces Before and After a Change](traces.md)
    - [Spans as a Tree](traces-tree.md)
    - [Who Calls Whom, and How Often](traces-cardinality.md)
    - [The Trace Diff](traces-diff.md)
    - [Rendering the Diff](traces-rendering.md)
    - [Streaming Large Trace Files](traces-streaming.md)
- [Working with Text](text.md)
- [Files and the Outside World](files.md)
- [Embedding sxml in C](c.md)
- [sxml in JavaScript](javascript.md)
- [Performance and Memory](performance.md)
- [Project: Generating Code from a Schema](codegen.md)
    - [Building a Tree of the Program with XQuery](codegen-typescript.md)
    - [Printing the Tree with XSLT](codegen-printer.md)
    - [Running the Generated Code](codegen-running.md)
    - [A Pipeline, a Second Target and a Changing Schema](codegen-pipeline.md)

---

- [Appendix A: The Command-Line Reference](appendix-cli.md)
- [Appendix B: Conformance](appendix-conformance.md)
- [Appendix C: Glossary](appendix-glossary.md)
- [Appendix D: Differences from Other Processors](appendix-differences.md)
```

Eighteen chapters, seven of them projects. Order: the three languages on
the spec (the spec is the running example of Chapters 2 to 4, and each of
those chapters ends with a project section that assembles a tool), then
the first stand-alone project (SVG) once XQuery and XSLT are known, then
XSD (spec for the basics, SCXML for identity, 1.1 and typed data), then
the SCXML project, which draws its machine with the SVG chapter's module.
Static analysis (call chains) comes before dynamic (traces) because that
is the story: the code diff predicts the change, the traces confirm it
and say how often. Renamed: `xquery-grouping-windows.md` to
`xquery-grouping.md` (windows move to the traces project);
`xslt-streaming.md` to `traces-streaming.md`. New: `xslt-project.md`,
`svg*.md`, `scxml*.md`, `call-chains*.md`, `traces*.md`.

## 5. The chapters

For each file: the project directory (its `examples:` comment), what it
teaches in order, the main files it adds, and the packet. "Seed" means
the files of section 2.

### 1. Getting Started (book-6) — `examples: getting-started`

Stays tiny and self-contained: `getting-started/draft.xml`, a ten-line
cut of the draft (two sections, three requirements); `draft.xsd`;
`broken.xml`; `invalid.xml` (a `bcp14` of "MIGHT" and a missing `name`);
`requirements.xq`; `outline.xsl`. installation.md is unchanged except
its "Following along" paragraph. hello-xpath.md: strings, `//t`,
`string()`, predicates (`//t[bcp14 = "MUST"]`), `count`, `@anchor`, `!`,
the two errors. the-sxml-command.md: check, validate, xquery, transform,
serialize, exit status, as today. Replaces books.xml and friends.

### 2. Finding Things with XPath (book-6) — `examples: spec`

- xpath.md: what an expression is; the draft as a tree (front, middle,
  sections, mixed content of `t`).
- xpath-paths.md: steps from the top (`/rfc/middle/section/name`),
  predicates (`section[@anchor = "lockout"]`), positions (`section[2]`,
  `(//t[bcp14])[last()]`), axes (ancestor::section for "which section is
  this requirement in", following-sibling, preceding, the section number
  as `count(preceding-sibling::section) + 1` per ancestor), kinds of
  nodes (text(), comment, processing-instruction, the `cref`), combining
  (`|`, `except`: requirements outside Security), the value of a node
  (`string(t)` across `xref` and `bcp14`).
- xpath-values.md: literals, arithmetic (counting requirements per
  keyword), untyped values (`@year` of references: RFC 2119 1997),
  general vs value comparison (`bcp14 = ("MUST", "SHALL")`), dates
  (`xs:date` from `date/@year/@month/@day` with a month map; the age of
  each reference; durations between -00 and -01: `doc("latch-00.xml")`),
  effective boolean values, sequence types.
- xpath-functions.md: strings (normalize-space of paragraphs, the
  docName's version with `substring-after`/`tokenize`), regular
  expressions (finding BCP 14 words written *without* `bcp14` markup:
  `matches(., "\b(MUST|SHOULD|MAY)\b")` in text nodes, a classic draft
  bug), numbers (format-number of percentages per keyword), sequences
  (distinct-values of keywords, index-of), the arrow operator, conditions
  (`if`, `otherwise`), variables and loops (`let`, `for` in XPath),
  namespaces of functions.
- xpath-maps-arrays.md: maps (anchor to title: the lookup table for
  xrefs), maps from documents (`map:merge(//*[@anchor] ! {@anchor: …})`),
  arrays (the authors of each reference as arrays), JSON (the requirement
  list serialized with method json and read back with parse-json),
  records (`record(anchor, keyword, section)`).
- xpath-higher-order.md: inline functions, named references, partial
  application, sort-by (requirements by keyword strength: a map from
  keyword to rank), filter/for-each/fold-left (the requirement count per
  section), functions that make functions (a checker per keyword), what
  XPath can't do alone (points to XQuery).

Deletes `examples/xpath/` with its last use.

### 3. Writing Programs with XQuery (book-7) — `examples: spec`

- xquery.md: running a query file against the draft; comments; the
  chapter's plan.
- xquery-flwor.md: for/where/order by/return (the requirements index),
  let, positions (`at`), joins (xrefs to terms joined with the `dt`s;
  requirements of -00 with those of -01 by anchor), group by (first
  look), count, while (the requirements before the first SHOULD), more
  clauses.
- xquery-grouping.md: grouping by a computed key (the section of each
  requirement), several keys (section and keyword), after the grouping
  (counts, string-join), choosing a key well. Ends with one paragraph
  that points to windows in [Who Calls Whom, and How
  Often](traces-cardinality.md).
- xquery-constructors.md: direct constructors (an XHTML index of
  requirements), enclosed expressions, computed constructors (an element
  named after the keyword), namespaces (the XHTML namespace, the default
  element namespace), not only XML (text, comments, processing
  instructions; maps).
- xquery-types-errors.md: switch (keyword to strength), typeswitch (block
  kinds `t`, `dl`, `sourcecode`), saying what you expect (`as`), naming
  types (`declare type keyword as enum(…)`), kinds of errors, try/catch
  (a check that reports every xref to a missing anchor without stopping),
  raising errors (`error(xs:QName("spec:dangling"), …)`), errors on
  purpose (finally).
- xquery-modules.md: `spec.xqm` (namespace `urn:example:spec`):
  `spec:requirements($doc)`, `spec:section-number($section)`,
  `spec:keywords($t)`, `spec:anchor-map($doc)`; library modules, external
  variables (`$draft`), typed functions catching mistakes, private
  functions and module variables, functions as values, a failed import.
- xquery-inputs.md: the context item, `doc()`, many documents (both drafts
  and, after cli-3, `collection()` over a `drafts/` subdirectory book-7
  adds), external variables (`--param=draft=latch-00.xml`). **Order:
  after cli-3's commit, which edits this file today.**
- xquery-serialization.md: output methods; the requirements index as
  XML, XHTML, HTML (`method=html`), JSON (an array of records), CSV
  (anchor, section, keyword, text), text; serialize() inside a query;
  saving with `--output`.
- xquery-updates.md: on draft -01: give the unanchored SHOULD an anchor
  (`insert node attribute anchor {…}`) and remove issue-2's `cref`;
  add a requirement after req-8 (`insert node … after`); rename a term
  everywhere (`dt`, every `xref` to it and its text: one pending update
  list); move a section and see that nothing needs renumbering; renumber
  requirement anchors in document order and every xref with them (and
  the warning: other documents, such as lock.scxml, refer to anchors);
  copy-modify, `transform with`; updating functions; the errors
  XUST0001, XUTY0005, XUDY0015; fn:put of draft -02.
- xquery-4.md: as today, examples over the draft.
- xquery-project.md, *Project: Editing a Specification Without
  Rewriting It*: `edit.xqm` (updating functions: add-requirement,
  rename-term, set-keyword, add-reference), `check.xq` (dangling xrefs,
  unused terms and references, BCP 14 words without markup, requirements
  without anchors; exit with an error code), `changes.xq` (the
  requirement diff -00 to -01: added, removed, changed text, changed
  keyword; as text and JSON), and an edit session: three commands that
  make -02 with `--output`, checked, diffed. The point the owner made:
  each edit touches only the nodes it names, so a large draft is never
  rewritten by hand or by a language model.

Deletes `examples/xquery/` and `examples/xquery-project/`.

### 4. Transforming with XSLT (book-8) — `examples: spec`

- xslt.md: built-in rules (the draft's text falls out), copying with
  changes (an identity transform that drops `cref`s for publication).
- xslt-templates.md: template rules for the draft to HTML (`section` to
  `section` with `h2`, `t`, `xref` to links, `bcp14` to `<span
  class="bcp14">`), text value templates, priorities (a requirement `t`
  vs a plain `t`), modes (`toc` for the table of contents; `text` for
  RFC-style plain text with 72-column lines and `[RFC2119]` references),
  starting elsewhere (`--initial-template`, `--initial-mode`).
- xslt-keys-grouping.md: keys (`xsl:key name="anchor"`: xref text
  computed from the target: "Section 4", "[RFC5234]", the term), numbers
  (`xsl:number level="multiple"` for section numbers, requirement
  numbers in document order), grouping (the term index grouped by first
  letter; references by normative/informative), sorting (references by
  anchor, with a collation pointer to Chapter 13).
- xslt-packages.md: many outputs (one HTML page per top-level section
  with `xsl:result-document` and `--result-directory`), importing
  (`common.xsl` with the numbering and xref text, imported by
  `html.xsl` and `text.xsl`), functions (`spec:number`, `spec:xref-text`),
  packages (`spec.xsl` as a package used by both), transformations from
  queries (`fn:transform` from the XQuery index). One sentence says
  streaming is taught in [Streaming Large Trace
  Files](traces-streaming.md).
- xslt-project.md, *Project: Publishing the Specification*: one script
  (`publish.sh`): validate, check.xq from Chapter 3 (symlink or a copy
  named `publish-check.xq`; book-8 must not depend on book-7's files
  unless book-7 has committed them: use only seed files and its own),
  HTML with the toc and the requirements index, the plain-text draft,
  a JSON requirements file for tools; and a "what changed" HTML page
  from both drafts (requirements marked added and changed).

Deletes `examples/xslt/` and moves streaming (below).

### 5. Project: Drawing Diagrams with SVG (book-10) — `examples: svg`

- svg.md: what SVG is (an XML vocabulary in its namespace; boxes, text,
  lines, markers, style), `diagram.xml` and `diagram.svg` side by side,
  the plan.
- svg-layout.md: the layout in XQuery: ranks by breadth-first distance as
  a fold over passes or a `while` clause (maps id to rank), rows with
  `group by` into arrays, positions as a map of records (`declare record
  place(x, y)`), the widest row, centring: maps, arrays, records,
  higher-order functions used for real.
- svg-xquery.md: writing SVG with constructors: `declare default element
  namespace`, attributes from expressions, `{{`/`}}` in the style text,
  computed constructors for the shape by kind, output declarations
  (indent), `--output=diagram.svg` and `diff` against the committed file;
  `draw.xqm` (namespace `urn:example:draw`,
  `draw:svg($diagram as element(diagram)) as element()`, honouring the
  `class` attributes) is the reusable module other chapters link to. It
  must be committed early: Chapters 7, 11 and 12 use it.
- svg-xslt.md: the same drawing with XSLT from a laid-out diagram (the
  XQuery adds x and y): template rules per element, a mode per layer
  (`edges`, `nodes`, `labels`), `xsl:next-match` for stores,
  attribute value templates, `xpath-default-namespace` for matching SVG.
- svg-transform.md: transforming SVG files, any SVG: a dark theme
  (replace the `style`, identity with `on-no-match="shallow-copy"`),
  highlighting one node and its edges (a parameter), removing ids and
  metadata, listing the text of a drawing, scaling with `viewBox`;
  namespaces in patterns.

### 6. Validating with XSD (book-9)

- xsd.md (`spec`): the schema for the draft, validating both drafts,
  rules across values (the docName assertion), strict and lax, DTDs
  (xml2rfc's own DTD exists: one sentence; a two-line internal subset).
- xsd-simple-types.md (`spec`): built-in types (gYear, positiveInteger),
  restriction with pattern, enumeration and range facets (draft-name,
  keyword, day), lists and unions (fragment schemas the packet adds:
  `levels.xsd` for a list of keywords, a union of `day` and a token), what
  validation does to values (whitespace in `bcp14`, typed values), facets
  that don't fit.
- xsd-complex-types.md (`spec`): the schema read top-down; element
  declarations, sequence/choice/all (`front`, a section's blocks), attributes
  (use, fixed version), four kinds of content (empty `date`, simple
  `title`, element-only `front`, mixed `t`), mistakes in content, groups.
- xsd-derivation.md (`spec`): extension (`title` and `sourcecode`
  extend xs:string; a `requirement` type extending `text` with a required
  anchor), restriction, xsi:type, substitution groups (block elements as
  a `block` group: `t`, `dl`, `sourcecode`), nil (an empty `day`), block
  and final.
- xsd-namespaces.md (`scxml`): a target namespace (SCXML's), namespace
  mistakes, `xs:import` of latch-data.xsd, `##other` wildcards and lax
  processing (the XHTML inside states is not validated, the latch
  attributes are), schema hints (`--use-schema-location`), namespaces in
  queries.
- xsd-identity.md (`scxml`): state ids as `xs:key`, targets and initial
  as `xs:keyref`, event names as `xs:unique`, paths in constraints
  (namespaced selectors), keys or IDs (and why latch.xsd uses
  `xs:unique`: most paragraphs have no anchor), broken copies the packet
  adds (`bad-target.scxml`, `duplicate-event.scxml`).
- xsd-11.md (`scxml`): assertions on values and on elements (the status
  payload, `data`'s one-of rule, a transition's event/cond/target),
  conditional type assignment (`xs:alternative` on `latch:event` by
  payload), open content, default attributes, xs:override (SCXML's full
  rule: a target list, `xs:list` of NCName, and what keyref then cannot
  check), smaller changes.
- xsd-errors.md (`spec`): the parts of a message, errors in the schema,
  errors in documents (the broken drafts the packet adds:
  `bad-keyword.xml`, `bad-anchor.xml`, `bad-xref.xml`), after an element
  that is not allowed, not well-formed, lax, `--format=json`.
- xsd-typed-data.md (`scxml`): typed values in XQuery (`import schema`,
  `validate`, `data(@latch:type)` as xs:QName, `@expr` as xs:string and
  the FORG0006 it gives in `sum`), values of your own types
  (`latch:seconds`), the `xsi:type` check of each data value, elements
  without a typed value, types in signatures (`schema-element(latch:status)`),
  typed copies, schema-aware XSLT, from C and JavaScript (pointers).

Deletes `examples/xsd/`.

### 7. Project: Self-Describing State Machines (book-9) — `examples: scxml`

- scxml.md: SCXML in one page (states, transitions, events, data model),
  the lock machine, its annotations (events with payload types, typed
  data, XHTML, `latch:implements`), validating it with both schemas.
- scxml-checks.md: what the schema cannot check, as a schema-aware query
  `check.xq`: unreachable states (the closure of targets from `initial`,
  a `while` clause or fold), events handled but not declared, declared
  but unused (counting `send` and `raise`), payload names that name no
  element declaration, data values against their types (`xsi:type` and
  `validate` in try/catch), requirements implemented: `latch:implements`
  anchors that the draft does not have, and the draft's MUST requirements
  no state implements (`scxml/latch.xml` symlink to the seed). The four
  flaws of the seed are found; a fixed `lock-fixed.scxml` is made with an
  update (`transition with`) and checked clean.
- scxml-xhtml.md: an XSLT `document.xsl` writing an XHTML page: the
  states with their XHTML copied through (`xsl:copy-of` with
  namespaces), a table of transitions, the events with their payload
  described from latch-data.xsd (`doc("latch-data.xsd")`: element
  children, types, facets as words, `xs:documentation`), links to the
  draft's requirements (`latch.html#req-7`). method xhtml.
- scxml-drawing.md: the machine as `diagram.xml` (states as nodes,
  transitions as edges labelled with events; cycles, hence breadth-first
  ranks) drawn with `../svg/draw.xqm` (symlink). **Order: after book-10
  has committed `svg/draw.xqm`.**

### 8. Parsing Text with Invisible XML (book-8 for one section)

Unchanged, except ixml-tasks.md gains a task "The grammar in a
specification": the ABNF of `latch.xml` (symlink `ixml/latch.xml`)
extracted with XPath and parsed with a small iXML grammar for ABNF rules
(rule names, alternatives, repetitions), listing the rules each rule
uses and the undefined ones (SP, CRLF, ALPHA, DIGIT come from RFC 5234's
core rules). The performance chapter's numbers stay tied to
`tests/bench/run-ixml-bench.py --check` and are not touched.

### 9. JSON, CSV and HTML (book-12) — `examples: traces`

JSON with `json-doc("before.json")`: the lookup operator through
resourceSpans, scopeSpans and spans, `?*`, a filter on `?kind = 2`,
numbers become doubles and why OTLP writes times and int64 values as
strings (xs:integer of `startTimeUnixNano`), `json-to-xml` of an OTLP
file and `xml-to-json`; CSV: the spans as CSV for a spreadsheet
(`method=csv`) and `parse-csv` of a CSV a tracing UI exports (a small
`spans.csv` the packet adds); HTML: `parse-html` of a status page
(`status.html`, added). The iXML section becomes one paragraph pointing
to Chapter 8.

### 10. Source Code as XML (book-11)

Unchanged (its small examples are about the bridge, not the library
data); one sentence at the end points to the call-chain project.

### 11. Project: A Diff of Call Chains (book-11) — `examples: call-chains`

- call-chains.md: the lock service, the change in prose, the two
  directories, the plan.
- call-chains-graph.md: functions (`function_declaration`, methods, and
  arrows assigned to consts), calls (`call_expression` and its `function`
  field), resolving callees: namespace imports (`registry.getDoor` to
  module registry), named imports (`listDoors` from ./doors.ts), local
  functions, and everything else as external (`fetch`, `JSON.parse`,
  `doors.push` is a method of a local, filtered); the graph as XML
  (`<call from="doors.listDoors" to="registry.getDoor" line="20"
  repeated="true"/>`, `repeated` when an ancestor is a loop); with
  `--allow-file-access` and `file:list` over `before/` and `after/`.
- call-chains-diff.md: chains from the entry points (`server.route`,
  `server.start`) to the leaves, made without recursion by a loop over
  a worklist (a `while` clause or fold) and with recursion (both
  correct; the book can show a recursive function here), the diff of
  edges and of chains (added, removed, now repeated), a text report and
  an HTML report, and the diff as `diagram.xml` with classes drawn by
  `../svg/draw.xqm` (symlink; **after book-10 commits it**). The report
  ends with the prediction the traces will check: one call to the
  registry becomes one plus one per door.

### 12. Project: Comparing Traces Before and After a Change (book-12) — `examples: traces`

- traces.md: what a trace, a span, a resource and a scope are; OTLP JSON;
  the two files; the plan.
- traces-tree.md: flattening resourceSpans into span records (`for
  member`, `declare record span`), a map from span id to span, the tree
  from parent ids, durations from nanosecond strings, the tree as XML
  (`<span service="…" name="…" duration="…">` nested): the normal form
  the later sections and the streaming section use (`trace.xqm`).
- traces-cardinality.md: participants from client spans (the server
  span's service, else server.address), grouping by (route, caller,
  callee), counts as a map; **windows** (taught here, all of the old
  section): tumbling windows over a span's children in start order to
  find runs of the same call (the N+1 as a run of four `GET
  /doors/{id}`), windows with an end, sliding windows (calls in any
  20 ms), choosing between them.
- traces-diff.md: the diff of two cardinality maps (added and removed
  participants, changed counts, the service versions), matching traces by
  root route, errors when files share no route (a typed error); the
  output above, as text and JSON.
- traces-rendering.md: an HTML report (XSLT over the diff as XML: a
  table with added/removed rows and the counts) and the participants as
  a `diagram.xml` with `added`/`removed`/`changed` classes, drawn with
  `../svg/draw.xqm` (symlink; **after book-10 commits it**).
- traces-streaming.md: all of the old xslt-streaming.md, on traces: a
  query writes a large trace archive in the normal form (`make-traces.xq
  --param=count=100000`), a streamable mode counting spans per service,
  what can stream (the XTSE3430 example), accumulators (count per
  participant and the slowest span), `xsl:iterate` with `xsl:break` (the
  first trace slower than a threshold), more streaming (source-document,
  fork, merge of two archives by time), memory numbers measured again.
  Deletes `examples/xslt-streaming/`.

### 13. Working with Text (book-8) — `examples: spec`

Collations (author and reference names, M'Raihi, sorted with the UCA
collation; terms), full-text search over requirements ("lock" with
stemming finds "locks" and "locked"; phrases; `ftand`; scores), dates in
other languages (the draft's date with `format-date` in German and
French, and the date built from `year`, `month`, `day`), numbers in words
(`format-integer(3, "w")` checks that the text says "three"). Deletes
`examples/text/`.

### 14. Files and the Outside World (book-8) — `examples: spec`

Resolvers; files and the sxml command (`doc("latch-00.xml")`, missing
files, no network); external entities and DTDs as xml2rfc authors use
them: `latch-entities.xml` whose references come from
`bibxml/reference.RFC.2119.xml` and friends through external entities
(`trust-external`), and a DTD default; writing files; EXPath File over
`bibxml/` (never the directory root); EXPath Binary (the first bytes of a
file, a hex dump of the UTF-8 BOM check). Deletes `examples/files/`.

### 15. Embedding sxml in C (book-8) — `examples: c`

The same sections; the programs read `latch.xml` (symlink `c/latch.xml`)
and print requirements by keyword; compile once and run for both drafts;
resources and XQuery (the resolver serves the draft from memory);
errors; memory; features; threads. File names change (`requirements.c`
for `books.c`).

### 16. sxml in JavaScript (book-12) — `examples: javascript`, requires javascript

The same sections with `traces/before.json` (symlink) and the draft
(symlink): processors and values, XQuery over OTLP JSON, XSLT and XSD on
the draft, resources and errors, code languages unchanged, large outputs
and inputs with a generated trace archive.

### 17. Performance and Memory (book-12) — `examples: traces`

Build a release; compile once; lazy sequences (the first slow span of a
large archive); look up instead of searching: parent lookup by scanning
all spans vs a map by span id (the old slow-join/fast-join, now
`slow-tree.xq` and `fast-tree.xq` over `make-traces.xq`'s output);
memory; the step limit; habits. Deletes `examples/performance/`.

### 18. Project: Generating Code from a Schema — unchanged

Owned by nobody in this rewrite; book-13 only fixes "Chapter N" texts in
it.

## 6. Coverage: every feature the book teaches today, and where it goes

| Today (file: topics) | New place |
| --- | --- |
| getting-started, installation, hello-xpath, the-sxml-command: install, first expressions, check, validate, xquery, transform, serialize, exit status | Chapter 1, same files, tiny draft |
| xpath.md: expressions, the tree | xpath.md (spec) |
| xpath-paths: steps, predicates, positions, axes, node kinds, union/intersect/except, node values | xpath-paths.md (spec) |
| xpath-values: literals, arithmetic, untyped, comparisons, dates/durations, EBV, sequence types | xpath-values.md (spec) |
| xpath-functions: strings, regex, numbers, sequences, arrow, conditions, variables/loops, function namespaces | xpath-functions.md (spec) |
| xpath-maps-arrays: maps, maps from documents, arrays, JSON, records | xpath-maps-arrays.md (spec); JSON input in depth: formats.md (traces) |
| xpath-higher-order: inline, references, partial application, HOFs, function factories | xpath-higher-order.md (spec); used again in svg-layout.md |
| xquery.md: running, comments | xquery.md (spec) |
| xquery-flwor: for/where/order/return, let, at, joins, group by, count, while | xquery-flwor.md (spec) |
| xquery-grouping-windows: computed keys, several keys, after grouping | xquery-grouping.md (spec) |
| xquery-grouping-windows: tumbling, windows with an end, sliding, choosing | traces-cardinality.md (traces) |
| xquery-constructors: direct, enclosed, computed, namespaces, other node kinds | xquery-constructors.md (spec, XHTML); in depth with the SVG namespace: svg-xquery.md |
| xquery-types-errors: switch, typeswitch, as, declare type/enum, error kinds, try/catch, error(), finally | xquery-types-errors.md (spec) |
| xquery-modules: functions, library modules, external variables, typing, private, function values, failed import | xquery-modules.md (spec, spec.xqm) |
| xquery-inputs: context item, doc, many documents, external variables (and collections, cli-3) | xquery-inputs.md (spec, both drafts) |
| xquery-serialization: methods, declarations, XML, HTML, JSON, CSV, serialize(), --output | xquery-serialization.md (spec) |
| xquery-updates: five updates, copy-modify, transform with, updating functions, XUST0001/XUTY0005/XUDY0015, fn:put | xquery-updates.md (spec: targeted edits) |
| xquery-4: syntax, pipelines, string templates, destructuring, clauses, types, functions | xquery-4.md (spec) |
| xquery-project: library module, HTML, JSON, fn:put per member | xquery-project.md (spec: edit toolkit, check, draft diff); HTML report: xslt-project.md |
| xslt.md: built-in rules, identity with changes | xslt.md (spec) |
| xslt-templates: rules, TVTs, priorities, modes, initial template/mode | xslt-templates.md (spec); svg-xslt.md, svg-transform.md |
| xslt-keys-grouping: keys, for-each-group, positional grouping, sort, xsl:number | xslt-keys-grouping.md (spec) |
| xslt-packages: result documents, import, xsl:function, packages, fn:transform | xslt-packages.md (spec) |
| xslt-streaming: streamable modes, XTSE3430, accumulators, iterate/break, source-document/fork/merge | traces-streaming.md (traces) |
| xsd.md: a first schema, validating, assertions across values, strict/lax, DTDs | xsd.md (spec) |
| xsd-simple-types: built-ins, facets, lists, unions, value effects, bad facets | xsd-simple-types.md (spec) |
| xsd-complex-types: declarations, sequence/choice/all, attributes, content kinds, errors, groups | xsd-complex-types.md (spec) |
| xsd-derivation: extension, restriction, xsi:type, substitution groups, nil, block/final | xsd-derivation.md (spec) |
| xsd-namespaces: target namespace, mistakes, include/import, schema hints, namespaces in queries | xsd-namespaces.md (scxml) |
| xsd-identity: key/keyref/unique, selector paths, keys vs IDs | xsd-identity.md (scxml) |
| xsd-11: assertions, CTA, open content, default attributes, override, smaller changes | xsd-11.md (scxml) |
| xsd-errors: message parts, schema errors, document errors, recovery, not well-formed, lax, JSON reports | xsd-errors.md (spec) |
| xsd-typed-data: typed values, validate, own types, untyped elements, signatures, typed copies, schema-aware XSLT, C/JS | xsd-typed-data.md (scxml); applied in scxml-checks.md |
| ixml (five files) | unchanged, plus the ABNF task in ixml-tasks.md |
| formats: JSON, json-to-xml, CSV, HTML, iXML pointer | formats.md (traces) |
| source-code: the bridge, highlights, positions, languages, injections, errors, own languages | source-code.md unchanged; call graphs and diffs: call-chains*.md |
| text: collations, full text, dates in languages, numbers in words | text.md (spec) |
| files: resolvers, local files, entities/DTDs, writing, EXPath File, Binary | files.md (spec, bibxml entities) |
| c: building, first program, compile once, resources, errors, memory, features, threads | c.md (spec) |
| javascript: package, values, languages, resources, code languages, streams, memory | javascript.md (traces, spec) |
| performance: release, compile once, laziness, lookups, memory, step limit, habits | performance.md (traces) |
| codegen (five files) | unchanged |

New teaching that no chapter has today: SVG and its namespace, a layout
algorithm, SCXML, XHTML generation with copied foreign content, xml2rfc,
OTLP and semantic conventions, call graphs, diffs of graphs.

## 7. Packets

Seven rewrite packets run in parallel; each owns its chapter files and
the files it adds; book-13 runs after all of them.

| Packet | Chapter files | Example files | Deletes | Order |
| --- | --- | --- | --- | --- |
| book-6 Getting started and XPath | getting-started.md, installation.md, hello-xpath.md, the-sxml-command.md, xpath.md, xpath-paths.md, xpath-values.md, xpath-functions.md, xpath-maps-arrays.md, xpath-higher-order.md | getting-started/* (rewritten); new files in spec/ named `xpath-*` if any | examples/xpath | none |
| book-7 XQuery | xquery.md, xquery-flwor.md, xquery-grouping.md (from xquery-grouping-windows.md), xquery-constructors.md, xquery-types-errors.md, xquery-modules.md, xquery-inputs.md, xquery-serialization.md, xquery-updates.md, xquery-4.md, xquery-project.md | spec/: spec.xqm, edit.xqm, check.xq, changes.xq, index.xq, *.xq of its sections, drafts/ | examples/xquery, examples/xquery-project | xquery-inputs.md after cli-3 |
| book-8 XSLT, text, files, C, ABNF | xslt.md, xslt-templates.md, xslt-keys-grouping.md, xslt-packages.md, xslt-project.md (new), text.md, files.md, c.md, ixml-tasks.md (one section) | spec/: *.xsl, publish.sh, publish-check.xq, text-*.xq, latch-entities.xml, bibxml/; c/* (rewritten, latch.xml symlink); ixml/latch.xml symlink, ixml/abnf.ixml | examples/xslt, examples/text, examples/files | none |
| book-9 XSD and SCXML | xsd.md, xsd-simple-types.md, xsd-complex-types.md, xsd-derivation.md, xsd-namespaces.md, xsd-identity.md, xsd-11.md, xsd-errors.md, xsd-typed-data.md, scxml.md, scxml-checks.md, scxml-xhtml.md, scxml-drawing.md (new) | spec/: bad-*.xml, levels.xsd and other fragment schemas named `xsd-*` or `bad-*`; scxml/: everything new (check.xq, document.xsl, lock-fixed.scxml, bad-*.scxml, latch.xml symlink, draw.xqm symlink, machine-diagram.xq) | examples/xsd | scxml-drawing.md after book-10's draw.xqm |
| book-10 SVG | svg.md, svg-layout.md, svg-xquery.md, svg-xslt.md, svg-transform.md (new) | svg/: layout.xq, draw.xqm, draw.xq, layout.xsl, draw.xsl, dark.xsl, highlight.xsl, …; may regenerate diagram.svg | none | commit draw.xqm first |
| book-11 Call chains | call-chains.md, call-chains-graph.md, call-chains-diff.md (new), source-code.md (one sentence) | call-chains/: graph.xqm, graph.xq, chains.xq, diff.xq, report.xsl, draw.xqm symlink | none | the drawing after book-10's draw.xqm |
| book-12 Traces, formats, JavaScript, performance | traces.md, traces-tree.md, traces-cardinality.md, traces-diff.md, traces-rendering.md, traces-streaming.md (new; xslt-streaming.md removed), formats.md, javascript.md, performance.md | traces/: trace.xqm, *.xq, *.xsl, make-traces.xq, spans.csv, status.html, draw.xqm symlink; javascript/* (symlinks to before.json and latch.xml) | examples/xslt-streaming, examples/formats, examples/performance, docs/book/src/xslt-streaming.md | rendering after book-10's draw.xqm |
| book-13 Integration (after all) | SUMMARY.md (section 4), appendix-cli.md (after cli-3), introduction.md (the Latch story, the projects, one directory per project), foreword.md (if needed), appendix-glossary.md (terms the packets report), appendix-differences.md, every "Chapter N" text, codegen*.md references only | removes orphaned example directories | — | after book-6 to book-12 |

**book-13 done when**: `tools/check-book-examples.sh` on a release build
of HEAD passes for the whole book (with SXML_JAVASCRIPT for Chapters 16
and 18's Node section), `mdbook build docs/book` succeeds with no
missing file, `grep -rn "Chapter [0-9]" docs/book/src` shows only
correct numbers, and no example directory is unused.

Every rewrite packet is **done when** its chapter files pass the check on
a release build of HEAD, it deleted the directories it replaces, and its
report lists new glossary terms and "Chapter N" texts it left for book-13.

## Proposed data changes

(Packets add requests here, one line each, for book-13 to decide. This
file is a shared ledger: stage only your hunk and commit the index
without a pathspec, as BRIEF.md says.)
