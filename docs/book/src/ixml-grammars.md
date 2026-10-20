# Writing Grammars

<!-- examples: ixml -->

A grammar is a list of rules. This chapter goes through everything a
rule can say, with a small grammar for each feature. All the grammars
are files in `docs/book/examples/ixml`.

## Rules and alternatives

A rule is a name, a colon (or `=`), one or more *alternatives* and a full
stop. Alternatives are separated by `;` (or `|`), and the parts of an
alternative by commas:

`greeting.ixml`:

```
{{#include ../examples/ixml/greeting.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("greeting.ixml"))("Hello, World!")'
<greeting><hello>Hello</hello><name>World</name></greeting>
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("greeting.ixml"))("Hi, Ada!")'
<greeting><hello>Hi</hello><name>Ada</name></greeting>
```

Spaces and line breaks between the parts of a rule do not matter, and
text in braces, `{like this}`, is a comment. An alternative may be
empty: `sign: "-"; .` is a minus sign or nothing.

## Characters

The smallest parts of a grammar match single characters:

| Notation | Matches |
| --- | --- |
| `"abc"` or `'abc'` | the characters a, b and c, in that order; `""` inside `"..."` is one quote |
| `#a`, `#20AC` | the one character with that hexadecimal code point: line feed, the euro sign |
| `["a"-"z"; "_"; #9]` | one character of a set: here a letter from a to z, an underscore or a tab |
| `~["0"-"9"]` | one character that is *not* in the set |
| `[L]`, `[Nd]`, `[Lu; Ll]` | one character of a Unicode general category: any letter, a decimal digit, an upper- or lowercase letter |

`fields.ixml` reads a word in any script, a number with an optional
fraction, and a quoted text, separated by tabs:

`fields.ixml`:

```
{{#include ../examples/ixml/fields.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("fields.ixml"))(unparsed-text("fields.txt"))'
<fields><word>Zürich</word><number>42.5</number><quoted>two words</quoted></fields>
```

sxml's categories are those of Unicode 16.0, the version its regular
expressions use.

## Repetition and options

After any part:

- `?` makes it optional;
- `*` repeats it zero or more times, `+` one or more times;
- `**sep` and `++sep` repeat it with `sep` between the repetitions, so
  `item++","` is one or more items separated by commas.

Parentheses group parts into one: `("+"; "-")?` is an optional sign.

`list.ixml`:

```
{{#include ../examples/ixml/list.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("list.ixml"))("[1, 22,333 ]")'
<list><item>1</item><item>22</item><item>333</item></list>
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("list.ixml"))("[]")'
<list/>
```

## Elements, attributes and hidden parts

A *mark* before a name says what the part becomes in the XML:

- `^` (the default): an element;
- `@`: an attribute, which goes on the nearest element around it;
- `-`: nothing of its own; its content goes into the element around it.

A mark on a rule applies wherever the rule is used. A mark where a name
is used applies there only and wins over the rule's. Strings and
character sets take `-` too, which drops their characters, as the
hyphens of the date did.

`person.ixml`:

```
{{#include ../examples/ixml/person.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("person.ixml"))("Ada Lovelace, 36, London")'
<person age="36"><name first="Ada" last="Lovelace"/>London</person>
```

`first` and `last` are attributes of `name`, the element they are in;
`age` is an attribute of `person`. `city` is hidden, so its text is part
of `person`'s content. An attribute's value is all the text inside it:
marks inside an attribute only drop characters.

## Insertions

`+"text"` adds text to the XML that is not in the input. An insertion
matches nothing, so it can go anywhere in an alternative:

`temperature.ixml`:

```
{{#include ../examples/ixml/temperature.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("temperature.ixml"))("21C")'
<temperature><number>21</number><unit>celsius</unit></temperature>
```

The input says `C`; the XML says `celsius`. Insertions are how a grammar
fills in what the text leaves out, such as the leading zero of a month
in the [dates task](ixml-tasks.md#dates).

## Renaming

A rule describes one form of text, and its name is the element's name.
Sometimes one form appears in two roles: the two numbers of a range are
both numbers, but one is the low end and one the high end. `>` gives a
part another name in the XML:

`range.ixml`:

```
{{#include ../examples/ixml/range.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("range.ixml"))("1..10")'
<range><low>1</low><high>10</high></range>
```

`>` can also follow a rule's name, to rename it everywhere. Renaming is
part of version 1.1 of the specification, so the grammar starts with
`ixml version "1.1".`; sxml reads the rest of a 1.0 grammar the same
way.

## Ambiguous grammars

A grammar is *ambiguous* when it describes a text in more than one way.
This grammar reads a name as a first and a last part, but either part
may contain spaces:

`name.ixml`:

```
{{#include ../examples/ixml/name.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("name.ixml"))("Mary Ann Smith")'
<name xmlns:ixml="http://invisiblexml.org/NS" ixml:state="ambiguous"><first>Mary</first><last>Ann Smith</last></name>
```

"Mary" and "Ann Smith" is one way to read the text; "Mary Ann" and
"Smith" is another. An ambiguous parse is not an error: the parser
returns one of the ways and marks the document element with
`ixml:state="ambiguous"`. Which way it picks is not specified, so a
query must not depend on it. The attribute is in the namespace
`http://invisiblexml.org/NS`, so a query tests it as
`@Q{http://invisiblexml.org/NS}state` or `@*:state`.

Ambiguity is almost always a mistake in the grammar. Ask where a part
can end: here, at any space. The fix is to say which part may contain
spaces. If the last name is one word, the reading is unique:

```console
$ sxml xpath --method=xml 'invisible-xml(replace(unparsed-text("name.ixml"), "last: \[L; "" ""\]\+", "last: [L]+"))("Mary Ann Smith")'
<name><first>Mary Ann</first><last>Smith</last></name>
```

(The expression changes the grammar's text before compiling it, a quick
way to try a fix.) Three more causes of ambiguity are common: two
repetitions next to each other that can both take the same character
(`"x"*, "x"*`); an optional part next to a repetition that can stand for
it; and a separator that a field may also contain. The
[tasks](ixml-tasks.md) and [errors](ixml-errors.md) chapters show each.

## The prolog

A grammar may start with `ixml version "1.0".` or `ixml version "1.1".`.
sxml reads both versions. For any other version it parses with its own
and marks the result with `ixml:state="version-mismatch"`, as the
specification asks.
