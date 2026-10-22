# Computing a Layout

<!-- examples: svg -->

A layout says where each box goes and which way each line runs. The
method of this chapter is the one most tools for such diagrams follow:
the *layered* method of Sugiyama, Tagawa and Toda, which Graphviz's
`dot` and the layered algorithm of the Eclipse Layout Kernel (ELK) are
built on. It puts the nodes in layers, one under the other, so that the
edges point down; it orders each layer so that few edges cross; and only
then does it work out coordinates. ELK adds an idea this chapter uses
throughout: the order of the description, its *model order*, is
information. Wherever two choices are equally good, the one that keeps
the order the author wrote wins, so the author steers the drawing by
the order of the nodes and edges and never moves a box by hand (Sören
Domrös, Max Riepe and Reinhard von Hanxleden, *Model Order in Sugiyama
Layouts*, IVAPP 2023).

The method has five phases:

1. **Break the cycles.** Turn some edges round, so that no path leads
   in a circle.
2. **Assign layers.** Give every node a layer, so that every edge
   points down.
3. **Order the layers.** Sort each layer, so that few edges cross.
4. **Place.** Work out the x of every box.
5. **Route.** Draw the path of every edge.

Each phase is a function of the library module `draw.xqm`, namespace
`urn:example:draw`. We'll read the module a part at a time and run each
part as we go; [Writing SVG with XQuery](svg-xquery.md) shows how it
draws.

## Breaking cycles

Layers only work if every edge can point down, and an edge that closes
a circle cannot: some edge of the circle has to point up. A state
machine has circles everywhere. `states.xml` has four states of a
feature flag, and two transitions lead back to `stable`:

`states.xml`:

```xml
{{#include ../examples/svg/states.xml}}
```

The first phase picks the edges to turn round. Drawn upward, they still
point where they go, but for the layout they point down like the
others. Finding the fewest such edges is a hard problem, so layout
tools use a heuristic. Ours is a *depth-first search*, the one Graphviz
uses: start at a node, follow its first edge, and the first edge of the
node that leads to, and so on, and come back to try the next edge only
when a node has no edges left to follow. The nodes on the way from the
start to where the search is now are its *path*. An edge to a node on
the path closes a circle; that edge is turned round.

```xquery
{{#include ../examples/svg/draw.xqm:cycles}}
```

`draw:visit` calls itself: an XQuery function may be *recursive*, and
the search is easiest to say that way. The state of the search is a map
with three entries, passed from one step to the next: the nodes seen,
the path, and the positions of the edges found to turn. `map:put`
returns a new map with one entry changed; maps are values, and the old
one is still there, which is why `draw:visit` can give back the path as
it found it, `$search?path`, when it is done with a node. `fold-left`
carries the map through the edges of a node and, in `draw:reversed`,
through the nodes of the description. In a module, `draw:` is the
module's prefix, and `%private` keeps `draw:visit` out of the module's
interface.

The search starts at the first node of the description and follows the
edges in their order, and that is where the model order comes in.
Here, the search starts at `stable` and goes down to `checking` and
`rolling-out`; the edges from them back to `stable` close circles. An
`import module` declaration in the query names the module's namespace
and file:

```console
$ sxml xquery --method=adaptive --context-item=states.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; for $position in draw:reversed(/diagram) return /diagram/edge[$position] ! `{@from} -> {@to}`'
"checking -> stable"
"rolling-out -> stable"
```

The Vane diagram has no circles, and the search turns nothing round:

```console
$ sxml xquery --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; count(draw:reversed(/diagram))'
0
```

## The graph to lay out

The phases after the first work on a graph of their own: the nodes,
the edges with the reversed ones turned round, and one more thing. An
edge label needs room, and a label drawn halfway along a line lands on
whatever happens to be there. ELK gives every label a *label vertex*: a
small node of its own, put in the middle of its edge, that the layers,
the order and the places treat like any other. A labelled edge becomes
two links, one to its label vertex and one from it.

A vertex and a link are maps with fixed entries, and a *record type*
says which entries those are and what they hold. `declare record` names
the type and also declares a function of the same name that makes one.
The module starts with the sizes of the drawing and its records:

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

A box is as wide as its label needs. The width of a text depends on the
font, which the layout doesn't have, so `draw:text-width` estimates it
from the average width of a letter. Here is the graph:

```xquery
{{#include ../examples/svg/draw.xqm:graph}}
```

A node with a loop, an edge to itself, reserves room for the loop on its
right; loops never get into the layers. The key of a vertex is its
place in the description, and a label vertex takes the key of the node
its edge leaves, plus a half, so that it sorts just after it. Here are
the links of `states.xml`: the turned edges now point from `stable`
down to their label vertices, and from those down to where they came
from:

```console
$ sxml xquery --method=adaptive --context-item=states.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:graph(/diagram)?links ! `{?from} -> {?to}`'
"stable -> label-1"
"label-1 -> checking"
"checking -> label-2"
"label-2 -> rolling-out"
"stable -> label-3"
"label-3 -> checking"
"stable -> label-4"
"label-4 -> rolling-out"
"archived -> label-5"
"label-5 -> stable"
```

## Layers

The layer of a vertex is the length of the longest path that leads to
it: a vertex nothing leads to is in layer 0, and every other one is one
layer below the lowest of the vertices that lead to it. Every link then
points down. The layers are a map from vertex id to layer, and the
module finds them the way you would by hand: start with every vertex in
layer 0, and move vertices down until no link points up or sideways.
`while-do` holds the state of that loop, the map, and calls its third
argument as long as its second one says there is more to do.
`map:build` makes each new map from the ids and a function that gives
each id its layer:

```xquery
{{#include ../examples/svg/draw.xqm:layers}}
```

The longest path puts every vertex that nothing leads to in layer 0,
even one that only leads to something far below. So a second step
lowers such a vertex to just above the highest vertex it leads to, and
its links stay short. In the Vane diagram the two SDKs are in layer 0,
their `HTTP` labels in layer 1, the API in layer 2:

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:layers(draw:graph(/diagram))'
{"browser":0,"node":0,"api":2,"flags":3,"registry":4,"cache":4,"postgresql":6,"redis":6,"label-1":1,"label-2":1,"label-6":5,"label-7":5}
```

A link that skips layers is a problem for the next phase, which only
compares neighbouring layers. `draw:proper` breaks such a link up: it
adds a *bend vertex* in every layer the link passes, so that every link
joins two neighbouring layers. In `states.xml` the edge from
`rolling-out` back to `stable` passes two layers:

```console
$ sxml xquery --method=adaptive --context-item=states.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; let $graph := draw:graph(/diagram) return draw:proper($graph, draw:layers($graph))?vertices[?kind = "bend"]?id'
"bend-4-4"
"bend-4-5"
```

## Order

Two links between the same two layers cross when their upper ends are
in one order and their lower ends in the other. The third phase sorts
the vertices of each layer so that few links cross. The method is a
*layer sweep*: go down the layers, and sort each by the positions of
the vertices above it, then go up again and sort each by the vertices
below it. A vertex's sort key is its *barycentre*, the mean position of
its neighbours in the other layer, so that it moves towards them.

```xquery
{{#include ../examples/svg/draw.xqm:order}}
```

The model order matters twice here. The first order of every layer is
the order of the keys, the order of the description. And `sort`, with
the key function as its third argument, is *stable*: two vertices with
the same barycentre keep the order they had. The result is the order
with the fewest crossings that any sweep found, and on a tie the
earlier one wins, so the order of the description stays whenever no
sweep does better. Here are the layers of `states.xml`, top to bottom:

```console
$ sxml xquery --method=adaptive --context-item=states.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; let $graph := draw:graph(/diagram) return draw:orders(draw:proper($graph, draw:layers($graph)))'
["archived"]
["label-5"]
["stable"]
["label-1","label-3","label-4"]
["checking","bend-4-4"]
["label-2","bend-4-5"]
["rolling-out"]
```

## Places

Now coordinates. Each vertex wants to be at the mean x of its
neighbours, so that its links are short and steep, but the vertices of
a layer must keep their order and a gap between them. Finding the x
closest to what each one wants, under those rules, is a small classic.
Subtract from each wanted x the room the vertices before it need; the
rule "at least that far right of the one before" becomes "never smaller
than the one before". Then go along the layer, and wherever a value is
smaller than the one before, pool the two into one block at their mean,
and keep pooling backwards while blocks are out of order. This is
*isotonic regression* by *pooling adjacent violators*, and it gives the
closest positions there are:

```xquery
{{#include ../examples/svg/draw.xqm:place}}
```

Two boxes 100 wide that both want to be at 100 can't be: they need 130
between their centres, 50 + 30 + 50, so they share the difference, 65
each way:

```console
$ sxml xquery --method=adaptive --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:pack(([50, 50], [50, 50]), (100, 100))'
35
165
```

`draw:xs` runs this for each layer in passes down and up the layers.
A bend vertex has one neighbour above and one below, and so lines up
with them, and the long edge it belongs to comes out nearly straight.

## Routes

The last phase draws the edges. Each one leaves the bottom of its upper
box and enters the top of its lower one; where several edges meet a
side, their ends are spread along it in the order of where they go, so
that they don't cross at the box. Between layers an edge is a cubic
Bézier curve that leaves and arrives vertically, the `C` command of an
SVG path; past a label it runs straight down, with the label to its
right. A loop goes round the right side of its box:

```xquery
{{#include ../examples/svg/draw.xqm:route}}
```

```console
$ sxml xquery --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:path((draw:place(90, 60), draw:place(90, 124)), false()), draw:loop(draw:box(20, 20, 100, 40))?path'
M 90 60 C 90 92, 90 92, 90 124 M 120 30 C 142 22, 142 58, 120 50
```

The curve from (90, 60) to (90, 124) has its two control points at the
height halfway between, straight above and below its ends.

## The whole layout

`draw:layout` runs the five phases and turns the results into a record:
the size of the drawing, a box per node and a route per edge, by the
edge's position in the description. A reversed edge is routed the other
way round, from its end to its start, so that its arrow points the way
the edge goes.

```xquery
{{#include ../examples/svg/draw.xqm:layout}}
```

```console
$ sxml xquery --method=adaptive --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:layout(/diagram)?boxes'
{"browser":{"x":20,"y":20,"width":126,"height":40},"node":{"x":176,"y":20,"width":102,"height":40},"api":{"x":105,"y":124,"width":100,"height":40},"flags":{"x":104,"y":188,"width":102,"height":40},"registry":{"x":20,"y":252,"width":126,"height":40},"cache":{"x":176,"y":252,"width":102,"height":40},"postgresql":{"x":32,"y":356,"width":102,"height":40},"redis":{"x":177,"y":356,"width":100,"height":40}}
```

These are the boxes in `diagram.svg`: the `rect` of `registry` has the
`x`, `y` and width of its box.

## The layout as XML

Maps and records are what XQuery computes with, but another tool can't
read them. `layout.xq` writes the layout as XML: the description again,
with the size of the drawing on `diagram`, the box on each `node`, and
the path (`d`) and the place of the label on each `edge`:

`layout.xq`:

```xquery
{{#include ../examples/svg/layout.xq}}
```

Attributes are copied from the description and made with computed
attribute constructors: `attribute { $name } { … }` takes its name from
an expression, which is how the four numbers of a box get their names.
An edge between two modules has no `label` in the description, so it
has no label place either: `$route?label ! (…)` makes the attributes
only when there is one.

```console
$ sxml xquery --context-item=diagram.xml layout.xq
<diagram title="Vane" width="299" height="416">
  <node id="browser" label="@vane/browser" x="20" y="20" width="126" height="40"/>
  <node id="node" label="@vane/node" x="176" y="20" width="102" height="40"/>
  <node id="api" label="vane/api" x="105" y="124" width="100" height="40"/>
  <node id="flags" label="vane/flags" x="104" y="188" width="102" height="40"/>
  <node id="registry" label="vane/registry" x="20" y="252" width="126" height="40"/>
  <node id="cache" label="vane/cache" x="176" y="252" width="102" height="40"/>
  <node id="postgresql" label="postgresql" kind="store" x="32" y="356" width="102" height="40"/>
  <node id="redis" label="redis" kind="store" x="177" y="356" width="100" height="40"/>
  <edge from="browser" to="api" label="HTTP" d="M 83 60 C 83 72, 123 72, 123 84 L 123 100 C 123 112, 138 112, 138 124" label-x="128" label-y="96"/>
  <edge from="node" to="api" label="HTTP" d="M 227 60 C 227 72, 187 72, 187 84 L 187 100 C 187 112, 171 112, 171 124" label-x="192" label-y="96"/>
  <edge from="api" to="flags" d="M 155 164 C 155 176, 155 176, 155 188"/>
  <edge from="flags" to="registry" d="M 138 228 C 138 240, 83 240, 83 252"/>
  <edge from="flags" to="cache" d="M 172 228 C 172 240, 227 240, 227 252"/>
  <edge from="registry" to="postgresql" label="SQL" d="M 83 292 C 83 304, 83 304, 83 316 L 83 332 C 83 344, 83 344, 83 356" label-x="88" label-y="328"/>
  <edge from="cache" to="redis" label="Redis" d="M 227 292 C 227 304, 227 304, 227 316 L 227 332 C 227 344, 227 344, 227 356" label-x="232" label-y="328"/>
</diagram>
```

[Drawing with XSLT](svg-xslt.md) draws from this file. First, the
drawing in XQuery itself.
