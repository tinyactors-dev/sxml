# Transforming SVG

<!-- examples: svg -->

A drawing is a document, so it can be changed after it is drawn, by
someone who has never seen the description it came from. This section
writes small stylesheets that take an SVG file, ours or one an editor
saved, and write a changed copy: a dark theme, one node highlighted,
the editor's leftovers removed, a different size. A query lists the
text. All of them read SVG, so all of them must name SVG elements in
the SVG namespace, and the section shows the ways to do that.

## Copying everything, changing a little

Each stylesheet here starts from the same line:

```xml
<xsl:mode on-no-match="shallow-copy"/>
```

It says what happens to a node no template rule matches: it is copied,
and its attributes and children are processed in turn. With only that
line, a stylesheet copies its input unchanged; every template rule added
to it is an exception. The rules below change a few nodes and leave the
rest of the drawing alone, however large it is.

## A dark theme

A dark theme swaps colours. `dark.xsl` holds a *palette*, a map from
each light colour of our drawings to a dark one, and applies it
wherever colours are written: in the style text, and in the colour
attributes `fill`, `stroke` and `stop-color`. It also gives the drawing a
dark background and makes the default colour light, because what has
no colour of its own is drawn black:

`dark.xsl`:

```xml
{{#include ../examples/svg/dark.xsl}}
```

`theme:dark` replaces every colour of the palette in a string, with a
`fold-left` over the palette's keys: the result of one replacement is
the input of the next. The flags `q` and `i` of `replace` make the
colour a literal string (not a regular expression) and ignore case, as
CSS does for colours. The function is in a namespace of our own: a
stylesheet function needs a prefix, and the SVG namespace is not ours to
add functions to.

Names in the patterns have a prefix, `svg:`, bound to the SVG namespace
on the stylesheet: `/svg:svg` is the root of an SVG drawing, and
`svg:style/text()` the text of its style. `exclude-result-prefixes`
keeps these declarations out of the result.

```console
$ sxml transform dark.xsl diagram.svg --output=dark.svg
$ sxml xpath --context-item=dark.svg '/*/@style/string(), //*:style/string()'
"background-color: #121212"
"
    .node rect { fill: #1f2a3d; stroke: #8fb0e8; stroke-width: 1.5 }
    .store rect { fill: #3a3020; stroke: #d9b878 }
    .node text { font: 14px sans-serif; fill: #e8e8e8 }
    .edge line { stroke: #b0b0b0; stroke-width: 1.5 }
    .edge text { font: 12px sans-serif; fill: #b0b0b0 }
  "
```

The palette works on any drawing that uses these colours. The rules for
`added`, `removed` and `changed` use colours that are not in it, and stay
as they are, readable on dark too:

```console
$ sxml xquery --context-item=change.xml draw.xq | sxml transform dark.xsl - | sxml xpath --context-item=- 'tokenize(//*:style, "\n")[contains(., "added") or contains(., "store")] ! normalize-space()'
".store rect { fill: #3a3020; stroke: #d9b878 }"
".added rect, .added line, .added path { stroke: #2e7d32; stroke-width: 3 }"
```

## Highlighting a node

To point at one module in a review, highlight it and the edges that
start or end at it, and fade the rest. Which node is a *stylesheet
parameter*, given on the command line:

`highlight.xsl`:

```xml
{{#include ../examples/svg/highlight.xsl}}
```

This stylesheet uses the other way of naming SVG elements:
`xpath-default-namespace` puts unprefixed names in patterns and paths in
the SVG namespace, so `g`, `style` and `//g` mean SVG elements.

The drawing has no description inside it, only ids, so the stylesheet
works from them. A node's group has the id `node-ID`, and an edge's
group `edge-FROM-TO`. The variable `$ids` collects the node ids, and
`$highlighted` the ids of the groups to highlight: the node's own, and
for every node M the two possible edges between it and M. Matching
whole ids, rather than looking for the node's id somewhere inside a
group's id, matters here: the backend SDK's node has the id `node`, and
`contains(@id, 'node')` would be true of every node's group.

A template rule whose pattern uses a variable is fine in XSLT: `g[@id =
$highlighted]` matches the groups to change, which get the class
`highlight`, and two style rules do the rest. In CSS, `:not(.highlight)`
selects the groups without the class.

```console
$ sxml transform --param=node=registry highlight.xsl diagram.svg --output=highlight.svg
$ sxml xpath --context-item=highlight.svg '//*:g[contains-token(@class, "highlight")]/@id/string()'
"edge-flags-registry"
"edge-registry-postgresql"
"node-registry"
```

`as="xs:string"` on the parameter converts the value given, and
`required="yes"` makes a missing one an error. A node the drawing
doesn't have, such as the audit module, which only the drawing after
the change has, is not an error, but worth a word, which `xsl:message`
writes to standard error while the transformation goes on:

```console
$ sxml transform highlight.xsl diagram.svg
highlight.xsl: err:XTDE0050: no value is supplied for a required stylesheet parameter (XSLT 4.0, 9.5)
$ sxml transform --param=node=audit highlight.xsl diagram.svg --output=nothing.svg
highlight.xsl: the drawing has no node "audit"
```

Without `xpath-default-namespace`, the stylesheet still runs, and does
nothing: `//g` finds no groups in no namespace, so even `registry` is
not found. This is the most common mistake with SVG and XSLT, and the
message is how you notice it:

```console
$ sed '/xpath-default-namespace/d' highlight.xsl > highlight-plain.xsl
$ sxml transform --param=node=registry highlight-plain.xsl diagram.svg | grep -c highlight
highlight.xsl: the drawing has no node "registry"
0
```

## Removing ids and metadata

Drawings made in an editor carry the editor with them. `legend.svg` is a
legend for our diagrams as an editor saved it, with a comment, its own
elements and attributes in its own namespaces, RDF metadata, and an id
on everything:

`legend.svg`:

```xml
{{#include ../examples/svg/legend.svg}}
```

`strip.xsl` removes all that. Removing a node is a template rule with
no content: the node matches, and nothing is written for it.

`strip.xsl`:

```xml
{{#include ../examples/svg/strip.xsl}}
```

Ids need care. The gradient `fade` is used by `fill="url(#fade)"`, and a
drawing that loses that id loses its gradient (in our drawings, the
arrow marker is `url(#arrow)`). So `$referenced` collects every `#name`
in the attributes and in the style text, and only the other ids go.

Three details are about namespaces and whitespace:

- Patterns test namespaces directly: `*[namespace-uri() ne '…']` is any
  element outside SVG, and `@*[namespace-uri()]` any attribute in a
  namespace (ordinary attributes are in none).
- A copied element takes the namespace declarations in scope with it,
  even unused ones; `copy-namespaces="no"` on `xsl:copy` leaves out the
  ones the copy doesn't need, so the `inkscape` and `rdf` declarations
  disappear along with what used them.
- The whitespace around a removed element stays behind as blank lines.
  `xsl:strip-space` drops whitespace-only text from the input (except in
  `text` and `style`, where it may matter), and `indent="yes"` lays the
  result out again.

The rule that drops elements is a union of three patterns. In XSLT 4.0 a
union has the lowest priority of its parts, which is `comment()`'s -0.5,
lower than the -0.25 of `svg:*`; the rule gives itself `priority="1"`, or
`svg:*` would copy `metadata` instead of dropping it.

```console
$ sxml transform strip.xsl legend.svg
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="85" viewBox="0 0 120 85" version="1.1">
  <defs>
    <linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#eef3fb"/>
    </linearGradient>
  </defs>
  <g>
    <rect x="10" y="10" width="100" height="30" rx="6" fill="url(#fade)" stroke="#3b5b92"/>
    <text x="60" y="30" text-anchor="middle" font-size="12">module</text>
    <rect x="10" y="48" width="100" height="30" rx="15" fill="#f6efe3" stroke="#8a6a2f"/>
    <text x="60" y="68" text-anchor="middle" font-size="12">store</text>
  </g>
</svg>
```

Transformations compose. The cleaned legend in the dark theme is two
stylesheets in a pipeline, and the gradient's light stops become dark:

```console
$ sxml transform strip.xsl legend.svg | sxml transform dark.xsl - | sxml xpath --context-item=- '//*:stop/@stop-color/string(), //*:rect/@fill/string()'
"#121212"
"#1f2a3d"
"url(#fade)"
"#3a3020"
```

## Listing the text

A query reads SVG as easily. `declare namespace` binds a prefix for the
query, the XQuery counterpart of `xmlns:svg` on a stylesheet:

`texts.xq`:

```xquery
{{#include ../examples/svg/texts.xq}}
```

```console
$ sxml xquery --context-item=diagram.svg texts.xq
HTTP
HTTP
SQL
Redis
@vane/browser
@vane/node
vane/api
vane/flags
vane/registry
vane/cache
postgresql
redis
$ sxml xquery --context-item=legend.svg texts.xq
module
store
```

The edge labels come first because the edges are drawn first. A list
like this is what a spelling checker or a translator needs, and what a
test can compare.

## Scaling with the viewBox

The `viewBox` of the root says which area of the drawing's own
coordinates is shown, and `width` and `height` say how large the result
is. Change the size and keep the `viewBox`, and the whole drawing scales,
lines and text alike:

`scale.xsl`:

```xml
{{#include ../examples/svg/scale.xsl}}
```

```console
$ sxml transform scale.xsl diagram.svg | head -1
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="960" viewBox="0 0 360 480">
$ sxml transform --param=factor=0.5 scale.xsl box.svg | head -1
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="70" viewBox="0 0 200 140">
```

Without a `viewBox`, a larger width would show more empty space, not a
larger drawing, so the rule for `/svg:svg[not(@viewBox)]` adds one made
from the old size before the sizes change. The pattern `/svg:svg/@width`
starts with `/`: it matches only the root's width, not the width of
every `rect`.

```console
$ sed 's/ viewBox="0 0 200 140"//' box.svg > plain.svg
$ sxml transform scale.xsl plain.svg | head -1
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 140" width="400" height="280">
```

## Namespaces in patterns, once more

Every stylesheet and query in this section names SVG elements, and they
do it in four ways:

| Way | Where | Example |
| --- | --- | --- |
| a prefix bound in the stylesheet or query | `dark.xsl`, `strip.xsl`, `scale.xsl`, `texts.xq` | `svg:style`, `//svg:text` |
| a default namespace for names in paths and patterns | `highlight.xsl`, `boxes.xsl` | `xpath-default-namespace="…"`, then `g` |
| any namespace | quick XPath at the command line | `//*:rect` |
| the namespace in the name | anywhere | `Q{http://www.w3.org/2000/svg}g` |

What doesn't work is an unprefixed name with no default: it means no
namespace, and an SVG drawing has no elements there.

With this, the diagram tool is complete: a description, a layout, two
ways of drawing it, and stylesheets that change any drawing. The state
machine, call-chain and trace projects reuse `draw.xqm` to draw what
they compute.
