# Deriving Types

<!-- examples: spec -->

Simple types are derived by restriction, as [Simple Types and
Facets](xsd-simple-types.md) showed. Complex types can be derived too,
in two ways: *extension* adds content to a type, and *restriction*
narrows it. A derived type can be used wherever its base type is
expected, which gives XSD something like the subclasses of
object-oriented languages.

## Extension

`latch.xsd` already has two extensions, of a simple type. A `title` is
an `xs:string` with an `abbrev` attribute, and a `sourcecode` is an
`xs:string` with a `type` and a `name`:

```xml
{{#include ../examples/spec/latch.xsd:159:165}}
```

`xs:simpleContent` with `xs:extension` adds attributes to a simple
type, and the result is a complex type with simple content.

An editor who works on the draft keeps a worksheet: the paragraphs to
look at, some source code, and the day the next draft is due. Its
schema, `xsd-derivation.xsd`, includes `latch.xsd` and derives new
types from the draft's:

`xsd-derivation.xsd`:

```xml
{{#include ../examples/spec/xsd-derivation.xsd}}
```

`changed-requirement` extends `requirement` with a `since` attribute
that names the draft that changed it. An extension's content is the
base type's content followed by its own, and its attributes are the
base type's and its own. Here only an attribute is added, so a changed
requirement has the mixed content of every paragraph.

## Restriction

A restriction of a complex type allows a subset of what its base type
allows. It writes out the whole content again, narrowed. `requirement`
is a `text` whose `anchor` is required: every requirement is a valid
paragraph, but not every paragraph is a requirement. sxml checks, when
it compiles the schema, that a restriction really narrows. Making the
anchor optional again in a restriction of `requirement` is an error:

```console
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:include schemaLocation="xsd-derivation.xsd"/>
  <xs:complexType name="loose-requirement" mixed="true">
    <xs:complexContent>
      <xs:restriction base="requirement">
        <xs:group ref="inline" minOccurs="0" maxOccurs="unbounded"/>
        <xs:attribute name="anchor" type="anchor"/>
      </xs:restriction>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>' | sxml validate --schema=- xsd-worksheet.xml
-:3:3: derivation-ok-restriction.3: The attributes of a restriction are not allowed by its base
```

## Choosing the type in the document: xsi:type

The worksheet declares its paragraphs as `t` with the type `text`. In
the document, the attribute `xsi:type` says that a paragraph has a type
derived from `text`. `xsi` is the namespace
`http://www.w3.org/2001/XMLSchema-instance`, which XSD reserves for
attributes that talk to the validator:

`xsd-worksheet.xml`:

```xml
{{#include ../examples/spec/xsd-worksheet.xml}}
```

```console
$ sxml validate --schema=xsd-derivation.xsd xsd-worksheet.xml
```

Each `t` is validated by the type its `xsi:type` names, or by `text`
when it has none. These mistakes show the rules:

`bad-worksheet.xml`:

```xml
{{#include ../examples/spec/bad-worksheet.xml}}
```

```console
$ sxml validate --schema=xsd-derivation.xsd bad-worksheet.xml
bad-worksheet.xml:2:3: cvc-complex-type.4: element 't' lacks the required attribute 'anchor'
bad-worksheet.xml:3:3: cvc-complex-type.4: element 't' lacks the required attribute 'since'
bad-worksheet.xml:5:6: cvc-elt.4.3: xsi:type gives element 't' type 'xs:string', which is not validly derived from its declared type, type 'text'
bad-worksheet.xml:6:3: cvc-elt.2: element 'block' has an abstract declaration
bad-worksheet.xml:7:3: cvc-complex-type.2.4: element 'figure' is not allowed here; expected 'block' or 'due'
bad-worksheet.xml:8:3: cvc-datatype-valid.1.2.1: element 'due' has the value '', which is not valid for type 'xs:date'
```

- The first paragraph says it is a requirement, but has no anchor.
- The second is a changed requirement without its `since`.
- The third names a type that isn't derived from `text` at all.
  sxml ignores that `xsi:type`, and validates the paragraph as `text`.

The last three are about the substitution group and `xsi:nil`, below.

Types derived from the declared type are what make queries over mixed
content easy. With the schema's types, `element(*, requirement)`
matches every element whose type is `requirement` or derived from it,
so the changed requirement counts too:

```console
$ sxml xquery --method=adaptive --context-item=xsd-worksheet.xml --expression='
    import schema default element namespace "" at "xsd-derivation.xsd";
    (validate { / })//t[. instance of element(*, requirement)]/@anchor/string()'
"req-3"
"req-7"
```

## Substitution groups

`xsi:type` keeps one element name and varies the type. A *substitution
group* varies the element name instead: a global element declares that
it can stand in for another, the *head* of the group, wherever the head
is allowed. In the worksheet, `t`, `dl` and `sourcecode` are members of
the group whose head is `block`, and the worksheet's content model
names only `block`.

`block` is `abstract="true"`, so it can't appear itself; only its
members can. That was the fourth mistake above. The fifth, a `figure`,
is no member of the group, and the message names the head, `block`,
which stands for every member. A member's type must be the head's type
or derived from it; `block` has no type, which means `xs:anyType`, so
any type will do.

In XQuery and XSLT, `schema-element(block)` matches the head and all
its members:

```console
$ sxml xquery --method=adaptive --context-item=xsd-worksheet.xml --expression='
    import schema default element namespace "" at "xsd-derivation.xsd";
    (validate { / })/worksheet/*[. instance of schema-element(block)] ! name()'
"t"
"t"
"t"
"sourcecode"
```

In `latch.xsd`, a section's blocks are a choice of three local
elements. A substitution group is the open alternative: another schema
that includes this one can add a member, a `table` say, and every
content model that allows a `block` allows it too, without a change.

## Nothing, explicitly

An element that is required but has no value can be declared
`nillable="true"`. The document then says `xsi:nil="true"`, and the
element must be empty: the worksheet's `due` is there, but no day is
set yet. That's different from an empty element, which is an empty
string, and an empty string is not a date: the sixth mistake.

In XPath, `nilled` tells a nilled element from others:

```console
$ sxml xquery --method=adaptive --context-item=xsd-worksheet.xml --expression='
    import schema default element namespace "" at "xsd-derivation.xsd";
    (validate { / })//due ! nilled(.)'
true()
```

## Controlling derivation

A schema can forbid what others may do with its types. `final` on a
type forbids deriving from it (`final="extension"`, `"restriction"` or
`"#all"`), and `block` on a type or element forbids using derived types
in documents, through `xsi:type` or substitution. `changed-requirement`
is final, so a schema that extends it is in error:

```console
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:include schemaLocation="xsd-derivation.xsd"/>
  <xs:complexType name="reviewed-requirement" mixed="true">
    <xs:complexContent>
      <xs:extension base="changed-requirement">
        <xs:attribute name="reviewer" type="xs:string"/>
      </xs:extension>
    </xs:complexContent>
  </xs:complexType>
</xs:schema>' | sxml validate --schema=- xsd-worksheet.xml
-:3:3: cos-ct-extends.1.1: The base type's final forbids extension
```

Use them for types whose meaning would break if someone extended them,
or for elements that must have exactly their declared type.
