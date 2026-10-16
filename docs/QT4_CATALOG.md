# QT4 test catalog format

A reference for writing the conformance test driver. It covers the W3C QT4
test suite (`qt4cg/qt4tests`) and gives an outline of the XSLT 4.0 suite
(`qt4cg/xslt40-test`). Both are fetched by `tools/fetch-test-suites.sh` into
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
  (rare: encodings, large queries). Every `file` attribute is resolved
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

- `catalog.xml` is in namespace `http://www.w3.org/2012/10/xslt-test-catalog`.
  It lists 278 `<test-set name file>` entries, with 13,867 test cases in all.
  12,639 of them apply to XSLT 4.0 (tokens `XSLT10+` up to `XSLT40+`).
- `tests/<group>/<area>/_<area>-test-set.xml` sits next to its `.xsl` and
  `.xml` files. The groups, with case counts: `misc` 3,123, `strm`
  (streaming) 2,564, `insn` 1,996, `attr` 1,634, `decl` 1,497, `fn` 1,339,
  `type` 975 and `expr` 739. `sandp` holds generated streamability-posture tests.
- Dependencies sit inside a `<dependencies>` wrapper, as `<spec value>`,
  `<feature value>` and implementation-defined items such as
  `year_component_values`. Again, a test-case spec replaces the test-set
  spec.
- `<test>` holds `stylesheet file`, `package`, `initial-template`,
  `initial-mode`, `initial-function`, `param` and `output`. Environments are
  like those in QT4, plus `stylesheet` entries.
- The assertions are the QT4 set plus `assert-result-document`,
  `assert-message`, `assert-warning` and `assert-serialization`.
- `admin/catalog-schema.xsd` defines the format. `runner/` holds an XSLT-based
  reference runner.
