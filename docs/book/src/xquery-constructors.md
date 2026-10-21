# Building XML

<!-- examples: spec -->

XQuery builds new XML with *constructors*. The most natural kind is
written as XML itself. In this section we'll build an index of the
draft's requirements, first as plain XML and then as an XHTML page.

## Direct constructors

An element in a query is an element in the result. Curly braces inside
it hold expressions, whose values become content:

```console
$ sxml xquery --context-item=latch.xml --expression='<p>The draft has { count(//t[@anchor]) } requirements.</p>'
<p>The draft has 11 requirements.</p>
```

In an attribute, curly braces work the same way:

```console
$ sxml xquery --context-item=latch.xml --expression='<a href="#{ (//t[@anchor])[1]/@anchor }" title="{ (//t[@anchor])[1]/bcp14 }">first</a>'
<a href="#req-1" title="MUST">first</a>
```

To write a curly brace as text, double it: `{{` and `}}`.

## What goes inside

Nodes in an enclosed expression are *copied* into the new element.
Atomic values become text, and neighbouring atomic values are separated
by one space:

```console
$ sxml xquery --context-item=latch.xml --expression='<keywords>{ //section[@anchor = "unlock"]//bcp14 }</keywords>'
<keywords><bcp14>MUST</bcp14><bcp14>MUST NOT</bcp14><bcp14>MAY</bcp14></keywords>
$ sxml xquery --context-item=latch.xml --expression='<keywords>{ //section[@anchor = "unlock"]//bcp14/string() }</keywords>'
<keywords>MUST MUST NOT MAY</keywords>
$ sxml xquery --expression='<p>{ "a", "b" }</p>, <p>{ "a" }{ "b" }</p>'
<p>a b</p><p>ab</p>
```

The second result shows why the space is not always what you want: "MUST
MUST NOT MAY" could be four keywords or three. When the values need a
clear separator, join them yourself with `string-join(..., ", ")`.

Constructors nest, and enclosed expressions can hold any query, FLWOR
expressions included. This query makes an index of the requirements,
section by section:

`index.xq`:

```xquery
{{#include ../examples/spec/index.xq}}
```

```console
$ sxml xquery --context-item=latch.xml index.xq
<requirements draft="draft-example-latch-protocol-01">
  <section anchor="states" name="Lock States">
    <requirement anchor="req-1">MUST</requirement>
    <requirement anchor="req-2">MUST</requirement>
    <requirement anchor="req-3">SHOULD</requirement>
  </section>
  <section anchor="unlock" name="Unlock">
    <requirement anchor="req-4">MUST</requirement>
    <requirement anchor="req-5">MUST NOT</requirement>
    <requirement anchor="req-9">MAY</requirement>
  </section>
  <section anchor="status" name="Status">
    <requirement anchor="req-6">MUST</requirement>
    <requirement anchor="req-10">SHOULD NOT</requirement>
  </section>
  <section anchor="lockout" name="Lockout">
    <requirement anchor="req-7">MUST</requirement>
    <requirement anchor="req-8">SHOULD</requirement>
    <requirement anchor="req-11">MAY</requirement>
  </section>
</requirements>
```

`//section[t/@anchor]` is every section that has a requirement of its
own; Messages has only subsections, so it isn't there. The value of an
attribute such as `name="{ $section/name }"` is the string value of the
`name` element: an attribute holds text, so an element in it is
atomized.

## Computed constructors

When the name of an element is not known until the query runs, use a
*computed constructor*: the keyword `element`, the name (or an
expression for it in braces), and the content in braces. Here each
requirement becomes an element named after its keyword:

```console
$ sxml xquery --context-item=latch.xml --expression='//t[@anchor] ! element { lower-case(translate(bcp14, " ", "-")) } { @anchor }'
<must anchor="req-1"/><must anchor="req-2"/><should anchor="req-3"/><must anchor="req-4"/><must-not anchor="req-5"/><may anchor="req-9"/><must anchor="req-6"/><should-not anchor="req-10"/><must anchor="req-7"/><should anchor="req-8"/><may anchor="req-11"/>
```

The keyword "MUST NOT" has a space, which a name can't have, so
`translate` makes it a hyphen. The content `{ @anchor }` is an attribute
node, and an attribute node in the content of an element becomes an
attribute of it. `attribute`, `text`, `comment`, `document` and
`processing-instruction` constructors work the same way:

```console
$ sxml xquery --expression='let $name := "anchor" return element t { attribute { $name } { "req-12" }, "A new requirement." }'
<t anchor="req-12">A new requirement.</t>
$ sxml xquery --expression='<a>{ comment { "made by sxml" } }</a>, processing-instruction xml-stylesheet { "href=""latch.css""" }, text { "plain" }'
<a><!--made by sxml--></a><?xml-stylesheet href="latch.css"?>plain
```

A doubled quote, `""`, is a quote inside a string literal.

## Namespaces

A namespace declaration on a direct constructor works as it does in
XML. In the prolog, `declare namespace` binds a prefix for the whole
query:

```console
$ sxml xquery --context-item=latch.xml --expression='<h:p xmlns:h="http://www.w3.org/1999/xhtml">{ count(//t[@anchor]) } requirements</h:p>'
<h:p xmlns:h="http://www.w3.org/1999/xhtml">11 requirements</h:p>
$ sxml xquery --context-item=latch.xml --expression='declare namespace h = "http://www.w3.org/1999/xhtml"; <h:p>{ count(//t[@anchor]) } requirements</h:p>'
<h:p xmlns:h="http://www.w3.org/1999/xhtml">11 requirements</h:p>
```

An XHTML page is easier to write without a prefix on every element.
`declare default element namespace` gives unprefixed element names a
namespace, in constructors *and in paths*. That second part catches
everyone once: the draft's elements are in no namespace, so `//t` now
looks for XHTML `t` elements, and finds none:

```console
$ sxml xquery --context-item=latch.xml --expression='declare default element namespace "http://www.w3.org/1999/xhtml"; <p>{ count(//t) }</p>'
<p xmlns="http://www.w3.org/1999/xhtml">0</p>
```

`Q{}t` is the name `t` in no namespace, whatever the default is. The
index as a page:

`index-page.xq`:

```xquery
{{#include ../examples/spec/index-page.xq}}
```

```console
$ sxml xquery --context-item=latch.xml index-page.xq
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>Requirements of draft-example-latch-protocol-01</title>
  </head>
  <body>
    <h1>The Latch Protocol for Remote Door Locks</h1>
    <ul>
      <li id="req-1"><b>MUST</b> in Lock States</li>
      <li id="req-2"><b>MUST</b> in Lock States</li>
      <li id="req-3"><b>SHOULD</b> in Lock States</li>
      <li id="req-4"><b>MUST</b> in Unlock</li>
      <li id="req-5"><b>MUST NOT</b> in Unlock</li>
      <li id="req-9"><b>MAY</b> in Unlock</li>
      <li id="req-6"><b>MUST</b> in Status</li>
      <li id="req-10"><b>SHOULD NOT</b> in Status</li>
      <li id="req-7"><b>MUST</b> in Lockout</li>
      <li id="req-8"><b>SHOULD</b> in Lockout</li>
      <li id="req-11"><b>MAY</b> in Lockout</li>
    </ul>
  </body>
</html>
```

Whitespace between two tags, or between a tag and an enclosed
expression, like the line break after `<li ...>`, is *boundary whitespace* and is dropped, so the
query's own indentation doesn't end up in the page. Text with anything
other than whitespace in it, such as `" in "`, is kept as written,
spaces and all. (`declare boundary-space preserve` keeps boundary
whitespace too.)

[Writing SVG with XQuery](svg-xquery.md) builds drawings in the SVG
namespace the same way, and goes deeper into namespaces in
constructors.

## Not only XML

A query can build maps and arrays as well as XML, and the output
declarations decide how the result is written. With the json method,
the requirements become JSON:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare option output:method "json";
    declare option output:indent "yes";
    array { (//t[@anchor])[position() le 2] ! { "anchor": string(@anchor), "keyword": string(bcp14) } }'
[
  {
    "anchor": "req-1",
    "keyword": "MUST"
  },
  {
    "anchor": "req-2",
    "keyword": "MUST"
  }
]
```

The output declarations are `output:method`, `output:indent`,
`output:item-separator`, `output:omit-xml-declaration` and the other
parameters of *XSLT and XQuery Serialization 4.0*. On the command line,
`--method` overrides the method. [Writing Results](xquery-serialization.md)
goes through them.
