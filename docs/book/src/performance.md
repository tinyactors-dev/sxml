# Performance and Memory

<!-- examples: performance -->

sxml is built to be small and predictable rather than to win benchmarks.
This chapter explains where time and memory go, and the few habits that
make the biggest difference. The examples are in
`docs/book/examples/performance`; the times in the text were measured
with a release build on one core of an ordinary Linux machine.

## Build a release

The debug build checks a great deal as it runs and is many times slower.
For real work, use the release build (`cmake --preset release`) or a
release archive.

## Compile once

Compiling an expression, a query, a stylesheet or a schema does real
work: parsing, resolving names, checking types, optimizing. When you run
the same thing many times, compile it once and keep the compiled object,
as Chapter 9 shows. The `sxml` command compiles each time it runs, so a
shell loop that calls it for each of a thousand files pays a thousand
compilations; one query over all the files pays one.

## Sequences are computed as they are read

A range such as `1 to 1000000000` is not a billion integers in memory.
It is computed as it is read, and operations that need only a part of
it read only that part:

```console
$ sxml xpath '(1 to 1000000000)[3]'
3
$ sxml xpath 'count(1 to 1000000000)'
1000000000
```

Both return at once. Sequences that come from functions, filters and
paths are computed when they are needed too, and released as soon as
nothing refers to them. When only the first items of a sequence matter,
sxml stops making it once it has them: `exists`, `empty`, `head`,
`boolean`, a predicate `[1]`, `some` and `every` read as far as the
first item that decides, and no further:

```console
$ sxml xpath 'exists((1 to 1000000000)[. = 999])'
true()
$ sxml xpath 'some $n in (1 to 1000000000)[. mod 7 = 0] satisfies $n > 50'
true()
```

An error in an item that is never read is not raised, as XPath allows.

## Look up instead of searching

The most common cause of a slow query is a search inside a loop. This
query finds, for each order, the order with the next id:

`slow-join.xq`:

```xquery
{{#include ../examples/performance/slow-join.xq}}
```

For each of *n* orders, `//order[...]` looks at all *n* orders, so
the query does *n*² comparisons. Building an index once, here a map from
id to order, turns each search into one lookup:

`fast-join.xq`:

```xquery
{{#include ../examples/performance/fast-join.xq}}
```

Both give the same answer:

```console
$ sxml xquery --param=count=1000 --output=orders.xml make-orders.xq
$ sxml xquery --context-item=orders.xml --method=adaptive slow-join.xq
143
$ sxml xquery --context-item=orders.xml --method=adaptive fast-join.xq
143
```

With 2,000 orders, the first query takes 5 seconds and the second 0.04
seconds. With 20,000 orders, the first one would take hours, and stops
at the evaluation step limit after about 15 seconds; the second takes
0.3 seconds. In XSLT, `xsl:key` does the same job (Chapter 4).

sxml builds such an index itself when a condition compares something of
the item alone with something that does not depend on it. Written as
`//order[@id = $order/@id + 1]`, the first query takes 0.03 seconds
too: the left side depends only on the order looked at, the right side
only on `$order`, so sxml sorts the orders by `@id` once and looks each
`$order/@id + 1` up. In `@id - $order/@id = 1` the two are mixed, and
sxml has to search.

## Memory

A parsed document takes several times the size of its file, because each
node has its name, its links to its neighbours and its text. A 69 MB
document of a million small elements takes about 540 MB as a tree.

When a document is too large for that, stream it. An XSLT stylesheet
with a streamable mode processes the same 69 MB in 11 MB, and the
memory stays the same for a file ten times the size (Chapter 4).

In a program, all of sxml's memory comes from the allocator you give it
(Chapter 9), so you can measure it, cap it, or place it where you like.
The processor's limits bound the size of the item heap and of each
document, the depth of nesting, the number of nested function calls and
the number of evaluation steps, so a query you don't trust can't take
the machine down with it.

## The step limit

Every evaluation counts its steps, and stops after a hundred million by
default, a few seconds of work. A query that would run for hours stops,
after about ten seconds, with an error that says so, instead of hanging:

```console
$ sxml xpath 'some $n in 1 to 100000000000 satisfies $n = 0'
expression: the evaluation took more steps than maximum_evaluation_step_count allows
```

A program sets the limit in its processor configuration, higher or
lower, to fit the work it expects.

## Habits that help

- Start paths from what you know. `/library/shelf/book` touches fewer
  nodes than `//book`, which looks at every node of the document.
- Convert untyped values once. `xs:decimal(price)` in a `let`, or a
  schema that gives the values their types, saves converting the same
  text again and again.
- Use maps, keys and `group by` for lookups and grouping, not nested
  loops.
- Stream large inputs with XSLT.
