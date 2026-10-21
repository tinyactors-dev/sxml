# XSD 1.1 Features

<!-- examples: scxml -->

XSD 1.1 keeps everything of XSD 1.0 and adds what users had missed
most: rules written in XPath, types chosen by attribute values, content
models that are open to extensions, and a clean way to change a schema
without copying it. sxml implements XSD 1.1, and validates XSD 1.0
schemas with the same engine. The machine's schemas already use some of
it, and `vane-1.1.xsd`, a stricter version of the annotations' schema,
uses the rest:

`vane-1.1.xsd`:

```xml
{{#include ../examples/scxml/vane-1.1.xsd}}
```

It is used together with `scxml.xsd`. The machine is valid by it too:

```console
$ sxml validate --schema=vane-1.1.xsd --schema=scxml.xsd flag.scxml; echo "exit status $?"
exit status 0
```

## Assertions on values

The `assertion` facet restricts a simple type by an XPath condition, in
which `$value` is the value being checked. SCXML writes the delay of a
`send` as a number and a unit, `600s` or `250ms`. A pattern can check the
form, and an assertion the size: `delay` allows at most an hour.

The second new facet, `explicitTimezone`, says whether dates and times
must have a timezone (`required`), must not have one (`prohibited`), or
may (`optional`, the default). A moment in a log is only one when its
timezone is known.

`vane-data.xsd` declares an element `vane:value` whose type is
`xs:anySimpleType`, any simple value at all, so that a value can be
checked against any of the schema's types with `xsi:type`:

```console
$ for delay in 600s 250ms 2h 4000s; do \
    echo "<vane:value xmlns:vane='urn:example:vane' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='vane:delay'>$delay</vane:value>" \
      | sxml validate --schema=vane-1.1.xsd -; \
  done
-:1:1: cvc-pattern-valid: element 'vane:value' has the value '2h', which is not valid for type 'vane:delay': the value matches no pattern facet (XSD 1.1 Part 2, 4.3.4.4); the pattern facet is '[0-9]+(s|ms)'
-:1:1: cvc-assertions-valid: the value '4000s' breaks an assertion of type 'vane:delay'
$ echo "<vane:value xmlns:vane='urn:example:vane' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='vane:moment'>2026-10-21T09:15:00</vane:value>" \
  | sxml validate --schema=vane-1.1.xsd -
-:1:1: cvc-explicitTimezone-valid: element 'vane:value' has the value '2026-10-21T09:15:00', which is not valid for type 'vane:moment': the value's timezone is not what the explicitTimezone facet requires (XSD 1.1 Part 2, 4.3.14.4); the explicitTimezone facet is 'required'
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
- `vane-data.xsd` says that a status has a `retry-after` exactly when
  the flag is paused: `exists(vane:retry-after) = (vane:state =
  'paused')`. The comparison of two booleans says "if and only if".

A machine and a status that break them:

```console
$ sed -e 's|<data id="pauseSeconds" expr="300"|& src="durations.json"|' \
      -e 's|<transition event="flag.evaluated">|<transition>|' flag.scxml \
  | sxml validate --schema=scxml.xsd -
-:40:5: cvc-assertion: element 'Q{http://www.w3.org/2005/07/scxml}data' breaks the assertion 'count((@expr, @src, *)) le 1' of an anonymous type derived from 'xs:anyType'
-:75:5: cvc-assertion: element 'Q{http://www.w3.org/2005/07/scxml}transition' breaks the assertion 'exists(@event | @cond | @target)' of type 'Q{http://www.w3.org/2005/07/scxml}transition'
$ echo '<status xmlns="urn:example:vane"><flag>new-checkout</flag><state>stable</state><failed-checks>0</failed-checks><retry-after>300</retry-after></status>' \
  | sxml validate --schema=vane-data.xsd -
-:1:1: cvc-assertion: element 'Q{urn:example:vane}status' breaks the assertion 'exists(vane:retry-after) = (vane:state = 'paused')' of an anonymous type derived from 'xs:anyType'
```

No structure could say any of the three. An assertion is reported at
the end of its element, when all its content is known.

## Conditional type assignment

`xs:alternative` chooses an element's type by a condition on its
attributes. The first alternative whose `test` is true gives the type;
one without a `test` is the fallback. The declared type is used when
no alternative applies, and each alternative's type must be derived
from it.

In `vane-1.1.xsd`, an event is a `vane:event`, whose XHTML
description is optional; an event with a `vane:payload` gets the type
`vane:event-with-payload` instead, a restriction that requires the
description: an event that carries data must say what it is. Without
its paragraph, `status.response` breaks the rule:

```console
$ sed 's|<p xmlns="http://www.w3.org/1999/xhtml">The service answers with the state.</p>||' flag.scxml \
  | sxml validate --schema=vane-1.1.xsd --schema=scxml.xsd -
-:22:5: cvc-complex-type.2.4: the content of element 'vane:event' is incomplete; expected an element of the wildcard's namespaces
```

The tests can only look at attributes, not children: the type must be
known before the children are read. An alternative whose type is
`xs:error`, a type that no element can be valid against, is how a
schema says "not this one at all".

## Open content

`xs:openContent` lets elements that a content model doesn't name appear
among the ones it does. With `mode="interleave"`, they may appear
anywhere; with `mode="suffix"`, only at the end. The wildcard inside
says which elements. The status of `vane-1.1.xsd` is open at the end
for any element of another namespace, so a team that runs the service
can add its own fields, such as the error rate its checks measured:

```console
$ echo '<status xmlns="urn:example:vane" xmlns:acme="urn:example:acme"><flag>new-checkout</flag><state>paused</state><failed-checks>3</failed-checks><retry-after>300</retry-after><acme:error-rate>0.07</acme:error-rate></status>' \
  | sxml validate --schema=vane-1.1.xsd -; echo "exit status $?"
exit status 0
$ echo '<status xmlns="urn:example:vane" xmlns:acme="urn:example:acme"><flag>new-checkout</flag><acme:error-rate>0.07</acme:error-rate><state>paused</state><failed-checks>3</failed-checks><retry-after>300</retry-after></status>' \
  | sxml validate --schema=vane-1.1.xsd -
-:1:89: cvc-complex-type.2.4: element 'acme:error-rate' is not allowed here; expected 'state'
-:1:128: cvc-complex-type.2.4: element 'state' is not allowed here; expected an element of another namespace
-:1:149: cvc-complex-type.2.4: element 'failed-checks' is not allowed here; expected an element of another namespace
-:1:181: cvc-complex-type.2.4: element 'retry-after' is not allowed here; expected an element of another namespace
```

In the middle, the error rate is not allowed. sxml takes it as the start
of the suffix, so the fields after it are reported too: once the suffix
has begun, only elements of other namespaces may follow.

`xs:defaultOpenContent`, at the top of a schema, gives every complex
type of the schema the same open content.

## Default attributes

`defaultAttributes` on `xs:schema` names an attribute group that every
complex type of the schema document gets, unless it says
`defaultAttributesApply="false"`. In `vane-1.1.xsd`, the group
`traced` holds `vane:implements`, so an event may now say which
requirements it implements, which `vane-data.xsd` doesn't allow:

```console
$ sed 's|<vane:event name="check.passed">|<vane:event name="check.passed" vane:implements="req-4">|' flag.scxml > traced.scxml
$ sxml validate --schema=scxml.xsd traced.scxml
traced.scxml:10:37: cvc-complex-type.3.2.2: attribute 'vane:implements' is not allowed on element 'vane:event'
$ sxml validate --schema=vane-1.1.xsd --schema=scxml.xsd traced.scxml; echo "exit status $?"
exit status 0
```

## Changing a schema: xs:override

`vane-1.1.xsd` doesn't copy `vane-data.xsd`: it names it in
`xs:override`, and replaces two of its components, the `events` and
`status` elements, keeping the rest. Every schema document that
includes or imports the overridden one sees the replacements, so
`scxml.xsd`'s import of `vane-data.xsd` now finds the new events.

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
$ sxml validate --schema=scxml-targets.xsd flag.scxml two-targets.scxml; echo "exit status $?"
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
