# The Trace Diff

<!-- examples: traces -->

With a map of cardinalities for each file, the diff of two trace files
is the diff of two maps: the calls that are only in the new map were
added, the calls only in the old one were removed, and the calls in
both whose counts differ changed. In this section we write it, with the
versions of the programs beside it, as text and as JSON.

## Matching traces by route

Two files can only be compared where they recorded the same kind of
request. A file recorded during a test of the kill switch has no
`GET /api/getFlags`, and its missing calls are not a change. So the diff first
finds the routes both files have, and compares the calls of those
routes only:

`trace.xqm` (the diff):

```xquery
{{#include ../examples/traces/trace.xqm:diff}}
```

The keys of both maps together, without duplicates, are every call of
either file. A call missing from one map has the count 0 there:
looking up a missing key gives the empty sequence, and `otherwise 0`
replaces it. The `order by $key` sorts the changes by route, caller and
callee, since that is how the key is made.

## The versions of the programs

The versions of the programs that recorded the spans, the resources,
tell which deployment changed. A program that only one file has, has no
version in the other:

`trace.xqm` (the versions):

```xquery
{{#include ../examples/traces/trace.xqm:versions}}
```

`deep-equal` compares two sequences, so it also says that a version and
no version differ, where `ne` would give the empty sequence.

## The diff as text

`diff.xq`:

```xquery
{{#include ../examples/traces/diff.xq}}
```

The external variables have defaults, so without parameters it compares
the two files of the project:

```console
$ sxml xquery diff.xq
vane: 2.3.0 -> 2.4.0
GET /api/getFlags: vane -> postgresql  1 -> 5
POST /api/killFlag: vane -> hooks.example.com  0 -> 1
POST /api/killFlag: vane -> postgresql  2 -> 3
POST /api/killFlag: vane -> redis  1 -> 0
```

The service's new version made listing the flags query PostgreSQL
five times instead of once. [Project: A Diff of Call Chains](call-chains.md)
found the cause in the code, a call of the registry module inside a
loop over the flag keys, and predicted `flags.listFlags > registry: 1
-> 1 + n`, one call plus one per flag, each call a query. With four
flags, that is five. Killing a flag no longer reads Redis; the audit
module writes one more row to PostgreSQL, and posts to a new
participant, the owners' webhook. The storefront and the console still
call the service once per request, and their versions did not change,
so the diff leaves them out.

## The diff as JSON

Other tools, a dashboard or a check in a deployment pipeline, want the
diff as data. Records are maps, so the JSON output method writes them
as objects:

`diff-json.xq`:

```xquery
{{#include ../examples/traces/diff-json.xq}}
```

```console
$ sxml xquery diff-json.xq
{
  "before": "before.json",
  "after": "after.json",
  "versions": [
    {
      "service": "vane",
      "before": "2.3.0",
      "after": "2.4.0"
    }
  ],
  "changes": [
    {
      "route": "GET /api/getFlags",
      "caller": "vane",
      "callee": "postgresql",
      "before": 1,
      "after": 5
    },
    {
      "route": "POST /api/killFlag",
      "caller": "vane",
      "callee": "hooks.example.com",
      "before": 0,
      "after": 1
    },
    {
      "route": "POST /api/killFlag",
      "caller": "vane",
      "callee": "postgresql",
      "before": 2,
      "after": 3
    },
    {
      "route": "POST /api/killFlag",
      "caller": "vane",
      "callee": "redis",
      "before": 1,
      "after": 0
    }
  ]
}
```

JSON allows a solidus to be written as `\/`, and the serializer does so unless
`escape-solidus` is `no`, which keeps the routes readable.

## When the files share no route

To see the error, we need two files with different routes. `select.xq`
writes a copy of an OTLP file with only the traces of one route: it
finds the trace ids of the root spans with that name, and rebuilds the
nested arrays with `map:put` and `array:filter`, leaving out scopes
and resources that have no spans left:

`select.xq`:

```xquery
{{#include ../examples/traces/select.xq}}
```

```console
$ sxml xquery --param=file=before.json '--param=route=GET /api/getFlags' --output=flags.json select.xq
$ sxml xquery --param=file=after.json '--param=route=POST /api/killFlag' --output=kill.json select.xq
$ sxml xpath 'json-doc("kill.json")?resourceSpans?*?scopeSpans?*?spans?* => count()'
10
```

Comparing the listing of the old version with only the kill request
of the new one:

```console
$ sxml xquery --param=before=flags.json --param=after=kill.json diff.xq
trace.xqm:180:35: trace:no-shared-route: flags.json and kill.json have no route in common
```

The error's code is a QName in the module's own namespace,
`trace:no-shared-route`, so a caller can catch exactly this error and
let every other error through:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    for $after in ("after.json", "kill.json")
    return try {
      "changes: " || count(trace:diff(trace:load("flags.json"), trace:load($after)))
    } catch trace:no-shared-route {
      "nothing to compare: " || $err:description
    }'
"changes: 1"
"nothing to compare: flags.json and kill.json have no route in common"
```

Against `after.json`, only `GET /api/getFlags` is compared, and its
change is found; the kill request's changes are not, because
`flags.json` has none of them to compare with. The versions are
compared all the same, and here one appears that only one file has:

```console
$ sxml xquery --param=before=flags.json diff.xq
vane: 2.3.0 -> 2.4.0
vane-console: none -> 1.8.0
GET /api/getFlags: vane -> postgresql  1 -> 5
```

`flags.json` holds only the listing, so it has no span of the console,
and no resource for it either: the console's version is the empty
sequence there, which `otherwise "none"` in `diff.xq` writes as
`none`. With real deployments, such a line shows a program that is new,
or gone. In JSON, an empty field becomes `null`:

```console
$ sxml xquery --method=json --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    array { trace:versions(trace:load("flags.json"), trace:load("after.json")) }'
[{"service":"vane","before":"2.3.0","after":"2.4.0"},{"service":"vane-console","before":null,"after":"1.8.0"}]
```
