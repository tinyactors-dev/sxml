# A Call Graph from the Code

<!-- examples: call-chains -->

A call graph has a node for each function of a program and an edge
from a function to each function it calls. To build one from the trees
that `sxml:parse-code` makes, we need three things: the functions, the
calls inside each function, and for each call, the function it calls.
The first two are paths in the tree. The third is the interesting part:
the code says `registry.getDoor(id)`, and the graph needs to say
`registry.getDoor` of the module `registry.ts`, not a method of
something named `registry`.

This section builds the library module `graph.xqm` (namespace
`urn:example:graph`) one function at a time, and ends with the call
graph of both versions of the lock service as XML.

## Functions

In TypeScript's grammar a function declared with `function` is a
`function_declaration`, and its name is the child whose field is
`name`. With the option `positions`, each element knows its line:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/doors.ts"), "typescript", { "positions": true() })//function_declaration ! `{@line}: {identifier}`'
"12: summarize"
"16: listDoors"
"25: codeMatches"
"29: unlockDoor"
```

The exported functions are inside an `export_statement`, the others
are not; `//` finds both. A program has functions of other kinds too:
methods of classes are `method_definition`s, and a function is often
an arrow function assigned to a const,
`const openAll = (locks) => …`, which is an `arrow_function` inside a
`variable_declarator`. An arrow function passed as an argument, as in
`locks.map((lock) => lock.open(code))`, has no name; its calls are
calls of the function around it. `graph.xqm` says this in two
functions:

```xquery
{{#include ../examples/call-chains/graph.xqm:functions}}
```

`graph:function-of` is the nearest function above any node: the
`ancestor` axis goes up from the node, and the predicate `[1]` on a
reverse axis is the nearest one. The functions are named after their
module, which is the file name without `.ts`: `doors.listDoors`,
`registry.getDoor`, and a method after its module and class,
`lock.Lock.open`.

## Calls

A call is a `call_expression`, and what it calls is its child with the
field `function`. Its text is the callee as the code writes it:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/doors.ts"), "typescript", { "positions": true() })//call_expression ! `{@line}: {*[@field = "function"]}`'
"17: registry.getDoorIds"
"20: doors.push"
"20: registry.getDoor"
"22: doors.map"
"26: door.codes.includes"
"30: registry.getDoor"
"31: codeMatches"
"34: registry.setState"
"35: audit.record"
```

Line 20 has two calls, one inside the other: `doors.push(await
registry.getDoor(id))`. The callee is either an `identifier`
(`codeMatches`) or a `member_expression`, whose `object` and
`property` fields are the two sides of the dot:

```console
$ sxml xpath --method=xml '(sxml:parse-code(unparsed-text("after/doors.ts"), "typescript", { "highlights": false() })//call_expression)[1]'
<call_expression xmlns:sxml="urn:sxml"><member_expression field="function"><identifier field="object">registry</identifier>.<property_identifier field="property">getDoorIds</property_identifier></member_expression><arguments field="arguments">()</arguments></call_expression>
```

The list shows the three kinds of calls a call graph has to tell
apart. `registry.getDoor` and `audit.record` call functions of other
modules of the program, and `codeMatches` a function of the same
module. `doors.push`, `doors.map` and `door.codes.includes` call
methods of local values, an array and a string array, which are not
part of the program's structure. And other files have calls of
functions outside the program: `fetch`, `JSON.parse`, `createServer`.

## Resolving a callee

What a name stands for in a module comes from the module's imports.
In `doors.ts`:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/doors.ts"), "typescript")//import_statement ! `{.//(namespace_import/identifier | import_specifier/*[@field = "name"])} from {string/string_fragment}`'
"registry from ./registry.ts"
"audit from ./audit.ts"
"Door from ./http.ts"
```

A *namespace import*, `import * as registry from "./registry.ts"`,
makes `registry` stand for the module `registry`, so the call
`registry.getDoor` is the function `registry.getDoor`. A *named
import*, `import { listDoors } from "./doors.ts"` in `server.ts`, makes
`listDoors` stand for the function `doors.listDoors`. `graph:imports`
makes a map from each imported name to what it stands for. Imports
from packages, `node:http` and `redis`, are not modules of the program,
so they are left out, and calls to them become external:

```xquery
{{#include ../examples/call-chains/graph.xqm:imports}}
```

The other names a call can start with are the function's own: its
parameters and variables. A call through one of them is a method of a
local value. `graph:locals` lists them, and `graph:root` finds the name
a callee starts with, going down the `object` fields of a chain of
member expressions:

```xquery
{{#include ../examples/call-chains/graph.xqm:locals}}
```

`graph:callee` puts the rules together. It returns a record of the
type `graph:target`, declared at the top of the module with
`declare record graph:target(id as xs:string, external as xs:boolean)`,
or nothing for a method of a local value:

```xquery
{{#include ../examples/call-chains/graph.xqm:callee}}
```

In order:

1. A plain name that was imported is the function it was imported as;
   one that names a function of the same module is that function; a
   local name is a value, not part of the graph; any other name is a
   function outside the program, such as `fetch`.
2. `this.check` in a method is the method `check` of the same class.
3. `registry.getDoor`, where `registry` is a namespace import, is the
   function `getDoor` of that module. The test that the object *is* the
   root (`is` compares the identity of nodes) keeps out a longer chain
   such as `registry.cache.get`.
4. A callee that starts with a local name or with a value, as in
   `door.codes.includes` or `new Date().toISOString`, is left out.
   Anything else, `JSON.parse` or `client.get` on a const of the
   module, is external.

## Functions passed as values

`listDoors` ends with `doors.map(summarize)`. That is a call of
`doors.map`, a method of a local array, but `summarize` is called too,
by `map`, once per door. A function of the module named as an argument
is an edge of the graph as well; the graph marks it `passed`.

## Calls in loops

A call inside a loop of its function runs once per iteration. The new
`listDoors` calls `registry.getDoor` in a `for … of` loop, which the
grammar calls a `for_in_statement` (it has `for … in` and `for … of`
both). A loop belongs to the call's function only when the nearest
function above the loop is the call's function too; a loop in an
outer function does not count:

```xquery
{{#include ../examples/call-chains/graph.xqm:in-loop}}
```

The graph marks such a call `repeated`. A loop is not the only way to
call something many times: the `summarize` passed to `map` runs once per
door as well. The graph says only what the code says in so many words.

## The graph as XML

`graph:graph` takes the parsed modules of a program as a map from the
module name to its `program` element, and returns the graph: a
`function` element for each function, and a `call` element for each
call that resolves to a function, with its line:

```xquery
{{#include ../examples/call-chains/graph.xqm:graph}}
```

The `for $to at $position in ($target, $passed)` clause makes one
`call` element for the callee and one for each function passed to it;
the position tells them apart. The conditional attributes use the
braced form of `if`, `if (C) { E }`, which needs no `else`.

`graph:read` reads a directory of TypeScript files into that map, with
the EXPath File functions:

```xquery
{{#include ../examples/call-chains/graph.xqm:read}}
```

`graph.xq` prints the graph of a directory:

```xquery
{{#include ../examples/call-chains/graph.xq}}
```

The File functions need `--allow-file-access`, and the directory is an
external variable:

```console
$ sxml xquery --allow-file-access --param=directory=after graph.xq
<graph>
  <function id="audit.record" line="4"/>
  <function id="doors.summarize" line="12"/>
  <function id="doors.listDoors" line="16"/>
  <function id="doors.codeMatches" line="25"/>
  <function id="doors.unlockDoor" line="29"/>
  <function id="http.request" line="10"/>
  <function id="registry.getDoorIds" line="4"/>
  <function id="registry.getDoor" line="8"/>
  <function id="registry.setState" line="12"/>
  <function id="server.route" line="5"/>
  <function id="server.start" line="17"/>
  <call from="audit.record" to="http.request" line="5"/>
  <call from="audit.record" to="JSON.stringify" line="7" external="true"/>
  <call from="doors.listDoors" to="registry.getDoorIds" line="17"/>
  <call from="doors.listDoors" to="registry.getDoor" line="20" repeated="true"/>
  <call from="doors.listDoors" to="doors.summarize" line="22" passed="true"/>
  <call from="doors.unlockDoor" to="registry.getDoor" line="30"/>
  <call from="doors.unlockDoor" to="doors.codeMatches" line="31"/>
  <call from="doors.unlockDoor" to="registry.setState" line="34"/>
  <call from="doors.unlockDoor" to="audit.record" line="35"/>
  <call from="http.request" to="fetch" line="11" external="true"/>
  <call from="registry.getDoorIds" to="http.request" line="5"/>
  <call from="registry.getDoor" to="http.request" line="9"/>
  <call from="registry.setState" to="http.request" line="13"/>
  <call from="registry.setState" to="JSON.stringify" line="15" external="true"/>
  <call from="server.route" to="doors.listDoors" line="7"/>
  <call from="server.route" to="JSON.parse" line="11" external="true"/>
  <call from="server.route" to="doors.unlockDoor" line="12"/>
  <call from="server.start" to="createServer" line="18" external="true"/>
  <call from="server.start" to="server.route" line="23"/>
  <call from="server.start" to="JSON.stringify" line="25" external="true"/>
</graph>
```

`server.start` calls `server.route` from inside the arrow function it
passes to `createServer`; the arrow function has no name, so the call
belongs to `start`. The calls `doors.push`, `response.json`,
`path.match` and the others on local values are not there.

## Asking the graph

Once the graph is a document, questions about the program are short
XPath expressions. Save it first:

```console
$ sxml xquery --allow-file-access --param=directory=after graph.xq --output=after.xml
```

Who calls `registry.getDoor`, and where?

```console
$ sxml xpath --context-item=after.xml '//call[@to = "registry.getDoor"] ! `{@from}, line {@line}{if (@repeated) then ", in a loop" else ""}`'
"doors.listDoors, line 20, in a loop"
"doors.unlockDoor, line 30"
```

Which functions does no call reach? Those are where the program is
entered:

```console
$ sxml xpath --context-item=after.xml '//function[not(@id = //call/@to)]/@id/string()'
"server.start"
```

Which functions outside the program does it use?

```console
$ sxml xpath --context-item=after.xml 'distinct-values(//call[@external]/@to)'
"JSON.stringify"
"fetch"
"JSON.parse"
"createServer"
```

## What a call graph from the code cannot see

`kinds/lock.ts` has the other kinds of functions: two methods, an
arrow function assigned to a const, and an arrow function passed to
`map`:

```typescript
{{#include ../examples/call-chains/kinds/lock.ts}}
```

```console
$ sxml xquery --allow-file-access --param=directory=kinds graph.xq
<graph>
  <function id="lock.Lock.open" line="4"/>
  <function id="lock.Lock.check" line="8"/>
  <function id="lock.openAll" line="13"/>
  <function id="lock.log" line="16"/>
  <call from="lock.Lock.open" to="lock.Lock.check" line="5"/>
  <call from="lock.Lock.open" to="lock.log" line="5"/>
  <call from="lock.log" to="console.log" line="17" external="true"/>
</graph>
```

The methods, `this.check` and the call of `log` are found. But
`openAll` calls `Lock.open` for each lock, and the graph does not say
so: `lock.open(code)` is a call on `lock`, a parameter of the arrow
function, and the graph cannot know what `lock` is without the types.
The code has them (`locks: Lock[]`), and a longer query could follow
the type annotations to the class. Other calls no query of the code
can resolve: a function chosen at run time, from a map of handlers or
by a name in a string. A call graph from the code is a good
approximation, and for this project it is exact: the lock service
calls its modules through namespace and named imports, and everything
else is a local value or outside the program.
