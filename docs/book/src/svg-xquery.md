# Writing SVG with XQuery

<!-- examples: svg -->

With the layout computed, drawing is a matter of building elements:
a group, a box and a label for each node, a line and a label for each
edge. XQuery's constructors build them, as they build any XML, and the
one thing to get right is the namespace.

## Elements in the SVG namespace

An element constructor written in a query is an element in the result,
and an `xmlns` attribute on it puts it, and the elements inside it, in
a namespace:

```console
$ sxml xquery --expression='<svg xmlns="http://www.w3.org/2000/svg" width="180" height="60"><rect x="20" y="10" width="140" height="40"/></svg>'
<svg xmlns="http://www.w3.org/2000/svg" width="180" height="60"><rect x="20" y="10" width="140" height="40"/></svg>
$ sxml xquery --method=adaptive --expression='namespace-uri(<svg><rect/></svg>), namespace-uri(<svg xmlns="http://www.w3.org/2000/svg"><rect/></svg>/*)'
""
"http://www.w3.org/2000/svg"
```

Without the namespace, the result looks right and draws nothing: to a
browser, `rect` in no namespace is not an SVG rectangle.

A query that writes only SVG can say the namespace once, in its prolog:
`declare default element namespace` puts every unprefixed element name
in the query into it. *Every* name: the names in constructors, and the
names in paths too. That is a trap when the input has no namespace:

```console
$ sxml xquery --context-item=diagram.xml --expression='declare default element namespace "http://www.w3.org/2000/svg"; count(//node), count(//Q{}node)'
0 8
```

`//node` now looks for SVG `node` elements and finds none. `Q{}node`
names an element in no namespace explicitly, whatever the default is.
Here is a first drawing, one box per node in a column, with the
namespace declared once:

`boxes.xq`:

```xquery
{{#include ../examples/svg/boxes.xq}}
```

```console
$ sxml xquery --context-item=diagram.xml boxes.xq
<svg xmlns="http://www.w3.org/2000/svg" width="180" height="500">
  <rect x="20" y="20" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="45" text-anchor="middle">@vane/browser</text>
  <rect x="20" y="80" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="105" text-anchor="middle">@vane/node</text>
  <rect x="20" y="140" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="165" text-anchor="middle">vane/api</text>
  <rect x="20" y="200" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="225" text-anchor="middle">vane/flags</text>
  <rect x="20" y="260" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="285" text-anchor="middle">vane/registry</text>
  <rect x="20" y="320" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="345" text-anchor="middle">vane/cache</text>
  <rect x="20" y="380" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="405" text-anchor="middle">postgresql</text>
  <rect x="20" y="440" width="140" height="40" fill="#eef3fb" stroke="#3b5b92"/>
  <text x="90" y="465" text-anchor="middle">redis</text>
</svg>
```

The default namespace does not touch attributes: an unprefixed attribute
name is always in no namespace, so `$node/@label` works as before.

## Attributes from expressions

Curly braces in an attribute value hold an expression, and literal text
around them stays. When the expression gives several values they are
joined with spaces, and an empty sequence adds nothing, which is just
what the `class` attribute of SVG needs:

```console
$ sxml xquery --expression='<g class="{ "node", "store", () }" id="node-{ "cache" }"/>'
<g class="node store" id="node-cache"/>
```

## Curly braces in text

CSS uses curly braces too. In the content of a direct constructor a
single brace starts an expression, so a style rule written as it is
fails to parse; doubled, `{{` and `}}` are one brace each:

```console
$ sxml xquery --expression='<style>rect { fill: blue }</style>'
expression:1:15: err:XPST0003: The text is not valid XQuery 4.0 syntax (XQuery 4.0, Appendix A)
$ sxml xquery --expression='<style>rect {{ fill: blue }}</style>'
<style>rect { fill: blue }</style>
```

String templates, the strings between backticks, follow the same rule.
The module keeps its five style rules in one:

```xquery
{{#include ../examples/svg/draw.xqm:style}}
```

The second variable holds rules that a drawing gets only when it needs
them: a rule for loops when an edge goes from a node to itself, and one
for each of the classes `added`, `removed` and `changed`, which the
later projects use to draw differences. These are ordinary string
literals, where a brace is just a brace.

## Computed constructors

When the name of an element depends on the data, a *computed
constructor* gives it as an expression: `element { $name } { content }`.
Its attributes can be computed as well, with `attribute name { value }`,
and the content of the braces decides which ones there are. This
variant of the drawing puts each box at its place from `draw:layout`,
and draws stores as ellipses:

`shapes.xq`:

```xquery
{{#include ../examples/svg/shapes.xq}}
```

```console
$ sxml xquery --context-item=diagram.xml shapes.xq
<svg xmlns="http://www.w3.org/2000/svg" width="360" height="480">
  <rect x="20" y="20" width="140" height="40" fill="none" stroke="black"/>
  <rect x="200" y="20" width="140" height="40" fill="none" stroke="black"/>
  <rect x="110" y="120" width="140" height="40" fill="none" stroke="black"/>
  <rect x="110" y="220" width="140" height="40" fill="none" stroke="black"/>
  <rect x="20" y="320" width="140" height="40" fill="none" stroke="black"/>
  <rect x="200" y="320" width="140" height="40" fill="none" stroke="black"/>
  <ellipse cx="90" cy="440" rx="70" ry="20" fill="none" stroke="black"/>
  <ellipse cx="270" cy="440" rx="70" ry="20" fill="none" stroke="black"/>
</svg>
```

A computed name without a prefix is in the default element namespace,
like a direct one, so the ellipses are SVG ellipses. The description is
read as `/Q{}diagram`, because of the trap above.

## The drawing module

`draw.xqm` does not declare a default element namespace. It reads the
description with plain names, and puts the `xmlns` attribute on the
outermost constructor of each function that writes SVG instead. Inside
such a constructor the default namespace is SVG as well, for paths too,
so each function reads what it needs from the description *before* its
constructor, in `let` clauses, and inside uses only variables and
attributes. A node is a group with a box and a label:

```xquery
{{#include ../examples/svg/draw.xqm:node}}
```

An edge is a group with a line and its label at the line's middle, moved
a little to the right and down; an edge from a node to itself gets the
loop instead:

```xquery
{{#include ../examples/svg/draw.xqm:edge}}
```

`$label ! <text …>{ . }</text>` makes a `text` only when there is a
label: `!` over an empty sequence gives an empty sequence. And
`draw:svg` puts it all together:

```xquery
{{#include ../examples/svg/draw.xqm:svg}}
```

The edges come before the nodes, because SVG draws in document order:
the boxes are drawn over the ends of the lines, which makes the arrows
stop exactly at the box. The style is the five rules plus the ones for
the features the diagram uses, one rule per line.

## Writing the file

The main query that draws a diagram is short. It imports the module and
declares how to serialize the result:

`draw.xq`:

```xquery
{{#include ../examples/svg/draw.xq}}
```

`declare option output:indent "yes"` asks for indentation, one element
per line. Without it the only line breaks are the ones in the style
text; a browser draws the result the same way, but it is hard to read
and to compare:

```console
$ sxml xquery --context-item=diagram.xml --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:svg(/diagram)' | wc -l
7
$ sxml xquery --context-item=diagram.xml draw.xq | wc -l
72
```

`--output` writes the result to a file, and only when the query
succeeds. The drawing it writes is the committed `diagram.svg`, byte for
byte:

```console
$ sxml xquery --context-item=diagram.xml --output=drawn.svg draw.xq
$ diff drawn.svg diagram.svg && echo same
same
```

`--output` won't replace a file that exists unless `--force` is given,
so an old drawing is never lost by accident:

```console
$ sxml xquery --context-item=diagram.xml --output=drawn.svg draw.xq
sxml xquery: 'drawn.svg' exists; give --force to replace it
```

## Drawing a change

The optional `class` attribute of a node or an edge is copied to its
group, and the three classes `added`, `removed` and `changed` get their
own style rules. `change.xml` is the Vane system after a change to the
`flags` module: a kill no longer reads its flag from the `cache` module,
each kill is recorded by a new `audit` module, which writes to
PostgreSQL and posts to the owners' webhook at hooks.example.com, and
the way `flags` calls the registry changed. The cache module stays, and
Redis with it, because other modules still use it; only the edge from
`flags` goes. The nodes of each row are in an order that keeps the
lines from crossing: the layout keeps the order of the description, so
the author can still decide that much.

`change.xml`:

```xml
{{#include ../examples/svg/change.xml}}
```

```console
$ sxml xquery --context-item=change.xml --output=change.svg draw.xq
$ sxml xpath --context-item=change.svg '//*:style/string()'
"
    .node rect { fill: #eef3fb; stroke: #3b5b92; stroke-width: 1.5 }
    .store rect { fill: #f6efe3; stroke: #8a6a2f }
    .node text { font: 14px sans-serif; fill: #1b1b1b }
    .edge line { stroke: #555555; stroke-width: 1.5 }
    .edge text { font: 12px sans-serif; fill: #555555 }
    .changed rect, .changed line, .changed path { stroke: #b26a00; stroke-width: 3 }
    .added rect, .added line, .added path { stroke: #2e7d32; stroke-width: 3 }
    .removed rect, .removed line, .removed path { stroke: #c62828; stroke-dasharray: 6 4 }
  "
$ sxml xpath --context-item=change.svg '//*:g[tokenize(@class) = ("added", "removed", "changed")] ! `{@id}: {@class}`'
"edge-flags-cache: edge removed"
"edge-flags-registry: edge changed"
"edge-flags-audit: edge added"
"edge-audit-postgresql: edge added"
"edge-audit-hooks: edge added"
"node-flags: node service changed"
"node-audit: node service added"
"node-hooks: node service added"
```

The added module and the webhook, and the three edges to and from the
audit module, are drawn with a thick green line, the removed edge with a
dashed red one, and the changed module and edge in amber. Each class
rule is in the style once, in the order the classes first appear in the
description: `changed` on `flags`, `added` on `audit`, `removed` on the
first edge. A rule written
later in the style wins over an earlier one of the same weight, which is
why the class rules come last.

## Using the module elsewhere

Any query can draw a diagram by importing the module; all it needs is a
`diagram` element, from a file or built on the spot. The state machine,
call-chain and trace projects each keep a link to `svg/draw.xqm` in
their own directory and import it there. The module's functions are its
interface:

| Function | Result |
| --- | --- |
| `draw:svg($diagram)` | the drawing, an SVG `svg` element |
| `draw:layout($diagram)` | a `draw:layout` record: `width`, `height`, `places` |
| `draw:ranks($diagram)` | a map from node id to rank |
| `draw:line($from, $to)` | the ends of a line between two places, or nothing |
| `draw:loop($place)` | the path of an edge from a node to itself |
| `draw:node($node, $place)`, `draw:edge($edge, $places)` | one group |

A diagram built in the query itself is drawn the same way. Here the loop
shows up, with its style rule:

```console
$ sxml xquery --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; draw:svg(<diagram title="Polling"><node id="flag" label="flag"/><edge from="flag" to="flag" label="status.request"/></diagram>)' --output=loop.svg
$ sxml xpath --context-item=loop.svg '//*:path[not(ancestor::*:defs)]/@d/string(), contains(//*:style, ".edge path")'
"M 40 60 C 40 84, 70 84, 70 60"
true()
```
