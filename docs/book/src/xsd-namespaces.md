# Namespaces and Larger Schemas

<!-- examples: xsd -->

The schemas so far describe elements in no namespace. Most real
vocabularies have one: XHTML, Atom, SVG, and your company's formats
too. This section shows how a schema describes a namespace, and how a
schema is split into several documents.

## A target namespace

`targetNamespace` on `xs:schema` puts every global declaration and
named type of the schema into that namespace. Here is a schema for the
fines that members pay, in the namespace `https://example.org/lending`:

`lending.xsd`:

```xml
{{#include ../examples/xsd/lending.xsd}}
```

Inside the schema, references to its own components are QNames in the
target namespace. That's why the schema binds the target namespace as
its default namespace, with `xmlns="https://example.org/lending"`, so
that `type="fine"` and `type="member"` find them. Built-in types keep
their `xs:` prefix.

`elementFormDefault="qualified"` puts the *local* elements, such as
`fine` and `amount`, into the target namespace as well. Without it,
only global elements are in the namespace, and local elements are in
no namespace, which surprises almost everyone; most schemas say
`qualified`. Attributes are different: unprefixed attributes are in no
namespace, and `attributeFormDefault` stays at its default,
`unqualified`.

A document in the namespace:

`fines.xml`:

```xml
{{#include ../examples/xsd/fines.xml}}
```

```console
$ sxml validate --schema=lending.xsd fines.xml
```

## Namespace mistakes

The same document without the namespace has elements that the schema
doesn't know at all. A document with the right prefixes on the wrong
elements fails where they don't match:

`fines-wrong-prefix.xml`:

```xml
{{#include ../examples/xsd/fines-wrong-prefix.xml}}
```

```console
$ sxml validate --schema=lending.xsd fines-no-namespace.xml fines-wrong-prefix.xml
fines-no-namespace.xml:1:1: cvc-elt.1: no global declaration governs element 'fines'
fines-wrong-prefix.xml:3:25: cvc-complex-type.2.4: element 'name' is not allowed here; expected 'l:name'
fines-wrong-prefix.xml:4:5: cvc-complex-type.2.4: element 'm:amount' is not allowed here; expected 'l:amount'
```

The messages write names with the prefixes of the document. `name` has
no prefix, so it's in no namespace, but the schema wants it in the
lending namespace, `l:name`. And `amount` has the *type*
`money:amount`, but the *element* is declared in `lending.xsd`, so it
is in the lending namespace. An element's namespace comes from where it
is declared, not from its type.

When `cvc-elt.1` says no declaration governs an element that you can
see in the schema, check its namespace first.

## Include and import

A schema can be split into several documents.

- `xs:include` brings in a schema document with the *same* target
  namespace (or none, in which case its components take the including
  schema's namespace). `people.xsd` declares the type `member` for the
  lending namespace.
- `xs:import` makes the components of *another* namespace available. The
  `money` namespace has its own schema, `money.xsd`, which other
  vocabularies can share. The importing schema binds a prefix for it,
  `money`, and refers to its types with it.

`money.xsd`:

```xml
{{#include ../examples/xsd/money.xsd}}
```

`schemaLocation` is relative to the document that contains it. It's
only a hint on `xs:import`: the components can also come from another
schema document given on the command line. Without the hint, and
without the other document, the reference can't be resolved:

```console
$ sxml validate --schema=lending-no-location.xsd fines.xml
lending-no-location.xsd:22:7: src-resolve: A QName value names no component of its kind: Q{https://example.org/money}amount
$ sxml validate --schema=lending-no-location.xsd --schema=money.xsd fines.xml
```

Every `--schema` adds documents to one schema, which is how you combine
vocabularies that don't import each other.

## Schema hints in documents

Documents often name their schema with `xsi:schemaLocation` (pairs of a
namespace and a location) or `xsi:noNamespaceSchemaLocation` (a location
for names in no namespace). `hinted-fines.xml` is `fines.xml` with such a
hint on its document element:

```xml
<fines xmlns="https://example.org/lending"
       xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
       xsi:schemaLocation="https://example.org/lending lending.xsd">
```

By default `sxml validate` doesn't follow these hints: a document
doesn't get to choose the schema it's checked against, so you name the
schema with `--schema`. Without one, there is nothing to validate
against:

```console
$ sxml validate hinted-fines.xml
sxml validate: missing option '--schema' (or give --use-schema-location)
Usage: sxml validate [--schema=SCHEMA]... [--use-schema-location] [--mode=MODE] [--format=FORMAT] FILE...
Try 'sxml validate --help'.
```

`--use-schema-location` asks for the hints to be followed. The hinted
documents are local files, relative to the document that names them,
and they join the `--schema` documents, if you give any, in one schema
for that document:

```console
$ sxml validate --use-schema-location hinted-fines.xml
$ sxml validate --use-schema-location --schema=lending.xsd hinted-fines.xml
```

A hint only adds a namespace the schema doesn't have yet: with
`--schema=lending.xsd`, the hint for the lending namespace is ignored, so
a document can't swap the schema you chose for one of its own. A hinted
file that doesn't exist isn't an error by itself (XSD 1.1 Part 1, 4.3.2);
the elements it would have declared are then simply undeclared.

## Namespaces in queries

In XQuery, `import schema` binds a prefix to the schema's namespace, or
makes it the default element namespace. Then element names and type
names work as in the schema:

```console
$ sxml xquery --method=adaptive --context-item=fines.xml --expression='
    import schema namespace l = "https://example.org/lending" at "lending.xsd";
    declare namespace money = "https://example.org/money";
    let $fines := validate { / }
    return (
      $fines//l:amount instance of element(l:amount, money:amount),
      data($fines//l:amount) * 2
    )'
true()
4.8
```

The imported `money.xsd` comes along with `lending.xsd`, so its types
are known; `declare namespace` only binds the prefix.
