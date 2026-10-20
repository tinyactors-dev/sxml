# Simple Types and Facets

<!-- examples: xsd -->

A *simple type* describes text: the value of an attribute, or the
content of an element that has no children and no attributes. XSD has
many simple types built in, and you make your own by restricting them,
by making lists of them, and by joining them in unions.

## Built-in types

These are the built-in types you'll use most. All of them are in the
namespace `http://www.w3.org/2001/XMLSchema`, usually with the prefix
`xs`:

| Type | Values | Examples |
| --- | --- | --- |
| `xs:string` | any text | `Dune` |
| `xs:token` | text without leading, trailing or double spaces; these are removed | `Ada Lovelace` |
| `xs:boolean` | `true`, `false`, `1`, `0` | `true` |
| `xs:decimal` | exact decimal numbers | `9.99` |
| `xs:integer` | whole numbers | `-3`, `1965` |
| `xs:positiveInteger`, `xs:nonNegativeInteger` | whole numbers above zero, or from zero | `412` |
| `xs:double` | floating-point numbers | `1.5E3`, `INF` |
| `xs:date`, `xs:time`, `xs:dateTime` | dates and times of ISO 8601 | `2026-10-20`, `09:30:00` |
| `xs:gYear`, `xs:gYearMonth` | a year, a month of a year | `1965`, `2026-10` |
| `xs:duration` | a length of time | `P21D`, `PT2H` |
| `xs:anyURI` | a URI | `https://example.org/` |
| `xs:NCName` | an XML name without a colon | `dune` |
| `xs:ID`, `xs:IDREF` | a name unique in the document, and a reference to one | `dune` |
| `xs:language` | a language tag | `en-GB` |

Each type has a *lexical space*, the texts that are allowed, and a
*value space*, the values they stand for. `1965`, `01965` and `+1965`
are three texts for one `xs:integer`. That matters when values are
compared, as XPath compares typed values, not their texts.

## Restricting a type

Most simple types you write are restrictions: a built-in type, with
*facets* that narrow down its values. Here is a schema for a member of
the library, with a type for every field:

`member.xsd`:

```xml
{{#include ../examples/xsd/member.xsd}}
```

The facets in it:

- `pattern` is a regular expression that the whole text must match. The
  syntax is the one of XPath's `matches`, without `^` and `$`: a pattern
  always matches the whole value. `M[0-9]{4}` is an M and four digits.
- `minLength`, `maxLength` and `length` limit the length: the number of
  characters of a string, or the number of items of a list.
- `enumeration` lists the allowed values, one facet for each value.
- `minInclusive`, `maxInclusive`, `minExclusive` and `maxExclusive`
  limit ordered values: numbers, dates, durations.
- `totalDigits` and `fractionDigits` limit the digits of decimals, as
  the `amount` type of `library.xsd` did.
- `whiteSpace` says what happens to spaces: `preserve` keeps them,
  `replace` turns tabs and line ends into spaces, and `collapse` also
  removes leading, trailing and double spaces. `xs:token` is
  `xs:string` with `whiteSpace` set to `collapse`.

This member is valid:

`member.xml`:

```xml
{{#include ../examples/xsd/member.xml}}
```

```console
$ sxml validate --schema=member.xsd member.xml
```

And this one has a mistake in every field:

`bad-member.xml`:

```xml
{{#include ../examples/xsd/bad-member.xml}}
```

```console
$ sxml validate --schema=member.xsd bad-member.xml
bad-member.xml:1:9: cvc-pattern-valid: attribute 'id' has the value '42', which is not valid for type 'member-id': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is 'M[0-9]{4}'
bad-member.xml:2:3: cvc-minLength-valid: element 'name' has the value '', which is not valid for type 'name': the value is shorter than the minLength facet (XSD 1.1 Part 2, 4.3.2.4); the minLength facet is '1'
bad-member.xml:3:3: cvc-pattern-valid: element 'email' has the value 'ada at example.org', which is not valid for type 'email': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[^@\s]+@[^@\s]+\.[^@\s]+'
bad-member.xml:4:3: cvc-datatype-valid.1.2.1: element 'joined' has the value '1 March 2024', which is not valid for type 'xs:date'
bad-member.xml:5:3: cvc-enumeration-valid: element 'level' has the value 'gold', which is not valid for type 'level': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'basic', 'plus', 'staff'
bad-member.xml:6:3: cvc-maxInclusive-valid: element 'limit' has the value '25', which is not valid for type 'limit': the value is above the maxInclusive facet (XSD 1.1 Part 2, 4.3.7.4); the maxInclusive facet is '20'
bad-member.xml:7:3: cvc-maxLength-valid: element 'interests' has the value 'mathematics engines poetry music', which is not valid for type 'interests': the value is longer than the maxLength facet (XSD 1.1 Part 2, 4.3.3.4); the maxLength facet is '3'
bad-member.xml:8:3: cvc-datatype-valid.1.2.1: element 'phone' has the value '555-0100', which is not valid for type 'phone'
```

Each message names the facet that failed and the facet's value. When
the text isn't even a value of the base type, as `1 March 2024` isn't
an `xs:date`, the message is `cvc-datatype-valid.1.2.1`.

A restriction can restrict a type of your own, and the facets add up:
a restriction of `limit` with `maxInclusive` 10 allows 1 to 10. It can
only narrow, never widen: a `maxInclusive` of 30 on `limit` is an
error in the schema.

## Lists

A *list type* holds several values in one text, separated by spaces.
`interest-list` is a list of `xs:NCName`, so
`mathematics engines poetry` is three values. On a list, `length`,
`minLength` and `maxLength` count the items; `interests` allows at
most three.

Lists are handy in attributes, where you can't repeat an element.
XSD's own `xs:IDREFS` and `xs:NMTOKENS` are lists.

## Unions

A *union type* allows the values of any of its member types. `phone`
is a phone number in international form, or the word `unlisted`. The
member types are tried in order, and the first that accepts the text
gives the value its type. `555-0100` fits neither, so it's not a
`phone`.

The members can be named, with `memberTypes="xs:date xs:gYear"`, or
written inside the `xs:union`, as in `member.xsd`.

## What validation does to values

A validated element has the value its type says, not the text it was
written with. `--context-item` gives XQuery the document, `validate`
gives it the types (Chapter 5's last section explains how), and then
the values show what the schema did:

```console
$ sxml xquery --method=adaptive --context-item=member.xml --expression='
    import schema default element namespace "" at "member.xsd";
    let $member := (validate { / })/member
    return (
      data($member/name),
      count(data($member/interests)),
      data($member/joined) + xs:yearMonthDuration("P1Y")
    )'
"Ada Lovelace"
3
xs:date("2025-03-01")
```

The name lost its extra spaces, because `xs:token` collapses them. The
interests are three values, because their type is a list. And
`joined` is a date, so adding a year to it is date arithmetic. Without
the schema, the same name is the text as it is written:

```console
$ sxml xquery --method=adaptive --context-item=member.xml --expression='string(/member/name)'
"  Ada   Lovelace "
```

## Facets that don't fit

Not every facet applies to every type: `fractionDigits` makes sense for
decimals, not for strings, and a minimum above the maximum allows
nothing. Such mistakes are errors in the schema, and sxml reports them
before it reads any document:

```console
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:simpleType name="code">
    <xs:restriction base="xs:string">
      <xs:fractionDigits value="2"/>
    </xs:restriction>
  </xs:simpleType>
</xs:schema>' | sxml validate --schema=- member.xml
-:4:7: cos-applicable-facets: the facet does not apply to the base type's variety and primitive type (XSD 1.1 Part 2, 4.1.5)
```

XSD 1.1 adds two facets: `assertion`, an XPath condition on the
value, and `explicitTimezone`, which requires or forbids a timezone on
dates and times. [XSD 1.1 Features](xsd-11.md) shows both.
