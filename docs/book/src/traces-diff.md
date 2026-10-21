# The Trace Diff

<!-- examples: traces -->

With a map of cardinalities for each file, the diff of two trace files
is the diff of two maps: the calls that are only in the new map were
added, the calls only in the old one were removed, and the calls in
both whose counts differ changed. In this section we write it, with the
versions of the services beside it, as text and as JSON.

## Matching traces by route

Two files can only be compared where they recorded the same kind of
request. A file recorded during a test of the unlock button has no
`GET /doors`, and its missing calls are not a change. So the diff first
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

## The versions of the services

The services' versions tell which deployment changed. A service that
only one file has, has no version in the other:

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
lock-service: 2.3.0 -> 2.4.0
audit-service: none -> 0.3.1
GET /doors: lock-registry -> postgresql  1 -> 5
GET /doors: lock-service -> lock-registry  1 -> 5
POST /doors/{id}/unlock: audit-service -> postgresql  0 -> 1
POST /doors/{id}/unlock: lock-service -> audit-service  0 -> 1
POST /doors/{id}/unlock: lock-service -> door-cache  1 -> 0
```

The lock service's new version made listing the doors call the registry
five times instead of once, and the registry query the database five
times. [Project: A Diff of Call Chains](call-chains.md) found the cause
in the code, a call to the registry inside a loop over the door ids,
and predicted one call plus one per door. With four doors, that is
five. Unlocking a door no longer reads the cache and calls the new
audit service, which writes to the database. Nothing changed between
the gateway and the lock service, so the diff leaves those calls out.

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
      "service": "lock-service",
      "before": "2.3.0",
      "after": "2.4.0"
    },
    {
      "service": "audit-service",
      "before": null,
      "after": "0.3.1"
    }
  ],
  "changes": [
    {
      "route": "GET /doors",
      "caller": "lock-registry",
      "callee": "postgresql",
      "before": 1,
      "after": 5
    },
    {
      "route": "GET /doors",
      "caller": "lock-service",
      "callee": "lock-registry",
      "before": 1,
      "after": 5
    },
    {
      "route": "POST /doors/{id}/unlock",
      "caller": "audit-service",
      "callee": "postgresql",
      "before": 0,
      "after": 1
    },
    {
      "route": "POST /doors/{id}/unlock",
      "caller": "lock-service",
      "callee": "audit-service",
      "before": 0,
      "after": 1
    },
    {
      "route": "POST /doors/{id}/unlock",
      "caller": "lock-service",
      "callee": "door-cache",
      "before": 1,
      "after": 0
    }
  ]
}
```

The empty `before` of the audit service became `null`. JSON allows a
solidus to be written as `\/`, and the serializer does so unless
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
$ sxml xquery --param=file=before.json '--param=route=GET /doors' --output=doors.json select.xq
$ sxml xquery --param=file=after.json '--param=route=POST /doors/{id}/unlock' --output=unlock.json select.xq
$ sxml xpath 'json-doc("unlock.json")?resourceSpans?*?scopeSpans?*?spans?* => count()'
12
```

Comparing the listing of the old version with only the unlock request
of the new one:

```console
$ sxml xquery --param=before=doors.json --param=after=unlock.json diff.xq
trace.xqm:176:35: trace:no-shared-route: doors.json and unlock.json have no route in common
```

The error's code is a QName in the module's own namespace,
`trace:no-shared-route`, so a caller can catch exactly this error and
let every other error through:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    for $after in ("after.json", "unlock.json")
    return try {
      count(trace:diff(trace:load("doors.json"), trace:load($after))) || " changes"
    } catch trace:no-shared-route {
      "nothing to compare: " || $err:description
    }'
"2 changes"
"nothing to compare: doors.json and unlock.json have no route in common"
```

Against `after.json`, only `GET /doors` is compared, and its two
changes are found; the unlock request's changes are not, because
`doors.json` has none of them to compare with.
