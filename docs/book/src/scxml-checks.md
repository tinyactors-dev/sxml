# Checking What the Schema Cannot

<!-- examples: scxml -->

A schema checks each document against rules about its structure and
values. Some rules of a good machine are about its behaviour, or about
another document: every state can be reached, every event the machine
handles is declared, every requirement the machine names exists in the
draft. A query can check them, and a schema-aware query can rely on the
types the schemas give: the `vane:implements` lists are lists, the
`vane:type` names are names, and the payload names point into
`vane-data.xsd`.

## The checks

`check.xq` takes the machine as its context item, reads the draft from
`vane.xml`, and prints one line for each problem it finds:

`check.xq`:

```xquery
{{#include ../examples/scxml/check.xq}}
```

The machine has four flaws:

```console
$ sxml xquery --context-item=flag.scxml check.xq
unreachable state: archived
undeclared event: flag.evaluated
unused event: flag.archived
not implemented: req-1 (MUST)
```

The output declarations make the result text, one line per problem, so
a machine without problems prints nothing.

## Reachable states

A state is reachable when some path of transitions leads to it from
the initial state. `local:reachable` computes the set the way you would
by hand: start with the initial state, add the targets of the
transitions of every state reached so far, and repeat. Each pass adds
at least one state or adds nothing, so as many passes as there are
states are enough; `fold-left` over `1 to count($states)` runs them,
and `distinct-values` keeps the set a set. The machine has cycles
(`stable` to `checking` and back), which is why the passes collect a
set instead of following paths.

No transition targets `archived`, so the flag can never get there:
the state is dead code.

## Events

Three sets of event names, and two comparisons:

- `$declared`, the names of `vane:events`;
- `$handled`, the events transitions wait for. `data(@event)` is a list
  of tokens, so the transition that waits for `kill.request
  step.done` adds two names;
- `$sent`, the events of `send` and `raise`.

`flag.evaluated` is handled, in `rolling-out`, but not declared:
nobody wrote down what it means, or who sends it. `flag.archived` is
declared but nothing handles it, sends it or raises it.

`status.response` is declared and sent, but no transition waits for it:
the service sends it to the SDK that asked. A check that compared only
declared and handled events would report it as unused, which it is
not; counting `send` as a use is what tells an outgoing event from a
forgotten one.

## Names that point elsewhere

`vane:payload` and `vane:type` name components of `vane-data.xsd`,
and the schema checks only that they are names. The query checks that
they name something:

- a payload must be a global element of `vane-data.xsd`. The query
  reads the schema as an XML document, with `doc`, and makes the names
  of its `xs:element`s into QNames in its target namespace, which
  compare with the typed payload names;
- a type must be a simple type of the imported schema. Every such type
  has a constructor function, so `function-lookup($type, 1)` is empty
  exactly when there is no such type.

For a type that exists, the query checks the data item's value against
it, as [Typed Data and Schema-Aware Queries](xsd-typed-data.md) did:
it makes a `vane:value` element with `xsi:type` naming the type (in
whatever namespace the type has, with a computed namespace binding) and
validates it. `validate` raises `err:XQDY0027` when the value is not
one of the type; `try` turns that into a line of the report, and the
check goes on.

## Requirements

`vane:implements` holds anchors of the draft. Two checks join the
machine with the draft: every anchor the machine names must be the
anchor of a paragraph of the draft, and every MUST or MUST NOT of the
draft should be implemented somewhere in the machine. `req-1`, a flag
is in exactly one of its states, is a property of the whole machine,
and nothing says so yet.

A copy of the machine with a mistake for each of the other checks:

```console
$ sed -e 's/expr="3"/expr="30"/' \
      -e 's/vane:type="vane:seconds"/vane:type="seconds"/' \
      -e 's/vane:payload="vane:rollout"/vane:payload="vane:rolout"/' \
      -e 's/req-5 req-8/req-5 req-12/' flag.scxml > worse.scxml
$ sxml validate --schema=scxml.xsd worse.scxml; echo "exit status $?"
exit status 0
$ sxml xquery --context-item=worse.scxml check.xq
unreachable state: archived
undeclared event: flag.evaluated
unused event: flag.archived
unknown payload: vane:rolout of rollout.request
bad value: maxFailures = 30 is not a vane:failure-limit
unknown type: seconds of stepSeconds
unknown type: seconds of pauseSeconds
unknown requirement: req-12
not implemented: req-1 (MUST)
```

The schema finds none of them. The type without its prefix is the
name `seconds` in SCXML's namespace, the default namespace of the
machine, and SCXML has no such type.

## Fixing the machine with an update

The fixes are small, and each touches one place: say that the machine
implements `req-1`, declare `flag.evaluated`, and let an owner archive
a stable or paused flag, which makes the `archived` state reachable and
the `flag.archived` event used. An update makes them without retyping the machine:

`fix.xq`:

```xquery
{{#include ../examples/scxml/fix.xq}}
```

`transform with` copies the context item, the machine's document node,
applies the updates to the copy and returns it ([Updating
Documents](xquery-updates.md) explains updates). The result is the
fixed machine, `flag-fixed.scxml`:

```console
$ sxml xquery --context-item=flag.scxml --output=flag-fixed.scxml --force fix.xq
$ diff flag.scxml flag-fixed.scxml
1,4c1
< <?xml version="1.0" encoding="UTF-8"?>
< <scxml xmlns="http://www.w3.org/2005/07/scxml"
<        xmlns:vane="urn:example:vane"
<        version="1.0" name="feature_flag" initial="stable" datamodel="ecmascript">
---
> <scxml xmlns="http://www.w3.org/2005/07/scxml" xmlns:vane="urn:example:vane" version="1.0" name="feature_flag" initial="stable" datamodel="ecmascript" vane:implements="req-1">
27c24
<     </vane:event>
---
>     </vane:event><vane:event name="flag.evaluated"><p xmlns="http://www.w3.org/1999/xhtml">An SDK reports that it evaluated the flag.</p></vane:event>
43c40
<   <state id="stable" vane:implements="req-2">
---
>   <state id="stable" vane:implements="req-2"><transition event="flag.archived" target="archived"/>
80c77
<   <state id="paused" vane:implements="req-5 req-8">
---
>   <state id="paused" vane:implements="req-5 req-8"><transition event="flag.archived" target="archived"/>
```

Everything else is as it was, down to the whitespace between the
untouched elements. The serializer writes the start tag of `scxml` on
one line, and the new nodes without line breaks, as the query made
them; the XML declaration is left out. The fixed machine is valid, and
the checks find nothing:

```console
$ sxml validate --schema=scxml.xsd flag-fixed.scxml; echo "exit status $?"
exit status 0
$ sxml xquery --context-item=flag-fixed.scxml check.xq
```

In a build, a script turns "prints nothing" into an exit status:

```console
$ problems=$(sxml xquery --context-item=flag.scxml check.xq); test -z "$problems"; echo "exit status $?"
exit status 1
$ problems=$(sxml xquery --context-item=flag-fixed.scxml check.xq); test -z "$problems"; echo "exit status $?"
exit status 0
```

The rest of the project uses the fixed machine.
