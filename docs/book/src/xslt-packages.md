# Many Outputs, Functions and Packages

<!-- examples: spec -->

## Writing many files

A transformation has one *principal result*, the one that goes to
standard output, and can make any number of *result documents* with
`xsl:result-document`. This stylesheet makes a page for each top-level
section of the draft and an index that links to them:

`pages.xsl`:

```xml
{{#include ../examples/spec/pages.xsl}}
```

sxml writes files only where you allow it. Without
`--result-directory`, it writes the principal result and tells you
about each result document it did not write:

```console
$ sxml transform pages.xsl vane.xml
sxml transform: result document 'sections/intro.html' not written; give --result-directory
sxml transform: result document 'sections/states.html' not written; give --result-directory
sxml transform: result document 'sections/messages.html' not written; give --result-directory
sxml transform: result document 'sections/pause.html' not written; give --result-directory
sxml transform: result document 'sections/security.html' not written; give --result-directory
sxml transform: result document 'sections/iana.html' not written; give --result-directory
<ul>
  <li><a href="sections/intro.html">Introduction</a></li>
  <li><a href="sections/states.html">Flag States</a></li>
  <li><a href="sections/messages.html">Messages</a></li>
  <li><a href="sections/pause.html">Pause</a></li>
  <li><a href="sections/security.html">Security Considerations</a></li>
  <li><a href="sections/iana.html">IANA Considerations</a></li>
</ul>
```

With `--result-directory`, the result documents are written below that
directory, and `--output` puts the principal result into a file too:

```console
$ mkdir site
$ sxml transform --result-directory=site --output=site/index.html pages.xsl vane.xml
$ find site -type f | sort
site/index.html
site/sections/iana.html
site/sections/intro.html
site/sections/messages.html
site/sections/pause.html
site/sections/security.html
site/sections/states.html
$ cat site/sections/messages.html
<h1>Messages</h1>
<p>5 requirements, 2 subsections</p>
```

All the files are written only when the whole transformation succeeds,
and a result document that would land outside the directory is an
error. Run the same command again, and sxml refuses to replace the
files without `--force`:

```console
$ sxml transform --result-directory=site --output=site/index.html pages.xsl vane.xml; echo $?
sxml transform: 'site/index.html' exists; give --force to replace it
3
```

## Functions and packages

The HTML page and the plain text of [Templates and
Modes](xslt-templates.md) need the same things: the number of a section
or a requirement, and the text of an `xref`. Written once, they belong
in functions that every stylesheet of the draft can call.

`xsl:function` declares a function that XPath expressions in the
stylesheet can call, with typed parameters and result. Its name must
have a namespace prefix of your own; here `spec`, for
`urn:example:spec`.

A *package* is a stylesheet module with a name and a version, whose
components (functions, templates, variables, keys, modes) are private
unless it says otherwise. Other stylesheets *use* a package and see only
what it makes public. Packages let a team share a library of functions
without the risk of names clashing or of rules leaking into each other.
This one holds the draft's functions:

`spec.xsl`:

```xml
{{#include ../examples/spec/spec.xsl}}
```

The three functions are public. The key `anchor` and the variable
`spec:strengths` are private: only the package's own functions can see
them. `spec:number` puts its `xsl:choose` inside `xsl:value-of`, which
makes one text node of what `xsl:text` and `xsl:number` write, and the
function's `as="xs:string"` turns that node into its string. Without
the `xsl:value-of`, a requirement's number would be two text nodes, "R"
and "6", and two strings are not the one string the function promises
(XTTE0780).

A stylesheet uses the package with `xsl:use-package`. It can call the
public functions, and nothing else:

`private.xsl`:

```xml
{{#include ../examples/spec/private.xsl}}
```

```console
$ sxml transform private.xsl vane.xml
private.xsl:7:29: err:XPST0008: the variable is not declared
```

The `sxml` command finds a package by reading its name as a file name,
relative to the stylesheet. A program that embeds sxml can find
packages any way it likes: by name and version, in a database, or built
in ([Embedding sxml in C](c.md)).

## Importing stylesheets

Some template rules are shared too: an `xref` becomes the text that
`spec:xref-text` makes, and a `cref` becomes nothing, in every output.
They go in a module of their own, with a function that gives the text
of a paragraph as a reader sees it:

`common.xsl`:

```xml
{{#include ../examples/spec/common.xsl}}
```

`xsl:import` brings in the rules of another stylesheet, with a lower
*import precedence*: a rule in the importing stylesheet beats any rule
it imports, whatever their priorities. That is how you extend a
stylesheet without changing it. The HTML stylesheet imports
`common.xsl`, uses the package, and replaces the `xref` rule with one
that puts a link around the imported rule's text, which
`xsl:apply-imports` calls:

`html.xsl`:

```xml
{{#include ../examples/spec/html.xsl}}
```

```console
$ sxml transform html.xsl vane.xml | grep -B1 -A1 'Section 4'
        while it is in a <a href="#term-pause">pause</a>
        (<a href="#pause">Section 4</a>).</p>
        <p id="req-9" class="requirement"><span class="number">R6</span> A service <span class="bcp14 optional">MAY</span> ask an owner for a
```

An `xsl:use-package` must be in the principal stylesheet module, not
in a module it imports (XTSE3008), so each stylesheet that imports
`common.xsl` uses the package itself. `xsl:include` is the other way to
combine modules: the included rules count as if they were written in
the including stylesheet, with the same precedence.

The plain-text stylesheet imports the same module and keeps its
`xref` rule. Its rules are those of the `text` mode of `draft.xsl`,
now in the default mode of a stylesheet of their own, with an RFC's
first line and the references at the end:

`text.xsl`:

```xml
{{#include ../examples/spec/text.xsl}}
```

```console
$ sxml transform text.xsl vane.xml | head -3
Internet-Draft                                              October 2026

The Vane Protocol for Feature Flag Rollouts
$ sxml transform text.xsl vane.xml | grep -A1 '(Section'
   A service MUST NOT roll out a flag while it is in a pause (Section
   4).
$ sxml transform text.xsl vane.xml | grep -A3 '^   \[RFC6238\]$'
   [RFC6238]
      D. M'Raihi, S. Machani, M. Pei, J. Rydell, "TOTP: Time-Based
      One-Time Password Algorithm", May 2011,
      <https://www.rfc-editor.org/info/rfc6238>.
```

`spec:spaces` is a function of the stylesheet itself, not of the
package: a function that only one stylesheet needs stays there. It has
no `visibility`, and in a stylesheet that is not a package that makes no
difference.

## Transformations from queries

`fn:transform` runs a stylesheet from XPath or XQuery and returns its
results in a map; `?output` is the principal result. With the `sxml`
command, it reads the stylesheet as `fn:doc` reads files. A query can
check what a stylesheet makes, such as the line lengths of the
plain-text draft:

```console
$ sxml xpath --context-item=vane.xml 'let $lines := transform({ "stylesheet-location": "text.xsl", "source-node": . })?output => tokenize("\n") return (count($lines), max($lines ! string-length()))'
117
72
```

The plain text has 117 lines, counting the empty ones, and none is longer than 72 characters.

Large inputs are a subject of their own: a stylesheet whose mode is
*streamable* reads its input once, without building a tree of it.
[Streaming Large Trace Files](traces-streaming.md) shows how, on files
of traces.
