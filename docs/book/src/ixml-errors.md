# When a Parse Fails

<!-- examples: ixml -->
<!-- status: draft -->

Two things can go wrong: the input does not match the grammar, or the
grammar itself is wrong. The first gives a document that says where the
parse stopped; the second is an error, FOIX0001, that says what is wrong
with the grammar and where. This chapter shows how to read both and how
to fix the grammar.

## Reading a failed parse

`date.ixml` wants two digits for the month:

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("date.ixml"))("2026-1-20")'
<failed xmlns:ixml="http://invisiblexml.org/NS" ixml:state="failed"><line>1</line><column>7</column><position>7</position><unexpected>"-"</unexpected><expected>["0"-"9"]</expected></failed>
```

The `failed` element says:

- `line` and `column`: where the parse stopped, counting from 1, and
  `position`, the same place counted in characters from the start of the
  input;
- `unexpected`: the character found there, written as the grammar would
  write it, or `end-of-input` when the input ended too soon;
- `expected`: each character set that the grammar could have read at
  that place, once each, in the grammar's notation and in code-point
  order.

The parse reads as far as any reading of the input can go, so the place
it stops is the first character that no reading can take. The mistake is
often a little before it. Here the parser read `2026-1` as the start of
a month and stopped at the hyphen, where it wanted the month's second
digit.

A missing quote is a common case. The third line of this file opens a
quoted field and never closes it:

`bad-prices.txt`:

```
{{#include ../examples/ixml/bad-prices.txt}}
```

```console
$ sxml xquery --param=grammar=prices.ixml --param=input=bad-prices.txt ixml.xq
<failed xmlns:ixml="http://invisiblexml.org/NS" ixml:state="failed">
  <line>4</line>
  <column>1</column>
  <position>59</position>
  <end-of-input/>
  <expected>'"'</expected>
  <expected>~['"']</expected>
</failed>
```

The parse reached the end of the input inside the quoted field, which
may hold line feeds, and still wanted a quote, or more of the field:
`~['"']` is any character but a quote. The failure is reported at the
end, but the cause is the quote on line 3.

## Failures in a query

The `failed` document is a document like any other, so a query can test
for it, `exists($result/failed)` or `$result/*/@*:state = "failed"`, and
read its parts:

```console
$ sxml xpath 'invisible-xml(unparsed-text("date.ixml"))("2026-1-20")/failed/(line, column) ! string()'
"1"
"7"
```

With `fail-on-error`, a failed parse raises FOIX0002, which `try` and
`catch` handle like any other error. This query reports every line of a
file that is not a date, and goes on after each:

`check-dates.xq`:

```xquery
{{#include ../examples/ixml/check-dates.xq}}
```

```console
$ sxml xquery --method=text --param=file=some-dates.txt check-dates.xq
line 2: 2026-1-20 is not a date
line 4: yesterday is not a date
```

## Mistakes in grammars

A grammar that does not follow the rules of iXML raises FOIX0001 when
`invisible-xml` compiles it, and the description says what is wrong.

A missing full stop is the most common mistake. Here line 3 does not end
with one:

`unfinished.ixml`:

```
{{#include ../examples/ixml/unfinished.ixml}}
```

```console
$ sxml xpath 'invisible-xml(unparsed-text("unfinished.ixml"))'
expression:1:1: err:FOIX0001: The grammar does not match the ixml grammar at line 4, column 1, at "d" (F&O 4.0, fn:invisible-xml)
```

The grammar is read like any input, so the place is where reading
stopped, as in a `failed` document: line 3 could go on (`digit, digit`
could be followed by `, more` or by `;`), and only `day` on line 4 shows
that it did not. Look just before the place for the missing `.`, `,` or
quote.

A name used but not defined, usually a typing mistake, is the error S02
of the specification:

`typo.ixml`:

```
{{#include ../examples/ixml/typo.ixml}}
```

```console
$ sxml xpath 'invisible-xml(unparsed-text("typo.ixml"))'
expression:1:1: err:FOIX0001: iXML S02: the grammar uses the nonterminal digti, which no rule defines
```

Two rules for one name is S03. One rule with two alternatives says the
same, and that is what the author meant:

`dup.ixml`:

```
{{#include ../examples/ixml/dup.ixml}}
```

```console
$ sxml xpath 'invisible-xml(unparsed-text("dup.ixml"))'
expression:1:1: err:FOIX0001: iXML S03: the grammar has more than one rule for date
```

The other grammar errors are about characters:

| Error | Cause | Example |
| --- | --- | --- |
| S07 | a hexadecimal character beyond the Unicode range | `#110000` |
| S08 | a hexadecimal character that is a noncharacter or a surrogate | `#FFFF`, `#D800` |
| S09 | a range whose first character comes after its last | `["z"-"a"]` |
| S10 | a class that is not a Unicode general category | `[Xx]` |

## Grammars that make bad XML

Some grammars are correct but make XML that cannot exist for some
inputs, for example an element with two attributes of one name. The
parse succeeds, but no document can hold its result, so the parser
raises FOIX0002 whatever `fail-on-error` says:

`twice.ixml`:

```
{{#include ../examples/ixml/twice.ixml}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("twice.ixml"))("a=b")'
expression:1:1: err:FOIX0002: iXML D02: the parse puts two attributes named key on the element entry
```

Rename one of them (`@key>value`, version 1.1) or make the second an
element. The specification's other errors of this kind:

| Error | Cause |
| --- | --- |
| D03 | a name that is not an XML name, such as one from renaming |
| D04 | a character that XML does not allow, such as one an insertion `+#1` adds |
| D05 | an attribute as the root of the document: the first rule is marked `@` |
| D06 | not exactly one element at the top: the first rule is hidden and gives text, or several elements |
| D07 | an attribute named `xmlns` |

## Ambiguity

An ambiguous grammar is not an error, but it is a warning sign: the
document says `ixml:state="ambiguous"`, and the parser has picked one
reading of several ([Writing Grammars](ixml-grammars.md#ambiguous-grammars)).
To find the cause, shorten the input until the attribute goes away: the
last part you removed holds the ambiguity. Then look for a place where
two parts of the grammar can both take the same characters: two
repetitions side by side, an optional part beside a repetition, a
separator that a field may also contain, or a line feed that two rules
both want, as in the [first try of the semicolon
grammar](ixml-tasks.md#semicolon-separated-values).

## A checklist

When a grammar does not do what you expect:

1. Start from the smallest input that shows the problem.
2. Remove the `-` marks for a moment: every hidden rule and string then
   shows in the XML, and you see how the input was read.
3. Check the ends: the start of the first rule and the end of the input,
   which often has a line feed the grammar does not mention.
4. For an ambiguous result, find the two readings, then say in the
   grammar which one you mean.
5. Compile the grammar once and keep the parser: errors in the grammar
   show at once, before any input is read.
