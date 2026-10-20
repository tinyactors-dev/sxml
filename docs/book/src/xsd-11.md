# XSD 1.1 Features

<!-- examples: xsd -->

XSD 1.1 keeps everything of XSD 1.0 and adds what users had missed
most: rules written in XPath, types chosen by attribute values, content
models that are open to extensions, and a clean way to change a schema
without copying it. sxml implements XSD 1.1, and validates XSD 1.0
schemas with the same engine. This section goes through the additions.

## Assertions on values

The `assertion` facet restricts a simple type by an XPath condition, in
which `$value` is the typed value being checked. An ISBN-13 has thirteen
digits, and their sum, with every second digit counted three times, ends
in 0. A pattern can check the digits, and an assertion the sum:

`copies.xsd`:

```xml
{{#include ../examples/xsd/copies.xsd}}
```

The second new facet, `explicitTimezone`, says whether dates and times
must have a timezone (`required`), must not have one (`prohibited`), or
may (`optional`, the default). A moment in time is only one when its
timezone is known.

`copies.xml`:

```xml
{{#include ../examples/xsd/copies.xml}}
```

```console
$ sxml validate --schema=copies.xsd copies.xml
copies.xml:4:9: cvc-assertions-valid: the value '9780465026563' breaks an assertion of type 'isbn'
copies.xml:5:30: cvc-explicitTimezone-valid: attribute 'checked' has the value '2026-10-20T09:15:00', which is not valid for type 'moment': the value's timezone is not what the explicitTimezone facet requires (XSD 1.1 Part 2, 4.3.14.4); the explicitTimezone facet is 'required'
```

The third copy's ISBN has a wrong check digit, and the fourth check
has no timezone.

## Assertions on elements

`xs:assert` in a complex type is a condition on the whole element, as
the loans at the start of this chapter showed. The context item
is the element, with its children and attributes already validated and
typed. This schema describes what the library wants to buy:

`acquisitions.xsd`:

```xml
{{#include ../examples/xsd/acquisitions.xsd}}
```

The assertion `sum(item/price) le @budget` adds up the prices, which
are decimals, and compares the sum with the budget, which is a decimal
too. No structure could say that.

`acquisitions.xml`:

```xml
{{#include ../examples/xsd/acquisitions.xml}}
```

```console
$ sxml validate --schema=acquisitions.xsd acquisitions.xml
```

## Conditional type assignment

`xs:alternative` chooses an element's type by a condition on its
attributes. The first alternative whose `test` is true gives the type;
one without a `test` is the fallback. The declared type, `item`, is
used when no alternative applies, and each alternative's type must be
derived from it. In `acquisitions.xsd`, an item of kind `book` must
have authors, an item of kind `film` must have minutes, and any other
kind gets the type `xs:error`, a type that no element can be valid
against. That's how a schema says "only these kinds".

The tests can only look at attributes, not children: the type must be
known before the children are read.

## Open content

`xs:openContent` lets elements that a content model doesn't name appear
among the ones it does. With `mode="interleave"`, they may appear
anywhere; with `mode="suffix"`, only at the end. The wildcard inside
says which elements: here any element of another namespace, so staff
can add comments in their own namespace, as the valid document does.
`xs:defaultOpenContent`, at the top of a schema, gives every complex
type of the schema the same open content.

## Default attributes

`defaultAttributes` on `xs:schema` names an attribute group that every
complex type of the schema gets, unless it says
`defaultAttributesApply="false"`. In `acquisitions.xsd`, every element
may have a `note`, without declaring it each time.

## Breaking every rule once

`bad-acquisitions.xml`:

```xml
{{#include ../examples/xsd/bad-acquisitions.xml}}
```

```console
$ sxml validate --schema=acquisitions.xsd bad-acquisitions.xml
bad-acquisitions.xml:5:5: cvc-complex-type.2.4: element 'minutes' is not allowed here; expected 'author'
bad-acquisitions.xml:7:9: cvc-type.3.1.1: attribute 'kind' is not allowed on element 'item': its type is simple, type 'xs:error'
bad-acquisitions.xml:8:5: cvc-type.3.1.2: element 'title' is not allowed: element 'item' has simple content of type 'xs:error'
bad-acquisitions.xml:11:3: cvc-complex-type.2.4: element 'comment' is not allowed here; expected 'item' or an element of another namespace
bad-acquisitions.xml:1:1: cvc-assertion: element 'acquisitions' breaks the assertion 'sum(item/price) le @budget' of an anonymous type derived from 'xs:anyType'
```

- The first item is a book, by its kind, so it needs an author, not
  minutes.
- The second has the kind `music`, so its type is `xs:error`, and
  nothing about it can be valid: neither its attributes nor its
  children.
- `comment` is in no namespace, and the open content allows only
  elements of other namespaces.
- The prices add up to more than the budget. The assertion is reported
  last, at the end of the element, when all its content is known.

## Changing a schema: xs:override

Sometimes a schema is almost right: you'd like the library's prices,
but with pounds too. Copying the schema and editing it makes two
schemas to maintain. `xs:override` names a schema document and replaces
some of its components, keeping the rest:

`price.xsd`:

```xml
{{#include ../examples/xsd/price.xsd}}
```

`price-uk.xsd`:

```xml
{{#include ../examples/xsd/price-uk.xsd}}
```

```console
$ sxml validate --schema=price.xsd pounds.xml
pounds.xml:1:8: cvc-enumeration-valid: attribute 'currency' has the value 'GBP', which is not valid for type 'currency': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'EUR', 'USD'
$ sxml validate --schema=price-uk.xsd pounds.xml
```

The `price` element of `price.xsd` now uses the new `currency`. Unlike
XSD 1.0's `xs:redefine`, which sxml supports too, an override doesn't
have to be a restriction or extension of what it replaces.

## Smaller changes

- In `xs:all`, elements may have `maxOccurs` above 1, and wildcards are
  allowed; a type whose content is an `xs:all` can be extended by
  another `xs:all`.
- Wildcards can exclude names with `notQName` and namespaces with
  `notNamespace`.
- `vc:minVersion` and `vc:maxVersion` (in the namespace
  `http://www.w3.org/2007/XMLSchema-versioning`) on any schema element
  make it apply only to some versions of XSD, so a schema can be
  written today for processors of later versions. sxml is version 1.1,
  so `vc:minVersion="1.2"` hides an element from it.
- `xs:error`, as above, and the new built-in types
  `xs:dateTimeStamp` (a dateTime with a required timezone),
  `xs:dayTimeDuration`, `xs:yearMonthDuration` and `xs:anyAtomicType`.
