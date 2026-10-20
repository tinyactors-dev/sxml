# Files and the Outside World

<!-- examples: files -->

A query or a stylesheet often needs more than its input: another
document, a text file, a module, a schema. This chapter explains how
sxml finds them, and how it keeps a query from reading or writing what
it shouldn't. The examples are in `docs/book/examples/files`.

## Resolvers

The sxml library itself never opens a file or a network connection.
Whenever it needs a resource, such as the document for `fn:doc`, the
text for `fn:unparsed-text`, a module for `import module` or a schema for
`import schema`, it asks a *resource resolver* that the program using
it provides. The resolver gets the kind of resource and its absolute
URI, and answers with the bytes, or with nothing.

That one rule makes sxml safe to embed. A web server that runs users'
queries can give them a resolver that knows three documents and nothing
else. A browser page can answer from memory. A test can answer from a
table. Chapter 9 shows how to write one in C, and Chapter 10 in
JavaScript.

## Files and the sxml command

The `sxml` command's resolver reads local files, and only those. URIs
are resolved against the *static base URI*, which for an expression on
the command line is the working directory:

```console
$ sxml xpath 'doc("data/lisbon.xml")/city/@name/string()'
"Lisbon"
$ sxml xpath 'unparsed-text-lines("notes.txt")'
"one"
"two"
"three"
```

The same holds for `sxml xquery`, whether the query is in a file or not.
In a stylesheet, relative URIs are resolved against the stylesheet's own
location, as XSLT expects. A resource that isn't there is an
error, with the code the specification gives, and a URI that is not a
local file is never fetched:

```console
$ sxml xpath 'doc("missing.xml")'
expression:1:1: err:FODC0002: The file cannot be read
$ sxml xpath 'doc("https://example.org/feed.xml")'
expression:1:1: err:FODC0002: Only local files are available
```

`doc-available` and `unparsed-text-available` ask first:

```console
$ sxml xpath 'doc-available("missing.xml"), unparsed-text-available("notes.txt")'
false()
true()
```

## External entities and DTDs

An XML document can pull in other files itself, through its DTD:

`article.xml`:

```xml
{{#include ../examples/files/article.xml}}
```

Reading those files when a document asks for them is a classic security
hole, so sxml doesn't, unless you say so. `sxml check` skips external
entities without `--external-entities`, and `fn:doc` refuses a document
that needs them unless the call trusts it:

```console
$ sxml xpath 'doc("article.xml")//section'
expression:1:1: err:FODC0016: The document refers to an external DTD or entity, and the call does not trust external resources (F&O 4.0, fn:doc, fn:parse-xml: trust-external)
$ sxml xpath --method=xml 'doc("article.xml", { "trust-external": true() })/article/section'
<section status="draft">Text from another file.</section>
```

The trusted parse read the entity `chapter.xml`, and the DTD
`article.dtd` added the default attribute `status="draft"`. Entities
that expand to too much text are stopped by a limit, so a small document
can't expand into gigabytes.

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
replaced only with `--force`.

## EXPath File

The EXPath File functions read and write files and directories directly,
with the prefix `file`. Because they can touch any file the user can,
they exist only with `--allow-file-access`:

```console
$ sxml xpath 'file:exists("notes.txt")'
expression:1:1: err:XPST0017: no function with this name and arity is known
$ sxml xpath --allow-file-access 'sort(file:list("data")) ! doc("data/" || .)/city ! `{@name}: {@population}`'
"Lisbon: 545923"
"Porto: 231800"
$ sxml xpath --allow-file-access 'file:write("summary.txt", "two cities"), file:read-text("summary.txt")'
"two cities"
```

Without the option, the functions don't exist at all, as if sxml had
never heard of them.

## EXPath Binary

The EXPath Binary functions, with the prefix `bin`, work on binary data:
bytes, numbers in binary encodings, and text in any encoding. They don't
touch files, so they are always there:

```console
$ sxml xpath 'bin:hex("CAFE") => bin:to-octets()'
202
254
$ sxml xpath 'unparsed-binary("data/lisbon.xml") => bin:part(0, 5) => bin:to-octets()'
60
99
105
116
121
$ sxml xpath 'bin:decode-string(bin:from-octets((104, 105)))'
"hi"
```

`unparsed-binary` reads a resource as bytes, through the same resolver
as everything else.
