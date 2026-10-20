# Phase 5: XSD 1.1 validation and schema-aware typing

Goal (PLAN.md): XSD 1.1 schemas compiled from schema documents, trees
validated against them into typed trees, and schema awareness in XPath,
XQuery and XSLT (import schema, validate, xsl:import-schema, validation and
type attributes, schema-element() tests); `sxml validate` (below). It
starts alongside the end of Phase 4 (D164) and must not regress the
Phase 4 counts. Design: D165. Rules for parallel packets are those of
`docs/tasks/phase-2.md` (D45, D96, D132: a packet starts when every packet
in its "After" list is committed).

## The design in one page (D165)

- **One public header**, `include/sxml/schema.h`: an `sxml_schema`
  (lifecycle quintet) compiled from schema documents given as text, as
  parsed nodes or through the resource resolver (new kind
  `SXML_RESOURCE_KIND_SCHEMA_DOCUMENT`, `processor.h`); an
  `sxml_schema_validator` (quintet) that **is a tree builder**: it takes
  the events of the tree to validate and passes them on, annotated, to a
  result builder; `sxml_schema_validate_node` for an existing node;
  errors with XSD constraint names as codes and a report sink that
  receives all of them.
- **Validation is a tree builder filter.** The XML parser feeds it while
  parsing (`sxml validate`, `fn:doc` with validation), the tree copier
  feeds it an existing node (XQuery `validate`, XSLT validation), node
  construction can feed it directly. The validated tree is an ordinary
  arena tree: the builder events already carry type annotation, is-id,
  is-idrefs and nilled (`tree.h`); defaults and normalized attribute
  values are just the events the validator sends. No new tree kind.
- **Typed values are derived, not stored**: from the string value and the
  type annotation, in `sxml_node_make_typed_value` (`src/xdm_node.c`), for
  every tree without a `get_typed_value` operation (a contract comment in
  `tree.h` and `xdm.h`).
- **Types live in the one type registry.** Schema compilation registers
  simple and complex type definitions through the internal
  `sxml_type_registry_register_schema_type` (`src/types_internal.h`):
  facets as text in the type description (`types.h`: `sxml_facet`), the
  content type of complex types (`sxml_complex_type`). Casting and facet
  checks are `src/cast.c`'s, patterns the regex engine's XSD dialect (a
  flag, D53). Schema types are found by name through their schema, not
  the registry (`sxml_schema_find_type`).
- **Schema components** are internal (`src/schema_internal.h`, packet
  5.2), index-addressed in the schema's arena as the IR and ASTs are; the
  compile pipeline runs stages (read, resolve, simple types, complex
  types, identity constraints, assertions) that each live in their own
  file, so packets add files, not lines (the 2.0 pattern).
- **XPath is reused** for assertions, type alternatives and identity
  constraints: compiled with the XPath API (`xpath.h`), evaluated by the
  one evaluator over subtrees the validator records.
- **Schema awareness in the languages** replaces the one rejection point
  of each feature (D162): no parallel machinery, schema-element(N) is
  resolved statically into element tests.

## The W3C XSD test suite (D13, D165)

Vendored at `third_party/xsdtests` (w3c/xsdtests
`7bc3365c652a322f3d762021b3879eb92dae7e30`, `third_party/xsdtests.commit`,
`tools/update-test-suites.sh`): 42,174 files, 54 MB; the reports' HTML,
CSS and images (`nistMeta/*.html`, 25 MB) and CVS backup copies are left
out. `suite.xml` lists 92 test sets, plus the introspection set, which
checks the metadata and is not run; `extra-suite.xml` (precisionDecimal,
not in XSD 1.1) is not run either.

**In scope**: tests applicable to an XSD 1.1 processor (the `version`
tokens of testSet, testGroup and test: `1.1`, `XML-1.0`, `XML-1.0-5e`,
`Unicode_6.0.0`, `CTR-all-compile`, `full-xpath-in-CTA`,
`comments-and-PIs-excluded`; a test applies when every token class it
names has a supported token), not `queried`, `disputed-test` or
`disputed-spec`, with an expected outcome of valid or invalid (the one
whose `version` applies, else the unversioned one): **41,538 tests**,
15,330 schema tests and 26,208 instance tests.

| Contributor | In scope | What |
| --- | --- | --- |
| NIST | 23,170 | datatypes: every built-in type and facet |
| Microsoft | 14,363 | structures and datatypes (Regex 3,999, DataTypes 3,429, Particles 1,368, IdentityConstraint 1,058) |
| Sun | 1,606 | structures |
| Saxonica | 1,376 | XSD 1.1: all groups, assertions, CTA, open content, override, wildcards, versioning |
| IBM | 919 | XSD 1.1: assertions, CTA, open content, ID/IDREF, default attributes, date and duration types |
| W3C WG, Oracle, Boeing | 104 | substitution groups, IRIs, time zones |

## Parallelism plan

| Wave | Packets (run together) |
| --- | --- |
| 1 | 5.2 schema components and stages, 5.3 XSD test driver |
| 2 | 5.4 reader and assembly, 5.5 simple types and facets, 5.6 complex types and content models |
| 3 | 5.7 validation and the PSVI |
| 4 | 5.8 identity constraints, 5.9 assertions and type alternatives, 5.10 `sxml validate`, 5.11 schema-aware XPath and XQuery |
| 5 | 5.12 schema-aware XSLT, 5.13 conformance sweeps |
| 6 | 5.14 review (gate) |

Dependencies ("After"): 5.2 and 5.3 after 5.1; 5.4, 5.5, 5.6 after 5.2
(5.5 and 5.6 test with components built by hand until 5.4 reads
documents); 5.7 after 5.4, 5.5, 5.6; 5.8, 5.9, 5.10, 5.11 after 5.7;
5.12 after 5.11; 5.13 after 5.7 (one sweep per area as its packets land);
5.14 after everything. Critical path: 5.2 5.4 5.7 5.11 5.12 5.13 5.14.
"Working XSD" for the owner is the end of 5.10: `sxml validate` checks
documents against a schema with every XSD 1.0 structure.

## Packets

### 5.1 Schema API, substrate additions and plan (design)
After the Phase 3 gate (D164). Owns `include/sxml/schema.h`, the D165
additions to `types.h`, `tree.h`, `xdm.h` and `processor.h`, the
declaration in `src/types_internal.h`, `third_party/xsdtests`, this file.
**Done** 2026-10-18 (D165): the header, the substrate additions (facets
and complex content types in type descriptions, the derived typed value
contract, the SCHEMA_DOCUMENT resource kind), the suite vendored, this
plan.

### 5.2 Schema components and compile stages (design, build)
After 5.1. Owns new `src/schema_internal.h`, stub files
`src/schema_simple_type.c`, `src/schema_complex_type.c`,
`src/schema_identity.c`, `src/schema_assertion.c`, `src/schema_validate.c`.
The component model of XSD 1.1 Part 1, 2.2, index-addressed in one arena
per schema (D24, D52 style): element and attribute declarations (scope,
value constraint, nillable, abstract, substitution group heads, type
table of alternatives, identity constraints, inheritable), attribute
uses and groups, complex types (derivation method, final, block,
abstract, content type: particle tree of model groups and particles,
wildcards, open content, attribute uses and wildcard, assertions,
defaultAttributesApply), simple types (the registered type identifier,
facets with fixed, assertions), model group definitions, identity
constraints (selector, fields, refer; xpathDefaultNamespace), notations;
the name indexes of the schema per symbol space (types, elements,
attributes, groups, attribute groups, identity constraints, notations)
and substitution group members. Each XPath of a component is kept as text
with its namespace context and base URI. The compile pipeline in stages,
each a function of one file (read, resolve, simple types, complex types,
identity constraints, assertions and alternatives) and the validator's
seams (content model matcher, subtree recorder, ID table) declared; the
stub files return `SXML_STATUS_UNSUPPORTED_FEATURE` (or do nothing) until
their packets fill them, as 2.0's evaluator stubs did. Done when the
components of three schemas (a purchase order, a recursive substitution
group, an XSD 1.1 schema with assertions, alternatives and open content)
are written out in comments, and the stubs build. Golden:
`src/xslt_ast_internal.h`, `src/evaluator_machine_internal.h`.
**Done** 2026-10-18 (D167): `src/schema_internal.h` (the component
tables, references, lists, texts, namespace contexts and XPath records;
the built-in components; `struct sxml_schema`; the compilation and its
six stages; the content model matcher, the derivation check, the
recorded subtree, the ID table and the assertion, alternative and
identity seams; the three schemas in text form), `src/schema_components.c`
(the store, the name index, blocks, built-in components, reports) with
`tests/schema_components_test.c`, and the five stubs.

### 5.3 XSD test driver (implementation)
After 5.1. Owns new `tests/xsd/driver/`, `tests/xsd/slice.txt`,
`tests/xsd/slice-baseline.txt`, new `tools/run-xsd.sh`,
`tools/conformance-report.py` (its XSD section), `docs/QT4_CATALOG.md`
(an XSTS section). Program `sxml-xsd-test` on the CLI framework, a
sibling of sxml-qt4 and sxml-xslt-test (D117), linking
`sxml_qt4_driver_library` for reports and documents. Reads
`third_party/xsdtests/suite.xml` and its `.testSet` files (namespace
`http://www.w3.org/XML/2004/xml-schema-test-suite/`): test groups, schema
tests (one or more `schemaDocument`s, compiled as one schema: valid means
it compiles) and instance tests (validated against the group's schema,
strict, with a report sink; an instance test of a group whose schema
fails is a fail), applicability as above. `list` works now; `run`
reports "XSD not implemented" (an unsupported outcome) until 5.4
compiles and 5.7 validates. `tools/run-xsd.sh` imitates
`tools/run-xslt.sh`: one process per test set, reports in
`build/qt4-run/XSD11/`, spec name XSD11 in the dashboard with the
contributor groups as areas. Done when all 41,538 in-scope tests resolve
(documents found, outcome read), a ctest slice of about 150 tests over
every contributor is chosen with a baseline file (0), and the report
JSON matches the QT4 driver's shape.
**Done** 2026-10-18 (D168): `sxml-xsd-test` list and run; the drivers'
common mechanisms shared in the QT4 driver's library (test budget, test
selection, slice script); 41,518 tests in scope (D168 applies the
version tokens per element; D165's 41,538 or-ed them), every one
resolves and is not run, "XSD not implemented"; `tests/xsd/slice.txt`,
147 tests, baseline 0; `tools/run-xsd.sh` (45 s) and the XSD11 dashboard
section.

### 5.4 Schema document reader and assembly (implementation)
After 5.2. Owns `src/schema.c` (the `sxml_schema` quintet and the find
functions), `src/schema_read.c`, `src/schema_assemble.c`,
`src/schema_resolve.c`, new `src/schema_text.c` (the text form of
D167), `tests/schema_read_test.c`, `tests/schema_test.c`.
Schema documents through the XML parser into arena trees (D12; the
reader walks a tree, as `src/xslt_read.c` walks a stylesheet), XML
representation rules of every xs: element and attribute
(src-element, src-attribute, src-ct, src-simple-type, ...), the
annotations skipped, conditional inclusion (vc:, Part 1, 4.2.2),
xs:include (chameleon included documents), xs:import, xs:redefine,
xs:override (4.2.3 to 4.2.5) through the resolver with documents read
once, QName resolution to components (src-resolve, the symbol spaces),
circularity of groups, attribute groups and derivation, the schema
defaults (elementFormDefault, attributeFormDefault, blockDefault,
finalDefault, defaultAttributes, xpathDefaultNamespace). No C recursion:
nested particles and groups use explicit stacks. Done when every schema
document of the suite reads without a crash, leak or limit, and the
in-scope schema tests pass ≥ 85% under the 5.3 driver (most expect a
valid schema; the component constraints of 5.5 and 5.6 raise the rest).
Golden: `src/xslt_read.c`, `src/xquery_module.c` (resolution, read once).
**Done** 2026-10-18 (D170): one table of schema element roles (the schema
for schema documents' attributes and child slots, the component each is
read into), a walk without recursion, conditional inclusion, the
assembly (read once, chameleon inclusion, redefine and override last and
innermost first, the xml: namespace known), RESOLVE (references, base,
item and member types, substitution groups, cycles), the text form with
content models, sxml-xsd-test compiling schemas; schema tests 15,163 of
15,329 (98.9 %), slice 110. Not done: src-redefine 6.2.2 and 7.2.2.

### 5.5 Simple types and facets (implementation)
After 5.2. Owns `src/schema_simple_type.c`, `src/types.c` (the
`sxml_type_registry_register_schema_type` of D165 and describing
registered schema types), `src/cast.c`, `src/cast_lexical.c`,
`src/regex.c`, `src/regex_internal.h`, `tests/schema_simple_type_test.c`,
`tests/cast_test.c`, `tests/regex_test.c`, `tests/types_test.c`. Simple
type definitions: restriction, list and union (Part 2, 4.1), facet
applicability by primitive type and the facets' own constraints (Part 2,
4.3: length and minLength/maxLength consistency, bounds, totalDigits and
fractionDigits, fixed facets, whiteSpace, explicitTimezone), the
canonical facet values of `types.h`, registration in derivation order.
Casting to registered atomic, list and union types in `src/cast.c`: the
whitespace facet, then the lexical mapping of the primitive type, then
every facet of the type and its bases (cvc-facet-valid, cvc-pattern-valid,
cvc-enumeration-valid, ...): one lexical-space engine, the cast engine,
for XSD and XPath alike. The regex engine's XSD dialect: a flag of
`sxml_regex_flags` (D53) that anchors the pattern to the whole value and
rejects what the XSD 1.1 grammar lacks (back-references, reluctant
quantifiers, non-capturing groups, lookaround, `^` and `$` as anchors;
Part 2, appendix G). Done when MS-DataTypes, MS-Regex, MS-SimpleType,
NIST ≥ 90% under the driver once 5.4 and 5.7 run them (unit tests before:
cast of `"5"` to a restriction of xs:integer with minInclusive 10 raises
FORG0001 with cvc-minInclusive-valid in the description). Golden:
`src/cast.c`, `src/types.c`.
**Done** 2026-10-18 (D171): facets in the cast engine (`src/cast_facet.c`,
hooked into `src/cast.c`), the regex engine's XSD dialect, schema type
registration in `src/types.c`, SIMPLE_TYPES in `src/schema_simple_type.c`
with `tests/schema_simple_type_test.c`; the packet's unit test passes.
Schema tests: NIST 100 %, MS-Regex 100 %, MS-DataTypes 99.7 %,
MS-SimpleType 96.4 %; the ≥ 90 % of the instance tests is measured once
5.7 validates.

### 5.6 Complex types and content models (implementation)
After 5.2. Owns `src/schema_complex_type.c`, new
`src/schema_content_model.c`, `tests/schema_content_model_test.c`.
Complex type definitions: simple and complex content by extension and
restriction (cos-ct-extends, derivation-ok-restriction; restriction of
content checked as language inclusion of the two automata, XSD 1.1
Part 1, 3.4.6.4), attribute uses and wildcards (cos-aw-union,
cos-aw-intersect), defaultAttributes, registration of the complex type
(content type variety and simple content type). Content models: the
particle tree compiled to a deterministic automaton whose transitions
are element declarations and wildcards, with counters for maxOccurs
beyond a small bound (so large values cost a counter, not states, within
`maximum_content_model_state_count`), Unique Particle Attribution
(cos-nonambig, with XSD 1.1's rule that an element particle wins over a
wildcard), Element Declarations Consistent (cos-element-consistent), XSD
1.1 all groups (counted members, wildcards, maxOccurs above 1), open
content (interleave and suffix modes, the schema's default open content),
substitution groups in element particles (cos-equiv-derived-ok-rec,
blocking), and the matcher the validator steps (start, child element name
to a declaration or wildcard, end, with the expected names for error
messages). Done when MS-Particles, MS-ModelGroups, MS-Group,
MS-ComplexType, MS-Wildcards, Sun MGroup and CType, Saxon All and Open,
IBM allGroup and openContent ≥ 80% once 5.7 runs them; unit tests of the
automaton first. Golden: `src/regex.c` (compiled program, explicit
stacks), `src/types.c`.
**Done** 2026-10-18 (D172): COMPLEX_TYPES and content models with unit
tests; the groups' schema tests pass 96 % or more with 5.4's reader
(MS-Particles 851 of 853); their instance tests are measured once 5.7
validates.

### 5.7 Validation and the post-schema-validation infoset (implementation)
After 5.4, 5.5, 5.6. Owns `src/schema_validate.c`, new
`src/schema_validate_internal.h`, `src/xdm_node.c` (typed values),
`tests/schema_validate_test.c`, `tests/xdm_node_test.c`. The schema
validator as a tree builder (`schema.h`): element assessment (cvc-elt,
cvc-type, cvc-complex-type with the 5.6 matcher, cvc-assess-elt, lax and
strict wildcards and the lax mode), xsi:type (cvc-elt.4, blocking),
xsi:nil (cvc-elt.3), attribute assessment (cvc-attribute,
cvc-assess-attr, attribute wildcards, xsi: attributes), simple values
through `src/cast.c` (cvc-datatype-valid, cvc-simple-type), value
constraints (default and fixed values, sent as attributes and content),
schema normalized attribute values, the annotations of every element and
attribute (invalid and not-assessed parts as XDM 4.0, 3.3.1.1 says), ID
and IDREF (cvc-id, is-id, is-idrefs) per validation root, the subtree
recorder that keeps an annotated copy of an element's subtree for the
packets that evaluate XPath on it (5.8, 5.9), the report sink and error
recovery (after an error in content the rest of the element is assessed
laxly, so one error gives one report). Typed values derived from string
value and annotation in `sxml_node_make_typed_value` (D165), with
FOTY0012 for element-only content. `sxml_schema_validate_node` over
`sxml_tree_builder_copy_node` and an arena tree. Done when the in-scope
instance tests of MS and Sun pass ≥ 80% and NIST ≥ 95%, and
`fn:data` of a validated `<a>12</a>` of a restriction of xs:integer gives
an item of that type (unit test through the public API). Golden:
`src/arena_tree.c` (a builder), `src/xslt_whitespace.c` (a filtering
builder).
**Done** 2026-10-18 (D173): the validator as a filtering tree builder
(content models, wildcards, xsi:type, xsi:nil, attributes, defaults and
fixed values, simple values through the cast engine, IDs as XSD 1.1
binds them), the subtree recorder and the type alternative copy for 5.8
and 5.9, `sxml_schema_validate_node`, derived typed values; the driver
validates instance tests and follows location hints. Instance tests in
scope: NIST 100 %, MS 95.0 %, Sun 93.9 %; XSD in scope 40,755 of 41,518,
slice 147. Not done: inherited attributes for type alternatives (5.9).

### 5.8 Identity constraints (implementation)
After 5.7. Owns `src/schema_identity.c`, `tests/schema_identity_test.c`.
xs:unique, xs:key, xs:keyref and XSD 1.1's `ref` (Part 1, 3.11): the
selector and field XPath subsets checked on the XPath front end's AST
(c-selector-xpath, c-fields-xpath; xpathDefaultNamespace), evaluated by
the evaluator over the subtree the 5.7 recorder keeps for the declaring
element; key sequences compared by typed value equality (Part 1, 3.11.4,
with XSD 1.1's identity of values), node tables carried up to ancestors
for keyref (cvc-identity-constraint). Done when MS-IdentityConstraint,
Sun IdConstrDefs and IBM identityConstraint and
xpathDefaultNSonKeyKeyRefUnique ≥ 80%.
**Done** 2026-10-18 (D177): subsets on the AST, compilation with
`xpath.h`, unique, key and keyref on the recorded subtree with node
tables from descendants; the four groups 100 % (MS 1,058, Sun 48, IBM
18 and 26).

### 5.9 Assertions and type alternatives (implementation)
After 5.7. Owns `src/schema_assertion.c`, `tests/schema_assertion_test.c`.
xs:assert on complex types and xs:assertion facets on simple types
(Part 1, 3.13; Part 2, 4.3.13: `$value`), and conditional type assignment
(xs:alternative, Part 1, 3.12): XPath compiled with `xpath.h` in the
schema document's static context (namespaces, xpathDefaultNamespace, the
base URI, the F&O function library), so the XPath 2.0 of XSD 1.1 is read
by the XPath 4.0 front end (a superset; XSD 1.1 allows it as an
extension); evaluated on the recorded subtree (assertions: the element
with its validated descendants, comments and processing instructions
excluded; alternatives: the element with its attributes and inherited
attributes, no children). Simple type assertions are checked by
validation; casting to a type with assertions in a schema-aware
expression evaluates them as a second step of the cast (5.11), never
from inside `src/cast.c`. Done when Saxon Assert and CTA, IBM assert,
assertion, CTA, typeAlternatives, typeAlternativesMixed ≥ 80%.
**Done** 2026-10-18 (D175): tests compiled with `xpath.h` in XPath
Valid's static context; assertions on an assertion copy of the recorded
subtree, assertion facets with `$value` (list items, union members),
simpleContent assertion facets on the simple content type, type
alternatives with inherited attributes, type table equivalence. The six
test sets 448 of 448 (100 %); XSD in scope 41,037 of 41,518.

### 5.10 `sxml validate` (implementation)
After 5.7. Owns new `apps/sxml/command_validate.c`, `apps/sxml/program.c`
(one table entry), `docs/CLI.md` (the command enters the table and its
section, D44), `tests/cli/` (golden tests). Exactly the design below.
Done when its golden tests pass under `debug`, `debug-gcc` and `wasm`,
the WebAssembly budget holds or its growth is reported under "Proposed",
and validating the suite's `msData/particles` instances through the CLI
agrees with the driver.
**Done** 2026-10-18 (D176): `apps/sxml/command_validate.c` on the public
API, the framework's required options, 26 golden tests, the docs/CLI.md
section; debug, debug-gcc and wasm pass; the CLI's WebAssembly budget
raised and reported under "Proposed"; the 514 particles instance tests
agree with sxml-xsd-test (hinted documents given as `--schema`, since
neither follows hints).

### 5.11 Schema-aware XPath and XQuery (design and implementation)
After 5.7. Owns `include/sxml/xpath.h` (one field, with a record),
`src/xpath_static.c`, `src/xquery_static.c`, `src/xquery_lower.c`,
`src/function_library_json.c` (the xsd-validation option),
`src/function_library_type.c`, new `src/function_library_schema.c`,
`tests/qt4/driver/` (schema environments), `tests/xquery_schema_test.c`.
Each delayed feature replaces its one rejection point (D162):
- `sxml_static_context_configuration.in_scope_schemas`, a view of compiled
  schemas at the end of the struct (empty: not schema-aware, as today):
  their types, element and attribute declarations are the in-scope
  schema definitions (XPath 4.0, 2.2.1); a type name resolves to a
  built-in type first, then in the first schema that has it (XPST0051
  otherwise).
- schema-element(N) and schema-attribute(N) (the XPST0008 place in
  `src/xpath_static.c`): resolved statically to the choice of
  element(M, T) tests over N's substitution group (each non-abstract
  member M with its declared type T, `?` when M is nillable), and to
  attribute(N, T): the registry's existing node tests and subtype rules
  then give matching and subtyping (XPath 4.0, 3.2.7.3 and 3.3.2), with
  no schema reference in the type registry.
- XQuery `import schema` (the XQST0009 place in `src/xquery_static.c`):
  the module's imports compile one `sxml_schema` (RESOLVED documents with
  the location hints, target namespaces checked; XQST0035, XQST0059,
  XQST0012 with the constraint name in the description); a namespace a
  host in-scope schema defines is taken from it, not read again.
- XQuery `validate` (the XQST0075 place in `src/xquery_lower.c`): an
  internal native over `sxml_schema_validate_node` (lax, strict, type;
  XQDY0027, XQDY0061, XQDY0084, XQST0104); construction mode preserve
  annotates constructed elements xs:anyType.
- `fn:xsd-validator` (F&O 4.0) in a new library part; the xsd-validation
  option of fn:doc and fn:parse-xml (the FODC0009 place); fn:schema-type
  and node-type-annotation with registered schema types; casts to types
  with assertions run them after `src/cast.c`.
- The QT4 driver reads `<schema>` environments into in-scope schemas and
  stops excluding schemaImport, schemaValidation and typedData; the
  dashboard counts them as a new area "schema-aware".
Done when the schema-aware XQ40 cases (about 1,100, 160 of them XP40)
pass ≥ 70% and XP40 and XQ40 have not dropped.
**Done** 2026-10-19 (D174): in-scope schemas, import schema, validate,
schema-element and schema-attribute, schema-type casts and constructors,
fn:xsd-validator, fn:parse-xml's xsd-validation, the driver's schema
environments. Schema-aware XQ40 1,231 of 1,516 (81.2 %), XP40 279 of 315;
XP40 30,924 and XQ40 41,261 passed, 0 errors.

### 5.12 Schema-aware XSLT (implementation)
After 5.11. Owns `src/xslt_read.c` (the XTSE1650 and XTSE1660 places),
`src/xslt_lower.c`, `src/xslt_lower_construct.c`,
`src/xslt_static.c`, `src/function_library_xslt.c`,
`tests/xslt/driver/` (schema_aware feature and schema environments),
`tests/xslt_schema_test.c`. xsl:import-schema (namespace,
schema-location, an inline xs:schema read as a NODE document; XTSE0220),
validation and type attributes of xsl:element, xsl:attribute, xsl:copy,
xsl:copy-of, xsl:document, xsl:result-document, literal result elements
and xsl:source-document, default-validation, typed modes, validation of
fn:doc and fn:document results (XTTE1510, XTTE1512, XTTE1515, XTTE1540,
XTTE1550, XTTE1555, XTSE1520, XTSE1530), type-available and
element-available for schema types, the system property is-schema-aware.
Done when the in-scope XSLT 4.0 cases with feature schema_aware (about
690, mostly attr- and decl- sets) pass ≥ 60% and the XT40 count has not
dropped.
**Done** 2026-10-19 (D179): `src/xslt_schema.c` (xsl:import-schema),
the validate row with XSLT's rules for every validation and type
attribute, default-validation, input-type-annotations, typed modes,
fn:document's xsd-validation, is-schema-aware, the driver's schema
environments. 615 of the 679 in-scope schema_aware cases (90.6 %); XT40
8,642 passed (was 7,990).

### 5.13 Conformance sweeps (implementation, repeatable)
After 5.7. As 2.19 and 4.17, over the XSD contributor groups and then the
schema-aware QT4 and XSLT sets. Each sweep claims its files in
`docs/LOG.md`.
**Sweep 1** done 2026-10-18 (D178): D174's QT4 outcome rules removed and
their cases fixed (group by tuples, leaf predicates in the filter task);
validation descriptions that name what failed; configured schema
documents located by the library; one XPath record compiler; src-redefine
6.2.2 and 7.2.2, open content and empty content, override defaults,
simple bases in the registry, year -0000, src-ct, wildcard subset. XSD in
scope 41,483 of 41,518, XP40 30,931, XQ40 41,273 (0 errors), XT40 7,992.
**Sweep 2** done 2026-10-18 (D180): XT40's six errors cleared at their
causes (built-in types restated by the schema for schema documents, the
stand-in xml.xsd only without another document of the namespace, invalid
sources judged as the transformation's error); validate-items a row of
its own (sxml.wasm in budget); xslt.h's in_scope_schemas comment
accepted. XT40 8,650 (0 errors), XP40 30,931, XQ40 41,273, XSD 41,483.
**Sweep 3** done 2026-10-20 (D277): the 32 XSD11 failures by cause, 21
fixed by XSD 1.1's rules (src-attribute.5, src-redefine.1, attribute
types, required uses in restrictions, simple content of
xs:anySimpleType, fixed values as values, Element Declarations
Consistent with wildcards, wildcard unions, xml:lang), 11 left as 1.0
rules or disputed; content recovery after cvc-complex-type.2.4; no
xsi:schemaLocation in the CLI (the library follows no hints). XSD
41,507 of 41,518; schema-aware XQ40 and XT40 sets unchanged.

### 5.14 Phase 5 review (review)
After everything. Owns the gate boxes. A separate agent (D11) runs the
rubric and re-checks each "done when"; open items go under "Proposed".
**Done** 2026-10-19 (D182), except the confirming full runs and
fuzzing, deferred to the owner's machine: numbers at 02303a7 and
3b0812a, a new schema fuzz target, both step budgets judged fair, no
driver reclassifies (the XSD driver's "not run" for an unsupported
feature fixed), D71 through component graphs and function pointers,
NCName, whitespace and token helpers merged (fifteen files), an
allocation-failure loop for schema.h, `schema.h` stable, five accepted
designs out of Proposed.

### 5.16 Schema location hints (design and implementation)
After 5.13 sweep 3 (D277). Owns the additive field of
`include/sxml/schema.h` (with D278), `src/schema_assemble.c`,
`src/function_library_schema.c`, the use-xsi-schema-location hunks of
`src/function_library_json.c`, `src/function_library_node.c`,
`src/function_library_xslt.c` and `src/function_library_core_internal.h`,
`apps/sxml/command_validate.c`, `tests/cli/` (validate_hints*),
`tests/xsd/driver/`, `third_party/w3c-xlink/`, the book's
"Schema hints in documents". XSD 1.1 Part 1, 4.3.2: the hints of an
instance join the assembly when the host asks; fn:xsd-validator's
use-xsi-schema-location works; `sxml validate` follows hints with a
flag; xlink.xsd vendored so that the introspection set runs.
**Done** 2026-10-20 (D278): `schema_location_hint_node`, hints only
for namespaces the schema lacks; fn:xsd-validator, fn:doc,
fn:doc-available, fn:parse-xml and fn:document; XQuery validate and
XSLT validation keep ignoring hints; `sxml validate
--use-schema-location`; the XSD driver through the library; the QT4
driver's schema-location-hint. XSD11 41,507 to 41,599 (the 92
introspection tests), 11 failed unchanged; QT4 schema-aware XQ40
11,691 to 11,696; XT40 schema-aware 4,084 to 4,087; none lost.

## `sxml validate` (design for 5.10; docs/CLI.md gets it when it works)

```
sxml validate --schema=SCHEMA [--schema=SCHEMA]... [--mode=MODE] [--format=FORMAT] FILE...
```

Validates each FILE (`-`: standard input) against the XSD 1.1 schema
assembled from the SCHEMA documents. One intent (D44): it checks validity
and writes no document; a typed result is the business of `sxml xquery`
with `validate`.

| Option | Description |
| --- | --- |
| `--schema=SCHEMA` | A schema document; repeatable, at least one. All of them, with every document they include, import, redefine or override (local files, relative to the referring document's path), form one schema |
| `--mode=MODE` | `strict` (default): each FILE's document element must have a global declaration; `lax`: one without is assessed laxly and is "not known" |
| `--format=FORMAT` | Report format: `text` (default) or `json` |

- The schema is compiled first. If it has errors, each goes to standard
  error as `SCHEMA:LINE:COLUMN: CONSTRAINT: description` (CONSTRAINT is
  the XSD constraint name, such as `src-resolve`) and no FILE is read.
- Each FILE is parsed and validated in one pass (the parser feeds the
  validator; no tree is built). sxml prints nothing when every FILE is
  valid. Each error goes to standard error as
  `FILE:LINE:COLUMN: CONSTRAINT: description`, every error of the file,
  not only the first; a FILE that is not well-formed is reported as
  `sxml check` reports it. xsi:schemaLocation hints are ignored
  (5.16 added `--use-schema-location`, D278).
- Standard input is read at most once: `-` as a SCHEMA and a FILE, or
  twice, is a usage error.
- With `--format=json`, one JSON object on standard output:
  `{"schema":{"documents":["po.xsd"],"status":"valid"},"files":[{"file":"a.xml","status":"valid"},{"file":"b.xml","status":"invalid","errors":[{"code":"cvc-complex-type.2.4","line":3,"column":5,"byte_offset":40,"message":"..."}]}]}`.
  The schema's status is `valid`, `invalid` (with `errors` as above, each
  with `document`), `unreadable` or `failed`; a FILE's is `valid`,
  `invalid`, `not-known` (lax), `not-well-formed` (with `line`, `column`,
  `byte_offset`, `message` as for `sxml check`), `unreadable` or `failed`.
  Fields may be added later; existing ones do not change.
- Exit status: 0 when every FILE is valid (or not known, with
  `--mode=lax`); 1 when the schema or a FILE is invalid or a FILE is not
  well-formed; 2 for a usage error (no `--schema`, no FILE, a bad
  `--mode` or `--format`, `-` twice); 3 when a SCHEMA or a FILE cannot be
  read (a document a SCHEMA includes that cannot be read is a schema
  error, src-include, exit 1); 4 at a limit. As for `sxml check`, 3 wins
  over 4 and 4 over 1.

```console
$ sxml validate --schema=po.xsd po.xml
$ sxml validate --schema=po.xsd broken-po.xml
broken-po.xml:7:5: cvc-complex-type.2.4: element 'comment' is not allowed here; expected 'shipTo'
$ sxml validate --schema=po.xsd --format=json po.xml
{"schema":{"documents":["po.xsd"],"status":"valid"},"files":[{"file":"po.xml","status":"valid"}]}
```

## Gate (proposed in D165; the owner may change the numbers)

- [ ] In-scope XSD 1.1 tests (41,538) ≥ 90% pass under `release`; the
  datatype groups (NIST, MS-DataTypes, MS-Regex: about 30,600) ≥ 95%; the
  structure groups (the other MS, Sun, Boeing, WG and Oracle sets: about
  8,600) ≥ 80%; the XSD 1.1 groups (Saxonica, IBM: 2,295) ≥ 70%.
  (deferred to the owner's machine (D181); measured by the review at
  02303a7 and 3b0812a: 41,483 of 41,518 in scope, 99.9 %; datatypes
  100 %, structures 99.7 %, XSD 1.1 99.6 %)
- [ ] Schema-aware QT4 cases (features schemaImport, schemaValidation,
  typedData: about 1,100 XQ40) ≥ 70%; schema-aware XSLT 4.0 cases (about
  690) ≥ 60%. (deferred to the owner's machine (D181); measured: XQ40
  1,236 of 1,516, 81.5 %; XT40 627 of 971, 64.6 %)
- [ ] XP40 ≥ 90%, XQ40 ≥ 85%, XT40 ≥ 70% still, none of their counts
  dropped; all ctest slices at their baselines. (deferred to the owner's
  machine (D181); measured: 97.6 %, 97.9 %, 92.0 %, core sets 88.5 % or
  more, passed 30,931, 41,273, 8,650; slices 198, 186, 147, 147 under
  every preset at 3b0812a)
- [ ] No in-scope test crashes, leaks or ends in an "error" outcome.
  (deferred to the owner's machine (D181), with the fuzzing; measured:
  every test set of the four suites finished, 0 errors)
- [x] `sxml validate` matches docs/CLI.md. (agent review, D182)
- [x] Review agent pass (D11), findings fixed; the owner is told schema
  validation works (D11 goal). (agent review, D182; the confirming full
  runs and fuzzing are the owner's, the larger follow-ups are listed in
  D182 and the LOG)
