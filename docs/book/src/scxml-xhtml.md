# Documenting the Machine in XHTML

<!-- examples: scxml -->

The machine's annotations were written for people as much as for
checks: each state and each event has a paragraph of XHTML, each data
item names its type, and each payload names an element of
`latch-data.xsd`. A stylesheet can put all of it on one page, together
with what only the schema knows, such as what a door id may look like.
The page is XHTML, so the paragraphs of the machine can go into it as
they are.

## The stylesheet

`document.xsl`:

```xml
{{#include ../examples/scxml/document.xsl}}
```

It reads two documents: the machine, its source, and the schema of the
annotations, with `doc('latch-data.xsd')`. A schema is an XML document
like any other, so a stylesheet can read the declarations in it, just
as `check.xq` read the names of the payloads.

The XHTML namespace is the default namespace of the stylesheet, so the
literal result elements, `html`, `section`, `table`, are XHTML
elements. The same namespace is bound to the prefix `html` too, for the
patterns and paths that select the machine's XHTML: in a path, an
unprefixed name would mean no namespace, not the default namespace of
literal results. `exclude-result-prefixes` keeps the namespaces the
page doesn't use out of it.

`xsl:output method="xhtml"` writes the page as XHTML: an XML document
that browsers can also read as HTML, with the `<!DOCTYPE html>` of
`html-version="5"` and a `meta` element for the encoding.

## Writing the page

The machine is the one [Checking What the Schema
Cannot](scxml-checks.md) fixed, `lock-fixed.scxml`:

```console
$ sxml transform --output=lock.html document.xsl lock-fixed.scxml
$ sed -n '1,12p' lock.html
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <meta charset="UTF-8"/>
    <title>The door_lock machine</title>
  </head>
  <body>
    <h1>The door_lock machine</h1>
    <p>It starts in the state <a href="#locked">locked</a>.
          <span class="implements">Implements
      <a href="latch.html#req-1">req-1</a>.</span></p>
```

## The machine's own XHTML

Each state's section has its heading, then the content of the state's
`div`, copied with `xsl:copy-of`:

```console
$ sed -n '/<section id="lockout">/,/<\/section>/p' lock.html
    <section id="lockout"><h3>lockout</h3>
      <p>The lock refuses every code for <var>lockoutSeconds</var> seconds.</p>
    <span class="implements">Implements
      <a href="latch.html#req-5">req-5</a>, <a href="latch.html#req-8">req-8</a>.</span></section>
```

The `p` and the `var` inside it are copied as they are: they were
XHTML in the machine, and they are XHTML on the page. A copied element
takes along the namespace bindings in scope where it was, and in the
machine the namespace `urn:example:latch` was in scope everywhere.
`copy-namespaces="no"` leaves the bindings out that the copy doesn't
use. Without it, each paragraph carries the latch namespace into the
page:

```console
$ sed 's|<xsl:copy-of select="html:div/node()" copy-namespaces="no"/>|<xsl:copy-of select="html:div/node()"/>|' document.xsl > keep.xsl
$ sxml transform keep.xsl lock-fixed.scxml | sed -n '/<section id="lockout">/,/<\/section>/p'
    <section id="lockout"><h3>lockout</h3>
      <p xmlns:latch="urn:example:latch">The lock refuses every code for <var>lockoutSeconds</var> seconds.</p>
    <span class="implements">Implements
      <a href="latch.html#req-5">req-5</a>, <a href="latch.html#req-8">req-8</a>.</span></section>
```

It is not wrong, but it is noise in a page meant for browsers. The
`section` of a state is written on fewer lines than the rest: an
element with text of its own is mixed content, and the serializer
doesn't indent inside it, because new whitespace there would change
the text.

## A table of transitions

The transitions are a table, one row for each, in document order, with
links to the states' sections:

```console
$ sed -n '/<h2>Transitions/,/<\/table>/p' lock.html | head -26
    <h2>Transitions</h2>
    <table>
      <tr>
        <th>From</th>
        <th>Event</th>
        <th>Condition</th>
        <th>To</th>
      </tr>
      <tr>
        <td><a href="#locked">locked</a></td>
        <td>bolt.jammed</td>
        <td></td>
        <td><a href="#jammed">jammed</a></td>
      </tr>
      <tr>
        <td><a href="#locked">locked</a></td>
        <td>unlock.request</td>
        <td></td>
        <td><a href="#checking">checking</a></td>
      </tr>
      <tr>
        <td><a href="#locked">locked</a></td>
        <td>status.request</td>
        <td></td>
        <td>(stays)</td>
      </tr>
```

## Events and their payloads

Each event is a term of a definition list, with its XHTML paragraph.
For an event with a payload, the stylesheet finds the payload's
declaration in the schema and lists its children:

```console
$ sed -n '/<dt id="event-status.response">/,/<\/dd>/p' lock.html
      <dt id="event-status.response">status.response</dt>
      <dd>
        <p>The lock answers with its state.</p>
        <p>It carries a <code>latch:status</code>:</p>
        <ul>
          <li><code>door</code>,
          xs:string, matching [A-Za-z0-9\-]{1,16}</li>
          <li><code>state</code>,
          xs:string, one of locked, unlocked, lockout</li>
          <li><code>failed-attempts</code>,
          xs:nonNegativeInteger</li>
          <li><code>retry-after</code> (optional),
          xs:positiveInteger, at most 3600</li>
        </ul>
      </dd>
```

`@latch:payload` is a QName in the machine, written with the machine's
prefix. `resolve-QName` resolves it with the namespace bindings of its
element, and the stylesheet compares only the local name with the
schema's declarations; [Checking What the Schema
Cannot](scxml-checks.md) made sure that the payloads name elements of
this schema.

The types are described by the function `doc:describe`: the type's
`xs:documentation` when it has one, else its base type and its facets
in words. `matching [0-9]{4,8}` is the code's pattern, `at most 3600`
the maximum of `latch:seconds`, `one of locked, unlocked, lockout` an
enumeration. The types of `latch-data.xsd` have no documentation, so
all of them are described by their facets; a schema that documents its
types, as `latch.xsd` documents `keyword`, gets its own words on the
page. The data items are described the same way:

```console
$ sed -n '/<h2>Data/,/<\/table>/p' lock.html | grep 'code>'
        <td><code>latch:attempt-count</code>: xs:nonNegativeInteger</td>
        <td><code>latch:attempt-limit</code>: xs:positiveInteger, at most 10</td>
        <td><code>latch:seconds</code>: xs:positiveInteger, at most 3600</td>
        <td><code>latch:seconds</code>: xs:positiveInteger, at most 3600</td>
```

## Links to the draft

`latch:implements` becomes links to the requirements of the draft, in
the form `latch.html#req-8`: a link into an HTML version of the
draft, such as [Project: Publishing the Specification](xslt-project.md)
makes, in which each requirement's anchor is the `id` of its paragraph.
The parameter `draft` says
where the published draft is, relative to the page:

```console
$ sxml transform --param=draft=../spec/latch.html document.xsl lock-fixed.scxml | grep -m 2 'href="../spec'
      <a href="../spec/latch.html#req-1">req-1</a>.</span></p>
      <a href="../spec/latch.html#req-2">req-2</a>.</span></section>
```

The page is generated from the machine, so it can't fall behind it:
when the machine changes, the page is made again, and when the machine
names a requirement, the page links to its text. [Drawing the
Machine](scxml-drawing.md) adds a picture.
