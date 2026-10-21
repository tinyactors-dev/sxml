# Streaming Large Trace Files

<!-- examples: traces -->

Two traces fit anywhere. A collector that keeps a day of a busy system
writes millions of spans, and a tree of them takes several times the
memory of the file. Most XML processors read the whole input into a
tree before they transform it; for an archive of several gigabytes, that
does not work.

XSLT can *stream*: process the input as it is parsed, one piece at a
time, and never build the tree. You ask for it by declaring a mode
`streamable="yes"`. The processor then checks that every rule in that
mode can work in one pass over the input, from start to end, and runs
it that way. The normal form of [Spans as a Tree](traces-tree.md) suits
streaming well: each trace is one element, with its spans inside it.

## A large input

`make-traces.xq` writes an archive in the normal form, with as many
made-up requests to the old flag service as `$count` says, one every
100 milliseconds, two listings of the flags for each kill, and
database queries that take between 5 and 65 milliseconds:

`make-traces.xq`:

```xquery
{{#include ../examples/traces/make-traces.xq}}
```

```console
$ sxml xquery --param=count=10000 --output=archive.xml make-traces.xq
$ head -c 300 archive.xml
<traces source="make-traces.xq"><service name="api-gateway" version="1.4.0"/><service name="flag-service" version="2.3.0"/><service name="flag-registry" version="1.9.2"/><trace id="1" route="GET /flags" start="2026-10-21T09:00:00Z"><span id="1.1" service="api-gateway" name="GET /flags" kind="server"
```

## A streaming stylesheet

`spans.xsl`:

```xml
{{#include ../examples/traces/spans.xsl}}
```

```console
$ sxml transform spans.xsl archive.xml
63,333
```

It looks like any other stylesheet, but `sxml transform` read the
traces as they were parsed and kept only the count. With 100,000 traces
(an 82 MB file), a release build of sxml uses 11 MB of memory at its
peak, the same as for the 8 MB file here. Without `streamable="yes"`,
the same stylesheet builds the tree and uses about 390 MB.

## What can stream

In one pass, the processor sees each node once, as it goes by. A rule
in a streamable mode can do anything with the node it matched and the
attributes and ancestors of that node, but it can go down into the
children only once. This rule goes down twice, once to count the traces
and once to count their spans:

`counts.xsl`:

```xml
{{#include ../examples/traces/counts.xsl}}
```

```console
$ sxml transform counts.xsl archive.xml
counts.xsl:6:3: err:XTSE3430: a template rule of a streamable mode is not guaranteed-streamable (XSLT 4.0, 6.6.4): its body is free-ranging
```

The stylesheet is rejected before anything runs, with the error the
XSLT specification gives. The rules that decide what is *guaranteed
streamable* are long, but the idea is short: one downward pass, no
looking back and no looking ahead. To compute several things in one
pass, use accumulators, `xsl:iterate` or `xsl:fork`.

## Accumulators

An *accumulator* is a value that the processor updates as it goes
through the document. Each `xsl:accumulator-rule` matches nodes and
computes the new value from the old one, `$value`.
`accumulator-before` and `accumulator-after` read the value at a node,
before and after its content.

The participants of [Who Calls Whom, and How Often](traces-cardinality.md)
come from client spans, and the callee is the service of the client's
child server span, if there is one. In a stream that child is still
ahead when the client span starts, so this accumulator uses what is at
hand, the client span's `peer`, and counts the calls in a map:

`calls.xsl`:

```xml
{{#include ../examples/traces/calls.xsl}}
```

```console
$ sxml transform calls.xsl archive.xml
api-gateway -> flag-service: 10000
flag-registry -> postgresql: 10000
flag-service -> flag-cache: 3333
flag-service -> flag-registry: 10000
```

The empty rule for `trace` makes `xsl:apply-templates` read each trace
and do nothing with it but let the accumulator see its spans; after
that, `accumulator-after` on the `traces` element has the final map.
These are the counts of the old version, from ten thousand requests.

Several accumulators are computed in the same single pass. One counts
the traces, and one keeps the slowest database query so far, as a map
of its id and duration:

`slowest.xsl`:

```xml
{{#include ../examples/traces/slowest.xsl}}
```

```console
$ sxml transform slowest.xsl archive.xml
2500 traces read
5000 traces read
7500 traces read
10000 traces read
10000 traces, the slowest query 50.6, 65 ms
```

The progress lines are `xsl:message` output, which goes to standard
error. Both stylesheets stream: with the 82 MB archive, each uses 11 MB.

## Iterating

`xsl:iterate` is a loop over a sequence that carries parameters from one
item to the next, and that can stop early with `xsl:break`. Which is the
first request that took longer than a threshold, and how many were
faster before it?

`first-slow.xsl`:

```xml
{{#include ../examples/traces/first-slow.xsl}}
```

```console
$ sxml transform first-slow.xsl archive.xml
GET /flags at 2026-10-21T09:00:04.9Z took 81 ms, after 49 faster requests.
$ sxml transform --param=threshold=100 first-slow.xsl archive.xml
No request took longer than 100 ms.
```

`xsl:on-completion` gives the result when the loop reaches the end of
the sequence without a break. The rule is guaranteed streamable, so
the mode accepts it, and the loop runs on each trace as it is parsed,
carrying only its parameters: with the 82 MB archive it uses 11 MB.
After `xsl:break`, nothing more is kept, but sxml still parses the rest
of the file, so stopping early does not make it faster.

The threshold is declared `as="xs:decimal"`, so the value given on the
command line, which is text, is converted to a decimal before the
stylesheet runs; a value that is not a number is an error before
anything is read:

```console
$ sxml transform --param=threshold=fast first-slow.xsl archive.xml
first-slow.xsl: err:XTTE0590: The value does not match the required type of the parameter (XSLT 4.0, section 9.3)
```

## Grouping neighbours

`xsl:for-each-group` streams too, when the groups are runs of
neighbours: `group-adjacent`, `group-starting-with` and
`group-ending-with`. The archive is in the order of time, so the
requests of each second are neighbours, and this stylesheet makes one
line per second:

`seconds.xsl`:

```xml
{{#include ../examples/traces/seconds.xsl}}
```

```console
$ sxml transform seconds.xsl archive.xml | head -n 4
2026-10-21T09:00:00: 10 requests, the slowest 77 ms
2026-10-21T09:00:01: 10 requests, the slowest 78 ms
2026-10-21T09:00:02: 10 requests, the slowest 79 ms
2026-10-21T09:00:03: 10 requests, the slowest 80 ms
```

The processor holds the group that is forming, the requests of one
second, and hands it to the body when the next second begins; it never
holds more. `current-group()` is a stream too, and can be read only
once, so the body copies it with `copy-of`, a small tree, and reads the
copy twice, to count the requests and to find the slowest. With the
82 MB archive, the stylesheet uses 10 MB. `group-by` is different: its
groups collect members from anywhere in the input, so in a streamable
mode it is rejected with `err:XTSE3430`, as `counts.xsl` was.

## More streaming

`xsl:source-document streamable="yes"` streams a document read by URI,
from any instruction, so a stylesheet can stream files that are not its
principal input. Inside it, `xsl:fork` runs several downward passes over
the same stream at once, which is how the two counts that `counts.xsl`
could not make are made after all:

`fork.xsl`:

```xml
{{#include ../examples/traces/fork.xsl}}
```

```console
$ sxml transform fork.xsl
10000 63333
```

`sxml transform` without an input calls the initial template, here the
template named `xsl:initial-template`. With the 82 MB archive
(`--param=archive=…`), it uses 11 MB.

`xsl:merge` combines several inputs that are each sorted by a key into
one sequence in the order of that key, reading each input as a stream.
Two collectors that each wrote an archive in the order of time make one
timeline. The second archive here starts 50 milliseconds after the
first:

```console
$ sxml xquery --param=count=10000 --param=start=2026-10-21T09:00:00.05Z --output=archive-2.xml make-traces.xq
```

`merge.xsl`:

```xml
{{#include ../examples/traces/merge.xsl}}
```

```console
$ sxml transform merge.xsl | head -n 4
2026-10-21T09:00:00Z first GET /flags
2026-10-21T09:00:00.05Z second GET /flags
2026-10-21T09:00:00.1Z first GET /flags
2026-10-21T09:00:00.15Z second GET /flags
```

Each group of the merge holds the items of all sources with the same
key; `current-merge-group('first')` holds the ones from the source named
`first`. [Performance and Memory](performance.md) has more on the memory
sxml uses.

## Summary

The trace project took OTLP JSON apart and put it together again:

- `for member`, records and `map:build` flatten the nested arrays of an
  OTLP file and rebuild each trace's tree from parent ids;
- `group by` counts the calls between services, and windows find runs,
  requests from start to end, and bursts in the order of the spans;
- the diff of two maps of counts shows the participants that came and
  went and the calls whose cardinality changed, as text, JSON, an HTML
  report and an SVG diagram;
- streamable modes, accumulators, `xsl:iterate`, adjacent groups,
  `xsl:fork` and `xsl:merge` read archives of any size in constant
  memory.
