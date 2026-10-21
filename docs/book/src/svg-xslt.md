# Drawing with XSLT

<!-- examples: svg -->

XQuery computed the layout and wrote the drawing in one go. The drawing
half is also a natural job for XSLT: each kind of element in the input
becomes a piece of SVG, which is exactly what template rules say. In
this section a stylesheet draws the diagram from the layout that
`layout.xq` writes, and it draws the edge labels last, on top of
everything, which the XQuery drawing doesn't do.

## The input: a laid-out diagram

The stylesheet does no arithmetic of its own beyond the label positions.
It reads the description with coordinates added, which
[Computing a Layout](svg-layout.md) made with `layout.xq`:

```console
$ sxml xquery --context-item=diagram.xml --output=laid-out.xml layout.xq
$ sxml xpath --context-item=laid-out.xml '/diagram/@width/string(), //node[@id = "redis"]/@*!string()'
"360"
"redis"
"redis"
"store"
"200"
"420"
```

## The stylesheet

`draw.xsl`:

```xml
{{#include ../examples/svg/draw.xsl}}
```

Read it from the top.

- `xmlns="http://www.w3.org/2000/svg"` on the stylesheet makes it the
  default namespace of the *literal result elements*: `svg`, `g`, `rect`
  and the others are written in the SVG namespace. It does not change
  what the patterns and paths mean: `match="node"` and `select="edge"`
  still name elements in no namespace, as in the input. (XSLT keeps the
  two apart; XQuery's `declare default element namespace` changes both.)
- `expand-text="yes"` turns on *text value templates*: curly braces in
  text hold expressions, as `{@title}` and `{@label}` do. Braces in
  attributes are *attribute value templates*, which XSLT always has. In
  the style rules a brace that is CSS is doubled, the same rule as in
  XQuery. The text of `style` is copied as it is written, indentation
  included; CSS doesn't mind.
- `xsl:output indent="yes"` asks for one element per line.

## One mode per layer

The template for `/diagram` writes the frame (title, style, the arrow
marker) and three groups, and it fills each group by applying templates
in a different *mode*: `edges`, `nodes` and `labels`. A mode is a
separate set of template rules, so the same `edge` elements are
processed twice, once to draw their lines and once to write their labels,
by different rules. The order of the groups is the order of drawing:
lines first, then the boxes over the ends of the lines, then the labels
over both, where no box can hide them.

Within a mode the most specific rule wins. `edge[@x1]` (an edge with a
line) has a predicate, which gives it the default priority 0.5, and
`edge` alone has priority 0; so lines are drawn by the first rule, and an
edge without `x1`, a loop, falls to the second, which draws its path.
In the `labels` mode the loop's label is placed next to its node, which
the key `node` finds by id.

## `xsl:next-match` for stores

A store is a node with rounder corners. Instead of a second copy of the
node rule, the store rule hands the work back: `xsl:next-match` applies
the next rule that matches, here the general `node` rule, and
`xsl:with-param` gives it the corner radius. The general rule declares
the parameter with its default, 6:

```console
$ sxml transform draw.xsl laid-out.xml --output=drawn.svg
$ sxml xpath --context-item=drawn.svg '//*:g[@id = ("node-cache", "node-redis")] ! `{@id}: {@class}, rx {*:rect/@rx}`'
"node-cache: service, rx 6"
"node-redis: store, rx 20"
```

The cache module is a box like the other modules; Redis, where it keeps
its data, is the store.

The class is an attribute value template with a sequence in it,
`{(@kind, 'service')[1], @class}`: the values are joined with spaces, and
a missing `class` adds nothing.

## The result

```console
$ sxml transform draw.xsl laid-out.xml
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="360" height="480" viewBox="0 0 360 480">
  <title>Vane</title>
  <style>
        .nodes rect { fill: #eef3fb; stroke: #3b5b92; stroke-width: 1.5 }
        .nodes .store rect { fill: #f6efe3; stroke: #8a6a2f }
        .nodes text { font: 14px sans-serif; fill: #1b1b1b }
        .edges line, .edges path { fill: none; stroke: #555555; stroke-width: 1.5 }
        .labels text { font: 12px sans-serif; fill: #555555 }
      </style>
  <defs>
    <marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5" markerWidth="8" markerHeight="8" orient="auto">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="#555555"/>
    </marker>
  </defs>
  <g class="edges">
    <line id="edge-browser-api" x1="90" y1="60" x2="180" y2="120" marker-end="url(#arrow)"/>
    <line id="edge-node-api" x1="270" y1="60" x2="180" y2="120" marker-end="url(#arrow)"/>
    <line id="edge-api-flags" x1="180" y1="160" x2="180" y2="220" marker-end="url(#arrow)"/>
    <line id="edge-flags-registry" x1="180" y1="260" x2="90" y2="320" marker-end="url(#arrow)"/>
    <line id="edge-flags-cache" x1="180" y1="260" x2="270" y2="320" marker-end="url(#arrow)"/>
    <line id="edge-registry-postgresql" x1="90" y1="360" x2="90" y2="420" marker-end="url(#arrow)"/>
    <line id="edge-cache-redis" x1="270" y1="360" x2="270" y2="420" marker-end="url(#arrow)"/>
  </g>
  <g class="nodes">
    <g id="node-browser" class="service">
      <rect x="20" y="20" width="140" height="40" rx="6"/>
      <text x="90" y="45" text-anchor="middle">@vane/browser</text>
    </g>
    <g id="node-node" class="service">
      <rect x="200" y="20" width="140" height="40" rx="6"/>
      <text x="270" y="45" text-anchor="middle">@vane/node</text>
    </g>
    <g id="node-api" class="service">
      <rect x="110" y="120" width="140" height="40" rx="6"/>
      <text x="180" y="145" text-anchor="middle">vane/api</text>
    </g>
    <g id="node-flags" class="service">
      <rect x="110" y="220" width="140" height="40" rx="6"/>
      <text x="180" y="245" text-anchor="middle">vane/flags</text>
    </g>
    <g id="node-registry" class="service">
      <rect x="20" y="320" width="140" height="40" rx="6"/>
      <text x="90" y="345" text-anchor="middle">vane/registry</text>
    </g>
    <g id="node-cache" class="service">
      <rect x="200" y="320" width="140" height="40" rx="6"/>
      <text x="270" y="345" text-anchor="middle">vane/cache</text>
    </g>
    <g id="node-postgresql" class="store">
      <rect x="20" y="420" width="140" height="40" rx="20"/>
      <text x="90" y="445" text-anchor="middle">postgresql</text>
    </g>
    <g id="node-redis" class="store">
      <rect x="200" y="420" width="140" height="40" rx="20"/>
      <text x="270" y="445" text-anchor="middle">redis</text>
    </g>
  </g>
  <g class="labels">
    <text x="141" y="94">HTTP</text>
    <text x="231" y="94">HTTP</text>
    <text x="96" y="394">SQL</text>
    <text x="276" y="394">Redis</text>
  </g>
</svg>
```

The labels layer has four texts, not seven: the stylesheet applies the
`labels` mode only to `edge[@label]`, and the three edges between
modules have no label. The boxes, lines and labels are where the XQuery
drawing has them; only
the grouping and the style differ. Arithmetic on attributes works because
an attribute of an untyped document is untyped: `@x + 70` converts it to
a number. The two steps also run as one pipeline, the layout's output
read by the stylesheet from standard input (`-`):

```console
$ sxml xquery --context-item=states.xml layout.xq | sxml transform draw.xsl - --output=states.svg
$ sxml xpath --context-item=states.svg '//*:g[@class = "labels"]/*:text ! `{.} at {@x},{@y}`'
"rollout.request at 141,94"
"check.passed at 186,194"
"check.failed at 171,94"
"step.done at 171,144"
"kill.request at 186,44"
```

## A rule for input that is wrong

Given the description itself, without the layout, the rules above would
write a drawing with empty sizes and no places, and say nothing. One
more rule catches that: `/diagram[not(@width)]` matches a diagram
without a layout, and `xsl:message terminate="yes"` stops the
transformation with the message. Both patterns, `/diagram` and
`/diagram[not(@width)]`, have the default priority 0.5, so the rule
gives itself `priority="1"` to win:

```console
$ sxml transform draw.xsl diagram.xml
Vane: no layout; run layout.xq first
draw.xsl:43:5: err:XTMM9000: Vane: no layout; run layout.xq first
```

## Matching SVG

So far the stylesheet *wrote* SVG and *read* elements in no namespace.
A stylesheet that reads SVG must match SVG names, and the attribute
`xpath-default-namespace` says which namespace unprefixed names in
patterns and paths are in. This one lists the boxes of any drawing:

`boxes.xsl`:

```xml
{{#include ../examples/svg/boxes.xsl}}
```

Run on both drawings, it shows that they agree:

```console
$ sxml transform boxes.xsl diagram.svg
20,20 @vane/browser
200,20 @vane/node
110,120 vane/api
110,220 vane/flags
20,320 vane/registry
200,320 vane/cache
20,420 postgresql
200,420 redis
$ sxml transform boxes.xsl drawn.svg
20,20 @vane/browser
200,20 @vane/node
110,120 vane/api
110,220 vane/flags
20,320 vane/registry
200,320 vane/cache
20,420 postgresql
200,420 redis
```

Without `xpath-default-namespace`, `//rect` would look for `rect` in no
namespace, find nothing, and the output would be empty. The next
section is all about stylesheets like this one, which read drawings and
write changed ones.
