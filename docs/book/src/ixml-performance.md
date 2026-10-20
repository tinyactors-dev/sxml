# Invisible XML Performance

<!-- examples: ixml -->
<!-- status: draft -->

An iXML grammar can describe any context-free language, and a parser
for all of them must be able to keep many readings of the input open at
once. That is more work than a parser made for one fixed language, such
as sxml's XML parser, does. This chapter says how much work, which
grammars sxml parses in time proportional to the input, and how to write
grammars that stay fast on inputs of many megabytes.

## What to expect

The times below are processor seconds of a release build on one core of
an ordinary Linux machine, for the whole `sxml` command: starting,
reading the input, parsing, and building the XML tree. *Memory* is the
most memory the process held, about 10 MB of it the program itself;
*per MB* is how much more each further megabyte of input takes, the
memory the parse and its XML really cost. The benchmark suite
`tests/bench/run-ixml-bench.py` measures them, and its `--check` option
fails when a number is worse than this chapter says (`--large` adds the
100 MB log).

| Grammar | Input | Time | Memory | Per MB |
| --- | --- | --- | --- | --- |
| the log of [Real Tasks](ixml-tasks.md#log-files) | 1 MB, 16,000 lines | under 0.2 s | under 60 MB | under 20 MB |
| semicolon-separated values | 1 MB, 34,000 rows | under 0.4 s | under 100 MB | under 60 MB |
| the small programming language | 1 MB, 43,000 statements | under 0.6 s | under 150 MB | under 100 MB |
| Markdown-like text | 1 MB, 8,700 sections | under 0.4 s | under 120 MB | under 80 MB |
| a right-recursive list | 1 MB, 159,000 numbers | under 0.4 s | under 80 MB | under 40 MB |
| brackets nested 100,000 deep | 200 KB | under 0.2 s | under 100 MB | |
| a grammar of 5,800 rules, compiled | 200 KB | under 0.5 s | under 200 MB | |
| the ambiguous sum below | 2,000 characters | under 2 s | under 200 MB | |
| the log | 10 MB, 160,000 lines | under 1.5 s | under 250 MB | under 20 MB |
| the log | 100 MB, 1,600,000 lines | under 15 s | under 2,000 MB | under 20 MB |

For every grammar in the table but the ambiguous sum, four times the
input takes about four times as long and four times the memory: the
parser is *linear* on them. A large input costs what its XML costs and
little more. The XML tree of the log, an element and three attributes
for each line and the messages as text, takes about 9 bytes for each
character of the log; the parse adds less than that again, and lets go
of all of it before the query goes on.

A 1 MB log, made by a query:

`make-log.xq`:

```xquery
{{#include ../examples/ixml/make-log.xq}}
```

```console
$ sxml xquery --method=text --param=lines=25000 --output=big.log make-log.xq
$ sxml xpath 'string-length(unparsed-text("big.log"))'
972227
$ sxml xquery --method=text --param=file=big.log count-levels.xq
ERROR 8333
INFO 8333
WARN 8334
```

`count-levels.xq` is the grouping query of the log task, reading the
file that `$file` names.

## Which grammars are linear

sxml parses with Earley's algorithm, which reads the input once, from
left to right, and keeps a set of *items* for each position: the rules
that a reading could be in the middle of there. The work is the number
of items, and five refinements keep it in proportion to the input and
small:

- **One character of lookahead.** An item is kept only when the next
  character can continue it, so the parser does not follow readings that
  the next character already rules out. A rule that could end anywhere,
  such as `~[#a]*`, ends only where the next character can follow it.
- **Leo's items for right recursion.** A rule that ends with itself,
  such as `rest: ",", number, rest?`, would otherwise complete once for
  every level of the recursion at every place the list could end; sxml
  completes the whole chain at once.
- **Compiled character classes.** Each character of the input is looked
  up once in a table that says which of the grammar's character sets it
  belongs to.
- **Groups written out.** A group or an option, such as `(char; em)` or
  `inline?`, and a hidden rule of one character set, such as `-digit`,
  is replaced where it is used, so the parser does not complete a rule
  of its own for every character. The XML is the same.
- **Regular rules read at once.** A rule such as `@date: digit, digit,
  ...` or `message: ~[#a]*` needs no nesting: it is made of characters,
  character sets, repetitions and other such rules, without recursion,
  so its language is *regular*. When its XML is nothing but its text, as
  for a hidden rule, an attribute, or an element with only text inside,
  sxml compiles it once, with the grammar, into an automaton. The parser
  runs the automaton over the input in one step instead of making items
  for every character, and completes the rule at each place it can end
  where the next character can follow it. The XML is the same, and so is
  an ambiguity inside such a rule: the automaton counts the ways it
  reaches each end.

With these, the parser is linear on every grammar that has one reading
for each input and can tell, at each place, how to go on with a bounded
look ahead: the *LR-regular* grammars of parsing theory (Joop Leo, 1991).
They include left recursion, right recursion, nesting, repetition and
every grammar of [Real Tasks](ixml-tasks.md). A right-recursive list of
200,000 numbers:

`numbers.ixml`:

```
{{#include ../examples/ixml/numbers.ixml}}
```

```console
$ sxml xpath --method=text --output=numbers.txt 'string-join(1 to 200000, ",")'
$ sxml xpath 'count(invisible-xml(unparsed-text("numbers.ixml"))(unparsed-text("numbers.txt"))//number)'
200000
```

Brackets nested 100,000 deep:

```console
$ sxml xpath 'count(invisible-xml("nest: -""("", nest?, -"")"".")(string-join(((1 to 100000) ! "(", (1 to 100000) ! ")")))//nest)'
100000
```

## Large inputs

The parser needs memory for its items and for the parse tree it reads
out of them. A rule read at once makes no items inside it and one node
for all its text; characters and hidden rules that the XML leaves out
make no nodes, and neighbouring characters share one. So the memory
follows the rules the parser cannot read at once, and is small when a
grammar's text is in rules that it can: the log needs some 20 items and
5 nodes for each line of about 60 characters. All of it is released
when the XML is built.

A log of 10 MB:

```console
$ sxml xquery --method=text --param=lines=250000 --output=big.log make-log.xq
$ sxml xpath 'string-length(unparsed-text("big.log"))'
9972228
$ sxml xquery --method=text --param=file=big.log count-levels.xq
ERROR 83333
INFO 83333
WARN 83334
```

## Ambiguous grammars

A grammar with many readings costs more. This one says nothing about
how a sum groups, so a sum of *n* numbers has as many readings as there
are ways to put in brackets, which grows faster than any power of *n*:

`sum.ixml`:

```
{{#include ../examples/ixml/sum.ixml}}
```

```console
$ sxml xpath 'let $sum := invisible-xml(unparsed-text("sum.ixml"))(string-join(1 to 300, "+")) return (count($sum//number), string($sum/sum/@*:state))'
300
"ambiguous"
```

sxml never lists the readings. It keeps one for each item, so the items
grow with the square of the input and the time with its cube: 2,000
characters take one to two seconds, eight times as long as 1,000. The
cure is the cure of [Writing Grammars](ixml-grammars.md#ambiguous-grammars):
say how the parts group, here `sum: sum, -"+", number; number.`, and the
grammar is linear again.

## Steps and limits

Every item the parser makes, every node of the parse tree, and every
run of an automaton, with one more for every 16 characters it reads,
counts as one step of the evaluation, so the step limit that stops a
runaway query stops a runaway parse too. A linear grammar makes a few
items for each character that no automaton reads, so the default limit
of 10<sup>8</sup> steps allows inputs of many megabytes: the log of
100 MB parses within it. A host that embeds sxml can set a higher limit
(Chapter "Embedding sxml in C").

## Writing fast grammars

1. **Avoid ambiguity.** It is the one thing that makes a parse more than
   linear, and it is a mistake in the grammar anyway.
2. **Hide what you do not need.** Hidden rules and strings make no
   elements, so the tree is smaller and the query after the parse is
   faster.
3. **Use character sets for runs of characters.** `[L]+` is one set read
   many times; `letter+` with `letter: "a"; "b"; ...` is one rule with
   many alternatives at every character.
4. **Keep text in rules of its own.** A rule whose XML is just its
   text is read at once when it is regular (see above). A mark or an
   insertion inside it, or a hidden character among visible ones, such
   as the `-'"'` of a quoted field, makes it an ordinary rule again:
   leave such parts to the rule around it, as `-quoted: -'"', text,
   -'"'` with `-text: ~['"']*` does.
5. **Compile once.** Keep the parser in a variable and call it for every
   input ([Compile once](ixml.md#compile-once-parse-many-times)).
