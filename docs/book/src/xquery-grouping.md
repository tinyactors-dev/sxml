# Grouping

<!-- examples: spec -->

Reports are mostly about putting things together: the requirements of
each section, the uses of each term, the keywords of each chapter.
`group by` puts together the bindings that share a key, wherever they
are in the document. This section shows how far that goes.

## Grouping by a computed key

The key of `group by` can be any expression whose value is one atomic
value (or nothing). The draft's sections nest, and a reader of the
table of contents thinks in top-level sections. Here the requirements
are grouped by the number of the top-level section they are in:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    group by $number := count($requirement/ancestor::section[last()]/preceding-sibling::section) + 1
    return `{$number}. {$requirement[1]/ancestor::section[last()]/name}: {count($requirement)}`'
"2. Flag States: 3"
"3. Messages: 5"
"4. Pause: 3"
```

`ancestor::section[last()]` is the outermost section around a
requirement, and the sections before it make its number. Section 1, the
Introduction, has no requirements, so it has no group.

Remember the rule from the previous section: after `group by`, the
grouping variable `$number` is one value, and every other variable,
here `$requirement`, holds all the values of its group, in the order
they had before. That's why the name is taken from `$requirement[1]`,
the first requirement of the group.

A grouping variable can also be one that a `let` bound earlier. Then
`group by $keyword` alone is enough:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    let $keyword := string($requirement/bcp14)
    group by $keyword
    order by $keyword
    return { "keyword": $keyword, "count": count($requirement), "anchors": array { $requirement/@anchor ! string() } }'
{"keyword":"MAY","count":2,"anchors":["req-9","req-11"]}
{"keyword":"MUST","count":5,"anchors":["req-1","req-2","req-4","req-6","req-7"]}
{"keyword":"MUST NOT","count":1,"anchors":["req-5"]}
{"keyword":"SHOULD","count":2,"anchors":["req-3","req-8"]}
{"keyword":"SHOULD NOT","count":1,"anchors":["req-10"]}
```

Each group became a map, which is often the most convenient form for a
summary, and which the json output method writes as JSON.

## Several keys

With more than one key, a group is made for each combination of keys
that occurs. These are the keywords of each section:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    group by $section := string($requirement/ancestor::section[1]/name),
             $keyword := string($requirement/bcp14)
    order by $section, $keyword
    return `{$section}, {$keyword}: {count($requirement)}`'
"Flag States, MUST: 2"
"Flag States, SHOULD: 1"
"Pause, MAY: 1"
"Pause, MUST: 1"
"Pause, SHOULD: 1"
"Rollout, MAY: 1"
"Rollout, MUST: 1"
"Rollout, MUST NOT: 1"
"Status, MUST: 1"
"Status, SHOULD NOT: 1"
```

Combinations that don't occur, such as a MAY in Status, make no group.

## After the grouping

Clauses after `group by` work on the groups. `let` computes something
for each group, `where` keeps some groups and drops others, and
`order by` sorts them. Which anchors are referred to more than once, and
from where?

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $xref in //xref
    group by $target := string($xref/@target)
    where count($xref) > 1
    order by $target
    return $target || ": " || string-join($xref/ancestor::*[@anchor][1]/@anchor, ", ")'
"term-flag: terminology, req-1"
"term-pause: req-5, req-7"
```

How many requirements, and how many different keywords, does each
section have?

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    group by $section := string($requirement/ancestor::section[1]/name)
    let $keywords := distinct-values($requirement/bcp14)
    order by count($keywords) descending, $section
    return `{$section}: {count($requirement)} requirements, {count($keywords)} keywords`'
"Pause: 3 requirements, 3 keywords"
"Rollout: 3 requirements, 3 keywords"
"Flag States: 3 requirements, 2 keywords"
"Status: 2 requirements, 2 keywords"
```

Keys are compared as `distinct-values` compares them: strings by their
characters, numbers as numbers. A `collation` after the key compares
strings by other rules, for example without regard to case. A term is
defined as "Flag" and used as "flag"; with this collation, the
definition and its uses fall into one group:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $word in (//dt, //xref[starts-with(@target, "term-")])
    group by $key := string($word)
        collation "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive"
    return $key || ": " || count($word)'
"Flag: 3"
"SDK: 2"
"Percentage: 2"
"Pause: 3"
```

The key of a group is the key of its first member, here the `dt`, which
comes first in the sequence. [Working with Text](text.md) has more on
collations.

## Choosing a key well

A key is atomized, as a function argument is. A node used as a key
becomes its string value, not the node itself:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    group by $chapter := $requirement/ancestor::section[last()]
    return $chapter/name'
expression:4:12: err:XPTY0004: An operand on the left of "/" is not a node (XPath 4.0, section 4.7.3: XPTY0004, formerly XPTY0019)
```

`$chapter` is the whole text of a section as an untyped atomic value,
which has no `name` child. That's why the first example of this section
grouped by a number and took the name from the group's first member.
Text makes a poor key anyway: two sections with the same text would be
one group. Group by something that names the thing, such as a number or
an anchor.

A key must be one value or nothing. Nothing is a key of its own: every
paragraph without a `bcp14` lands in one group, whose key is the empty
sequence:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $paragraph in //t
    group by $keyword := $paragraph/bcp14/string()
    return `{ $keyword otherwise "(none)" }: { count($paragraph) }`'
"(none): 5"
"MUST: 5"
"SHOULD: 3"
"MUST NOT: 1"
"MAY: 2"
"SHOULD NOT: 1"
```

There are three SHOULDs here, not two: the paragraph in Security
Considerations has a keyword but no anchor, so the earlier queries,
which looked only at `//t[@anchor]`, did not see it. The draft's own
editorial note says it needs an anchor; [Updating
Documents](xquery-updates.md) gives it one.

Two values are an error. A paragraph with two keywords can't be put in
one keyword's group:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    for $paragraph in (//t, <t><bcp14>MUST</bcp14> and <bcp14>MAY</bcp14></t>)
    group by $keyword := $paragraph/bcp14
    return count($paragraph)'
expression:3:14: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The draft's convention is one keyword per requirement, and this error is
one way to find a paragraph that breaks it. To group such a paragraph
under each of its keywords, iterate over the keywords instead: `for
$keyword in //bcp14 group by $name := string($keyword)`, and reach the
paragraph as `$keyword/..`.

## Runs of neighbours

`group by` doesn't care where the members of a group are. Sometimes
position is the whole point: the queries a service sends to its
database one after another, the events of one session. XQuery's *window clauses* group
items that are next to each other, in tumbling windows that don't
overlap and sliding windows that do. They are taught on trace data,
where they find a run of repeated calls, in [Who Calls Whom, and How
Often](traces-cardinality.md).
