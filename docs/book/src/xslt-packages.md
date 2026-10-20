# Many Outputs, Functions and Packages

<!-- examples: xslt -->

## Writing many files

A transformation has one *principal result*, the one that goes to
standard output, and can make any number of *result documents* with
`xsl:result-document`. This stylesheet makes a page for each book and an
index that links to them:

`pages.xsl`:

```xml
{{#include ../examples/xslt/pages.xsl}}
```

sxml writes files only where you allow it. Without
`--result-directory`, it writes the principal result and tells you
about each result document it did not write:

```console
$ sxml transform pages.xsl library.xml
sxml transform: result document 'books/geb.html' not written; give --result-directory
sxml transform: result document 'books/taocp.html' not written; give --result-directory
sxml transform: result document 'books/sicp.html' not written; give --result-directory
sxml transform: result document 'books/dune.html' not written; give --result-directory
sxml transform: result document 'books/lhod.html' not written; give --result-directory
<ul>
  <li><a href="books/geb.html">Godel, Escher, Bach</a></li>
  <li><a href="books/taocp.html">The Art of Computer Programming</a></li>
  <li><a href="books/sicp.html">Structure and Interpretation of Computer Programs</a></li>
  <li><a href="books/dune.html">Dune</a></li>
  <li><a href="books/lhod.html">The Left Hand of Darkness</a></li>
</ul>
```

With `--result-directory`, the result documents are written below that
directory, and `--output` puts the principal result into a file too:

```console
$ mkdir site
$ sxml transform --result-directory=site --output=site/index.html pages.xsl library.xml
$ find site -type f | sort
site/books/dune.html
site/books/geb.html
site/books/lhod.html
site/books/sicp.html
site/books/taocp.html
site/index.html
$ cat site/books/dune.html
<h1>Dune</h1>
<p>Frank Herbert, 1965</p>
```

All the files are written only when the whole transformation succeeds,
and a result document that would land outside the directory is an
error. Run the same command again, and sxml refuses to replace the
files without `--force`.

## Importing stylesheets

`xsl:import` brings in the rules of another stylesheet, with a lower
*import precedence*: a rule in the importing stylesheet beats any rule
it imports, whatever their priorities. That is how you extend a
stylesheet without changing it:

`classics.xsl`:

```xml
{{#include ../examples/xslt/classics.xsl}}
```

```console
$ sxml transform classics.xsl library.xml | grep '<li'
      <li><cite>Godel, Escher, Bach</cite> by Douglas Hofstadter</li>
      <li><cite>The Art of Computer Programming</cite> ★</li>
      <li><cite>Structure and Interpretation of Computer Programs</cite> by Harold Abelson and Gerald Jay Sussman</li>
      <li><cite>Dune</cite> ★</li>
      <li><cite>The Left Hand of Darkness</cite> ★</li>
```

`xsl:include` is the other way to combine modules: the included rules
count as if they were written in the including stylesheet.

## Functions

`xsl:function` declares a function that XPath expressions in the
stylesheet can call, with typed parameters and result. Its name must
have a namespace prefix of your own. The package in the next section
declares two.

## Packages

A *package* is a stylesheet module with a name and a version, whose
components (functions, templates, variables, modes) are private unless
it says otherwise. Other stylesheets *use* a package and see only what
it makes public. Packages let a team share a library of functions
without the risk of names clashing or of rules leaking into each other.

`money.xsl`:

```xml
{{#include ../examples/xslt/money.xsl}}
```

The variable `money:rates` is private: only the package's own functions
can see it. The two functions are public. A stylesheet uses the package
with `xsl:use-package`:

`prices.xsl`:

```xml
{{#include ../examples/xslt/prices.xsl}}
```

```console
$ sxml transform prices.xsl library.xml
Godel, Escher, Bach: 20.70 EUR
The Art of Computer Programming: 165.60 EUR
Structure and Interpretation of Computer Programs: 50.60 EUR
Dune: 9.99 EUR
The Left Hand of Darkness: 11.50 EUR
Total: 258.39 EUR
```

The `sxml` command finds a package by reading its name as a file name,
relative to the stylesheet. A program that embeds sxml can find
packages any way it likes: by name and version, in a database, or built
in (Chapter 9).

## Transformations from queries

`fn:transform` runs a stylesheet from XPath or XQuery and returns its
results in a map. With the `sxml` command, it reads the stylesheet as
`fn:doc` reads files:

```console
$ sxml xpath --method=text --context-item=library.xml 'transform({ "stylesheet-location": "by-tag.xsl", "source-node": . })?output'
algorithms: taocp, sicp
art: geb
desert: dune
lisp: sicp
logic: geb
music: geb
politics: dune, lhod
winter: lhod
```
