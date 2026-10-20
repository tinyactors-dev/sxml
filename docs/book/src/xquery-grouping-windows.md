# Grouping and Windows

<!-- examples: xquery -->

Reports are mostly about putting things together: the loans of each
member, the books of each decade, the events of each day. XQuery has two
clauses for this. `group by` puts together the bindings that share a
key, wherever they are. A *window clause* puts together items that are
next to each other.

## Grouping by a computed key

The key of `group by` can be any expression whose value is one atomic
value (or nothing). Here the books are grouped by the decade they were
written in:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    group by $decade := xs:integer($book/@year) idiv 10 * 10
    order by $decade
    return $decade || "s: " || string-join($book/@id, ", ")'
"1960s: taocp, dune, lhod"
"1970s: geb"
"1980s: sicp"
```

Remember the rule from the previous section: after `group by`, the
grouping variable `$decade` is one value, and every other variable,
here `$book`, holds all the values of its group, in the order they had
before.

A grouping variable can also be one that a `let` bound earlier. Then
`group by $shelf` alone is enough:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    let $shelf := string($book/../@name)
    group by $shelf
    return { "shelf": $shelf, "books": count($book), "pages": sum($book/@pages ! xs:integer(.)) }'
{"shelf":"Computing","books":3,"pages":2106}
{"shelf":"Fiction","books":2,"pages":716}
```

Each group became a map, which is often the most convenient form for a
summary, and which the json output method writes as JSON.

## Several keys

With more than one key, a group is made for each combination of keys
that occurs. These are the loans of each member in each month:

```console
$ sxml xquery --method=adaptive --expression='
    for $loan in doc("loans.xml")//loan
    group by $member := string($loan/@member),
             $month := substring($loan/@date, 1, 7)
    order by $member, $month
    return $member || " " || $month || ": " || count($loan)'
"ada 2026-09: 2"
"ada 2026-10: 1"
"alan 2026-09: 1"
"alan 2026-10: 1"
"grace 2026-09: 1"
```

## After the grouping

Clauses after `group by` work on the groups. `let` computes something
for each group, `where` keeps some groups and drops others, and
`order by` sorts them. Which tags are on more than one book?

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $tag in //tag
    group by $name := string($tag)
    where count($tag) > 1
    order by $name
    return $name || ": " || string-join($tag/ancestor::book/@id, ", ")'
"algorithms: taocp, sicp"
"politics: dune, lhod"
```

How many different members borrowed each book?

```console
$ sxml xquery --method=adaptive --expression='
    for $loan in doc("loans.xml")//loan
    group by $book := string($loan/@book)
    let $members := distinct-values($loan/@member)
    order by count($members) descending, $book
    return $book || ": " || count($loan) || " loans by " || count($members) || " members"'
"dune: 3 loans by 3 members"
"geb: 1 loans by 1 members"
"lhod: 1 loans by 1 members"
"sicp: 1 loans by 1 members"
```

Keys are compared as `distinct-values` compares them: strings by their
characters, numbers as numbers. A `collation` after the key compares
strings by other rules, for example without regard to case:

```console
$ sxml xquery --method=adaptive --expression='
    for $word in ("Apple", "apple", "Banana")
    group by $key := $word
        collation "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive"
    return $key || ": " || count($word)'
"Apple: 2"
"Banana: 1"
```

The key of a group is the key of its first member, here `"Apple"`.
Chapter 7 has more on collations.

## Tumbling windows

`group by` doesn't care where the members of a group are. Sometimes
position is the whole point: a document can hide its structure in a
flat list. These notes are a list of headings and paragraphs, and each
heading starts a section:

`notes.xml`:

```xml
{{#include ../examples/xquery/notes.xml}}
```

A *tumbling window* cuts a sequence into runs that don't overlap. The
`start` condition says where a new run begins:

```console
$ sxml xquery --context-item=notes.xml --expression='
    declare option output:indent "yes";
    <sections>{
      for tumbling window $section in /notes/*
          start $heading when $heading[self::h1]
      return <section title="{ $heading }">{ tail($section) }</section>
    }</sections>'
<sections>
  <section title="Opening hours">
    <p>Monday to Friday, nine to five.</p>
    <p>Closed on public holidays.</p>
  </section>
  <section title="Membership">
    <p>Membership is free.</p>
  </section>
  <section title="Fines">
    <p>Ten cents a day.</p>
    <p>At most two euros a book.</p>
  </section>
</sections>
```

`$section` is bound to the items of each window in turn, and `$heading`
to the item that starts it. Without an `end` condition, a window ends
just before the next one starts.

Start and end can be bound to more variables: the item itself (`$s`), its
position (`at $p`), and the items before it and after it (`previous
$before`, `next $after`). This tumbling window takes three numbers at a
time, using positions:

```console
$ sxml xquery --method=adaptive --expression='
    for tumbling window $batch in 1 to 10
        start at $start when true()
        end at $end when $end - $start = 2
    return array { $batch }'
[1,2,3]
[4,5,6]
[7,8,9]
[10]
```

Here, each window ends two items after it starts. The last window is
shorter: the sequence ran out before its end condition was met.

## Windows with an end

The library's desk writes a log of what happens during the day. The desk
opens and closes, and between those it lends and takes back books:

`desk.xml`:

```xml
{{#include ../examples/xquery/desk.xml}}
```

A window from each `open` to the next `close` is one session at the
desk. When the end condition is never met, the window ends at the last
item, and the end variable is bound to that last item, so test what it
is:

```console
$ sxml xquery --context-item=desk.xml --method=adaptive --expression='
    for tumbling window $session in //event
        start $open when $open/@kind = "open"
        end $close when $close/@kind = "close"
    let $loans := count($session[@kind = "loan"])
    return `{ $open/@time }: { $loans } loans, {
      if ($close/@kind = "close") then "closed at " || $close/@time else "still open" }`'
"2026-10-19T09:00:00: 1 loans, closed at 2026-10-19T12:30:00"
"2026-10-19T14:00:00: 2 loans, closed at 2026-10-19T17:00:00"
"2026-10-20T09:00:00: 0 loans, still open"
```

`only end` instead of `end` drops a window whose end condition is never
met. That's what you want to compute how long the desk was open:

```console
$ sxml xquery --context-item=desk.xml --method=adaptive --expression='
    for tumbling window $session in //event
        start $open when $open/@kind = "open"
        only end $close when $close/@kind = "close"
    let $hours := (xs:dateTime($close/@time) - xs:dateTime($open/@time))
                  div xs:dayTimeDuration("PT1H")
    return `{ substring($open/@time, 1, 10) }: open { $hours } hours`'
"2026-10-19: open 3.5 hours"
"2026-10-19: open 3 hours"
```

Subtracting two `xs:dateTime` values gives a duration, and dividing a
duration by a duration gives a number.

## Sliding windows

A *sliding window* starts a window at every item where the start
condition holds, so its windows can overlap. Each window here is a pair
of neighbours, and `avg` makes a moving average:

```console
$ sxml xquery --method=adaptive --expression='
    for sliding window $pair in (3, 5, 4, 8, 6)
        start at $start when true()
        only end at $end when $end - $start = 1
    return avg($pair)'
4
4.5
6
7
```

The last item, 6, starts a window too, but it has no neighbour after
it, so `only end` drops that window.

`next` makes it easy to compare an item with the one after it. Which
events came less than ten minutes before the next one?

```console
$ sxml xquery --context-item=desk.xml --method=adaptive --expression='
    for sliding window $w in //event
        start $first next $second
            when xs:dateTime($second/@time) - xs:dateTime($first/@time) lt xs:dayTimeDuration("PT10M")
        end when true()
    return `{ $first/@kind } at { $first/@time }, then { $second/@kind }`'
"open at 2026-10-19T14:00:00, then loan"
"loan at 2026-10-19T14:05:00, then loan"
```

`end when true()` ends each window at the item that starts it: the
windows are only a way to look at each item together with its
neighbour.

## Choosing between them

- Use `group by` when the members of a group can be anywhere, and the
  group is named by a value: a member, a month, a tag.
- Use a tumbling window when the groups are runs of neighbours: the
  paragraphs after a heading, a session from open to close, batches of
  ten.
- Use a sliding window to look at each item with the items around it:
  moving averages, gaps, repeated values.
