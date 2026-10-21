# Templates and Modes

<!-- examples: spec -->

## Template rules

A template rule has a `match` pattern and a body. The body is a
*sequence constructor*: literal result elements (any element that is
not in the XSLT namespace, such as `<p>`), text, and instructions. This
stylesheet makes an HTML page of the draft, with one rule for each kind
of element:

`draft.xsl`:

```xml
{{#include ../examples/spec/draft.xsl:html}}
```

(The file goes on with the rules of a second mode, shown later in this
chapter.) Run it, and look at the first part of the page:

```console
$ sxml transform draft.xsl vane.xml | head -31
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>The Vane Protocol for Feature Flag Rollouts</title>
  </head>
  <body>
    <h1>The Vane Protocol for Feature Flag Rollouts</h1>
    <ol class="toc">
      <li><a href="#intro">Introduction</a><ol><li><a href="#terminology">Terminology</a></li></ol></li>
      <li><a href="#states">Flag States</a></li>
      <li><a href="#messages">Messages</a><ol><li><a href="#rollout">Rollout</a></li><li><a href="#status">Status</a></li></ol></li>
      <li><a href="#pause">Pause</a></li>
      <li><a href="#security">Security Considerations</a></li>
      <li><a href="#iana">IANA Considerations</a></li>
    </ol>
    <section id="intro">
      <h2>Introduction</h2>
      <p>An application has many features, and each feature has a flag. An
      <a href="#term-sdk">SDK</a> in the application keeps the flags
      in a cache and evaluates them for each user. It holds an update stream
      open to the Vane service, which sends a message on it whenever a flag
      changes, and it asks the service for the status of a flag.</p>
      <section id="terminology">
        <h2>Terminology</h2>
        <p>The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL
        NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED",
        "MAY", and "OPTIONAL" in this document are to be interpreted as
        described in BCP 14 <a href="#RFC2119">RFC2119</a> <a href="#RFC8174">RFC8174</a>
        when, and only when, they appear in all capitals, as shown here.</p>
        <dl>
```

Let's read it rule by rule:

- `match="/"` matches the document node, where every transformation
  starts. It writes the page, and `xsl:apply-templates` processes the
  draft's sections where the page's body goes.
- `match="section"` writes a `section` with a heading, and processes
  the section's children except its `name`. A nested section matches
  the same rule, so sections nest in the page as deep as in the draft.
- `match="t"` writes a paragraph, and `match="xref"` a link. An `xref`
  with no content shows its target's anchor for now; [Keys, Numbers and
  Grouping](xslt-keys-grouping.md) makes "Section 4" of it.
- `match="dl | dd"` uses `xsl:copy`, which copies the element without
  its attributes or children: the content comes from the rules again.
  A pattern with `|` matches either kind of node.
- The text in between is copied by the built-in rule, so the words of a
  paragraph pass through with the elements inside them turned into HTML.

`xsl:apply-templates` is the engine of XSLT. It selects nodes (with
`select`, or the children by default), and for each one it finds the
best matching rule. The `xsl:output` element says how to write the
result: here as HTML, indented. The `html` method writes the
`<!DOCTYPE html>` and the `meta` element for the encoding itself.

## Text value templates

With `expand-text="yes"` on the stylesheet, curly braces in text work as
they do in XQuery: `{name}` is replaced by the value of the expression
`name`, the section's title. In attributes of literal result elements,
curly braces always work; they are *attribute value templates*:
`id="{@anchor}"` gives the HTML element the draft's anchor, so the links
work. Without `expand-text`, you write `<xsl:value-of select="name"/>`
instead, as older stylesheets do and as the `bcp14` rule does once.

## Priorities

Two rules match a requirement: `t` and `t[@anchor]`. When several rules
match, the one with the highest *priority* wins. A rule can state its
priority, as the second one does. Otherwise, the priority comes from the
pattern: a pattern with a predicate or several steps is more specific
than a plain name, and gets a higher default priority (0.5 against 0).
So the `priority="1"` above isn't even needed; it just makes the intent
clear. The requirements get an `id` and a class:

```console
$ sxml transform draft.xsl vane.xml | grep -A2 'id="req-1"'
      <p id="req-1" class="requirement">A <a href="#term-flag">flag</a>
      <span class="bcp14 absolute">MUST</span> be in exactly one of the states "stable",
      "rolling-out" and "paused".</p>
```

Patterns are not expressions that are evaluated from the top: a pattern
tests a node. `t` matches any `t` element anywhere, `section/t` a
paragraph whose parent is a section (not one in the abstract), and
`section[1]` a section that is the first section child of its parent.

## Named templates and parameters

The `bcp14` rule shows several more things:

- A template with a `name` instead of a `match` is a *named template*.
  `xsl:call-template` calls it, and `xsl:with-param` passes its
  parameters, which can have types: `strength` takes an `xs:string`.
- `xsl:attribute` makes an attribute from a sequence constructor, here
  the text the named template writes.
- `xsl:choose` picks the first `xsl:when` whose test is true, or
  `xsl:otherwise`. There is also `xsl:if` for a single test, as in the
  `cref` rule.
- A stylesheet declares every prefix it uses, `xs` too: here
  `xmlns:xs="http://www.w3.org/2001/XMLSchema"` on `xsl:stylesheet`
  makes `xs:string` a type, and `exclude-result-prefixes="xs"` keeps
  the declaration out of the result. Unlike XPath and XQuery, XSLT
  predeclares no prefix but `xml`; an undeclared one is an error
  (XPST0081 in an expression or a type such as `as`, XTSE0280 in a name
  such as `mode`).

`xsl:param` at the top level declares a *stylesheet parameter*, with a
default. `notes` decides whether the editorial notes show, and
`--param` sets it from the command line:

```console
$ sxml transform --param=notes=show draft.xsl vane.xml | grep 'class="note"'
      <span class="note">[AM: Should an owner be able to change the duration?]</span></p>
      <span class="note">[AB: This needs a requirement anchor of its own.]</span></p>
```

## Modes

The page processes the same sections twice: once for the table of
contents and once for the content. A *mode* is a named set of rules.
`xsl:apply-templates mode="toc"` uses only the rules of the `toc` mode,
so a section there makes a list item with a link, not a `section`
element. Each mode has its own built-in rules, too, which is why the
`toc` mode has a rule for the document node: without it, the built-in
rule would copy all the text of the draft into the table of contents.

A mode can do more than a part of the page. The rest of `draft.xsl` is
a second rendering of the whole draft, as plain text in the style of an
RFC: section numbers, paragraphs indented by three spaces and wrapped
at 72 columns, and a reference written as `[RFC2119]`:

```xml
{{#include ../examples/spec/draft.xsl:text}}
```

The `t` rule applies the rules of the mode to the paragraph's children
inside an `xsl:variable`, so the variable holds the paragraph's text
with every `xref` and `bcp14` already turned into text. `normalize-space`
joins its lines, and one `replace` breaks it again into lines of at
most 69 characters plus the indent: `(.{1,69})( |$)` takes as many
characters as fit before a space or the end. The section number is the
position of each enclosing section among its siblings, joined with
dots.

`xsl:value-of` with `separator=""` joins the lines of the ABNF without
a space between them, and `xsl:text` writes text exactly, which keeps
the whitespace of the stylesheet out of the result: `&#10;` is a line
feed.

## Starting somewhere else

A transformation normally starts by applying templates to the document
node in the default mode. `--initial-mode` starts in another mode. The
`toc` mode alone is a table of contents:

```console
$ sxml transform --initial-mode=toc draft.xsl vane.xml
<ol class="toc">
  <li><a href="#intro">Introduction</a><ol><li><a href="#terminology">Terminology</a></li></ol></li>
  <li><a href="#states">Flag States</a></li>
  <li><a href="#messages">Messages</a><ol><li><a href="#rollout">Rollout</a></li><li><a href="#status">Status</a></li></ol></li>
  <li><a href="#pause">Pause</a></li>
  <li><a href="#security">Security Considerations</a></li>
  <li><a href="#iana">IANA Considerations</a></li>
</ol>
```

And the `text` mode is the plain-text draft. Its result is text, not
HTML, so `--method=text` replaces the stylesheet's output method:

```console
$ sxml transform --initial-mode=text --method=text draft.xsl vane.xml | head -30
The Vane Protocol for Feature Flag Rollouts

1.  Introduction

   An application has many features, and each feature has a flag. An SDK
   in the application keeps the flags in a cache and evaluates them for
   each user. It holds an update stream open to the Vane service, which
   sends a message on it whenever a flag changes, and it asks the
   service for the status of a flag.

1.1.  Terminology

   The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
   "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and
   "OPTIONAL" in this document are to be interpreted as described in BCP
   14 [RFC2119] [RFC8174] when, and only when, they appear in all
   capitals, as shown here.

   Flag
      A named switch in an application that turns a feature on for a
      share of its users.

   SDK
      The library in an application that evaluates flags from its cache
      and keeps the cache current with Vane messages.

   Percentage
      A whole number from 0 to 100 that an owner gives to roll a flag
      out to that share of users. An SDK puts a user in the share by a
      hash of the flag and the user, so the user stays in it while the
$ sxml transform --initial-mode=text --method=text draft.xsl vane.xml | grep 'in a pause ('
   A service MUST NOT roll out a flag while it is in a pause ([pause]).
```

The reference to Section 4 still shows as `[pause]`: an `xref`
without text needs a lookup of its target, the subject of the next
section.

`--initial-template` calls a named template instead of applying
templates. A stylesheet that needs no input names its starting template
`xsl:initial-template`, and runs without a document:

`bcp14.xsl`:

```xml
{{#include ../examples/spec/bcp14.xsl}}
```

```console
$ sxml transform bcp14.xsl
BCP 14 has 11 key words: MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT, RECOMMENDED, NOT RECOMMENDED, MAY, OPTIONAL.
```
