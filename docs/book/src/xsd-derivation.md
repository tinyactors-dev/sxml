# Deriving Types

<!-- examples: xsd -->

Simple types are derived by restriction, as [Simple Types and
Facets](xsd-simple-types.md) showed. Complex types can be derived too,
in two ways:
*extension* adds content to a type, and *restriction* narrows it. A
derived type can be used wherever its base type is expected, which
gives XSD something like the subclasses of object-oriented languages.

## Extension

The library collects books and films. Both have a title and an id, and
each has fields of its own. The schema says so with an abstract base
type and two extensions:

`media.xsd`:

```xml
{{#include ../examples/xsd/media.xsd}}
```

An extension's content is the base type's content followed by its own:
a `book` is a `title`, then one or more `author`s and `pages`. Its
attributes are the base type's and its own; here, the `id`.

`abstract="true"` on `item` means no element may have that type
itself, only one of the types derived from it.

## Restriction

A restriction of a complex type allows a subset of what its base type
allows. It writes out the whole content again, narrowed: `short-film`
is a `film` whose `minutes` are at most 40. Every valid `short-film` is
also a valid `film`; sxml checks that when it compiles the schema.

## Choosing the type in the document: xsi:type

The `collection` element declares its children as `item`. In the
document, the attribute `xsi:type` says which type derived from `item`
each one has. `xsi` is the namespace
`http://www.w3.org/2001/XMLSchema-instance`, which XSD reserves for
attributes that talk to the validator:

`collection.xml`:

```xml
{{#include ../examples/xsd/collection.xml}}
```

```console
$ sxml validate --schema=media.xsd collection.xml
```

Each `item` is validated by the type its `xsi:type` names. These
mistakes show the rules:

`bad-collection.xml`:

```xml
{{#include ../examples/xsd/bad-collection.xml}}
```

```console
$ sxml validate --schema=media.xsd bad-collection.xml
bad-collection.xml:3:3: cvc-type.2: the type of element 'item' is abstract: type 'item'
bad-collection.xml:9:5: cvc-maxInclusive-valid: element 'minutes' has the value '90', which is not valid for an anonymous type derived from 'xs:positiveInteger': the value is above the maxInclusive facet (XSD 1.1 Part 2, 4.3.7.4); the maxInclusive facet is '40'
bad-collection.xml:11:9: cvc-elt.4.3: xsi:type gives element 'item' type 'xs:string', which is not validly derived from its declared type, type 'item'
bad-collection.xml:11:3: cvc-type.2: the type of element 'item' is abstract: type 'item'
bad-collection.xml:11:40: cvc-complex-type.2.3: the text 'Text' is not allowed: element 'item' has element-only content
```

- The first item has no `xsi:type`, so it would have the abstract type
  `item` itself.
- The second is a `short-film` of 90 minutes.
- The third names a type that isn't derived from `item` at all. sxml
  ignores that `xsi:type`, and goes on with `item`, which brings two
  more errors.

Types derived from the declared type are what make queries over
mixed collections easy. With the schema's types, `element(*, film)`
matches every item whose type is `film` or derived from it, so the
short film counts too:

```console
$ sxml xquery --method=adaptive --context-item=collection.xml --expression='
    import schema default element namespace "" at "media.xsd";
    (validate { / })//item[. instance of element(*, film)]/@id/string()'
"dune-1984"
"spice"
```

## Substitution groups

`xsi:type` keeps one element name and varies the type. A *substitution
group* varies the element name instead: a global element declares that
it can stand in for another, the *head* of the group, wherever the head
is allowed. This schema reuses the types of `media.xsd`, with
`xs:include`:

`media-shelf.xsd`:

```xml
{{#include ../examples/xsd/media-shelf.xsd}}
```

`book` and `film` can appear wherever `entry` can, and since `entry` is
abstract, only they can:

`media-shelf.xml`:

```xml
{{#include ../examples/xsd/media-shelf.xml}}
```

```console
$ sxml validate --schema=media-shelf.xsd media-shelf.xml
$ echo '<shelf><dvd id="x"><title>X</title></dvd><checked/></shelf>' | sxml validate --schema=media-shelf.xsd -
-:1:8: cvc-complex-type.2.4: element 'dvd' is not allowed here; expected 'entry'
-:1:42: cvc-complex-type.2.4: element 'checked' is not allowed here; expected 'entry'
-:1:42: cvc-datatype-valid.1.2.1: element 'checked' has the value '', which is not valid for type 'xs:date'
```

The message names the head, `entry`, which stands for every member of
its group. A member's type must be the head's type or derived from it,
as `book` and `film` are derived from `item`. In XQuery and XSLT,
`schema-element(entry)` matches the head and all its members.

## Nothing, explicitly

An element that is required but has no value can be declared
`nillable="true"`. The document then says `xsi:nil="true"`, and the
element must be empty. That's different from an empty element, which
is an empty string, not a date:

```console
$ echo '<shelf><book id="x"><title>X</title><author>A</author><pages>1</pages></book><checked/></shelf>' | sxml validate --schema=media-shelf.xsd -
-:1:78: cvc-datatype-valid.1.2.1: element 'checked' has the value '', which is not valid for type 'xs:date'
```

In XPath, `nilled` tells a nilled element from others.

## Controlling derivation

A schema can forbid what others may do with its types. `final` on a
type forbids deriving from it (`final="extension"`, `"restriction"` or
`"#all"`), and `block` on a type or element forbids using derived types
in documents, through `xsi:type` or substitution. Use them for types
whose meaning would break if someone extended them.
