# Templates and Modes

<!-- examples: xslt -->

## Template rules

A template rule has a `match` pattern and a body. The body is a
*sequence constructor*: literal result elements (any element that is
not in the XSLT namespace, such as `<li>`), text, and instructions. This
stylesheet makes an HTML page of the library:

`catalog.xsl`:

```xml
{{#include ../examples/xslt/catalog.xsl}}
```

```console
$ sxml transform catalog.xsl library.xml
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>Library</title>
  </head>
  <body>
    <h2>Computing</h2>
    <ul>
      <li><cite>Godel, Escher, Bach</cite> by Douglas Hofstadter</li>
      <li class="classic"><cite>The Art of Computer Programming</cite> by Donald Knuth</li>
      <li><cite>Structure and Interpretation of Computer Programs</cite> by Harold Abelson and Gerald Jay Sussman</li>
    </ul>
    <h2>Fiction</h2>
    <ul>
      <li class="classic"><cite>Dune</cite> by Frank Herbert</li>
      <li class="classic"><cite>The Left Hand of Darkness</cite> by Ursula K. Le Guin</li>
    </ul>
  </body>
</html>
```

Let's read it rule by rule:

- `match="/"` matches the document node, where every transformation
  starts. It writes the page, and `xsl:apply-templates` processes the
  shelves where the page's body goes.
- `match="shelf"` writes a heading and a list, and processes the books
  of the shelf inside the list.
- `match="book"` writes a list item for a book.
- `match="book[@year &lt; 1970]"` also matches some books. Patterns use
  XPath syntax, predicates included, and `&lt;` is how XML writes `<`
  inside an attribute.

`xsl:apply-templates` is the engine of XSLT. It selects nodes (with
`select`, or the children by default), and for each one it finds the
best matching rule. The `xsl:output` element says how to write the
result: here as HTML, indented. The `html` method writes the
`<!DOCTYPE html>` and the `meta` element for the encoding itself.

## Text value templates

With `expand-text="yes"` on the stylesheet, curly braces in text work as
they do in XQuery: `{title}` is replaced by the value of the expression
`title`. In attributes of literal result elements, curly braces always
work; they are *attribute value templates*. Without `expand-text`, you
write `<xsl:value-of select="title"/>` instead, as older stylesheets do.

## Priorities

Two rules match the old books: `book` and `book[@year &lt; 1970]`. When
several rules match, the one with the highest *priority* wins. A rule
can state its priority, as the second one does. Otherwise, the priority
comes from the pattern: a pattern with a predicate or several steps is
more specific than a plain name, and gets a higher default priority
(0.5 against 0). So the `priority="1"` above isn't even needed; it just
makes the intent clear.

Patterns are not expressions that are evaluated from the top: a pattern
tests a node. `book` matches any `book` element anywhere,
`shelf/book` a book whose parent is a shelf, and `book[1]` a book that
is the first book child of its parent.

## Modes

Sometimes you need to process the same nodes twice, in different ways:
once for a table of contents and once for the content. A *mode* is a
named set of rules. `xsl:apply-templates mode="contents"` uses only the
rules of the `contents` mode:

`page.xsl`:

```xml
{{#include ../examples/xslt/page.xsl}}
```

```console
$ sxml transform page.xsl library.xml
<!DOCTYPE html>
<html>
  <body>
    <ol class="contents">
      <li><a href="#dune">Dune</a></li>
      <li><a href="#geb">Godel, Escher, Bach</a></li>
      <li><a href="#sicp">Structure and Interpretation of Computer Programs</a></li>
      <li><a href="#taocp">The Art of Computer Programming</a></li>
      <li><a href="#lhod">The Left Hand of Darkness</a></li>
    </ol>
    <section id="geb">
      <h2>Godel, Escher, Bach</h2>
      <p>22.50 USD</p>
    </section>
    <section id="taocp">
      <h2>The Art of Computer Programming</h2>
      <p>An investment: 180.00 USD</p>
    </section>
    <section id="sicp">
      <h2>Structure and Interpretation of Computer Programs</h2>
      <p>55.00 USD</p>
    </section>
  </body>
</html>
```

This stylesheet shows several more things:

- `xsl:sort` inside `xsl:apply-templates` sorts the nodes before they
  are processed.
- `xsl:param` at the top level declares a *stylesheet parameter*, with a
  default. Here it chooses which books get a section.
- A template with a `name` instead of a `match` is a *named template*.
  `xsl:call-template` calls it, and `xsl:with-param` passes its
  parameters, which can have types.
- `xsl:choose` picks the first `xsl:when` whose test is true (there is
  also `xsl:if` for a single test).
- `xsl:text` writes text exactly, which keeps the whitespace of the
  stylesheet out of the result.

`--param` sets a stylesheet parameter from the command line:

```console
$ sxml transform --param=currency=EUR page.xsl library.xml | grep '<p>'
      <p>A bargain: 9.99 EUR</p>
      <p>A bargain: 11.50 EUR</p>
```

## Starting somewhere else

A transformation normally starts by applying templates to the document
node in the default mode. `--initial-mode` starts in another mode, and
`--initial-template` calls a named template instead. A stylesheet that
needs no input can name its starting template `xsl:initial-template`:

`hello.xsl`:

```xml
{{#include ../examples/xslt/hello.xsl}}
```

```console
$ sxml transform hello.xsl
Hello from XSLT 4.0
```
