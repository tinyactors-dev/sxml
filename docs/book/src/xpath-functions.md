# Functions and Operators

<!-- examples: xpath -->

XPath comes with a large library of functions, defined in a companion
specification, *XPath and XQuery Functions and Operators 4.0*. This
section shows the ones you'll use every day, and the expressions that
make XPath feel like a small programming language: conditions,
variables and loops.

## Strings

```console
$ sxml xpath 'contains("Dune", "un"), starts-with("Dune", "D"), substring("Hofstadter", 1, 4)'
true()
true()
"Hofs"
$ sxml xpath --context-item=library.xml 'string-join(//book[@year < 1970]/@id, ", ")'
"taocp, dune, lhod"
```

`string-join` joins a sequence with a separator, and `||` joins two
strings. A *string template*, written between backticks, puts the values
of expressions in curly braces into a string:

```console
$ sxml xpath --context-item=library.xml '//shelf ! `{@name}: {count(book)} books`'
"Computing: 3 books"
"Fiction: 2 books"
```

Other string functions include `upper-case`, `lower-case`,
`normalize-space`, `string-length`, `substring-before`,
`substring-after`, `translate` and `char`.

## Regular expressions

`matches`, `replace` and `tokenize` take regular expressions, in the
syntax of XML Schema with the usual extensions (`\d`, `\s`, `\w`, groups,
and `$1` in replacements):

```console
$ sxml xpath 'replace("2026-10-20", "(\d+)-(\d+)-(\d+)", "$3.$2.$1")'
"20.10.2026"
$ sxml xpath 'tokenize("logic, music, art", ",\s*")'
"logic"
"music"
"art"
$ sxml xpath --context-item=library.xml '//author ! tokenize(.)[last()]'
"Hofstadter"
"Knuth"
"Abelson"
"Sussman"
"Herbert"
"Guin"
```

With one argument, `tokenize` splits at whitespace.

## Numbers

```console
$ sxml xpath --context-item=library.xml 'count(//book), sum(//book/@pages), avg(//book/@pages)'
5
2.822e3
5.644e2
$ sxml xpath 'abs(-3), floor(2.7), ceiling(2.1), round(2.5), round-half-to-even(2.5)'
3
2
3
3
2
$ sxml xpath 'format-number(1234567.891, "#,##0.00"), format-integer(1968, "I")'
"1,234,567.89"
"MCMLXVIII"
```

`format-number` writes a number with a picture string, and
`format-integer` writes integers as words, roman numerals or letters.
Chapter 7 shows them in other languages.

## Sequences

Many functions work on whole sequences:

```console
$ sxml xpath --context-item=library.xml 'distinct-values(//tag)'
"logic"
"music"
"art"
"algorithms"
"lisp"
"desert"
"politics"
"winter"
$ sxml xpath 'reverse(1 to 4)'
4
3
2
1
$ sxml xpath 'index-of(("a", "b", "c", "b"), "b"), head((5, 6, 7)), tail((5, 6, 7))'
2
4
5
6
7
$ sxml xpath --context-item=library.xml 'sort(//book/title/string())'
"Dune"
"Godel, Escher, Bach"
"Structure and Interpretation of Computer Programs"
"The Art of Computer Programming"
"The Left Hand of Darkness"
```

`1 to 4` is the sequence of integers from 1 to 4. `empty` and `exists`
test whether a sequence has items, and `otherwise` gives a fallback when
the left side is empty:

```console
$ sxml xpath --context-item=library.xml 'empty(//book[@year < 1900])'
true()
$ sxml xpath --context-item=library.xml '//book[@year < 1900]/title otherwise "none"'
"none"
```

## The arrow operator

Function calls nest inside out, which can be hard to read. The arrow
operator `=>` passes the value on its left as the first argument of the
function on its right, so you can write a pipeline from left to right:

```console
$ sxml xpath '"hello world" => upper-case() => tokenize()'
"HELLO"
"WORLD"
```

`=>` passes the whole sequence. `=!>` passes each item separately, as
`!` does:

```console
$ sxml xpath '("apple", "Banana", "cherry") =!> upper-case()'
"APPLE"
"BANANA"
"CHERRY"
```

## Conditions

`if` chooses between two values:

```console
$ sxml xpath --context-item=library.xml '//book ! (if (@year < 1970) then "old" else "new")'
"new"
"old"
"new"
"old"
"old"
```

`some` and `every` test a condition over a sequence:

```console
$ sxml xpath --context-item=library.xml 'some $book in //book satisfies $book/@year < 1966'
true()
$ sxml xpath --context-item=library.xml 'every $book in //book satisfies $book/author'
true()
```

## Variables and loops

`let` binds a name to a value, and `for` evaluates an expression once
for each item of a sequence. Variable names start with `$`:

```console
$ sxml xpath --context-item=library.xml 'let $prices := //price ! xs:decimal(.) return max($prices) - min($prices)'
170.01
$ sxml xpath --context-item=library.xml 'for $book in //book return $book/@id || ": " || $book/@pages'
"geb: 777"
"taocp: 672"
"sicp: 657"
"dune: 412"
"lhod: 304"
```

A variable can also come from outside. `--param` binds one from the
command line, as an untyped value:

```console
$ sxml xpath --context-item=library.xml --param=before=1970 '//book[@year < $before]/@id/string()'
"taocp"
"dune"
"lhod"
```

XQuery builds on `for` and `let` with a much richer form, the FLWOR
expression, in Chapter 3.

## Namespaces of functions

Every function has a namespace. The standard functions are in the `fn`
namespace, which is the default, so `count` is `fn:count`. Functions on
maps and arrays are in `map` and `array`, mathematical functions in
`math`, and the constructor functions of types in `xs`:

```console
$ sxml xpath 'math:sqrt(2), math:pi()'
1.4142135623730951e0
3.141592653589793e0
```

A name the processor doesn't know is a static error, raised before
anything runs:

```console
$ sxml xpath 'square-root(2)'
expression:1:1: err:XPST0017: no function with this name and arity is known
```
