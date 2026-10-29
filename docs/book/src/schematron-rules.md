# Patterns, Rules and Assertions

<!-- examples: spec -->

`draft.sch` had one rule in each pattern and one assertion in each
rule. A reviewer of the draft looks for more: key words written without
their markup, open issues, references that nothing cites. This chapter
writes those rules, in the `spec` directory, and uses them to show the
rest of the language. Here is the whole schema; the sections below
take it apart:

`review.sch`:

```xml
{{#include ../examples/spec/review.sch}}
```

It finds three things in draft -01:

```console
$ sxml validate --schematron=review.sch vane.xml
vane.xml:116:7: open-issue info: Open issue issue-1 of AM: Should an owner be able to change the duration? (review.sch:34:7)
vane.xml:123:7: anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none. (review.sch:29:7)
vane.xml:125:7: open-issue info: Open issue issue-2 of AB: This needs a requirement anchor of its own. (review.sch:34:7)
```

and every mistake of the broken copy that the XSD schema does not
catch:

```console
$ sed -e 's|<bcp14>MAY</bcp14> report|MAY report|' -e 's|target="term-percentage"|target="term-share"|' -e 's|<xref target="RFC6238"/>|one-time passwords|' vane.xml > broken.xml
$ sxml validate --schematron=review.sch broken.xml
broken.xml:118:7: bare-keyword: MAY without bcp14 markup. (review.sch:20:7)
broken.xml:116:7: open-issue info: Open issue issue-1 of AM: Should an owner be able to change the duration? (review.sch:34:7)
broken.xml:123:7: anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none. (review.sch:29:7)
broken.xml:125:7: open-issue info: Open issue issue-2 of AB: This needs a requirement anchor of its own. (review.sch:34:7)
broken.xml:54:11: used warning: The dt term-percentage is defined, and nothing refers to it. (review.sch:42:7)
broken.xml:169:9: used warning: The reference RFC6238 is defined, and nothing refers to it. (review.sch:42:7)
```

The findings come pattern by pattern, in the order of the schema, and
within a pattern in the order of the document.

## Assert and report

An `assert` describes what should be true: its message is reported when
its test is *false*. A `report` describes what is worth saying: its
message is reported when its test is *true*. Both are *assertions*, and
each message they produce is a *finding*. Choose the one that makes
the test read naturally: "assert that the paragraph has an anchor",
"report a paragraph that has an open issue". `test="true()"` reports
every node the rule applies to, which is what `open-issue` and
`bare-keyword` want: the rule's context has already chosen the nodes.

## Rules and their order

A rule's `context` is an XSLT pattern, the language of `match` in
[Templates and Modes](xslt-templates.md): `t[bcp14]` is any `t` element
with a `bcp14` child, `dt[@anchor] | reference[@anchor]` either of two
kinds of element. The rules of a pattern are tried in order, and for
each node only the *first* rule whose context matches it fires.

The pattern `markup` uses that order. Its first rule matches the
paragraph of BCP 14's boilerplate, which quotes all the key words in
capitals on purpose. The rule has no assertion, only a `p` that says
why it is there: it catches the paragraph, so the second rule, which
looks for bare key words in every `t`, never sees it.

## Variables

`let` binds a name to the value of an expression, as `let` does in
XQuery. Where it stands decides when it is evaluated:

- `$keywords`, a child of `schema`, is evaluated once for the
  document, with the document node as the context;
- `$section`, a child of a rule, is evaluated once for each node the
  rule matches, before its assertions.

A variable is visible in every expression below it: `$keywords` in each
rule, `$section` in the rule's tests and messages. A pattern can have
variables too, evaluated once for the document like the schema's. A
name can be defined only once among the scopes around a rule.

## Messages

The text of an assertion is its message, with white space collapsed.
Inside it:

- `value-of select="..."` is replaced by the string value of its
  expression, evaluated for the node the rule matched. The expression
  must give at most one item: `bcp14[1]` takes the first key word of a
  paragraph, in case it has two.
- `name` is replaced by the name of the node, as written in the
  document; with `path="..."`, by the name of the node the path selects.
  The rule `used` applies to two kinds of element, and its message says
  which: `dt` or `reference`.

A good message says what is wrong and where, in the words of the
document's readers, so a writer can fix it without reading the schema.

## Levels

Not every finding means the same. The `severity` of an assertion says
how much it matters: `fatal`, `error`, `warning` or `info`. `sxml
validate` prints it after the id, and only `fatal` and `error` make the
document invalid. An assertion without a severity can say the same with
its `role` or its `flag`, which older schemas use; `draft.sch` used
`role="warning"`. Without any of the three, a finding is an error, which
is what most rules in practice want.

`open-issue` is `info`: the open issues are not mistakes, but a
reviewer wants the list. `used` is a `warning`: an unused term is
probably a leftover, and the author decides.

## Subjects

A finding is reported at its *subject*: by default, the node the rule
matched. `open-issue`'s rule matches paragraphs, but its finding is
about the issue, so `subject="cref"` points it at the `cref` inside the
paragraph. Lines 116 and 125 are the lines of the two `cref` elements,
not of their paragraphs.

The subject also changes the message's context: `value-of` in a message
is evaluated for the subject. That is why the message of `open-issue`
says `@anchor` and `@source`, the attributes of the `cref`, and
`normalize-space()`, its text.

## Visiting each item

A rule fires once for each node it matches, and its assertions test that
node. Sometimes the things to check are inside the node: the key words
of a paragraph, the names in an attribute that holds a list. The 2025
edition of Schematron adds `visit-each` for them. Its expression is
evaluated for the node the rule matched, and the assertions run once
for each item it gives, with the item as their context.

`bare-keyword`'s rule visits each word in capitals of a paragraph's text,
outside `bcp14` and `cref`, that is a key word: `tokenize` splits the
text at every character that is not a capital letter, and the predicate
keeps the words in `$keywords`. The items are strings, so the report's
`value-of select="."` is the key word, and the finding is reported at
the paragraph, the node the rule matched.

## Groups

A `group` is a pattern in which every rule whose context matches a node
fires, not only the first. The security paragraph is a requirement
without an anchor *and* a paragraph with an open issue, and both of the
group's rules fire for it, at lines 123 and 125. So does the paragraph
of `req-8`, which has an anchor, so only its issue is reported.

With `pattern` in place of `group`, the first rule takes every
requirement, and the second never sees the two paragraphs, which are
both requirements:

```console
$ sed -e 's|<group id="paragraphs">|<pattern id="paragraphs">|' -e 's|</group>|</pattern>|' review.sch > first-rule.sch
$ sxml validate --schematron=first-rule.sch vane.xml
vane.xml:123:7: anchored: A requirement needs an anchor: the SHOULD in "Security Considerations" has none. (first-rule.sch:29:7)
```

Use a pattern when the rules are cases of one check, ordered from the
special to the general, as in `markup`; use a group when they are
independent checks that happen to share elements.

## Diagnostics

A message says what is wrong. A *diagnostic* can say more: a hint, a
value, how to fix it. Diagnostics are written once, in `diagnostics`,
and an assertion names the ones it wants in its `diagnostics`
attribute. `next-anchor` computes the anchor the requirement could get:
one more than the highest number among the `req-` anchors.

The text output keeps to one line per finding, and leaves diagnostics
out. They are in the JSON report, with every other field of a finding
([Reports, Errors and Exit Statuses](schematron-reports.md) has the
details), which the book's tools read like any other JSON:

```console
$ sxml validate --schematron=review.sch --format=json vane.xml > review.json
$ sxml xpath 'json-doc("review.json")?files?*?findings?*[exists(?diagnostics?*)] ! `{?id}: {?diagnostics?*?text}`'
"anchored: The next free anchor is req-12."
```

## Namespaces

The expressions of a schema know two kinds of prefixes: those of the
function library, which every query binding of Schematron makes
available (`xs`, `fn`, `math`, `map`, `array` and `err`), and those
that `ns` elements declare. They do not know the namespace declarations
of the schema document. The diagnostic calls `xs:integer`, and
`review.sch` needs no `ns` element for it; a prefix for the names of a
document does need one, and without it the schema does not compile
([Reports, Errors and Exit Statuses](schematron-reports.md) shows the
error). An `ns` element may bind one of the library's prefixes to
another namespace, and then its binding counts. The draft has no
namespace, so its names need none; the state machine's rules, in the
next chapter, need two.
