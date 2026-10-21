# FLWOR Expressions

<!-- examples: spec -->

FLWOR (say "flower") stands for the clauses `for`, `let`, `where`,
`order by` and `return`. A FLWOR expression is a pipeline: each clause
takes a stream of variable bindings and passes on a new stream, and
`return` makes one result for each binding that reaches the end.

## for, where, order by, return

Which requirements are recommendations, SHOULD or SHOULD NOT?

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    where starts-with($requirement/bcp14, "SHOULD")
    order by $requirement/@anchor
    return `{$requirement/@anchor}: {$requirement/bcp14}`'
"req-10: SHOULD NOT"
"req-3: SHOULD"
"req-8: SHOULD"
```

`for` binds `$requirement` to each paragraph with an anchor in turn,
`where` drops the ones whose keyword doesn't start with SHOULD,
`order by` sorts the rest, and `return` gives a line for each. A FLWOR
expression can be written across several lines; the shell keeps the line
breaks inside the quotes.

The order is right for strings, and wrong for people: an anchor is text,
and `"req-10"` comes before `"req-3"` as `"10"` comes before `"3"` in a
dictionary. Sort by the number in the anchor instead:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    where starts-with($requirement/bcp14, "SHOULD")
    order by xs:integer(substring-after($requirement/@anchor, "req-"))
    return `{$requirement/@anchor}: {$requirement/bcp14}`'
"req-3: SHOULD"
"req-8: SHOULD"
"req-10: SHOULD NOT"
```

## let

`let` binds a variable once for each binding that reaches it, without
making more bindings. Which requirements are long?

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    let $words := count(tokenize(normalize-space($requirement)))
    where $words > 15
    order by $words descending
    return `{$requirement/@anchor}: {$words} words`'
"req-6: 24 words"
"req-4: 19 words"
"req-11: 17 words"
"req-3: 16 words"
"req-7: 16 words"
```

The string value of a paragraph is all of its text, the words inside
`bcp14` and `xref` included, so `tokenize` splits the whole sentence.

## Positions

`at` binds a second variable to the position of each item. The
requirements in document order, numbered as a reader of the draft sees
them, with the section each one is in:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement at $position in //t[@anchor]
    return `{$position}. {$requirement/@anchor} ({$requirement/ancestor::section[1]/name})`'
"1. req-1 (Flag States)"
"2. req-2 (Flag States)"
"3. req-3 (Flag States)"
"4. req-4 (Rollout)"
"5. req-5 (Rollout)"
"6. req-9 (Rollout)"
"7. req-6 (Status)"
"8. req-10 (Status)"
"9. req-7 (Pause)"
"10. req-8 (Pause)"
"11. req-11 (Pause)"
```

The anchors and the positions differ: an anchor is a name given once,
when a requirement is written, and the later draft put req-9, req-10 and
req-11 into sections that already had requirements. The number a reader
sees is computed from the position, so it never has to be kept up to
date by hand.

## Joins

A FLWOR expression can bring things together that the document keeps
apart. A term is defined once, as a `dt` with an anchor, and used many
times, as an `xref` that targets that anchor. For each use, a `let`
finds the definition:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $xref in //xref[starts-with(@target, "term-")]
    let $term := //dt[@anchor = $xref/@target]
    return `{$xref} -> {$term}, in {$xref/ancestor::*[@anchor][1]/@anchor}`'
"SDK -> SDK, in intro"
"flag -> Flag, in terminology"
"flag -> Flag, in req-1"
"percentage -> Percentage, in req-4"
"pause -> Pause, in req-5"
"pause -> Pause, in req-7"
```

`ancestor::*[@anchor][1]` is the nearest enclosing element with an
anchor: a requirement, or else a section.

A join can bring two documents together as well. `doc("vane-00.xml")`
reads the earlier draft. Anchors don't change between versions, so a
requirement of the new draft finds its earlier version by its anchor,
and comparing the text tells what changed:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $new in //t[@anchor]
    let $old := doc("vane-00.xml")//t[@anchor = $new/@anchor]
    where normalize-space($old) ne normalize-space($new)
    return `{$new/@anchor}: {if (empty($old)) then "added" else "changed"}`'
"req-9: added"
"req-10: added"
"req-7: changed"
"req-8: changed"
"req-11: added"
```

For an added requirement `$old` is empty, its string value is the empty
string, and `ne` finds it different too. `normalize-space` makes the
comparison ignore how the lines are broken. A requirement that the new
draft removed would not be found this way, as the loop runs over the
new draft only; the project at the end of the chapter looks in both
directions.

`doc` reads a document by its URI. The `sxml` command resolves a relative
URI against the working directory, so `vane-00.xml` is the file in the
directory you run the command from.
[Files and the Outside World](files.md) explains how files are found.

## group by

`group by` gathers the bindings that have the same key into one. After
it, the grouping variable holds the key, and every other variable holds
*all* the values of its group:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    group by $section := string($requirement/ancestor::section[1]/name)
    return $section || ": " || count($requirement)'
"Flag States: 3"
"Rollout: 3"
"Status: 2"
"Pause: 3"
```

Before `group by`, `$requirement` is one paragraph. After it,
`$requirement` is the sequence of all the paragraphs of a section, so
`count($requirement)` counts them. The groups come in the order their
first members had. [Grouping](xquery-grouping.md) goes further.

## count

`count` numbers the bindings at that point of the pipeline, which makes
ranking easy. Now `keywords.xq` from the start of the chapter reads
clearly: group the requirements by keyword, count each group, sort by
the count and then the keyword, number the groups, and keep the first
three.

```xquery
{{#include ../examples/spec/keywords.xq:4:}}
```

`where` after `count` filters by the number that `count` gave. Before
the `order by`, the number would be the position in document order
instead.

## while

`while` ends the pipeline at the first binding for which its condition is
false, unlike `where`, which only skips it. Which requirements come
before the first SHOULD?

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[bcp14]
    while $requirement/bcp14 ne "SHOULD"
    return string($requirement/@anchor)'
"req-1"
"req-2"
```

req-3 is the first SHOULD, and nothing after it is looked at, though
req-4 is a MUST again. With `where`, the result would have every
requirement except the SHOULDs.

## More clauses

FLWOR expressions have two more tools: `group by` with several keys and
computed keys, in the next section, and *window clauses*, which bind a
variable to runs of consecutive items. Windows are taught on trace data,
where runs of neighbouring calls are the whole point, in
[Who Calls Whom, and How Often](traces-cardinality.md).
