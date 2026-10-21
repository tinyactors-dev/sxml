# Project: Comparing Traces Before and After a Change

<!-- examples: traces -->

The Vane service of [Project: A Diff of Call Chains](call-chains.md)
changed: listing the flags used to read them from PostgreSQL with one
query, and now it reads the flag keys and then each flag; the kill
switch no longer reads the flag from Redis, and now writes an audit
event and calls the owners' webhook. The diff of the call chains
predicted what that does at run time. In this project we look at what
the service actually did, before and after the change, from the traces
it recorded.

The examples are in `docs/book/examples/traces`. Run every command of
this project from that directory.

## Traces, spans, resources and scopes

When a request goes through a system, each program on its way records
what it did as *spans*. A span is one operation: it has a name, a start
and an end, and a *kind*. A *server* span is a program handling a
request; a *client* span is a program calling something else, another
program, a database or a webhook; an *internal* span is work inside a
program, such as one function of a module. Every span but the first
names a *parent*, the span that caused it, and all the spans of one
request share a *trace id*: together they are a *trace*, a tree of
spans.

OpenTelemetry is the common standard for recording traces, and OTLP,
the OpenTelemetry Protocol, is how programs send their spans to a
collector. OTLP has a JSON encoding, and collectors can write the
spans they receive to files in it. Each file holds an
`ExportTraceServiceRequest`: a list of *resources*, each the program
that recorded the spans (OpenTelemetry calls it a service: its
`service.name` and `service.version`), with a list of *scopes*, each
the instrumentation library that made the spans (one for HTTP, one for
PostgreSQL, one for Redis, one for the functions of the program
itself), with the spans:

`before.json` (the start):

```json
{{#include ../examples/traces/before.json:1:28}}
```

The resource is `storefront`, an application that uses Vane's backend
SDK, `@vane/node`, and this is the span the SDK recorded when it
downloaded the flags: a call `GET /api/getFlags` to the Vane service,
of kind 3 (client), from the time `startTimeUnixNano` to
`endTimeUnixNano`, in nanoseconds since 1970. Its attributes follow
OpenTelemetry's semantic conventions, the agreed names such as
`http.request.method`, `server.address` and, on the database spans,
`db.system.name` and `db.query.text`. The ids are hexadecimal strings,
and the times and the integer attributes (`intValue`) are strings too;
[JSON, CSV and HTML](formats.md) says why.

## The two files

`before.json` was recorded with the old version of the Vane service,
2.3.0, `after.json` with the new one, 2.4.0. Each holds two traces,
one of each remote function the change touched: the backend SDK
listing the flags (`getFlags`), and an owner killing the flag
`new-checkout` in the console (`killFlag`). A first look:

```console
$ sxml xpath '("before.json", "after.json") ! `{.}: {count(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*)} spans of {count(distinct-values(json-doc(.)?resourceSpans?*?scopeSpans?*?spans?*?traceId))} traces in {string-join(json-doc(.)?resourceSpans?*?resource?attributes?*[?key = "service.name"]?value?stringValue, ", ")}`'
"before.json: 14 spans of 2 traces in storefront, vane, vane-console"
"after.json: 23 spans of 2 traces in storefront, vane, vane-console"
```

Nine spans more, and the same three programs: the console, the
storefront with its SDK, and the service. Which spans, and why?

## The plan

The project builds a small tool in XQuery and XSLT that answers that
question for any two trace files:

- [Spans as a Tree](traces-tree.md) reads OTLP JSON into records,
  rebuilds each trace's tree from the parent ids, and writes it as XML:
  the normal form the rest of the project uses.
- [Who Calls Whom, and How Often](traces-cardinality.md) finds the
  participants of each call, counts the calls per request, and uses
  windows to find runs of the same call.
- [The Trace Diff](traces-diff.md) compares the counts of the two files,
  and the versions of the programs, as text and as JSON.
- [Rendering the Diff](traces-rendering.md) writes the diff as an HTML
  report and draws it as a diagram.
- [Streaming Large Trace Files](traces-streaming.md) handles a trace
  archive too large to load, with streaming XSLT.

All the functions live in one library module, `trace.xqm`, whose parts
each section explains.
