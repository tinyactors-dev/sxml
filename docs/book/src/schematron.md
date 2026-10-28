# Validating with Schematron

<!-- examples: schematron -->

A schema in XSD says what a document may contain: which elements, in
which order, with values of which types. Many rules of a real document
are of another kind. The amount of an invoice line is its quantity times
its price; the total is the sum of the lines; an invoice in euros does
not mix currencies. These are statements about values *in relation to
each other*, and they are easier to write as XPath tests than as types.
Schematron (ISO/IEC 19757-3) is the schema language for such rules, and
sxml implements its 2025 edition.

A Schematron schema is a list of *patterns*. Each pattern has *rules*;
each rule has a *context*, an XSLT pattern that says which nodes the rule
applies to, and *assertions*. An `assert` reports when its test is false,
a `report` when its test is true; each report is a *finding*, with a
message that can quote values of the document. The tests are XPath, and
sxml compiles the whole schema into an XSLT stylesheet that it runs over
each document, so everything XPath 4.0 offers is available in them.

## A first schema

`invoice.sch` checks the lines and the total of an invoice:

```xml
{{#include ../examples/schematron/invoice.sch}}
```

- `param` declares a *schema parameter*, a piece of query text that is
  put in place of `$currency` wherever it appears; `let` declares a
  *variable*, evaluated once (at the schema's level) or once per node
  (in a rule).
- The pattern `lines` checks each `line` element; the pattern `totals`
  the `invoice` element. The `value-of` elements of a message are
  evaluated for the node the rule matched.
- `phase` elements name sets of patterns to run: `quick` runs only the
  lines. Without a phase, every pattern runs.

`sxml validate --schematron` runs it. A document without findings gives
no output:

```console
$ sxml validate --schematron=invoice.sch invoice.xml
```

`bad-invoice.xml` has a line without items, a line with too many, two
amounts that are wrong, another currency and a wrong total:

```console
$ sxml validate --schematron=invoice.sch bad-invoice.xml
bad-invoice.xml:3:3: LINE-1: Line 1 orders no pen. (invoice.sch:14:7)
bad-invoice.xml:4:3: LINE-2 warning: Line 2 orders 120 of one item. (invoice.sch:15:7)
bad-invoice.xml:4:3: LINE-3: Line 2: 25.00 is not 30. (invoice.sch:16:7)
bad-invoice.xml:5:3: LINE-3: Line 3: 9.00 is not 8. (invoice.sch:16:7)
bad-invoice.xml:2:1: TOTAL-1 error: The currency is USD, not EUR. (invoice.sch:22:7)
bad-invoice.xml:2:1: TOTAL-2: The total 40.00 is not the sum of the lines. (invoice.sch:23:7)
```

Each line names the place in the document, the assertion's `id`, its
level, the message, and the place of the assertion in the schema. The
level is the assertion's `severity`, else its `flag`, else its `role`.

## Errors and warnings

Not every finding makes a document invalid. A finding is an *error*
when its severity is `fatal` or `error`; without a severity, when its
role or one of its flags is `fatal` or `error`, or when it has neither
a role nor a flag, as most assertions in practice. `LINE-2` has
`severity="warning"`: it is reported, but alone it does not fail the
document, and `sxml validate` exits with status 0 for it. An error
finding gives status 1, as an invalid document does for XSD.

## Phases and parameters

`--phase` chooses a phase; `#ALL` is every pattern, and `#ANY` lets
each document choose: the first phase whose `when` test holds for it.

```console
$ sxml validate --schematron=invoice.sch --phase=quick bad-invoice.xml
bad-invoice.xml:3:3: LINE-1: Line 1 orders no pen. (invoice.sch:14:7)
bad-invoice.xml:4:3: LINE-2 warning: Line 2 orders 120 of one item. (invoice.sch:15:7)
bad-invoice.xml:4:3: LINE-3: Line 2: 25.00 is not 30. (invoice.sch:16:7)
bad-invoice.xml:5:3: LINE-3: Line 3: 9.00 is not 8. (invoice.sch:16:7)
```

`--schematron-param` gives a schema parameter another value. The value
is query text, so a string needs its quotes:

```console
$ sxml validate --schematron=invoice.sch --schematron-param=currency="'USD'" bad-invoice.xml
bad-invoice.xml:3:3: LINE-1: Line 1 orders no pen. (invoice.sch:14:7)
bad-invoice.xml:4:3: LINE-2 warning: Line 2 orders 120 of one item. (invoice.sch:15:7)
bad-invoice.xml:4:3: LINE-3: Line 2: 25.00 is not 30. (invoice.sch:16:7)
bad-invoice.xml:5:3: LINE-3: Line 3: 9.00 is not 8. (invoice.sch:16:7)
bad-invoice.xml:2:1: TOTAL-2: The total 40.00 is not the sum of the lines. (invoice.sch:23:7)
```

Parameters are how one set of rules serves several cases. *Abstract
patterns* take this further: a pattern with `abstract="true"` is a
template whose tests use `$name` placeholders, and each pattern with
`is-a` instantiates it with its own `param` values. The EN 16931 rules
for electronic invoices are written that way: one abstract model of an
invoice, bound once to the UBL syntax and once to CII.

## Reports for programs

`--format=json` gives one object with the schema's status and, for each
file, its findings with every field: the node's line, column and path,
the assertion's place, its pattern and test, and its diagnostics and
properties:

```console
$ sxml validate --schematron=invoice.sch --format=json --phase=quick invoice.xml
{"schematron":{"document":"invoice.sch","phase":"quick","edition":"2025","status":"valid"},"files":[{"file":"invoice.xml","status":"valid","findings":[],"phase":"quick"}]}
```

`--format=svrl` writes the report in SVRL, the XML report format of the
Schematron standard, which other tools read:

```console
$ sxml validate --schematron=invoice.sch --format=svrl --phase=quick invoice.xml
<?xml version="1.0" encoding="UTF-8"?>
<svrl:schematron-output xmlns:svrl="http://purl.oclc.org/dsdl/svrl" title="Invoice rules" phase="quick" schematronEdition="2025">
  <svrl:active-pattern id="lines"/>
  <svrl:fired-rule context="line"/>
  <svrl:fired-rule context="line"/>
</svrl:schematron-output>
```

## Errors in the schema

A schema is compiled before any document is read. sxml checks it
against the grammar and the constraints of the standard, and compiles
every test and context; each error is reported with its place, all of
them at once:

`broken.sch`:

```xml
{{#include ../examples/schematron/broken.sch}}
```

```console
$ sxml validate --schematron=broken.sch invoice.xml
broken.sch:8:7: schematron:syntax: an element of the Schematron namespace that the grammar does not define (Annex A)
broken.sch:7:5: schematron:syntax: a rule has no assert, report, extends or p (Annex A)
broken.sch:4:30: err:XTSE0340: the pattern is incomplete (XSLT 4.0, 6.3.1)
broken.sch:5:27: err:XPST0003: The text is not valid XPath 4.0 syntax (XPath 4.0, Appendix A)
```

Codes starting with `schematron:` are the standard's own rules (the
grammar, references between elements, parameters, variables, includes
and the query binding); the others are those of XPath and XSLT. A
`queryBinding` other than `xslt`, `xslt2`, `xslt3`, `xslt4`, `xpath`,
`xpath2`, `xpath3`, `xpath31` and `xpath4` is refused: sxml reads them
all as XPath 4.0 and XSLT 4.0.

## Schematron and XSD together

The two kinds of schema complement each other, and one command runs
both: `--schema` validates each document against the XSD schema first,
and the Schematron rules then run over the validated copy, whose values
have the types the schema gives them. That copy keeps no line numbers,
so findings name the node by its path:

```console
$ sxml validate --schema=invoice.xsd --schematron=invoice.sch --phase=quick bad-invoice.xml
bad-invoice.xml:/Q{}invoice[1]/Q{}line[1]: LINE-1: Line 1 orders no pen. (invoice.sch:14:7)
bad-invoice.xml:/Q{}invoice[1]/Q{}line[2]: LINE-2 warning: Line 2 orders 120 of one item. (invoice.sch:15:7)
bad-invoice.xml:/Q{}invoice[1]/Q{}line[2]: LINE-3: Line 2: 25.00 is not 30. (invoice.sch:16:7)
bad-invoice.xml:/Q{}invoice[1]/Q{}line[3]: LINE-3: Line 3: 9.00 is not 8. (invoice.sch:16:7)
```

## In programs

The C API (`include/sxml/schematron.h`) compiles a schema once with
`sxml_schematron_create` and validates any number of documents with
`sxml_schematron_validate_node`; each finding goes to a report sink with
both places, the node's and the assertion's. The JavaScript package has
the same shape: `processor.compileSchematron(text, { phase, parameters })`
and `schematron.validate(xml)`, which gives the findings as objects.

Compiling is fast because sxml does not generate stylesheet text: the
rules become the syntax tree of a stylesheet directly. The rules of EN
16931 for UBL invoices, about 2,400 lines in six documents, compile in
about half a second.
