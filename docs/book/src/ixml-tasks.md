# Real Tasks

<!-- examples: ixml -->

This chapter solves five everyday problems with a grammar each: a log
file, semicolon-separated values, dates written in several ways, a small
programming language, and Markdown-like text. Every solution has two
parts, a grammar that turns the text into XML and a query that uses the
XML.

## Log files

`log.ixml`:

```
{{#include ../examples/ixml/log.ixml}}
```

A `log` is lines separated by line feeds, with an optional line feed at
the end (`-#a?`). The date, time and level become attributes of their
line, and `message` takes everything up to the end of the line,
`~[#a]*`. The grammar describes this log:

`app.log`:

```
{{#include ../examples/ixml/app.log}}
```

Once the log is a document, grouping and filtering are ordinary XQuery:

`levels.xq`:

```xquery
{{#include ../examples/ixml/levels.xq}}
```

```console
$ sxml xquery --method=text levels.xq
INFO: 3
ERROR: 2
WARN: 1
09:21:13 disk full
10:02:31 connection refused
```

Real logs are rarely this tidy. Two habits keep the grammar useful: make
the part you care about precise (the date, the level) and let the rest
be "anything up to the end of the line", and test the grammar on the
oddest lines first.

## Semicolon-separated values

For plain CSV, `parse-csv` ([JSON, CSV, HTML and Invisible
XML](formats.md#csv)) is quicker. A grammar earns its place when the
format is a variant that a CSV parser does not know. This one separates
fields with semicolons and lets a quoted field hold semicolons and
doubled quotes:

`prices.ixml`:

```
{{#include ../examples/ixml/prices.ixml}}
```

`prices.txt`:

```
{{#include ../examples/ixml/prices.txt}}
```

```console
$ sxml xquery --param=grammar=prices.ixml --param=input=prices.txt ixml.xq
<table>
  <header>
    <field>item</field>
    <field>price</field>
    <field>note</field>
  </header>
  <row>
    <field>Coffee</field>
    <field>3.50</field>
    <field>hot; large</field>
  </row>
  <row>
    <field>Tea</field>
    <field>2.10</field>
    <field/>
  </row>
  <row>
    <field>Juice "fresh"</field>
    <field>4.20</field>
    <field>cold</field>
  </row>
</table>
```

Note the first rule: every row ends with a line feed, `(row, -#a)+`. A
first try, `row++-#a, -#a?`, reads the last line feed in two ways, as the
end of the file or as a separator before a row with one empty field, and
the result is ambiguous. Saying exactly where each line feed belongs
removes the doubt.

From the table to JSON is one query, with the header naming the fields:

`prices-json.xq`:

```xquery
{{#include ../examples/ixml/prices-json.xq}}
```

```console
$ sxml xquery prices-json.xq
[
  {
    "item": "Coffee",
    "price": 3.5,
    "note": "hot; large"
  },
  {
    "item": "Tea",
    "price": 2.1,
    "note": ""
  },
  {
    "item": "Juice \"fresh\"",
    "price": 4.2,
    "note": "cold"
  }
]
```

## Dates

People write dates in many ways. One grammar can read several forms and
give them all the same XML, so the query after it sees one form only:

`dates.ixml`:

```
{{#include ../examples/ixml/dates.ixml}}
```

`dates.txt`:

```
{{#include ../examples/ixml/dates.txt}}
```

Three features do the work. The three forms are hidden alternatives of
`date`, so each gives `year`, `month` and `day` directly. The month names
are hidden strings with an insertion of the month's number, and `>month`
renames `month-name` to `month`, so a written month and a numbered one
are the same element. And `+"0", digit` pads a one-digit day or month.
The result is in [the first chapter](ixml.md#running-a-grammar-from-the-command-line);
here a query sorts the dates:

`sort-dates.xq`:

```xquery
{{#include ../examples/ixml/sort-dates.xq}}
```

```console
$ sxml xquery --method=adaptive sort-dates.xq
"1 May 1886, a Saturday"
"4 July 1976, a Sunday"
"20 October 2026, a Tuesday"
"20 October 2026, a Tuesday"
```

The first and second lines of `dates.txt` were the same day in two forms.

## A small programming language

A grammar can describe a language with statements, names and
expressions. In this one, `let` names a value, `print` prints one, and
`*` and `/` bind tighter than `+` and `-`:

`calc.ixml`:

```
{{#include ../examples/ixml/calc.ixml}}
```

Precedence comes from the shape of the rules: a `sum` adds `term`s, a
`term` is a `product` or a `factor`, and a `product` multiplies
`factor`s, so `1 + 2 * 3` can only be read with the product inside the
sum. Because `sum` begins with `expression` and `product` with `term`,
both are *left-recursive*, and `8 - 2 - 1` groups as `(8 - 2) - 1`.
`@add>op` and `@multiply>op` make both kinds of operator an attribute
`op`. `s` is optional space and `S` required space.

`area.calc`:

```
{{#include ../examples/ixml/area.calc}}
```

```console
$ sxml xquery --param=grammar=calc.ixml --param=input=area.calc ixml.xq
<program>
  <let name="width">
    <number>6</number>
  </let>
  <let name="height">
    <number>7</number>
  </let>
  <print>
    <product op="*">
      <variable name="width"/>
      <variable name="height"/>
    </product>
  </print>
  <print>
    <product op="*">
      <sum op="+">
        <variable name="width"/>
        <variable name="height"/>
      </sum>
      <number>2</number>
    </product>
  </print>
</program>
```

The XML is the program's syntax tree, and an interpreter is a recursive
function over it:

`run.xq`:

```xquery
{{#include ../examples/ixml/run.xq}}
```

```console
$ sxml xquery --method=adaptive --param=file=area.calc run.xq
42
26
```

The same tree could be checked for names used before they are set,
pretty-printed, or translated into another language with XSLT.

## Markdown-like text

The last grammar reads a little of Markdown: headings, lists and
paragraphs separated by blank lines, with emphasis and code inside. Its
names are HTML's, so the XML it makes *is* HTML:

`markdown.ixml`:

```
{{#include ../examples/ixml/markdown.ixml}}
```

`notes.md`:

```
{{#include ../examples/ixml/notes.md}}
```

```console
$ sxml xquery --param=grammar=markdown.ixml --param=input=notes.md ixml.xq
<body>
  <h1>Release notes</h1>
  <p>Version 2 reads <em>Invisible XML</em> grammars and parses <code>CSV</code> files.</p>
  <h2>Changes</h2>
  <ul>
    <li>faster parsing</li>
    <li>better <em>error</em> messages</li>
  </ul>
</body>
```

Two details make this grammar unambiguous. The characters of a line are
single hidden characters, `-char`, so a run of them is one text node and
there is only one way to split it. And a paragraph line may not start
with `#` or `-`, so a heading or a list item can never also be read as a
paragraph. The line feed inside a paragraph becomes a space with
`-#a, +" "`.
