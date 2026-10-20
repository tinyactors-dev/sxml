# Building XML

<!-- examples: xquery -->

XQuery builds new XML with *constructors*. The most natural kind is
written as XML itself.

## Direct constructors

An element in a query is an element in the result. Curly braces inside
it hold expressions, whose values become content:

```console
$ sxml xquery --expression='<p>Two plus two is { 2 + 2 }.</p>'
<p>Two plus two is 4.</p>
```

In an attribute, curly braces work the same way:

```console
$ sxml xquery --context-item=library.xml --expression='<book id="{ (//book)[1]/@id }" pages="{ 700 + 77 }"/>'
<book id="geb" pages="777"/>
```

To write a curly brace as text, double it: `{{` and `}}`.

## What goes inside

Nodes in an enclosed expression are *copied* into the new element.
Atomic values become text, and neighbouring atomic values are separated
by one space:

```console
$ sxml xquery --context-item=library.xml --expression='<tags>{ (//book)[1]/tags/tag }</tags>'
<tags><tag>logic</tag><tag>music</tag><tag>art</tag></tags>
$ sxml xquery --context-item=library.xml --expression='<tags>{ (//book)[1]/tags/tag/string() }</tags>'
<tags>logic music art</tags>
$ sxml xquery --expression='<p>{ "a", "b" }</p>, <p>{ "a" }{ "b" }</p>'
<p>a b</p><p>ab</p>
```

Constructors nest, and enclosed expressions can hold any query, FLWOR
expressions included. This query makes a summary of the members and
the books they borrowed:

`members.xq`:

```xquery
{{#include ../examples/xquery/members.xq}}
```

```console
$ sxml xquery --context-item=library.xml members.xq
<members>
  <member name="ada" loans="3">
    <book>Dune</book>
    <book>Structure and Interpretation of Computer Programs</book>
    <book>The Left Hand of Darkness</book>
  </member>
  <member name="alan" loans="2">
    <book>Godel, Escher, Bach</book>
    <book>Dune</book>
  </member>
  <member name="grace" loans="1">
    <book>Dune</book>
  </member>
</members>
```

## Computed constructors

When the name of an element is not known until the query runs, use a
*computed constructor*: the keyword `element`, the name (or an
expression for it in braces), and the content in braces. `attribute`,
`text`, `comment`, `document` and `processing-instruction` work the same
way:

```console
$ sxml xquery --expression='let $name := "chapter" return element { $name } { attribute number { 1 }, "Text" }'
<chapter number="1">Text</chapter>
$ sxml xquery --expression='<a>{ comment { "made by sxml" } }</a>'
<a><!--made by sxml--></a>
```

## Namespaces

A namespace declaration on a direct constructor works as it does in
XML:

```console
$ sxml xquery --expression='<atom:feed xmlns:atom="http://www.w3.org/2005/Atom"><atom:title>Books</atom:title></atom:feed>'
<atom:feed xmlns:atom="http://www.w3.org/2005/Atom"><atom:title>Books</atom:title></atom:feed>
```

In the prolog, `declare namespace` binds a prefix for the whole query,
and `declare default element namespace` gives unprefixed element names a
namespace, which is handy for XHTML:

```console
$ sxml xquery --expression='declare default element namespace "http://www.w3.org/1999/xhtml"; <html><body><p>Hi</p></body></html>'
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hi</p></body></html>
```

## Not only XML

A query can build maps and arrays as well as XML, and the output
declarations decide how the result is written. With the json method,
the same library becomes JSON:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare option output:method "json";
    declare option output:indent "yes";
    array { (//book)[position() le 2] ! { "id": string(@id), "year": xs:integer(@year) } }'
[
  {
    "id": "geb",
    "year": 1979
  },
  {
    "id": "taocp",
    "year": 1968
  }
]
```

The output declarations are `output:method`, `output:indent`,
`output:item-separator`, `output:omit-xml-declaration` and the other
parameters of *XSLT and XQuery Serialization 4.0*. On the command line,
`--method` overrides the method. [Writing Results](xquery-serialization.md)
goes through them.
