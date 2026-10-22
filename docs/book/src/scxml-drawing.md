# Drawing the Machine

<!-- examples: scxml -->

A state machine is easiest to read as a picture: boxes for the states,
arrows for the transitions. [Project: Drawing Diagrams with SVG](svg.md)
built a module that draws any diagram description, `draw.xqm`, with a
computed layout. This section turns the machine into such a
description and draws it, and then draws what the fix of [Checking What
the Schema Cannot](scxml-checks.md) changed.

`draw.xqm` here is a link to the module in the `svg` directory, so the
machine is drawn by the same code as every other diagram of the book.

## The machine as a diagram

A diagram description is a `diagram` element with `node` and `edge`
elements, in no namespace. For the machine, each state is a node, and
each pair of states that transitions connect is an edge, labelled with
the events that lead from one to the other:

`machine-diagram.xq`:

```xquery
{{#include ../examples/scxml/machine-diagram.xq}}
```

- The initial state comes first, because the layout starts from the
  first node, and so draws it near the top.
- The transitions are grouped by their source and target, so that two
  transitions between the same states make one arrow with both events
  on it; there is one arrow per pair, and `draw.xqm` gives each the id
  `edge-FROM-TO`.
- A transition without a target stays in its state, so it is an edge
  from the state to itself, which `draw.xqm` draws as a small loop on
  the right of its box.
- The edges are records, `record(from, to, label)`, so that the edges of
  two machines can be compared by their source and target, below.

```console
$ sxml xquery --context-item=flag.scxml machine-diagram.xq
<diagram title="feature_flag">
  <node id="stable" label="stable"/>
  <node id="checking" label="checking"/>
  <node id="rolling-out" label="rolling-out"/>
  <node id="paused" label="paused"/>
  <node id="archived" label="archived"/>
  <edge from="stable" to="checking" label="rollout.request"/>
  <edge from="stable" to="stable" label="status.request"/>
  <edge from="checking" to="rolling-out" label="check.passed"/>
  <edge from="checking" to="stable" label="check.failed"/>
  <edge from="checking" to="paused" label="check.failed"/>
  <edge from="rolling-out" to="stable" label="kill.request, step.done"/>
  <edge from="rolling-out" to="rolling-out" label="flag.evaluated"/>
  <edge from="paused" to="stable" label="pause.expired"/>
  <edge from="archived" to="stable" label="kill.request"/>
</diagram>
```

## Cycles

A machine is full of cycles: `stable` to `checking` and back. The
layered layout of `draw.xqm` puts every node in a layer so that every
edge points down, so first it turns round the edges that close a
circle, found by a depth-first search that starts at the first node,
the initial state:

```console
$ sxml xquery --context-item=flag.scxml machine-diagram.xq \
  | sxml xquery --method=adaptive --context-item=- \
      --expression='import module namespace draw = "urn:example:draw" at "draw.xqm"; for $position in draw:reversed(/diagram) return /diagram/edge[$position] ! `{@from} -> {@to}`'
"checking -> stable"
"rolling-out -> stable"
"paused -> stable"
```

`--context-item=-` reads the diagram from the pipe. The three edges
from below back to `stable` are the ones turned round: drawn, they
curve up into `stable`. The edge from `archived` to `stable` closes no
circle, and stays as it is. No transition leads to `archived`,
so nothing pulls it down, and the layout puts it in the layer above
`stable`, the one state it leads to. The flaw that `check.xq` reported
as "unreachable state: archived" is visible in the drawing: the state
sits on top, above the initial state, with an arrow that leaves it and
none that arrive.

## Drawing it

`draw:svg` makes the SVG element; an output declaration indents it:

```console
$ sxml xquery --context-item=flag.scxml machine-diagram.xq > flag-diagram.xml
$ sxml xquery --context-item=flag-diagram.xml --output=flag.svg --expression='
    import module namespace draw = "urn:example:draw" at "draw.xqm";
    declare option output:indent "yes";
    draw:svg(/diagram)'
$ grep -A3 'id="node-' flag.svg
  <g class="node service" id="node-stable">
    <rect x="182" y="124" width="100" height="40" rx="6"/>
    <text x="232" y="149" text-anchor="middle">stable</text>
  </g>
  <g class="node service" id="node-checking">
    <rect x="79" y="228" width="100" height="40" rx="6"/>
    <text x="129" y="253" text-anchor="middle">checking</text>
  </g>
  <g class="node service" id="node-rolling-out">
    <rect x="46" y="332" width="110" height="40" rx="6"/>
    <text x="101" y="357" text-anchor="middle">rolling-out</text>
  </g>
  <g class="node service" id="node-paused">
    <rect x="312" y="332" width="100" height="40" rx="6"/>
    <text x="362" y="357" text-anchor="middle">paused</text>
  </g>
  <g class="node service" id="node-archived">
    <rect x="182" y="20" width="100" height="40" rx="6"/>
    <text x="232" y="45" text-anchor="middle">archived</text>
  </g>
```

`archived` is in the top layer and `stable` in the next; `checking` is
below `stable`, and `rolling-out` and `paused` share the bottom layer.
The layers between them hold the labels of the transitions, each beside
its arrow. The transitions that stay are loops:

```console
$ grep -A3 'id="edge-stable-stable"' flag.svg
  <g class="edge" id="edge-stable-stable">
    <path d="M 282 134 C 304 126, 304 162, 282 154" marker-end="url(#arrow)"/>
    <text x="308" y="148">status.request</text>
  </g>
```

![The flag's state machine in layers: archived on top, stable below it, checking, then rolling-out and paused; each transition is labelled beside its arrow, the transitions back to stable curve up into it, and stable and rolling-out have loops on their right](figures/scxml/flag.svg)

## Drawing a change

`machine-diagram.xq` takes an optional parameter, `before`: the file of
an earlier version of the machine. States and edges that the earlier
version doesn't have get the class `added`, and the ones only the
earlier version has get `removed`. `draw.xqm` copies the classes to the
SVG groups, and its style draws added parts in green and removed parts
dashed in red. The fixed machine against the seed:

```console
$ sxml xquery --context-item=flag-fixed.scxml --param=before=flag.scxml machine-diagram.xq \
  | sxml xquery --context-item=- --output=changes.svg --expression='
      import module namespace draw = "urn:example:draw" at "draw.xqm";
      declare option output:indent "yes";
      draw:svg(/diagram)'
$ sxml xpath --context-item=changes.svg '//*:g[contains-token(@class, "added")] ! `{@id}: {*:text}`'
"edge-stable-archived: flag.archived"
"edge-paused-archived: flag.archived"
```

![The fixed machine against the seed: the two new flag.archived transitions in green, from stable and from paused down to archived, now at the bottom](figures/scxml/changes.svg)

The two new transitions on `flag.archived` are the change, and they
move `archived` to the bottom: something now leads to it, from
`stable` and from `paused`, so it goes in the layer below them, and
its own transition to `stable` curves up. The new declaration of `flag.evaluated` and the new
`vane:implements` change no state or transition, so the drawing doesn't
show them; the page of [Documenting the Machine in
XHTML](scxml-xhtml.md) does.

The same mechanism, a diagram description with classes drawn by one
module, draws the diff of call chains in [Comparing Two
Versions](call-chains-diff.md) and the diff of traces in [Rendering the
Diff](traces-rendering.md).
