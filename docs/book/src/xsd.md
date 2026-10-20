# Validating with XSD

<!-- examples: xsd -->

A document can be well-formed and still be wrong: a price that is not a
number, a book without a title, two books with the same id. A *schema*
describes what a valid document of some kind looks like, so a program
can check its input once, at the door, instead of everywhere it uses
it. XSD (XML Schema Definition Language) is the schema language of the
W3C, and sxml implements its current version, XSD 1.1.

The examples of this chapter are in `docs/book/examples/xsd`.

## A schema for the library

A schema is an XML document in the namespace
`http://www.w3.org/2001/XMLSchema`. It declares *elements* and
*attributes*, and gives each a *type*. *Simple types* describe text:
numbers, dates, strings with restrictions. *Complex types* describe
elements with attributes and children.

`library.xsd`:

```xml
{{#include ../examples/xsd/library.xsd}}
```

Read from the top:

- The document element is `library`, which contains one or more
  `shelf` elements. `xs:unique` says that the `id` of each book is
  unique in the whole library.
- A shelf has books and a required `name`.
- A book has a title, one or more authors, a price and maybe tags, *in
  that order*, and attributes for its id, year and page count, each with
  its type: `xs:gYear` is a year, `xs:positiveInteger` a whole number
  above zero.
- A price is an `amount`, a decimal of at least zero with at most two
  digits after the point, with a `currency` attribute that must be EUR or
  USD.

## Validating

`sxml validate` checks documents against the schema. The library is
valid, so it prints nothing:

```console
$ sxml validate --schema=library.xsd library.xml
```

This copy of a shelf has several problems:

`damaged.xml`:

```xml
{{#include ../examples/xsd/damaged.xml}}
```

```console
$ sxml validate --schema=library.xsd damaged.xml
damaged.xml:6:14: cvc-enumeration-valid: attribute 'currency' has the value 'GBP', which is not valid for type 'currency': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'EUR', 'USD'
damaged.xml:6:7: cvc-fractionDigits-valid: element 'price' has the value '9.999', which is not valid for type 'amount': the value has more fraction digits than the fractionDigits facet (XSD 1.1 Part 2, 4.3.12.4); the fractionDigits facet is '2'
damaged.xml:8:11: cvc-id.2: the ID 'dune' of attribute 'id' is used twice in the document
damaged.xml:9:7: cvc-complex-type.2.4: element 'author' is not allowed here; expected 'title'
damaged.xml:10:7: cvc-complex-type.2.4: element 'title' is not allowed here; expected 'author' or 'price'
damaged.xml:8:5: cvc-identity-constraint.4.1: element 'book': its key sequence is that of an earlier member (unique 'book-id')
```

sxml reports every error, not only the first. Each one has the line and
column, the name of the rule in the XSD specification that failed (such
as `cvc-complex-type.2.4`), the value, and what was expected. The id
`dune` is reported twice: once because `xs:ID` values must be unique in
a document, and once for the `xs:unique` constraint.

For programs, `--format=json` writes the same report as JSON:

```console
$ sxml validate --schema=library.xsd --format=json library.xml
{"schema":{"documents":["library.xsd"],"status":"valid"},"files":[{"file":"library.xml","status":"valid"}]}
```

## Rules across values

XSD 1.1 adds *assertions*: XPath conditions that a valid element must
meet. They can express rules that connect several values, which
structure alone can't:

`loan.xsd`:

```xml
{{#include ../examples/xsd/loan.xsd}}
```

```console
$ echo '<loan book="dune" from="2026-10-01" until="2026-09-01"/>' | sxml validate --schema=loan.xsd -
-:1:1: cvc-assertion: element 'loan' breaks the assertion '@until gt @from' of an anonymous type derived from 'xs:anyType'
```

Inside the assertion, `@until` and `@from` are dates, as the schema
declares them, so `gt` compares dates.

## Strict and lax

By default, validation is *strict*: the document element must have a
declaration in the schema. With `--mode=lax`, elements without a
declaration are let through, and the ones with a declaration are
validated wherever they are:

```console
$ echo '<shelf><loan book="dune" from="2026-10-01" until="2026-09-01"/></shelf>' | sxml validate --mode=lax --schema=loan.xsd -
-:1:8: cvc-assertion: element 'loan' breaks the assertion '@until gt @from' of an anonymous type derived from 'xs:anyType'
```

## DTDs

Older documents often carry a DTD, the schema language built into XML
itself. `sxml check --validate-dtd` validates a document against its
DTD; Appendix A has the details.

## What's in this chapter

The rest of the chapter teaches XSD step by step:

- [Simple Types and Facets](xsd-simple-types.md): built-in types,
  restrictions, lists and unions;
- [Complex Types and Content Models](xsd-complex-types.md): sequences,
  choices, `all`, attributes, mixed, simple and empty content, groups;
- [Deriving Types](xsd-derivation.md): extension, restriction,
  `xsi:type`, substitution groups and `xsi:nil`;
- [Namespaces and Larger Schemas](xsd-namespaces.md): target
  namespaces, include and import;
- [Keys and References](xsd-identity.md): `xs:unique`, `xs:key` and
  `xs:keyref`;
- [XSD 1.1 Features](xsd-11.md): assertions, conditional types, open
  content, default attributes and overrides;
- [Reading Validation Errors](xsd-errors.md): what the messages mean,
  and how to fix what they report;
- [Typed Data and Schema-Aware Queries](xsd-typed-data.md): using the
  types in XQuery and XSLT.

