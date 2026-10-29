# Reports, Errors and Exit Statuses

<!-- examples: scxml -->

A finding is read by a person in a terminal, by a script in a build, or
by another tool. `sxml validate` writes the same findings in three
formats for them, and says with its exit status how the validation
went. This chapter reads the reports of `flag.sch` from [Checking the
State Machine](schematron-machine.md), in the `scxml` directory, and
then looks at what happens when the schema itself is wrong.

## Text

The default format is one line per finding, on standard error:

```
flag.scxml:92:3: reachable: No transition leads to the state archived. (flag.sch:34:7)
```

From left to right: the document the finding's subject is in (the
validated file, unless a rule looked into another), the line and column
of the subject; the assertion's `id` (`assert` or `report` when it has none);
its level, when it has one: its `severity`, else its `flag`, else its
`role`; the message; and the place of the assertion in its schema file.
Editors that read compiler output can jump to either place. When the
rules run over the typed copy of a document (with `--schema`, [Rules
Across Documents](schematron-documents.md)), the line and column are
replaced by the node's path.

## Exit statuses

`sxml validate` exits with:

| Status | When |
| --- | --- |
| 0 | Every document is valid: no XSD error and no error finding. Warnings and other findings may have been reported. |
| 1 | A document is invalid or not well-formed, or the XSD schema or the Schematron schema has errors (an unsupported query binding included). |
| 2 | The command line is wrong. |
| 3 | A schema or a document cannot be read. |
| 4 | A limit was exceeded, or the validation needs a feature that sxml does not have yet. |

A finding is an *error finding*, which makes its document invalid, when
its severity is `fatal` or `error`; without a severity, when its role
or one of its flags is `fatal` or `error`, or when it has none of the
three. The other statuses look like this:

```console
$ sxml validate --schematron=flag.sch missing.scxml; echo "exit status $?"
sxml validate: cannot read 'missing.scxml'
exit status 3
$ sxml validate --phase=machine flag.scxml; echo "exit status $?"
sxml validate: --phase and --schematron-param need --schematron
Usage: sxml validate [--schema=SCHEMA]... [--schematron=SCH] [--phase=PHASE] [--schematron-param=NAME=VALUE]... [--use-schema-location] [--mode=MODE] [--format=FORMAT] FILE...
Try 'sxml validate --help'.
exit status 2
```

## JSON

`--format=json` writes one JSON object on standard output, for all the
documents of the command:

```console
$ sxml validate --schematron=flag.sch --format=json flag-fixed.scxml
{"schematron":{"document":"flag.sch","phase":"#ALL","edition":"2025","status":"valid"},"files":[{"file":"flag-fixed.scxml","status":"valid","findings":[],"phase":"#ALL"}]}
```

`schematron` describes the schema: its file, the phase compiled, the
edition it says it follows (`schematronEdition`), and whether it
compiled. Each document in `files` has its status, the phase in force
(under `#ANY`, the one its `when` chose), and its findings. A finding
has every field there is: `kind` (`failed-assert` or
`successful-report`), `id`, `role`, `flag`, `severity`, `error`, the
`message`, the `document` its node is in (the validated file, or
another one, [Rules Across Documents](schematron-documents.md)), the
`line`, `column` and `byte_offset` of its node there, its `path`, the `pattern`, the `test`, the `assertion`'s place in the
schema, and its `diagnostics` and `properties`.

The book's tools read it like any JSON. A summary per document, for a
build log:

```console
$ sxml validate --schematron=flag.sch --format=json flag.scxml flag-fixed.scxml > report.json
$ sxml xpath 'json-doc("report.json")?files?* ! `{?file}: {?status}, {count(?findings?*[?error])} errors, {count(?findings?*[not(?error)])} warnings`'
"flag.scxml: invalid, 2 errors, 2 warnings"
"flag-fixed.scxml: valid, 0 errors, 0 warnings"
```

New versions of sxml may add fields to the object, but do not change
the ones it has.

## SVRL

SVRL, the Schematron Validation Report Language, is the report format
that the Schematron standard describes, in XML. Other Schematron tools
write it and read it, so it is the format to keep or to pass on.
`--format=svrl` writes the report of one document on standard output:

```console
$ sxml validate --schematron=flag.sch --format=svrl flag.scxml > flag.svrl; echo "exit status $?"
exit status 1
$ head -4 flag.svrl
<?xml version="1.0" encoding="UTF-8"?>
<svrl:schematron-output xmlns:svrl="http://purl.oclc.org/dsdl/svrl" title="Rules of a Vane machine" phase="#ALL" schematronEdition="2025">
  <svrl:ns-prefix-in-attribute-values uri="http://www.w3.org/2005/07/scxml" prefix="sc"/>
  <svrl:ns-prefix-in-attribute-values uri="urn:example:vane" prefix="vane"/>
```

After the namespaces come the patterns, each an `svrl:active-pattern`,
then for each node a rule fired on an `svrl:fired-rule`, and each
finding as an `svrl:failed-assert` or `svrl:successful-report`, with
its id, the node's path in `location`, the test, and the message in
`svrl:text`. The report says which rules ran, not only what failed:
the fired rules are how a reviewer sees that a rule applied at all.
It is XML, so XPath reads it:

```console
$ sxml xpath --context-item=flag.svrl 'count(//*:fired-rule), //*:active-pattern/@id/string()'
36
"states"
"events"
"data"
"requirements"
"draft"
$ sxml xpath --context-item=flag.svrl '//*:failed-assert ! `{@id}: {*:text}`'
"reachable: No transition leads to the state archived."
"used: Nothing handles, sends or raises the event flag.archived."
"declared: The event flag.evaluated is not declared in vane:events."
"implemented: Nothing in the machine implements the draft's req-1."
```

## Errors in the schema

A schema is compiled before any document is read. sxml checks it
against the grammar of Schematron and the rules of the standard, and
compiles each context and test, and reports every error it finds, each
at its place, before it gives up. This schema has a mistake in each
rule:

`broken.sch`:

```xml
{{#include ../examples/scxml/broken.sch}}
```

```console
$ sxml validate --schematron=broken.sch flag.scxml; echo "exit status $?"
broken.sch:14:7: schematron:syntax: an element of the Schematron namespace that the grammar does not define (Annex A)
broken.sch:13:28: schematron:reference: diagnostics names no diagnostic (5.5.2): hint
broken.sch:6:20: err:XTSE0340: the text is not a pattern (XSLT 4.0, 6.3.1)
exit status 1
```

Codes that start with `schematron:` are the standard's own rules: the
grammar (`syntax`), ids that name nothing (`reference`), `include`,
`parameter`, `variable`, `phase` and `query-binding`. The others are the
codes of XPath and XSLT: `XTSE0340` is a context that is no pattern,
here because its predicate is not closed.

The second rule calls a function with the prefix `vane`, which the
schema does not declare, and the first run does not say so. Compiling
has two steps: reading the schema and parsing each expression, which
finds the errors above; then compiling the whole as a stylesheet, which
finds names that are not declared and types that do not fit, and runs
only when the first step found nothing. Fix the three mistakes, and the
fourth appears:

```console
$ sed -e 's/"sc:state\["/"sc:state"/' -e 's/ diagnostics="hint"//' -e '/<check /d' broken.sch > less-broken.sch
$ sxml validate --schematron=less-broken.sch flag.scxml
less-broken.sch:10:16: err:XPST0081: the namespace prefix is not declared
```

An error inside text that a placeholder brought in is reported where
that text is written: at the `value` of the `param`, not at the rule
that uses it.

## Query bindings

A schema's `queryBinding` attribute names the language of its
expressions. The standard defines bindings for XSLT 1.0 to 3.0 and
XPath 1.0 to 3.1, and the 2025 edition adds `xslt4` and `xpath4`. sxml
reads all of them, and a schema without the attribute, as XPath 4.0 and
XSLT 4.0, the languages it implements: there is no XPath 1.0
compatibility mode. Under an XSLT binding (`xslt`, `xslt2`, `xslt3`,
`xslt4`), a schema can hold XSLT declarations, as `flag.sch` does with
`xsl:function` and `xsl:import-schema`; under an XPath binding they are
ignored. Most schemas written for other processors use `xslt2` or
`xslt3` and run unchanged.

Any other binding, such as `xquery`, is refused with an error at the
attribute:

```console
$ sed 's/queryBinding="xslt3"/queryBinding="xquery"/' flag.sch > xquery.sch
$ sxml validate --schematron=xquery.sch flag.scxml; echo "exit status $?"
xquery.sch:7:9: schematron:query-binding: the query binding is not supported; sxml reads xslt, xslt2, xslt3, xslt4, xpath, xpath2, xpath3, xpath31 and xpath4 (6.5): xquery
exit status 1
```
