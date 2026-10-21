# Hello, XPath

<!-- examples: getting-started -->

Now that you have sxml, it's traditional to say hello. XPath is an
expression language: you give it an expression, and it gives you back a
value. A string in quotes is an expression whose value is that string:

```console
$ sxml xpath '"Hello, XPath!"'
"Hello, XPath!"
```

`sxml xpath` evaluated the expression and wrote the result. The result is
written with quotes because sxml shows you *what kind* of value you got:
`"Hello, XPath!"` is a string, while `42` would be a number. Single
quotes around the expression are for your shell; the double quotes belong
to XPath.

Expressions can compute:

```console
$ sxml xpath 'upper-case("hello") || ", " || 6 * 7'
"HELLO, 42"
```

`upper-case` is one of the more than two hundred functions that come with
XPath, and `||` joins strings together.

## A first document

XPath is made for finding things in documents. The documents of this
book belong to Vane, a small, made-up service for feature flags, and the
first is a short cut of the specification of its protocol. Like the specifications of
the IETF, it is written in XML: sections with a `name`, paragraphs `t`,
and *requirements*, the paragraphs with an `anchor` and a key word such
as MUST or SHOULD in a `bcp14` element. Here it is, the file `draft.xml`
in the examples:

`draft.xml`:

```xml
{{#include ../examples/getting-started/draft.xml}}
```

Give the document to `sxml xpath` with `--context-item`. The document
becomes the *context item*: the thing the expression starts from. Now ask
for every `name` element:

```console
$ sxml xpath --context-item=draft.xml '//name'
<name>Flag States</name>
<name>Pause</name>
```

`//name` is a *path expression*. It reads as "from the top of the
document, go down any number of levels, and find elements named
`name`". The result is a *sequence* of two elements, and sxml writes
one item of the sequence on each line.

Most of the time you want what is inside the elements, not the elements
themselves. A path can end with a function call that is applied to each
element it finds:

```console
$ sxml xpath --context-item=draft.xml '//name/string()'
"Flag States"
"Pause"
```

## Asking questions

Square brackets after a step filter it. They hold a *predicate*: a
condition each item must meet. Which paragraphs say MUST?

```console
$ sxml xpath --context-item=draft.xml '//t[bcp14 = "MUST"]/string()'
"A flag MUST be stable when it is created."
"After three failed checks, a service MUST pause the flag."
```

The predicate `[bcp14 = "MUST"]` keeps each `t` that has a `bcp14`
child whose text is `MUST`. The string of a paragraph is all the text
inside it, so the key word is part of it, although it sits in an element
of its own.

How many paragraphs are there, and how many of them are requirements?

```console
$ sxml xpath --context-item=draft.xml 'count(//t), count(//t[@anchor])'
4
3
```

The comma makes a sequence of two values. The `@` in `@anchor` selects
an *attribute* instead of an element, and `t[@anchor]` is a paragraph
that has one. Asked for the attributes themselves, sxml writes them as
they look in XML:

```console
$ sxml xpath --context-item=draft.xml '//t/@anchor'
anchor="req-2"
anchor="req-3"
anchor="req-7"
```

The `!` operator applies the expression on its right to each item on its
left, and inside it, names and `.` refer to that item. Here it makes one
line of the index of requirements for each anchored paragraph:

```console
$ sxml xpath --context-item=draft.xml '//t[@anchor] ! (@anchor || ": " || bcp14)'
"req-2: MUST"
"req-3: SHOULD"
"req-7: MUST"
```

The anchors are names, not numbers: `req-1` is not in this cut of the
draft, and the full draft adds requirements in the middle. You'll learn
much more about paths in the next chapter.

## When something goes wrong

If an expression has a mistake in it, sxml tells you where it is and what
the problem is:

```console
$ sxml xpath '1 +'
expression:1:3: err:XPST0003: The text is not valid XPath 4.0 syntax (XPath 4.0, Appendix A)
```

The message has the line and column (`1:3`), an *error code*
(`err:XPST0003`), and a description that names the part of the
specification that decides it. Every error in XPath, XQuery and XSLT
has a code like this one; the same code means the same problem in every
processor. Here is another common one:

```console
$ sxml xpath '//t'
expression:1:1: err:XPDY0002: The focus or a variable of the dynamic context is absent (XPath 4.0, section 2.2.2)
```

`//t` needs a document to start from, and we forgot to give one.

## Summary

You have run your first XPath expressions:

- `sxml xpath EXPRESSION` evaluates an expression and writes the result;
- `--context-item=FILE` gives the expression a document to start from;
- paths such as `//section/name` find nodes, predicates such as
  `[bcp14 = "MUST"]` filter them, `@anchor` selects an attribute, and
  functions such as `string()` and `count()` compute with them.

Next, let's look at the other things the `sxml` command can do.
