# Documents, Parameters and the Context

<!-- examples: spec -->

A query gets its input in three ways: the *context item*, which paths
start from; documents and other files that it reads by name; and
*external variables*, the parameters of the whole query. This section
shows each of them with the `sxml` command, on both versions of the
draft.

## The context item

`--context-item=FILE` parses a document and makes it the context item,
so that a path such as `//t` starts from it. A file name of `-` reads
the document from standard input:

```console
$ sxml xquery --context-item=- --expression='count(//t[@anchor])' < vane.xml
11
```

A query can say what context item it expects, with `declare context
value`. Then a wrong document is an error at the start, instead of an
empty result later. Here the schema of the draft is given where a draft
was meant:

```console
$ sxml xquery --context-item=vane.xsd --expression='
    declare context value as document-node(element(rfc)) external;
    count(//t[@anchor])'
expression: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

A declaration can also give a default, used when no context item is
given:

```console
$ sxml xquery --expression='
    declare context value as document-node(element(rfc)) external := doc("vane-00.xml");
    count(//t[@anchor])'
8
```

(XQuery 3.1 wrote this `declare context item`, which still works.)

## Reading documents

`doc` reads an XML document by its URI. With the `sxml` command, a
relative URI names a file relative to the working directory. A query
can read as many documents as it likes; reading the same URI twice gives
the same document, not a copy:

```console
$ sxml xquery --method=adaptive --expression='doc("vane.xml") is doc("vane.xml")'
true()
```

`doc-available` asks whether a document can be read, without raising
an error when it can't:

```console
$ sxml xquery --method=adaptive --expression='
    for $file in ("vane.xml", "vane-00.xml", "vane-02.xml")
    return if (doc-available($file)) then string(doc($file)/rfc/@docName) else $file || " is missing"'
"draft-example-vane-protocol-01"
"draft-example-vane-protocol-00"
"vane-02.xml is missing"
```

Data in other formats has functions of its own: `json-doc` reads JSON
into maps and arrays, `unparsed-text` and `unparsed-text-lines` read
text, and `csv-doc` and `html-doc` read CSV and HTML.
[JSON, CSV and HTML](formats.md) has the details. They mix freely. The
draft's editorial notes name issues, and the issue tracker exports its
issues as JSON:

`issues.json`:

```json
{{#include ../examples/spec/issues.json}}
```

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    let $issues := json-doc("issues.json")
    for $note in //cref
    let $issue := $issues($note/@anchor)
    return `{$note/@anchor} ({$issue?state}, opened {$issue?opened}): {$issue?title}`'
"issue-1 (open, opened 2026-07-20): Configurable pause duration"
"issue-2 (open, opened 2026-09-02): Anchor for the encryption requirement"
```

A map is a function from keys to values, so `$issues($note/@anchor)`
looks up the key `"issue-1"`; `?state` then takes the issue's state.

## Many documents

XQuery's `collection` function returns a whole set of documents by one
URI. What the URI names is up to the processor: a database, a list, or
for the `sxml` command a directory. The project keeps every published
version of the draft in `drafts`, and `collection("drafts")` gives the
document of every `.xml` file in it, sorted by name:

```console
$ sxml xquery --method=adaptive --expression='
    for $draft in collection("drafts")
    return `{document-uri($draft) => tokenize("/") => foot()}: {$draft/rfc/@docName}, {count($draft//t[@anchor])} requirements`'
"vane-00.xml: draft-example-vane-protocol-00, 8 requirements"
"vane-01.xml: draft-example-vane-protocol-01, 11 requirements"
```

`document-uri` tells which file each document came from; it is an
absolute `file:` URI, so the query keeps only its last part. The
documents are the ones `doc` reads for the same files, so
`doc("drafts/vane-00.xml") is collection("drafts")[1]` is true.
`uri-collection` gives the URIs without reading the files.

Parameters after a `?` change what the directory gives, separated by
`;`. `select` names the files (`*` stands for any characters, and the
default is `*.xml`), so `uri-collection` can also find files that are
not XML, for the functions that read them; `recurse=yes` takes the files
of subdirectories too:

```console
$ sxml xquery --method=adaptive --expression='
    doc("drafts/vane-00.xml") is collection("drafts")[1],
    uri-collection("drafts?select=*-01.xml") ! tokenize(., "/")[last()]'
true()
"vane-01.xml"
```

A file that is not well-formed stops `collection` with
`err:FODC0002`, which names the file and the line; with
`on-error=ignore` it is left out instead.

`--collection=DIR` makes a directory the *default collection*, the one
`collection()` returns without an argument. The query then does not
name the directory, and the same query reads another one next time:

```console
$ sxml xquery --method=adaptive --collection=drafts --expression='
    count(collection()//t[@anchor])'
19
```

## External variables

An *external variable* is declared in the prolog with `external`, and
its value comes from outside the query. A type after `as` says what it
must be, and a value after `:=` is its default. An Internet-Draft is
valid for six months, which the IETF counts as 185 days; this query
says when drafts expire:

`expiry.xq`:

```xquery
{{#include ../examples/spec/expiry.xq}}
```

`--param=NAME=VALUE` sets an external variable:

```console
$ sxml xquery --param=today=2026-10-21 expiry.xq
draft-example-vane-protocol-01: published 2026-10-21, expires 2027-04-24
$ sxml xquery --param=today=2027-02-01 --param="drafts=vane-00.xml vane.xml" expiry.xq
draft-example-vane-protocol-00: published 2026-07-06, expires 2027-01-07, expired
draft-example-vane-protocol-01: published 2026-10-21, expires 2027-04-24
$ sxml xquery --param=today=2027-02-01 --param="drafts=vane-00.xml vane.xml" --param=days=90 expiry.xq
draft-example-vane-protocol-00: published 2026-07-06, expires 2026-10-04, expired
draft-example-vane-protocol-01: published 2026-10-21, expires 2027-01-19, expired
```

The command line has only text, so `--param` passes the value as an
untyped value, and the declared type converts it: `2027-02-01` became
an `xs:date`, `90` an `xs:integer`. Text that isn't a value of the type
is an error:

```console
$ sxml xquery --param=today=1.2.2027 expiry.xq
expiry.xq: err:FORG0001: the value is not in the lexical space or value space of the target type
```

Without a default, an external variable must be given:

```console
$ sxml xquery --expression='declare variable $today as xs:date external; $today'
expression:1:46: err:XPDY0002: The focus or a variable of the dynamic context is absent (XPath 4.0, section 2.2.2)
```

A `--param` for a variable the query doesn't declare is ignored, so one
script can pass the same parameters to several queries.

Some advice for external variables:

- Always declare a type. It documents the parameter and converts the
  text from the command line.
- Give a default when there is a sensible one. `current-date()` is a
  good default for "today", and a parameter makes the query testable
  with a fixed date, as above.
- Pass a list as one string and split it with `tokenize`, as the file
  names of `$drafts` above.
