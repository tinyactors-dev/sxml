# Typed Data and Schema-Aware Queries

<!-- examples: scxml -->

Validation does more than say yes or no. A validated document carries
the types of its schema: each element and attribute is *annotated* with
its type, and its *typed value* is a number, a name or a list, whatever
the type says, instead of untyped text. XPath, XQuery and XSLT can use
those types. Processors that can do this are called *schema-aware*,
and sxml is one.

## Typed values in XQuery

`import schema` brings a schema's declarations into a query, and
`validate` validates a document (or element) and returns a typed copy:

`typed.xq`:

```xquery
{{#include ../examples/scxml/typed.xq}}
```

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml typed.xq
"attempts: latch:attempt-count"
"maxAttempts: latch:attempt-limit"
"relockSeconds: latch:seconds"
"lockoutSeconds: latch:seconds"
```

The schema imports `latch-data.xsd`, so its declarations come along;
`declare namespace` only binds the prefix. `latch:type` is declared as
an `xs:QName`, so its typed value is a name with a namespace, not a
string with a colon in it:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    let $type := data(((validate { / })//sc:data/@latch:type)[3])
    return ($type instance of xs:QName, namespace-uri-from-QName($type), local-name-from-QName($type))'
true()
"urn:example:latch"
"seconds"
```

The prefix `latch` was resolved when the document was validated, with
the namespace declarations of the machine; a query that used another
prefix for the same namespace would get the same name.

Lists are sequences. A transition's `event` is a list of event
descriptors, so `@event = "timeout.relock"` is true for the transition
that waits for `lock.request timeout.relock`, as general comparisons
compare each item; the ten transitions handle ten events in all. And
the default value of `type`, which no transition writes, is there:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    let $machine := validate { / }
    return (
      $machine//sc:transition[@event = "timeout.relock"]/@target/string(),
      count($machine//sc:transition/data(@event)),
      distinct-values($machine//sc:transition/@type)
    )'
"locked"
10
"external"
```

Types can also be stricter than untyped data. On the document as it is
parsed, the `expr` of the data items are untyped, and `sum` takes them
as numbers. After validation, they are what the schema says, strings,
because an `expr` is an ECMAScript expression, not a number; `sum` of
strings is an error:

```console
$ sxml xquery --context-item=lock.scxml --expression='
    declare namespace sc = "http://www.w3.org/2005/07/scxml";
    sum(//sc:data/@expr)'
313
$ sxml xquery --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    sum((validate { / })//sc:data/@expr)'
expression:3:5: err:FORG0006: The values are not all numbers, or not all durations of one kind, or not comparable (F&O 4.0, fn:sum, fn:avg, fn:max, fn:min)
$ sxml xquery --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    sum((validate { / })//sc:data/@expr ! xs:integer(.))'
313
```

The error is useful: an `expr` such as `attempts + 1` is no number, and
a typed query says so instead of returning `NaN`. Saying what you mean,
`xs:integer(.)`, makes the conversion explicit.

When the document is not valid, `validate` raises `err:XQDY0027` with
the message `sxml validate` gives, which a query can catch, for example
to check several files and go on:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    for $file in ("lock.scxml", "bad-target.scxml", "duplicate-event.scxml")
    return try {
      (validate { doc($file) }) ! ($file || ": valid")
    } catch err:XQDY0027 {
      $file || ": " || substring-before($err:description, ":")
    }'
"lock.scxml: valid"
"bad-target.scxml: cvc-identity-constraint.4.2.2"
"duplicate-event.scxml: cvc-identity-constraint.4.1"
```

The description is the message of the first error that validation
found.

## Validating in a query

`validate` has three modes:

- `validate { E }` or `validate strict { E }`: the element must have a
  global declaration, and must be valid by it;
- `validate lax { E }`: elements without a declaration are let through,
  and the ones with a declaration are validated wherever they are;
- `validate type T { E }`: the element is validated against the type
  `T`, whatever its name.

A lock's log holds the status payloads it sent. `latch-data.xsd`
declares `latch:status` but no log, and lax validation still finds the
status inside and types it, so its `retry-after` is a number:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    let $log := validate lax {
      <log><latch:status><latch:door>front</latch:door><latch:state>lockout</latch:state>
        <latch:failed-attempts>3</latch:failed-attempts><latch:retry-after>300</latch:retry-after></latch:status></log>
    }
    return $log/latch:status/latch:retry-after + 1'
301
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    validate { <log/> }'
expression:3:5: err:XQDY0084: Strict validation of an element whose name has no global element declaration among the in-scope schema definitions (XQuery 4.0, 4.25)
```

A `validate` expression can't be followed directly by a path or `!`;
put it in parentheses, `(validate { / })//sc:data`, as the examples
here do.

`validate type` works for an element that has no global declaration of
its own. A transition is declared only inside states, but its type is
named:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    (validate type sc:transition { <sc:transition event="lock.request" target="locked"/> })/@type/string()'
"external"
```

## Values of your own types

Every named simple type of an imported schema has a constructor
function, as the built-in types do, and `cast as` and `castable as`
work with it. The facets are checked:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    latch:seconds("300") + 10, "3601" castable as latch:seconds, latch:door-id("front")'
310
false()
"front"
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    latch:state("jammed")'
expression:3:5: err:FORG0001: cvc-enumeration-valid: the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4)
```

A `latch:seconds` is a positive integer, so adding to it is integer
arithmetic. `jammed` is a state of the machine, but not one of the
states the protocol lets a lock report: the draft knows only `locked`,
`unlocked` and `lockout`.

## Checking each value against its type

The machine says the type of each data item with `latch:type`, but no
schema checks that the item's `expr` is a value of that type: the
`expr` is ECMAScript, and the schema can't know that `3` means the
number three. A query can check it. `latch:value` is declared with the
type `xs:anySimpleType`, so `xsi:type` can give it any type, and
validating one with the item's text checks the value. Here is a copy of
the machine that allows 30 attempts, three times the protocol's limit:

```console
$ sed 's/id="maxAttempts" expr="3"/id="maxAttempts" expr="30"/' lock.scxml > generous.scxml
$ sxml xquery --method=adaptive --context-item=generous.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    declare namespace xsi = "http://www.w3.org/2001/XMLSchema-instance";
    for $data in (validate { / })//sc:data
    let $type := data($data/@latch:type)
    return try {
      let $value := validate {
        <latch:value xsi:type="latch:{local-name-from-QName($type)}">{string($data/@expr)}</latch:value>
      }
      return `{$data/@id}: {data($value)}`
    } catch err:XQDY0027 {
      `{$data/@id}: {substring-before($err:description, ":")}`
    }'
"attempts: 0"
"maxAttempts: cvc-maxInclusive-valid"
"relockSeconds: 10"
"lockoutSeconds: 300"
```

The constructor functions give a shorter way: `function-lookup` finds
the constructor whose name is the type's name, and calling it checks
the facets:

```console
$ sxml xquery --method=adaptive --context-item=generous.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    for $data in (validate { / })//sc:data
    let $constructor := function-lookup(data($data/@latch:type), 1)
    return try { `{$data/@id}: {$constructor($data/@expr)}` }
    catch err:FORG0001 { `{$data/@id}: not a {data($data/@latch:type)}` }'
"attempts: 0"
"maxAttempts: not a latch:attempt-limit"
"relockSeconds: 10"
"lockoutSeconds: 300"
```

The `xsi:type` way also works for complex types and gives the full
message; the constructor works only for simple types. [Checking What
the Schema Cannot](scxml-checks.md) makes this one check of several.

## Elements without a typed value

The typed value of an element with simple content, such as
`latch:retry-after`, is its value. An element whose type has
*element-only* content, such as `sc:state`, has no typed value at all,
and asking for it is an error:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    data(((validate { / })//sc:state)[1])'
expression:3:5: err:FOTY0012: An element whose type has element-only content has no typed value (XDM 4.0, section 7.5.2.4)
```

That also catches a common mistake: `$state = "locked"` compares the
typed value of `$state`, so it raises this error for a validated state.
On an untyped document it compares all the text of the state, its
XHTML included, which is rarely what you meant either. Compare
`$state/@id` instead. Elements of mixed content, such as `sc:data`,
have their text as an untyped value.

## Types in signatures

With a schema imported, sequence types can name its declarations and
types. `schema-element(latch:status)` is a `latch:status` element that
was validated by the global declaration. A function with that parameter
type accepts only validated payloads, which catches a whole class of
mistakes:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    declare function local:summary($status as schema-element(latch:status)) as xs:string {
      `{$status/latch:door}: {$status/latch:state}, {$status/latch:failed-attempts} failed`
    };
    local:summary(validate {
      <latch:status><latch:door>front</latch:door><latch:state>locked</latch:state>
        <latch:failed-attempts>2</latch:failed-attempts></latch:status>
    })'
"front: locked, 2 failed"
```

Pass it the same payload without `validate`, and the call fails:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace latch = "urn:example:latch" at "latch-data.xsd";
    declare function local:summary($status as schema-element(latch:status)) as xs:string {
      string($status/latch:door)
    };
    local:summary(<latch:status><latch:door>front</latch:door><latch:state>locked</latch:state>
      <latch:failed-attempts>2</latch:failed-attempts></latch:status>)'
expression:6:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The sequence types that use a schema are:

- `element(N, T)`: an element named `N` whose type is `T` or derived
  from it; `element(*, T)` for any name;
- `attribute(N, T)` and `attribute(*, T)`, the same for attributes;
- `schema-element(N)`: an element that the global declaration `N`
  governs, or a member of its substitution group;
- `schema-attribute(N)`: the same for a global attribute declaration,
  such as `schema-attribute(latch:implements)`.

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    let $machine := validate { / }
    return (
      ($machine//sc:state)[1] instance of element(*, sc:state),
      ($machine//@latch:type)[1] instance of attribute(*, xs:QName),
      $machine/sc:scxml instance of schema-element(sc:scxml),
      /sc:scxml instance of schema-element(sc:scxml)
    )'
true()
true()
true()
false()
```

The last one is false because the context item, the document as it was
parsed, was never validated: its elements are untyped.

## Typed copies

When a constructor copies validated nodes into a new element, the
copies keep their types only with `declare construction preserve`.
With `strip`, they become untyped, as if they came from a document
without a schema:

```console
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    declare construction preserve;
    let $copy := <types>{ (validate { / })//sc:data }</types>
    return type-of(data(($copy//@latch:type)[1]))'
"xs:QName"
$ sxml xquery --method=adaptive --context-item=lock.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace latch = "urn:example:latch";
    declare construction strip;
    let $copy := <types>{ (validate { / })//sc:data }</types>
    return type-of(data(($copy//@latch:type)[1]))'
"xs:untypedAtomic"
```

## Schema-aware XSLT

In XSLT, `xsl:import-schema` imports a schema, and the `validation`
attribute of `xsl:copy-of`, `xsl:document`, `xsl:result-document` and
other instructions (`xsl:validation` on a literal result element)
validates what they make. Patterns and sequence types can test types.
This stylesheet makes the status a lock answers with, from the
machine's data and parameters for the door, the state and the count of
failed attempts:

`status.xsl`:

```xml
{{#include ../examples/scxml/status.xsl}}
```

- `xsl:copy-of` with `validation="strict"` validates the input, and the
  variable's type, `document-node(schema-element(sc:scxml))`, says that
  it must be a validated machine.
- `match="schema-element(sc:scxml)"` matches by declaration, not only
  by name; `match="element(*, sc:state)"` would match every element
  whose type is `sc:state` or derived from it, whatever its name.
- `xsl:validation="strict"` on `latch:status` validates the new
  element against its declaration as it is made: its facets, and its
  assertion.

```console
$ sxml transform --param=state=lockout --param=attempts=3 status.xsl lock.scxml
<?xml version="1.0" encoding="UTF-8"?>
<latch:status xmlns:latch="urn:example:latch">
  <latch:door>front</latch:door>
  <latch:state>lockout</latch:state>
  <latch:failed-attempts>3</latch:failed-attempts>
  <latch:retry-after>300</latch:retry-after>
</latch:status>
```

A state the protocol doesn't have stops the transformation with a type
error instead of writing an invalid status:

```console
$ sxml transform --param=state=jammed status.xsl lock.scxml
status.xsl:27:5: err:XTTE1510: cvc-enumeration-valid: element 'latch:state' has the value 'jammed', which is not valid for type 'latch:state': the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4); the enumeration facets are 'locked', 'unlocked', 'lockout'
```

So does a stylesheet with a mistake, here one that always writes a
`retry-after`, which breaks the status's assertion:

```console
$ sed "s/test=\"\$state = 'lockout'\"/test=\"true()\"/" status.xsl > always.xsl
$ sxml transform always.xsl lock.scxml
always.xsl:27:5: err:XTTE1510: cvc-assertion: element 'latch:status' breaks the assertion 'exists(latch:retry-after) = (latch:state = 'lockout')' of an anonymous type derived from 'xs:anyType'
```

And an input that isn't valid is stopped at the door, by the
`xsl:copy-of`:

```console
$ sxml transform status.xsl bad-target.scxml
status.xsl:20:7: err:XTTE1510: cvc-identity-constraint.4.2.2: element 'Q{http://www.w3.org/2005/07/scxml}state': its key sequence is that of an earlier member (key 'Q{http://www.w3.org/2005/07/scxml}state-id')
```

A schema-aware stylesheet can also validate a whole *output* document,
with `validation="strict"` on `xsl:result-document`: a transformation
that would write an invalid document fails instead.

## From C and JavaScript

Programs validate through the library's schema interface: compile a
schema once, then validate many documents with it, and receive each
error as a value with its constraint and location. [Embedding sxml in
C](c.md) shows the pattern in C (`sxml/schema.h`), and [sxml in
JavaScript](javascript.md) in JavaScript, where `compileSchema` makes a
schema whose `validate` returns a report of the validity and the
errors.
