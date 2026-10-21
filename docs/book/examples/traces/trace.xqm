(: Reading OTLP JSON traces into a tree of spans, and the calls between
   services that the tree shows. :)
module namespace trace = "urn:example:trace";

(: ANCHOR: span :)
(: One span, flattened out of its resource and scope. Times are in
   nanoseconds since 1970; attributes map each key to its value. :)
declare record trace:span(
  trace as xs:string,
  id as xs:string,
  parent as xs:string?,
  service as xs:string,
  name as xs:string,
  kind as xs:string,
  start as xs:integer,
  end as xs:integer,
  attributes as map(xs:string, xs:anyAtomicType)
);
(: ANCHOR_END: span :)

(: ANCHOR: values :)
(: The kinds of span, by the number OTLP writes. :)
declare variable $trace:kinds := {
  0: "unspecified", 1: "internal", 2: "server", 3: "client",
  4: "producer", 5: "consumer"
};

(: The value of an OTLP AnyValue: { "stringValue": "GET" },
   { "intValue": "200" } (an int64, written as a string), ... :)
declare function trace:value($value as map(*)) as xs:anyAtomicType? {
  if (map:contains($value, "stringValue")) then $value?stringValue
  else if (map:contains($value, "intValue")) then xs:integer($value?intValue)
  else if (map:contains($value, "doubleValue")) then xs:double($value?doubleValue)
  else if (map:contains($value, "boolValue")) then $value?boolValue
  else ()
};

(: OTLP attributes, a list of key-value pairs, as a map. :)
declare function trace:attributes($attributes as array(*)?) as map(xs:string, xs:anyAtomicType) {
  map:merge($attributes?* ! { ?key: trace:value(?value) })
};
(: ANCHOR_END: values :)

(: ANCHOR: spans :)
(: Every span of an ExportTraceServiceRequest, flattened. :)
declare function trace:spans($request as map(*)) as trace:span* {
  for member $resource in $request?resourceSpans
  let $service := trace:attributes($resource?resource?attributes)?("service.name")
  for member $scope in $resource?scopeSpans
  for member $span in $scope?spans
  return trace:span(
    trace := $span?traceId,
    id := $span?spanId,
    parent := $span?parentSpanId[. ne ""],
    service := $service,
    name := $span?name,
    kind := $trace:kinds(xs:integer($span?kind)),
    start := xs:integer($span?startTimeUnixNano),
    end := xs:integer($span?endTimeUnixNano),
    attributes := trace:attributes($span?attributes)
  )
};
(: ANCHOR_END: spans :)

(: ANCHOR: times :)
(: Nanoseconds as milliseconds. :)
declare function trace:milliseconds($nanoseconds as xs:integer) as xs:decimal {
  $nanoseconds div 1000000
};

(: Nanoseconds since 1970 as a date and time, to the millisecond. :)
declare function trace:date-time($nanoseconds as xs:integer) as xs:dateTime {
  xs:dateTime("1970-01-01T00:00:00Z")
    + xs:dayTimeDuration("PT0.001S") * ($nanoseconds idiv 1000000)
};
(: ANCHOR_END: times :)

(: ANCHOR: tree :)
(: A span and, inside it, its children in the order they started.
   $children maps a span id to the spans whose parent it is; $origin
   is the start of the trace. :)
declare function trace:element(
  $span as trace:span,
  $children as map(xs:string, trace:span*),
  $origin as xs:integer
) as element(span) {
  <span id="{ $span?id }" service="{ $span?service }" name="{ $span?name }"
        kind="{ $span?kind }"
        start="{ trace:milliseconds($span?start - $origin) }"
        duration="{ trace:milliseconds($span?end - $span?start) }">{
    $span?attributes?("server.address") ! attribute peer { . },
    for $child in $children($span?id)
    order by $child?start
    return trace:element($child, $children, $origin)
  }</span>
};

(: The spans of each trace as a tree, from the parent ids. :)
declare function trace:tree($spans as trace:span*) as element(trace)* {
  let $children := map:build($spans[exists(?parent)], fn { ?parent })
  for $root in $spans[empty(?parent)]
  order by $root?start
  return <trace id="{ $root?trace }" route="{ $root?name }"
                start="{ trace:date-time($root?start) }">{
    trace:element($root, $children, $root?start)
  }</trace>
};
(: ANCHOR_END: tree :)

(: ANCHOR: load :)
(: A file of OTLP JSON in the normal form: its services, then its traces. :)
declare function trace:load($file as xs:string) as element(traces) {
  let $request := json-doc($file)
  return <traces source="{ $file }">{
    for member $resource in $request?resourceSpans
    let $attributes := trace:attributes($resource?resource?attributes)
    return <service name="{ $attributes?("service.name") }"
                    version="{ $attributes?("service.version") }"/>,
    trace:tree(trace:spans($request))
  }</traces>
};
(: ANCHOR_END: load :)

(: ANCHOR: calls :)
(: A call from one service to another, made in a trace of one route. :)
declare record trace:call(route as xs:string, caller as xs:string, callee as xs:string);

(: Who a client span calls: the service of the server span it caused,
   else the server it names (a database has no spans of its own). :)
declare function trace:callee($client as element(span)) as xs:string {
  string(($client/span[@kind = "server"]/@service, $client/@peer)[1])
};

(: Every call of the traces, one for each client span. :)
declare function trace:calls($traces as element(traces)) as trace:call* {
  for $client in $traces//span[@kind = "client"]
  return trace:call(
    route := string($client/ancestor::trace/@route),
    caller := string($client/@service),
    callee := trace:callee($client)
  )
};
(: ANCHOR_END: calls :)

(: ANCHOR: cardinality :)
(: How often each service calls another in one request of a route: a
   map from "route: caller -> callee" to the call and its count. :)
declare record trace:edge(
  route as xs:string, caller as xs:string, callee as xs:string,
  count as xs:decimal
);

declare function trace:cardinality($traces as element(traces)) as map(xs:string, trace:edge) {
  map:merge(
    for $call in trace:calls($traces)
    group by $route := $call?route, $caller := $call?caller, $callee := $call?callee
    let $requests := count($traces/trace[@route = $route])
    return {
      `{ $route }: { $caller } -> { $callee }`:
        trace:edge($route, $caller, $callee, count($call) div $requests)
    }
  )
};
(: ANCHOR_END: cardinality :)

(: ANCHOR: diff :)
(: A change in how often one service calls another, per request. :)
declare record trace:change(
  route as xs:string, caller as xs:string, callee as xs:string,
  before as xs:decimal, after as xs:decimal
);

(: The calls whose count changed, in the routes both files recorded. :)
declare function trace:diff($before as element(traces), $after as element(traces)) as trace:change* {
  let $routes := distinct-values($before/trace/@route[. = $after/trace/@route])
  return if (empty($routes)) then error(
    xs:QName("trace:no-shared-route"),
    `{ $before/@source } and { $after/@source } have no route in common`
  ) else
    let $old := trace:cardinality($before)
    let $new := trace:cardinality($after)
    for $key in distinct-values((map:keys($old), map:keys($new)))
    let $edge := ($old($key), $new($key))[1]
    where $edge?route = $routes
    let $count-before := $old($key)?count otherwise 0
    let $count-after := $new($key)?count otherwise 0
    where $count-before ne $count-after
    order by $key
    return trace:change(
      $edge?route, $edge?caller, $edge?callee, $count-before, $count-after)
};
(: ANCHOR_END: diff :)

(: ANCHOR: versions :)
(: A service whose version changed, or that only one file has. :)
declare record trace:version(service as xs:string, before as xs:string?, after as xs:string?);

declare function trace:versions($before as element(traces), $after as element(traces)) as trace:version* {
  for $service in distinct-values(($before, $after)/service/@name)
  let $old := $before/service[@name = $service]/@version ! string()
  let $new := $after/service[@name = $service]/@version ! string()
  where not(deep-equal($old, $new))
  return trace:version($service, $old, $new)
};
(: ANCHOR_END: versions :)
