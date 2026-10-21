# Transforming with XSLT

<!-- examples: spec -->

XQuery describes a result by computing it: for each requirement, make
this element. XSLT describes it by *rules*: when you meet a requirement,
make this element. A stylesheet is a set of *template rules*, each with
a *pattern* that says which nodes it applies to. The processor walks the
input, finds the best rule for each node, and the rules decide whether
and how to go on to the children.

That way of working fits documents well, and the Latch draft is a
document: paragraphs hold text, cross references, key words and notes
in any order, and sections nest as deep as the authors like. Rules say
what to do with each part without knowing the whole structure in
advance. It is also what makes XSLT easy to extend: a new stylesheet
can import an old one and override only the rules it wants to change.

A stylesheet is itself an XML document, with its instructions in the
XSLT namespace, `http://www.w3.org/1999/XSL/Transform`, usually with the
prefix `xsl`. This chapter turns the draft, `latch.xml` in
`docs/book/examples/spec`, into the things a specification needs: a
copy for publication, an HTML page, a plain-text version in the style of
an RFC, and indexes. Run the commands in that directory.

## The built-in rules

The smallest stylesheet has no rules at all:

`empty.xsl`:

```xml
{{#include ../examples/spec/empty.xsl}}
```

Even so, it does something:

```console
$ sxml transform empty.xsl latch.xml | grep -v '^ *$' | head -12
<?xml version="1.0" encoding="UTF-8"?>
    The Latch Protocol for Remote Door Locks
      Example Locks
      ada@example.com
      Example Locks
      alan@example.com
    Applications
    lock
    access control
      This document specifies Latch, a line-based protocol between a
      controller and the door locks it manages. It defines the states of a
      lock, the messages that change them, and the lockout that protects a
```

When no rule matches a node, a *built-in rule* applies. For elements
and the document node, it processes the children. For text, it copies
the text. So an empty stylesheet writes all the text of a document,
without the tags: the title, the authors' organizations and email
addresses, the keywords, the abstract and then the rest of the draft.
(`grep` leaves out the lines that hold only the spaces between
elements; they are text too, and the empty stylesheet copies them.)
The attributes are gone, `docName` and every `anchor`, because the
built-in rules don't process attributes, and so is the comment at the
top of the file: the built-in rule for comments and processing
instructions makes nothing.

## Copying with changes

The built-in behaviour can be changed for a whole *mode*, the set of
rules that are used together. With `on-no-match="shallow-copy"`, a node
that no rule matches is copied, and its children are processed. Now the
stylesheet copies the document as it is, and you only write rules for
what should change. This is the most useful pattern in XSLT.

Before an Internet-Draft is published, the editorial notes (`cref`)
come out, and an IANA section that only says there is nothing to do is
removed:

`publication.xsl`:

```xml
{{#include ../examples/spec/publication.xsl}}
```

```console
$ sxml transform publication.xsl latch.xml | head -4
<?xml version="1.0" encoding="UTF-8"?><rfc version="3" category="std" ipr="trust200902" submissionType="IETF" docName="draft-example-latch-protocol-01">
  <front>
    <title abbrev="Latch">The Latch Protocol for Remote Door Locks</title>
    <seriesInfo name="Internet-Draft" value="draft-example-latch-protocol-01"/>
```

The rules with an empty body make nothing, so the notes, the comment and
the IANA section are gone; everything else is copied with its
attributes. A pattern is not limited to a name: `/comment()` is a
comment whose parent is the document node, and the predicates on
`section` say which section. Save the result and compare:

```console
$ sxml transform --output=public.xml publication.xsl latch.xml
$ sxml xpath --context-item=latch.xml 'count(//cref), count(//section)'
2
9
$ sxml xpath --context-item=public.xml 'count(//cref), count(//section)'
0
8
$ sxml xpath --context-item=public.xml '//t[@anchor = "req-8"]'
<t anchor="req-8">A lockout <bcp14>SHOULD</bcp14> last five minutes.
      </t>
```

The space before the note is still there: it is a text node of its own,
and only the note's element was left out. A rule's body could just as
well write a new element in the old one's place, as the next sections
do for every element of the draft.

The rest of this chapter goes through the parts of XSLT you'll use the
most:

- [Templates and Modes](xslt-templates.md): rules, patterns,
  priorities, parameters and modes, for an HTML page and a plain-text
  draft;
- [Keys, Numbers and Grouping](xslt-keys-grouping.md): looking up the
  targets of cross references, numbering sections and requirements, an
  index of terms and the references;
- [Many Outputs, Functions and Packages](xslt-packages.md): a page for
  each section, and the stylesheets organized as modules and a package;
- [Project: Publishing the Specification](xslt-project.md): one script
  that checks the draft and writes HTML, text, JSON and a page of
  changes.

XSLT also draws: [Drawing with XSLT](svg-xslt.md) writes SVG with
template rules, and [Transforming SVG](svg-transform.md) changes
drawings it did not make. [Printing the Tree with
XSLT](codegen-printer.md) prints a tree of TypeScript code as text with
one rule for each kind of element, and [A Pipeline, a Second Target and
a Changing Schema](codegen-pipeline.md) writes an OpenAPI document from
an XSD schema with template rules.
