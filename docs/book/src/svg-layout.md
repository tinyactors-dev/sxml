# Computing a Layout

<!-- examples: svg -->

A layout says where each box goes. Ours is simple and predictable: the
nodes that nothing calls go in the top row, the nodes they call in the
next row, and so on. Each row is centred, and the nodes of a row keep
the order of the description. Graphviz and its friends do much more
(they move nodes to avoid crossings and route lines around boxes), but
this one fits on a page, and it is enough for the diagrams of this book.

The functions of this section live in the library module `draw.xqm`,
namespace `urn:example:draw`. We'll read it a part at a time and run each
part as we go; [Writing SVG with XQuery](svg-xquery.md) shows the module
as a whole.

## Ranks

The *rank* of a node is its row: how many edges away it is from the top.
The top row starts with the nodes without incoming edges. XPath finds
them, and the nodes one edge further:

```console
$ sxml xpath --context-item=diagram.xml '//node[not(@id = //edge/@to)]/@id/string()'
"gateway"
$ sxml xpath --context-item=diagram.xml '//edge[@from = "flags"]/@to/string()'
"registry"
"cache"
```

Doing that step again and again until no new node turns up is a
*breadth-first search*. The ranks found so far are a map from node id to
rank, and each step adds the nodes one edge away from the last rank,
leaving out the ones that already have a rank. `map:build` makes the
first map from a sequence of keys and a function that gives each key's
value; `fn { 0 }` is a function that ignores its argument:

```console
$ sxml xpath --context-item=diagram.xml 'map:build(("gateway", "flags"), value := fn { 0 })'
{"gateway":0,"flags":0}
```

Here is the search as two functions of `draw.xqm`:

```xquery
{{#include ../examples/svg/draw.xqm:ranks}}
```

`draw:next` finds the *frontier* (the nodes of the last rank) and the
nodes their edges lead to. `while-do` holds the state of the loop, the
map, and calls its third argument as long as its second one says there
is more to do. Because a node gets a rank only once, the search ends
even when the edges go round in a circle.

Two details make it work on any graph. The first node of the description
always starts too, so a diagram in which every node has an incoming edge
(a state machine, say) still has a top row; and nodes the search never
reaches go into one last row, so no node is lost. In a module, `draw:` is
the module's prefix, and `%private` keeps `draw:next` out of the
module's interface.

Let's run it. An `import module` declaration in the query names the
module's namespace and file, and `--method=adaptive` shows the map:

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:ranks(/diagram)'
{"gateway":0,"flags":1,"registry":2,"cache":2,"database":3}
```

A diagram with cycles works the same way. `states.xml` has four states
of a feature flag; every state has an incoming edge except `archived`:

`states.xml`:

```xml
{{#include ../examples/svg/states.xml}}
```

```console
$ sxml xquery --method=adaptive --context-item=states.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:ranks(/diagram)'
{"stable":0,"archived":0,"checking":1,"rolling-out":2}
```

`stable` is first, so it starts the search; `archived` has no incoming edge,
so it starts too. The edges back to `stable` change nothing, because it
already has a rank. And here are two nodes that only point at each
other, behind a first node that leads nowhere: they go into the last row.

```console
$ sxml xquery --method=adaptive --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:ranks(<diagram><node id="a"/><node id="b"/><node id="c"/><edge from="b" to="c"/><edge from="c" to="b"/></diagram>)'
{"a":0,"b":1,"c":1}
```

## The same search as a fold

`while-do` stops when the frontier leads nowhere new. The search can
also be written as a fixed number of passes: a graph of *n* nodes has no
rank above *n* − 1, so *n* passes of `fold-left` are always enough, and
pass number *p* ranks the nodes one edge away from rank *p* − 1:

`ranks-fold.xq`:

```xquery
{{#include ../examples/svg/ranks-fold.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml ranks-fold.xq
{"gateway":0,"flags":1,"registry":2,"cache":2,"database":3}
$ sxml xquery --method=adaptive --context-item=states.xml ranks-fold.xq
{"stable":0,"archived":0,"checking":1,"rolling-out":2}
```

The results are the same. The fold says plainly that it ends, and it
does its last passes for nothing; `while-do` stops as soon as it can. The
`distinct-values` matters in both: `map:build` keeps every value given
for a key, so a node that two frontier nodes lead to would get its rank
twice:

```console
$ sxml xpath 'map:build(("a", "b", "a"), value := fn { 0 })'
{"a":(0,0),"b":0}
```

## Rows

With the ranks known, the rows are a grouping: the nodes grouped by their
rank, in the order of the ranks. Each group becomes an array of ids, so
that the rows stay apart (a sequence of sequences would flatten into one
sequence):

`rows.xq`:

```xquery
{{#include ../examples/svg/rows.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml rows.xq
["gateway"]
["flags"]
["registry","cache"]
["database"]
$ sxml xquery --method=adaptive --context-item=states.xml rows.xq
["stable","archived"]
["checking"]
["rolling-out"]
```

`group by` keeps the nodes of each group in their order in the
description, so `registry` comes before `cache`.

## Places as records

Each box needs a place: the `x` and `y` of its top left corner. A map
with two entries holds that well, and a *record type* says which entries
it has and what they hold. `declare record` names the type and also
declares a function of the same name that makes one. The module starts
with the sizes of the drawing and two records:

```xquery
{{#include ../examples/svg/draw.xqm:sizes}}
```

A record is a map, so `?x` reads an entry. The type checks what goes in:

```console
$ sxml xquery --method=adaptive --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:place(110, 20), draw:place(110, 20)?x'
{"x":110,"y":20}
110
$ sxml xquery --method=adaptive --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:place(1.5, 20)'
expression:1:66: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

`draw:layout` uses a record type for its result too, with a typed map
inside: `map(xs:string, draw:place)` is a map from strings to places.

## The widest row, and centring

A row of *n* boxes is *n* × 140 wide plus *n* − 1 gaps of 40. The drawing
is as wide as its widest row plus the padding on both sides, and each row
starts half of its free space in from the left. The width of a row is a
small function, kept in a variable so that it can be used twice:

```console
$ sxml xpath 'let $row-width := fn($row as array(*)) { array:size($row) * 140 + (array:size($row) - 1) * 40 } return (["gateway"], ["registry", "cache"]) ! $row-width(.)'
140
320
```

For the services the widest row is 320, so the drawing is 360 wide. The
row of one box starts at (360 − 140) / 2 = 110, and the row of two at
(360 − 320) / 2 = 20. Rows are 40 high with 60 between them, so row *r*
(counting from 1) is at *y* = 20 + (*r* − 1) × 100. Here is all of it:

```xquery
{{#include ../examples/svg/draw.xqm:layout}}
```

The places come from two nested loops: `for $row at $row-number` over
the sequence of rows, and `for member $id at $position` over the
members of one row's array. Each pair gives a one-entry map, and
`map:merge` puts them together.

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:layout(/diagram)'
{"width":360,"height":380,"places":{"gateway":{"x":110,"y":20},"flags":{"x":110,"y":120},"registry":{"x":20,"y":220},"cache":{"x":200,"y":220},"database":{"x":110,"y":320}}}
```

These are the numbers in `diagram.svg`: the `rect` of `registry` is at
`x="20" y="220"`.

## Where the lines go

An edge is a line between two boxes. Which sides it joins depends on
where the boxes are, and `draw:line` returns the line's ends as four
numbers, or nothing for an edge from a node to itself, which
`draw:loop` draws as a small curve below the box instead:

```xquery
{{#include ../examples/svg/draw.xqm:line}}
```

An edge down, the common case, leaves the bottom centre of its source
and arrives at the top centre of its target. An edge up (in `states.xml`,
`check.failed` from `checking` back to `stable`) runs a little to the
right, so that it doesn't cover the edge that goes down between the same
boxes. Edges in one row join the sides. A line that passes a box on its
way is hidden under it, because the boxes are drawn last; a bigger tool
would route it around.

```console
$ sxml xquery --method=adaptive --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:line(draw:place(110, 120), draw:place(20, 220)), draw:loop(draw:place(20, 20))'
180
160
90
220
"M 40 60 C 40 84, 70 84, 70 60"
```

## The layout as XML

Maps and records are what XQuery computes with, but another tool can't
read them. `layout.xq` writes the layout as XML: the description again,
with the size of the drawing on `diagram`, the place on each `node`, and
the line (or the loop's path, `d`) on each `edge`:

`layout.xq`:

```xquery
{{#include ../examples/svg/layout.xq}}
```

Attributes are copied from the description and made with computed
attribute constructors: `attribute { $name } { … }` takes its name from
an expression, which is how the four ends of a line get their names.

```console
$ sxml xquery --context-item=diagram.xml layout.xq
<diagram title="Vane services" width="360" height="380">
  <node id="gateway" label="api-gateway" x="110" y="20"/>
  <node id="flags" label="flag-service" x="110" y="120"/>
  <node id="registry" label="flag-registry" x="20" y="220"/>
  <node id="cache" label="flag-cache" kind="store" x="200" y="220"/>
  <node id="database" label="postgresql" kind="store" x="110" y="320"/>
  <edge from="gateway" to="flags" label="HTTP" x1="180" y1="60" x2="180" y2="120"/>
  <edge from="flags" to="registry" label="HTTP" x1="180" y1="160" x2="90" y2="220"/>
  <edge from="flags" to="cache" label="Redis" x1="180" y1="160" x2="270" y2="220"/>
  <edge from="registry" to="database" label="SQL" x1="90" y1="260" x2="180" y2="320"/>
</diagram>
```

[Drawing with XSLT](svg-xslt.md) draws from this file. First, the
drawing in XQuery itself.
