# The sxml Command

<!-- examples: getting-started -->

`sxml xpath` is one of several commands. Each one does one job, and they
all work the same way: options start with `--`, results go to standard
output, and errors go to standard error with the file, line and column
where they happened. Let's take a quick tour, using the same
`books.xml` as before. The commands for queries, stylesheets and schemas
each have a chapter of their own later in the book.

## Checking documents

`sxml check` checks that documents are *well-formed*: that they follow
the rules of XML, such as every start tag having a matching end tag.
When they do, it prints nothing and exits with status 0:

```console
$ sxml check books.xml; echo "exit status $?"
exit status 0
```

This file has a `book` element that is never closed:

`broken.xml`:

```xml
{{#include ../examples/getting-started/broken.xml}}
```

```console
$ sxml check broken.xml; echo "exit status $?"
broken.xml:4:1: not well-formed: the text does not match the XML 1.0 grammar here (XML 1.0, section 2.1)
exit status 1
```

The parser noticed the problem at line 4, where `</books>` arrived while
`book` was still open.

## Validating documents

Well-formed is only the first step. A *schema* describes what a kind of
document must contain: which elements, in which order, with which types
of values. `books.xsd` is an XSD schema for our catalog. It says, among
other things, that each book has a title, one or more authors and a
price, and that its `year` is a year:

`books.xsd`:

```xml
{{#include ../examples/getting-started/books.xsd}}
```

`sxml validate` checks documents against it. Our catalog is valid:

```console
$ sxml validate --schema=books.xsd books.xml
```

This one is not:

`invalid.xml`:

```xml
{{#include ../examples/getting-started/invalid.xml}}
```

```console
$ sxml validate --schema=books.xsd invalid.xml
invalid.xml:2:9: cvc-datatype-valid.1.2.1: attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'
invalid.xml:4:5: cvc-complex-type.2.4: element 'price' is not allowed here; expected 'author'
```

Each error names the rule of XSD that failed, such as
`cvc-complex-type.2.4`, and says what was expected.

## Running queries

`sxml xquery` runs XQuery, a programming language that contains XPath
and adds variables, loops, sorting, functions and a way to build new
XML. This query lists the titles by year:

`titles.xq`:

```xquery
{{#include ../examples/getting-started/titles.xq}}
```

```console
$ sxml xquery --context-item=books.xml titles.xq
<titles>
  <title year="1968">The Art of Computer Programming</title>
  <title year="1979">Godel, Escher, Bach</title>
  <title year="1986">Structure and Interpretation of Computer Programs</title>
</titles>
```

The `for` clause goes through the books, `order by` sorts them, and
`return` builds a new `title` element for each one. The curly braces
put computed values into the XML. The first line asks for indented
output.

A short query can be given on the command line with `--expression`:

```console
$ sxml xquery --expression='<greeting>{ "Hello, " || "XQuery" }</greeting>'
<greeting>Hello, XQuery</greeting>
```

## Transforming documents

`sxml transform` runs an XSLT stylesheet. XSLT works by rules: each
*template* says what to make when the transformation meets a certain
kind of node. This stylesheet turns the catalog into an HTML page:

`list.xsl`:

```xml
{{#include ../examples/getting-started/list.xsl}}
```

```console
$ sxml transform list.xsl books.xml
<!DOCTYPE html>
<html>
  <body>
    <h1>Books</h1>
    <ul>
      <li>Godel, Escher, Bach (1979)</li>
      <li>Structure and Interpretation of Computer Programs (1986)</li>
      <li>The Art of Computer Programming (1968)</li>
    </ul>
  </body>
</html>
```

The first template matches the `books` element and writes the page
around the list. Inside it, `xsl:apply-templates` processes each book,
sorted by title, and the second template turns each book into a list
item. With `expand-text="yes"`, `{title}` in the text is replaced by the
value of the expression `title`.

## Writing documents again

`sxml serialize` parses a document and writes it again, for example
indented, or by another output method:

```console
$ echo '<a><b>1</b><b>2</b></a>' | sxml serialize --indent -
<a>
  <b>1</b>
  <b>2</b>
</a>
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
$ sxml chek books.xml
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
languages properly, starting with XPath.
