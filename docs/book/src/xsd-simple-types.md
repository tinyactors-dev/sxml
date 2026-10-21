# Simple Types and Facets

<!-- examples: spec -->

A *simple type* describes text: the value of an attribute, or the
content of an element that has no children and no attributes. XSD has
many simple types built in, and you make your own by restricting them,
by making lists of them, and by joining them in unions.

## Built-in types

These are the built-in types you'll use most. All of them are in the
namespace `http://www.w3.org/2001/XMLSchema`, usually with the prefix
`xs`. The examples come from the draft where it has them:

| Type | Values | Examples |
| --- | --- | --- |
| `xs:string` | any text | `Flag States` |
| `xs:token` | text without leading, trailing or double spaces; these are removed | `trust200902` |
| `xs:boolean` | `true`, `false`, `1`, `0` | `true` |
| `xs:decimal` | exact decimal numbers | `1.0` |
| `xs:integer` | whole numbers | `3`, `-3` |
| `xs:positiveInteger`, `xs:nonNegativeInteger` | whole numbers above zero, or from zero | `21` |
| `xs:double` | floating-point numbers | `1.5E3`, `INF` |
| `xs:date`, `xs:time`, `xs:dateTime` | dates and times of ISO 8601 | `2026-10-21`, `09:30:00` |
| `xs:gYear`, `xs:gYearMonth` | a year, a month of a year | `1997`, `2026-10` |
| `xs:duration` | a length of time | `PT5M`, `PT10M` |
| `xs:anyURI` | a URI | `https://www.rfc-editor.org/info/rfc2119` |
| `xs:NCName` | an XML name without a colon | `req-7` |
| `xs:ID`, `xs:IDREF` | a name unique in the document, and a reference to one | `term-flag` |
| `xs:language` | a language tag | `en-GB` |

`vane.xsd` uses several of them directly: the `year` of a `date` is an
`xs:gYear`, the `version` of `rfc` an `xs:integer`, the `target` of a
`reference` an `xs:anyURI`.

Each type has a *lexical space*, the texts that are allowed, and a
*value space*, the values they stand for. `21`, `021` and `+21` are
three texts for one `xs:positiveInteger`. That matters when values are
compared, as XPath compares typed values, not their texts.

## Restricting a type

Most simple types you write are restrictions: a built-in type, with
*facets* that narrow down its values. These are the first simple types
of `vane.xsd`:

```xml
{{#include ../examples/spec/vane.xsd:11:63}}
```

The facets in them, and the others XSD has:

- `pattern` is a regular expression that the whole text must match. The
  syntax is the one of XPath's `matches`, without `^` and `$`: a pattern
  always matches the whole value. `draft-[a-z0-9]+(-[a-z0-9]+)*-[0-9]{2}`
  is `draft-`, words of small letters and digits joined by hyphens, and
  a two-digit version.
- `enumeration` lists the allowed values, one facet for each value. A
  `keyword` is one of the eleven key words of BCP 14, and a `month` one
  of the twelve names xml2rfc writes.
- `minInclusive`, `maxInclusive`, `minExclusive` and `maxExclusive`
  limit ordered values: numbers, dates, durations. A `day` is a positive
  integer up to 31.
- `minLength`, `maxLength` and `length` limit the length: the number of
  characters of a string, or the number of items of a list.
- `totalDigits` and `fractionDigits` limit the digits of decimals.
- `whiteSpace` says what happens to spaces: `preserve` keeps them,
  `replace` turns tabs and line ends into spaces, and `collapse` also
  removes leading, trailing and double spaces. `xs:token` is
  `xs:string` with `whiteSpace` set to `collapse`.

`anchor` restricts `xs:NCName` with no facets at all. That is still
worth a name: the schema says what an anchor is once, and every
attribute that holds one says `type="anchor"`.

`xs:annotation` and `xs:documentation` hold text for people. They
change nothing about validation, but tools can read them; the state
machine's documentation in [Documenting the Machine in
XHTML](scxml-xhtml.md) does.

A draft with a mistake for three of the types:

```console
$ sed -e 's/vane-protocol-01"/vane-protocol-1"/' \
      -e 's/month="October" day="21"/month="Oct" day="32"/' vane.xml \
  | sxml validate --schema=vane.xsd -
-:8:6: cvc-pattern-valid: attribute 'docName' has the value 'draft-example-vane-protocol-1', which is not valid for type 'draft-name': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is 'draft-[a-z0-9]+(-[a-z0-9]+)*-[0-9]{2}'
-:20:23: cvc-enumeration-valid: attribute 'month' has the value 'Oct', which is not valid for type 'month': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', ...
-:20:35: cvc-maxInclusive-valid: attribute 'day' has the value '32', which is not valid for type 'day': the value is above the maxInclusive facet (XSD 1.1 Part 2, 4.3.7.4); the maxInclusive facet is '31'
-:7:1: cvc-assertion: element 'rfc' breaks the assertion 'not(@docName) or                        @docName = front/ser...' of an anonymous type derived from 'xs:anyType'
```

Each message names the facet that failed and the facet's value. When
the text isn't even a value of the base type, as `26` isn't an
`xs:gYear` in the chapter's first example, the message is
`cvc-datatype-valid.1.2.1` instead. The last message is a consequence
of the first edit: the `seriesInfo` still says `-01`, so the two names
no longer agree.

A restriction can restrict a type of your own, and the facets add up:
a restriction of `day` with `maxInclusive` 29 allows 1 to 29. It can
only narrow, never widen: a `maxInclusive` of 40 on a type whose
maximum is 31 is an error in the schema.

```console
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:simpleType name="day">
    <xs:restriction base="xs:positiveInteger"><xs:maxInclusive value="31"/></xs:restriction>
  </xs:simpleType>
  <xs:simpleType name="long-day">
    <xs:restriction base="day"><xs:maxInclusive value="40"/></xs:restriction>
  </xs:simpleType>
</xs:schema>' | sxml validate --schema=- vane.xml
-:6:32: maxInclusive-valid-restriction: a maxInclusive facet must lie within the base type's bounds (XSD 1.1 Part 2, 4.3.7.4)
```

## Lists

A *list type* holds several values in one text, separated by spaces.
`levels.xsd` adds two types to the draft's, for a summary that names
the key words a draft uses and the day of its next meeting:

`levels.xsd`:

```xml
{{#include ../examples/spec/levels.xsd}}
```

`xs:include` brings in the components of `vane.xsd`, so `keyword` and
`day` can be used here; [Namespaces and Larger
Schemas](xsd-namespaces.md) says more about combining schema documents.

`levels` is a list of `keyword`, so `MUST SHOULD MAY` is three values,
each a keyword. The list type is written inside a restriction, which
adds a facet: on a list, `length`, `minLength` and `maxLength` count
the items, so a summary names at most four levels.

```console
$ echo '<summary levels="MUST SHOULD MAY" meeting="21"/>' | sxml validate --schema=levels.xsd -
$ echo '<summary levels="MUST SHOULD MAY MUST OPTIONAL"/>' | sxml validate --schema=levels.xsd -
-:1:10: cvc-maxLength-valid: attribute 'levels' has the value 'MUST SHOULD MAY MUST OPTIONAL', which is not valid for type 'levels': the value is longer than the maxLength facet (XSD 1.1 Part 2, 4.3.3.4); the maxLength facet is '4'
```

The items of a list are separated by whitespace, always. Two of the key
words have a space inside, so a list of keywords can't hold them:
`MUST NOT` is the two items `MUST` and `NOT`, and `NOT` is not a
keyword:

```console
$ echo '<summary levels="MUST NOT"/>' | sxml validate --schema=levels.xsd -
-:1:10: cvc-enumeration-valid: attribute 'levels' has the value 'MUST NOT', which is not valid for type 'levels': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4)
```

A list type is the right choice for names and numbers, which never
contain spaces. Lists are handy in attributes, where you can't repeat
an element. XSD's own `xs:IDREFS` and `xs:NMTOKENS` are lists.

## Unions

A *union type* allows the values of any of its member types. A
`meeting-day` is a `day`, or the word `open`. The member types are
tried in order, and the first that accepts the text gives the value its
type. `32` is not a day, and `soon` is not `open`, so neither is a
`meeting-day`:

```console
$ echo '<summary levels="MAY" meeting="open"/>' | sxml validate --schema=levels.xsd -
$ echo '<summary levels="MAY" meeting="32"/>' | sxml validate --schema=levels.xsd -
-:1:23: cvc-datatype-valid.1.2.1: attribute 'meeting' has the value '32', which is not valid for type 'meeting-day'
```

The members can be named, with `memberTypes="day"`, or written inside
the `xs:union`, as the token is in `levels.xsd`, or both.

## What validation does to values

A validated element or attribute has the value its type says, not the
text it was written with. `import schema` gives a query the schema's
declarations and `validate` validates a node and returns a typed copy
([Typed Data and Schema-Aware Queries](xsd-typed-data.md) explains
both). Then the values show what the schema did:

```console
$ sxml xquery --method=adaptive --expression='
    import schema default element namespace "" at "levels.xsd";
    for $summary in (<summary levels=" MUST  MAY " meeting="07"/>,
                     <summary levels="MAY" meeting="open"/>)
    let $valid := validate { $summary }
    return (count(data($valid/@levels)), data($valid/@meeting),
            type-of(data($valid/@meeting)))'
2
7
"Q{}day"
1
"open"
"xs:token"
```

The levels are two values, because their type is a list, and the extra
spaces are gone. `07` is the number 7, and its type is `day`, the member
of the union that accepted it; `open` is a token. `Q{}day` is the type
`day` in no namespace, written as an *expanded name*.

The same works on the draft. Its `year` is a year, so it compares with
`xs:gYear("2010")`, and its `day` is a number:

```console
$ sxml xquery --method=adaptive --context-item=vane.xml --expression='
    import schema default element namespace "" at "vane.xsd";
    let $draft := validate { / }
    return (
      data($draft/rfc/front/date/@day) + 1,
      $draft//reference[front/date/@year lt xs:gYear("2010")]/@anchor/string()
    )'
22
"RFC2119"
"RFC5234"
```

Whitespace is part of the value too. `keyword` restricts `xs:string`,
whose `whiteSpace` is `preserve`, so a line break inside a `bcp14`, as
an editor might make when it wraps a long line, makes the key word
invalid (the message writes the line break as a space, so that each
message stays on one line):

```console
$ sed 's/<bcp14>MUST NOT</<bcp14>MUST\n        NOT</' vane.xml | sxml validate --schema=vane.xsd -
-:94:37: cvc-enumeration-valid: element 'bcp14' has the value 'MUST         NOT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'REQUIRED', 'SHALL', 'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', ...
```

Had `keyword` restricted `xs:token`, the line break and the spaces
would have collapsed to one space, and the text would be `MUST NOT`.
Choose `xs:token` for words and names that people type, and
`xs:string` when every space counts.

## Facets that don't fit

Not every facet applies to every type: `fractionDigits` makes sense for
decimals, not for strings, and a minimum above the maximum allows
nothing. Such mistakes are errors in the schema, and sxml reports them
before it reads any document:

```console
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:simpleType name="anchor">
    <xs:restriction base="xs:NCName">
      <xs:fractionDigits value="2"/>
    </xs:restriction>
  </xs:simpleType>
</xs:schema>' | sxml validate --schema=- vane.xml
-:4:7: cos-applicable-facets: the facet does not apply to the base type's variety and primitive type (XSD 1.1 Part 2, 4.1.5)
```

XSD 1.1 adds two facets: `assertion`, an XPath condition on the
value, and `explicitTimezone`, which requires or forbids a timezone on
dates and times. [XSD 1.1 Features](xsd-11.md) shows both.
