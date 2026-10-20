# Keys and References

<!-- examples: xsd -->

Types check each value on its own. Some rules are about values
together: no two books have the same id, every loan names a book that
exists. XSD's *identity constraints* express them: `xs:unique`,
`xs:key` and `xs:keyref`.

## A schema with keys

The records of a small library have books and loans, and four rules
about them:

`records.xsd`:

```xml
{{#include ../examples/xsd/records.xsd}}
```

An identity constraint has three parts:

- where it is declared: on the element whose content it covers, here
  `records`. The rule holds *within* each such element. A constraint on
  a `shelf` element would make ids unique on each shelf, not in the
  whole library;
- a `selector`, a path from that element to the elements the rule is
  about, here `books/book` or `loans/loan`;
- one or more `field`s, paths from each selected element to the values
  that make its *key sequence*, here `@id`, or `@book` and `@date`.

The three kinds differ in what they require:

- `xs:unique`: no two selected elements have the same key sequence.
  Elements that lack a field are left out, so an optional ISBN can be
  unique.
- `xs:key`: the same, and every selected element must have every
  field.
- `xs:keyref`: the key sequence of each selected element must be one of
  a key's (or a unique's) sequences. That's a foreign key, as in a
  database.

## Checking the records

These records are valid:

`records.xml`:

```xml
{{#include ../examples/xsd/records.xml}}
```

```console
$ sxml validate --schema=records.xsd records.xml
```

These break each rule:

`bad-records.xml`:

```xml
{{#include ../examples/xsd/bad-records.xml}}
```

```console
$ sxml validate --schema=records.xsd bad-records.xml
bad-records.xml:4:5: cvc-identity-constraint.4.2.1: element 'book': a key field has no value (key 'book-id')
bad-records.xml:6:5: cvc-identity-constraint.4.2.2: element 'book': its key sequence is that of an earlier member (key 'book-id')
bad-records.xml:5:5: cvc-identity-constraint.4.1: element 'book': its key sequence is that of an earlier member (unique 'book-isbn')
bad-records.xml:10:5: cvc-identity-constraint.4.3: element 'loan': its key sequence is no key sequence of the referenced key (keyref 'loan-book')
bad-records.xml:11:5: cvc-identity-constraint.4.1: element 'loan': its key sequence is that of an earlier member (unique 'loan-day')
```

- The second book has no `id`, which a key requires.
- The fourth book has the id of the third.
- The third book has the ISBN of the first.
- The second loan is of `emma`, which is no book's id.
- The third loan is of the same book on the same day as the first.

Each message names the constraint, so you can find it in the schema.
They are reported in groups, by constraint, at the end of the element
that the constraint is declared on: only then are all the values known.

Values are compared as typed values, not as text: when the field's type
is `xs:integer`, `7` and `007` are the same key.

## Paths in constraints

The paths of `selector` and `field` are a small subset of XPath: child
steps, `.//` at the start, `@` for attributes, `*`, and `|` to combine
paths. There are no predicates and no functions. Names with a namespace
need a prefix that the schema binds, or a default namespace for the
paths: in XSD 1.1, `xpathDefaultNamespace="##targetNamespace"` on
`xs:schema` (or on a `selector` or `field`) makes unprefixed names in
the paths mean the target namespace. Without one of them, `books/book`
means elements in no namespace, and a key in a schema with a target
namespace silently selects nothing.

## Keys or IDs?

`xs:ID` and `xs:IDREF` are simple types with a similar purpose: an `ID`
is unique in the whole document, and an `IDREF` must be one of the
document's IDs. They are older and simpler, but blunt: one namespace of
ids for the whole document, only names (no numbers or dates), and only
one field. Use keys when the rule belongs to a part of the document, or
when the key is a number, a date or several values. The library schema
at the start of this chapter uses both, which is why the duplicate
id in `damaged.xml` was reported twice.
