# Paths, Steps and Predicates

<!-- examples: spec -->

A path expression is a series of *steps* separated by `/`. Each step
starts from the nodes the previous step found and selects new nodes
relative to each of them.

## Steps from the top

A path that starts with `/` starts at the document node. Each step
names the elements to go down to:

```console
$ sxml xpath --context-item=vane.xml '/rfc/middle/section/name/string()'
"Introduction"
"Flag States"
"Messages"
"Pause"
"Security Considerations"
"IANA Considerations"
```

`/rfc` selects the `rfc` element, `/middle` its `middle` child,
`/section` the sections in it, and `/name` the name of each section. The
final `string()` turns each `name` element into its string. The nested
sections, such as Terminology, are not there: each step goes down one
level only.

`//` means "at any depth". `//section` is every `section` element in the
document, wherever it is. A `*` selects elements of any name:

```console
$ sxml xpath --context-item=vane.xml '//section[@anchor = "rollout"]/* ! name()'
"name"
"t"
"t"
"t"
```

`name()` gives the name of a node. The `!` operator ("for each") applies
it to each child that `*` found.

## Predicates

A predicate in square brackets keeps only the nodes for which it is
true. Inside the brackets, the expression is evaluated once for each
node, with that node as the *context item*, so names in it are relative
to the node:

```console
$ sxml xpath --context-item=vane.xml '//section[@anchor = "pause"]/t/@anchor/string()'
"req-7"
"req-8"
"req-11"
```

A predicate can test anything: whether a child exists, what its value
is, or what a function says about it:

```console
$ sxml xpath --context-item=vane.xml '//t[xref[starts-with(@target, "RFC")]]/@anchor/string()'
"req-9"
$ sxml xpath --context-item=vane.xml '//section[t[@anchor]] ! @anchor/string()'
"states"
"rollout"
"status"
"pause"
```

The first finds the requirements that cite an RFC: the paragraphs with
an `xref` whose target starts with `RFC`. Inside the inner predicate,
`@target` is the target of the `xref` being tested. The second finds the
sections that have requirements of their own.

## Positions

A number in a predicate selects by position, counting from 1:

```console
$ sxml xpath --context-item=vane.xml '/rfc/middle/section[2]/name/string()'
"Flag States"
```

This is where `//` can surprise you. Which section has the first
requirement with a key word?

```console
$ sxml xpath --context-item=vane.xml '//t[bcp14][1]/../name/string()'
"Flag States"
"Rollout"
"Status"
"Pause"
"Security Considerations"
```

There are five answers because `//t[bcp14][1]` means "each `t` with a
`bcp14` that is the first such `t` *of its parent*", and five sections
have one. To get the first in the whole document, put the path in
parentheses and filter the result:

```console
$ sxml xpath --context-item=vane.xml '(//t[bcp14])[1]/@anchor/string()'
"req-1"
```

`last()` is the position of the last item, and `position()` the position
of the current one:

```console
$ sxml xpath --context-item=vane.xml '(//t[bcp14])[last()]/string()'
"Vane messages travel in clear text on the network. Deployments
      SHOULD carry Vane over an encrypted link.
      This needs a requirement anchor of
      its own."
$ sxml xpath --context-item=vane.xml '(//t[@anchor])[position() <= 3]/@anchor/string()'
"req-1"
"req-2"
"req-3"
```

The last paragraph with a key word is in Security Considerations, and
it has no anchor: an editor's note in it says so. We'll come back to it.

## Axes

So far every step went down the tree, to children or attributes. A step
can go in other directions too. The direction is called an *axis*, and
it is written before the name with `::`. Which section is requirement
req-5 in?

```console
$ sxml xpath --context-item=vane.xml '//t[@anchor = "req-5"]/ancestor::section/name/string()'
"Messages"
"Rollout"
$ sxml xpath --context-item=vane.xml '//t[@anchor = "req-5"]/ancestor::section[1]/name/string()'
"Rollout"
```

`ancestor::section` goes *up*: to every section that contains the
paragraph, Messages and its subsection Rollout. The result of a path is
always in document order, but a predicate on a reverse axis such as
`ancestor` counts from the node outwards, so `ancestor::section[1]` is
the nearest one.

The sibling axes go sideways, to the nodes with the same parent, and
`following` and `preceding` go to everything after or before in the
document:

```console
$ sxml xpath --context-item=vane.xml '//t[@anchor = "req-4"]/following-sibling::t/@anchor/string()'
"req-5"
"req-9"
$ sxml xpath --context-item=vane.xml '//cref[@anchor = "issue-2"]/preceding::t[@anchor][1]/@anchor/string()'
"req-11"
```

The second asks for the last requirement before the editor's note
issue-2. These are the axes you can use:

| Axis | Selects |
| --- | --- |
| `child::` | the children (the default: `name` is `child::name`) |
| `attribute::` | the attributes (`@anchor` is `attribute::anchor`) |
| `descendant::`, `descendant-or-self::` | children, their children, and so on |
| `parent::` | the parent (`..` is `parent::node()`) |
| `ancestor::`, `ancestor-or-self::` | the parent, its parent, up to the document node |
| `following-sibling::`, `preceding-sibling::` | the nodes after or before, with the same parent |
| `following::`, `preceding::` | every node after or before in the document, other than descendants and ancestors |
| `self::` | the node itself |

`..` is the most common: from the `xref`s to the term "pause", go up
to the paragraphs that use it.

```console
$ sxml xpath --context-item=vane.xml '//xref[@target = "term-pause"]/../@anchor/string()'
"req-5"
"req-7"
```

The draft does not number its sections: the renderer does, by position.
Axes can compute those numbers. A section's number is one more than the
number of sections before it with the same parent, and a subsection's
number puts its parent's number before it:

```console
$ sxml xpath --context-item=vane.xml '//t[@anchor] ! (@anchor || " in " || string-join(ancestor::section ! (count(preceding-sibling::section) + 1), "."))'
"req-1 in 2"
"req-2 in 2"
"req-3 in 2"
"req-4 in 3.1"
"req-5 in 3.1"
"req-9 in 3.1"
"req-6 in 3.2"
"req-10 in 3.2"
"req-7 in 4"
"req-8 in 4"
"req-11 in 4"
```

For each requirement, `ancestor::section` finds its sections from the
outermost in, `count(preceding-sibling::section) + 1` numbers each, and
`string-join` joins the numbers with dots. Insert a section, and every
number after it changes, while the anchors stay what they are. That is
why the draft refers to sections by anchor.

## Kinds of nodes

Names select elements (or attributes, after `@`). Other tests select
other kinds of node: `text()` selects text nodes, `comment()` comments,
`processing-instruction()` processing instructions, `node()` any node,
and `element()` and `attribute()` elements and attributes of any name.
When sxml writes a text node, it writes the text itself, without quotes,
because a text node is not a string:

```console
$ sxml xpath --context-item=vane.xml '/rfc/front/title/text()'
The Vane Protocol for Feature Flag Rollouts
$ sxml xpath --context-item=vane.xml 'count(/comment()), count(//processing-instruction())'
1
0
```

The draft has one comment and no processing instructions. Its editorial
notes are not comments but `cref` elements, so that xml2rfc can show
them in a draft and leave them out of the RFC. They are elements in the
middle of the text:

```console
$ sxml xpath --context-item=vane.xml '//cref/text()'
Should an owner be able to
      change the duration?
This needs a requirement anchor of
      its own.
```

## Combining paths

`|` (or `union`) combines the nodes of two paths, `intersect` keeps the
nodes in both, and `except` the nodes in the first but not the second.
The result is always in *document order*, the order of the nodes in the
file, without duplicates. How many paragraphs with a key word are there
outside Security Considerations, and which requirements have an `xref`?

```console
$ sxml xpath --context-item=vane.xml 'count(//t[bcp14] except //section[@anchor = "security"]//t)'
11
$ sxml xpath --context-item=vane.xml '(//t[@anchor] intersect //t[xref])/@anchor/string()'
"req-1"
"req-4"
"req-5"
"req-9"
"req-7"
```

The anchors come out as req-9 before req-7 because that is their order
in the file. A step can also be any expression in parentheses. This
makes it easy to pick several children at once:

```console
$ sxml xpath --context-item=vane.xml '//reference[@anchor = "RFC5234"]/front/(title, author/@surname)/string()'
"Augmented BNF for Syntax Specifications: ABNF"
"Crocker"
"Overell"
```

## The value of a node

Every node has a *string value*: all the text inside it, joined. For a
`name` element, that's its name. For a paragraph, it's the text of the
paragraph and of every element in it, line breaks included.
`normalize-space` collapses the whitespace:

```console
$ sxml xpath --context-item=vane.xml '//t[@anchor = "req-5"]/string()'
"A service MUST NOT roll out a flag
        while it is in a pause
        ()."
$ sxml xpath --context-item=vane.xml 'normalize-space(//t[@anchor = "req-5"])'
"A service MUST NOT roll out a flag while it is in a pause ()."
```

The key word and the term are in the value, because they are text inside
the paragraph. The empty parentheses are a reference to a section,
`<xref target="pause"/>`, which has no text of its own: a renderer
writes "Section 4" there, and the string value has nothing. The same
goes for editorial notes, which are part of the value too. To get the
text of req-8 without its note, take only its own text nodes and its
key word:

```console
$ sxml xpath --context-item=vane.xml 'normalize-space(//t[@anchor = "req-8"])'
"A pause SHOULD last five minutes. Should an owner be able to change the duration?"
$ sxml xpath --context-item=vane.xml 'normalize-space(string-join(//t[@anchor = "req-8"]/(text() | bcp14)))'
"A pause SHOULD last five minutes."
```

When you compare a node with a value, or do arithmetic with it, XPath
uses this value. That is why `//t[bcp14 = "MUST"]` works: the string
value of the `bcp14` element is compared with the string `"MUST"`. How
comparisons work is the subject of the next section.
