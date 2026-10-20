# Writing Programs with XQuery

<!-- examples: xquery -->

XQuery is XPath grown into a programming language. Every XPath
expression is an XQuery query, and XQuery adds:

- **FLWOR expressions**, a powerful form of `for` and `let`, with
  `where`, `order by`, `group by`, `count` and windows;
- **constructors** that build new XML, written as XML with expressions
  inside;
- a **prolog** of declarations: functions (which can call themselves),
  variables, namespaces, options and imported modules;
- **updates**, which insert, delete, replace and rename nodes.

This chapter uses the same library as Chapter 2, and a second document,
a list of loans:

`loans.xml`:

```xml
{{#include ../examples/xquery/loans.xml}}
```

The examples run in `docs/book/examples/xquery`.

## Running a query

A query usually lives in a file, ending in `.xq`. `sxml xquery` runs it:

```console
$ sxml xquery --context-item=library.xml popular.xq
1. Dune (3)
2. Godel, Escher, Bach (1)
3. The Left Hand of Darkness (1)
```

Here is the query:

`popular.xq`:

```xquery
{{#include ../examples/xquery/popular.xq}}
```

The first two lines are the *prolog*: here, two options for the output,
which ask for plain text with one item on each line (`&#10;` is a line
feed, written as an XML character reference). The rest is the *query
body*, one FLWOR expression that groups the loans by book, counts them,
sorts and ranks the groups, and keeps the first three. By the end of the
next section you'll be able to read every clause of it.

For short queries, `--expression` saves writing a file. Unlike
`sxml xpath`, `sxml xquery` writes its result with the *xml* output
method by default, as a query's result is usually XML; `--method`
chooses another:

```console
$ sxml xquery --expression='for $i in 1 to 3 return <item>{ $i }</item>'
<item>1</item><item>2</item><item>3</item>
$ sxml xquery --method=adaptive --expression='"one", 2'
"one"
2
```

## Comments

XQuery comments are written between `(:` and `:)`, and can be nested.
Comments that start with `(:~` document the declaration after them:

```xquery
(: A comment. (: A nested one. :) :)
```
