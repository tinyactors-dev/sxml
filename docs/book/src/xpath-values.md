# Values and Types

<!-- examples: xpath -->

Every atomic value in XPath has a type. You rarely have to write a type
down, but knowing they exist explains most of the surprises you'll meet.

## Literals

Numbers and strings can be written directly. The way a number is
written decides its type:

```console
$ sxml xpath '42, 3.14, 1e3, "text", true()'
42
3.14
1.0e3
"text"
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
$ sxml xpath 'type-of(1), type-of(1.0), type-of("1")'
"xs:integer"
"xs:decimal"
"xs:string"
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

Dividing an integer or a decimal by zero is an error. Dividing a double
by zero gives infinity, as floating-point numbers do:

```console
$ sxml xpath '1 div 0'
expression:1:1: err:FOAR0001: division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)
$ sxml xpath '1e0 div 0'
INF
```

## Untyped values

A plain XML document has no types in it. The text of `<price>22.50</price>`
is just the characters `2`, `2`, `.`, `5`, `0`. XPath gives such values
the type `xs:untypedAtomic`, and converts them when they are used:

- in arithmetic, an untyped value becomes an `xs:double`;
- compared with a number using `=` or `<`, it becomes a number too;
- compared with a string, it is compared as a string.

That is why the sum of the pages is a double:

```console
$ sxml xpath --context-item=library.xml 'sum(//book/@pages)'
2.822e3
```

When you want exact arithmetic, convert the values first with a
*constructor function*, named after the type:

```console
$ sxml xpath --context-item=library.xml 'sum(//book/@pages ! xs:integer(.))'
2822
$ sxml xpath --context-item=library.xml 'sum(//price ! xs:decimal(.))'
278.99
```

Constructor functions exist for every type: `xs:string`, `xs:integer`,
`xs:decimal`, `xs:double`, `xs:boolean`, `xs:date`, `xs:dateTime`,
`xs:duration` and many more. When the text is not a valid value of the
type, they raise an error:

```console
$ sxml xpath 'xs:integer("twelve")'
expression:1:1: err:FORG0001: the value is not in the lexical space or value space of the target type
```

`castable as` asks first, and `cast as` is another way to write the
conversion:

```console
$ sxml xpath '"twelve" castable as xs:integer, "12" cast as xs:integer'
false()
12
```

Chapter 5 shows how a schema gives the values of a document their
types, so that `price` is a decimal without any conversion.

## Two kinds of comparison

XPath has two sets of comparison operators, and the difference matters.

The *general comparisons*, `=`, `!=`, `<`, `<=`, `>` and `>=`, compare
sequences. They are true if *any* pair of items compares true:

```console
$ sxml xpath --context-item=library.xml '//book/@year = 1968'
true()
$ sxml xpath --context-item=library.xml '//book/@year = (1900, 1965)'
true()
```

The first asks "is any book's year 1968?". The second asks whether any
year is 1900 or 1965. This is what makes predicates such as
`[tags/tag = "algorithms"]` so convenient: the book has several tags,
and the test is true if one of them matches.

```console
$ sxml xpath --context-item=library.xml '//book[tags/tag = "algorithms"]/@id/string()'
"taocp"
"sicp"
```

It also means `!=` is not the opposite of `=`: `//book/@year != 1968`
is true, because there are years that are not 1968.

The *value comparisons*, `eq`, `ne`, `lt`, `le`, `gt` and `ge`, compare
exactly one item with one item, and they are strict about types. An
untyped value is compared as a string, so comparing an attribute with a
number is an error:

```console
$ sxml xpath --context-item=library.xml '(//book)[1]/@year eq 1979'
expression:1:1: err:XPTY0004: the compared items belong to different type families (XPath 4.0, section 4.11.1; F&O 4.0, fn:compare)
$ sxml xpath --context-item=library.xml 'xs:integer((//book)[1]/@year) eq 1979'
true()
```

Use general comparisons in predicates over documents, and value
comparisons when you know you have single values of known types.

Strings compare by their characters. Numbers compare as numbers. That
makes a difference:

```console
$ sxml xpath '"10" lt "9", 10 lt 9'
true()
false()
```

## Dates, times and durations

Dates, times and durations are types of their own, with arithmetic that
knows about calendars:

```console
$ sxml xpath 'xs:date("2026-12-25") - xs:date("2026-10-20")'
xs:dayTimeDuration("P66D")
$ sxml xpath 'xs:date("2026-01-31") + xs:yearMonthDuration("P1M")'
xs:date("2026-02-28")
$ sxml xpath 'xs:dateTime("2026-10-20T09:30:00Z") + xs:dayTimeDuration("PT90M")'
xs:dateTime("2026-10-20T11:00:00Z")
```

sxml writes them as constructor calls, so you can paste a result back
into an expression. Durations are written as ISO 8601 durations: `P66D`
is 66 days and `PT90M` 90 minutes.

## Effective boolean values

A predicate or an `if` needs true or false. When the value is something
else, XPath takes its *effective boolean value*: the empty sequence is
false, a sequence that starts with a node is true, a string is true unless
it is empty, a number is true unless it is zero or NaN.

```console
$ sxml xpath 'boolean(()), boolean(""), boolean("false"), boolean(0)'
false()
false()
true()
false()
```

Note the third one: the string `"false"` is not empty, so it is true.

## Sequence types

Sometimes you write a type down: in `instance of`, in a function's
signature, or in a variable declaration in XQuery. A *sequence type* is an
item type with an optional occurrence indicator: `?` for zero or one,
`*` for any number, `+` for one or more.

```console
$ sxml xpath '"Dune" instance of xs:string+, () instance of xs:string?'
true()
true()
$ sxml xpath --context-item=library.xml '(//book)[1]/price instance of element(price)'
true()
```

`element(price)` is the type of elements named `price`; `node()`,
`attribute()`, `text()` and the other kind tests are types too. You'll
use sequence types throughout XQuery and XSLT.
