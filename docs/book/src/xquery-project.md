# Project: Editing a Specification Without Rewriting It

<!-- examples: spec -->

Let's put the whole chapter to work. A specification lives a long time,
and most of its changes are small: a keyword becomes stronger, a
requirement is added, a reference comes in, a term gets a better name.
Each of those is a change to a few nodes. Retyping the text around them,
by hand or by asking a language model to rewrite the section, puts every
other sentence at risk, and leaves the reviewers to find out which words
moved.

In this project we'll build a small toolkit for the Vane draft that
works the other way:

- `edit.xqm`, a module of updating functions, one for each kind of edit;
  each changes only the nodes it names;
- `check.xq`, which finds the mistakes the schema cannot see, and fails
  so that a script stops;
- `changes.xq`, which lists what changed in the requirements between two
  versions, as text for people and JSON for tools.

At the end we'll make draft -02 in three commands, check it, and read
the whole change as a diff of a few lines. Everything runs in the
specification project's directory, with `spec.xqm` from
[Functions and Modules](xquery-modules.md) underneath.

## The edits

`edit.xqm`:

```xquery
{{#include ../examples/spec/edit.xqm}}
```

Let's read it from the top.

- The module imports `spec.xqm`, as a query does: modules build on each
  other.
- `declare boundary-space preserve` keeps the whitespace written in the
  constructors, so that a new reference is indented like the draft's
  own references.
- `edit:keyword` is an enumeration, so a misspelled keyword is an error
  at the call. `edit:rfc` is a record with a default: a reference is
  informative unless the caller says otherwise.
- `edit:next-anchor` is an ordinary function. It finds the anchor that
  has never been used, so that anchors stay names given once.
- Every other public function is an updating function, and does one
  kind of edit. They insert the indentation of their neighbours with the
  nodes they add, which keeps the diff to the lines that matter.

## Trying the pieces

Ordinary functions can be tried on their own:

```console
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:next-anchor(/), edit:next-anchor(doc("vane-00.xml")),
    edit:rfc(5424, "The Syslog Protocol", "R. Gerhards", 2009, "March")'
"req-12"
"req-9"
{"number":5424,"title":"The Syslog Protocol","authors":"R. Gerhards","year":2009,"month":"March","normative":false()}
```

Updating functions are tried as updating queries. As in
[Updating Documents](xquery-updates.md), compare the result with the
draft as sxml writes it:

```console
$ sxml serialize vane.xml > before.xml
$ sxml xquery --context-item=vane.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:rename-term(//dt[@anchor = "term-percentage"], "Rollout Percentage")' > after.xml
$ diff before.xml after.xml
51c51
<           <dt anchor="term-percentage">Percentage</dt>
---
>           <dt anchor="term-rollout-percentage">Rollout Percentage</dt>
89c89
<         <xref target="term-percentage">percentage</xref> of a rollout
---
>         <xref target="term-rollout-percentage">rollout percentage</xref> of a rollout
```

The types of the parameters guard the edits. BCP 14 has no MIGHT:

```console
$ sxml xquery --context-item=vane.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:set-keyword(//t[@anchor = "req-3"], "MIGHT")'
expression:3:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

One thing the types can't see. All the edits of one query see the
draft as it was before the query, so two requirements added in one query
both get the next anchor, the same one. The schema finds that, as its
`xs:unique` on anchors forbids it:

```console
$ sxml xquery --context-item=vane.xml --output=twice.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:add-requirement(//t[@anchor = "req-11"], <t>A service <bcp14>MAY</bcp14> log each rollout.</t>),
    edit:add-requirement(//t[@anchor = "req-8"], <t>A service <bcp14>MAY</bcp14> log each pause.</t>)'
$ sxml validate --schema=vane.xsd twice.xml
twice.xml:118:7: cvc-identity-constraint.4.1: element 't': its key sequence is that of an earlier member (unique 'anchor')
```

Add one requirement per command, as the session below does.

## The check

A schema says what a valid draft looks like: which elements go where,
which keywords exist, that anchors are unique and cross references
point to anchors. It can't say that every term is used, that no MUST
was written without its markup, or that a paragraph with a keyword has
an anchor. Those rules need a query:

`check.xq`:

```xquery
{{#include ../examples/spec/check.xq}}
```

`$problems` is a sequence of lines, made by four FLWOR expressions one
after another. When it is empty, the query writes one line; otherwise
it writes each problem with `fn:message`, on standard error, and raises
`spec:check`, which ends the query with exit status 1. Draft -01 has the
one problem we know of:

```console
$ sxml xquery --context-item=vane.xml check.xq; echo "exit status $?"
security: a requirement without an anchor (SHOULD)
check.xq:35:3: spec:check: 1 problem(s) in draft-example-vane-protocol-01
exit status 1
```

To see the other checks at work, break a copy of the draft in three
places: a keyword loses its markup, a cross reference names a term that
doesn't exist, and the one-time password reference is no longer cited:

```console
$ sed -e 's|<bcp14>MAY</bcp14> report|MAY report|' -e 's|target="term-percentage"|target="term-share"|' -e 's|<xref target="RFC6238"/>|one-time passwords|' vane.xml > broken.xml
$ sxml xquery --context-item=broken.xml check.xq; echo "exit status $?"
req-4: a cross reference to the missing anchor term-share
term-percentage: a term that nothing refers to
RFC6238: a reference that nothing refers to
req-11: MAY without bcp14 markup
security: a requirement without an anchor (SHOULD)
check.xq:35:3: spec:check: 5 problem(s) in draft-example-vane-protocol-01
exit status 1
```

The renamed target counts twice, as it should: the reference is
dangling, and the term `term-percentage` lost its only use.

Rules like these, each a sentence about one place in the document, can
also be written in Schematron, the schema language for rules between
values, which reports each problem with its line and a level of its
own: [Checking Rules with Schematron](schematron.md) starts from these
checks.

## The changes

Reviewers of a new version want to know what changed in the
requirements, not in the commas. Requirements are matched by anchor,
which is why anchors never change:

`changes.xq`:

```xquery
{{#include ../examples/spec/changes.xq}}
```

The entries come from `spec:entry`, so the text is the text a reader
sees, on one line. A keyword change and a text change are told apart by
`local:wording`, which leaves the keyword out, so a new keyword is not
also reported as new text. From -00 to -01:

```console
$ sxml xquery changes.xq
req-9 added: A service MAY ask an owner for a time-based one-time password [RFC6238] before a rollout.
req-10 added: An SDK SHOULD NOT send more than one status request per second for a flag; it uses its cached answer in between.
req-7 text: After five failed checks in a row, a service MUST put the flag in a pause.
    now: After three failed checks in a row, a service MUST put the flag in a pause.
req-8 text: A pause SHOULD last one minute.
    now: A pause SHOULD last five minutes.
req-11 added: A service MAY report a pause to the owners of the flag as soon as it begins.
```

Nothing was removed in -01; the other way round, three requirements
were:

```console
$ sxml xquery --param=old=vane.xml --param=new=vane-00.xml changes.xq | grep removed
req-9 removed: A service MAY ask an owner for a time-based one-time password [RFC6238] before a rollout.
req-10 removed: An SDK SHOULD NOT send more than one status request per second for a flag; it uses its cached answer in between.
req-11 removed: A service MAY report a pause to the owners of the flag as soon as it begins.
```

`$format` is an enumeration too, so the command line can only ask for
what the query can write:

```console
$ sxml xquery --param=format=xml changes.xq
changes.xq: err:FORG0001: the value is not in the value space of the target type
```

The JSON form follows in the session.

## An edit session

Now the real work: draft -02. The working group decided three things.
The SHOULD in Security Considerations becomes a requirement, and the
note asking for it goes. The service should log every failed check,
with syslog. And a rolling-out flag *must* return to stable when its
step is over, not only *should*. Each decision is one command, and each
command writes a new file with `--output`, so every step can be looked
at:

```console
$ sxml xquery --context-item=vane.xml --output=step-1.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:new-version(/, xs:date("2026-11-30")),
    edit:give-anchor(//section[@anchor = "security"]/t[bcp14]),
    edit:remove-note(//cref[@anchor = "issue-2"])'
$ sxml xquery --context-item=step-1.xml --output=step-2.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:add-requirement(//t[@anchor = "req-11"],
      <t>A service <bcp14>SHOULD</bcp14> log every failed check with syslog <xref target="RFC5424"/>.</t>),
    edit:add-reference(/, edit:rfc(5424, "The Syslog Protocol", "R. Gerhards", 2009, "March"))'
$ sxml xquery --context-item=step-2.xml --output=drafts/vane-02.xml --expression='
    import module namespace edit = "urn:example:edit" at "edit.xqm";
    edit:set-keyword(//t[@anchor = "req-3"], "MUST")'
```

The second command adds a requirement and the reference it cites in one
query: they belong together, and they touch different nodes. Is the new
draft valid, and does it pass the check?

```console
$ sxml validate --schema=vane.xsd drafts/vane-02.xml; echo "exit status $?"
exit status 0
$ sxml xquery --context-item=drafts/vane-02.xml check.xq
draft-example-vane-protocol-02: no problems
```

What changed in the requirements, as JSON for the issue tracker:

```console
$ sxml xquery --param=old=vane.xml --param=new=drafts/vane-02.xml --param=format=json changes.xq
[
  {
    "anchor": "req-3",
    "change": "keyword",
    "old": "SHOULD",
    "new": "MUST"
  },
  {
    "anchor": "req-13",
    "change": "added",
    "old": null,
    "new": "A service SHOULD log every failed check with syslog [RFC5424]."
  },
  {
    "anchor": "req-12",
    "change": "added",
    "old": null,
    "new": "Vane messages travel in clear text on the network. Deployments SHOULD carry Vane over an encrypted link."
  }
]
```

And the whole change, for the reviewers, is this diff:

```console
$ diff before.xml drafts/vane-02.xml
5c5
<      numbers sections and requirements in document order. --><rfc version="3" category="std" ipr="trust200902" submissionType="IETF" docName="draft-example-vane-protocol-01">
---
>      numbers sections and requirements in document order. --><rfc version="3" category="std" ipr="trust200902" submissionType="IETF" docName="draft-example-vane-protocol-02">
8c8
<     <seriesInfo name="Internet-Draft" value="draft-example-vane-protocol-01"/>
---
>     <seriesInfo name="Internet-Draft" value="draft-example-vane-protocol-02"/>
17c17
<     <date year="2026" month="October" day="21"/>
---
>     <date year="2026" month="November" day="30"/>
69c69
<       <t anchor="req-3">A rolling-out flag <bcp14>SHOULD</bcp14> return to the
---
>       <t anchor="req-3">A rolling-out flag <bcp14>MUST</bcp14> return to the
116a117
>       <t anchor="req-13">A service <bcp14>SHOULD</bcp14> log every failed check with syslog <xref target="RFC5424"/>.</t>
120c121
<       <t>Vane messages travel in clear text on the network. Deployments
---
>       <t anchor="req-12">Vane messages travel in clear text on the network. Deployments
122,123c123
<       <cref anchor="issue-2" source="AB">This needs a requirement anchor of
<       its own.</cref></t>
---
>       </t>
175a176,183
>         </reference>
>         <reference anchor="RFC5424" target="https://www.rfc-editor.org/info/rfc5424">
>           <front>
>             <title>The Syslog Protocol</title>
>             <author fullname="R. Gerhards" initials="R." surname="Gerhards"/>
>             <date year="2009" month="March"/>
>           </front>
>           <seriesInfo name="RFC" value="5424"/>
```

Every changed line is one that a decision named. The other lines of
the draft are as they were, and nobody had to read them again. The
new version sits with the others, ready for the queries of
[Documents, Parameters and the Context](xquery-inputs.md):

```console
$ sxml xquery --method=adaptive --expression='collection("drafts") ! string(rfc/@docName)'
"draft-example-vane-protocol-00"
"draft-example-vane-protocol-01"
"draft-example-vane-protocol-02"
$ sxml xquery --param=today=2026-11-30 --param=drafts=drafts/vane-02.xml expiry.xq
draft-example-vane-protocol-02: published 2026-11-30, expires 2027-06-03
```

## What we built

The three tools are short because the module does the work, and they
agree with each other because the rules are written once: what a
requirement is, how it is numbered and what its text is live in
`spec.xqm`, and the edits, the check and the list of changes all use
them. Along the way, the project used most of this chapter:

- library modules that import each other, with record types, enumeration
  types, typed functions, a private helper and an error code of their
  own;
- updating functions that insert, delete and replace exactly the nodes
  of one decision, and `transform with` inside one of them;
- FLWOR expressions with `where`, joins of two versions by anchor, and
  `switch` on the kind of change;
- external variables with a type and a default, and an enumeration that
  limits them;
- `fn:message` and `error` for a check that a script can rely on;
- serialization as text and as JSON, and `--output` for each step.

[Project: Publishing the Specification](xslt-project.md) takes the
draft the rest of the way, to HTML and RFC-style text, with XSLT, and
runs this chapter's check before it publishes.

## Summary

XQuery adds to XPath what you need to write programs:

- FLWOR expressions iterate, filter, sort, group, number and join;
- constructors build XML, and output declarations write it as XML,
  XHTML, HTML, text, JSON or CSV;
- `switch`, `typeswitch`, sequence types and `try`/`catch` deal with
  untidy data;
- functions, which can be recursive, and library modules organize a
  program, and external variables make it a tool;
- updates change documents, or copies of them, one named node at a
  time.

Next, a different way to think about turning documents into other
documents: XSLT.
