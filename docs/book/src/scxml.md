# Project: Self-Describing State Machines

<!-- examples: scxml -->

A feature flag is a small state machine: it is stable, its guardrail
metrics are checked before a rollout, it rolls out a new percentage
for one step, and it refuses every rollout for a while after too many
failed checks. The Vane draft describes that machine in prose. This project
writes it as a machine that a program can run, and then makes the
machine describe itself: what each event carries, what type each piece
of data has, what each state means in words, and which requirement of
the draft each part implements. With those annotations, the tools of
the earlier chapters can check the machine against its schemas and
against the draft, write its documentation, and draw it.

The machine is written in SCXML, the W3C's State Chart XML. Its files
are in the `scxml` directory:

- `flag.scxml`, the machine;
- `scxml.xsd`, a schema for the part of SCXML the machine uses, and
  `vane-data.xsd`, a schema for its annotations, both read in
  [Validating with XSD](xsd.md);
- `vane.xml`, a link to the draft in the `spec` directory.

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

## The flag

`flag.scxml`:

```xml
{{#include ../examples/scxml/flag.scxml}}
```

Read the states from the top. `stable` waits for a rollout request,
and answers a status request without leaving. `checking` decides: a
passed check starts the rollout; a failed check returns to `stable`
while there are failures left, and enters `paused` when there are
none. Both transitions for `check.failed` wait for the same event, and
the first whose condition holds is taken. `rolling-out` sends itself a
`step.done` when it is entered, and returns to `stable` on that or on
a kill request. `paused` sends itself `pause.expired` and then goes
back to `stable`. `archived` is for a flag that is no longer used.

A query lists the transitions as a table:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    declare default element namespace "http://www.w3.org/2005/07/scxml";
    for $transition in //transition
    return `{$transition/../@id} --{$transition/@event}--> {($transition/@target, "(stays)")[1]}`'
"stable --rollout.request--> checking"
"stable --status.request--> (stays)"
"checking --check.passed--> rolling-out"
"checking --check.failed--> stable"
"checking --check.failed--> paused"
"rolling-out --kill.request step.done--> stable"
"rolling-out --flag.evaluated--> (stays)"
"paused --pause.expired--> stable"
"archived --kill.request--> stable"
```

## The annotations

Everything in the namespace `urn:example:vane` is an annotation. An
SCXML interpreter ignores elements and attributes of other namespaces,
so they don't change how the machine runs; they describe it:

- `vane:events` declares every event the machine knows, with an XHTML
  paragraph that says what it means, and, for events that carry data,
  `vane:payload`, the name of an element of `vane-data.xsd` that
  describes the data. A rollout request carries `vane:rollout`, a
  flag key and a percentage; a status response carries `vane:status`.
- `vane:type` on each `data` item names a simple type of
  `vane-data.xsd`: `maxFailures` is a `vane:failure-limit`, a
  positive integer up to 10.
- `vane:implements` on states and transitions lists the anchors of the
  draft's requirements that they carry out. The paused state
  implements `req-5`, a paused flag isn't rolled out, and `req-8`,
  the pause lasts five minutes.
- An XHTML `div` in each state says in words what the state is.

The events with their payloads and descriptions:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    declare namespace vane = "urn:example:vane";
    //vane:event ! `{@name}{@vane:payload ! (" carries " || .)}: {normalize-space()}`'
"rollout.request carries vane:rollout: An owner asks for a new percentage."
"check.passed: The guardrail metrics are within their limits."
"check.failed: A guardrail metric is over its limit."
"kill.request: An owner pulls the kill switch."
"status.request: An SDK asks for the state."
"status.response carries vane:status: The service answers with the state."
"step.done: Sent by the machine to itself when a rollout step has run."
"pause.expired: Sent by the machine to itself when a pause ends."
"flag.archived: An owner archived the flag."
```

And the requirements the machine names, with their key words, looked
up in the draft:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    declare namespace vane = "urn:example:vane";
    for $anchor in distinct-values(//@vane:implements ! tokenize(.))
    let $requirement := doc("vane.xml")//t[@anchor = $anchor]
    return `{$anchor}: {$requirement/bcp14}`'
"req-2: MUST"
"req-6: MUST"
"req-4: MUST"
"req-7: MUST"
"req-3: SHOULD"
"req-5: MUST NOT"
"req-8: SHOULD"
```

The numbers of the machine are the draft's numbers: three failed
checks (`req-7`), ten minutes, 600 seconds, for a rollout step
(`req-3`), and five minutes, 300 seconds, of pause (`req-8`).

## Validating it

`scxml.xsd` imports `vane-data.xsd`, so one schema checks both the
machine and its annotations:

```console
$ sxml validate --schema=scxml.xsd flag.scxml; echo "exit status $?"
exit status 0
```

The payload elements have declarations of their own, so the data a
machine sends or receives can be validated with `vane-data.xsd` alone:

```console
$ echo '<rollout xmlns="urn:example:vane"><flag>new-checkout</flag><percentage>1000</percentage></rollout>' \
  | sxml validate --schema=vane-data.xsd -
-:1:60: cvc-pattern-valid: element 'percentage' has the value '1000', which is not valid for type 'percentage': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[0-9]{1,3}'
```

The percentage has four digits, and a percentage of the protocol is
one to three digits, as the draft's ABNF says. The pattern says no
more than the ABNF does, so it lets 999 through, although the draft's
terminology says a percentage is at most 100; a `maxInclusive` facet
on an integer type would say that, and the ABNF could not.

The machine is valid, and yet it has four flaws that no schema finds:
a state that nothing leads to, an event that is handled but never
declared, an event that is declared but never used, and a requirement
of the draft that it doesn't implement. The rest of the project:

- [Checking What the Schema Cannot](scxml-checks.md) finds them with
  a schema-aware query, and fixes them with an update;
- [Documenting the Machine in XHTML](scxml-xhtml.md) writes a page
  about the machine from its annotations and its schemas;
- [Drawing the Machine](scxml-drawing.md) draws it as a diagram.
