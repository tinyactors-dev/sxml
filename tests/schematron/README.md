# A Schematron conformance suite

Test cases for Schematron as ISO/IEC 19757-3:2025 (the fourth edition)
defines it, written for sxml (packet 7.30) and usable by any Schematron
processor. 332 cases in 18 test sets; `coverage.xml` maps the standard's
requirements to them.

## Layout

| File | What it holds |
|------|---------------|
| `catalog.xml` | The test sets, by name and file. |
| `SET.xml` | One test set: the cases for one clause group. |
| `SET/` | The schema, instance and other documents of that set's cases. |
| `coverage.xml` | Every normative requirement of clauses 3 to 7 and of the normative annexes, by clause, with a short paraphrase and the cases that cover it (or why none can). |
| `catalog.xsd` | The schema of `catalog.xml`, the test-set files and `coverage.xml`. |
| `tools/crosscheck.py` | Runs the cases through SchXslt and compares. |
| `tools/schxslt-crosscheck.txt` | The last cross-check's result, one line per case. |

The test sets: `schema-structure`, `static-errors`, `patterns-rules`,
`assertions`, `variables`, `abstract-patterns`, `abstract-rules`,
`inclusion`, `phases`, `diagnostics-properties`, `name-value-of`, `group`,
`visit-each`, `severity-role-flag`, `query-bindings`, `documents`,
`schematron-edition`, `svrl`.

## The format

The namespace is `urn:sxml:schematron-test-suite`. The layout follows the
QT4 test suite (`third_party/qt4tests`): a catalog, test sets, test cases.

```xml
<test-case name="rules-first-match-wins" since="2020">
  <description>Two rules with the same context in a pattern: only the first fires.</description>
  <spec clause="3.20"/> <spec clause="6.3"/>
  <dependency type="query-binding" value="xslt3"/>   <!-- optional -->
  <schema file="rules-first-match-wins.sch"/>
  <instance file="rules-first-match-wins.xml"/>
  <phase>#ALL</phase>                                <!-- optional -->
  <param name="limit" value="3"/>                    <!-- optional, schema parameters -->
  <result>...</result>
</test-case>
```

- `since` is `2020` for a case that uses only constructs of the third
  edition (ISO/IEC 19757-3:2020), `2025` for one that needs the fourth.
  Its expected result follows the 2025 text either way.
- File paths are relative to the set's directory (`patterns-rules/` for
  `patterns-rules.xml`).
- Without `phase`, the processor is given no phase, which 5.4.17 makes
  `#DEFAULT`.
- A `dependency` of type `query-binding` names a binding the case needs;
  a processor without it shall fail with an error (6.5) and the case does
  not apply. `feature` names an optional capability: `svrl` (SVRL output,
  Annex D) or `schema-parameter-override` (5.4.11, implementation-defined
  mechanism).
- Unless a case is about query bindings, its schema says
  `queryBinding="xslt3"`. How the default binding `xslt` (XSLT 1.0,
  Annex C) is read is the implementation's matter in practice, so no case
  outside `query-bindings` depends on it.

The `result` element holds exactly one expected outcome:

- `findings` (with zero or more `finding`), then optionally `flags` and
  `assert-svrl`. The findings form a multiset; their order is
  implementation-dependent (6.6). No findings means the instance is valid
  (3.22).
  - `kind`: `failed-assert` or `successful-report`.
  - `location`: the subject node (5.5.19, 6.3) as XPath 3.1's `fn:path`
    gives it, `/Q{}doc[1]/Q{}item[2]/@n`. Absent only when the subject is
    not a node (visit-each over atomic values); it is then not compared.
  - `id`, `role`, `flag`, `severity`: the assertion's, after evaluation
    (5.5.6, 5.5.14, 5.5.16). An absent attribute means the finding has
    none.
  - `document`: for a finding in a subordinate document (5.4.12), that
    document's path relative to the set's directory.
  - `message`: the assertion's text with `name` and `value-of` evaluated
    and markup reduced to its text, compared after whitespace
    normalization (5.3).
  - `diagnostic` and `property`: those the assertion references, with
    `ref`, the evaluated text and, where the case gives them, `role`,
    `lang`, `scheme`. Compared as sets.
  - `flags`: when present, the set of flags that became true (5.5.6).
  - `assert-svrl`: when present, an XPath expression (prefix `svrl` bound
    to `http://purl.oclc.org/dsdl/svrl`) that must be true of the SVRL
    document; only with the `svrl` feature.
- `schema-error clause="..."`: the schema is not a correct schema; the
  clause makes it so. A processor rejects it before or instead of
  validating.
- `error clause="..."`: validation ends in "error" (6.1): a dynamic
  error, a document that cannot be read.
- `any-of`: either of the outcomes it lists (when a processor may find
  the error statically or dynamically).
- `implementation-defined clause="..."`: the clause leaves the outcome to
  the implementation; the text says what each reading gives. Nothing is
  compared.

Two files are deliberately not well-formed, for 5.1 and 5.4.6; they carry
a `.txt` suffix: `static-errors/error-not-well-formed.sch.txt` and
`inclusion/include-not-well-formed/broken.sch.txt`. Every other file is
well-formed, and the catalog, the test sets and `coverage.xml` are valid
against `catalog.xsd`:

    sxml validate --schema=catalog.xsd catalog.xml coverage.xml *.xml

## Readings the suite takes

Where the text admits more than one reading, the suite takes the one
below and says so in the case's description; where it cannot decide, the
case is implementation-defined.

- The parameter delimiter (Annexes C, H, J, M, N): `$` marks a parameter
  only when the whole name token after it, a name as 3.11 defines it, is
  a parameter's name. `$AMOUNTX` is not a reference to a parameter
  `AMOUNT` (`abstract-param-prefix-variable`).
- Annex B's `param-names-match` assertion, read literally, fails for every
  instance of an abstract pattern that declares no `param` (the third
  edition's form, which the 2025 grammar still allows). The suite applies
  it only to abstract patterns that declare parameters.
- Variables (5.4.7, 7.2): a name may be defined once over the schema, the
  phase, the pattern and the rule in force; a rule variable with the name
  of a global one is an error. A pattern's variables are scoped to the
  pattern (3.23).
- The context of `name` and `value-of` in a message is the subject (6.3).
- `value-of` uses `fn:string()` (Annexes H to N): a sequence of several
  items is an error.
- Under the xpath bindings a relative rule context (`item`) has no
  defined evaluation context; cases with one are implementation-defined.
  Absolute contexts and expressions that select the same nodes from any
  start (`(//item)[1]`) stay normative.

## The cross-check

Every case with `since="2020"` runs through SchXslt 1.10.1, an XSLT
implementation of the third edition (MIT licence; not part of this
suite), on the sxml command:

    python3 tests/schematron/tools/crosscheck.py \
        --sxml BUILD/apps/sxml \
        --schxslt schxslt-1.10.1/2.0/pipeline-for-svrl.xsl \
        --report tests/schematron/tools/schxslt-crosscheck.txt

SchXslt compiles each schema to XSLT (`sxml transform
pipeline-for-svrl.xsl case.sch`, with `--param=phase=...` for a phase),
the result validates the instance and writes SVRL, and the script
compares the SVRL with the expected result. 265 cases ran (2026-10-28,
a release build of main of that day): 198 agree, 5 are
implementation-defined (outcome recorded), 13 need a binding SchXslt does
not have (xslt, xpath2, xpath3, xpath31; SchXslt rejects them with E0002),
and 49 disagree. Each disagreement, and the clause that decides it:

| Cases | SchXslt | Decided by |
|-------|---------|------------|
| 27 schema-error cases: `error-unknown-element`, `-unqualified-attribute`, `-rule-without-assertion`, `-ns-after-pattern`, `-two-diagnostics-sections`, `-duplicate-id`, `-id-not-a-name`, `-flag-not-a-name`, `-phase-id-missing`, `-diagnostics-in-pattern`, `-dir-bad-value`, `-span-without-class`; `abstract-param-duplicate`, `abstract-without-id`, `abstract-with-is-a`, `abstract-instance-with-rule`, `abstract-instance-with-let`; `extends-abstract-with-context`, `extends-abstract-without-id`, `extends-rule-and-href`, `extends-neither`; `include-wrong-place`, `include-missing-fragment`, `extends-href-wrong-kind`; `phase-reserved-name`; `diagnostic-unknown`; `documents-pattern-variable` | does not check the grammar and constraints; it validates | Annexes A and B, 5.4.4, 5.4.6, 5.4.10, 5.4.12, 5.4.13, 6.4, 7.2 |
| `let-rule-shadows-schema`, `let-rule-shadows-pattern` | allows a rule variable to shadow a global | 5.4.7, 7.2 |
| `let-same-name-two-patterns`, `let-pattern-not-visible-elsewhere` | makes pattern variables global (E0003 for the first, the second runs) | 3.23, 5.4.7 |
| `let-in-name-path`, `abstract-param-in-value-of-and-name`, `name-path-element`, `name-path-attribute`, `name-prefixed` | `name path` gives the value of the path, not the node's name | 5.4.9, Annexes H and J (`fn:node-name`) |
| `value-of-subject-context`, `value-of-rule-subject-context` | evaluates `name` and `value-of` with the rule context | 6.3 |
| `value-of-several-items` | joins the items with spaces | Annexes H and J (`fn:string`) |
| `abstract-param-in-let`, `abstract-param-prefix-variable` | replaces `$part` inside `$parts` (a parameter that is a prefix of a variable's name) | Annexes H and J, 3.11 |
| `abstract-param-in-subject` | does not replace parameters in `subject` | 6.2, 6.4 |
| `include-base-uri`, `extends-href-base-uri` | resolves `doc()` in included queries against the including schema | 6.8 |
| `phase-selects-nothing` | requires at least one active pattern (XTTE0780) | 5.4.13, Annex A (`active*`) |
| `message-markup` | drops whitespace-only text between `emph`, `dir` and `span` | 5.3 |
| `diagnostic-on-report` | omits the diagnostic's `role` | Annex D (informative), Table D.1 and D.3 |
| `svrl-fired-rules` | writes no `fired-rule/@name` for a rule's title | Annex D (informative), Table D.1 |
| `svrl-subordinate-document` | writes no `fired-rule/@document` | Annex D (informative), Table D.1 |

The cases with `since="2025"` (67) are checked by a second, independent
derivation of each expected result from the clause it cites, done without
seeing the results written here.

## Existing suites

Looked for on 2026-10-28:

- **Schematron conformance tests**
  (github.com/schematron/schematron-conformance, schematron.github.io/schematron-conformance):
  49 cases for the 2016 edition (valid / invalid / error outcomes, some
  with SVRL checks). The repository states no licence, so nothing of it is
  reused. Its topics (variable scope and duplicates, undefined variables,
  rule contexts by node kind, base URI fixup, abstract rules, phases,
  documents) are covered here by cases written anew; where it treats
  pattern variables as global (its "pattern variable has global scope")
  the 2025 text (3.23) decides differently.
- **SchXslt** (MIT): its own tests are unit tests of its XSLT and were not
  reused; SchXslt itself is the cross-check's reference.
- **XQS** (github.com/AndrewSales/XQS, Apache-2.0), an XQuery
  implementation that aims at the 2025 edition: BaseX unit tests of its
  own; not reused. It is a candidate second reference for the 2025-only
  cases.
- No suite on schematron.com.

## Licence and provenance

The suite is part of sxml and under its licence (MIT). Every schema,
instance and expected result was written for this suite from the text of
ISO/IEC 19757-3:2025. The standard is copyrighted and not included: the
cases cite clause numbers and the coverage list paraphrases each
requirement in a few words; no sentence, example or schema of the
standard is copied.
