# Project: A Diff of Call Chains

<!-- examples: call-chains -->

A code review shows which lines changed. It does not show what the
change does to the program around those lines: which functions now run
when a request comes in, which of them run once and which once per item
of a list, which services the program talks to. Those are questions
about *call chains*, the paths of calls from where a program is entered
down to the functions that do the work, and the lines of a diff hold
the answer only for a reader who follows every call by hand.

In this project we let a query follow the calls. [Source Code as
XML](source-code.md) showed that `sxml:parse-code` turns TypeScript into
an XML document; here we read every file of a small service, build its
*call graph* (which function calls which), list its call chains, and
compare the graphs and chains of two versions of the service. The
result is a report of what the change does, as text and as HTML, and a
drawing of the two call graphs in one.

The examples are in `docs/book/examples/call-chains`. Run every command
of this project from that directory.

## The lock service

The Latch system locks doors. Its *lock service* answers two requests:
`GET /doors` lists the doors with their state, and `POST
/doors/{id}/unlock` unlocks a door when the code is right. It does not
keep the doors itself: it asks the *lock registry*, another service,
over HTTP. The service is five TypeScript modules:

- `server.ts` is the HTTP server. `start` starts it, and `route` sends
  each request to the function that answers it.
- `doors.ts` answers the requests: `listDoors` and `unlockDoor`.
- `registry.ts` is the client of the lock registry: `getDoor`,
  `setState` and others.
- `http.ts` has `request`, which every client uses to call a service.
- `cache.ts` reads doors from a Redis cache.

`doors.ts`:

```typescript
{{#include ../examples/call-chains/before/doors.ts}}
```

The module imports the others as namespaces (`import * as registry`),
so each call to another module names it: `registry.getDoors()`,
`cache.getDoor(id)`. That will make the calls easy to resolve.

## The change

The directory `before` holds the service as it was, and `after` the
service after a change. Two modules are new or gone, and two changed:

```console
$ diff -q before after
Only in after: audit.ts
Only in before: cache.ts
Files before/doors.ts and after/doors.ts differ
Files before/registry.ts and after/registry.ts differ
```

The change to `doors.ts`:

```console
$ diff before/doors.ts after/doors.ts
3c3
< import * as cache from "./cache.ts";
---
> import * as audit from "./audit.ts";
17c17,21
<   const doors = await registry.getDoors();
---
>   const ids = await registry.getDoorIds();
>   const doors: Door[] = [];
>   for (const id of ids) {
>     doors.push(await registry.getDoor(id));
>   }
26c30
<   const door = (await cache.getDoor(id)) ?? (await registry.getDoor(id));
---
>   const door = await registry.getDoor(id);
30a35
>   await audit.record(id, "unlock");
```

In words: the registry no longer has a call that returns every door,
so `listDoors` asks for the ids of the doors and then for each door,
one call per door. `unlockDoor` no longer reads the cache, which is
gone, and records each unlock with the new *audit service*
(`audit.ts`). The new `registry.ts` has `getDoorIds` in place of
`getDoors`.

A reviewer who knows the code sees what this means at run time: listing
four doors took one call to the registry and now takes five. A reviewer
who does not, sees a loop. The rest of this project makes the first
reading come out of the code.

## A first look

`sxml:parse-code` reads each file. With `--allow-file-access`, the
EXPath File functions list the directories and read the files, so one
expression looks at all ten of them:

```console
$ sxml xpath --allow-file-access 'for $version in ("before", "after") for $file in sort(file:list($version)) let $tree := sxml:parse-code(file:read-text($version || "/" || $file), "typescript") return `{$version}/{$file}: {count($tree//function_declaration)} functions, {count($tree//(ERROR | *[@missing]))} errors`'
"before/cache.ts: 1 functions, 0 errors"
"before/doors.ts: 4 functions, 0 errors"
"before/http.ts: 1 functions, 0 errors"
"before/registry.ts: 3 functions, 0 errors"
"before/server.ts: 2 functions, 0 errors"
"after/audit.ts: 1 functions, 0 errors"
"after/doors.ts: 4 functions, 0 errors"
"after/http.ts: 1 functions, 0 errors"
"after/registry.ts: 3 functions, 0 errors"
"after/server.ts: 2 functions, 0 errors"
```

Every file parses without an error, so the trees are complete. Eleven
functions before, eleven after, and the counts say nothing about the
change: the same number of functions call different things.

## The plan

The project builds a library module, `graph.xqm`, and a few small
queries around it:

- [A Call Graph from the Code](call-chains-graph.md) finds the
  functions of each module and the calls in each function, resolves
  each call to the function it calls, through the imports, and writes
  the call graph as XML (`graph.xq`).
- [Comparing Two Versions](call-chains-diff.md) follows the graph from
  where the service is entered to list its call chains (`chains.xq`),
  once with a worklist and once with a recursive function, compares the
  graphs and chains of `before` and `after` (`diff.xq`), and writes the
  diff as a text report (`report.xq`), an HTML page (`report.xsl`) and
  a drawing.

The report ends with a prediction: where `listDoors` made one call to
the registry, it now makes one call and one more per door. A static
analysis can say *one per door*; only the running service can say how
many doors there are. [Project: Comparing Traces Before and After a
Change](traces.md) checks the prediction against traces the service
recorded before and after the change.
