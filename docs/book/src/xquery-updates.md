# Updating Documents

<!-- examples: xquery -->

XQuery can also change documents, with the *XQuery Update Facility*. Its
expressions insert, delete, replace and rename nodes. We'll use a small
shelf for this section:

`shelf.xml`:

```xml
{{#include ../examples/xquery/shelf.xml}}
```

## Updating expressions

An *updating expression* doesn't return a value. It makes a list of
changes, called a *pending update list*, and the changes are all applied
together when the query ends. There are five kinds:

- `insert node ... into` (or `as first into`, `as last into`, `before`,
  `after`) adds nodes;
- `delete node` removes them;
- `replace node ... with` puts other nodes in their place;
- `replace value of node ... with` changes the text of a node;
- `rename node ... as` gives a node a new name.

When `sxml xquery` runs an updating query, it writes the context
document as the updates left it. The file itself is not changed, so you
can look at the result before you keep it:

`add-book.xq`:

```xquery
{{#include ../examples/xquery/add-book.xq}}
```

```console
$ sxml xquery --context-item=shelf.xml add-book.xq
<shelf name="Fiction"><book id="fdn" year="1951"><title>Foundation</title><price>8.99</price></book>
  <book id="dune" year="1965"><title>Dune</title><price>9.99</price></book>
  <book id="lhod" year="1969"><title>The Left Hand of Darkness</title><price>11.50</price></book>
</shelf>
```

To keep it, write it to a new file with `--output`, or let the shell
redirect it.

Updating expressions combine with everything else. A FLWOR expression
whose `return` updates makes one change for each binding:

`sale.xq`:

```xquery
{{#include ../examples/xquery/sale.xq}}
```

```console
$ sxml xquery --context-item=shelf.xml sale.xq
<shelf name="Fiction">
  <book id="dune" year="1965"><title>Dune</title><price>8.99</price></book>
  <book id="lhod" year="1969"><title>The Left Hand of Darkness</title><price>10.35</price></book>
</shelf>
```

Several changes can be made at once, separated by commas:

```console
$ sxml xquery --context-item=shelf.xml --expression='delete node //book[@year < 1966], rename node /shelf as "bookshelf"'
<bookshelf name="Fiction">

  <book id="lhod" year="1969"><title>The Left Hand of Darkness</title><price>11.50</price></book>
</bookshelf>
```

The empty line is where the book was: the line breaks and spaces around
it are text nodes, and they stay.

Because all the changes are applied at the end, an updating query never
sees its own changes, and the order of the changes in the query doesn't
matter. Two changes that conflict, such as renaming the same node twice,
are an error.

## Changing copies

Often you don't want to change a document, but to make a changed copy of
part of it. The `copy ... modify ... return` expression copies nodes,
applies updates to the copies only, and returns them. It is not an
updating expression, so you can use it anywhere, in any query:

```console
$ sxml xquery --context-item=shelf.xml --expression='
    copy $book := //book[1]
    modify (
      replace value of node $book/price with 0,
      insert node attribute free { "yes" } into $book
    )
    return $book'
<book id="dune" year="1965" free="yes"><title>Dune</title><price>0</price></book>
```

XQuery 4.0 has a shorter form for the common case, `transform with`:

```console
$ sxml xquery --context-item=shelf.xml --expression='//book ! (. transform with { delete node price })'
<book id="dune" year="1965"><title>Dune</title></book><book id="lhod" year="1969"><title>The Left Hand of Darkness</title></book>
```

## Updating functions

A function whose body is an updating expression must say so, with
`declare updating function`. It returns no value; calling it adds its
changes to the pending update list:

```console
$ sxml xquery --context-item=shelf.xml --expression='
    declare updating function local:lend($book as element(book), $member as xs:string) {
      insert node attribute lent-to { $member } into $book
    };
    local:lend(//book[@id = "dune"], "ada")'
<shelf name="Fiction">
  <book id="dune" year="1965" lent-to="ada"><title>Dune</title><price>9.99</price></book>
  <book id="lhod" year="1969"><title>The Left Hand of Darkness</title><price>11.50</price></book>
</shelf>
```

## Mistakes the processor catches

Updating and ordinary expressions don't mix: a query either makes
changes or computes a value, and the processor checks which before it
runs anything. An updating expression in a function that isn't declared
updating is a static error:

```console
$ sxml xquery --context-item=shelf.xml --expression='
    declare function local:no-prices() { delete node //price };
    local:no-prices()'
expression:2:40: err:XUST0001: an updating expression is used where only a simple expression is allowed (XQuery Update Facility 3.0, 2.2.2)
```

The target of `insert ... into`, `replace` and `rename` must be exactly
one node. `//book` is two books, so this fails when it runs:

```console
$ sxml xquery --context-item=shelf.xml --expression='insert node <note/> into //book'
expression:1:1: err:XUTY0005: the target of insert into is not one element or document node (5.4.1)
```

Write `for $book in //book return insert node <note/> into $book` to
change each one. And two changes to the same node that can't both
happen are found when the changes are applied:

```console
$ sxml xquery --context-item=shelf.xml --expression='rename node /shelf as "a", rename node /shelf as "b"'
expression: err:XUDY0015: a node is renamed twice (3.2.2)
```

## Storing documents

`fn:put` stores a node as a document at a URI. sxml never writes a file
you didn't ask for, so with the `sxml` command it needs `--allow-put`.
This query writes each book into a file of its own:

```console
$ sxml xquery --allow-put --context-item=shelf.xml --expression='
    for $book in //book return put($book, $book/@id || ".xml")' > /dev/null
$ cat dune.xml
<book id="dune" year="1965"><title>Dune</title><price>9.99</price></book>
```

The files are written only when the whole query succeeds, and a file that
exists is replaced only with `--force`. Without `--allow-put`, `fn:put`
raises an error:

```console
$ sxml xquery --context-item=shelf.xml --expression='put(., "copy.xml")'
expression: err:FOUP0002: fn:put has no location to store at: the host's update sink takes no documents (XQuery Update Facility 3.0, 6.1)
```

A program that embeds sxml receives the stored documents and the updated
trees through callbacks, and decides itself what to do with them
(Chapter 9).
