# Validating with XSD

<!-- examples: spec -->

A draft can be well-formed and still be wrong: a requirement that says
"MIGHT", which is not one of the key words of BCP 14; a cross reference
to an anchor that no section has; a draft whose name, written twice,
says `-01` in one place and `-02` in the other. A *schema* describes
what a valid document of some kind looks like, so a program can check
its input once, where it comes in, instead of everywhere it uses it. XSD (XML
Schema Definition Language) is the schema language of the W3C, and sxml
implements its current version, XSD 1.1.

The first half of this chapter works in the `spec` directory, on the
Vane draft and its schema `vane.xsd`. From [Namespaces and Larger
Schemas](xsd-namespaces.md) on, it moves to the `scxml` directory and
the schemas of the feature flag's state machine, which has namespaces, keys and
assertions of its own.

## A schema for the draft

A schema is an XML document in the namespace
`http://www.w3.org/2001/XMLSchema`. It declares *elements* and
*attributes*, and gives each a *type*. *Simple types* describe text:
numbers, dates, strings with restrictions. *Complex types* describe
elements with attributes and children.

`vane.xsd` describes the part of the xml2rfc vocabulary that the draft
uses. Here is its end, the declaration of the document element `rfc`:

```xml
{{#include ../examples/spec/vane.xsd:207:259}}
```

Read from the top:

- `rfc` contains a `front`, a `middle` of one or more sections, and
  maybe a `back`, *in that order*. `type="front"` and `type="section"`
  name types that are declared earlier in the schema.
- Its attributes each have a type: `version` is an integer and must be
  3, `category` is one of five words, and `docName` has the type
  `draft-name`, a string that must look like the name of an
  Internet-Draft.
- `xs:assert` is a rule in XPath that a valid `rfc` must meet. It is
  explained below.
- `xs:unique` and `xs:keyref` are rules about values across the whole
  document: no two anchors are the same, and every `xref` names one.

The rest of the schema, from the top, declares the types the draft
uses. [Simple Types and Facets](xsd-simple-types.md) and [Complex Types
and Content Models](xsd-complex-types.md) read it part by part.

## Validating

`sxml validate` checks documents against the schema. Both drafts are
valid, so it prints nothing, and its exit status is 0:

```console
$ sxml validate --schema=vane.xsd vane.xml vane-00.xml; echo "exit status $?"
exit status 0
```

To see what an invalid draft looks like, change a copy of the draft
with `sed` and validate it from standard input, which `-` names. This
copy has a requirement that says "MIGHT", an `xref` to a term that
doesn't exist, and a two-digit year:

```console
$ sed -e 's/<bcp14>MUST NOT/<bcp14>MIGHT/' \
      -e 's/target="term-percentage"/target="term-percentages"/' \
      -e 's/<date year="2026"/<date year="26"/' vane.xml \
  | sxml validate --schema=vane.xsd -
-:20:11: cvc-datatype-valid.1.2.1: attribute 'year' has the value '26', which is not valid for type 'xs:gYear'
-:90:37: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
-:88:9: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
```

sxml reports every error, not only the first. Each one has the line and
column, the name of the rule in the XSD specification that failed (such
as `cvc-enumeration-valid`), the value, and what was expected. The
`xref` comes last although it is on line 88: rules across the document,
such as the keyref, are checked at the end of the element they belong
to, here `rfc`, when every anchor is known. [Reading Validation
Errors](xsd-errors.md) goes through the messages in detail.

For programs, `--format=json` writes the same report as JSON:

```console
$ sxml validate --schema=vane.xsd --format=json vane.xml
{"schema":{"documents":["vane.xsd"],"status":"valid"},"files":[{"file":"vane.xml","status":"valid"}]}
```

## Rules across values

The draft's name is written twice: in the `docName` attribute of `rfc`,
and in the `seriesInfo` of the front matter, which is what the rendered
draft shows. Nothing about the structure of the document says that the
two must agree, but XSD 1.1 can say it with an *assertion*: an XPath
condition on the element, here

```xml
<xs:assert test="not(@docName) or
                 @docName = front/seriesInfo[@name = 'Internet-Draft']/@value"/>
```

A draft whose `docName` was updated for `-02` while its `seriesInfo`
was not breaks it:

```console
$ sed 's/docName="draft-example-vane-protocol-01"/docName="draft-example-vane-protocol-02"/' vane.xml \
  | sxml validate --schema=vane.xsd -
-:7:1: cvc-assertion: element 'rfc' breaks the assertion 'not(@docName) or                        @docName = front/ser...' of an anonymous type derived from 'xs:anyType'
```

The message shows the start of the assertion, so you can find it in the
schema. Inside the test, the context item is the `rfc` element, with its
attributes and children already validated, so the test can rely on
their types. [XSD 1.1 Features](xsd-11.md) has more assertions.

## Strict and lax

A reviewer sends notes about a short draft, with the draft inside them:

`xsd-review.xml`:

```xml
{{#include ../examples/spec/xsd-review.xml}}
```

`vane.xsd` declares no `review` element. By default, validation is
*strict*: the document element must have a declaration in the schema,
so the review is invalid. sxml still looks inside it, and validates the
elements it does know:

```console
$ sxml validate --schema=vane.xsd xsd-review.xml
xsd-review.xml:3:1: cvc-elt.1: no global declaration governs element 'review'
xsd-review.xml:15:22: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
```

With `--mode=lax`, elements without a declaration are let through, and
the ones with a declaration are validated wherever they are. The review
itself is no longer an error, but the draft inside it still is:

```console
$ sxml validate --mode=lax --schema=vane.xsd xsd-review.xml; echo "exit status $?"
xsd-review.xml:15:22: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
exit status 1
```

Only the *global* declarations, the ones directly inside `xs:schema`,
can be found this way. In `vane.xsd` that is only `rfc`: a `section`
on its own is not validated even in lax mode, because the schema
declares sections only inside other types.

## DTDs

Older documents often carry a DTD, the schema language built into XML
itself, in a *document type declaration* at the top. xml2rfc has one
too, for authors who prefer it. A DTD can be part of the document, in
its *internal subset*:

`xsd-dtd.xml`:

```xml
{{#include ../examples/spec/xsd-dtd.xml}}
```

`sxml check --validate-dtd` validates a document against its DTD. This
editorial note has no `anchor`, which its DTD requires:

```console
$ sxml check --validate-dtd xsd-dtd.xml
xsd-dtd.xml:5:1: cvc-complex-type.4: element 'cref' lacks the required attribute 'anchor'
```

A DTD knows no types beyond names and IDs, and nothing about
namespaces; that is why vocabularies with namespaces and typed values
use XSD. [Files and the Outside World](files.md) shows external DTDs
and entities, and Appendix A has the details of `sxml check`.

## What's in this chapter

The rest of the chapter teaches XSD step by step:

- [Simple Types and Facets](xsd-simple-types.md): built-in types,
  restrictions, lists and unions;
- [Complex Types and Content Models](xsd-complex-types.md): sequences,
  choices, `all`, attributes, mixed, simple and empty content, groups;
- [Deriving Types](xsd-derivation.md): extension, restriction,
  `xsi:type`, substitution groups and `xsi:nil`;
- [Namespaces and Larger Schemas](xsd-namespaces.md): target
  namespaces, import, wildcards and schema hints, on the flag's state
  machine;
- [Keys and References](xsd-identity.md): `xs:unique`, `xs:key` and
  `xs:keyref`;
- [XSD 1.1 Features](xsd-11.md): assertions, conditional types, open
  content, default attributes and overrides;
- [Reading Validation Errors](xsd-errors.md): what the messages mean,
  and how to fix what they report;
- [Typed Data and Schema-Aware Queries](xsd-typed-data.md): using the
  types in XQuery and XSLT.

A schema is also a good source for code: [Project: Generating Code from
a Schema](codegen.md) turns one into TypeScript types and runtime
checks.
