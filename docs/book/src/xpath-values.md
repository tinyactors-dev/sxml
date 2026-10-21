# Values and Types

<!-- examples: spec -->

Every atomic value in XPath has a type. You rarely have to write a type
down, but knowing they exist explains most of the surprises you'll meet.

## Literals

Numbers and strings can be written directly. The way a number is
written decides its type:

```console
$ sxml xpath '42, 3.14, 1e3, "MUST", true()'
42
3.14
1.0e3
"MUST"
true()
```

`42` is an `xs:integer`, `3.14` an `xs:decimal` and `1e3` an
`xs:double`. Integers and decimals are exact: sxml computes with them
digit by digit, however large they get. Doubles are the floating-point
numbers of most programming languages, with their usual rounding:

```console
$ sxml xpath '0.1 + 0.2, 0.1e0 + 0.2e0'
0.3
3.0000000000000004e-1
$ sxml xpath '9999999999999999999 + 1'
10000000000000000000
```

sxml writes doubles in exponent notation, so you can always tell them
apart. `true()` and `false()` are the two booleans; they are functions
because `true` alone would be the name of an element.

`type-of` tells you the type of any value:

```console
$ sxml xpath 'type-of(1), type-of(1.0), type-of("1"), type-of(1e0)'
"xs:integer"
"xs:decimal"
"xs:string"
"xs:double"
```

## Arithmetic

`+`, `-` and `*` are what you expect. Division is `div`, because `/`
already separates steps. `idiv` divides whole numbers and `mod` gives
the remainder:

```console
$ sxml xpath '7 div 2, 7 idiv 2, 7 mod 2'
3.5
3
1
```

`count` returns an integer, so arithmetic on counts is exact. What share
of the draft's eleven requirements are MUSTs?

```console
$ sxml xpath --context-item=vane.xml 'count(//bcp14[. = "MUST"]), count(//bcp14[. = "MUST"]) div count(//t[@anchor]), count(//bcp14[. = "MUST"]) * 100 idiv count(//t[@anchor])'
5
0.4545454545454545454545454545454545
45
```

Five of eleven: dividing two integers with `div` gives a decimal, with as
many digits as sxml keeps, and `idiv` gives the whole percentage.
[Functions and Operators](xpath-functions.md) shows how to write such a
number nicely.

Dividing an integer or a decimal by zero is an error. Dividing a double
by zero gives infinity, as floating-point numbers do:

```console
$ sxml xpath '1 div 0'
expression:1:1: err:FOAR0001: division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)
$ sxml xpath '1e0 div 0'
INF
```

## Untyped values

A plain XML document has no types in it. The `year` attribute of
`<date year="1997" month="March"/>` is just the characters `1`, `9`,
`9`, `7`. XPath gives such values the type `xs:untypedAtomic`; `data`
takes the value out of a node:

```console
$ sxml xpath --context-item=vane.xml 'type-of((//reference)[1]//date/@year), type-of(data((//reference)[1]//date/@year))'
"attribute()"
"xs:untypedAtomic"
```

An untyped value is converted when it is used:

- in arithmetic, it becomes an `xs:double`;
- compared with a number using `=` or `<`, it becomes a number too;
- compared with a string, it is compared as a string.

That is why the age of each reference, at the draft's date in 2026,
comes out as doubles:

```console
$ sxml xpath --context-item=vane.xml '//reference ! (2026 - .//date/@year)'
2.9e1
1.8e1
9.0e0
1.5e1
```

RFC 2119 is 29 years old. When you want exact arithmetic, convert the
values first with a *constructor function*, named after the type:

```console
$ sxml xpath --context-item=vane.xml '//reference ! (2026 - xs:integer(.//date/@year))'
29
18
9
15
```

Constructor functions exist for every type: `xs:string`, `xs:integer`,
`xs:decimal`, `xs:double`, `xs:boolean`, `xs:date`, `xs:dateTime`,
`xs:duration`, `xs:gYear` and many more. When the text is not a valid
value of the type, they raise an error:

```console
$ sxml xpath 'xs:integer("three")'
expression:1:1: err:FORG0001: the value is not in the lexical space or value space of the target type
```

`castable as` asks first, and `cast as` is another way to write the
conversion:

```console
$ sxml xpath '"three" castable as xs:integer, "3" cast as xs:integer'
false()
3
```

[Validating with XSD](xsd.md) shows how a schema gives the values of a
document their types, so that a year is a year without any conversion.

## Two kinds of comparison

XPath has two sets of comparison operators, and the difference matters.

The *general comparisons*, `=`, `!=`, `<`, `<=`, `>` and `>=`, compare
sequences. They are true if *any* pair of items compares true:

```console
$ sxml xpath --context-item=vane.xml '//bcp14 = "SHALL"'
false()
$ sxml xpath --context-item=vane.xml '//bcp14 = ("MUST", "SHALL")'
true()
```

The first asks "does any key word say SHALL?". BCP 14 allows SHALL as
another way to write MUST, and this draft never uses it. The second asks
whether any key word is MUST or SHALL. This is what makes predicates
over documents so convenient: a reference may have several dates or
authors, and a test such as `[.//date/@year < 2000]` is true if one of
them matches. Here the untyped year is compared with a number, as a
number:

```console
$ sxml xpath --context-item=vane.xml '//reference[.//date/@year < 2000]/@anchor/string()'
"RFC2119"
$ sxml xpath --context-item=vane.xml '//reference[.//date/@year = (1997, 2017)]/@anchor/string()'
"RFC2119"
"RFC8174"
```

The two references of BCP 14 are from 1997 and 2017. It also means `!=`
is not the opposite of `=`: `//bcp14 != "MUST"` is true, because there
are key words that are not MUST.

```console
$ sxml xpath --context-item=vane.xml '//bcp14 != "MUST"'
true()
```

The *value comparisons*, `eq`, `ne`, `lt`, `le`, `gt` and `ge`, compare
exactly one item with one item, and they are strict about types. An
untyped value is compared as a string, so comparing an attribute with a
number is an error:

```console
$ sxml xpath --context-item=vane.xml '/rfc/front/date/@year eq 2026'
expression:1:1: err:XPTY0004: the compared items belong to different type families (XPath 4.0, section 4.11.1; F&O 4.0, fn:compare)
$ sxml xpath --context-item=vane.xml 'xs:integer(/rfc/front/date/@year) eq 2026'
true()
```

Use general comparisons in predicates over documents, and value
comparisons when you know you have single values of known types.

Strings compare by their characters. Numbers compare as numbers. That
makes a difference, as anyone who has sorted RFC numbers as text knows:

```console
$ sxml xpath '"RFC10" lt "RFC9", 10 lt 9'
true()
false()
```

## Dates, times and durations

Dates, times and durations are types of their own, with arithmetic that
knows about calendars. The draft writes its date the xml2rfc way, with
the month as a word:

```xml
{{#include ../examples/spec/vane.xml:20}}
```

To make an `xs:date` from it, we need the month's number. A *map* is a
lookup table, written with curly braces, and calling it with a key gives
the value; [Maps, Arrays and Records](xpath-maps-arrays.md) explains
maps fully. `format-integer` writes the numbers with two digits, and the
string between backticks is a *string template*, which puts the values
of the expressions in curly braces into a string:

```console
$ sxml xpath --context-item=vane.xml 'let $months := { "January": 1, "February": 2, "March": 3, "April": 4, "May": 5, "June": 6, "July": 7, "August": 8, "September": 9, "October": 10, "November": 11, "December": 12 } let $date := /rfc/front/date return xs:date(`{$date/@year}-{format-integer($months($date/@month), "00")}-{format-integer($date/@day, "00")}`)'
xs:date("2026-10-21")
```

sxml writes dates as constructor calls, so you can paste a result back
into an expression. `let` binds a name to a value; it comes back in
[Functions and Operators](xpath-functions.md).

The previous version of the draft is in `vane-00.xml`. `doc` reads
another document, so one expression can look at both. Here are both
dates, -00 first, and the time between them (this time the map has only
the two months it needs):

```console
$ sxml xpath --context-item=vane.xml 'let $months := { "July": 7, "October": 10 } let $dates := (doc("vane-00.xml"), /) ! rfc/front/date ! xs:date(`{@year}-{format-integer($months(@month), "00")}-{format-integer(@day, "00")}`) return ($dates, $dates[2] - $dates[1])'
xs:date("2026-07-06")
xs:date("2026-10-21")
xs:dayTimeDuration("P107D")
```

Subtracting two dates gives a duration. Durations are written as in ISO
8601: `P107D` is 107 days. Adding a duration to a date gives a date. An
Internet-Draft expires 185 days after it is posted, and a month later is
a month later, whatever its length:

```console
$ sxml xpath 'xs:date("2026-10-21") + xs:dayTimeDuration("P185D")'
xs:date("2027-04-24")
$ sxml xpath 'xs:date("2026-01-31") + xs:yearMonthDuration("P1M")'
xs:date("2026-02-28")
$ sxml xpath 'xs:dateTime("2026-10-21T09:30:00Z") + xs:dayTimeDuration("PT90M")'
xs:dateTime("2026-10-21T11:00:00Z")
```

`PT90M` is 90 minutes: the `T` separates the date part of a duration
from its time part.

## Effective boolean values

A predicate or an `if` needs true or false. When the value is something
else, XPath takes its *effective boolean value*: the empty sequence is
false, a sequence that starts with a node is true, a string is true unless
it is empty, a number is true unless it is zero or NaN.

```console
$ sxml xpath --context-item=vane.xml 'boolean(//t[@anchor = "req-12"]), boolean(//cref), boolean(""), boolean("false"), boolean(0)'
false()
true()
false()
true()
false()
```

There is no req-12, so the first is false; the draft has editorial
notes, so the second is true. Note the fourth one: the string `"false"`
is not empty, so it is true. This is what lets a predicate such as
`[@anchor]` mean "has an anchor": the path finds the attribute or
nothing.

## Sequence types

Sometimes you write a type down: in `instance of`, in a function's
signature, or in a variable declaration in XQuery. A *sequence type* is an
item type with an optional occurrence indicator: `?` for zero or one,
`*` for any number, `+` for one or more.

```console
$ sxml xpath '"MUST" instance of xs:string+, () instance of xs:string?'
true()
true()
$ sxml xpath --context-item=vane.xml '//t[bcp14][not(@anchor)] instance of element(t)?, //bcp14 instance of element(bcp14)+'
true()
true()
```

The draft has at most one paragraph with a key word and no anchor (it
has exactly one), and one or more `bcp14` elements. `element(bcp14)` is
the type of elements named `bcp14`; `node()`, `attribute()`, `text()`
and the other kind tests are types too. You'll use sequence types
throughout XQuery and XSLT.
