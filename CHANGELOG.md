# Changelog

All notable changes to sxml. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0.0, a minor version may change or remove a declaration, and a
decision record (`docs/DECISIONS.md`) always says so. The steps for a
release are in `docs/RELEASE.md`.

## [Unreleased]

XQuery 4.0 (Phase 3, D161), XSLT 4.0 (Phase 4, D169) and XSD 1.1 with
schema awareness (Phase 5, D182). The owner decides whether this goes
into 0.2.0, which is not tagged yet, or into the next minor release.

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
  under Node.js. The JavaScript package `sxml-js` (`bindings/javascript/`)
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
