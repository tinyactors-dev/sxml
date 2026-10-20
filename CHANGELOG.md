# Changelog

All notable changes to sxml. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0.0, a minor version may change or remove a declaration, and a
decision record (`docs/DECISIONS.md`) always says so. The steps for a
release are in `docs/RELEASE.md`.

## [Unreleased]

XQuery 4.0 (Phase 3, D161), XSLT 4.0 (Phase 4, D169), XSD 1.1 with
schema awareness (Phase 5, D182) and the deferred features of Phase 7
(D183, D237). The owner decides whether this goes into 0.2.0, which is
not tagged yet, or into the next minor release.

### Added (Phase 7)

- **Invisible XML for large inputs** (D290): regular rules whose XML is
  their text are compiled with the grammar into automata and read in
  one step, item sets are made only where items are, the input is read
  as UTF-8 in place, and the parse tree keeps only what the XML shows:
  a 100 MB log parses in about 6 s and 1.2 GB (the log needs about
  10 MB of memory for each MB, most of it the XML tree), within the
  default step limit.

- **Fast Invisible XML** (D283): the Earley parser of `fn:invisible-xml`
  uses one character of lookahead, Leo items for right recursion and
  compiled codepoint classes, and simplifies grammars when compiling, so
  unambiguous grammars parse in linear time (1 MB in under 0.6 s);
  grammar errors give line and column or name the rule; failure
  documents list expected sets once, sorted. The book has a part on
  Invisible XML.
- **Source code as XML** (D285): `sxml:parse-code` (namespace
  `urn:sxml`, optional feature `sxml_source_code_feature()`) parses JSON,
  JavaScript (with JSX), Python, C, CSS, HTML and Go with vendored
  tree-sitter grammars into documents whose elements are the named
  nodes of the syntax tree, with `field` and `highlight` attributes from
  each grammar's highlights query, and `sxml:code-languages()`. The
  `sxml` command binds the prefix `sxml`. `name.h` gains
  `SXML_NAMESPACE_SXML`.
- **More source code languages** (D288): TypeScript, TSX, Markdown and
  its inline grammar; the locals query (references take their
  definition's highlight) and the injections query (code inside code is
  parsed with its own language, option `injections`). Hosts register
  their own tree-sitter languages (`sxml_source_code_language`,
  `sxml_processor_configuration.source_code_languages`,
  `sxml_source_code_check_language`; tree-sitter language ABI 13 to 15),
  and `sxml xpath`, `xquery` and `transform` load one from a shared
  library with `--code-language=LIBRARY`.
- **Optional features** (`processor.h`, D189): a processor names the
  features it uses (`sxml_optional_feature`), so a program links only
  those; without one, the feature's single rejection place gives the
  specification's error (D162). `sxml_all_optional_features()` names
  them all. Large data comes from the host through the resolver (D184):
  the DUCET and CLDR files, TZif zones, Full Text stop words and
  thesauri.
- **Streaming XSLT** (D212, D219, D223, D228, D231, D233, D236):
  streamability analysis with XTSE3430, streamed xsl:source-document,
  streamable modes and template rules, accumulators, xsl:iterate,
  xsl:fork, xsl:merge over streamed inputs, capture="yes", a streamed
  principal source and result (`xslt.h`), and a byte reader resource
  form (`processor.h`): `sxml transform` sums 1,000,000 orders in
  11 MB. supports-streaming is true.
- **The XQuery Update Facility 3.0** and the 4.0 draft's additions
  (D187, D205): every updating expression, copy-modify, fn:put and
  revalidation, through an update sink (`xquery.h`); `sxml xquery
  --allow-put`.
- **XQuery and XPath Full Text 3.0** (D214, D235): every selection,
  match option and positional filter, Snowball stemmers for en, de, fr,
  it, es and nl, stop words, thesauri and scores.
- **fn:parse-html and fn:html-doc** (D201, D218): the WHATWG tokenizer
  and tree construction, encoding sniffing and every legacy decoder.
- **fn:invisible-xml** (D215) over an Earley parser.
- **fn:transform** (D203) and **xsl:evaluate** (D196).
- **EXPath Binary** (D198) and **EXPath File** (D200, a file system in
  `processor.h`; `sxml xpath` and `sxml xquery --allow-file-access`).
- **UCA collations** with CLDR tailorings (D213, D220); **IANA time
  zones** for fn:civil-timezone and the formatting functions' places
  (D209); **languages and calendars** in the formatting functions:
  names, ordinals and spell-out for en, de, fr, it, es, nl; the BE, JE,
  OS and AH calendars (D221).
- **XML 1.1 input** (D195) and **DTD processing** (D199, D210, D211): the
  external subset, external entities, attribute defaults and DTD
  validation (an optional feature).
- **XSLT 1.0 behaviour** and XPath 1.0 compatibility mode (`xpath.h`,
  D197, D206), EXSLT common; **disable-output-escaping** (D188).
- **The JavaScript package** compiles XQuery, XSLT and schemas (D216).
  It is one module, `sxml.wasm`, with every language and every optional
  feature but EXPath File (D272); `sxml-full.wasm` is no longer built,
  and the package's `./sxml-full.wasm` export names `sxml.wasm`.
- **fn:load-xquery-module** in XPath and XSLT too (F&O 4.0, 18.2; D272).
- **The sxml command** writes files: `--output`, `--force`,
  `--result-directory`, `--method=csv` (D217).
- Performance (D222, D224, D229, D232): XML parsing in bounded segments
  (100 MB in 98 MB), filter indexes for joins, 25 MB/s parsing.
- Conformance at 9c5b1ba (`docs/CONFORMANCE.md`): XP40 99.5 %, XQ40
  99.5 %, XT40 94.8 % of the in-scope cases; XSD 99.9 %; the iXML suite
  886 of 907, html5lib tree construction 1,749 of 1,753, xmlconf
  2,237 of 2,237.

### Added

- **XSD 1.1 validation** (`schema.h`, D165): schemas compiled once from
  schema documents given as text, as parsed nodes or through the
  resolver, with every document they include, import, redefine or
  override; every XSD 1.0 structure and the XSD 1.1 additions (open
  content, conditional inclusion, assertions, type alternatives,
  override, inheritable attributes), identity constraints, all built-in
  datatypes and facets. The validator is a tree builder filter: the
  parser feeds it while parsing, or `sxml_schema_validate_node`
  validates an existing node into a typed copy. Errors carry the XSD
  constraint name and a description of what failed; a report sink gets
  all of them (D170 to D178).
- **Schema awareness** (D174, D179): in-scope schemas for XPath through
  the static context; XQuery's `import schema` and `validate`; XSLT's
  `xsl:import-schema`, validation and type attributes,
  default-validation, input-type-annotations and typed modes;
  schema-element() and schema-attribute() tests, casts and constructor
  functions for schema types, fn:xsd-validator, and the xsd-validation
  option of fn:parse-xml and fn:document.
- **`sxml validate`** checks documents against a schema, with
  `--mode=lax` and `--format=json` (`docs/CLI.md`, D176).
- **Schema location hints** (D278): `sxml_schema_configuration` gains
  `schema_location_hint_node` (additive; the zero item follows none), so
  the xsi:schemaLocation and xsi:noNamespaceSchemaLocation hints of an
  instance join the assembly through the resolver, for namespaces the
  schema does not have yet. fn:xsd-validator, fn:doc, fn:doc-available,
  fn:parse-xml and XSLT's fn:document honour use-xsi-schema-location
  (it raised FODC0009 before), and `sxml validate --use-schema-location`
  follows each FILE's hints; `--schema` is then optional.
- **XInclude** (D286): the xinclude option of fn:doc, fn:doc-available,
  fn:parse-xml and XSLT's fn:document processes xi:include elements
  (documents, text, fallbacks; with trust-external; xpointer is not
  supported); it raised FODC0013 before. fn:format-integer and
  xsl:number number with the Japanese ideographic numerals (U+4E00).
- XSD conformance (`docs/CONFORMANCE.md`): the W3C XSD test suite in
  scope 99.9 %; schema-aware XQ40 81.5 %, schema-aware XT40 64.6 %.

- **XSLT 4.0** (`xslt.h`): stylesheets compiled once from text or
  through the resolver, with their included and imported modules, used
  packages (xsl:use-package, xsl:override, visibility) and static
  parameters, and run many times on the shared IR and evaluator (D112,
  D157). Template rules, modes and patterns; sequence constructors,
  literal result elements and the node instructions; xsl:choose,
  xsl:iterate, xsl:try, xsl:fork; sorting and grouping
  (xsl:for-each-group, xsl:merge); keys, xsl:number, xsl:analyze-string;
  stylesheet functions with caching; accumulators; whitespace stripping;
  xsl:message and xsl:assert; output definitions, character maps and
  result documents, delivered raw, as trees or serialized to a result
  document handler (a collector is built in). The invocation kinds are
  apply-templates, call-template and call-function. Content streams into
  the constructor it builds (D166). Schema awareness, streaming and
  backwards-compatible behaviour are not supported yet (D162: Phase 5 and
  Phase 7), each refused in one place with the error XSLT 4.0 gives;
  xsl:evaluate raises XTDE3175 (dynamic evaluation is optional).
- **`sxml transform`** applies a stylesheet to a document, or calls an
  initial template, with `--initial-mode`, `--initial-template`,
  `--param` and `--method`; modules and documents are read from local
  files relative to the stylesheet (`docs/CLI.md`).
- XSLT conformance (`docs/CONFORMANCE.md`): XT40 in scope 91.5 %, every
  core test set 88 % or more.

- **XQuery 4.0** (`xquery.h`): main and library modules with their
  prologs (namespaces, defaults, variables, functions, options, decimal
  formats, context value declarations), module import through the
  processor's resolver, cyclic imports, fn:load-xquery-module, FLWOR with
  group by, order by and window clauses, direct and computed node
  constructors, try/catch, switch and typeswitch, output declarations and
  output:parameter-document. Queries compile once and evaluate many
  times on the shared IR and evaluator, as XPath does (D106, D146).
- **Serialization 4.0** for queries: `sxml_xquery_query_get_serialization_parameters`
  gives the parameters the prolog declares.
- **`sxml xquery`** runs a query file or `--expression` with
  `--context-item`, `--param` and `--method`; library modules are read
  from local files (`docs/CLI.md`).
- Conformance (`docs/CONFORMANCE.md`): XQ40 in scope 97.2 %, XQ-only
  98.8 %, serialization methods 433 of 508; XP40 in scope 97.1 %.

### Changed

- `xquery.h` is no longer provisional (D161): it changes only as the
  other public headers do. XQuery's default for omit-xml-declaration is
  still the owner's choice (D146) and may change with a record.
- `xslt.h` is no longer provisional (D169): it changes only as the other
  public headers do.
- `schema.h` is no longer provisional (D182): it changes only as the
  other public headers do.
- XSLT's xsl:import-schema and validation attributes are supported now
  (they raised XTSE1650 and XTSE1660, D162), and fn:system-properties
  says schema-aware.
- **XSLT stylesheets no longer see XPath's predeclared prefixes** (D261):
  as XSLT 4.0 (5.3.1) says, a stylesheet knows the namespaces it declares
  (natively or with fixed-namespaces) and xml, nothing else. A stylesheet
  that writes `xs:integer`, `map:get` or `math:pi()` without declaring
  the prefix now fails with XPST0081 (in an expression or an `as` type)
  or XTSE0280 (in a name attribute such as `mode`); declare it
  (`xmlns:xs="http://www.w3.org/2001/XMLSchema"`) or write
  `fixed-namespaces="#standard"`. XPath and XQuery keep theirs. Casts of
  strings to xs:QName at run time follow the same rule (D262):
  `xs:QName($name)` with `$name` = `'fn:x'` raises FONS0004 unless the
  stylesheet declares fn.
- **QNames in instances resolve with the instance's namespaces only**
  (D271): `xsi:type="xs:string"`, or a value of type xs:QName, in an
  element that does not declare the prefix `xs` is invalid
  (cvc-elt.4.1, cvc-datatype-valid), where XPath's predeclared prefixes
  used to fill in; a defaulted attribute or element value is read with
  the namespaces of the schema document that gives the default.
- `sxml xquery` and `sxml xpath`: `fn:collection` and
  `fn:uri-collection` raise FODC0002 for a relative URI too (it ended
  with exit 4 and `SXML_STATUS_INVALID_ARGUMENT`; D271).
- `sxml xquery`: a dynamic error raised in a library module names the
  module's file, as a static error does (D271).
- `sxml xpath`, `sxml xquery` and `sxml transform` write fn:trace and
  fn:message output to standard error (D271); it was dropped.
- The document of `--context-item` (and the input of `sxml transform`
  and `sxml serialize`) has its file URI as document URI and base URI
  (D271).

## [0.2.0] - unreleased

The first public release (D47, D104, D138). From this version, the public
headers in `include/sxml/` are stable, except the provisional `xquery.h`
and `xslt.h` (D151). `SXML_API_VERSION` is 0.2.0.

### Added

- **XDM 4.0 value model**: items, sequences, nodes, maps, arrays,
  function items and JNodes. They come from a reference-counted item heap,
  and a name pool and a type registry go with it. The memory comes from
  caller-supplied allocators, and no part of sxml recurses in C (D71).
- **XML 1.0 parser**, built on tree-sitter. It parses into any tree
  builder, with namespaces, DTD entities, encodings and bounded entity
  expansion.
- **XPath 4.0**: the full syntax, compiled to a shared intermediate
  representation and run on an evaluator with explicit stacks. Static
  contexts can have namespaces, external variables, decimal formats and
  collations. Dynamic contexts can have a context value, variables,
  available documents, a resource resolver, a clock, a trace sink and
  environment variables.
- **The F&O 4.0 function library**: strings and regular expressions,
  numerics with arbitrary-precision decimals (libmpdec), dates and times
  with formatting, URIs, QNames, hashes, maps, arrays, higher-order
  functions, JSON, CSV, element maps, collations, resources, fn:trace
  and fn:message. Functions D97 lists as non-goals are left out.
- **Serialization 4.0**: the adaptive, XML, XHTML, HTML, text, JSON and CSV
  methods, with fn:serialize's parameter maps and parameter elements.
- **Processor API** (`processor.h`, `xpath.h`, `serialize.h`, `xml.h`) for
  hosts: compile once, evaluate many times, serialize. Resources are read
  only through the host's resolver.
- **`sxml` command-line program**: `sxml check`, `sxml xpath`,
  `sxml xquery`, `sxml serialize` (with `--method`) and `sxml version`
  (`docs/CLI.md`).
- **WebAssembly**: a `wasm` build preset (WASI SDK) that runs the tests
  under Node.js. The JavaScript package `@tinyactors/sxml` (`bindings/javascript/`)
  parses, evaluates XPath with variables and a context, serializes and
  resolves resources in Node.js and in browsers. Its WebAssembly size
  budget is checked by ctest.
- Conformance: the W3C QT4 test suite runs through `sxml-qt4`, and the
  latest results are in `docs/CONFORMANCE.md`.

### Not yet

- XQuery 4.0 (Phase 3): `xquery.h` compiles and runs main modules, and
  `sxml xquery` uses it; library modules, module import and
  fn:load-xquery-module are in progress. `xquery.h` is provisional until
  the Phase 3 gate (D151). (Done since: see Unreleased, D161.)
- XSLT 4.0 (Phase 4): `xslt.h` is declared and partly implemented; it is
  provisional until the Phase 4 gate (D151). Schema validation is
  Phase 5.

[0.2.0]: https://github.com/dhamidi/sxml/releases/tag/v0.2.0
