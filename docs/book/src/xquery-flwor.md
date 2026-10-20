# FLWOR Expressions

<!-- examples: xquery -->

FLWOR (say "flower") stands for the clauses `for`, `let`, `where`,
`order by` and `return`. A FLWOR expression is a pipeline: each clause
takes a stream of variable bindings and passes on a new stream, and
`return` makes one result for each binding that reaches the end.

## for, where, order by, return

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    where $book/@year < 1970
    order by $book/title
    return $book/title/string()'
"Dune"
"The Art of Computer Programming"
"The Left Hand of Darkness"
```

`for` binds `$book` to each book in turn, `where` drops the books from
1970 and later, `order by` sorts the rest, and `return` gives each
title. A FLWOR expression can be written across several lines; the
shell keeps the line breaks inside the quotes.

## let

`let` binds a variable once for each binding that reaches it, without
making more bindings:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    let $pages := xs:integer($book/@pages)
    where $pages > 500
    order by $pages descending
    return `{$book/@id}: {$pages}`'
"geb: 777"
"taocp: 672"
"sicp: 657"
```

## Positions

`at` binds a second variable to the position of each item:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book at $position in //book
    return $position || ". " || $book/title'
"1. Godel, Escher, Bach"
"2. The Art of Computer Programming"
"3. Structure and Interpretation of Computer Programs"
"4. Dune"
"5. The Left Hand of Darkness"
```

## Joins

A FLWOR expression can bring two documents together. `doc("loans.xml")`
reads the loans; for each loan, a `let` finds the book with the same id:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $loan in doc("loans.xml")//loan
    let $book := //book[@id = $loan/@book]
    return $loan/@member || " borrowed " || $book/title'
"ada borrowed Dune"
"alan borrowed Godel, Escher, Bach"
"grace borrowed Dune"
"ada borrowed Structure and Interpretation of Computer Programs"
"ada borrowed The Left Hand of Darkness"
"alan borrowed Dune"
```

`doc` reads a document by its URI. The `sxml` command resolves a relative
URI against the working directory, so `loans.xml` is the file in the
directory you run the command from. Chapter 8 explains how files are found.

## group by

`group by` gathers the bindings that have the same key into one. After
it, the grouping variable holds the key, and every other variable holds
*all* the values of its group:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $loan in doc("loans.xml")//loan
    group by $member := string($loan/@member)
    order by count($loan) descending
    return $member || ": " || count($loan)'
"ada: 3"
"alan: 2"
"grace: 1"
```

Before `group by`, `$loan` is one loan. After it, `$loan` is the
sequence of all of a member's loans, so `count($loan)` counts them.

## count

`count` numbers the bindings at that point of the pipeline, which makes
ranking easy. Now `popular.xq` from the start of the chapter reads
clearly: group the loans by book, count each group, sort by the count,
number the groups, and keep the first three.

```xquery
{{#include ../examples/xquery/popular.xq:4:}}
```

## while

`while` ends the pipeline at the first binding for which its condition is
false, unlike `where`, which only skips it:

```console
$ sxml xquery --context-item=library.xml --method=adaptive --expression='
    for $book in //book
    while $book/@year > 1966
    return string($book/@id)'
"geb"
"taocp"
"sicp"
```

## Windows

A *window clause* binds a variable to runs of consecutive items, which
is useful for batches, moving averages and documents with structure
hidden in a flat list. A tumbling window starts a new window when the
previous one ends:

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

`start at $start` and `end at $end` bind the positions where a window
starts and ends, and the `when` conditions decide where the boundaries
are. Here, each window ends two items after it starts. A `sliding`
window starts a window at every item where the start condition holds,
so its windows overlap. `only end` drops a last window that never met
its end condition:

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

That's a moving average of each pair of neighbours.
