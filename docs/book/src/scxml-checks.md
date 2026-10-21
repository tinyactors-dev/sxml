# Checking What the Schema Cannot

<!-- examples: scxml -->

A schema checks each document against rules about its structure and
values. Some rules of a good machine are about its behaviour, or about
another document: every state can be reached, every event the machine
handles is declared, every requirement the machine names exists in the
draft. A query can check them, and a schema-aware query can rely on the
types the schemas give: the `latch:implements` lists are lists, the
`latch:type` names are names, and the payload names point into
`latch-data.xsd`.

## The checks

`check.xq` takes the machine as its context item, reads the draft from
`latch.xml`, and prints one line for each problem it finds:

`check.xq`:

```xquery
{{#include ../examples/scxml/check.xq}}
```

The machine has four flaws:

```console
$ sxml xquery --context-item=lock.scxml check.xq
unreachable state: jammed
undeclared event: door.opened
unused event: bolt.jammed
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
(`locked` to `checking` and back), which is why the passes collect a
set instead of following paths.

No transition targets `jammed`, so the lock can never get there: the
state is dead code.

## Events

Three sets of event names, and two comparisons:

- `$declared`, the names of `latch:events`;
- `$handled`, the events transitions wait for. `data(@event)` is a list
  of tokens, so the transition that waits for `lock.request
  timeout.relock` adds two names;
- `$sent`, the events of `send` and `raise`.

`door.opened` is handled, in `unlocked`, but not declared: nobody wrote
down what it means, or who sends it. `bolt.jammed` is declared but
nothing handles it, sends it or raises it.

`status.response` is declared and sent, but no transition waits for it:
the lock sends it to the controller. A check that compared only
declared and handled events would report it as unused, which it is
not; counting `send` as a use is what tells an outgoing event from a
forgotten one.

## Names that point elsewhere

`latch:payload` and `latch:type` name components of `latch-data.xsd`,
and the schema checks only that they are names. The query checks that
they name something:

- a payload must be a global element of `latch-data.xsd`. The query
  reads the schema as an XML document, with `doc`, and makes the names
  of its `xs:element`s into QNames in its target namespace, which
  compare with the typed payload names;
- a type must be a simple type of the imported schema. Every such type
  has a constructor function, so `function-lookup($type, 1)` is empty
  exactly when there is no such type.

For a type that exists, the query checks the data item's value against
it, as [Typed Data and Schema-Aware Queries](xsd-typed-data.md) did:
it makes a `latch:value` element with `xsi:type` naming the type (in
whatever namespace the type has, with a computed namespace binding) and
validates it. `validate` raises `err:XQDY0027` when the value is not
one of the type; `try` turns that into a line of the report, and the
check goes on.

## Requirements

`latch:implements` holds anchors of the draft. Two checks join the
machine with the draft: every anchor the machine names must be the
anchor of a paragraph of the draft, and every MUST or MUST NOT of the
draft should be implemented somewhere in the machine. `req-1`, a lock
is in exactly one of its states, is a property of the whole machine,
and nothing says so yet.

A copy of the machine with a mistake for each of the other checks:

```console
$ sed -e 's/expr="3"/expr="30"/' \
      -e 's/latch:type="latch:seconds"/latch:type="seconds"/' \
      -e 's/latch:payload="latch:credentials"/latch:payload="latch:credential"/' \
      -e 's/req-5 req-8/req-5 req-12/' lock.scxml > worse.scxml
$ sxml validate --schema=scxml.xsd worse.scxml; echo "exit status $?"
exit status 0
$ sxml xquery --context-item=worse.scxml check.xq
unreachable state: jammed
undeclared event: door.opened
unused event: bolt.jammed
unknown payload: latch:credential of unlock.request
bad value: maxAttempts = 30 is not a latch:attempt-limit
unknown type: seconds of relockSeconds
unknown type: seconds of lockoutSeconds
unknown requirement: req-12
not implemented: req-1 (MUST)
```

The schema finds none of them. The type without its prefix is the
name `seconds` in SCXML's namespace, the default namespace of the
machine, and SCXML has no such type.

## Fixing the machine with an update

The fixes are small, and each touches one place: say that the machine
implements `req-1`, declare `door.opened`, and let a jammed bolt lead
to the `jammed` state, which makes the state reachable and the event
used. An update makes them without retyping the machine:

`fix.xq`:

```xquery
{{#include ../examples/scxml/fix.xq}}
```

`transform with` copies the context item, the machine's document node,
applies the updates to the copy and returns it ([Updating
Documents](xquery-updates.md) explains updates). The result is the
fixed machine, `lock-fixed.scxml`:

```console
$ sxml xquery --context-item=lock.scxml --output=lock-fixed.scxml --force fix.xq
$ diff lock.scxml lock-fixed.scxml
1,4c1
< <?xml version="1.0" encoding="UTF-8"?>
< <scxml xmlns="http://www.w3.org/2005/07/scxml"
<        xmlns:latch="urn:example:latch"
<        version="1.0" name="door_lock" initial="locked" datamodel="ecmascript">
---
> <scxml xmlns="http://www.w3.org/2005/07/scxml" xmlns:latch="urn:example:latch" version="1.0" name="door_lock" initial="locked" datamodel="ecmascript" latch:implements="req-1">
27c24
<     </latch:event>
---
>     </latch:event><latch:event name="door.opened"><p xmlns="http://www.w3.org/1999/xhtml">The door sensor reports that the door opened.</p></latch:event>
43c40
<   <state id="locked" latch:implements="req-2">
---
>   <state id="locked" latch:implements="req-2"><transition event="bolt.jammed" target="jammed"/>
66c63
<   <state id="unlocked" latch:implements="req-3">
---
>   <state id="unlocked" latch:implements="req-3"><transition event="bolt.jammed" target="jammed"/>
```

Everything else is as it was, down to the whitespace between the
untouched elements. The serializer writes the start tag of `scxml` on
one line, and the new nodes without line breaks, as the query made
them; the XML declaration is left out. The fixed machine is valid, and
the checks find nothing:

```console
$ sxml validate --schema=scxml.xsd lock-fixed.scxml; echo "exit status $?"
exit status 0
$ sxml xquery --context-item=lock-fixed.scxml check.xq
```

In a build, a script turns "prints nothing" into an exit status:

```console
$ problems=$(sxml xquery --context-item=lock.scxml check.xq); test -z "$problems"; echo "exit status $?"
exit status 1
$ problems=$(sxml xquery --context-item=lock-fixed.scxml check.xq); test -z "$problems"; echo "exit status $?"
exit status 0
```

The rest of the project uses the fixed machine.
