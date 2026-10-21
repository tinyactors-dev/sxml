# A Call Graph from the Code

<!-- examples: call-chains -->

A call graph has a node for each function of a program and an edge
from a function to each function it calls. To build one from the trees
that `sxml:parse-code` makes, we need three things: the functions, the
calls inside each function, and for each call, the function it calls.
The first two are paths in the tree. The third is the interesting part:
the code says `registry.getFlag(key)`, and the graph needs to say
`registry.getFlag` of the module `registry.ts`, not a method of
something named `registry`.

This section builds the library module `graph.xqm` (namespace
`urn:example:graph`) one function at a time, and ends with the call
graph of both versions of the flag service as XML.

## Functions

In TypeScript's grammar a function declared with `function` is a
`function_declaration`, and its name is the child whose field is
`name`. With the option `positions`, each element knows its line:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/flags.ts"), "typescript", { "positions": true() })//function_declaration ! `{@line}: {identifier}`'
"12: summarize"
"16: listFlags"
"25: isOwner"
"29: killFlag"
```

The exported functions are inside an `export_statement`, the others
are not; `//` finds both. A program has functions of other kinds too:
methods of classes are `method_definition`s, and a function is often
an arrow function assigned to a const,
`const evaluateAll = (flags) => …`, which is an `arrow_function` inside a
`variable_declarator`. An arrow function passed as an argument, as in
`flags.map((flag) => flag.evaluate(user))`, has no name; its calls are
calls of the function around it. `graph.xqm` says this in two
functions:

```xquery
{{#include ../examples/call-chains/graph.xqm:functions}}
```

`graph:function-of` is the nearest function above any node: the
`ancestor` axis goes up from the node, and the predicate `[1]` on a
reverse axis is the nearest one. The functions are named after their
module, which is the file name without `.ts`: `flags.listFlags`,
`registry.getFlag`, and a method after its module and class,
`flag.Flag.evaluate`.

## Calls

A call is a `call_expression`, and what it calls is its child with the
field `function`. Its text is the callee as the code writes it:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/flags.ts"), "typescript", { "positions": true() })//call_expression ! `{@line}: {*[@field = "function"]}`'
"17: registry.getFlagKeys"
"20: flags.push"
"20: registry.getFlag"
"22: flags.map"
"26: flag.owners.includes"
"30: registry.getFlag"
"31: isOwner"
"34: registry.setPercentage"
"35: audit.record"
```

Line 20 has two calls, one inside the other: `flags.push(await
registry.getFlag(key))`. The callee is either an `identifier`
(`isOwner`) or a `member_expression`, whose `object` and
`property` fields are the two sides of the dot:

```console
$ sxml xpath --method=xml '(sxml:parse-code(unparsed-text("after/flags.ts"), "typescript", { "highlights": false() })//call_expression)[1]'
<call_expression xmlns:sxml="urn:sxml"><member_expression field="function"><identifier field="object">registry</identifier>.<property_identifier field="property">getFlagKeys</property_identifier></member_expression><arguments field="arguments">()</arguments></call_expression>
```

The list shows the three kinds of calls a call graph has to tell
apart. `registry.getFlag` and `audit.record` call functions of other
modules of the program, and `isOwner` a function of the same
module. `flags.push`, `flags.map` and `flag.owners.includes` call
methods of local values, an array and a string array, which are not
part of the program's structure. And other files have calls of
functions outside the program: `fetch`, `JSON.parse`, `createServer`.

## Resolving a callee

What a name stands for in a module comes from the module's imports.
In `flags.ts`:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("after/flags.ts"), "typescript")//import_statement ! `{.//(namespace_import/identifier | import_specifier/*[@field = "name"])} from {string/string_fragment}`'
"registry from ./registry.ts"
"audit from ./audit.ts"
"Flag from ./http.ts"
```

A *namespace import*, `import * as registry from "./registry.ts"`,
makes `registry` stand for the module `registry`, so the call
`registry.getFlag` is the function `registry.getFlag`. A *named
import*, `import { listFlags } from "./flags.ts"` in `server.ts`, makes
`listFlags` stand for the function `flags.listFlags`. `graph:imports`
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
3. `registry.getFlag`, where `registry` is a namespace import, is the
   function `getFlag` of that module. The test that the object *is* the
   root (`is` compares the identity of nodes) keeps out a longer chain
   such as `registry.cache.get`.
4. A callee that starts with a local name or with a value, as in
   `flag.owners.includes` or `new Date().toISOString`, is left out.
   Anything else, `JSON.parse` or `client.get` on a const of the
   module, is external.

## Functions passed as values

`listFlags` ends with `flags.map(summarize)`. That is a call of
`flags.map`, a method of a local array, but `summarize` is called too,
by `map`, once per flag. A function of the module named as an argument
is an edge of the graph as well; the graph marks it `passed`.

## Calls in loops

A call inside a loop of its function runs once per iteration. The new
`listFlags` calls `registry.getFlag` in a `for … of` loop, which the
grammar calls a `for_in_statement` (it has `for … in` and `for … of`
both). A loop belongs to the call's function only when the nearest
function above the loop is the call's function too; a loop in an
outer function does not count:

```xquery
{{#include ../examples/call-chains/graph.xqm:in-loop}}
```

The graph marks such a call `repeated`. A loop is not the only way to
call something many times: the `summarize` passed to `map` runs once per
flag as well. The graph says only what the code says in so many words.

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
  <function id="flags.summarize" line="12"/>
  <function id="flags.listFlags" line="16"/>
  <function id="flags.isOwner" line="25"/>
  <function id="flags.killFlag" line="29"/>
  <function id="http.request" line="11"/>
  <function id="registry.getFlagKeys" line="4"/>
  <function id="registry.getFlag" line="8"/>
  <function id="registry.setPercentage" line="12"/>
  <function id="server.route" line="5"/>
  <function id="server.start" line="17"/>
  <call from="audit.record" to="http.request" line="5"/>
  <call from="audit.record" to="JSON.stringify" line="7" external="true"/>
  <call from="flags.listFlags" to="registry.getFlagKeys" line="17"/>
  <call from="flags.listFlags" to="registry.getFlag" line="20" repeated="true"/>
  <call from="flags.listFlags" to="flags.summarize" line="22" passed="true"/>
  <call from="flags.killFlag" to="registry.getFlag" line="30"/>
  <call from="flags.killFlag" to="flags.isOwner" line="31"/>
  <call from="flags.killFlag" to="registry.setPercentage" line="34"/>
  <call from="flags.killFlag" to="audit.record" line="35"/>
  <call from="http.request" to="fetch" line="12" external="true"/>
  <call from="registry.getFlagKeys" to="http.request" line="5"/>
  <call from="registry.getFlag" to="http.request" line="9"/>
  <call from="registry.setPercentage" to="http.request" line="13"/>
  <call from="registry.setPercentage" to="JSON.stringify" line="15" external="true"/>
  <call from="server.route" to="flags.listFlags" line="7"/>
  <call from="server.route" to="JSON.parse" line="11" external="true"/>
  <call from="server.route" to="flags.killFlag" line="12"/>
  <call from="server.start" to="createServer" line="18" external="true"/>
  <call from="server.start" to="server.route" line="23"/>
  <call from="server.start" to="JSON.stringify" line="25" external="true"/>
</graph>
```

`server.start` calls `server.route` from inside the arrow function it
passes to `createServer`; the arrow function has no name, so the call
belongs to `start`. The calls `flags.push`, `response.json`,
`path.match` and the others on local values are not there.

## Asking the graph

Once the graph is a document, questions about the program are short
XPath expressions. Save it first:

```console
$ sxml xquery --allow-file-access --param=directory=after graph.xq --output=after.xml
```

Who calls `registry.getFlag`, and where?

```console
$ sxml xpath --context-item=after.xml '//call[@to = "registry.getFlag"] ! `{@from}, line {@line}{if (@repeated) then ", in a loop" else ""}`'
"flags.listFlags, line 20, in a loop"
"flags.killFlag, line 30"
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

`kinds/flag.ts` has the other kinds of functions: two methods, an
arrow function assigned to a const, and an arrow function passed to
`map`:

```typescript
{{#include ../examples/call-chains/kinds/flag.ts}}
```

```console
$ sxml xquery --allow-file-access --param=directory=kinds graph.xq
<graph>
  <function id="flag.Flag.evaluate" line="4"/>
  <function id="flag.Flag.check" line="8"/>
  <function id="flag.evaluateAll" line="13"/>
  <function id="flag.log" line="16"/>
  <call from="flag.Flag.evaluate" to="flag.Flag.check" line="5"/>
  <call from="flag.Flag.evaluate" to="flag.log" line="5"/>
  <call from="flag.log" to="console.log" line="17" external="true"/>
</graph>
```

The methods, `this.check` and the call of `log` are found. But
`evaluateAll` calls `Flag.evaluate` for each flag, and the graph does
not say so: `flag.evaluate(user)` is a call on `flag`, a parameter of
the arrow function, and the graph cannot know what `flag` is without
the types. The code has them (`flags: Flag[]`), and a longer query could follow
the type annotations to the class. Other calls no query of the code
can resolve: a function chosen at run time, from a map of handlers or
by a name in a string. A call graph from the code is a good
approximation, and for this project it is exact: the flag service
calls its modules through namespace and named imports, and everything
else is a local value or outside the program.
