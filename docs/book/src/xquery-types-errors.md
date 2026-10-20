# Conditions, Types and Errors

<!-- examples: xquery -->

Real data is untidy. A value can be a number or a string, an element
can be missing, a lookup can fail. This section shows how a query
chooses between cases, how it states the types it expects, and how it
deals with errors.

## Choosing by value: switch

`if` chooses between two branches; XPath's `if` was in Chapter 2.
`switch` chooses between many, by comparing one value with the values
of its `case` clauses. A `case` can list several values, and `default`
is required:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    return switch (string($book/@id))
      case "geb" case "taocp" case "sicp" return "computing"
      case "dune" return "desert"
      default return "other"'
"computing"
"computing"
"computing"
"desert"
"other"
```

The values are compared as `deep-equal` compares them, so the string
`"1"` doesn't match the number `1`. That's why the query converts the
attribute with `string` first.

In XQuery 4.0, the value in parentheses can be left out. Then each
`case` is a condition, and the first one that is true wins:

```console
$ sxml xquery --method=adaptive --expression='
    for $price in (5, 50, 500)
    return switch () {
      case $price lt 10 return "cheap"
      case $price lt 100 return "fair"
      default return "dear"
    }'
"cheap"
"fair"
"dear"
```

The braces around the cases are optional; they help when a `switch`
sits inside a larger expression.

## Choosing by type: typeswitch

`typeswitch` chooses by the *type* of a value. Each `case` names a
sequence type, and a variable in the `case` binds the value with that
type:

```console
$ sxml xquery --method=adaptive --expression='
    for $value in (42, "42", 4.2, xs:date("2026-10-20"), <price>9.99</price>, [4, 2])
    return typeswitch ($value)
      case xs:integer | xs:decimal return "a number: " || $value
      case xs:string return "a string: " || $value
      case $date as xs:date return "a date in " || year-from-date($date)
      case element(price) return "a price element"
      default return "something else"'
"a number: 42"
"a string: 42"
"a number: 4.2"
"a date in 2026"
"a price element"
"something else"
```

The cases are tried in order. Types are checked as `instance of` checks
them, so derived types count: an `xs:integer` is also an `xs:decimal`,
but an `xs:double` is neither:

```console
$ sxml xquery --method=adaptive --expression='
    for $value in (42, 4.2, 4.2e0)
    return typeswitch ($value)
      case xs:integer return "integer"
      case xs:decimal return "decimal"
      default return "other"'
"integer"
"decimal"
"other"
```

## Saying what you expect

A sequence type after `as` states what a variable, a parameter or a
function result must be. You can write one on `let` and `for` too:

```console
$ sxml xquery --method=adaptive --expression='let $count as xs:integer := 5 return $count * 2'
10
$ sxml xquery --method=adaptive --expression='let $count as xs:integer := "5" return $count * 2'
expression:1:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The value must already have the type, or be convertible to it by the
*coercion rules*, which are few: untyped values are cast to the
required type, integers and decimals are promoted to doubles where a
double is required, and nodes are atomized where an atomic value is
required. A string is never turned into a number. Untyped values are
what documents without a schema give you, so this works:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    let $pages as xs:integer := (//book)[1]/@pages
    return $pages + 1'
778
```

`treat as` is a check without a conversion: it raises `err:XPDY0050`
when the value doesn't have the type, and otherwise gives the value
unchanged:

```console
$ sxml xquery --context-item=library.xml --expression='(//book)[1]/title treat as element(title)'
<title>Godel, Escher, Bach</title>
$ sxml xquery --context-item=library.xml --expression='(//book)[1]/@year treat as element(title)'
expression:1:1: err:XPDY0050: The value of a treat expression does not match its type (XPath 4.0, section 4.16.6)
```

## Naming types

When the same type appears in many signatures, give it a name.
`declare type` names any item type, and `enum` is the type of a few
fixed strings:

```console
$ sxml xquery --method=adaptive --expression='
    declare type local:currency as enum("EUR", "USD");
    declare function local:price($amount as xs:decimal, $currency as local:currency) {
      $amount || " " || $currency
    };
    local:price(9.99, "EUR"), local:price(5, "GBP")'
expression:6:31: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The first call is fine, but `"GBP"` is not one of the strings of the
enumeration, so the second call fails, and with it the whole query.

`declare record` names a record type, and also declares a function of
the same name that makes such records. Fields can have default values,
and the function takes keyword arguments:

```console
$ sxml xquery --method=adaptive --expression='
    declare record local:loan(book as xs:string, member as xs:string, days as xs:integer := 21);
    local:loan("dune", "ada"),
    local:loan("geb", "alan", days := 7),
    { "book": "dune" } instance of local:loan'
{"book":"dune","member":"ada","days":21}
{"book":"geb","member":"alan","days":7}
false()
```

The last map has no `member`, so it is not a `local:loan`.

## Kinds of errors

Errors come in three kinds, and the second letter pair of the code
tells you which:

- **Static errors** (`XPST`, `XQST`) are found when the query is
  compiled, before anything runs: a syntax error, an unknown function,
  an undeclared variable.
- **Type errors** (`XPTY`, `XQTY`) mean a value has the wrong type for
  where it is used.
- **Dynamic errors** (`XPDY`, `XQDY`, and the `FO` codes of the
  function library) happen while the query runs: division by zero, a
  missing document, a value that can't be cast.

A query with a static error doesn't run at all, so a static error can't
be caught. Here is one that surprises many people: a function body has
no context item, so a path inside a function can't start from the
document:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare function local:titles() { //title };
    local:titles()'
expression:2:39: err:XPDY0002: the focus is absent in a function body
```

Pass the document to the function as a parameter, or bind it to a
global variable, whose value may use the context item.

## Catching errors

`try` evaluates an expression, and when it raises a dynamic or type
error, the first `catch` clause whose name matches the error code gives
the value instead. `*` matches every error, `err:*` every error of the
specifications, and `|` lists several codes:

```console
$ sxml xquery --method=adaptive --expression='
    for $text in ("12", "twelve")
    return try { xs:integer($text) } catch err:FORG0001 | err:XPTY0004 { -1 }'
12
-1
```

Inside a `catch`, variables describe the error: `$err:code` is its
name as an `xs:QName`, `$err:description` its message, `$err:value` the
value that came with it, and `$err:module`, `$err:line-number` and
`$err:column-number` say where it happened. `$err:map` holds all of them
in one map:

```console
$ sxml xquery --method=adaptive --expression='try { 1 div 0 } catch * { $err:map }'
{"code":#Q{http://www.w3.org/2005/xqt-errors}FOAR0001,"description":"division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)","module":"expression","line-number":1,"column-number":7}
```

`#Q{...}FOAR0001` is how the adaptive method writes an `xs:QName`.

## Raising your own errors

`error` raises an error with a code of your own, a description and,
optionally, a value. Give your codes a namespace of your own, so they
can't be confused with the standard ones. This query looks books up
by id, and turns a missing book into a message instead of a failure:

`lookup.xq`:

```xquery
{{#include ../examples/xquery/lookup.xq}}
```

```console
$ sxml xquery --context-item=library.xml lookup.xq
Dune
(skipped emma: No book has the id emma)
Structure and Interpretation of Computer Programs
```

`otherwise` gives its right side when the left side is empty, which
makes "this, or else fail" short to write. An error that nobody catches
ends the query, and the `sxml` command reports it with its code and
description, and exits with status 1:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare namespace app = "https://example.org/app";
    //book[@id = "emma"] otherwise error(xs:QName("app:unknown-book"), "No book has the id emma")'; echo "exit status $?"
expression:3:36: app:unknown-book: No book has the id emma
exit status 1
```

## Errors on purpose

Raising errors is the right thing to do much more often than you might
think. A query that silently returns nothing for bad input is hard to
debug; one that says "No book has the id emma" is not. Some habits
help:

- Declare the types of parameters and results. The error then happens
  at the call that is wrong, not three functions later.
- Use `exactly-one`, `one-or-more` and `zero-or-one` when a path must
  find a certain number of nodes; they raise an error otherwise.
- Catch only the errors you expect, by their codes, and let the others
  through.

`fn:trace` and `fn:message` are for debugging output. `trace` returns
its input unchanged and writes it, after its label, to standard error;
`message` writes its input and returns nothing. The values are written
as the adaptive method shows them, so strings are in quotes:

```console
$ sxml xquery --method=adaptive --expression='
    let $total := trace(sum((3, 4)), "total")
    return (message("halfway"), $total * 2)'
total: 7
"halfway"
14
```

A program that embeds sxml receives the same output through a trace
sink of its own.
