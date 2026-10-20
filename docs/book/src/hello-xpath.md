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

XPath is made for finding things in documents. Here is a small
catalog of books, the file `books.xml` in the examples:

`books.xml`:

```xml
{{#include ../examples/getting-started/books.xml}}
```

Give the document to `sxml xpath` with `--context-item`. The document
becomes the *context item*: the thing the expression starts from. Now ask
for every `title` element:

```console
$ sxml xpath --context-item=books.xml '//title'
<title>Godel, Escher, Bach</title>
<title>The Art of Computer Programming</title>
<title>Structure and Interpretation of Computer Programs</title>
```

`//title` is a *path expression*. It reads as "from the top of the
document, go down any number of levels, and find elements named
`title`". The result is a *sequence* of three elements, and sxml writes
one item of the sequence on each line.

Most of the time you want what is inside the elements, not the elements
themselves. A path can end with a function call that is applied to each
element it finds:

```console
$ sxml xpath --context-item=books.xml '//title/string()'
"Godel, Escher, Bach"
"The Art of Computer Programming"
"Structure and Interpretation of Computer Programs"
```

## Asking questions

Square brackets after a step filter it. They hold a *predicate*: a
condition each item must meet. Which books cost more than 50?

```console
$ sxml xpath --context-item=books.xml '//book[price > 50]/title/string()'
"The Art of Computer Programming"
"Structure and Interpretation of Computer Programs"
```

How many books are there, and what is the earliest year?

```console
$ sxml xpath --context-item=books.xml 'count(//book), min(//book/@year)'
3
1.968e3
```

The comma makes a sequence of two values. The `@` in `@year` selects an
*attribute* instead of an element. The second result, `1.968e3`, is
1968 written as a floating-point number (a `double`). The document has
no types in it, so `min` treated the text `"1968"` as a number, and the
numbers XPath makes from untyped text are doubles. To get a whole number,
say so:

```console
$ sxml xpath --context-item=books.xml 'min(//book/@year ! xs:integer(.))'
1968
```

The `!` operator applies the expression on its right to each item on its
left, and `.` is that item. `xs:integer(.)` turns the attribute into an
integer. You'll learn much more about types in the next chapter.

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
$ sxml xpath '//title'
expression:1:1: err:XPDY0002: The focus or a variable of the dynamic context is absent (XPath 4.0, section 2.2.2)
```

`//title` needs a document to start from, and we forgot to give one.

## Summary

You have run your first XPath expressions:

- `sxml xpath EXPRESSION` evaluates an expression and writes the result;
- `--context-item=FILE` gives the expression a document to start from;
- paths such as `//book/title` find nodes, predicates such as
  `[price > 50]` filter them, and functions such as `string()` and
  `count()` compute with them.

Next, let's look at the other things the `sxml` command can do.
