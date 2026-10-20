# QT4 test catalog format

A reference for writing the conformance test driver. It covers the W3C QT4
test suite (`qt4cg/qt4tests`) and gives an outline of the XSLT 4.0 suite
(`qt4cg/xslt40-test`). Both are vendored by `tools/update-test-suites.sh` into
`third_party/` at the commits pinned in decision D13. The authoritative
definition is `third_party/qt4tests/catalog-schema.xsd` together with
`guide/running.html`.

## Files

- `catalog.xml`: root `<catalog test-suite="FOTS" version="4.0">` in namespace
  `http://www.w3.org/2010/09/qt-fots-catalog`. It holds global
  `<environment name="...">` elements and `<test-set name="..." file="..."/>`
  entries that point to the test-set files.
- A test-set file (for example `fn/abs.xml`) has the root `<test-set name>`:
  `<description>`, `<link>`, local `<environment name>`, set-level
  `<dependency>` and one or more `<test-case>`.
- `<test-case name>` holds `description`, `created`, `modified*`, an optional
  `<environment>`, `<module>*`, `<dependency>*`, `<test>` and `<result>`.
- `<test>` holds the expression text, or `@file` names an external query
  (rare: encodings, large queries). An XQuery Update test case has several:
  the ones before the last (`@update="true"`) change the mutable sources,
  in order, and the last one's result is checked (D187). Every `file` attribute is resolved
  against the base URI of the XML file that contains it.

Test-set names are prefixed by area: `fn-`, `op-`, `math-`, `map-`,
`array-`, `xs-`, `prod-` (grammar productions), `misc-`, `method-`
(serialization), `app-` (use cases), `bin-` and `expath-` (EXPath), `upd-`
(Update) and `ft-` (Full Text).

## Environments

A test case's `<environment>` is one of three things:

- `ref="name"`: resolved first in the test-set file, then in `catalog.xml`.
- An inline element with no `name` and no `ref`.
- Absent, which means an empty static and dynamic context: the context item
  is absent.

The environment `empty` is explicitly empty. Children of an environment:

| Element | Meaning |
|---|---|
| `source role="." file uri validation` | XML document. `role="."` makes it the context item. `role="$name"` binds an external variable. `uri` makes it available to `fn:doc(uri)`. A `validation` attribute (strict, lax) means schema awareness, so skip the test. `@dtd`, `@retain-location` (for `fn:location`) and `@mutable` (Update) also occur. |
| `param name select as source declared` | External variable. `select` is an XPath expression, usually a literal or a constructor call. `source` names a source document. `declared="true"` means the query prolog already declares it. |
| `context-item select` | The context item, given as an expression. |
| `resource uri file media-type encoding` | Text resource for `unparsed-text*`, `json-doc` and similar, reached through `uri`. |
| `collection uri` | Contains `source`, `resource` and `query` elements. An empty or absent `uri` means the default collection. `query` is an XQuery expression whose result forms the collection. |
| `namespace prefix uri` | Statically known namespace. An empty prefix sets the default element namespace. Only pure XPath tests use it. |
| `decimal-format name ...` | A named or default decimal format for `format-number`. Properties are `decimal-separator`, `grouping-separator`, `zero-digit`, `digit`, `minus-sign`, `percent`, `per-mille`, `pattern-separator`, `exponent-separator`, `infinity` and `NaN`. In 4.0 a property may have the form `c:rendition`, for example `%:pc`. Only pure XPath tests use it. |
| `static-base-uri uri` | The static base URI. `#UNDEFINED` means the base URI is absent. If no value is given, the base URI is the URI of the test file. |
| `collation uri default` | The test uses this collation URI. `default="true"` makes it the default collation. Skip the test if the collation is not supported (only codepoint is required). |
| `schema uri file` | Schema import. This needs schema awareness, so skip the test. |
| `function-library`, `sandpit path` | XSLT/XQuery function library. `sandpit` gives a writable copy of a directory (EXPath File). |

The `<module uri file location>` element in a test case maps
`import module` (with an optional location hint) to a file. It is XQuery-only.

## Dependencies

`<dependency type value satisfied="true|false">` can appear on a test set and
on a test case. With `satisfied="false"`, the test runs only when the
dependency is *not* satisfied.

The `spec` type takes space-separated tokens such as `XP40+ XQ40+`. A token
matches if its language is right (`XP` or `XQ`, and `XT` in a few tests) and
its version is equal, or, for a token with a trailing `+`, the version is
equal or later. The test runs if any token matches.

- A test-case `spec` replaces the test-set one. Under AND semantics 13 tests
  could never run (e.g. set `XQ10+`, case `XP30+ XQ30+`).
- No `spec` anywhere means every version of both languages (about 15,600
  cases, mainly in `fn-`, `op-` and `prod-`).
- A run targets exactly one language and version: `XP40` or `XQ40`.

Other types (each value is described in `dependency_meanings.xml`):

- `feature`: `higherOrderFunctions`, `schemaImport`, `schemaValidation`,
  `typedData`, `staticTyping`, `serialization`, `namespace-axis`,
  `xpath-1.0-compatibility`, `moduleImport`, `fn-load-xquery-module`,
  `fn-transform-XSLT`, `fn-transform-XSLT30`, `infoset-dtd`,
  `simple-uca-fallback`, `advanced-uca-fallback`,
  `non_unicode_codepoint_collation`, `collection-stability`,
  `directory-as-collection-uri`, `non_empty_sequence_collection`,
  `olson-timezone`, `arbitraryPrecisionDecimal`, `dtd-validation-option`,
  `xinclude-option`, `remote_http`, `fn-invisible-xml`,
  `fn-format-integer-CLDR`, `schema-location-hint`, `XQUpdate`, `fullText*`,
  `binary` and `expath-file`.
- `xml-version` (`1.0`, `1.1`, `1.0:4-`, `1.0:5+`), `xsd-version`,
  `language` and `default-language` (`en`, `de`, ...), `calendar`,
  `format-integer-sequence`, `limits` (`big_integer`, `year_lt_0`),
  `unicode-version`, `unicode-normalization-form` and `revalidation`.

## Result assertions

`<result>` contains exactly one assertion. `$result` stands for the value of
the test expression. The expected values are themselves XPath expressions,
unless the description below says otherwise.

| Assertion | Passes when |
|---|---|
| `assert-eq` | The result is one atomic value, `eq` the expected expression. Numeric promotion and untypedAtomic casting apply. |
| `assert-deep-eq` | `deep-equal($result, expected)`. |
| `assert-permutation` | Some permutation of the result is deep-equal to the expected value. NaN equals NaN. |
| `assert-true` / `assert-false` | The result is exactly `xs:boolean` true or false, not an effective boolean value. |
| `assert-empty` | The result is `()`. |
| `assert-count` | The result has N items. N is an integer literal. |
| `assert-type` | `$result instance of T`. The content is a SequenceType. |
| `assert` | The effective boolean value of the content expression is true, with `$result` bound. |
| `assert-string-value` | `string-join($result ! string(), " ")` equals the literal text content. With `normalize-space="true"`, both sides are normalized. |
| `assert-xml` | The result, serialized (`method=xml`, `indent=no`, no XML declaration), matches the literal XML fragment, or the one in `@file`. Compare canonically or with `deep-equal` after wrapping in an element. With `ignore-prefixes="true"`, prefixes are ignored. |
| `serialization-matches` | The result, serialized with the query's own parameters, matches a regex (`@flags`, unanchored). The regex is literal text or `@file`. |
| `assert-serialization-error code` | Evaluation succeeds and serialization fails. |
| `error code` | Evaluation raises an error. `code` is an NCName in the `err` namespace, `Q{uri}local`, or `*`. A different code is usually reported as "pass, wrong error". |
| `all-of` / `any-of` | Every child or at least one child passes. These are used to allow implementation-dependent outcomes, often a value or an `error`. |
| `not` | The child assertion fails. |

Frequency: `assert-true` 11,638; `error` 10,063; `assert-eq` 6,500;
`assert-string-value` 5,989; `assert-deep-eq` 5,013; `assert-false` 3,990;
`assert-xml` 2,981; `assert` 2,931; `any-of` 2,163; `all-of` 1,770; `assert-type`
1,763; `assert-empty` 927; `serialization-matches` 794; others under 300.

## Counts (pinned qt4tests commit)

- 737 test sets with 47,313 test cases. XQ40: 46,444 cases in 737 sets.
  XP40: 32,928 cases in 540 sets.
- Both XP40 and XQ40: 32,822. XQ40 only: 13,622. XP40 only: 106. Neither
  (1.0, 3.0 or 3.1 only): 763.
- Every `prod-TryCatchExpr` test is XQ-only. In the suite, try/catch is
  XQuery-only.

## xslt40-test layout

Read by `sxml-xslt-test` (`tests/xslt/driver/`, D117), which this section
describes as it reads the suite.

- `catalog.xml` is in namespace `http://www.w3.org/2012/10/xslt-test-catalog`.
  It holds only `<test-set name file>` entries: 250 active ones (28 more are
  commented out), with 13,867 test cases in all, and no environments
  ("at present there are no global environments", catalog-schema.xsd).
  12,639 of them apply to XSLT 4.0 (tokens `XSLT10+` up to `XSLT40+`, or
  `XSLT40`; 12 cases have no spec at all and apply to every version).
- `tests/<group>/<area>/_<area>-test-set.xml` sits next to its `.xsl` and
  `.xml` files. The groups, with case counts: `misc` 3,123, `strm`
  (streaming) 2,564, `insn` 1,996, `attr` 1,634, `decl` 1,497, `fn` 1,339,
  `type` 975 and `expr` 739. `sandp` holds generated streamability-posture
  tests, none of them in the catalog today.
- Scope (D95): of the 12,639, the 8,728 outside `strm` and `sandp` that
  need none of the features schema_aware, streaming, dtd, XML_1.1, XSD_1.1,
  backwards_compatibility, HTML4, ICU_spellout. A case that asks for one of
  them to be absent (`satisfied="false"`) stays in scope.
- Dependencies sit inside a `<dependencies>` wrapper of the test set or the
  test case. Each child is one dependency, its element name the type:
  `<spec value>`, `<feature value [satisfied]>` and implementation-defined
  items such as `year_component_values`, `combinations_for_numbering`,
  `package_version_resolution`, `on-multiple-match` and
  `maximum_number_of_decimal_digits`. Their values are phrases ("support
  negative year", "CIRCLED DIGIT ONE"), not token lists; only spec values
  are space-separated tokens. A test-case spec replaces the test-set spec.
  `XPath_3.1` is XSLT 3.0's optional support of XPath 3.1, not a request
  for 3.1 behaviour.
- Environments are named in the test set (`<environment name>`), referred
  to by `<environment ref>` in a test case, or inline there. They hold the
  QT4 children `source`, `param`, `resource`, `collection`, `collation`,
  `schema`, `context-item`, plus `stylesheet`, `package` and `output`. A
  `source` gives its document by `file` or inline in a `<content>` child
  (CDATA), may `select` the global context item within it (role "."), and
  has the flags `streaming`, `dtd`, `defines-stylesheet` (the document's
  xml-stylesheet processing instruction names the stylesheet: 16 cases),
  `xinclude` and `xml-version`.
- `<test>` holds `package*`, `stylesheet*` (`file`, `role` principal or
  secondary; packages also `uri`, the package name, and `package-version`),
  `posture-and-sweep?`, `param*` (`name` an xs:QName, `select`, `as`,
  `static="yes"`), then one of `initial-template name?` (absent name:
  `xsl:initial-template`), `initial-mode name select?` (`#default`,
  `#unnamed` or a QName; `select` is the initial match selection) and
  `initial-function name`, each with its own `param` children (`tunnel`
  for templates and modes, by position for functions), and `output`
  (`serialize`, `tree`, `well-formed`, `file`, `result-var`). Without an
  initial element the stylesheet is applied to the global context item in
  the default mode. The principal module is the first non-secondary
  module of `<test>`, else of the environment (8,913 cases have it in the
  test, 4,708 only in the environment). Counts: initial-template 6,312,
  initial-mode 156, initial-function 38; 422 tests use packages.
- The assertions are the QT4 set plus `assert-result-document uri`,
  `assert-message`, `assert-warning`, `assert-serialization`
  (`method`, `file`), `serialization-matches` (`flags`),
  `assert-serialization-error code` and `assert-posture-and-sweep`. `assert`
  is evaluated with the document node of the principal result as the
  context item and the default element namespace none (not with
  `$result`); `assert-message` and `assert-result-document` each hold one
  assertion about one message or result document. Counts, as assertions
  (test cases): `assert` 8,998 (5,345), `assert-xml` 5,215 (5,178),
  `error` 2,828 (2,706), `all-of` 1,273, `serialization-matches` 890
  (369), `assert-string-value` 350, `assert-result-document` 156 (74),
  `assert-serialization` 112, `assert-message` 92 (50), `assert-warning`
  6; `assert-posture-and-sweep` occurs only in `sandp`.
- `admin/catalog-schema.xsd` defines the format. `runner/` holds an XSLT-based
  reference runner.

## XSTS layout (the W3C XSD test suite)

Read by `sxml-xsd-test` (`tests/xsd/driver/`, D168), which this section
describes as it reads the suite (`third_party/xsdtests`, D165).

- Every element is in namespace
  `http://www.w3.org/XML/2004/xml-schema-test-suite/`, every reference an
  `xlink:href` (`http://www.w3.org/1999/xlink`) relative to its file.
  `common/xsts.xsd` defines the format.
- `suite.xml` is a `testSuite` of 93 `testSetRef`s (two more, the
  precisionDecimal sets, are commented out and listed in
  `extra-suite.xml`). The first, `common/introspection.testSet`, validates
  the suite's own files against `common/xsts.xsd`; it is run since the
  driver serves the XLink schema document xsts.xsd imports
  (`third_party/w3c-xlink`, D278; D165 left it out).
- A `testSet` (`name`, `contributor`, `version?`) holds `testGroup`s
  (`name`, `version?`), each with at most one `schemaTest` and any number
  of `instanceTest`s (`name`, `version?`). A schema test lists its
  `schemaDocument`s (loaded one by one, in order, into one schema; valid
  means it compiles); an instance test has one `instanceDocument`,
  validated against the schema of its group's schema test, or against the
  built-in components alone when the group has none (55 groups). Each test
  has `expected validity version?` elements and a `current status`
  (submitted, accepted, stable, queried, disputed-test, disputed-spec).
  `testSet/@name` repeats across contributors (Assert, CTA, TargetNS, VC,
  Zone), so the driver names a set after its file: `saxonMeta/Assert.testSet`
  is `saxon-Assert`; a test is `GROUP/TEST`.
- Version tokens (xsts.xsd, `version-info`): on testSet, testGroup and
  tests they are or-ed per element ("supports any of them"), on `expected`
  and-ed. Tokens fall into classes (XSD version, 1.0 edition, XML
  substrate, Unicode version, CTR-all behaviour, XPath in CTA, XDM
  filtering). In the suite: `1.1` 1,042 times, `1.0` 144,
  `full-xpath-in-CTA` 30, `restricted-xpath-in-CTA` 10, `Unicode_4.0.0` 9,
  `Unicode_6.0.0` 8; no other token occurs.
- Counts: 41,643 tests in the 92 test sets besides the introspection one
  (92 more there);
  statuses accepted 40,635, stable 850, queried 57, submitted 12,
  disputed-spec 5, disputed-test 1; expected elements valid 26,515, invalid 15,312,
  indeterminate 16. In scope for an XSD 1.1 processor (each level's version
  applies, status not queried or disputed, valid or invalid expected):
  41,518 (15,329 schema tests, 26,189 instance tests). D165 counted 41,538
  by or-ing the three levels' tokens together; D168 applies each level, as
  xsts.xsd says, which leaves out 19 XSD 1.0 instance tests of
  `wgMeta/substitution-groups.testSet` (five of them name files that do
  not exist) and one 1.0 test group of Saxonica.
