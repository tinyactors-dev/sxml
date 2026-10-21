# Spans as a Tree

<!-- examples: traces -->

An OTLP file is grouped the way exporters send spans: by the service
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
"api-gateway, @opentelemetry/instrumentation-http: 4 spans"
"flag-service, @opentelemetry/instrumentation-http: 5 spans"
"flag-service, @opentelemetry/instrumentation-redis: 1 spans"
"flag-registry, @opentelemetry/instrumentation-http: 3 spans"
"flag-registry, @opentelemetry/instrumentation-pg: 3 spans"
```

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
gets it. The first client span:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    trace:spans(json-doc("before.json"))[?kind = "client"][1]'
{"trace":"48114dcf6840d1bd46460c4b1ce0d09d","id":"f1952a06701f65f9","parent":"9db4d10ecccc96f0","service":"api-gateway","name":"GET /flags","kind":"client","start":1792573200001000000,"end":1792573200049000000,"attributes":{"http.request.method":"GET","server.address":"flag-service","server.port":80,"url.full":"http://flag-service/flags","url.template":"/flags","http.response.status_code":200}}
```

## A map from span id to span

A span names its parent only by id. To find the parent of each span
quickly, build a map from id to span once; `map:build` takes the items
and a function that gives each item's key. Which service called each
database?

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace trace = "urn:example:trace" at "trace.xqm";
    let $spans := trace:spans(json-doc("before.json"))
    let $by-id := map:build($spans, fn { ?id })
    for $query in $spans[map:contains(?attributes, "db.system.name")]
    let $caller := $by-id($query?parent)
    return `{ $query?name } on { $query?attributes?("server.address") }, called by { $caller?service } in { $caller?name }`'
"GET on flag-cache, called by flag-service in POST /flags/{key}/kill"
"SELECT flags on postgresql, called by flag-registry in GET /flags"
"SELECT flags on postgresql, called by flag-registry in GET /flags/{key}"
"UPDATE flags on postgresql, called by flag-registry in PUT /flags/{key}/percentage"
```

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
both in milliseconds, and, for a call, `peer`, the server it called.
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
  <service name="api-gateway" version="1.4.0"/>
  <service name="flag-service" version="2.3.0"/>
  <service name="flag-registry" version="1.9.2"/>
  <trace id="48114dcf6840d1bd46460c4b1ce0d09d" route="GET /flags" start="2026-10-21T09:00:00Z">
    <span id="9db4d10ecccc96f0" service="api-gateway" name="GET /flags" kind="server" start="0" duration="50">
      <span id="f1952a06701f65f9" service="api-gateway" name="GET /flags" kind="client" start="1" duration="48" peer="flag-service">
        <span id="450bd70b42ff202b" service="flag-service" name="GET /flags" kind="server" start="2" duration="46">
          <span id="9507aae4931c1a17" service="flag-service" name="GET /flags" kind="client" start="4" duration="41" peer="flag-registry">
            <span id="2b3938e2ad2889ea" service="flag-registry" name="GET /flags" kind="server" start="5" duration="39">
              <span id="bc26eff93c7d3107" service="flag-registry" name="SELECT flags" kind="client" start="7" duration="34" peer="postgresql"/>
            </span>
          </span>
        </span>
      </span>
    </span>
  </trace>
  <trace id="aaab30cfb7723ae45a23d581f962cc40" route="POST /flags/{key}/kill" start="2026-10-21T09:00:02Z">
    <span id="c377adc01acf7ff5" service="api-gateway" name="POST /flags/{key}/kill" kind="server" start="0" duration="34">
      <span id="595ed12c603e8700" service="api-gateway" name="POST /flags/{key}/kill" kind="client" start="1" duration="32" peer="flag-service">
        <span id="a4c5b3440594b922" service="flag-service" name="POST /flags/{key}/kill" kind="server" start="2" duration="30">
          <span id="ecb5733260ceb4b2" service="flag-service" name="GET" kind="client" start="3" duration="2" peer="flag-cache"/>
          <span id="8662f3310d0158f0" service="flag-service" name="GET /flags/{key}" kind="client" start="6" duration="10" peer="flag-registry">
            <span id="0cb733ec39d63363" service="flag-registry" name="GET /flags/{key}" kind="server" start="7" duration="8">
              <span id="1822add2fe4ca68d" service="flag-registry" name="SELECT flags" kind="client" start="8" duration="6" peer="postgresql"/>
            </span>
          </span>
          <span id="9cf360297e503c8d" service="flag-service" name="PUT /flags/{key}/percentage" kind="client" start="17" duration="14" peer="flag-registry">
            <span id="b71d3117a25377cd" service="flag-registry" name="PUT /flags/{key}/percentage" kind="server" start="18" duration="12">
              <span id="6bd326bf36cab938" service="flag-registry" name="UPDATE flags" kind="client" start="19" duration="10" peer="postgresql"/>
            </span>
          </span>
        </span>
      </span>
    </span>
  </trace>
</traces>
```

Now the shape of each request is plain to see. Listing the flags is a
chain of single calls from the gateway down to PostgreSQL; killing a
flag reads the cache, reads the flag from the registry, and sets its
percentage to 0. The new version's file shows the change:

```console
$ sxml xquery --param=file=after.json --output=after.xml normalize.xq
$ sxml xpath --context-item=after.xml '//trace[@route = "GET /flags"]//span[@service = "flag-service"][@kind = "client"] ! `{@start} ms: {@name}, {@duration} ms`'
"4 ms: GET /flag-keys, 12 ms"
"18 ms: GET /flags/{key}, 20 ms"
"40 ms: GET /flags/{key}, 20 ms"
"62 ms: GET /flags/{key}, 20 ms"
"84 ms: GET /flags/{key}, 20 ms"
```

One call for the keys, then one for each of the four flags, one after
the other: the request now takes more than twice as long. This XML,
one `traces` element with its services and a tree of spans for each
trace, is the *normal form* of a trace file, and the rest of the
project works on it: ordinary paths and patterns find spans, their
ancestors and their children, and XSLT can stream it.
