# XSD 1.1 Features

<!-- examples: scxml -->

XSD 1.1 keeps everything of XSD 1.0 and adds what users had missed
most: rules written in XPath, types chosen by attribute values, content
models that are open to extensions, and a clean way to change a schema
without copying it. sxml implements XSD 1.1, and validates XSD 1.0
schemas with the same engine. The machine's schemas already use some of
it, and `latch-1.1.xsd`, a stricter version of the annotations' schema,
uses the rest:

`latch-1.1.xsd`:

```xml
{{#include ../examples/scxml/latch-1.1.xsd}}
```

It is used together with `scxml.xsd`. The machine is valid by it too:

```console
$ sxml validate --schema=latch-1.1.xsd --schema=scxml.xsd lock.scxml; echo "exit status $?"
exit status 0
```

## Assertions on values

The `assertion` facet restricts a simple type by an XPath condition, in
which `$value` is the value being checked. SCXML writes the delay of a
`send` as a number and a unit, `10s` or `250ms`. A pattern can check the
form, and an assertion the size: `delay` allows at most an hour.

The second new facet, `explicitTimezone`, says whether dates and times
must have a timezone (`required`), must not have one (`prohibited`), or
may (`optional`, the default). A moment in a log is only one when its
timezone is known.

`latch-data.xsd` declares an element `latch:value` whose type is
`xs:anySimpleType`, any simple value at all, so that a value can be
checked against any of the schema's types with `xsi:type`:

```console
$ for delay in 10s 250ms 2h 4000s; do \
    echo "<latch:value xmlns:latch='urn:example:latch' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='latch:delay'>$delay</latch:value>" \
      | sxml validate --schema=latch-1.1.xsd -; \
  done
-:1:1: cvc-pattern-valid: element 'latch:value' has the value '2h', which is not valid for type 'latch:delay': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[0-9]+(s|ms)'
-:1:1: cvc-assertions-valid: the value '4000s' breaks an assertion of type 'latch:delay'
$ echo "<latch:value xmlns:latch='urn:example:latch' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='latch:moment'>2026-10-21T09:15:00</latch:value>" \
  | sxml validate --schema=latch-1.1.xsd -
-:1:1: cvc-explicitTimezone-valid: element 'latch:value' has the value '2026-10-21T09:15:00', which is not valid for type 'latch:moment': the value's timezone is not what the explicitTimezone facet requires (XSD 1.1 Part 2, 4.3.14.4); the explicitTimezone facet is 'required'
```

Hours are not a unit SCXML has, and 4000 seconds is more than an hour.
The moment has no timezone.

## Assertions on elements

`xs:assert` in a complex type is a condition on the whole element, as
the draft's `docName` rule in [Validating with XSD](xsd.md) showed. The
context item is the element, with its children and attributes already
validated and typed. The machine's schemas have three:

- SCXML 1.0, section 3.5, says that a transition has an event, a
  condition or a target, at least one of them. `scxml.xsd` says
  `exists(@event | @cond | @target)` on the transition type.
- Section 5.3.1 says that a `data` element gets its value from an
  `expr`, from a `src`, or from its content, and from at most one of
  them: `count((@expr, @src, *)) le 1`.
- `latch-data.xsd` says that a status has a `retry-after` exactly when
  the lock is in lockout: `exists(latch:retry-after) = (latch:state =
  'lockout')`. The comparison of two booleans says "if and only if".

A machine and a status that break them:

```console
$ sed -e 's|<data id="lockoutSeconds" expr="300"|& src="lockout.json"|' \
      -e 's|<transition event="door.opened">|<transition>|' lock.scxml \
  | sxml validate --schema=scxml.xsd -
-:40:5: cvc-assertion: element 'Q{http://www.w3.org/2005/07/scxml}data' breaks the assertion 'count((@expr, @src, *)) le 1' of an anonymous type derived from 'xs:anyType'
-:75:5: cvc-assertion: element 'Q{http://www.w3.org/2005/07/scxml}transition' breaks the assertion 'exists(@event | @cond | @target)' of type 'Q{http://www.w3.org/2005/07/scxml}transition'
$ echo '<status xmlns="urn:example:latch"><door>front</door><state>locked</state><failed-attempts>0</failed-attempts><retry-after>300</retry-after></status>' \
  | sxml validate --schema=latch-data.xsd -
-:1:1: cvc-assertion: element 'Q{urn:example:latch}status' breaks the assertion 'exists(latch:retry-after) = (latch:state = 'lockout')' of an anonymous type derived from 'xs:anyType'
```

No structure could say any of the three. An assertion is reported at
the end of its element, when all its content is known.

## Conditional type assignment

`xs:alternative` chooses an element's type by a condition on its
attributes. The first alternative whose `test` is true gives the type;
one without a `test` is the fallback. The declared type is used when
no alternative applies, and each alternative's type must be derived
from it.

In `latch-1.1.xsd`, an event is a `latch:event`, whose XHTML
description is optional; an event with a `latch:payload` gets the type
`latch:event-with-payload` instead, a restriction that requires the
description: an event that carries data must say what it is. Without
its paragraph, `status.response` breaks the rule:

```console
$ sed 's|<p xmlns="http://www.w3.org/1999/xhtml">The lock answers with its state.</p>||' lock.scxml \
  | sxml validate --schema=latch-1.1.xsd --schema=scxml.xsd -
-:22:5: cvc-complex-type.2.4: the content of element 'latch:event' is incomplete; expected an element of the wildcard's namespaces
```

The tests can only look at attributes, not children: the type must be
known before the children are read. An alternative whose type is
`xs:error`, a type that no element can be valid against, is how a
schema says "not this one at all".

## Open content

`xs:openContent` lets elements that a content model doesn't name appear
among the ones it does. With `mode="interleave"`, they may appear
anywhere; with `mode="suffix"`, only at the end. The wildcard inside
says which elements. The status of `latch-1.1.xsd` is open at the end
for any element of another namespace, so a lock vendor can add its own
fields, such as a battery level:

```console
$ echo '<status xmlns="urn:example:latch" xmlns:acme="urn:example:acme"><door>front</door><state>lockout</state><failed-attempts>3</failed-attempts><retry-after>300</retry-after><acme:battery>81</acme:battery></status>' \
  | sxml validate --schema=latch-1.1.xsd -; echo "exit status $?"
exit status 0
$ echo '<status xmlns="urn:example:latch" xmlns:acme="urn:example:acme"><door>front</door><acme:battery>81</acme:battery><state>lockout</state><failed-attempts>3</failed-attempts><retry-after>300</retry-after></status>' \
  | sxml validate --schema=latch-1.1.xsd -
-:1:83: cvc-complex-type.2.4: element 'acme:battery' is not allowed here; expected 'state'
-:1:114: cvc-complex-type.2.4: element 'state' is not allowed here; expected an element of another namespace
-:1:136: cvc-complex-type.2.4: element 'failed-attempts' is not allowed here; expected an element of another namespace
-:1:172: cvc-complex-type.2.4: element 'retry-after' is not allowed here; expected an element of another namespace
```

In the middle, the battery is not allowed. sxml takes it as the start
of the suffix, so the fields after it are reported too: once the suffix
has begun, only elements of other namespaces may follow.

`xs:defaultOpenContent`, at the top of a schema, gives every complex
type of the schema the same open content.

## Default attributes

`defaultAttributes` on `xs:schema` names an attribute group that every
complex type of the schema document gets, unless it says
`defaultAttributesApply="false"`. In `latch-1.1.xsd`, the group
`traced` holds `latch:implements`, so an event may now say which
requirements it implements, which `latch-data.xsd` doesn't allow:

```console
$ sed 's|<latch:event name="code.accepted">|<latch:event name="code.accepted" latch:implements="req-4">|' lock.scxml > traced.scxml
$ sxml validate --schema=scxml.xsd traced.scxml
traced.scxml:10:39: cvc-complex-type.3.2.2: attribute 'latch:implements' is not allowed on element 'latch:event'
$ sxml validate --schema=latch-1.1.xsd --schema=scxml.xsd traced.scxml; echo "exit status $?"
exit status 0
```

## Changing a schema: xs:override

`latch-1.1.xsd` doesn't copy `latch-data.xsd`: it names it in
`xs:override`, and replaces two of its components, the `events` and
`status` elements, keeping the rest. Every schema document that
includes or imports the overridden one sees the replacements, so
`scxml.xsd`'s import of `latch-data.xsd` now finds the new events.

`scxml.xsd` itself is simplified in one place: its comment says that a
transition has at most one target. SCXML allows a list, `target="motor
light"`, to enter several states at once. This machine has one:

`two-targets.scxml`:

```xml
{{#include ../examples/scxml/two-targets.scxml}}
```

`scxml-targets.xsd` overrides the transition type, with a list type for
the target. That is not enough: a keyref compares a field's whole value
with each key, and the list `motor light` is no state's id. The keyref
can't check each item of a list, so the override replaces the `scxml`
element too, and an assertion takes the keyref's place:

`scxml-targets.xsd`:

```xml
{{#include ../examples/scxml/scxml-targets.xsd}}
```

```console
$ sxml validate --schema=scxml-targets.xsd lock.scxml two-targets.scxml; echo "exit status $?"
exit status 0
$ sed 's/motor light/motor lights/' two-targets.scxml | sxml validate --schema=scxml-targets.xsd -
-:3:1: cvc-assertion: element 'Q{http://www.w3.org/2005/07/scxml}scxml' breaks the assertion 'every $target in .//sc:transition/data(@target)             ...' of an anonymous type derived from 'xs:anyType'
```

The assertion catches the mistake, but it can only say that it failed,
not which target is wrong: the keyref's message pointed at the
transition. That is the trade the subset of `scxml.xsd` avoids, and a
reason to keep keys where they fit. [Checking What the Schema
Cannot](scxml-checks.md) shows how a query reports each mistake on its
own.

Unlike XSD 1.0's `xs:redefine`, which sxml supports too, an override
doesn't have to be a restriction or extension of what it replaces.

## Smaller changes

- In `xs:all`, elements may have `maxOccurs` above 1, and wildcards are
  allowed; a type whose content is an `xs:all` can be extended by
  another `xs:all`.
- Wildcards can exclude names with `notQName` and namespaces with
  `notNamespace`.
- `vc:minVersion` and `vc:maxVersion` (in the namespace
  `http://www.w3.org/2007/XMLSchema-versioning`) on any schema element
  make it apply only to some versions of XSD, so a schema can be
  written today for processors of later versions. The schemas of this
  book say `vc:minVersion="1.1"` on `xs:schema`: they need XSD 1.1, and
  a processor of an earlier version that knows the attribute skips them
  instead of misreading their assertions. sxml is version 1.1, so
  `vc:minVersion="1.2"` would hide an element from it.
- `xs:error`, as above, and the new built-in types
  `xs:dateTimeStamp` (a dateTime with a required timezone),
  `xs:dayTimeDuration`, `xs:yearMonthDuration` and `xs:anyAtomicType`.
