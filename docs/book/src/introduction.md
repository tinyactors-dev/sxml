# Introduction

Welcome to *The sxml Book*, an introduction to sxml and to the XML
languages it implements.

sxml is an XML toolchain in portable C. It parses XML and HTML, validates
documents against XSD 1.1 schemas and DTDs, and runs XPath 4.0, XQuery
4.0 and XSLT 4.0. All of its languages share one data model and one
engine. You can use it in three ways:

- as the `sxml` command, from a terminal or a shell script;
- as a C library, linked into your own program;
- as a JavaScript package, in Node.js and in browsers, through
  WebAssembly.

## Who this book is for

This book is for programmers who want to read, query, transform or check
XML (or JSON, CSV and HTML) and who want to do it with standard
languages instead of ad hoc code. It assumes you can use a terminal and
read a little code. It does not assume you know XPath, XQuery or XSLT:
each chapter introduces what it needs.

If you already know these languages from another processor, the book
still has something for you. Skim the language chapters for what is new
in the 4.0 versions, and read the chapters on embedding sxml in C and
JavaScript, on files and resolvers, and on performance.

## How to use this book

The chapters build on each other. The first chapter installs sxml and
runs a first expression. The next three teach the three languages in the
order they build on each other: XPath, then XQuery, which contains
XPath, then XSLT, which uses XPath inside its templates. Later chapters
cover validation, other data formats, text, files, and using sxml from
your own programs. The appendices are for reference.

Code examples come in two kinds. Files you create look like this:

`books.xml`:

```xml
<books>
  <book year="1979"><title>Godel, Escher, Bach</title></book>
</books>
```

Commands you type, with the output they print, look like this. You type
the text after the `$`; the lines without a `$` are the output:

```console
$ sxml xpath '1 + 1'
2
```

All the example files are in the book's source, in the
`docs/book/examples/` directory of the sxml repository, one directory
for each chapter. Run the commands from inside that chapter's directory,
and you will see the same output as the book.

## Source code

The sxml repository is at <https://github.com/dhamidi/sxml>. The book's
text is in `docs/book/` there. If you find a mistake, an example that
does not do what the book says, or a passage that confused you, please
open an issue.
