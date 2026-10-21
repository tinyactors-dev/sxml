# Finding Things with XPath

<!-- examples: spec -->

XPath is the language at the heart of everything else in this book.
XQuery is XPath with more around it, and XSLT uses XPath to select what
its templates process. Time spent learning XPath pays off three times.

This chapter and the next two work on one document: the specification
of the Vane protocol. The Vane service holds the feature flags of
applications, and an SDK in each application keeps a cache of them. The
protocol is how the service tells the SDKs, on an update stream, that a
flag rolled out or was killed, and how an SDK asks for the status of a
flag. The draft is written the way the IETF writes its specifications: as an Internet-Draft in XML, in the vocabulary of the
xml2rfc tool (RFC 7991), which turns such a file into the HTML and the
plain text you know from RFCs. Keeping a specification in XML means a
program can answer questions about it: which requirements there are,
which terms are used and where, what changed between two versions. By
the end of the chapters on XPath, XQuery and XSLT, we'll have the tools
to do all of that.

The examples run in `docs/book/examples/spec`, the directory of the
specification project, and give the draft as the context item. Here is
the file, `vane.xml`:

`vane.xml`:

```xml
{{#include ../examples/spec/vane.xml}}
```

It is long, but its shape is simple. The `front` has the title, the
authors and the date; the `middle` has the sections, each with a `name`
and paragraphs `t`; the `back` has the references. A *requirement* is a
paragraph with an `anchor`, and it says how strong it is with one of
the key words of BCP 14 (MUST, SHOULD, MAY and their negations) in a
`bcp14` element. `xref` elements refer to other parts of the draft by
their anchors: to the terms of section 1.1, to sections, and to
references. The same directory holds `vane-00.xml`, the draft's
previous version.

## What an XPath expression is

An XPath expression is a formula. It never changes anything: it looks at
its input and computes a value. Every value is a *sequence*: zero, one or
more *items* in order. An item is one of three things:

- a **node**: a part of a document, such as an element, an attribute or
  a piece of text;
- an **atomic value**: a string, a number, a date, a boolean and so on,
  each with a *type* such as `xs:string` or `xs:integer`;
- a **function**, which includes *maps* and *arrays*, as we'll see later
  in this chapter.

A single item and a sequence of one item are the same thing, and
sequences never nest: `(1, (2, 3))` is the sequence `1, 2, 3`.

```console
$ sxml xpath '(1, (2, 3), ())'
1
2
3
```

`()` is the empty sequence. It is how XPath says "nothing", and you'll
meet it often: a path that finds nothing gives `()`, not an error.

## The document as a tree

XPath sees a document as a tree of nodes. At the top is the *document
node*, which stands for the whole file. Its children are the comment at
the start of the file and the outermost element, `rfc`:

```console
$ sxml xpath --context-item=vane.xml '/node() ! (if (self::comment()) then "comment" else name())'
"comment"
"rfc"
```

Each element has its attributes, and its children: more elements, and
text nodes for the text between the tags, including the line breaks and
spaces that indent the file. Count them:

```console
$ sxml xpath --context-item=vane.xml 'count(//*), count(//@*), count(//text()), count(//comment())'
126
116
219
1
```

A hundred and twenty-six elements, a hundred and sixteen attributes,
two hundred and nineteen text nodes (most of them the whitespace between
the tags) and one comment.

Sections nest: a `section` can contain sections. The draft has six
sections at the top of `middle` and nine in all:

```console
$ sxml xpath --context-item=vane.xml 'count(/rfc/middle/section), count(//section)'
6
9
```

A paragraph has *mixed content*: text with elements in it. Requirement
req-7 is a text node, the `bcp14` element, another text node, an `xref`
and a last text node with the full stop:

```console
$ sxml xpath --context-item=vane.xml '//t[@anchor = "req-7"]/node() ! (if (self::text()) then "text" else "element " || name())'
"text"
"element bcp14"
"text"
"element xref"
"text"
```

Keep this picture in mind: most of the surprises in XPath come from
forgetting that text is made of nodes too.

The rest of this chapter has five parts:

- [Paths, Steps and Predicates](xpath-paths.md): finding nodes;
- [Values and Types](xpath-values.md): numbers, strings, dates and
  comparisons;
- [Functions and Operators](xpath-functions.md): the function library,
  conditions and loops;
- [Maps, Arrays and Records](xpath-maps-arrays.md): structured data
  beyond XML;
- [Functions as Values](xpath-higher-order.md): passing functions to
  functions.
