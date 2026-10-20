# Transforming with XSLT

<!-- examples: xslt -->

XQuery describes a result by computing it: for each book, make this
element. XSLT describes it by *rules*: when you meet a book, make this
element. A stylesheet is a set of *template rules*, each with a
*pattern* that says which nodes it applies to. The processor walks the
input, finds the best rule for each node, and the rules decide whether
and how to go on to the children.

That way of working fits documents well. A document can contain
anything, in any order, and rules say what to do with each part of it
without having to know the whole structure in advance. It is also what
makes XSLT easy to extend: a new stylesheet can import an old one and
override only the rules it wants to change.

A stylesheet is itself an XML document, with its instructions in the
XSLT namespace, `http://www.w3.org/1999/XSL/Transform`, usually with the
prefix `xsl`. The examples of this chapter are in
`docs/book/examples/xslt`, with the same `library.xml` as before.

## The built-in rules

The smallest stylesheet has no rules at all:

`empty.xsl`:

```xml
{{#include ../examples/xslt/empty.xsl}}
```

Even so, it does something:

`note.xml`:

```xml
{{#include ../examples/xslt/note.xml}}
```

```console
$ sxml transform empty.xsl note.xml
<?xml version="1.0" encoding="UTF-8"?>
  Ada
  Remember the meeting at noon.
```

When no rule matches a node, a *built-in rule* applies. For elements
and the document node, it processes the children. For text, it copies
the text. So an empty stylesheet writes all the text of a document,
without the tags. Note that the attribute `date` is gone too: the
built-in rules don't process attributes.

## Copying with changes

The built-in behaviour can be changed for a whole *mode*, the set of
rules that are used together. With `on-no-match="shallow-copy"`, a node
that no rule matches is copied, and its children are processed. Now the
stylesheet copies the document as it is, and you only write rules for
what should change. This is the most useful pattern in XSLT:

`no-prices.xsl`:

```xml
{{#include ../examples/xslt/no-prices.xsl}}
```

```console
$ sxml transform no-prices.xsl library.xml | head -9
<?xml version="1.0" encoding="UTF-8"?><library>
  <shelf name="Computing">
    <book id="geb" year="1979" pages="777">
      <title>Godel, Escher, Bach</title>
      <author>Douglas Hofstadter</author>

      <keywords>logic, music, art</keywords>
    </book>
    <book id="taocp" year="1968" pages="672">
```

The rule for `price` is empty, so prices make nothing. The rule for
`tags` makes a new element instead. Everything else is copied by the
mode. (The blank line is the text node of spaces that came before the
price.)

The rest of this chapter goes through the parts of XSLT you'll use the
most:

- [Templates and Modes](xslt-templates.md): rules, patterns, priorities,
  parameters and modes;
- [Keys and Grouping](xslt-keys-grouping.md): looking things up, and
  making groups;
- [Many Outputs, Functions and Packages](xslt-packages.md): writing many
  files, and organizing larger stylesheets;
- [Streaming Large Files](xslt-streaming.md): transforming documents
  larger than memory.

Chapter 12 writes an OpenAPI document from an XSD schema with template
rules, in [A Pipeline, a Second Target and a Changing
Schema](codegen-pipeline.md).
