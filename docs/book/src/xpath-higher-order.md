# Functions as Values

<!-- examples: spec -->

In XPath, a function is a value like any other. You can bind it to a
variable, pass it to another function, and return it from one. Functions
that take functions are called *higher-order functions*, and they are
how XPath does many things that other languages do with loops.

## Inline functions

An inline function is written `fn` (or `function`), with its parameters
and a body in curly braces. The anchors of the draft's requirements end
in the number they were given; this function takes it out:

```console
$ sxml xpath 'let $number := fn($anchor) { xs:integer(substring-after($anchor, "req-")) } return $number("req-11")'
11
```

Parameters and the result can have types. An attribute passed to a
parameter of type `xs:string` becomes its string:

```console
$ sxml xpath --context-item=vane.xml 'let $number := fn($anchor as xs:string) as xs:integer { xs:integer(substring-after($anchor, "req-")) } return //section[@anchor = "pause"]/t/@anchor ! $number(.)'
7
8
11
```

A function with no parameter list, `fn { ... }`, takes one argument and
makes it the context item, so `.` and relative paths in the body refer
to it. That is the short form we used with `map:build`, and it's ideal
for sort keys. In the document, the requirements are in the order of
the text; sorted by their numbers, they are in the order they were
written:

```console
$ sxml xpath --context-item=vane.xml 'sort(//t[@anchor], (), fn { xs:integer(substring-after(@anchor, "req-")) }) ! string(@anchor)'
"req-1"
"req-2"
"req-3"
"req-4"
"req-5"
"req-6"
"req-7"
"req-8"
"req-9"
"req-10"
"req-11"
```

`sort` takes the sequence, a collation (`()` for the default) and a key
function, and sorts by the key. Sorted by the anchors as strings,
`req-10` would come before `req-2`.

## Named function references

`name#arity` refers to an existing function by its name and number of
arguments:

```console
$ sxml xpath --context-item=vane.xml 'for-each(//dt, upper-case#1)'
"FLAG"
"SDK"
"PERCENTAGE"
"PAUSE"
```

## Partial application

A function call with `?` in place of some arguments does not call the
function: it makes a new function that waits for the missing ones:

```console
$ sxml xpath 'for-each(("req-3", "req-11"), substring-after(?, "req-"))'
"3"
"11"
$ sxml xpath --context-item=vane.xml 'let $in-section := fn($anchor, $t) { exists($t/ancestor::section[@anchor = $anchor]) } let $in-pause := $in-section("pause", ?) return //t[@anchor][$in-pause(.)]/@anchor/string()'
"req-7"
"req-8"
"req-11"
```

`substring-after(?, "req-")` is a function of one argument, the string
to cut. `$in-section("pause", ?)` fixes the section and leaves the
paragraph open.

## The classic higher-order functions

`for-each` applies a function to each item, `filter` keeps the items for
which a function is true, and `fold-left` combines the items one by one
into a single result:

```console
$ sxml xpath --context-item=vane.xml 'for-each(//section[t[@anchor]], fn($section) { $section/@anchor || ": " || count($section/t[@anchor]) })'
"states: 3"
"rollout: 3"
"status: 2"
"pause: 3"
$ sxml xpath --context-item=vane.xml 'filter(//t[@anchor], fn($t) { starts-with($t/bcp14, "SHOULD") }) ! string(@anchor)'
"req-3"
"req-10"
"req-8"
$ sxml xpath --context-item=vane.xml 'fold-left(//t[@anchor], {}, fn($counts, $t) { let $section := string($t/ancestor::section[1]/@anchor) return map:put($counts, $section, ($counts($section) otherwise 0) + 1) })'
{"states":3,"rollout":3,"status":2,"pause":3}
```

The fold starts with an empty map and, for each requirement, puts the
count of its section one higher: the requirement count per section,
built one requirement at a time.

XPath 4.0 adds more: `highest` and `lowest` find the items with the
largest or smallest key, `sort-by` sorts by several keys, `partition`
splits a sequence, and `while-do` repeats a function while a condition
holds:

```console
$ sxml xpath --context-item=vane.xml 'highest(//section, (), fn { count(t[@anchor]) }) ! string(@anchor)'
"states"
"rollout"
"pause"
$ sxml xpath --context-item=vane.xml 'lowest(//t[@anchor], (), fn { string-length(normalize-space()) })/@anchor/string()'
"req-2"
$ sxml xpath --context-item=vane.xml 'partition(//t[@anchor], fn($group, $next) { not($next/.. is $group[1]/..) }) ! string-join(?* ! @anchor, " ")'
"req-1 req-2 req-3"
"req-4 req-5 req-9"
"req-6 req-10"
"req-7 req-8 req-11"
$ sxml xpath --context-item=vane.xml 'while-do(//t[@anchor = "req-10"], fn { exists(parent::section) }, fn { .. })/name/string()'
"Messages"
```

Three sections tie for the most requirements, and `highest` gives all
three. `partition` starts a new group (an array) whenever the next
requirement has another parent than the first of the group: `is`
compares nodes by identity. `while-do` goes up from req-10 for as long
as the parent is a section, and ends at the top-level section it is in.

`sort-by` takes one record per key, with the key function and,
optionally, the order. Requirements are often listed by strength: all
MUSTs first, then SHOULDs, then MAYs. A map from key word to rank makes
that a key, and the anchor number decides within a rank, here with the
latest first:

```console
$ sxml xpath --context-item=vane.xml 'let $strength := { "MUST": 1, "MUST NOT": 1, "SHOULD": 2, "SHOULD NOT": 2, "MAY": 3 } return sort-by(//t[@anchor], ({ "key": fn { $strength(bcp14) } }, { "key": fn { xs:integer(substring-after(@anchor, "req-")) }, "order": "descending" })) ! `{@anchor} {bcp14}`'
"req-7 MUST"
"req-6 MUST"
"req-5 MUST NOT"
"req-4 MUST"
"req-2 MUST"
"req-1 MUST"
"req-10 SHOULD NOT"
"req-8 SHOULD"
"req-3 SHOULD"
"req-11 MAY"
"req-9 MAY"
```

## Functions that make functions

A function can return a function. This one makes a checker for a key
word: a function that tells whether a requirement has that key word or
its negation:

```console
$ sxml xpath --context-item=vane.xml 'let $checker := fn($keyword) { fn($t) { $t/bcp14 = ($keyword, $keyword || " NOT") } } let $requirements := //t[@anchor] for $keyword in ("MUST", "SHOULD", "MAY") return `{$keyword}: {count(filter($requirements, $checker($keyword)))}`'
"MUST: 6"
"SHOULD: 3"
"MAY: 2"
```

The inner function remembers `$keyword` after `$checker` has returned:
it is a *closure*. Functions can be combined, too. This one composes two
functions into a third:

```console
$ sxml xpath --context-item=vane.xml 'let $compose := fn($f, $g) { fn($x) { $g($f($x)) } } return $compose(normalize-space#1, string-length#1)(//t[@anchor = "req-1"])'
81
```

## What XPath can't do alone

Sections nest, and a function that walks them would like to call itself
for each subsection. A function bound with `let` can't, because the name
is not in scope inside its own body:

```console
$ sxml xpath 'let $factorial := fn($n) { if ($n le 1) then 1 else $n * $factorial($n - 1) } return $factorial(5)'
expression:1:58: err:XPST0008: the variable is not declared
```

XPath is an expression language, without declarations. To write
recursive functions, to keep functions such as the section number in a
module, and to build new documents, you need XQuery, which is where
we're going next.

## Summary

This chapter covered the whole of XPath:

- paths, steps, axes and predicates find nodes;
- atomic values have types, and untyped values from documents are
  converted where they are used;
- general comparisons (`=`) compare sequences, value comparisons (`eq`)
  compare single values;
- the function library covers strings, regular expressions, numbers,
  dates and sequences, and `=>` chains calls;
- maps and arrays hold structured data, and JSON is maps and arrays;
- functions are values, and higher-order functions replace many loops.
