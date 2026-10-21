# Complex Types and Content Models

<!-- examples: spec -->

A *complex type* describes an element that has attributes, children,
or both. Its *content model* says which children it may have, how
many, and in which order. This section reads the rest of `vane.xsd`
from the top, and finds every kind of content XSD has in the draft.

## Elements and their declarations

`xs:element` *declares* an element: its name and its type. An element
declared directly in `xs:schema` is *global*: it can be the document
element, and other declarations can refer to it with `ref`. The others
are *local*: they exist only inside the content model where they are
declared. In `vane.xsd`, `rfc` is the only global element, and every
other element is local.

A type can be *named*, with `xs:complexType name="section"` at the top
level, and used by name, `type="section"`; or *anonymous*, written
inside the element declaration, as for `rfc` and `middle`. Name a type
when it's used in more than one place, or when you want to refer to it
from XQuery or XSLT. `vane.xsd` names most of its types: `front` is
used by the draft and by every reference, `text` by every paragraph,
`dt` and `dd`, and `section` by sections at every depth.

Local declarations can give the same name different types in different
places. `t` in a section has the type `text`; `name` in a section is a
plain string; and `front` declares its own `title`, which is not the
`title` of anything else.

## Sequences, choices and all

Here are the front matter and the section:

```xml
{{#include ../examples/spec/vane.xsd:108:121}}
```

```xml
{{#include ../examples/spec/vane.xsd:167:183}}
```

Three *model groups* combine the children:

- `xs:sequence`: the children in this order. The front matter has a
  title, then the series information, the authors, the date, and the
  rest.
- `xs:choice`: exactly one of the alternatives, each time the choice
  occurs. After its `name`, a section has any number of blocks, each a
  paragraph, a definition list or source code, in any order; then its
  subsections. Groups nest freely, so a choice inside a sequence is
  common.
- `xs:all`: each child at most once, in any order. `vane.xsd` has
  none, but xml2rfc's address is a good use: an email, a phone number
  and a web address, in whatever order the author wrote them.

`minOccurs` and `maxOccurs` on an element or a group say how often it
may occur. Both are 1 by default; `minOccurs="0"` makes it optional, and
`maxOccurs="unbounded"` removes the upper limit. A draft has one or more
authors and zero or more keywords; on the section's choice, the two
attributes let the choice repeat.

`xsd-author.xsd` describes an author with a fuller address:

`xsd-author.xsd`:

```xml
{{#include ../examples/spec/xsd-author.xsd}}
```

The address may list its children in any order, but each only once:

```console
$ echo '<author fullname="Ada Byron" surname="Byron"><address><uri>https://example.com/</uri><email>ada@example.com</email></address></author>' | sxml validate --schema=xsd-author.xsd -
$ echo '<author fullname="Ada Byron" surname="Byron"><address><email>ada@example.com</email><phone>1</phone><phone>2</phone></address></author>' | sxml validate --schema=xsd-author.xsd -
-:1:101: cvc-complex-type.2.4: element 'phone' is not allowed here; expected 'uri'
```

## Attributes

`xs:attribute` declares an attribute of a complex type, always with a
simple type. Attributes are optional unless `use="required"`: every
section must have an `anchor`, while a paragraph may have one. A
`default` value is added when the attribute is missing; a `fixed` value
is added too, and an attribute that is there must have exactly that
value. The `version` of `rfc` is fixed at 3.

The default is part of the validated document. A contact of
`xsd-author.xsd` that doesn't say its role has the role `chair` after
validation, and a draft that doesn't say its version has version 3, an
integer:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "xsd-author.xsd";
    (validate { <contact fullname="Grace Hopper" surname="Hopper"/> })/@role/string()'
"chair"
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "vane.xsd";
    let $draft := validate {
      <rfc>
        <front><title>T</title><author fullname="A B" surname="B"/><date year="2026"/></front>
        <middle><section anchor="intro"><name>Introduction</name></section></middle>
      </rfc>
    }
    return ($draft/@version/string(), type-of(data($draft/@version)))'
"3"
"xs:integer"
```

## Four kinds of content

The content of a complex type is one of four kinds, and the draft has
all four:

- **Element-only**: children, and no text except whitespace between
  them. That's `front`, `section` and `rfc`.
- **Mixed**: children with text between them, with `mixed="true"`.
  That's `text`, the type of every paragraph, which allows the inline
  elements anywhere in its text, as often as you like.
- **Simple**: text of a simple type, plus attributes, with
  `xs:simpleContent`. A `title` is a string with an `abbrev` attribute;
  a `sourcecode` is a string with a `type` and a `name`.
- **Empty**: no content at all, only attributes. A complex type with no
  model group is empty, as `date` and `seriesInfo` are.

Here are the mixed and the simple types:

```xml
{{#include ../examples/spec/vane.xsd:65:106}}
```

`xref` and `cref` are mixed types without a model group: text, but no
child elements. That's how xml2rfc writes them: `<xref
target="term-flag">flag</xref>` has the text a reader sees, and
`<xref target="RFC2119"/>` has none, so the renderer writes the
reference's name.

## Mistakes in content

This draft breaks a rule of each kind:

`bad-content.xml`:

```xml
{{#include ../examples/spec/bad-content.xml}}
```

```console
$ sxml validate --schema=vane.xsd bad-content.xml
bad-content.xml:1:6: cvc-au: attribute 'version' has the value '2', not its fixed value '3'
bad-content.xml:3:5: cvc-complex-type.2.4: element 'author' is not allowed here; expected 'title'
bad-content.xml:4:5: cvc-complex-type.2.4: element 'title' is not allowed here; expected 'author' or 'date'
bad-content.xml:5:23: cvc-complex-type.2.1: the text 'October' is not allowed: element 'date' has empty content
bad-content.xml:5:37: cvc-complex-type.2.3: the text '     See the mailing list.   ' is not allowed: element 'front' has element-only content
bad-content.xml:9:5: cvc-complex-type.4: element 'section' lacks the required attribute 'anchor'
bad-content.xml:11:10: cvc-complex-type.3.2.2: attribute 'keyword' is not allowed on element 't'
bad-content.xml:12:7: cvc-complex-type.2.4: element 'name' is not allowed here; expected 't', 'dl', 'sourcecode' or 'section'
```

Read them in order:

- The version is 2, not the fixed 3.
- The author comes before the title. sxml takes the title as missing,
  goes on after the author, and then finds the title too late.
- The date has text, but its type is empty: the month belongs in the
  `month` attribute.
- The front matter has text between its elements; only whitespace is
  allowed there.
- The section has no anchor, which is required.
- The paragraph has an attribute that `text` doesn't declare. Unlike
  elements in some other schema languages, attributes must be declared
  too.
- The section has a second `name`, after a paragraph, where only
  blocks and subsections may come.

`cvc-complex-type.2.4` is the message you'll see most. It says which
element came where it isn't allowed, and lists the elements that the
content model would accept at that point, which usually shows you the
fix.

## Reusing parts of content models

When several types share attributes or children, name the shared part
once. `xs:group` names a model group, and `xs:attributeGroup` a set of
attributes:

- `vane.xsd` names the inline elements `inline`, and `text` refers to
  the group with `<xs:group ref="inline" .../>`. A schema for more of
  xml2rfc would refer to it from the other places that allow inline
  text, such as list items and table cells.
- `xsd-author.xsd` names the attributes of a person `names`, and both
  `author` and `contact` refer to them.

```console
$ echo '<contact surname="Hopper"/>' | sxml validate --schema=xsd-author.xsd -
-:1:1: cvc-complex-type.4: element 'contact' lacks the required attribute 'fullname'
```

A reference to a group is the same as writing its content in that
place. Groups share parts; the next section shows how types share more,
by deriving one type from another.
