# Keys and Grouping

<!-- examples: xslt -->

## Keys

A *key* is an index that the processor builds for you. You declare what
to index and by which value, and the `key` function looks nodes up. This
stylesheet lists the loans from Chapter 3 with the title of each book,
looked up in the library by id:

`loans.xsl`:

```xml
{{#include ../examples/xslt/loans.xsl}}
```

```console
$ sxml transform loans.xsl loans.xml
2026-09-02  ada  Dune
2026-09-05  alan  Godel, Escher, Bach
2026-09-14  grace  Dune
2026-09-20  ada  Structure and Interpretation of Computer Programs
2026-10-01  ada  The Left Hand of Darkness
2026-10-03  alan  Dune
```

`key('book', @book, $library)` finds the books whose `@id` equals the
loan's `@book`, in the document `$library`. Without the third argument,
`key` searches the document of the context node. A predicate such as
`$library//book[@id = current()/@book]` would give the same result, but
the key is built once and each lookup is fast, however many books there
are.

The `&#10;` at the end of the text is a line feed.

## Grouping

`xsl:for-each-group` puts the selected items into groups and processes
each group once. Inside it, `current-grouping-key()` is the key of the
group, and `current-group()` its items.

`group-by` groups by a computed value. A book with several tags is in
several groups:

`by-tag.xsl`:

```xml
{{#include ../examples/xslt/by-tag.xsl}}
```

```console
$ sxml transform by-tag.xsl library.xml
algorithms: taocp, sicp
art: geb
desert: dune
lisp: sicp
logic: geb
music: geb
politics: dune, lhod
winter: lhod
```

## Grouping by position

Grouping is not only for values. Documents often have structure that is
only implied by the order of elements, such as headings followed by
paragraphs:

`article.xml`:

```xml
{{#include ../examples/xslt/article.xml}}
```

`group-starting-with` starts a new group at each item that matches a
pattern, which turns the flat list into sections:

`sections.xsl`:

```xml
{{#include ../examples/xslt/sections.xsl}}
```

```console
$ sxml transform sections.xsl article.xml
<article>
  <section title="Getting Started">
    <p>Install sxml.</p>
    <p>Run a first expression.</p>
  </section>
  <section title="XPath">
    <p>Paths find nodes.</p>
  </section>
  <section title="XQuery">
    <p>FLWOR expressions.</p>
    <p>Constructors.</p>
  </section>
</article>
```

There are two more ways to form groups. `group-adjacent` puts
neighbouring items with the same key together, such as runs of
paragraphs of the same style, and `group-ending-with` ends a group at
each item that matches a pattern.

## Sorting and numbering

`xsl:sort` sorts the items of `xsl:apply-templates`, `xsl:for-each`
and `xsl:for-each-group`, as in `by-tag.xsl`. Several sorts make
several sort keys, and `order="descending"`, `data-type` and `lang`
refine them. `xsl:number` writes the position of a node in many
formats, such as `1.`, `a)`, `iv` or `1.2.3` for nested sections.
