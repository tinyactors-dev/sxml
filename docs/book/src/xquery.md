# Writing Programs with XQuery

<!-- examples: spec -->

XQuery is XPath grown into a programming language. Every XPath
expression is an XQuery query, and XQuery adds:

- **FLWOR expressions**, a powerful form of `for` and `let`, with
  `where`, `order by`, `group by`, `count` and windows;
- **constructors** that build new XML, written as XML with expressions
  inside;
- a **prolog** of declarations: functions (which can call themselves),
  variables, namespaces, options and imported modules;
- **updates**, which insert, delete, replace and rename nodes.

This chapter works on the same document as
[Finding Things with XPath](xpath.md): the Vane protocol, an
Internet-Draft written in XML. Its requirements are paragraphs with an
anchor and a BCP 14 keyword:

```xml
{{#include ../examples/spec/vane.xml:111:120}}
```

There is an earlier version of the draft too, `vane-00.xml`, and the
chapter compares the two. The examples run in
`docs/book/examples/spec`, the directory of the specification project.
By the end of the chapter we'll have a small toolkit for that project: a
module that knows the draft's structure, a check that finds its
mistakes, a list of what changed between versions, and updates that edit
the draft in place, one named node at a time, instead of rewriting it.

## Running a query

A query usually lives in a file, ending in `.xq`. `sxml xquery` runs it:

```console
$ sxml xquery --context-item=vane.xml keywords.xq
1. MUST (5): req-1, req-2, req-4, req-6, req-7
2. MAY (2): req-9, req-11
3. SHOULD (2): req-3, req-8
```

Here is the query:

`keywords.xq`:

```xquery
{{#include ../examples/spec/keywords.xq}}
```

The first two lines are the *prolog*: here, two options for the output,
which ask for plain text with one item on each line (`&#10;` is a line
feed, written as an XML character reference). The rest is the *query
body*, one FLWOR expression that groups the requirements by keyword,
counts them, sorts and ranks the groups, and keeps the first three. By
the end of the next section you'll be able to read every clause of it.

The same query runs on the earlier draft, which had no MAY yet:

```console
$ sxml xquery --context-item=vane-00.xml keywords.xq
1. MUST (5): req-1, req-2, req-4, req-6, req-7
2. SHOULD (2): req-3, req-8
3. MUST NOT (1): req-5
```

For short queries, `--expression` saves writing a file. Unlike
`sxml xpath`, `sxml xquery` writes its result with the *xml* output
method by default, as a query's result is usually XML; `--method`
chooses another:

```console
$ sxml xquery --context-item=vane.xml --expression='//section[@anchor = "pause"]/name'
<name>Pause</name>
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

## What's in this chapter

- [FLWOR Expressions](xquery-flwor.md): `for`, `let`, `where`,
  `order by`, `group by`, `count` and `while`;
- [Grouping](xquery-grouping.md): putting requirements together by
  section and by keyword;
- [Building XML](xquery-constructors.md): constructors and namespaces,
  for an XHTML index of the requirements;
- [Conditions, Types and Errors](xquery-types-errors.md): `switch`,
  `typeswitch`, sequence types, `try` and `catch`;
- [Functions and Modules](xquery-modules.md): declared functions,
  library modules and private helpers;
- [Documents, Parameters and the Context](xquery-inputs.md): the input
  of a query, both drafts at once, and external variables;
- [Writing Results](xquery-serialization.md): output methods and
  serialization parameters;
- [Updating Documents](xquery-updates.md): the XQuery Update Facility,
  for edits that touch only the nodes they name;
- [What's New in XQuery 4.0](xquery-4.md): a tour for readers who know
  XQuery 3.1;
- [Project: Editing a Specification Without Rewriting It](xquery-project.md):
  all of it in a toolkit for the draft.

Full-text search, with `contains text`, is part of XQuery in sxml too;
it is in [Working with Text](text.md), with the other ways of working
with text. Windows, the FLWOR clauses for runs of neighbouring items,
are taught on trace data in
[Who Calls Whom, and How Often](traces-cardinality.md), and
[Project: Generating Code from a Schema](codegen.md) uses XQuery to
build the tree of a TypeScript program.
