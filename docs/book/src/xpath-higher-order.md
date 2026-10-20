# Functions as Values

<!-- examples: xpath -->

In XPath, a function is a value like any other. You can bind it to a
variable, pass it to another function, and return it from one. Functions
that take functions are called *higher-order functions*, and they are
how XPath does many things that other languages do with loops.

## Inline functions

An inline function is written `fn` (or `function`), with its parameters
and a body in curly braces:

```console
$ sxml xpath 'let $double := fn($n) { $n * 2 } return $double(21)'
42
```

Parameters and the result can have types:

```console
$ sxml xpath 'let $square := fn($n as xs:integer) as xs:integer { $n * $n } return (1 to 4) ! $square(.)'
1
4
9
16
```

A function with no parameter list, `fn { ... }`, takes one argument and
makes it the context item, so `.` and relative paths in the body refer
to it. That is the short form we used with `map:build`, and it's ideal
for sort keys:

```console
$ sxml xpath --context-item=library.xml 'sort(//book, (), fn { xs:integer(@year) }) ! @id/string()'
"dune"
"taocp"
"lhod"
"geb"
"sicp"
```

`sort` takes the sequence, a collation (`()` for the default) and a key
function, and sorts by the key.

## Named function references

`name#arity` refers to an existing function by its name and number of
arguments:

```console
$ sxml xpath 'for-each(("a", "b"), upper-case#1)'
"A"
"B"
```

## Partial application

A function call with `?` in place of some arguments does not call the
function: it makes a new function that waits for the missing ones:

```console
$ sxml xpath 'let $add := fn($a, $b) { $a + $b } let $increment := $add(1, ?) return $increment(41)'
42
```

## The classic higher-order functions

`for-each` applies a function to each item, `filter` keeps the items for
which a function is true, and `fold-left` combines the items one by one
into a single result:

```console
$ sxml xpath 'for-each(1 to 4, fn($n) { $n * $n })'
1
4
9
16
$ sxml xpath 'filter(1 to 10, fn($n) { $n mod 3 = 0 })'
3
6
9
$ sxml xpath --context-item=library.xml 'fold-left(//book, 0, fn($total, $book) { $total + xs:integer($book/@pages) })'
2822
```

XPath 4.0 adds more: `highest` and `lowest` find the items with the
largest or smallest key, `sort-by` sorts by several keys,
`partition` splits a sequence, and `while-do` repeats a function while a
condition holds:

```console
$ sxml xpath --context-item=library.xml 'highest(//book, (), fn { xs:integer(@year) })/title/string()'
"Structure and Interpretation of Computer Programs"
$ sxml xpath --context-item=library.xml 'sort-by(//book, { "key": fn { xs:decimal(price) }, "order": "descending" }) ! @id/string()'
"taocp"
"sicp"
"geb"
"lhod"
"dune"
$ sxml xpath 'partition(1 to 10, fn($group, $next) { count($group) = 3 })'
[1,2,3]
[4,5,6]
[7,8,9]
[10]
$ sxml xpath 'while-do(1, fn($n) { $n < 100 }, fn($n) { $n * 2 })'
128
```

## Functions that make functions

A function can return a function. This one composes two functions into
a third:

```console
$ sxml xpath 'let $compose := fn($f, $g) { fn($x) { $g($f($x)) } } return $compose(upper-case#1, string-length#1)("abc")'
3
```

The inner function remembers `$f` and `$g` after `$compose` has returned:
it is a *closure*.

## What XPath can't do alone

A function bound with `let` can't call itself, because the name is not
in scope inside its own body:

```console
$ sxml xpath 'let $fact := fn($n) { if ($n le 1) then 1 else $n * $fact($n - 1) } return $fact(5)'
expression:1:53: err:XPST0008: the variable is not declared
```

XPath is an expression language, without declarations. To write
recursive functions, to keep functions in modules, and to build new
documents, you need XQuery, which is where we're going next.

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
