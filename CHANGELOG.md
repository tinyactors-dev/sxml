# Changelog

All notable changes to sxml. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0.0, a minor version may change or remove a declaration, and a
decision record (`docs/DECISIONS.md`) always says so. The steps for a
release are in `docs/RELEASE.md`.

## [0.2.0] - unreleased

The first public release (D47, D104, D138). From this version, the public
headers in `include/sxml/` are stable. `SXML_API_VERSION` is 0.2.0.

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
- **`sxml` command-line program**: `sxml check`, `sxml xpath` and
  `sxml version` (`docs/CLI.md`).
- **WebAssembly**: a `wasm` build preset (WASI SDK) that runs the tests
  under Node.js. The JavaScript package `sxml-js` (`bindings/javascript/`)
  parses, evaluates XPath with variables and a context, serializes and
  resolves resources in Node.js and in browsers. Its WebAssembly size
  budget is checked by ctest.
- Conformance: the W3C QT4 test suite runs through `sxml-qt4`, and the
  latest results are in `docs/CONFORMANCE.md`.

### Not yet

- XQuery 4.0: `xquery.h` is declared, and its implementation is in
  progress (Phase 3).
- XSLT 4.0 (Phase 4) and schema validation (Phase 5).

[0.2.0]: https://github.com/dhamidi/sxml/releases/tag/v0.2.0
