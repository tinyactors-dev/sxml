# Querying Documents with XPath

<!-- examples: xpath -->

XPath is the language at the heart of everything else in this book.
XQuery is XPath with more around it, and XSLT uses XPath to select what
its templates process. Time spent learning XPath pays off three times.

In this chapter we will query one document, a small library with two
shelves:

`library.xml`:

```xml
{{#include ../examples/xpath/library.xml}}
```

All the commands in this chapter run in the `docs/book/examples/xpath`
directory, and give that file as the context item.

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
node*, which stands for the whole file. Its child is the outermost
element, `library`. Each element has its attributes, and its children:
more elements, and text nodes for the text between the tags, including
the line breaks and spaces that indent the file.

Count them:

```console
$ sxml xpath --context-item=library.xml 'count(//*), count(//@*), count(//text())'
39
22
62
```

Thirty-nine elements, twenty-two attributes, and sixty-two text nodes:
most of those are the whitespace between the tags.

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
