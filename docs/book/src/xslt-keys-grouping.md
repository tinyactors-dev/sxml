# Keys, Numbers and Grouping

<!-- examples: spec -->

## Keys

An `xref` without text, such as `<xref target="pause"/>`, asks the
renderer for the text: "Section 4" when it points to a section,
"[RFC5234]" when it points to a reference. To write it, a stylesheet
must find the element whose anchor is the target. A *key* is an index
that the processor builds for that. You declare what to index and by
which value, and the `key` function looks nodes up:

`xrefs.xsl`:

```xml
{{#include ../examples/spec/xrefs.xsl}}
```

```console
$ sxml transform xrefs.xsl vane.xml
term-sdk -> the term "SDK"
RFC2119 -> [RFC2119]
RFC8174 -> [RFC8174]
term-flag -> the term "Flag"
term-flag -> the term "Flag"
RFC5234 -> [RFC5234]
term-percentage -> the term "Percentage"
term-pause -> the term "Pause"
pause -> Section 4
RFC6238 -> [RFC6238]
term-pause -> the term "Pause"
```

`key('anchor', @target)` finds the elements whose `@anchor` equals the
xref's target, in the document of the context node; a third argument
names another document. A predicate such as `//*[@anchor =
current()/@target]` would give the same result, but the key is built
once and each lookup is fast, however large the draft is. The rules of
the mode `xref-text` then decide the text by the kind of element the
key found: one rule for a section, one for a reference, one for a term.

## Numbers

The draft has no section numbers in it, and it should not: when the
authors insert a section, every number after it changes, and every
"Section 4" in the text with it. The renderer numbers by position, and
`xsl:number` is the instruction that does it:

`numbers.xsl`:

```xml
{{#include ../examples/spec/numbers.xsl}}
```

```console
$ sxml transform numbers.xsl vane.xml
1  Introduction
1.1  Terminology
2  Flag States
    R1 req-1: MUST
    R2 req-2: MUST
    R3 req-3: SHOULD
3  Messages
3.1  Rollout
    R4 req-4: MUST
    R5 req-5: MUST NOT
    R6 req-9: MAY
3.2  Status
    R7 req-6: MUST
    R8 req-10: SHOULD NOT
4  Pause
    R9 req-7: MUST
    R10 req-8: SHOULD
    R11 req-11: MAY
5  Security Considerations
6  IANA Considerations
```

`level="multiple"` counts the node and each of its ancestors that
match `count` among their siblings, and `format="1.1"` writes the
numbers with dots between them. `level="any"` counts every matching
node before it in the document, at any depth: that numbers the
requirements from R1 to R11 in reading order, while their anchors keep
the order in which they were written (`req-9` was added in the second
draft and is the sixth requirement). Other formats give `a`, `A`, `i`,
`I` or `01`, as in `format="a)"` or `format="i."`. A format token
that `xsl:number` does not know falls back to decimal digits, so `R1`
as a format would write `1`; the `R` is written as text before the
number.

## Grouping

`xsl:for-each-group` puts the selected items into groups and processes
each group once. Inside it, `current-grouping-key()` is the key of the
group, and `current-group()` its items.

`group-by` groups by a computed value. An index of the draft's terms is
grouped by first letter, and lists the sections that use each term,
found with a second key:

`term-index.xsl`:

```xml
{{#include ../examples/spec/term-index.xsl}}
```

```console
$ sxml transform term-index.xsl vane.xml
F
  Flag: 1.1, 2
P
  Pause: 3.1, 4
  Percentage: 3.1
S
  SDK: 1
```

`key('uses', @anchor)` gives every `xref` to the term, and
`/ancestor::section[1]` the section around each one. A path removes
duplicates and returns nodes in document order, so a section that uses
a term twice would still be listed once, and the sections come in the
order of the draft.

The references are grouped by the `references` element they are in,
normative or informative:

`references.xsl`:

```xml
{{#include ../examples/spec/references.xsl}}
```

```console
$ sxml transform references.xsl vane.xml
Normative References
  [RFC8174] 2017 Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words
  [RFC5234] 2008 Augmented BNF for Syntax Specifications: ABNF
  [RFC2119] 1997 Key words for use in RFCs to Indicate Requirement Levels
Informative References
  [RFC6238] 2011 TOTP: Time-Based One-Time Password Algorithm
```

## Grouping by position

Grouping is not only for values. Documents often have structure that is
only implied by the order of elements. The terms of the draft are a
`dl`, a flat list in which each `dt` is followed by its `dd`.
`group-starting-with` starts a new group at each item that matches a
pattern, which turns the flat list into entries:

`glossary.xsl`:

```xml
{{#include ../examples/spec/glossary.xsl}}
```

```console
$ sxml transform glossary.xsl vane.xml
<?xml version="1.0" encoding="UTF-8"?>
<glossary>
  <entry term="Flag">A named switch in an application that turns a feature on for a share of its users.</entry>
  <entry term="SDK">The library in an application that evaluates flags from its cache and keeps the cache current with Vane messages.</entry>
  <entry term="Percentage">A whole number from 0 to 100 that an owner gives to roll a flag out to that share of users. An SDK puts a user in the share by a hash of the flag and the user, so the user stays in it while the percentage grows.</entry>
  <entry term="Pause">A state in which a flag refuses every rollout for a time.</entry>
</glossary>
```

There are two more ways to form groups. `group-adjacent` puts
neighbouring items with the same key together, such as runs of
paragraphs of the same kind, and `group-ending-with` ends a group at
each item that matches a pattern.

## Sorting

`xsl:sort` sorts the items of `xsl:apply-templates`, `xsl:for-each`
and `xsl:for-each-group`. In `term-index.xsl` it sorts the groups by
their key and the terms in each group by name. In `references.xsl`, two
sorts make two sort keys: the year, newest first, as a number
(`data-type="number"`, `order="descending"`), and the anchor for
references of the same year.

Strings sort by their Unicode code points unless a sort names a
*collation*, with `collation` or `lang`: `lang="de"` sorts by the rules
of German. [Working with Text](text.md) sorts the authors of the
references, `M'Raihi` among them, and the anchors `req-1` to `req-11`
in the order people expect.
