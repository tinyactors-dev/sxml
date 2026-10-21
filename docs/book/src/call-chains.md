# Project: A Diff of Call Chains

<!-- examples: call-chains -->

A code review shows which lines changed. It does not show what the
change does to the program around those lines: which functions now run
when a request comes in, which of them run once and which once per item
of a list, which databases and services the program talks to. Those are
questions about *call chains*, the paths of calls from where a program
is entered down to the functions that do the work, and the lines of a
diff hold the answer only for a reader who follows every call by hand.

In this project we let a query follow the calls. [Source Code as
XML](source-code.md) showed that `sxml:parse-code` turns TypeScript into
an XML document; here we read every module of a small service, build its
*call graph* (which function calls which), list its call chains, and
compare the graphs and chains of two versions of the service. The
result is a report of what the change does, as text and as HTML, and a
drawing of the two call graphs in one.

The examples are in `docs/book/examples/call-chains`. Run every command
of this project from that directory.

## The Vane service

Vane is a feature-flag product: applications ask it whether a feature
is on for a user, and the owners of a flag roll it out to a growing
percentage of users. It is one service, `vane`, a TypeScript program on
Node.js that keeps the flags in PostgreSQL and the flags read recently
in Redis. Its callers reach it through *remote functions*, one HTTP
route per function, and the change this project looks at touches two
of them. `GET /api/getFlags` answers the backend SDK,
`@vane/node`, which downloads every flag with its targeting rules and
evaluates the flags in the application itself. `POST /api/killFlag` is
the kill switch, which the console calls: it sets a flag's percentage
to 0 when the request comes from one of the flag's owners.

The service is a set of modules, among them `rollout`, which runs the
rollout lifecycle of each flag. The directories of this project hold the modules that those two
functions go through:

- `api.ts` is the HTTP server. `start` starts it, and `route` sends
  each request to the function that answers it.
- `flags.ts` answers the requests: `listFlags` and `killFlag`.
- `registry.ts` is the registry module, which keeps the flags in
  PostgreSQL: `getFlag`, `setPercentage` and others.
- `db.ts` has `query`, which every module uses to send SQL to
  PostgreSQL through one pool of connections.
- `cache.ts` is the cache module, which reads flags from Redis.

`flags.ts`:

```typescript
{{#include ../examples/call-chains/before/flags.ts}}
```

`listFlags` answers with a `FlagSummary` per flag, the fields an SDK
needs to evaluate the flag, and `killFlag` checks the owner before it
sets the percentage. The module imports the others as namespaces (`import * as registry`),
so each call to another module names it: `registry.getFlags()`,
`cache.getFlag(key)`. That will make the calls easy to resolve.

## The change

The directory `before` holds the modules as they were, and `after` the
modules after a change. One module is new, one is no longer on the
path, and two changed:

```console
$ diff -q before after
Only in after: audit.ts
Only in before: cache.ts
Files before/flags.ts and after/flags.ts differ
Files before/registry.ts and after/registry.ts differ
```

The change to `flags.ts`:

```console
$ diff before/flags.ts after/flags.ts
3c3
< import * as cache from "./cache.ts";
---
> import * as audit from "./audit.ts";
19c19,23
<   const flags = await registry.getFlags();
---
>   const keys = await registry.getFlagKeys();
>   const flags: Flag[] = [];
>   for (const key of keys) {
>     flags.push(await registry.getFlag(key));
>   }
28c32
<   const flag = (await cache.getFlag(key)) ?? (await registry.getFlag(key));
---
>   const flag = await registry.getFlag(key);
32a37
>   await audit.record(key, "kill");
```

In words: the registry module no longer has a function that returns
every flag, so `listFlags` asks for the keys of the flags and then for
each flag, one call per flag. The new `registry.ts` has `getFlagKeys` in
place of `getFlags`. `killFlag` no longer reads the flag from the cache
module: a kill must act on the flag as PostgreSQL holds it, not on a
copy that Redis may have kept for a minute. So no path of the two
functions goes through the cache module any more, and `cache.ts` is not
in `after`; the rest of the service still uses it. And `killFlag` now
records each kill with the new *audit module* (`audit.ts`), which
writes the kill to the audit log in PostgreSQL and tells the flag's
owners through their webhook.

A reviewer who knows the code sees what this means at run time: listing
four flags took one query to PostgreSQL and now takes five. A reviewer
who does not, sees a loop. The rest of this project makes the first
reading come out of the code.

## A first look

`sxml:parse-code` reads each file. With `--allow-file-access`, the
EXPath File functions list the directories and read the files, so one
expression looks at all ten of them:

```console
$ sxml xpath --allow-file-access 'for $version in ("before", "after") for $file in sort(file:list($version)) let $tree := sxml:parse-code(file:read-text($version || "/" || $file), "typescript") return `{$version}/{$file}: {count($tree//function_declaration)} functions, {count($tree//(ERROR | *[@missing]))} errors`'
"before/api.ts: 2 functions, 0 errors"
"before/cache.ts: 1 functions, 0 errors"
"before/db.ts: 1 functions, 0 errors"
"before/flags.ts: 4 functions, 0 errors"
"before/registry.ts: 3 functions, 0 errors"
"after/api.ts: 2 functions, 0 errors"
"after/audit.ts: 1 functions, 0 errors"
"after/db.ts: 1 functions, 0 errors"
"after/flags.ts: 4 functions, 0 errors"
"after/registry.ts: 3 functions, 0 errors"
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

The report ends with a prediction: where `listFlags` made one call to
the registry module, one query to PostgreSQL, it now makes one call and
one more per flag. A static
analysis can say *one per flag*; only the running service can say how
many flags there are. [Project: Comparing Traces Before and After a
Change](traces.md) checks the prediction against traces the service
recorded before and after the change.
