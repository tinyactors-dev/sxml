# Documents, Parameters and the Context

<!-- examples: xquery -->

A query gets its input in three ways: the *context item*, which paths
start from; documents and other files that it reads by name; and
*external variables*, the parameters of the whole query. This section
shows each of them with the `sxml` command.

## The context item

`--context-item=FILE` parses a document and makes it the context item,
so that a path such as `//book` starts from it. A file name of `-`
reads the document from standard input:

```console
$ sxml xquery --context-item=- --expression='count(//book)' < library.xml
5
```

A query can say what context item it expects, with `declare context
value`. Then a wrong document is an error at the start, instead of an
empty result later:

```console
$ sxml xquery --context-item=loans.xml --expression='
    declare context value as document-node(element(library)) external;
    count(//book)'
expression: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

A declaration can also give a default, used when no context item is
given:

```console
$ sxml xquery --expression='
    declare context value as document-node() external := doc("library.xml");
    count(//book)'
5
```

(XQuery 3.1 wrote this `declare context item`, which still works.)

## Reading documents

`doc` reads an XML document by its URI. With the `sxml` command, a
relative URI names a file relative to the working directory. A query
can read as many documents as it likes; reading the same URI twice gives
the same document, not a copy:

```console
$ sxml xquery --method=adaptive --expression='doc("library.xml") is doc("library.xml")'
true()
```

`doc-available` asks whether a document can be read, without raising
an error when it can't:

```console
$ sxml xquery --method=adaptive --expression='
    for $file in ("library.xml", "loans.xml", "missing.xml")
    return if (doc-available($file)) then name(doc($file)/*) else $file || " is missing"'
"library"
"loans"
"missing.xml is missing"
```

Data in other formats has functions of its own: `json-doc` reads JSON
into maps and arrays, `unparsed-text` and `unparsed-text-lines` read
text, and `csv-doc` and `html-doc` read CSV and HTML. Chapter 6 has
the details. They mix freely. The names of the members are in a JSON
file:

`members.json`:

```json
{{#include ../examples/xquery/members.json}}
```

```console
$ sxml xquery --method=adaptive --expression='
    let $members := json-doc("members.json")
    for $loan in doc("loans.xml")//loan
    return $members(string($loan/@member))?name || ": " || $loan/@book'
"Ada Lovelace: dune"
"Alan Turing: geb"
"Grace Hopper: dune"
"Ada Lovelace: sicp"
"Ada Lovelace: lhod"
"Alan Turing: dune"
```

A map is a function from keys to values, so `$members("ada")` looks up
the key `"ada"`; `?name` then takes the member's name.

## Many documents

XQuery's `collection` function returns a whole set of documents by one
URI. What the URI names is up to the processor: a database, a list, or
for the `sxml` command a directory. `collection("branches")` gives the
document of every `.xml` file in `branches`, sorted by name:

```console
$ sxml xquery --method=adaptive --expression='
    for $loans in collection("branches")/loans
    return $loans/@branch || ": " || count($loans/loan)'
"North: 2"
"South: 1"
```

The documents are the ones `doc` reads for the same files, so
`doc("branches/north.xml") is collection("branches")[1]` is true, and
`document-uri` tells which file each came from. `uri-collection` gives
those URIs without reading the files.

Parameters after a `?` change what the directory gives, separated by
`;`. `select` names the files (`*` stands for any characters), so
`uri-collection` can find files that are not XML, for the functions
that read them; `recurse=yes` takes the files of subdirectories too:

```console
$ sxml xquery --method=adaptive --expression='
    for $uri in uri-collection(".?select=*.json")
    return json-doc($uri)?ada?name'
"Ada Lovelace"
```

A file that is not well-formed stops `collection` with
`err:FODC0002`, which names the file and the line; with
`on-error=ignore` it is left out instead.

`--collection=DIR` makes a directory the *default collection*, the one
`collection()` returns without an argument. The query then does not
name the directory, and the same query reads another one next time:

```console
$ sxml xquery --method=adaptive --collection=branches --expression='
    count(collection()//loan)'
3
```

## External variables

An *external variable* is declared in the prolog with `external`, and
its value comes from outside the query. A type after `as` says what it
must be, and a value after `:=` is its default. This query finds the
loans that are overdue:

`overdue.xq`:

```xquery
{{#include ../examples/xquery/overdue.xq}}
```

`--param=NAME=VALUE` sets an external variable:

```console
$ sxml xquery --param=today=2026-10-20 overdue.xq
ada has dune since 2026-09-02, due 2026-09-23
alan has geb since 2026-09-05, due 2026-09-26
grace has dune since 2026-09-14, due 2026-10-05
ada has sicp since 2026-09-20, due 2026-10-11
$ sxml xquery --param=today=2026-10-20 --param=days=14 overdue.xq
ada has dune since 2026-09-02, due 2026-09-16
alan has geb since 2026-09-05, due 2026-09-19
grace has dune since 2026-09-14, due 2026-09-28
ada has sicp since 2026-09-20, due 2026-10-04
ada has lhod since 2026-10-01, due 2026-10-15
alan has dune since 2026-10-03, due 2026-10-17
```

The command line has only text, so `--param` passes the value as an
untyped value, and the declared type converts it: `2026-10-20` became an
`xs:date`, `14` an `xs:integer`. Text that isn't a value of the type is
an error:

```console
$ sxml xquery --param=today=20.10.2026 overdue.xq
overdue.xq: err:FORG0001: the value is not in the lexical space or value space of the target type
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
  names above.
