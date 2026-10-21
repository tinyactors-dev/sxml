# Keys and References

<!-- examples: scxml -->

Types check each value on its own. Some rules are about values
together: no two states have the same id, every transition goes to a
state that exists, no event is declared twice. XSD's *identity
constraints* express them: `xs:unique`, `xs:key` and `xs:keyref`.

## The machine's keys

The end of `scxml.xsd` declares three constraints on the `scxml`
element:

```xml
{{#include ../examples/scxml/scxml.xsd:143:157}}
```

and `latch-data.xsd` declares one on the event declarations:

```xml
{{#include ../examples/scxml/latch-data.xsd:93:112}}
```

An identity constraint has three parts:

- where it is declared: on the element whose content it covers. The
  rule holds *within* each such element. The state ids are unique in
  the whole machine, because the key is declared on `scxml`; the event
  names are unique within each `latch:events`.
- a `selector`, a path from that element to the elements the rule is
  about: every `state` and `final` at any depth, every `transition`,
  or each `latch:event`;
- one or more `field`s, paths from each selected element to the values
  that make its *key sequence*, here `@id`, `@target`, `@initial` or
  `@name`.

The three kinds differ in what they require:

- `xs:unique`: no two selected elements have the same key sequence.
  Elements that lack a field are left out.
- `xs:key`: the same, and every selected element must have every
  field. Every state has an id.
- `xs:keyref`: the key sequence of each selected element must be one of
  a key's (or a unique's) sequences. That's a foreign key, as in a
  database: a target is a state id. A transition without a target, as
  `status.request` in the `locked` state, is left out; so are the
  states without an `initial`.

## Checking a machine

This small machine has a mistake for each of the machine's rules:

`bad-target.scxml`:

```xml
{{#include ../examples/scxml/bad-target.scxml}}
```

```console
$ sxml validate --schema=scxml.xsd bad-target.scxml
bad-target.scxml:11:3: cvc-identity-constraint.4.2.2: element 'Q{http://www.w3.org/2005/07/scxml}state': its key sequence is that of an earlier member (key 'Q{http://www.w3.org/2005/07/scxml}state-id')
bad-target.scxml:6:5: cvc-identity-constraint.4.3: element 'Q{http://www.w3.org/2005/07/scxml}transition': its key sequence is no key sequence of the referenced key (keyref 'Q{http://www.w3.org/2005/07/scxml}transition-target')
bad-target.scxml:9:5: cvc-identity-constraint.4.3: element 'Q{http://www.w3.org/2005/07/scxml}transition': its key sequence is no key sequence of the referenced key (keyref 'Q{http://www.w3.org/2005/07/scxml}transition-target')
bad-target.scxml:3:1: cvc-identity-constraint.4.3: element 'Q{http://www.w3.org/2005/07/scxml}scxml': its key sequence is no key sequence of the referenced key (keyref 'Q{http://www.w3.org/2005/07/scxml}initial-state')
```

- The third state has the id of the first, `locked`.
- The first transition goes to `unlock`, which is no state's id.
- The second goes to `locked`, which *is* an id, but twice: a keyref
  can't refer to a key value that is not unique, so it is reported
  too. Fix the duplicate first, and this one goes away.
- The machine's `initial` names `closed`, which doesn't exist. The
  nested `initial="bolted"` is fine.

The elements and the constraints are written as expanded names,
`Q{namespace}local`, because the document writes them without a
prefix; an element with a prefix, such as `latch:event` below, is
written with it.

Each message names the constraint, so you can find it in the schema.
They are reported in groups, by constraint, at the end of the element
that the constraint is declared on: only then are all the values known.

The event declarations have their own rule:

`duplicate-event.scxml`:

```xml
{{#include ../examples/scxml/duplicate-event.scxml}}
```

```console
$ sxml validate --schema=scxml.xsd duplicate-event.scxml
duplicate-event.scxml:9:5: cvc-identity-constraint.4.1: element 'latch:event': its key sequence is that of an earlier member (unique 'Q{urn:example:latch}event-name')
```

Values are compared as typed values, not as text. An event name is an
`xs:token`, whose spaces collapse, so `name=" lock.request"` would be a
duplicate too; and when a field's type is `xs:integer`, `7` and `007`
are the same key.

## Paths in constraints

The paths of `selector` and `field` are a small subset of XPath: child
steps, `.//` at the start, `.` for the element itself, `@` for
attributes, `*`, and `|` to combine paths. There are no predicates and
no functions. `. | .//sc:state` in `initial-state` selects the machine
and every state, the elements that may have an `initial`.

Names with a namespace need a prefix that the schema binds, as
`scxml.xsd` binds `sc`. Without it, `.//transition` means transitions
in no namespace, and in a machine, where every transition is in
SCXML's namespace, the keyref silently selects nothing. Only the two
other mistakes are left:

```console
$ sed 's|xpath=".//sc:transition"|xpath=".//transition"|' scxml.xsd > loose.xsd
$ sxml validate --schema=loose.xsd bad-target.scxml
bad-target.scxml:11:3: cvc-identity-constraint.4.2.2: element 'Q{http://www.w3.org/2005/07/scxml}state': its key sequence is that of an earlier member (key 'Q{http://www.w3.org/2005/07/scxml}state-id')
bad-target.scxml:3:1: cvc-identity-constraint.4.3: element 'Q{http://www.w3.org/2005/07/scxml}scxml': its key sequence is no key sequence of the referenced key (keyref 'Q{http://www.w3.org/2005/07/scxml}initial-state')
```

In XSD 1.1, `xpathDefaultNamespace="##targetNamespace"` on `xs:schema`
(or on a `selector` or `field`) makes unprefixed names in the paths
mean the target namespace, which avoids the mistake.

The value of `refer` is a QName too: the keyrefs say
`refer="sc:state-id"`, because the key is a component of the target
namespace like any other.

## Keys or IDs?

`xs:ID` and `xs:IDREF` are simple types with a similar purpose: an `ID`
is unique in the whole document, and an `IDREF` must be one of the
document's IDs. They are older and simpler, but blunt: one namespace of
ids for the whole document, only names (no numbers or dates), and only
one field. The Recommendation's own schema for SCXML gives state ids
the type `xs:ID`. Use keys when the rule belongs to a part of the
document, or when the key is a number, a date or several values.

Choosing between `xs:key` and `xs:unique` is choosing whether the field
is required. The draft's schema, `latch.xsd`, makes its anchors unique
with an `xs:unique`, because most paragraphs have no anchor: only
requirements do. With an `xs:key` in its place, every selected element
must have one, and the draft has six paragraphs without (`latch.xml`
here is a link to the draft in the `spec` directory, and `latch.xsd` to
its schema):

```console
$ sed 's/<xs:unique name="anchor">/<xs:key name="anchor">/; s|</xs:unique>|</xs:key>|' latch.xsd \
  | sxml validate --schema=- latch.xml 2>&1 | head -3
latch.xml:25:7: cvc-identity-constraint.4.2.1: element 't': a key field has no value (key 'anchor')
latch.xml:34:7: cvc-identity-constraint.4.2.1: element 't': a key field has no value (key 'anchor')
latch.xml:39:9: cvc-identity-constraint.4.2.1: element 't': a key field has no value (key 'anchor')
```

A keyref may refer to a unique as well as to a key, which is what the
draft's `xref-target` does.
