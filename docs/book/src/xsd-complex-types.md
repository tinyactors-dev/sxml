# Complex Types and Content Models

<!-- examples: xsd -->

A *complex type* describes an element that has attributes, children,
or both. Its *content model* says which children it may have, how
many, and in which order. This section builds a schema for the
library's events, and uses every kind of content XSD has.

## The schema

`events.xsd`:

```xml
{{#include ../examples/xsd/events.xsd}}
```

And a document it describes:

`events.xml`:

```xml
{{#include ../examples/xsd/events.xml}}
```

```console
$ sxml validate --schema=events.xsd events.xml
```

## Elements and their declarations

`xs:element` *declares* an element: its name and its type. An element
declared directly in `xs:schema` is *global*: it can be the document
element, and other declarations can refer to it with `ref`. The others
are *local*: they exist only inside the content model where they are
declared. In `events.xsd`, `events` is global and everything else is
local.

A type can be *named*, with `xs:complexType name="event"` at the top
level, and used by name, `type="event"`; or *anonymous*, written inside
the element declaration, as for `events`. Name a type when it's used in
more than one place, or when you want to refer to it from XQuery or
XSLT.

## Sequences, choices and all

Three *model groups* combine the children:

- `xs:sequence`: the children in this order. An event has a title, then
  its dates, then the rest.
- `xs:choice`: exactly one of the alternatives. An event has a `date`,
  or a `from` and an `until`; the second alternative is itself a
  sequence. Groups nest freely.
- `xs:all`: each child at most once, in any order. A contact's email
  and phone can come in either order, as the first event shows.

`minOccurs` and `maxOccurs` on an element or a group say how often it
may occur. Both are 1 by default; `minOccurs="0"` makes it optional, and
`maxOccurs="unbounded"` removes the upper limit. An event can have up to
three speakers.

## Attributes

`xs:attribute` declares an attribute of a complex type, always with a
simple type. Attributes are optional unless `use="required"`. A
`default` value is added when the attribute is missing; a `fixed` value
is added too, and an attribute that is there must have exactly that
value.

The default is part of the validated document. The first event has no
`kind`, but after validation it has `talk`:

```console
$ sxml xquery --method=adaptive --context-item=events.xml --expression='
    import schema default element namespace "" at "events.xsd";
    (validate { / })//event ! string(@kind)'
"talk"
"workshop"
```

## Four kinds of content

The content of a complex type is one of four kinds:

- **Element-only**: children, and no text except whitespace between
  them. That's `event` and `contact`.
- **Mixed**: children with text between them, with `mixed="true"`.
  That's `text`, the type of `about`, which allows `em` and `book`
  elements anywhere in its text, as often as you like.
- **Simple**: text of a simple type, plus attributes, with
  `xs:simpleContent`. A `fee` is a decimal with a `currency` attribute.
- **Empty**: no content at all, only attributes. A complex type with no
  model group is empty, as `cancelled` is.

## Mistakes in content

This document breaks a rule of each kind:

`bad-events.xml`:

```xml
{{#include ../examples/xsd/bad-events.xml}}
```

```console
$ sxml validate --schema=events.xsd bad-events.xml
bad-events.xml:2:3: cvc-complex-type.4: element 'event' lacks the required attribute 'id'
bad-events.xml:5:5: cvc-complex-type.2.4: element 'contact' is not allowed here; expected 'until'
bad-events.xml:7:18: cvc-complex-type.3.2.2: attribute 'room' is not allowed on element 'event'
bad-events.xml:10:65: cvc-complex-type.2.4: element 'speaker' is not allowed here; expected 'about' or 'contact'
bad-events.xml:16:14: cvc-complex-type.2.3: the text 'Ask at the desk.' is not allowed: element 'contact' has element-only content
bad-events.xml:17:10: cvc-au: attribute 'currency' has the value 'USD', not its fixed value 'EUR'
bad-events.xml:18:16: cvc-complex-type.2.1: the text 'Too few people' is not allowed: element 'cancelled' has empty content
```

Read them in order:

- The first event has no `id`, which is required.
- It has a `from` without an `until`: having chosen the second
  alternative of the choice, it must finish it.
- The second event has an attribute that the type doesn't declare.
  Unlike elements in some other schema languages, attributes must be
  declared too.
- It has four speakers, and the fourth is one too many: after three,
  the schema expects `about` or `contact`.
- The third event has text in an element-only `contact`, a fee in the
  wrong currency, and text in an empty element.

`cvc-complex-type.2.4` is the message you'll see most. It says which
element came where it isn't allowed, and lists the elements that the
content model would accept at that point, which usually shows you the
fix.

## Reusing parts of content models

When several types share attributes or children, name the shared part
once. `xs:attributeGroup` names a set of attributes, and `xs:group` a
model group:

`article.xsd`:

```xml
{{#include ../examples/xsd/article.xsd}}
```

Both `article` and `review` have the credits and the dates:

```console
$ echo '<article created="2026-10-20"><title>New books</title><author>Ada</author><editor>Grace</editor></article>' | sxml validate --schema=article.xsd -
$ echo '<review><author>Alan</author><rating>5</rating></review>' | sxml validate --schema=article.xsd -
-:1:1: cvc-complex-type.4: element 'review' lacks the required attribute 'created'
```

A reference to a group is the same as writing its content in that
place. Groups share parts; the next section shows how types share more,
by deriving one type from another.
