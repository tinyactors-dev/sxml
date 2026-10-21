# Files and the Outside World

<!-- examples: spec -->

A query or a stylesheet often needs more than its input: another
document, a text file, a module, a schema. This chapter explains how
sxml finds them, and how it keeps a query from reading or writing what
it shouldn't. The examples are in `docs/book/examples/spec`, with the
Latch draft and its references.

## Resolvers

The sxml library itself never opens a file or a network connection.
Whenever it needs a resource, such as the document for `fn:doc`, the
text for `fn:unparsed-text`, a module for `import module` or a schema for
`import schema`, it asks a *resource resolver* that the program using
it provides. The resolver gets the kind of resource and its absolute
URI, and answers with the bytes, or with nothing.

That one rule makes sxml safe to embed. A web server that renders
users' drafts can give them a resolver that knows the draft and the
bibliography and nothing else. A browser page can answer from memory. A
test can answer from a table. [Embedding sxml in C](c.md) shows how to
write one in C, and [sxml in JavaScript](javascript.md) in JavaScript.

## Files and the sxml command

The `sxml` command's resolver reads local files, and only those. URIs
are resolved against the *static base URI*, which for an expression on
the command line is the working directory:

```console
$ sxml xpath 'doc("latch-00.xml")/rfc/@docName/string()'
"draft-example-latch-protocol-00"
$ sxml xpath 'unparsed-text-lines("publish.sh")[1]'
"#!/bin/sh"
```

The same holds for `sxml xquery`, whether the query is in a file or not.
In a stylesheet, relative URIs are resolved against the stylesheet's own
location, as XSLT expects. A resource that isn't there is an error, with
the code the specification gives, and a URI that is not a local file is
never fetched, not even the RFC Editor's bibliography:

```console
$ sxml xpath 'doc("latch-02.xml")'
expression:1:1: err:FODC0002: The file cannot be read (latch-02.xml)
$ sxml xpath 'doc("https://www.rfc-editor.org/refs/bibxml/reference.RFC.2119.xml")'
expression:1:1: err:FODC0002: Only local files are available (https://www.rfc-editor.org/refs/bibxml/reference.RFC.2119.xml)
```

`doc-available` and `unparsed-text-available` ask first:

```console
$ sxml xpath 'doc-available("latch-02.xml"), unparsed-text-available("publish.sh")'
false()
true()
```

## External entities and DTDs

An XML document can pull in other files itself, through its DTD. For
years, xml2rfc authors wrote their references that way: the RFC Editor
publishes a file for each RFC (its *bibxml*), and a draft declares an
external entity for each one it cites. `bibxml/` holds the four the
Latch draft needs:

`bibxml/reference.RFC.2119.xml`:

```xml
{{#include ../examples/spec/bibxml/reference.RFC.2119.xml}}
```

`latch-entities.xml`, a short version of the draft, refers to them, and
its internal DTD subset declares a default for an attribute:

```xml
{{#include ../examples/spec/latch-entities.xml}}
```

Reading those files when a document asks for them is a classic security
hole, so sxml doesn't, unless you say so. `sxml check` skips external
entities without `--external-entities`, and `fn:doc` refuses a document
that needs them unless the call trusts it:

```console
$ sxml check latch-entities.xml
$ sxml check --external-entities latch-entities.xml
$ sxml xpath 'doc("latch-entities.xml")//reference/@anchor'
expression:1:1: err:FODC0016: The document refers to an external DTD or entity, and the call does not trust external resources (F&O 4.0, fn:doc, fn:parse-xml: trust-external)
$ sxml xpath 'doc("latch-entities.xml", { "trust-external": true() })//reference ! `{@anchor} {@quoteTitle}: {front/title}`'
"RFC2119 true: Key words for use in RFCs to Indicate Requirement Levels"
"RFC5234 true: Augmented BNF for Syntax Specifications: ABNF"
"RFC8174 true: Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words"
"RFC6238 true: TOTP: Time-Based One-Time Password Algorithm"
```

The trusted parse read the four entities, each a file below `bibxml/`,
and the DTD added the default attribute `quoteTitle="true"` to every
`reference`, though no file writes it. Entities that expand to too much
text are stopped by a limit, so a small document can't expand into
gigabytes. Today's xml2rfc reads references with `xi:include` instead of
entities; the risk and the rule are the same.

## Writing files

The `sxml` command writes a file only when an option names it:

| Option | What it writes |
| --- | --- |
| `--output=FILE` | the result of `xpath`, `xquery`, `transform` or `serialize` |
| `--result-directory=DIR` | the result documents of `transform`, below DIR |
| `--allow-put` | the documents that `fn:put` stores in `xquery` |
| `--allow-file-access` | anything the EXPath File functions write |

Files are written only when the command succeeds, each in one step, so a
failed run never leaves half a file behind. A file that exists is
replaced only with `--force`. [Project: Publishing the
Specification](xslt-project.md) relies on both rules.

## EXPath File

The EXPath File functions read and write files and directories directly,
with the prefix `file`. Because they can touch any file the user can,
they exist only with `--allow-file-access`:

```console
$ sxml xpath 'file:exists("bibxml")'
expression:1:1: err:XPST0017: no function with this name and arity is known
$ sxml xpath --allow-file-access 'sort(file:list("bibxml")) ! doc("bibxml/" || .)/reference ! `{@anchor}: {front/date/@year}`'
"RFC2119: 1997"
"RFC5234: 2008"
"RFC6238: 2011"
"RFC8174: 2017"
$ sxml xpath --allow-file-access 'file:create-dir("notes"), file:write-text-lines("notes/references.txt", sort(file:list("bibxml"))), file:read-text-lines("notes/references.txt")'
"reference.RFC.2119.xml"
"reference.RFC.5234.xml"
"reference.RFC.6238.xml"
"reference.RFC.8174.xml"
$ cat notes/references.txt
reference.RFC.2119.xml
reference.RFC.5234.xml
reference.RFC.6238.xml
reference.RFC.8174.xml
```

`file:list` gives the names in a directory, in no particular order, so
the query sorts them. Without the option, the functions don't exist at
all, as if sxml had never heard of them.

## EXPath Binary

The EXPath Binary functions, with the prefix `bin`, work on binary data:
bytes, numbers in binary encodings, and text in any encoding. They don't
touch files, so they are always there. `unparsed-binary` reads a
resource as bytes, through the same resolver as everything else:

```console
$ sxml xpath 'unparsed-binary("latch.xml") => bin:part(0, 5) => bin:to-octets()'
60
63
120
109
108
$ sxml xpath 'unparsed-binary("latch.xml") => bin:part(0, 5) => bin:decode-string()'
"<?xml"
$ sxml xpath 'bin:hex("EFBBBF") => bin:to-octets()'
239
187
191
$ sxml xpath 'bin:decode-string(bin:from-octets((76, 97, 116, 99, 104)))'
"Latch"
```

The bytes `EF BB BF` are the UTF-8 byte order mark. Some editors write
it at the start of a file, and some tools that process drafts don't
expect it. A file that starts with one, and a check that finds it, with
a hex dump of the first three bytes of each file:

```console
$ printf '\357\273\277<rfc/>\n' > bom.xml
$ sxml xpath '("latch.xml", "bom.xml") ! `{.}: {xs:hexBinary(bin:part(unparsed-binary(.), 0, 3))}`'
"latch.xml: 3C3F78"
"bom.xml: EFBBBF"
$ sxml xpath '("latch.xml", "bom.xml")[bin:part(unparsed-binary(.), 0, 3) = bin:hex("EFBBBF")]'
"bom.xml"
```

`unparsed-binary` gives an `xs:base64Binary`; cast to `xs:hexBinary`,
the same bytes are written in hexadecimal. An XML parser skips the mark,
so `sxml check bom.xml` finds nothing wrong: only the bytes tell.
