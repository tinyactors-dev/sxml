# Who Calls Whom, and How Often

<!-- examples: traces -->

A trace diagram shows every span, but a change is easier to see one
level up: which services take part in a request, which calls which,
and how many times. That is the *cardinality* of each call. This
section computes it from the normal form of [Spans as a
Tree](traces-tree.md), and then looks at the order of the calls with
windows.

The examples use both files in the normal form:

```console
$ sxml xquery --param=file=before.json --output=before.xml normalize.xq
$ sxml xquery --param=file=after.json --output=after.xml normalize.xq
```

## Participants

Every call is a client span. The caller is the service that recorded
it. The callee is harder: when the called service is traced too, the
client span's child is the server span that service recorded, and its
`service` names it. PostgreSQL and Redis record no spans, so for them
the client span's `peer`, the `server.address` attribute, has to do.

`trace.xqm` (the calls):

```xquery
{{#include ../examples/traces/trace.xqm:calls}}
```

`($client/span[@kind = "server"]/@service, $client/@peer)[1]` takes the
first of the two that exists. The calls of the old version:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    trace:calls(trace:load("before.json"))'
{"route":"GET /flags","caller":"api-gateway","callee":"flag-service"}
{"route":"GET /flags","caller":"flag-service","callee":"flag-registry"}
{"route":"GET /flags","caller":"flag-registry","callee":"postgresql"}
{"route":"POST /flags/{key}/kill","caller":"api-gateway","callee":"flag-service"}
{"route":"POST /flags/{key}/kill","caller":"flag-service","callee":"flag-cache"}
{"route":"POST /flags/{key}/kill","caller":"flag-service","callee":"flag-registry"}
{"route":"POST /flags/{key}/kill","caller":"flag-registry","callee":"postgresql"}
{"route":"POST /flags/{key}/kill","caller":"flag-service","callee":"flag-registry"}
{"route":"POST /flags/{key}/kill","caller":"flag-registry","callee":"postgresql"}
```

The route comes from the trace the call belongs to: the same call
between the same services can happen for different requests, and a
change can affect one route and not another.

## Counting with group by

Grouping the calls by all three fields makes one group for each pair
of services in each route, and `count` tells how many calls the group
has:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    for $call in trace:calls(trace:load("after.json"))
    group by $route := $call?route, $caller := $call?caller, $callee := $call?callee
    where $route = "GET /flags"
    return `{ $caller } -> { $callee }: { count($call) }`'
"api-gateway -> flag-service: 1"
"flag-service -> flag-registry: 5"
"flag-registry -> postgresql: 5"
```

A file can hold many traces of the same route, so the module divides by
the number of requests and keeps the result in a map, with a readable
key for each call and a record as its value:

`trace.xqm` (the cardinality):

```xquery
{{#include ../examples/traces/trace.xqm:cardinality}}
```

A map makes the next step, comparing two files, a matter of looking up
the same key in both:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    ("before.json", "after.json")
      ! trace:cardinality(trace:load(.))?("GET /flags: flag-service -> flag-registry")?count'
1
5
```

`cardinality.xq` lists the whole map of one file:

```xquery
{{#include ../examples/traces/cardinality.xq}}
```

```console
$ sxml xquery --param=file=before.json cardinality.xq
GET /flags: api-gateway -> flag-service  1
GET /flags: flag-registry -> postgresql  1
GET /flags: flag-service -> flag-registry  1
POST /flags/{key}/kill: api-gateway -> flag-service  1
POST /flags/{key}/kill: flag-registry -> postgresql  2
POST /flags/{key}/kill: flag-service -> flag-cache  1
POST /flags/{key}/kill: flag-service -> flag-registry  2
$ sxml xquery --param=file=after.json cardinality.xq
GET /flags: api-gateway -> flag-service  1
GET /flags: flag-registry -> postgresql  5
GET /flags: flag-service -> flag-registry  5
POST /flags/{key}/kill: api-gateway -> flag-service  1
POST /flags/{key}/kill: audit-service -> postgresql  1
POST /flags/{key}/kill: flag-registry -> postgresql  2
POST /flags/{key}/kill: flag-service -> audit-service  1
POST /flags/{key}/kill: flag-service -> flag-registry  2
```

Listing the flags now makes five calls to the registry, and the registry
five queries; killing a flag no longer reads the cache, and calls a
new service, which writes to the database. The counts say *how many*.
The order of the calls, which the windows below look at, says more
about *why*.

## Tumbling windows

`group by` doesn't care where the members of a group are. Sometimes
position is the whole point. The children of a span are in the order
they started, and a run of the same call one after the other is the
mark of a loop in the code.

A *tumbling window* cuts a sequence into runs that don't overlap. The
`start` condition says where a new run begins. Here a run begins at
each call whose name differs from the call before it:

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    for $server in //span[@service = "flag-service"][@kind = "server"]
    for tumbling window $run in $server/span
        start $first previous $before when not($first/@name = $before/@name)
    return `{ $server/@name }: { count($run) } x { $first/@name }`'
"GET /flags: 1 x GET /flag-keys"
"GET /flags: 4 x GET /flags/{key}"
"POST /flags/{key}/kill: 1 x GET /flags/{key}"
"POST /flags/{key}/kill: 1 x PUT /flags/{key}/percentage"
"POST /flags/{key}/kill: 1 x POST /events"
```

`$run` is bound to the items of each window in turn, and `$first` to
the item that starts it. `previous $before` binds the item before the
start, which for the first item is the empty sequence; `not($first/@name
= $before/@name)` is then true, so the first item starts a window too.
Without an `end` condition, a window ends just before the next one
starts. The run of four `GET /flags/{key}` is the loop the change added:
one call per flag, the pattern known as N+1, where one query for all
the flags would do.

Start and end can be bound to more variables: the item itself, its
position (`at $p`), and the items before it and after it (`previous`,
`next`). An exporter sends spans to the collector in batches; this
tumbling window makes batches of at most eight spans, using positions:

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    for tumbling window $batch in //span
        start at $start when true()
        end at $end when $end - $start = 7
    return `{ count($batch) } spans, the first { $batch[1]/@id }`'
"8 spans, the first ad09d89327ec549f"
"8 spans, the first 62982530bd8bd355"
"8 spans, the first 68c9e72c9c458574"
"6 spans, the first cc8674ff41de5169"
```

Each window ends seven items after it starts. The last window is
shorter: the sequence ran out before its end condition was met.

## Windows with an end

A collector receives each span when it ends, and a live view of a
system sees requests start before it knows how they finish.
`timeline.xq` turns a file in the normal form into that view: an event
for the start and one for the end of every span, in the order they
happened, up to a moment `$until`:

`timeline.xq`:

```xquery
{{#include ../examples/traces/timeline.xq}}
```

At 20 milliseconds past 09:00:02, the kill request is still running:

```console
$ sxml xquery --context-item=after.xml --param=until=2026-10-21T09:00:02.020Z --output=live.xml timeline.xq
```

A window from the start of each request, a root span, to the end of the
same span is one request. When the end condition is never met, the
window ends at the last item, and the end variable is bound to that
last item, so test what it is:

```console
$ sxml xquery --context-item=live.xml --method=adaptive --expression='
    for tumbling window $request in /timeline/*
        start $open when $open[self::start]/@root = "true"
        end $close when $close[self::end]/@span = $open/@span
    return `{ $open/@name } at { $open/@at }: { count($request[self::start]) } spans, {
      if ($close/@span = $open/@span) then "ended at " || $close/@at else "still running" }`'
"GET /flags at 2026-10-21T09:00:00Z: 18 spans, ended at 2026-10-21T09:00:00.11Z"
"POST /flags/{key}/kill at 2026-10-21T09:00:02Z: 9 spans, still running"
```

The end condition refers to `$open`, the start of its own window: a
window can end at an item that depends on how it began.

`only end` instead of `end` drops a window whose end condition is never
met. That's what you want to compute how long the requests took:

```console
$ sxml xquery --context-item=live.xml --method=adaptive --expression='
    for tumbling window $request in /timeline/*
        start $open when $open[self::start]/@root = "true"
        only end $close when $close[self::end]/@span = $open/@span
    let $took := (xs:dateTime($close/@at) - xs:dateTime($open/@at))
                 div xs:dayTimeDuration("PT0.001S")
    return `{ $open/@name }: { $took } ms`'
"GET /flags: 110 ms"
```

Subtracting two `xs:dateTime` values gives a duration, and dividing a
duration by a duration gives a number.

## Sliding windows

A *sliding window* starts a window at every item where the start
condition holds, so its windows can overlap. How many calls did the
flag service start within 50 milliseconds of each other, while it
listed the flags? A window starts at each call and ends before the
first call that starts 50 milliseconds or more after it:

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    let $calls := //trace[@route = "GET /flags"]//span[@service = "flag-service"][@kind = "client"]
    for sliding window $burst in $calls
        start $first when true()
        end next $after when xs:decimal($after/@start) - xs:decimal($first/@start) ge 50
    return array { $burst/@start ! xs:decimal(.) }'
[4,18,40]
[18,40,62]
[40,62,84]
[62,84]
[84]
```

The windows overlap: 18 and 40 are in three of them. The last three
windows end because the calls ran out, not because a call came 50
milliseconds later; `only end` drops them, and keeps only the bursts
that are known to be complete:

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    let $calls := //trace[@route = "GET /flags"]//span[@service = "flag-service"][@kind = "client"]
    for sliding window $burst in $calls
        start $first when true()
        only end next $after when xs:decimal($after/@start) - xs:decimal($first/@start) ge 50
    return array { $burst/@start ! xs:decimal(.) }'
[4,18,40]
[18,40,62]
```

`next` makes it easy to compare an item with the one after it. Which
calls did the flag service make twice in a row, from the same span?

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    for sliding window $pair in //span[@service = "flag-service"][@kind = "client"]
        start $first next $second
            when $second/@name = $first/@name and $second/.. is $first/..
        end when true()
    return `{ $first/@name } at { $first/@start } ms, again at { $second/@start } ms`'
"GET /flags/{key} at 18 ms, again at 40 ms"
"GET /flags/{key} at 40 ms, again at 62 ms"
"GET /flags/{key} at 62 ms, again at 84 ms"
```

`end when true()` ends each window at the item that starts it: the
windows are only a way to look at each item together with its
neighbour. `$second/.. is $first/..` keeps neighbours that belong to
different requests apart; without it, the last `GET /flags/{key}` of the
listing and the first of the kill request would make a pair.

## Choosing between them

- Use `group by` when the members of a group can be anywhere, and the
  group is named by a value: a route, a pair of services, a status.
- Use a tumbling window when the groups are runs of neighbours: the
  same call repeated, a request from its start to its end, batches of
  eight.
- Use a sliding window to look at each item with the items around it:
  bursts within a time, gaps, repeated values.
