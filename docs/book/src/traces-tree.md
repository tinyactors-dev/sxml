# Spans as a Tree

<!-- examples: traces -->

An OTLP file is grouped the way exporters send spans: by the program
that recorded them, then by the instrumentation library. The trees of
the traces are not in it, only the parent ids that make them. In this
section we take the file apart into one record per span, put the trees
together again, and write them as XML.

## Flattening the file

`resourceSpans`, `scopeSpans` and `spans` are arrays. An array is one
item, so `for $resource in $request?resourceSpans` would run once, with
the whole array. `for member` runs once for each member of an array:

```console
$ sxml xquery --method=adaptive --expression='
    for member $resource in json-doc("before.json")?resourceSpans
    for member $scope in $resource?scopeSpans
    return `{ $resource?resource?attributes?1?value?stringValue }, { $scope?scope?name }: { array:size($scope?spans) } spans`'
"storefront, @vane/node: 1 spans"
"vane, @opentelemetry/instrumentation-http: 2 spans"
"vane, vane: 6 spans"
"vane, @opentelemetry/instrumentation-pg: 3 spans"
"vane, @opentelemetry/instrumentation-redis: 1 spans"
"vane-console, @opentelemetry/instrumentation-fetch: 1 spans"
```

The SDK and the console each recorded one span, their call to the
service. The service's spans come from four libraries: HTTP for the
requests it served, PostgreSQL and Redis for its queries, and its own
scope, `vane`, for the functions of its modules.

`$resource?resource?attributes?1` relies on `service.name` being the
first attribute, which OTLP does not promise. Attributes are a list of
key-value pairs whose values say their own type, `{ "stringValue":
"GET" }` or `{ "intValue": "200" }`; a map from each key to its value is
easier to use. These functions of the module `trace.xqm`, in the
namespace `urn:example:trace`, make one:

`trace.xqm` (the values):

```xquery
{{#include ../examples/traces/trace.xqm:values}}
```

`$trace:kinds` maps the number OTLP writes for a span's kind to its
name.

## A record for a span

Each span becomes a record, a map with a fixed set of fields, each of
a declared type. A record type is declared once and named:

`trace.xqm` (the span):

```xquery
{{#include ../examples/traces/trace.xqm:span}}
```

The declaration also declares a function of the same name that makes
such a record from its fields, checking each one against its type.
That catches mistakes early. JSON numbers are doubles, and a double is
not an integer, even when it is 2:

```console
$ sxml xquery --method=adaptive --expression='
    declare record span(id as xs:string, kind as xs:integer);
    let $first := json-doc("before.json")?resourceSpans?1?scopeSpans?1?spans?1
    return span($first?spanId, $first?kind)'
expression:4:12: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

`xs:integer($first?kind)` converts it. Here is the function that
flattens a whole file, with the fields given by name:

`trace.xqm` (the spans):

```xquery
{{#include ../examples/traces/trace.xqm:spans}}
```

The service's name is found once per resource, and each of its spans
gets it. The first client span is the SDK's call, the root of the
first trace; it has no parent, so its `parent` field is the empty
sequence, which the adaptive output writes as `()`:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    trace:spans(json-doc("before.json"))[?kind = "client"][1]'
{"trace":"48114dcf6840d1bd46460c4b1ce0d09d","id":"edba4075f74ba88f","parent":(),"service":"storefront","name":"GET /api/getFlags","kind":"client","start":1792573200000000000,"end":1792573200050000000,"attributes":{"http.request.method":"GET","server.address":"vane","server.port":8080,"url.full":"http://vane:8080/api/getFlags?input=%7B%7D","url.template":"/api/getFlags","http.response.status_code":200}}
```

## A map from span id to span

A span names its parent only by id. To find the parent of each span
quickly, build a map from id to span once; `map:build` takes the items
and a function that gives each item's key. Which function of the
service made each database call?

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    let $spans := trace:spans(json-doc("before.json"))
    let $by-id := map:build($spans, fn { ?id })
    for $query in $spans[map:contains(?attributes, "db.system.name")]
    let $caller := $by-id($query?parent)
    return `{ $query?name } on { $query?attributes?("server.address") }, called by { $caller?service } in { $caller?name }`'
"SELECT flags on postgresql, called by vane in registry.getFlags"
"SELECT flags on postgresql, called by vane in registry.getFlag"
"UPDATE flags on postgresql, called by vane in registry.setPercentage"
"GET on redis, called by vane in cache.getFlag"
```

Every query has the same caller, the one Vane service; the parent says
more: the module function that made the query, recorded as an internal
span.

Each lookup takes the same short time however many spans there are.
Searching the spans for the parent instead, `$spans[?id =
$query?parent]`, reads all of them for each query; [Performance and
Memory](performance.md) measures the difference on a large file.

## Times from nanosecond strings

The times are integers of nanoseconds since 1970, written as strings,
and `xs:integer` reads them exactly. Subtracting two gives a duration
in nanoseconds; dividing by a million gives milliseconds, an
`xs:decimal`, exact too. For dates people read, a number of
milliseconds multiplied by a one-millisecond `xs:dayTimeDuration` and
added to 1970 gives an `xs:dateTime`:

`trace.xqm` (the times):

```xquery
{{#include ../examples/traces/trace.xqm:times}}
```

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    let $root := trace:spans(json-doc("before.json"))[1]
    return (trace:date-time($root?start), trace:milliseconds($root?end - $root?start))'
xs:dateTime("2026-10-21T09:00:00Z")
50
```

The first request started at nine in the morning on 21 October 2026 and
took 50 milliseconds.

## The tree from the parent ids

The opposite direction, from a span to its children, is a map too:
from a parent id to the spans that name it, which `map:build` makes by
collecting all the items with the same key. Then a function makes a
`span` element for a span, and calls itself for each of its children:

`trace.xqm` (the tree):

```xquery
{{#include ../examples/traces/trace.xqm:tree}}
```

The spans without a parent are the roots, one per trace. Each `trace`
element has its route, the name of its root span, and its start; each
span its start relative to the start of the trace and its duration,
both in milliseconds, and, for a call, `peer`, the server it called,
and, for a database call, `db`, the kind of database.
A function that calls itself is fine here: a trace is a few levels
deep.

## The normal form

`trace:load` puts it together: the services of a file, then its traces.

`trace.xqm` (loading a file):

```xquery
{{#include ../examples/traces/trace.xqm:load}}
```

`normalize.xq`:

```xquery
{{#include ../examples/traces/normalize.xq}}
```

```console
$ sxml xquery --param=file=before.json normalize.xq
<traces source="before.json">
  <service name="storefront" version="5.2.0"/>
  <service name="vane" version="2.3.0"/>
  <service name="vane-console" version="1.8.0"/>
  <trace id="48114dcf6840d1bd46460c4b1ce0d09d" route="GET /api/getFlags" start="2026-10-21T09:00:00Z">
    <span id="edba4075f74ba88f" service="storefront" name="GET /api/getFlags" kind="client" start="0" duration="50" peer="vane">
      <span id="1927a6f46640ec9e" service="vane" name="GET /api/getFlags" kind="server" start="1" duration="48">
        <span id="877ed3a0b72c92c6" service="vane" name="flags.listFlags" kind="internal" start="2" duration="46">
          <span id="448683a55e6803a1" service="vane" name="registry.getFlags" kind="internal" start="3" duration="43">
            <span id="fb73f92d181aced2" service="vane" name="SELECT flags" kind="client" start="5" duration="39" peer="postgresql" db="postgresql"/>
          </span>
        </span>
      </span>
    </span>
  </trace>
  <trace id="aaab30cfb7723ae45a23d581f962cc40" route="POST /api/killFlag" start="2026-10-21T09:00:02Z">
    <span id="53dc1509f5cf3ba5" service="vane-console" name="POST /api/killFlag" kind="client" start="0" duration="34" peer="vane.example.com">
      <span id="f9206f822272e040" service="vane" name="POST /api/killFlag" kind="server" start="1" duration="32">
        <span id="900884cd9fed676d" service="vane" name="flags.killFlag" kind="internal" start="2" duration="30">
          <span id="5fdc37e904085e22" service="vane" name="cache.getFlag" kind="internal" start="3" duration="3">
            <span id="0e7b40a994b31e11" service="vane" name="GET" kind="client" start="4" duration="1" peer="redis" db="redis"/>
          </span>
          <span id="f82be3eb4c2b1180" service="vane" name="registry.getFlag" kind="internal" start="7" duration="9">
            <span id="2f9fc0a36c747d78" service="vane" name="SELECT flags" kind="client" start="8" duration="7" peer="postgresql" db="postgresql"/>
          </span>
          <span id="03d74407bc202a98" service="vane" name="registry.setPercentage" kind="internal" start="17" duration="13">
            <span id="bc2ed987977fa5c6" service="vane" name="UPDATE flags" kind="client" start="18" duration="11" peer="postgresql" db="postgresql"/>
          </span>
        </span>
      </span>
    </span>
  </trace>
</traces>
```

Now the shape of each request is plain to see. Listing the flags is a
chain from the SDK's call through the service's server span and two
module functions, `flags.listFlags` and `registry.getFlags`, down to
one PostgreSQL query; killing a flag reads the flag from Redis in the
cache module, reads it again from PostgreSQL in the registry module,
and sets its percentage to 0. The new version's file shows the change,
in the children of `flags.listFlags`:

```console
$ sxml xquery --param=file=after.json --output=after.xml normalize.xq
$ sxml xpath --context-item=after.xml '//span[@name = "flags.listFlags"]/span ! `{@start} ms: {@name}, {@duration} ms`'
"3 ms: registry.getFlagKeys, 12 ms"
"16 ms: registry.getFlag, 20 ms"
"38 ms: registry.getFlag, 20 ms"
"60 ms: registry.getFlag, 20 ms"
"82 ms: registry.getFlag, 20 ms"
```

One call of the registry module for the keys, then one for each of
the four flags, one after the other, each with a query of its own: the
request now takes more than twice as long. This XML,
one `traces` element with its services and a tree of spans for each
trace, is the *normal form* of a trace file, and the rest of the
project works on it: ordinary paths and patterns find spans, their
ancestors and their children, and XSLT can stream it.
