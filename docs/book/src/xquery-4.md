# What's New in XQuery 4.0

<!-- examples: xquery -->

sxml implements XQuery 4.0, and the earlier sections use its new syntax
where it makes a query shorter or clearer. If you learned XQuery 3.1
elsewhere, this section collects what is new, with small examples and
pointers to where each feature is explained. Everything in XPath 4.0
is part of XQuery 4.0 too.

## Shorter syntax

Maps are written with braces alone, `{ "a": 1 }`, and inline functions
with `fn`. A function with no parameter list is a *focus function*, whose
argument is the context item `.`:

```console
$ sxml xquery --method=adaptive --expression='
    let $increment := fn { . + 1 }
    return ($increment(5), (1, 2, 3) ! $increment(.))'
6
2
3
4
```

Numbers can be written in hexadecimal and binary, with underscores to
group digits, and `char` makes a character from a code point or an
HTML name:

```console
$ sxml xquery --method=adaptive --expression='0x1F, 0b101, 1_000_000, char(0x2713), char("amp")'
31
5
1000000
"✓"
"&"
```

`if` has a braced form without `else`, which gives the empty sequence
when the condition is false:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    if (count(//book) > 3) { "a big library" }'
"a big library"
```

## Pipelines

`=>` passes a value to a function call as its first argument, and
`=!>` does it for each item (Chapter 2). The *pipeline operator* `->`
passes a value to any expression, as the context value `.`:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    (//book)[1] -> (./title || " (" || ./@year || ")")'
"Godel, Escher, Bach (1979)"
```

## String templates

A string template, in backticks, puts the values of expressions in
braces into a string. Most of the earlier examples use them, because
they are much easier to read than many `||`:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    `{ count(//book) } books, { sum(//@pages ! xs:integer(.)) } pages`'
"5 books, 2822 pages"
```

## Binding more than one variable

A `let` can take a sequence, an array or a map apart, and bind its
parts to several variables at once:

```console
$ sxml xquery --method=adaptive --expression='
    let $($first, $second) := ("Ada", "Alan")
    let $[$year, $month] := [2026, 10]
    let ${$name, $since} := { "name": "Grace", "since": "2026-06-30" }
    return `{ $first } and { $second }; { $year }-{ $month }; { $name } since { $since }`'
"Ada and Alan; 2026-10; Grace since 2026-06-30"
```

`for member` iterates over the members of an array, and `for key` and
`for value` over the entries of a map:

```console
$ sxml xquery --method=adaptive --expression='
    for key $member value $loans in { "ada": 3, "alan": 2 }
    return $member || ": " || $loans'
"ada: 3"
"alan: 2"
```

## New clauses and expressions

- `while` in a FLWOR expression stops at the first binding for which its
  condition is false ([FLWOR Expressions](xquery-flwor.md)).
- `switch ()` without a value tests conditions, and `switch` and
  `typeswitch` can have braces
  ([Conditions, Types and Errors](xquery-types-errors.md)).
- `otherwise` gives a fallback for an empty sequence (Chapter 2 and
  [Conditions, Types and Errors](xquery-types-errors.md)).
- `try` can end with a `finally` clause, which runs whether or not an
  error happened; its value must be empty.
- `transform with` is a short form of `copy ... modify ... return`
  ([Updating Documents](xquery-updates.md)).

## Types

- `declare type` names an item type, and `enum("a", "b")` is the type of
  a few strings ([Conditions, Types and Errors](xquery-types-errors.md)).
- `record(...)` describes maps with known keys (Chapter 2), and
  `declare record` names one and declares a function that makes its
  records.
- A *choice type* in parentheses allows any of several item types:

```console
$ sxml xquery --method=adaptive --expression='42 instance of (xs:string | xs:integer)'
true()
```

## Functions

Parameters of declared functions can have default values, and every
function call can pass arguments by name:

```console
$ sxml xquery --method=adaptive --expression='
    declare function local:due($date as xs:date, $days as xs:integer := 21) as xs:date {
      $date + xs:dayTimeDuration("P" || $days || "D")
    };
    local:due(xs:date("2026-10-01")),
    local:due(days := 7, date := xs:date("2026-10-01"))'
xs:date("2026-10-22")
xs:date("2026-10-08")
```

A default is an expression of its own; it can't refer to the other
parameters of the function. The function library of XPath 4.0 is much
larger than 3.1's, with functions such as `sort-by`, `partition`,
`highest`, `index-where`, `parse-csv`, `parse-html` and
`array:build`; Chapter 2 shows many of them.
