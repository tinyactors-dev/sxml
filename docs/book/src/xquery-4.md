# What's New in XQuery 4.0

<!-- examples: spec -->

sxml implements XQuery 4.0, and the earlier sections use its new syntax
where it makes a query shorter or clearer. If you learned XQuery 3.1
elsewhere, this section collects what is new, with small examples on the
draft and pointers to where each feature is explained. Everything in
XPath 4.0 is part of XQuery 4.0 too.

## Shorter syntax

Maps are written with braces alone, `{ "a": 1 }`, and inline functions
with `fn`. A function with no parameter list is a *focus function*, whose
argument is the context item `.`. This one counts the words of its
argument:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    let $words := fn { count(tokenize(normalize-space(.))) }
    return ($words("one two three"), (//t[@anchor])[position() le 3] ! $words(.))'
3
14
12
16
```

Numbers can be written in hexadecimal and binary, with underscores to
group digits, and `char` makes a character from a code point or an
HTML name:

```console
$ sxml xquery --method=adaptive --expression='0x1F, 0b101, 1_000_000, char(0xA7), char("amp")'
31
5
1000000
"§"
"&"
```

`if` has a braced form without `else`, which gives the empty sequence
when the condition is false:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    if (exists(//cref)) { "the draft has editorial notes" }'
"the draft has editorial notes"
```

## Pipelines

`=>` passes a value to a function call as its first argument, and
`=!>` does it for each item ([Functions and Operators](xpath-functions.md)).
The *pipeline operator* `->` passes a value to any expression, as the
context value `.`:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    //t[@anchor = "req-7"] -> (./@anchor || " (" || ./bcp14 || ")")'
"req-7 (MUST)"
```

## String templates

A string template, in backticks, puts the values of expressions in
braces into a string. Most of the earlier examples use them, because
they are much easier to read than many `||`:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    `{ count(//t[@anchor]) } requirements in { count(//section) } sections, by { string-join(/rfc/front/author/@surname, " and ") }`'
"11 requirements in 9 sections, by Byron and Mathison"
```

## Binding more than one variable

A `let` can take a sequence, an array or a map apart, and bind its
parts to several variables at once:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    let $($first, $second) := /rfc/front/author/@surname ! string()
    let $[$year, $month] := [2026, "October"]
    let ${$anchor, $keyword} := { "anchor": "req-7", "keyword": "MUST" }
    return `{ $first } and { $second }; { $month } { $year }; { $anchor } is a { $keyword }`'
"Byron and Mathison; October 2026; req-7 is a MUST"
```

`for member` iterates over the members of an array, and `for key` and
`for value` over the entries of a map:

```console
$ sxml xquery --method=adaptive --expression='
    for key $keyword value $count in { "MUST": 5, "SHOULD": 2, "MAY": 2 }
    return $keyword || ": " || $count'
"MUST: 5"
"SHOULD: 2"
"MAY: 2"
```

## New clauses and expressions

- `while` in a FLWOR expression stops at the first binding for which its
  condition is false ([FLWOR Expressions](xquery-flwor.md)).
- `switch ()` without a value tests conditions, and `switch` and
  `typeswitch` can have braces
  ([Conditions, Types and Errors](xquery-types-errors.md)).
- `otherwise` gives a fallback for an empty sequence
  ([Functions and Operators](xpath-functions.md) and
  [Conditions, Types and Errors](xquery-types-errors.md)).
- `try` can end with a `finally` clause, which runs whether or not an
  error happened; its value must be empty
  ([Conditions, Types and Errors](xquery-types-errors.md)).
- `transform with` is a short form of `copy ... modify ... return`
  ([Updating Documents](xquery-updates.md)).

## Types

- `declare type` names an item type, and `enum("a", "b")` is the type of
  a few strings ([Conditions, Types and Errors](xquery-types-errors.md)).
- `record(...)` describes maps with known keys
  ([Maps, Arrays and Records](xpath-maps-arrays.md)), and
  `declare record` names one and declares a function that makes its
  records, like `spec:entry` in
  [Functions and Modules](xquery-modules.md).
- A *choice type* in parentheses allows any of several item types:

```console
$ sxml xquery --method=adaptive --expression='"req-7" instance of (xs:string | xs:integer)'
true()
```

## Functions

Parameters of declared functions can have default values, and every
function call can pass arguments by name. When does a draft published
on the 21st of October 2026 expire?

```console
$ sxml xquery --method=adaptive --expression='
    declare function local:expires($date as xs:date, $days as xs:integer := 185) as xs:date {
      $date + xs:dayTimeDuration("P" || $days || "D")
    };
    local:expires(xs:date("2026-10-21")),
    local:expires(days := 90, date := xs:date("2026-10-21"))'
xs:date("2027-04-24")
xs:date("2027-01-19")
```

A default is an expression of its own; it can't refer to the other
parameters of the function. The function library of XPath 4.0 is much
larger than 3.1's, with functions such as `sort-by`, `partition`,
`highest`, `index-where`, `parse-csv`, `parse-html` and
`array:build`; [Finding Things with XPath](xpath.md) shows many of them.
