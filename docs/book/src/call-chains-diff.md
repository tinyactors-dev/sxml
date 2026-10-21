# Comparing Two Versions

<!-- examples: call-chains -->

The call graphs of [A Call Graph from the Code](call-chains-graph.md)
say which function calls which. A change is easier to judge by its
*call chains*: the paths through the graph from where the service is
entered to the functions at the end, each one a thing that happens when
a request comes in. This section lists the chains of both versions,
compares the graphs and the chains, and writes what changed as a text
report, an HTML page and a drawing.

## Entries and next calls

A chain starts at an *entry*, a function that no call of the program
reaches, and the graph's queries found one: `server.start`. A chain is
a sequence of `call` elements, each going on from where the one before
it ended. What can follow a chain are the calls of its last function to
functions of the program; external calls such as `fetch` end a chain
and are not part of it:

```xquery
{{#include ../examples/call-chains/graph.xqm:next}}
```

The test `not(@to = $chain/@from)` keeps a chain from going through a
function twice. The lock service has no cycles, but a program with a
function that calls itself, or two that call each other, would
otherwise have chains without end.

## Chains with a worklist

The chains can be made without a function that calls itself: keep a
list of the chains that are not finished, the *worklist*, and take one
chain off it at a time. A chain that cannot go on is finished; any
other is put back once for each next call, one call longer. When the
worklist is empty, every chain is finished:

```xquery
{{#include ../examples/call-chains/graph.xqm:chains}}
```

The state of the loop is a map with the worklist, an array of chains,
and the finished chains. `fn:while-do` takes a start value, a condition
and an action: it applies the action as long as the condition holds and
returns the last value. Putting the longer chains at the front of the
worklist goes deep first, so the chains come out in the order of the
calls in the code.

`graph:chain-text` writes a chain as the functions it goes through,
with a star after a function called in a loop:

```xquery
{{#include ../examples/call-chains/graph.xqm:chain-text}}
```

`chains.xq` prints the chains of a directory:

```xquery
{{#include ../examples/call-chains/chains.xq}}
```

```console
$ sxml xquery --allow-file-access --param=directory=before chains.xq
server.start > server.route > doors.listDoors > registry.getDoors > http.request
server.start > server.route > doors.listDoors > doors.summarize
server.start > server.route > doors.unlockDoor > cache.getDoor
server.start > server.route > doors.unlockDoor > registry.getDoor > http.request
server.start > server.route > doors.unlockDoor > doors.codeMatches
server.start > server.route > doors.unlockDoor > registry.setState > http.request
$ sxml xquery --allow-file-access --param=directory=after chains.xq
server.start > server.route > doors.listDoors > registry.getDoorIds > http.request
server.start > server.route > doors.listDoors > registry.getDoor* > http.request
server.start > server.route > doors.listDoors > doors.summarize
server.start > server.route > doors.unlockDoor > registry.getDoor > http.request
server.start > server.route > doors.unlockDoor > doors.codeMatches
server.start > server.route > doors.unlockDoor > registry.setState > http.request
server.start > server.route > doors.unlockDoor > audit.record > http.request
```

Six chains before, seven after. Every chain that reaches the network
ends in `http.request`, and `cache.getDoor`, which calls Redis through a
client of its own, was a chain of its own.

## Chains with recursion

The same chains come from a function that calls itself, and it is
shorter: the chains that go on from a chain are, for each next call,
the chains that go on from the longer chain, and a chain that cannot go
on is one chain:

```xquery
{{#include ../examples/call-chains/graph.xqm:recursive}}
```

Both are correct, and give the same chains in the same order:

```console
$ sxml xquery --allow-file-access --param=directory=after chains.xq > worklist.txt
$ sxml xquery --allow-file-access --param=directory=after --param=method=recursive chains.xq > recursive.txt
$ cmp worklist.txt recursive.txt && echo same
same
```

The recursion is the definition of a chain written down. The worklist
says how the chains are made, step by step, and holds its state in a
value, so it can be stopped, counted or limited without changing the
definition: a maximum chain length is one more condition in the
`while-do`. For a graph with thousands of functions, the worklist also
keeps the depth of nested function calls flat.

## The diff

`diff.xq` compares two versions. It makes one document with every
function, call and chain of either version, each marked with an
attribute `change`: `added` when only the new version has it, `removed`
when only the old one has it, `changed` when both have it and it
differs, and no attribute when it is the same.

A call of the graph is a call site, with a line. Lines move when code
is added above them, so the diff compares calls by caller and callee
only, one for each pair, repeated when any of its call sites is in a
loop:

```xquery
{{#include ../examples/call-chains/diff.xq:calls}}
```

A chain is compared by the functions it goes through, without the
stars, so that a chain whose call became repeated is `changed`:

```xquery
{{#include ../examples/call-chains/diff.xq:chains}}
```

One function compares two maps of either kind. It goes through the keys
of both, in order, and copies the item of the new version, or of the
old one when the new one does not have it, with the change as an
attribute. `deep-equal` on two elements compares their names,
attributes and content:

```xquery
{{#include ../examples/call-chains/diff.xq:compare}}
```

The diff of the functions is the same function on two maps from id to
an empty `function` element. The main expression puts the three
together:

```xquery
{{#include ../examples/call-chains/diff.xq:main}}
```

`local:modules` is the last part, below. The two versions are external
variables, `before` and `after` unless they are given:

```console
$ sxml xquery --allow-file-access diff.xq --output=diff.xml
$ sxml xpath --context-item=diff.xml 'for $kind in ("function", "call", "chain") return `{$kind}s: {count(/diff/*[local-name() = $kind][@change = "added"])} added, {count(/diff/*[local-name() = $kind][@change = "removed"])} removed`'
"functions: 2 added, 2 removed"
"calls: 6 added, 5 removed"
"chains: 3 added, 2 removed"
```

The calls that changed:

```console
$ sxml xpath --context-item=diff.xml '/diff/call[@change] ! `{@change}: {@from} > {@to}{if (@repeated) then " (in a loop)" else ""}`'
"added: audit.record > JSON.stringify"
"added: audit.record > http.request"
"removed: cache.getDoor > JSON.parse"
"removed: cache.getDoor > client.get"
"added: doors.listDoors > registry.getDoor (in a loop)"
"added: doors.listDoors > registry.getDoorIds"
"removed: doors.listDoors > registry.getDoors"
"added: doors.unlockDoor > audit.record"
"removed: doors.unlockDoor > cache.getDoor"
"added: registry.getDoorIds > http.request"
"removed: registry.getDoors > http.request"
```

`doors.listDoors > registry.getDoor` is `added`, not `changed`: before
the change, `listDoors` did not call `getDoor` at all.

## How often a module is called

The question a reviewer asks of this change is how much more work a
request makes, and that is a question about modules: `registry.ts`
talks to the lock registry, `audit.ts` to the audit service, `cache.ts`
to Redis. For each function of both versions and each other module it
calls, `local:modules` counts the calls before and after. A call in a
loop counts as *n*, once per iteration:

```xquery
{{#include ../examples/call-chains/diff.xq:modules}}
```

The `group by` of the calls in `local:calls` is gone here: two calls to
the same function are two calls. `$module-of` and `$calls` are inline
functions bound with `let`, used as helpers inside one function. The
result is the end of `diff.xml`:

```console
$ sxml xpath --context-item=diff.xml '/diff/module ! `{@from} > {@name}: {@old} -> {@new}`'
"doors.listDoors > registry: 1 -> 1 + n"
"doors.unlockDoor > audit: 0 -> 1"
"doors.unlockDoor > cache: 1 -> 0"
```

That is the prediction: listing the doors calls the registry once more
for each door, unlocking a door calls the audit service once and the
cache no more.

## A text report

`report.xq` writes the diff for a terminal or a review comment. It
reads `diff.xml` as its context item and only formats:

```xquery
{{#include ../examples/call-chains/report.xq}}
```

```console
$ sxml xquery --context-item=diff.xml report.xq
functions added:
  audit.record
  registry.getDoorIds

functions removed:
  cache.getDoor
  registry.getDoors

calls added:
  audit.record > JSON.stringify
  audit.record > http.request
  doors.listDoors > registry.getDoor (in a loop)
  doors.listDoors > registry.getDoorIds
  doors.unlockDoor > audit.record
  registry.getDoorIds > http.request

calls removed:
  cache.getDoor > JSON.parse
  cache.getDoor > client.get
  doors.listDoors > registry.getDoors
  doors.unlockDoor > cache.getDoor
  registry.getDoors > http.request

chains added:
  server.start > server.route > doors.listDoors > registry.getDoor* > http.request
  server.start > server.route > doors.listDoors > registry.getDoorIds > http.request
  server.start > server.route > doors.unlockDoor > audit.record > http.request

chains removed:
  server.start > server.route > doors.listDoors > registry.getDoors > http.request
  server.start > server.route > doors.unlockDoor > cache.getDoor

Calls to other modules, before and after:
  doors.listDoors > registry: 1 -> 1 + n
  doors.unlockDoor > audit: 0 -> 1
  doors.unlockDoor > cache: 1 -> 0
```

## An HTML report

`report.xsl` writes the same diff as a page: the calls that changed in
a table, every chain with the changed ones marked, and the calls to
other modules. A template rule for each kind of element, and the
classes `added`, `removed` and `changed` for the style sheet to colour:

```xml
{{#include ../examples/call-chains/report.xsl}}
```

With `expand-text="yes"`, braces in text are text value templates, so
the braces of the CSS rules are doubled. The rule for `chain[@change]`
wins over the rule for `chain` because its pattern has a predicate,
which gives it a higher default priority.

```console
$ sxml transform report.xsl diff.xml --output=report.html
$ sed -n '/Calls to other modules/,/<\/table>/p' report.html
    <h2>Calls to other modules</h2>
    <table>
      <tr>
        <th>Function</th>
        <th>Module</th>
        <th>before</th>
        <th>after</th>
      </tr>
      <tr>
        <td><code>doors.listDoors</code></td>
        <td><code>registry</code></td>
        <td>1</td>
        <td>1 + n</td>
      </tr>
      <tr>
        <td><code>doors.unlockDoor</code></td>
        <td><code>audit</code></td>
        <td>0</td>
        <td>1</td>
      </tr>
      <tr>
        <td><code>doors.unlockDoor</code></td>
        <td><code>cache</code></td>
        <td>1</td>
        <td>0</td>
      </tr>
    </table>
```

## Drawing the diff

The drawing module of [Project: Drawing Diagrams with SVG](svg.md),
`draw.xqm`, draws any diagram description: `node` and `edge` elements,
each with an optional `class` that the drawing copies to its SVG group
and colours for `added`, `removed` and `changed`. This project's
directory has it, and the SVG project's main query `draw.xq`, as
symbolic links to the files in `../svg`, so the module stays one file.

So drawing the diff is writing it as a diagram. `diagram.xq` makes a
node for each function and an edge for each call within the program,
with the change as the class and the label `loop` on a repeated call:

```xquery
{{#include ../examples/call-chains/diagram.xq}}
```

`draw.xqm` puts the first node in the top row, so the functions are
ordered as the chains first go through them: `index-of` gives each
function its position in the distinct functions of all chains, and the
entry, `server.start`, comes first.

```console
$ sxml xquery --context-item=diff.xml diagram.xq --output=diagram.xml
$ sxml xquery --context-item=diagram.xml draw.xq --output=calls.svg
$ sxml xpath --context-item=calls.svg '//*:g[tokenize(@class) = ("added", "removed", "changed")] ! `{@class}: {@id}`'
"edge added: edge-audit.record-http.request"
"edge added: edge-doors.listDoors-registry.getDoor"
"edge added: edge-doors.listDoors-registry.getDoorIds"
"edge removed: edge-doors.listDoors-registry.getDoors"
"edge added: edge-doors.unlockDoor-audit.record"
"edge removed: edge-doors.unlockDoor-cache.getDoor"
"edge added: edge-registry.getDoorIds-http.request"
"edge removed: edge-registry.getDoors-http.request"
"node service added: node-registry.getDoorIds"
"node service removed: node-registry.getDoors"
"node service added: node-audit.record"
"node service removed: node-cache.getDoor"
```

The added functions and calls are drawn green and thick, the removed
ones red and dashed, and the call in the loop is labelled. The rows of
the drawing are the boxes with the same `y`:

```console
$ sxml xpath --context-item=calls.svg 'for $y in distinct-values(//*:rect/@y) return `{$y}: {string-join(//*:g[*:rect/@y = $y]/*:text, ", ")}`'
"20: server.start"
"120: server.route"
"220: doors.listDoors, doors.unlockDoor"
"320: doors.summarize, registry.getDoor, registry.getDoorIds, registry.getDoors, audit.record, cache.getDoor, doors.codeMatches, registry.setState"
"420: http.request"
```

The entry, the router, the two request handlers, the functions they
call, and the HTTP client at the bottom, where every call to another
service goes through. Before and after are one picture: the old
`getDoors` and the new `getDoorIds` and `getDoor` side by side in the
fourth row.

## What the traces will say

The diff predicts, from the code alone, that `GET /doors` makes one
call to the lock registry and then one per door, that unlocking a door
calls the audit service once, and that it no longer reads the cache.
It cannot say how many doors there are, how long each call takes, or
whether the audit service calls a database of its own. [Project:
Comparing Traces Before and After a Change](traces.md) answers those
from traces recorded by the running service, and with four doors, one
call to the registry becomes five.
