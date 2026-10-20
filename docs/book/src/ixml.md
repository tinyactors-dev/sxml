# Parsing Text with Invisible XML

<!-- examples: ixml -->

A great deal of data is text with a structure that only a person can
see: a log line, a date written in words, a configuration file, a small
language of your own. Invisible XML (iXML) makes that structure visible.
You describe the text with a grammar, and the grammar turns every text
it describes into an XML document, which the XPath, XQuery and XSLT of
the earlier parts can query and transform. The examples of this part are
in `docs/book/examples/ixml`.

## A first grammar

This grammar describes a date such as `2026-10-20`:

`date.ixml`:

```
{{#include ../examples/ixml/date.ixml}}
```

Each *rule* names a part of the text and says what it is made of, in
order, separated by commas: a `date` is a `year`, a hyphen, a `month`, a
hyphen and a `day`. A quoted string matches itself, and `["0"-"9"]`
matches one character in a range. The first rule describes the whole
text.

The function `invisible-xml` compiles a grammar into a parser, which is
itself a function: give it a text, and it returns a document.

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("date.ixml"))("2026-10-20")'
<date><year>2026</year><month>10</month><day>20</day></date>
```

Every rule became an element, except `digit`: the `-` before its name
*hides* it, so its characters go straight into the element around it.
The hyphens between the parts are hidden too, by the `-` before each
`"-"`. Without the marks, every digit would be an element of its own and
the hyphens would be text.

## Compile once, parse many times

Compiling a grammar takes more work than parsing one short text with it.
Keep the parser in a variable and call it for every text:

```console
$ sxml xpath 'let $parse := invisible-xml(unparsed-text("date.ixml")) for $text in ("2026-10-20", "1999-12-31") return $parse($text)/date/year/string()'
"2026"
"1999"
```

The result of the parser is a document node, so `$parse($text)/date`
is its document element, and every path, function and FLWOR expression
works on it.

## Running a grammar from the command line

The `sxml` command has no command of its own for Invisible XML: the
`xquery` command runs it, with the grammar and the input as parameters.
This query, kept in a file, works for any grammar and any input:

`ixml.xq`:

```xquery
{{#include ../examples/ixml/ixml.xq}}
```

```console
$ sxml xquery --param=grammar=date.ixml --param=input=today.txt ixml.xq
<failed xmlns:ixml="http://invisiblexml.org/NS" ixml:state="failed">
  <line>1</line>
  <column>11</column>
  <position>11</position>
  <unexpected>#a</unexpected>
</failed>
```

The parse failed. `today.txt` holds the date and a line feed, as text
files do, and the grammar says nothing about a line feed: after the day,
at column 11, it expected the end of the text and found `#a`, the line
feed. A failed parse is not an error. The parser returns a document whose
element is `failed`, with the attribute `ixml:state="failed"`, and says
where the parse stopped; [When a Parse Fails](ixml-errors.md) has more
on reading it.

The grammar can allow the line feed. `dates.ixml` in the
[next chapter's tasks](ixml-tasks.md#dates) reads a date on each line;
here it reads `dates.txt`:

```console
$ sxml xquery --param=grammar=dates.ixml --param=input=dates.txt ixml.xq
<dates>
  <date>
    <year>2026</year>
    <month>10</month>
    <day>20</day>
  </date>
  <date>
    <day>20</day>
    <month>10</month>
    <year>2026</year>
  </date>
  <date>
    <month>07</month>
    <day>04</day>
    <year>1976</year>
  </date>
  <date>
    <day>01</day>
    <month>05</month>
    <year>1886</year>
  </date>
</dates>
```

To keep the result, add `--output=FILE`; to use it at once, put the
parse in a query of its own, as the tasks in this part do.

## Errors or documents

By default a text that does not match gives the `failed` document, so a
query can look at it and go on. With the option `fail-on-error`, the
parser raises the error FOIX0002 instead:

```console
$ sxml xpath 'invisible-xml(unparsed-text("date.ixml"), {"fail-on-error": true()})("2026-1-20")'
expression:1:1: err:FOIX0002: The input does not match the grammar, and fail-on-error is true (F&O 4.0, fn:invisible-xml)
```

A grammar that is not a grammar is always an error, FOIX0001; the
[errors chapter](ixml-errors.md) shows the common ones.

## A grammar is XML too

Called without a grammar, `invisible-xml` returns the parser of iXML
grammars themselves. Its result is the grammar in its XML form, which the
specification defines and which `invisible-xml` accepts as well:

```console
$ sxml xpath --method=xml 'invisible-xml()("greeting: ""hello"".")'
<ixml><rule name="greeting"><alt><literal string="hello"/></alt></rule></ixml>
$ sxml xpath --method=xml 'invisible-xml(invisible-xml()("greeting: ""hello"".")/ixml)("hello")'
<greeting>hello</greeting>
```

A program can therefore read a grammar, change it with XQuery or XSLT,
and compile the changed grammar: add a rule, rename an element, or build
a grammar from a table.

## In this part

- [Writing Grammars](ixml-grammars.md): rules, characters, repetition,
  marks, attributes, insertions, renaming, and what an ambiguous grammar
  does.
- [Real Tasks](ixml-tasks.md): log files, semicolon-separated values,
  dates in many forms, a small programming language, and Markdown-like
  text.
- [When a Parse Fails](ixml-errors.md): the `failed` document, grammar
  errors, and how to fix a grammar.
- [Invisible XML Performance](ixml-performance.md): how large an input
  can be, and which grammars parse in time proportional to the input.

sxml implements the Invisible XML Specification 1.0 with its errata and
the renaming of the Community Group's draft of version 1.1, and
`fn:invisible-xml` of F&O 4.0. It passes every test of the iXML community
test suite that it runs (Appendix B).
