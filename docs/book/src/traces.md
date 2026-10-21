# Project: Comparing Traces Before and After a Change

<!-- examples: traces -->

The flag service of [Project: A Diff of Call Chains](call-chains.md)
changed: listing the flags used to ask the flag registry once, and now
it asks for the flag keys and then for each flag; the kill switch no
longer reads the flag cache and now writes an audit event. The diff of the
call chains predicted what that does at run time. In this project we
look at what the services actually did, before and after the change,
from the traces they recorded.

The examples are in `docs/book/examples/traces`. Run every command of
this project from that directory.

## Traces, spans, resources and scopes

When a request goes through a system of services, each service records
what it did as *spans*. A span is one operation: it has a name, a start
and an end, and a *kind*. A *server* span is a service handling a
request; a *client* span is a service calling something else, another
service or a database. Every span but the first names a *parent*, the
span that caused it, and all the spans of one request share a *trace
id*: together they are a *trace*, a tree of spans.

OpenTelemetry is the common standard for recording traces, and OTLP,
the OpenTelemetry Protocol, is how services send their spans to a
collector. OTLP has a JSON encoding, and collectors can write the
spans they receive to files in it. Each file holds an
`ExportTraceServiceRequest`: a list of *resources*, each the service
that recorded the spans (its `service.name` and `service.version`),
with a list of *scopes*, each the instrumentation library that made the
spans (one for HTTP, one for PostgreSQL, one for Redis), with the spans:

`before.json` (the start):

```json
{{#include ../examples/traces/before.json:1:28}}
```

This is the first span the API gateway recorded: it handled a request
`GET /flags`, of kind 2 (server), from the time `startTimeUnixNano` to
`endTimeUnixNano`, in nanoseconds since 1970. Its attributes follow
OpenTelemetry's semantic conventions, the agreed names such as
`http.request.method`, `http.route` and, on the database spans,
`db.system.name` and `db.query.text`. The ids are hexadecimal strings,
and the times and the integer attributes (`intValue`) are strings too;
[JSON, CSV and HTML](formats.md) says why.

## The two files

`before.json` was recorded with the old version of the flag service,
`after.json` with the new one. Each holds two traces, one of each kind
of request the gateway handled: listing the flags, and killing the
flag `new-checkout`. A first look:

```console
$ sxml xpath '("before.json", "after.json") ! `{.}: {count(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*)} spans of {count(distinct-values(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*?traceId))} traces in {string-join(json-doc(.)?resourceSpans?*?resource?attributes?*[?key = "service.name"]?value?stringValue, ", ")}`'
"before.json: 16 spans of 2 traces in api-gateway, flag-service, flag-registry"
"after.json: 30 spans of 2 traces in api-gateway, flag-service, flag-registry, audit-service"
```

Fourteen spans more, and a new service. Which spans, and why?

## The plan

The project builds a small tool in XQuery and XSLT that answers that
question for any two trace files:

- [Spans as a Tree](traces-tree.md) reads OTLP JSON into records,
  rebuilds each trace's tree from the parent ids, and writes it as XML:
  the normal form the rest of the project uses.
- [Who Calls Whom, and How Often](traces-cardinality.md) finds the
  services that take part in each call, counts the calls per request,
  and uses windows to find runs of the same call.
- [The Trace Diff](traces-diff.md) compares the counts of the two files,
  as text and as JSON.
- [Rendering the Diff](traces-rendering.md) writes the diff as an HTML
  report and draws it as a diagram.
- [Streaming Large Trace Files](traces-streaming.md) handles a trace
  archive too large to load, with streaming XSLT.

All the functions live in one library module, `trace.xqm`, whose parts
each section explains.
