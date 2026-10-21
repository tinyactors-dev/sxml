# Functions and Operators

<!-- examples: spec -->

XPath comes with a large library of functions, defined in a companion
specification, *XPath and XQuery Functions and Operators 4.0*. This
section shows the ones you'll use every day, and the expressions that
make XPath feel like a small programming language: conditions,
variables and loops.

## Strings

The draft's name, its `docName`, says which version it is:
`draft-example-latch-protocol-01` is version 01. The string functions
take it apart:

```console
$ sxml xpath --context-item=latch.xml 'contains(/rfc/@docName, "latch"), starts-with(/rfc/@docName, "draft-"), substring(/rfc/@docName, 15, 5)'
true()
true()
"latch"
$ sxml xpath --context-item=latch.xml 'substring-after(/rfc/@docName, "draft-"), tokenize(/rfc/@docName, "-")[last()]'
"example-latch-protocol-01"
"01"
```

`substring` counts characters from 1. `tokenize` splits a string at a
separator, here `-`, and `[last()]` keeps the last part: the version.

Paragraphs in XML are full of line breaks and indentation.
`normalize-space` turns every run of whitespace into one space and
removes it at both ends, which is what you want before you compare or
print a paragraph:

```console
$ sxml xpath --context-item=latch.xml 'normalize-space(//t[@anchor = "req-3"])'
"An unlocked lock SHOULD return to the state ""locked"" after ten seconds without a message."
```

The quotes from the paragraph are doubled: that is how a string literal
in XPath writes a quote, and sxml writes strings so that you could paste
them back into an expression.

`string-join` joins a sequence with a separator, and `||` joins two
strings. A *string template*, written between backticks, puts the values
of expressions in curly braces into a string:

```console
$ sxml xpath --context-item=latch.xml 'string-join(//section[@anchor = "lockout"]/t/@anchor, ", ")'
"req-7, req-8, req-11"
$ sxml xpath --context-item=latch.xml '/rfc/middle/section ! `{name}: {count(.//t[@anchor])} requirements`'
"Introduction: 0 requirements"
"Lock States: 3 requirements"
"Messages: 5 requirements"
"Lockout: 3 requirements"
"Security Considerations: 0 requirements"
"IANA Considerations: 0 requirements"
```

Messages has five because `.//t` also finds the requirements of its two
subsections. Other string functions include `upper-case`, `lower-case`,
`string-length`, `substring-before`, `translate` and `char`.

## Regular expressions

`matches`, `replace` and `tokenize` take regular expressions, in the
syntax of XML Schema with the usual extensions (`\d`, `\s`, `\w`, `\b`
for a word boundary, groups, and `$1` in replacements):

```console
$ sxml xpath --context-item=latch.xml 'replace(/rfc/@docName, "-(\d+)$", "-02")'
"draft-example-latch-protocol-02"
$ sxml xpath 'tokenize("lock, access control", ",\s*")'
"lock"
"access control"
$ sxml xpath --context-item=latch.xml '//author ! tokenize(@fullname)[last()]'
"Byron"
"Mathison"
"Bradner"
"Crocker"
"Overell"
"Leiba"
"M'Raihi"
"Machani"
"Pei"
"Rydell"
```

The first gives the name of the next version: `-(\d+)$` matches a dash
and the digits at the end. With one argument, `tokenize` splits at
whitespace.

Regular expressions find a classic mistake in drafts: a key word that is
written in capitals but not marked up. BCP 14 says the words have their
special meaning only "when, and only when, they appear in all capitals",
so a tool that lists requirements by their `bcp14` elements misses a
MUST that is plain text. `\b` matches at the edges of words, so
`\b(MUST|SHOULD|MAY)\b` finds the words but not "MUSTER". Look for them
in the text nodes that are not inside a `bcp14`:

```console
$ sxml xpath 'matches("Deployments SHOULD carry Latch over an encrypted link.", "\b(MUST|SHOULD|MAY)\b"), matches("A lock should lock.", "\b(MUST|SHOULD|MAY)\b")'
true()
false()
$ sxml xpath --context-item=latch.xml '//text()[not(parent::bcp14)][matches(., "\b(MUST|SHOULD|MAY)\b")] ! normalize-space()'
"The key words ""MUST"", ""MUST NOT"", ""REQUIRED"", ""SHALL"", ""SHALL NOT"", ""SHOULD"", ""SHOULD NOT"", ""RECOMMENDED"", ""NOT RECOMMENDED"", ""MAY"", and ""OPTIONAL"" in this document are to be interpreted as described in BCP 14"
```

The one match is the paragraph of Terminology that lists the key words,
where they are meant as words, not as requirements. Leave that section
out, and nothing is left: the draft marks up every key word it uses.

```console
$ sxml xpath --context-item=latch.xml 'count(//text()[not(parent::bcp14)][not(ancestor::section[@anchor = "terminology"])][matches(., "\b(MUST|SHOULD|MAY)\b")])'
0
```

## Numbers

```console
$ sxml xpath --context-item=latch.xml 'count(//t[@anchor]), sum((1, 2, 3)), avg((1, 2))'
11
6
1.5
$ sxml xpath 'abs(-3), floor(2.7), ceiling(2.1), round(2.5), round-half-to-even(2.5)'
3
2
3
3
2
$ sxml xpath 'format-number(5 div 11, "0%"), format-number(5 div 11, "0.0%"), format-number(1234.5, "#,##0.00"), format-integer(3, "w"), format-integer(11, "I")'
"45%"
"45.5%"
"1,234.50"
"three"
"XI"
```

`format-number` writes a number with a picture string; with `%` in the
picture it multiplies by a hundred first. `format-integer` writes
integers as words, roman numerals or letters. "After three rejected
codes" in req-7 is the number 3 in words. [Working with Text](text.md)
shows both in other languages.

## Sequences

Many functions work on whole sequences:

```console
$ sxml xpath --context-item=latch.xml 'distinct-values(//bcp14)'
"MUST"
"SHOULD"
"MUST NOT"
"MAY"
"SHOULD NOT"
$ sxml xpath --context-item=latch.xml 'index-of(//t[@anchor]/@anchor ! string(), "req-9")'
6
$ sxml xpath --context-item=latch.xml 'sort(//dt/string())'
"Code"
"Controller"
"Lock"
"Lockout"
$ sxml xpath --context-item=latch.xml 'head(//t[@anchor]/@anchor ! string()), tail(//section[@anchor = "lockout"]/t/@anchor ! string())'
"req-1"
"req-8"
"req-11"
$ sxml xpath 'reverse(1 to 4)'
4
3
2
1
```

The draft uses five of BCP 14's key words. `index-of` gives the
positions of a value in a sequence: req-9 is the sixth requirement in
the document, although it was the ninth to be written. `head` is the
first item and `tail` all the others. `1 to 4` is the sequence of
integers from 1 to 4.

`empty` and `exists` test whether a sequence has items, and `otherwise`
gives a fallback when the left side is empty:

```console
$ sxml xpath --context-item=latch.xml 'empty(//t[bcp14 = "SHALL"]), exists(//cref)'
true()
true()
$ sxml xpath --context-item=latch.xml '//t[bcp14 = "SHALL"]/@anchor otherwise "no SHALL"'
"no SHALL"
```

## The arrow operator

Function calls nest inside out, which can be hard to read. The arrow
operator `=>` passes the value on its left as the first argument of the
function on its right, so you can write a pipeline from left to right:

```console
$ sxml xpath --context-item=latch.xml '/rfc/@docName => tokenize("-") => reverse() => head()'
"01"
```

`=>` passes the whole sequence. `=!>` passes each item separately, as
`!` does:

```console
$ sxml xpath --context-item=latch.xml '//dt =!> upper-case()'
"LOCK"
"CONTROLLER"
"CODE"
"LOCKOUT"
```

## Conditions

`if` chooses between two values. A section with requirements of its own
is normative; the others only explain:

```console
$ sxml xpath --context-item=latch.xml '//section ! `{@anchor}: {if (t[@anchor]) then "normative" else "informative"}`'
"intro: informative"
"terminology: informative"
"states: normative"
"messages: informative"
"unlock: normative"
"status: normative"
"lockout: normative"
"security: informative"
"iana: informative"
```

`some` and `every` test a condition over a sequence. Is there a
paragraph with a key word but no anchor, and does every requirement
have a key word?

```console
$ sxml xpath --context-item=latch.xml 'some $t in //t[bcp14] satisfies empty($t/@anchor), every $t in //t[@anchor] satisfies $t/bcp14'
true()
true()
```

The first is the paragraph in Security Considerations again: a
requirement that other documents cannot refer to, because it has no
anchor.

## Variables and loops

`let` binds a name to a value, and `for` evaluates an expression once
for each item of a sequence. Variable names start with `$`. Here is the
share of each key word among the requirements:

```console
$ sxml xpath --context-item=latch.xml 'let $requirements := //t[@anchor] for $keyword in distinct-values($requirements/bcp14) return `{$keyword}: {format-number(count($requirements[bcp14 = $keyword]) div count($requirements), "0%")}`'
"MUST: 45%"
"SHOULD: 18%"
"MUST NOT: 9%"
"MAY: 18%"
"SHOULD NOT: 9%"
```

A variable can also come from outside. `--param` binds one from the
command line, as an untyped value:

```console
$ sxml xpath --context-item=latch.xml --param=keyword=SHOULD '//t[bcp14 = $keyword]/@anchor/string()'
"req-3"
"req-8"
```

XQuery builds on `for` and `let` with a much richer form, the FLWOR
expression, in [Writing Programs with XQuery](xquery.md).

## Namespaces of functions

Every function has a namespace. The standard functions are in the `fn`
namespace, which is the default, so `count` is `fn:count`. Functions on
maps and arrays are in `map` and `array`, mathematical functions in
`math`, and the constructor functions of types in `xs`:

```console
$ sxml xpath --context-item=latch.xml 'fn:count(//t[@anchor]), xs:integer("11"), math:pow(2, 10), math:sqrt(2)'
11
11
1.024e3
1.4142135623730951e0
```

A name the processor doesn't know is a static error, raised before
anything runs:

```console
$ sxml xpath 'square-root(2)'
expression:1:1: err:XPST0017: no function with this name and arity is known
```
