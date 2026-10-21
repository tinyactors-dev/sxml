# The sxml Command

<!-- examples: getting-started -->

`sxml xpath` is one of several commands. Each one does one job, and they
all work the same way: options start with `--`, results go to standard
output, and errors go to standard error with the file, line and column
where they happened. Let's take a quick tour, using the same
`draft.xml` as before. The commands for queries, stylesheets and schemas
each have a chapter of their own later in the book.

## Checking documents

`sxml check` checks that documents are *well-formed*: that they follow
the rules of XML, such as every start tag having a matching end tag.
When they do, it prints nothing and exits with status 0:

```console
$ sxml check draft.xml; echo "exit status $?"
exit status 0
```

This file has a paragraph `t` that is never closed:

`broken.xml`:

```xml
{{#include ../examples/getting-started/broken.xml}}
```

```console
$ sxml check broken.xml; echo "exit status $?"
broken.xml:6:5: not well-formed: the text does not match the XML 1.0 grammar here (XML 1.0, section 2.1)
exit status 1
```

The parser noticed the problem at line 6, where `</section>` arrived
while `t` was still open.

## Validating documents

Well-formed is only the first step. A *schema* describes what a kind of
document must contain: which elements, in which order, with which types
of values. `draft.xsd` is an XSD schema for our draft. It says, among
other things, that each section has a `name` and then one or more
paragraphs, and that a `bcp14` holds one of five key words:

`draft.xsd`:

```xml
{{#include ../examples/getting-started/draft.xsd}}
```

`sxml validate` checks documents against it. Our draft is valid:

```console
$ sxml validate --schema=draft.xsd draft.xml
```

This one is not:

`invalid.xml`:

```xml
{{#include ../examples/getting-started/invalid.xml}}
```

```console
$ sxml validate --schema=draft.xsd invalid.xml
invalid.xml:7:7: cvc-complex-type.2.4: element 't' is not allowed here; expected 'name'
invalid.xml:7:32: cvc-enumeration-valid: element 'bcp14' has the value 'MIGHT', which is not valid for type 'keyword': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'MUST', 'MUST NOT', 'SHOULD', 'SHOULD NOT', 'MAY'
```

The section has lost its `name`, and the requirement says MIGHT, which
is not a key word. Each error names the rule of XSD that failed, such as
`cvc-complex-type.2.4`, and says what was expected.

## Running queries

`sxml xquery` runs XQuery, a programming language that contains XPath
and adds variables, loops, sorting, functions and a way to build new
XML. This query makes an index of the requirements, sorted by key word:

`requirements.xq`:

```xquery
{{#include ../examples/getting-started/requirements.xq}}
```

```console
$ sxml xquery --context-item=draft.xml requirements.xq
<requirements>
  <requirement anchor="req-2" keyword="MUST">A flag MUST be stable when it is created.</requirement>
  <requirement anchor="req-7" keyword="MUST">After three failed checks, a service MUST pause the flag.</requirement>
  <requirement anchor="req-3" keyword="SHOULD">A rolling-out flag SHOULD be stable again after ten minutes.</requirement>
</requirements>
```

The `for` clause goes through the requirements, `order by` sorts them
by key word and then by anchor, and `return` builds a new `requirement`
element for each one. The curly braces put computed values into the
XML. The first line asks for indented output.

A short query can be given on the command line with `--expression`:

```console
$ sxml xquery --expression='<greeting>{ "Hello, " || "XQuery" }</greeting>'
<greeting>Hello, XQuery</greeting>
```

## Transforming documents

`sxml transform` runs an XSLT stylesheet. XSLT works by rules: each
*template* says what to make when the transformation meets a certain
kind of node. This stylesheet turns the draft into an HTML outline:

`outline.xsl`:

```xml
{{#include ../examples/getting-started/outline.xsl}}
```

```console
$ sxml transform outline.xsl draft.xml
<!DOCTYPE html>
<html>
  <body>
    <h1>The Vane Protocol for Feature Flag Rollouts</h1>
    <h2>1. Flag States</h2>
    <ul>
      <li id="req-2">A flag MUST be stable when it is created.</li>
      <li id="req-3">A rolling-out flag SHOULD be stable again after ten minutes.</li>
    </ul>
    <h2>2. Pause</h2>
    <ul>
      <li id="req-7">After three failed checks, a service MUST pause the flag.</li>
    </ul>
  </body>
</html>
```

The first template matches the `rfc` element and writes the page
around the sections. Inside it, `xsl:apply-templates` processes each
section, the second template writes a heading and a list for it, and the
third turns each requirement into a list item. With `expand-text="yes"`,
`{name}` in the text is replaced by the value of the expression `name`,
and `{position()}` by the section's position among the sections
processed.

## Writing documents again

`sxml serialize` parses a document and writes it again, for example
indented, or by another output method:

```console
$ echo '<section><name>Pause</name><t>Three failed checks.</t></section>' | sxml serialize --indent -
<section>
  <name>Pause</name>
  <t>Three failed checks.</t>
</section>
```

A file name of `-` means standard input, in every command.

## Exit status

Every command exits with the same status codes, so scripts can tell what
went wrong:

| Status | Meaning |
| --- | --- |
| 0 | Success |
| 1 | The operation failed: a document is not well-formed or not valid, or a query or stylesheet raised an error |
| 2 | Usage error: an unknown command or option, or a bad option value |
| 3 | A file could not be read or written |
| 4 | An internal error, such as running out of memory or exceeding a limit |

A mistyped command or option is a usage error, and sxml suggests what you
may have meant:

```console
$ sxml chek draft.xml
sxml: unknown command 'chek'; did you mean 'check'?
Usage: sxml [--version] COMMAND [ARGUMENT...]
Try 'sxml --help'.
```

## Summary

You now know the whole `sxml` command at a glance:

- `check` for well-formedness and `validate` for schemas;
- `xpath` for expressions, `xquery` for queries and `transform` for
  stylesheets;
- `serialize` to write a document again.

sxml reads only the local files you name, never uses the network and
writes a file only when an option asks it to. Now it's time to learn the
languages properly, starting with XPath on the whole Vane draft.
