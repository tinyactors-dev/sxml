# Updating Documents

<!-- examples: spec -->

XQuery can also change documents, with the *XQuery Update Facility*. Its
expressions insert, delete, replace and rename nodes. For a
specification, that is a way of working: an edit names the nodes it
changes, and everything else stays as it was, down to the line breaks.
Nobody retypes the draft, by hand or with a language model, to change
one sentence, and a reviewer reads a diff of three lines instead of
comparing two long texts.

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
can look at the result before you keep it. The best way to look is a
diff. The parser keeps what the document means, not every byte of the
file: a line break inside a start tag, a CDATA section and the
whitespace before the root element are not part of the tree. So write
the draft once as sxml writes it, and compare the edits with that:

```console
$ sxml serialize latch.xml > before.xml
```

The draft's editorial note `issue-2` says that the SHOULD in Security
Considerations needs an anchor, which would make it a requirement. One
query gives it the anchor and removes the note:

`give-anchor.xq`:

```xquery
{{#include ../examples/spec/give-anchor.xq}}
```

```console
$ sxml xquery --context-item=latch.xml give-anchor.xq > after.xml
$ diff before.xml after.xml
109c109
<       <t>A code travels in clear text on the local network. Deployments
---
>       <t anchor="req-12">A code travels in clear text on the local network. Deployments
111,112c111
<       <cref anchor="issue-2" source="AB">This needs a requirement anchor of
<       its own.</cref></t>
---
>       </t>
```

`insert node attribute ... into` adds an attribute to an element. The
new anchor is the next name never used, `req-12`: anchors are names
given once, not numbers to keep in order. The line break and spaces in
front of the note are a text node of their own, and they stay; that's
why `</t>` is now on a line of its own.

Updating expressions combine with everything else. A FLWOR expression
whose `return` updates makes one change for each binding, and several
changes can be made at once, separated by commas, as above.

## Adding a requirement

`insert node ... after` puts a node after another one. To keep the file
tidy, the query inserts the line break and the indentation too, as a
text node:

`new-requirement.xq`:

```xquery
{{#include ../examples/spec/new-requirement.xq}}
```

```console
$ sxml xquery --context-item=latch.xml new-requirement.xq > after.xml
$ diff before.xml after.xml
103a104,105
>       <t anchor="req-13">A <xref target="term-controller">controller</xref>
>       <bcp14>MAY</bcp14> end a lockout early.</t>
```

`declare boundary-space preserve` matters here. Without it, the line
break between `</xref>` and `<bcp14>` is boundary whitespace, which a
constructor drops ([Building XML](xquery-constructors.md)), and the
paragraph would read "controllerMAY". Even a single space between two
tags is boundary whitespace.

## One change, many places

A pending update list can hold many changes, and they all happen
together or not at all. Renaming a term touches its definition, its
anchor, and every cross reference to it:

`rename-term.xq`:

```xquery
{{#include ../examples/spec/rename-term.xq}}
```

```console
$ sxml xquery --context-item=latch.xml rename-term.xq > after.xml
$ diff before.xml after.xml
32c32
<       <xref target="term-controller">controller</xref> sends Latch messages
---
>       <xref target="term-hub">hub</xref> sends Latch messages
44c44
<           <dt anchor="term-controller">Controller</dt>
---
>           <dt anchor="term-hub">Hub</dt>
```

`replace value of node` on an element replaces its content with one
text node; on an attribute, its value. The edit changed exactly the
marked-up uses of the term. The draft also says "controller" in plain
text, without a cross reference, and those words are not the edit's to
guess at. A query finds them for the author:

```console
$ sxml xquery --context-item=after.xml --method=adaptive --expression='
    //text()[not(ancestor::xref | ancestor::dt)][contains(lower-case(.), "controller")]
      ! ancestor::*[@anchor or self::abstract][1] ! (@anchor, name())[1] ! string()'
"abstract"
"req-9"
"req-10"
"issue-1"
"req-11"
```

Because all the changes are applied at the end, an updating query never
sees its own changes, and the order of the changes in the query doesn't
matter: the `for` above finds the cross references by their old target,
although the same query changes it.

## Moving a section

A move is an insert of the node somewhere else and a delete of the
original. The inserted node is a copy, with everything inside it:

`move-section.xq`:

```xquery
{{#include ../examples/spec/move-section.xq}}
```

```console
$ sxml xquery --context-item=latch.xml move-section.xq > moved.xml
$ diff before.xml moved.xml
76a77,84
>       <section anchor="status">
>         <name>Status</name>
>         <t anchor="req-6">A lock <bcp14>MUST</bcp14> answer a status request
>         with its state and the number of failed attempts since it last
>         unlocked.</t>
>         <t anchor="req-10">A controller <bcp14>SHOULD NOT</bcp14> send more
>         than one status request per second to a lock.</t>
>       </section>
87,94d94
<       </section>
<       <section anchor="status">
<         <name>Status</name>
<         <t anchor="req-6">A lock <bcp14>MUST</bcp14> answer a status request
<         with its state and the number of failed attempts since it last
<         unlocked.</t>
<         <t anchor="req-10">A controller <bcp14>SHOULD NOT</bcp14> send more
<         than one status request per second to a lock.</t>
```

Now Status is section 3.1 and Unlock 3.2, and nothing in the draft had
to be renumbered, because no number is written in it. The table of
contents and the requirements are numbered from positions when they are
shown:

```console
$ sxml xquery --context-item=moved.xml toc.xq
1. Introduction
1.1. Terminology
2. Lock States
3. Messages
3.1. Status
3.2. Unlock
4. Lockout
5. Security Considerations
6. IANA Considerations
$ sxml xquery --context-item=moved.xml --param=anchor=req-5 requirement.xq
req-5 (MUST NOT, section 3.2): A lock MUST NOT unlock while it is in lockout (Section 4).
```

## Why anchors are never renumbered

It is tempting to tidy the anchors: req-9 sits between req-5 and req-6,
because it was written later. A query can renumber them in document
order, and every cross reference with them:

`renumber.xq`:

```xquery
{{#include ../examples/spec/renumber.xq}}
```

```console
$ sxml xquery --context-item=latch.xml renumber.xq > renumbered.xml
$ diff before.xml renumbered.xml | grep '^>'
>         <t anchor="req-6">A controller <bcp14>MAY</bcp14> send codes that are
>         <t anchor="req-7">A lock <bcp14>MUST</bcp14> answer a status request
>         <t anchor="req-8">A controller <bcp14>SHOULD NOT</bcp14> send more
>       <t anchor="req-9">After three rejected codes in a row, a lock
>       <t anchor="req-10">A lockout <bcp14>SHOULD</bcp14> last five minutes.
```

The draft is consistent afterwards. But the draft is not the only
document that names its anchors. The lock's state machine, `lock.scxml`
from [Project: Self-Describing State Machines](scxml.md), says which
requirements each state implements, by anchor. Look at what those
anchors mean after the renumbering:

```console
$ sxml xquery --method=adaptive --expression='
    declare namespace latch = "urn:example:latch";
    for $anchor in distinct-values(doc("lock.scxml")//@latch:implements ! tokenize(.))
    let $before := doc("latch.xml")//t[@anchor = $anchor]
    let $after := doc("renumbered.xml")//t[@anchor = $anchor]
    where not(deep-equal($before, $after))
    return `{$anchor}: {$before/bcp14} became {$after/bcp14} ({substring(normalize-space($after), 1, 40)}...)`'
"req-6: MUST became MAY (A controller MAY send codes that are tim...)"
"req-7: MUST became MUST (A lock MUST answer a status request with...)"
"req-8: SHOULD became SHOULD NOT (A controller SHOULD NOT send more than o...)"
```

The machine's status transition now claims to implement the TOTP
requirement, and nothing reports an error, because every anchor still
exists. An anchor is a name that other documents use; keep it, and let
the numbers be computed.

## Changing copies

Often you don't want to change a document, but to make a changed copy of
part of it. The `copy ... modify ... return` expression copies nodes,
applies updates to the copies only, and returns them. It is not an
updating expression, so you can use it anywhere, in any query. What
would req-8 look like as a MUST, without its note?

```console
$ sxml xquery --context-item=latch.xml --expression='
    copy $requirement := //t[@anchor = "req-8"]
    modify (
      delete node $requirement/cref,
      replace value of node $requirement/bcp14 with "MUST"
    )
    return $requirement'
<t anchor="req-8">A lockout <bcp14>MUST</bcp14> last five minutes.
      </t>
```

The copy has the whitespace text node that stood before the note, as
the draft did after `give-anchor.xq`.

XQuery 4.0 has a shorter form for the common case, `transform with`.
The paragraphs with editorial notes, as a reader of the published draft
will see them:

```console
$ sxml xquery --context-item=latch.xml --expression='//t[cref] ! (. transform with { delete node cref })'
<t anchor="req-8">A lockout <bcp14>SHOULD</bcp14> last five minutes.
      </t><t>A code travels in clear text on the local network. Deployments
      <bcp14>SHOULD</bcp14> carry Latch over an encrypted link.
      </t>
```

## Updating functions

A function whose body is an updating expression must say so, with
`declare updating function`. It returns no value; calling it adds its
changes to the pending update list:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare updating function local:set-keyword($requirement as element(t), $keyword as xs:string) {
      replace value of node $requirement/bcp14 with $keyword
    };
    local:set-keyword(//t[@anchor = "req-3"], "MUST")' > after.xml
$ diff before.xml after.xml
62c62
<       <t anchor="req-3">An unlocked lock <bcp14>SHOULD</bcp14> return to the
---
>       <t anchor="req-3">An unlocked lock <bcp14>MUST</bcp14> return to the
```

A module of such functions is a toolkit of edits, which is what the
project at the end of this chapter builds.

## Mistakes the processor catches

Updating and ordinary expressions don't mix: a query either makes
changes or computes a value, and the processor checks which before it
runs anything. An updating expression in a function that isn't declared
updating is a static error:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare function local:publish() { delete node //cref };
    local:publish()'
expression:2:38: err:XUST0001: an updating expression is used where only a simple expression is allowed (XQuery Update Facility 3.0, 2.2.2)
```

The target of `insert ... into`, `replace` and `rename` must be exactly
one node. The Lockout section has three paragraphs, so this fails when
it runs:

```console
$ sxml xquery --context-item=latch.xml --expression='insert node <cref>Check this.</cref> into //section[@anchor = "lockout"]/t'
expression:1:1: err:XUTY0005: the target of insert into is not one element or document node (5.4.1)
```

Write `for $paragraph in ... return insert node ... into $paragraph` to
change each one. And two changes to the same node that can't both
happen are found when the changes are applied. Two authors' edits in one
query, each renaming the same term:

```console
$ sxml xquery --context-item=latch.xml --expression='rename node //dt[@anchor = "term-code"] as "term", rename node //dt[@anchor = "term-code"] as "dfn"'
expression: err:XUDY0015: a node is renamed twice (3.2.2)
$ sxml xquery --context-item=latch.xml --expression='replace value of node //dt[@anchor = "term-code"] with "PIN", replace value of node //dt[@anchor = "term-code"] with "Passcode"'
expression: err:XUDY0017: the value of a node is replaced twice (3.2.2)
```

Nothing is written when that happens: the pending update list is
applied whole, or not at all.

## Storing documents

`fn:put` stores a node as a document at a URI. sxml never writes a file
you didn't ask for, so with the `sxml` command it needs `--allow-put`.
`put` is part of the pending update list, and stores the document as the
other updates of the query left it. This query starts draft -02 in the
`drafts` directory:

```console
$ sxml xquery --allow-put --context-item=latch.xml --expression='
    replace value of node /rfc/@docName with "draft-example-latch-protocol-02",
    replace value of node //seriesInfo[@name = "Internet-Draft"]/@value with "draft-example-latch-protocol-02",
    put(., "drafts/latch-02.xml")' > /dev/null
$ sxml xquery --method=adaptive --expression='collection("drafts") ! string(rfc/@docName)'
"draft-example-latch-protocol-00"
"draft-example-latch-protocol-01"
"draft-example-latch-protocol-02"
```

The files are written only when the whole query succeeds, and a file
that exists is replaced only with `--force`. Without `--allow-put`,
`fn:put` raises an error:

```console
$ sxml xquery --context-item=latch.xml --expression='put(., "drafts/latch-02.xml")'
expression: err:FOUP0002: fn:put has no location to store at: the host's update sink takes no documents (XQuery Update Facility 3.0, 6.1)
```

A program that embeds sxml receives the stored documents and the updated
trees through callbacks, and decides itself what to do with them
([Embedding sxml in C](c.md)).
