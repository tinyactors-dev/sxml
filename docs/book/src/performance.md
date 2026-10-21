# Performance and Memory

<!-- examples: traces -->

sxml is built to be small and predictable rather than to win benchmarks.
This chapter explains where time and memory go, and the few habits that
make the biggest difference. The examples use the trace archives of
[Streaming Large Trace Files](traces-streaming.md), in
`docs/book/examples/traces`; the times in the text were measured with a
release build on one core of an ordinary Linux machine.

## Build a release

The debug build checks a great deal as it runs and is many times slower.
For real work, use the release build (`cmake --preset release`) or a
release archive.

## Compile once

Compiling an expression, a query, a stylesheet or a schema does real
work: parsing, resolving names, checking types, optimizing. When you run
the same thing many times, compile it once and keep the compiled object,
as [Embedding sxml in C](c.md) and [sxml in JavaScript](javascript.md)
show. The `sxml` command compiles each time it runs, so a shell loop
that calls it for each of a thousand trace files pays a thousand
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
The same holds for paths over a document. When was the first request of
an archive that took longer than 80 milliseconds?

```console
$ sxml xquery --param=count=10000 --output=archive.xml make-traces.xq
$ sxml xpath --context-item=archive.xml 'head(//trace[span/@duration > 80])/@start/string()'
"2026-10-21T09:00:04.9Z"
```

`head` asks the filter for one trace, and the filter looks at traces
only until it finds one: the fiftieth of ten thousand. Parsing the
archive still takes most of the time; [Streaming Large Trace
Files](traces-streaming.md) avoids that too.

## Look up instead of searching

The most common cause of a slow query is a search inside a loop. An
exporter writes spans as a flat list, each naming its parent, and
[Spans as a Tree](traces-tree.md) puts the trees together again. With
`$form` set to `flat`, `make-traces.xq` writes such a list. This query
rebuilds the trees by searching all the spans for the children of each
one:

`slow-tree.xq`:

```xquery
{{#include ../examples/traces/slow-tree.xq}}
```

For each of *n* spans, `$spans[@parent = $span/@id]` looks at all *n*
spans, so the query does *n*² comparisons. Building an index once, here
a map from each parent id to its children, turns each search into one
lookup:

`fast-tree.xq`:

```xquery
{{#include ../examples/traces/fast-tree.xq}}
```

Both give the same answer, every span in a tree:

```console
$ sxml xquery --param=count=300 --param=form=flat --output=spans.xml make-traces.xq
$ sxml xquery --context-item=spans.xml --method=adaptive slow-tree.xq
1900
$ sxml xquery --context-item=spans.xml --method=adaptive fast-tree.xq
1900
```

With 1,000 traces (6,333 spans), the first query takes 2.3 seconds and
the second 0.2 seconds. With 10,000 traces, the first takes four and a
half minutes, the second 1.5 seconds. In XSLT, `xsl:key` does the same
job ([Keys, Numbers and Grouping](xslt-keys-grouping.md)).

sxml builds such an index itself when a condition in a loop compares
something of the item alone with something that does not depend on it,
and the items do not change between turns of the loop. Written as one
expression, `for $span in //span return //span[@parent = $span/@id]`,
the search over 3,000 traces takes 0.35 seconds: the left side depends
only on the span looked at, the right side only on `$span`, so sxml
sorts the spans by `@parent` once and looks each id up. In
`slow-tree.xq` the search is inside a function that is called anew for
each span, and in a condition such as `@parent - $span/@id = 0` the
two sides are mixed; then sxml has to search.

## Memory

A parsed document takes several times the size of its file, because each
node has its name, its links to its neighbours and its text. An 82 MB
archive of 100,000 traces, 633,333 spans, takes about 390 MB as a tree.

When a document is too large for that, stream it. An XSLT stylesheet
with a streamable mode processes the same 82 MB in 11 MB, and the
memory stays the same for a larger file ([Streaming Large Trace
Files](traces-streaming.md)).

In a program, all of sxml's memory comes from the allocator you give it
([Embedding sxml in C](c.md)), so you can measure it, cap it, or place
it where you like. The processor's limits bound the size of the item
heap and of each document, the depth of nesting, the number of nested
function calls and the number of evaluation steps, so a query you don't
trust can't take the machine down with it.

## The step limit

Every evaluation counts its steps, and stops after a hundred million by
default, a few seconds of work. A query that would run for hours stops,
after about ten seconds, with an error that says so, instead of hanging:

```console
$ sxml xpath 'some $n in 1 to 100000000000 satisfies $n = 0'
expression: the evaluation took more steps than maximum_evaluation_step_count allows
```

A program sets the limit in its processor configuration, higher or
lower, to fit the work it expects; in JavaScript, it is
`createProcessor({ maximumEvaluationStepCount })`.

A streamed document is counted differently. Each element it reads
grants 500 more steps, so the bound grows with the input. The streamed
identity of a 93 MB file of 2.7 million elements takes about 50 steps an
element and runs to the end; a loop inside a template that reads
nothing more still stops.

## Habits that help

- Start paths from what you know. The root spans are
  `/traces/trace/span`, which touches a few nodes of each trace;
  `//span[empty(parent::span)]` finds the same spans but looks at every
  node of the document.
- Convert untyped values once. `xs:decimal(@duration)` in a `let`, or a
  schema that gives the values their types, saves converting the same
  text again and again.
- Use maps, keys and `group by` for lookups and grouping, not nested
  loops.
- Stream large inputs with XSLT.
