# Project: Self-Describing State Machines

<!-- examples: scxml -->

A door lock is a small state machine: it is locked, it checks a code,
it is unlocked, it refuses every code for a while after too many wrong
ones. The Latch draft describes that machine in prose. This project
writes it as a machine that a program can run, and then makes the
machine describe itself: what each event carries, what type each piece
of data has, what each state means in words, and which requirement of
the draft each part implements. With those annotations, the tools of
the earlier chapters can check the machine against its schemas and
against the draft, write its documentation, and draw it.

The machine is written in SCXML, the W3C's State Chart XML. Its files
are in the `scxml` directory:

- `lock.scxml`, the machine;
- `scxml.xsd`, a schema for the part of SCXML the machine uses, and
  `latch-data.xsd`, a schema for its annotations, both read in
  [Validating with XSD](xsd.md);
- `latch.xml`, a link to the draft in the `spec` directory.

## SCXML in one page

An SCXML document is a `scxml` element in the namespace
`http://www.w3.org/2005/07/scxml`. Its children are:

- `state` elements, each with an `id`. A machine is in one of its
  states at a time (states can also nest, and run in parallel, which
  this machine doesn't need); the `initial` attribute of `scxml` names
  the first.
- In each state, `transition` elements. A transition waits for an
  *event*, named in `event`; when the event comes and the condition in
  `cond` holds, the machine leaves the state for the one named in
  `target`. A transition without a target handles the event and stays.
- *Executable content*, run on a transition and when a state is
  entered (`onentry`) or left (`onexit`): `assign` changes data, `log`
  writes a message, `raise` makes an event inside the machine, and
  `send` sends one, to the machine itself after a delay or to the world
  outside.
- A `datamodel` of `data` items, the machine's variables. The
  `datamodel` attribute says which language the expressions in `cond`,
  `expr` and `location` are written in; here ECMAScript.

An SCXML interpreter runs such a document. sxml doesn't run machines;
it reads, checks and transforms them, which is what this project needs.

## The lock

`lock.scxml`:

```xml
{{#include ../examples/scxml/lock.scxml}}
```

Read the states from the top. `locked` waits for an unlock request,
and answers a status request without leaving. `checking` decides:
an accepted code unlocks; a rejected code locks again while there are
attempts left, and enters `lockout` when there are none. Both
transitions for `code.rejected` wait for the same event, and the first
whose condition holds is taken. `unlocked` sends itself a
`timeout.relock` when it is entered, and locks again on that or on a
lock request. `lockout` sends itself `lockout.expired` and then goes
back to `locked`. `jammed` is for a stuck bolt.

A query lists the transitions as a table:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    declare default element namespace "http://www.w3.org/2005/07/scxml";
    for $transition in //transition
    return `{$transition/../@id} --{$transition/@event}--> {($transition/@target, "(stays)")[1]}`'
"locked --unlock.request--> checking"
"locked --status.request--> (stays)"
"checking --code.accepted--> unlocked"
"checking --code.rejected--> locked"
"checking --code.rejected--> lockout"
"unlocked --lock.request timeout.relock--> locked"
"unlocked --door.opened--> (stays)"
"lockout --lockout.expired--> locked"
"jammed --lock.request--> locked"
```

## The annotations

Everything in the namespace `urn:example:latch` is an annotation. An
SCXML interpreter ignores elements and attributes of other namespaces,
so they don't change how the machine runs; they describe it:

- `latch:events` declares every event the machine knows, with an XHTML
  paragraph that says what it means, and, for events that carry data,
  `latch:payload`, the name of an element of `latch-data.xsd` that
  describes the data. An unlock request carries `latch:credentials`, a
  door and a code; a status response carries `latch:status`.
- `latch:type` on each `data` item names a simple type of
  `latch-data.xsd`: `maxAttempts` is a `latch:attempt-limit`, a
  positive integer up to 10.
- `latch:implements` on states and transitions lists the anchors of the
  draft's requirements that they carry out. The lockout state
  implements `req-5`, a locked-out lock doesn't unlock, and `req-8`,
  the lockout lasts five minutes.
- An XHTML `div` in each state says in words what the state is.

The events with their payloads and descriptions:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    declare namespace latch = "urn:example:latch";
    //latch:event ! `{@name}{@latch:payload ! (" carries " || .)}: {normalize-space()}`'
"unlock.request carries latch:credentials: A person presents a code at the door."
"code.accepted: The code matches one of the stored codes."
"code.rejected: The code matches none of them."
"lock.request: The controller asks the lock to lock."
"status.request: The controller asks for the state."
"status.response carries latch:status: The lock answers with its state."
"timeout.relock: Sent by the lock to itself after an unlock."
"lockout.expired: Sent by the lock to itself when a lockout ends."
"bolt.jammed: The motor could not move the bolt."
```

And the requirements the machine names, with their key words, looked
up in the draft:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    declare namespace latch = "urn:example:latch";
    for $anchor in distinct-values(//@latch:implements ! tokenize(.))
    let $requirement := doc("latch.xml")//t[@anchor = $anchor]
    return `{$anchor}: {$requirement/bcp14}`'
"req-2: MUST"
"req-6: MUST"
"req-4: MUST"
"req-7: MUST"
"req-3: SHOULD"
"req-5: MUST NOT"
"req-8: SHOULD"
```

The numbers of the machine are the draft's numbers: three attempts
(`req-7`), ten seconds before relocking (`req-3`), and five minutes,
300 seconds, of lockout (`req-8`).

## Validating it

`scxml.xsd` imports `latch-data.xsd`, so one schema checks both the
machine and its annotations:

```console
$ sxml validate --schema=scxml.xsd lock.scxml; echo "exit status $?"
exit status 0
```

The payload elements have declarations of their own, so the data a
machine sends or receives can be validated with `latch-data.xsd` alone:

```console
$ echo '<credentials xmlns="urn:example:latch"><door>front</door><code>12a4</code></credentials>' \
  | sxml validate --schema=latch-data.xsd -
-:1:58: cvc-pattern-valid: element 'code' has the value '12a4', which is not valid for type 'code': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[0-9]{4,8}'
```

The code has a letter in it, and a code of the protocol is four to
eight digits, as the draft's ABNF says.

The machine is valid, and yet it has four flaws that no schema finds:
a state that nothing leads to, an event that is handled but never
declared, an event that is declared but never used, and a requirement
of the draft that it doesn't implement. The rest of the project:

- [Checking What the Schema Cannot](scxml-checks.md) finds them with
  a schema-aware query, and fixes them with an update;
- [Documenting the Machine in XHTML](scxml-xhtml.md) writes a page
  about the machine from its annotations and its schemas;
- [Drawing the Machine](scxml-drawing.md) draws it as a diagram.
