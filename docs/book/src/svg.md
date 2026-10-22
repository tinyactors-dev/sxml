# Project: Drawing Diagrams with SVG

<!-- examples: svg -->

Many teams draw their architecture with a text description and a tool
that turns it into a picture: Graphviz reads `a -> b`, Mermaid reads
`graph TD`. The description is small, it can be reviewed in a diff, and
nobody moves boxes by hand. In this project we'll build a small tool of
that kind from the parts of this book. The description is XML, the
layout is computed by XQuery, and the picture is SVG, which is XML too,
so the same tools can write it, check it and change it afterwards.

The diagram we'll draw is the Vane system and who calls whom in it: the
two SDKs, the modules of the Vane service, and the stores the service
keeps its data in. Later projects draw other things with the same code:
the flag's state machine, a diff of call chains, and a diff of traces.

The files are in `docs/book/examples/svg`.

## SVG in one page

SVG (Scalable Vector Graphics) is an XML vocabulary for drawings. Its
elements are in the namespace `http://www.w3.org/2000/svg`; without that
namespace, a browser shows an SVG file as unknown XML instead of a
picture. Here is one box with a label and an arrow:

`box.svg`:

```xml
{{#include ../examples/svg/box.svg}}
```

![box.svg: a box labelled vane/flags with an arrow pointing down from it](figures/svg/box.svg)

The root `svg` element gives the size of the picture (`width`, `height`)
and its coordinate system (`viewBox`: the left, top, width and height of
the area drawn). Coordinates grow to the right and *down*: (0, 0) is the
top left corner. The other elements draw in document order, later ones
on top of earlier ones:

- `rect` is a box at `x`, `y` of size `width` by `height`; `rx` rounds its
  corners.
- `text` writes text with its baseline at `y`; `text-anchor="middle"`
  centres it on `x`.
- `line` goes from (`x1`, `y1`) to (`x2`, `y2`). `marker-end` puts a
  marker on its end: here the triangle that `marker` defines inside
  `defs`, which holds things that are used rather than drawn, and is
  referred to by its `id` as `url(#arrow)`.
- `style` holds CSS rules, as in HTML. Presentation can also be written
  as attributes (`stroke="black"`); a rule in `style` applies to every
  element it matches.
- `g` (not used here) groups elements; a class or an id on the group
  applies to everything inside.

Open the file in a browser to see it. For the tools in this book it is
just a document:

```console
$ sxml check box.svg
$ sxml xpath --context-item=box.svg '/*/* ! local-name()'
"style"
"defs"
"rect"
"text"
"line"
```

## The description and the drawing

A diagram is described by a small vocabulary of our own, without a
namespace: a `diagram` with a `title`, a `node` for each box (an `id`,
a `label`, and `kind="store"` for a data store), and an `edge` for each
arrow, from the caller to what it calls:

`diagram.xml`:

```xml
{{#include ../examples/svg/diagram.xml}}
```

The Vane service is one program, so its parts are modules, not
services; each module is a box of its own, labelled `vane/MODULE`. The
prefix says that the boxes are parts of one program. A box drawn around
them would say it too, but it would need a second kind of node and a
layout that nests boxes inside boxes, new machinery for one picture; the
labels do the job with what the vocabulary already has. The two SDKs
call the service over HTTP, the modules call each other directly (those
edges have no label), and two modules talk to the stores.

Nothing in the description says where a box goes. That is the tool's
job, and its result is this drawing:

`diagram.svg`:

```xml
{{#include ../examples/svg/diagram.svg}}
```

![The Vane system: the two SDKs call vane/api, which calls vane/flags; flags calls vane/registry and vane/cache, which keep their data in PostgreSQL and Redis](figures/svg/diagram.svg)

The boxes are in five layers: the two SDKs at the top, the `api` module
they call below them, and so on down to PostgreSQL and Redis. Each node
is a group `g` with a `rect` and a `text`; each edge is a group with a
`path`, a curve from box to box, and, when the edge has one, its label
beside it. The groups
have ids made from the description (`node-registry`, `edge-flags-cache`)
and classes (`node`, `service`, `store`, `edge`) that the five style
rules use: `service` is the class of every box that is not a store, an
SDK or a module alike. Stores have rounder corners.

Both files are ordinary XML:

```console
$ sxml check diagram.xml diagram.svg
$ sxml xpath --context-item=diagram.xml 'count(//node), count(//edge), //node[@kind = "store"]/@label/string()'
8
7
"postgresql"
"redis"
```

An XPath expression over the drawing has to say the namespace. A name
test without a prefix, such as `//rect`, looks for elements in no
namespace and finds none. `*:rect` matches `rect` in any namespace, and
`Q{http://www.w3.org/2000/svg}g` names the namespace in the expression
itself:

```console
$ sxml xpath --context-item=diagram.svg 'count(//rect), count(//*:rect)'
0
8
$ sxml xpath --context-item=diagram.svg '//Q{http://www.w3.org/2000/svg}g/@id/string()'
"edge-browser-api"
"edge-node-api"
"edge-api-flags"
"edge-flags-registry"
"edge-flags-cache"
"edge-registry-postgresql"
"edge-cache-redis"
"node-browser"
"node-node"
"node-api"
"node-flags"
"node-registry"
"node-cache"
"node-postgresql"
"node-redis"
```

## The plan

The tool has two halves, and the next sections build them one at a
time:

- [Computing a Layout](svg-layout.md) decides where each box goes and
  how each edge runs, with the layered method that Graphviz and the
  Eclipse Layout Kernel use: layers so that edges point down, an order
  in each layer so that few edges cross, then coordinates and curves.
  It is written with maps, arrays, records and functions as values.
- [Writing SVG with XQuery](svg-xquery.md) turns the layout into
  `diagram.svg` with constructors in the SVG namespace, and packs both
  halves into a library module, `draw.xqm`, that the later projects
  import.
- [Drawing with XSLT](svg-xslt.md) draws the same diagram with template
  rules, from a description the layout query has given coordinates.
- [Transforming SVG](svg-transform.md) changes drawings that already
  exist, ours or anyone's: a dark theme, one node highlighted, ids and
  editor metadata removed, the text listed, the size changed.
