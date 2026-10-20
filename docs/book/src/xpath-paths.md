# Paths, Steps and Predicates

<!-- examples: xpath -->

A path expression is a series of *steps* separated by `/`. Each step
starts from the nodes the previous step found and selects new nodes
relative to each of them.

## Steps from the top

A path that starts with `/` starts at the document node. Each step
names the elements to go down to:

```console
$ sxml xpath --context-item=library.xml '/library/shelf/@name/string()'
"Computing"
"Fiction"
```

`/library` selects the `library` element, `/shelf` its `shelf` children,
and `@name` the `name` attribute of each shelf. The final
`string()` turns each attribute into its string value.

`//` means "at any depth". `//title` is every `title` element in the
document, wherever it is. A `*` selects elements of any name:

```console
$ sxml xpath --context-item=library.xml '//book[@id = "dune"]/* ! name()'
"title"
"author"
"price"
"tags"
```

`name()` gives the name of a node. The `!` operator ("for each") applies
it to each child that `*` found.

## Predicates

A predicate in square brackets keeps only the nodes for which it is
true. Inside the brackets, the expression is evaluated once for each
node, with that node as the *context item*, so names in it are relative
to the node:

```console
$ sxml xpath --context-item=library.xml '//shelf[@name = "Fiction"]/book/title/string()'
"Dune"
"The Left Hand of Darkness"
```

A predicate can test anything: whether a child exists, how many there
are, or what a function says about them:

```console
$ sxml xpath --context-item=library.xml '//book[author[2]]/title/string()'
"Structure and Interpretation of Computer Programs"
$ sxml xpath --context-item=library.xml '//title[starts-with(., "The")]/string()'
"The Art of Computer Programming"
"The Left Hand of Darkness"
```

`book[author[2]]` is a book that has a second author. Inside the
predicate of `title[...]`, the `.` is the title being tested.

## Positions

A number in a predicate selects by position, counting from 1. This is
where `//` can surprise you:

```console
$ sxml xpath --context-item=library.xml '//book[1]/@id/string()'
"geb"
"dune"
```

There are two answers because `//book[1]` means "each `book` that is the
first `book` child *of its parent*", and there are two shelves. To get
the first book in the whole document, put the path in parentheses and
filter the result:

```console
$ sxml xpath --context-item=library.xml '(//book)[1]/@id/string()'
"geb"
```

`last()` is the position of the last item, and `position()` the position
of the current one:

```console
$ sxml xpath --context-item=library.xml '(//book)[last()]/@id/string()'
"lhod"
$ sxml xpath --context-item=library.xml '(//book)[position() < 3]/@id/string()'
"geb"
"taocp"
```

## Axes

So far every step went down the tree, to children or attributes. A step
can go in other directions too. The direction is called an *axis*, and
it is written before the name with `::`:

```console
$ sxml xpath --context-item=library.xml '//tag[. = "politics"]/ancestor::book/@id/string()'
"dune"
"lhod"
$ sxml xpath --context-item=library.xml '//book[@id = "sicp"]/preceding-sibling::book/@id/string()'
"geb"
"taocp"
```

The first finds each `tag` element whose text is `politics`, then goes
*up* to the book that contains it. The second goes *back* to the books
before SICP on the same shelf. These are the axes you can use:

| Axis | Selects |
| --- | --- |
| `child::` | the children (the default: `title` is `child::title`) |
| `attribute::` | the attributes (`@id` is `attribute::id`) |
| `descendant::`, `descendant-or-self::` | children, their children, and so on |
| `parent::` | the parent (`..` is `parent::node()`) |
| `ancestor::`, `ancestor-or-self::` | the parent, its parent, up to the document node |
| `following-sibling::`, `preceding-sibling::` | the nodes after or before, with the same parent |
| `following::`, `preceding::` | every node after or before in the document, other than descendants and ancestors |
| `self::` | the node itself |

`..` is the most common: from an author, go up and get the title.

```console
$ sxml xpath --context-item=library.xml '//author[. = "Donald Knuth"]/../title/string()'
"The Art of Computer Programming"
```

## Kinds of nodes

Names select elements (or attributes, after `@`). Other tests select
other kinds of node: `text()` selects text nodes, `node()` any node, and
`element()`, `attribute()`, `comment()` and so on one kind. When sxml
writes a text node, it writes the text itself, without quotes, because a
text node is not a string:

```console
$ sxml xpath --context-item=library.xml '//book[@year < 1966]/title/text()'
Dune
```

## Combining paths

`|` (or `union`) combines the nodes of two paths, `intersect` keeps the
nodes in both, and `except` the nodes in the first but not the second.
The result is always in *document order*, the order of the nodes in the
file, without duplicates:

```console
$ sxml xpath --context-item=library.xml '(//shelf[@name = "Fiction"]/book except //book[@year > 1966])/@id/string()'
"dune"
```

A step can also be any expression in parentheses. This makes it easy to
pick several children at once:

```console
$ sxml xpath --context-item=library.xml '//book[@id = "dune"]/(title, author)/string()'
"Dune"
"Frank Herbert"
```

## The value of a node

Every node has a *string value*: all the text inside it, joined. For a
`title` element, that's its title. For a `book`, it's all the text of
all its children, whitespace included. `normalize-space` collapses the
whitespace:

```console
$ sxml xpath --context-item=library.xml 'normalize-space(//book[@id = "dune"])'
"Dune Frank Herbert 9.99 desertpolitics"
```

When you compare a node with a value, or do arithmetic with it, XPath
uses this value. That is why `//book[@year < 1966]` works: the `year`
attribute's value is compared with the number 1966. How that comparison
works is the subject of the next section.
