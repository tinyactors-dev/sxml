# Streaming Large Files

<!-- examples: xslt-streaming -->

Most XML processors read the whole input into a tree before they
transform it, and a tree takes several times the memory of the file. For
a file of a few megabytes that doesn't matter. For a log, a database
export or a feed of several gigabytes, it does.

XSLT can *stream*: process the input as it is parsed, one piece at a
time, and never build the tree. You ask for it by declaring a mode
`streamable="yes"`. The processor then checks that every rule in that
mode can work in one pass over the input, from start to end, and runs it
that way. The examples are in `docs/book/examples/xslt-streaming`.

## A large input

First we need a large file. This query writes one, with as many orders
as `$count` says:

`make-orders.xq`:

```xquery
{{#include ../examples/xslt-streaming/make-orders.xq}}
```

```console
$ sxml xquery --param=count=100000 --output=orders.xml make-orders.xq
$ head -c 150 orders.xml
<orders><order id="1"><customer>c1</customer><total>0.1</total></order><order id="2"><customer>c2</customer><total>0.2</total></order><order id="3"><c
```

## A streaming stylesheet

`total.xsl`:

```xml
{{#include ../examples/xslt-streaming/total.xsl}}
```

```console
$ sxml transform total.xsl orders.xml
4,995,000.00
```

It looks like any other stylesheet, but `sxml transform` read the
orders as they were parsed and kept only the running sum. With a million
orders (a 69 MB file), a release build of sxml uses 11 MB of memory at
its peak. Without `streamable="yes"`, the same stylesheet builds the
tree and uses about 540 MB. Streaming memory stays the same however
large the file gets.

## What can stream

In one pass, the processor sees each node once, as it goes by. A rule in
a streamable mode can do anything with the node it matched and the
attributes and ancestors of that node, but it can go down into the
children only once. This rule goes down twice, once for the sum and
once for the count:

`average.xsl`:

```xml
{{#include ../examples/xslt-streaming/average.xsl}}
```

```console
$ sxml transform average.xsl orders.xml
average.xsl:5:3: err:XTSE3430: a template rule of a streamable mode is not guaranteed-streamable (XSLT 4.0, 6.6.4): its body is free-ranging
```

The stylesheet is rejected before anything runs, with the error the XSLT
specification gives. The rules that decide what is *guaranteed
streamable* are long, but the idea is short: one downward pass, no
looking back and no looking ahead. To compute several things in one
pass, use accumulators or `xsl:iterate`.

## Accumulators

An *accumulator* is a value that the processor updates as it goes
through the document. Each `xsl:accumulator-rule` matches nodes, and
computes the new value from the old one, `$value`.
`accumulator-before` and `accumulator-after` read the value at a node,
before and after its content:

`summary.xsl`:

```xml
{{#include ../examples/xslt-streaming/summary.xsl}}
```

```console
$ sxml transform summary.xsl orders.xml
25000 orders read
50000 orders read
75000 orders read
100000 orders read
100000 orders, the largest 99.9
```

The progress lines are `xsl:message` output, which goes to standard
error. The two accumulators were computed in the same single pass.

## Iterating

`xsl:iterate` is a loop over a sequence that carries parameters from one
item to the next, and that can stop early with `xsl:break`. It streams,
and stopping early means the rest of the input is never processed:

`running.xsl`:

```xml
{{#include ../examples/xslt-streaming/running.xsl}}
```

```console
$ sxml transform running.xsl orders.xml
Order 141 passes 1,000: the total is 1001.1.
```

## More streaming

Besides modes, XSLT 4.0 has `xsl:source-document streamable="yes"` to
stream a document read by URI, `xsl:fork` to run several downward passes
over the same stream at once, and `xsl:merge` to combine several sorted
inputs. sxml supports all of them. Chapter 11 has more on the memory sxml
uses.

## Summary

XSLT transforms documents by rules:

- template rules match nodes with patterns, and `xsl:apply-templates`
  moves the processing on; the best rule wins by precedence and
  priority;
- modes process the same nodes in different ways, and a mode with
  `on-no-match="shallow-copy"` copies everything you don't change;
- keys index documents, and `xsl:for-each-group` groups by value or by
  position;
- result documents write many files, and imports and packages organize
  large stylesheets;
- streamable modes, accumulators and `xsl:iterate` transform files of any
  size in constant memory.
