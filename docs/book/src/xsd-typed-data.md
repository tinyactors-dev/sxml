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
$ sxml xquery --method=adaptive --context-item=flag.scxml typed.xq
"failures: vane:failure-count"
"maxFailures: vane:failure-limit"
"stepSeconds: vane:seconds"
"pauseSeconds: vane:seconds"
```

The schema imports `vane-data.xsd`, so its declarations come along;
`declare namespace` only binds the prefix. `vane:type` is declared as
an `xs:QName`, so its typed value is a name with a namespace, not a
string with a colon in it:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    let $type := data(((validate { / })//sc:data/@vane:type)[3])
    return ($type instance of xs:QName, namespace-uri-from-QName($type), local-name-from-QName($type))'
true()
"urn:example:vane"
"seconds"
```

The prefix `vane` was resolved when the document was validated, with
the namespace declarations of the machine; a query that used another
prefix for the same namespace would get the same name.

Lists are sequences. A transition's `event` is a list of event
descriptors, so `@event = "step.done"` is true for the transition
that waits for `kill.request step.done`, as general comparisons
compare each item; the nine transitions handle ten events in all. And
the default value of `type`, which no transition writes, is there:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    let $machine := validate { / }
    return (
      $machine//sc:transition[@event = "step.done"]/@target/string(),
      count($machine//sc:transition/data(@event)),
      distinct-values($machine//sc:transition/@type)
    )'
"stable"
10
"external"
```

Types can also be stricter than untyped data. On the document as it is
parsed, the `expr` of the data items are untyped, and `sum` takes them
as numbers. After validation, they are what the schema says, strings,
because an `expr` is an ECMAScript expression, not a number; `sum` of
strings is an error:

```console
$ sxml xquery --context-item=flag.scxml --expression='
    declare namespace sc = "http://www.w3.org/2005/07/scxml";
    sum(//sc:data/@expr)'
903
$ sxml xquery --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    sum((validate { / })//sc:data/@expr)'
expression:3:5: err:FORG0006: The values are not all numbers, or not all durations of one kind, or not comparable (F&O 4.0, fn:sum, fn:avg, fn:max, fn:min)
$ sxml xquery --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    sum((validate { / })//sc:data/@expr ! xs:integer(.))'
903
```

The error is useful: an `expr` such as `failures + 1` is no number, and
a typed query says so instead of returning `NaN`. Saying what you mean,
`xs:integer(.)`, makes the conversion explicit.

When the document is not valid, `validate` raises `err:XQDY0027` with
the message `sxml validate` gives, which a query can catch, for example
to check several files and go on:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    for $file in ("flag.scxml", "bad-target.scxml", "duplicate-event.scxml")
    return try {
      (validate { doc($file) }) ! ($file || ": valid")
    } catch err:XQDY0027 {
      $file || ": " || substring-before($err:description, ":")
    }'
"flag.scxml: valid"
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

A flag's log holds the status payloads the service sent. `vane-data.xsd`
declares `vane:status` but no log, and lax validation still finds the
status inside and types it, so its `retry-after` is a number:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
    let $log := validate lax {
      <log><vane:status><vane:flag>new-checkout</vane:flag><vane:state>paused</vane:state>
        <vane:failed-checks>3</vane:failed-checks><vane:retry-after>300</vane:retry-after></vane:status></log>
    }
    return $log/vane:status/vane:retry-after + 1'
301
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
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
    (validate type sc:transition { <sc:transition event="kill.request" target="stable"/> })/@type/string()'
"external"
```

## Values of your own types

Every named simple type of an imported schema has a constructor
function, as the built-in types do, and `cast as` and `castable as`
work with it. The facets are checked:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
    vane:seconds("300") + 10, "3601" castable as vane:seconds, vane:flag-key("new-checkout")'
310
false()
"new-checkout"
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
    vane:state("archived")'
expression:3:5: err:FORG0001: cvc-enumeration-valid: the value is not one of the enumeration facets (XSD 1.1 Part 2, 4.3.5.4)
```

A `vane:seconds` is a positive integer, so adding to it is integer
arithmetic. `archived` is a state of the machine, but not one of the
states the protocol lets a service report: the draft knows only
`stable`, `rolling-out` and `paused`.

## Checking each value against its type

The machine says the type of each data item with `vane:type`, but no
schema checks that the item's `expr` is a value of that type: the
`expr` is ECMAScript, and the schema can't know that `3` means the
number three. A query can check it. `vane:value` is declared with the
type `xs:anySimpleType`, so `xsi:type` can give it any type, and
validating one with the item's text checks the value. Here is a copy of
the machine that allows 30 failed checks, three times the limit of
`vane:failure-limit`:

```console
$ sed 's/id="maxFailures" expr="3"/id="maxFailures" expr="30"/' flag.scxml > generous.scxml
$ sxml xquery --method=adaptive --context-item=generous.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    declare namespace xsi = "http://www.w3.org/2001/XMLSchema-instance";
    for $data in (validate { / })//sc:data
    let $type := data($data/@vane:type)
    return try {
      let $value := validate {
        <vane:value xsi:type="vane:{local-name-from-QName($type)}">{string($data/@expr)}</vane:value>
      }
      return `{$data/@id}: {data($value)}`
    } catch err:XQDY0027 {
      `{$data/@id}: {substring-before($err:description, ":")}`
    }'
"failures: 0"
"maxFailures: cvc-maxInclusive-valid"
"stepSeconds: 600"
"pauseSeconds: 300"
```

The constructor functions give a shorter way: `function-lookup` finds
the constructor whose name is the type's name, and calling it checks
the facets:

```console
$ sxml xquery --method=adaptive --context-item=generous.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    for $data in (validate { / })//sc:data
    let $constructor := function-lookup(data($data/@vane:type), 1)
    return try { `{$data/@id}: {$constructor($data/@expr)}` }
    catch err:FORG0001 { `{$data/@id}: not a {data($data/@vane:type)}` }'
"failures: 0"
"maxFailures: not a vane:failure-limit"
"stepSeconds: 600"
"pauseSeconds: 300"
```

The `xsi:type` way also works for complex types and gives the full
message; the constructor works only for simple types. [Checking What
the Schema Cannot](scxml-checks.md) makes this one check of several.

## Elements without a typed value

The typed value of an element with simple content, such as
`vane:retry-after`, is its value. An element whose type has
*element-only* content, such as `sc:state`, has no typed value at all,
and asking for it is an error:

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    data(((validate { / })//sc:state)[1])'
expression:3:5: err:FOTY0012: An element whose type has element-only content has no typed value (XDM 4.0, section 7.5.2.4)
```

That also catches a common mistake: `$state = "stable"` compares the
typed value of `$state`, so it raises this error for a validated state.
On an untyped document it compares all the text of the state, its
XHTML included, which is rarely what you meant either. Compare
`$state/@id` instead. Elements of mixed content, such as `sc:data`,
have their text as an untyped value.

## Types in signatures

With a schema imported, sequence types can name its declarations and
types. `schema-element(vane:status)` is a `vane:status` element that
was validated by the global declaration. A function with that parameter
type accepts only validated payloads, which catches a whole class of
mistakes:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
    declare function local:summary($status as schema-element(vane:status)) as xs:string {
      `{$status/vane:flag}: {$status/vane:state}, {$status/vane:failed-checks} failed`
    };
    local:summary(validate {
      <vane:status><vane:flag>new-checkout</vane:flag><vane:state>stable</vane:state>
        <vane:failed-checks>2</vane:failed-checks></vane:status>
    })'
"new-checkout: stable, 2 failed"
```

Pass it the same payload without `validate`, and the call fails:

```console
$ sxml xquery --method=adaptive --expression='
    import schema namespace vane = "urn:example:vane" at "vane-data.xsd";
    declare function local:summary($status as schema-element(vane:status)) as xs:string {
      string($status/vane:flag)
    };
    local:summary(<vane:status><vane:flag>new-checkout</vane:flag><vane:state>stable</vane:state>
      <vane:failed-checks>2</vane:failed-checks></vane:status>)'
expression:6:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The sequence types that use a schema are:

- `element(N, T)`: an element named `N` whose type is `T` or derived
  from it; `element(*, T)` for any name;
- `attribute(N, T)` and `attribute(*, T)`, the same for attributes;
- `schema-element(N)`: an element that the global declaration `N`
  governs, or a member of its substitution group;
- `schema-attribute(N)`: the same for a global attribute declaration,
  such as `schema-attribute(vane:implements)`.

```console
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    let $machine := validate { / }
    return (
      ($machine//sc:state)[1] instance of element(*, sc:state),
      ($machine//@vane:type)[1] instance of attribute(*, xs:QName),
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
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    declare construction preserve;
    let $copy := <types>{ (validate { / })//sc:data }</types>
    return type-of(data(($copy//@vane:type)[1]))'
"xs:QName"
$ sxml xquery --method=adaptive --context-item=flag.scxml --expression='
    import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
    declare namespace vane = "urn:example:vane";
    declare construction strip;
    let $copy := <types>{ (validate { / })//sc:data }</types>
    return type-of(data(($copy//@vane:type)[1]))'
"xs:untypedAtomic"
```

## Schema-aware XSLT

In XSLT, `xsl:import-schema` imports a schema, and the `validation`
attribute of `xsl:copy-of`, `xsl:document`, `xsl:result-document` and
other instructions (`xsl:validation` on a literal result element)
validates what they make. Patterns and sequence types can test types.
This stylesheet makes the status the service answers with for a flag,
from the machine's data and parameters for the flag key, the state and
the count of failed checks:

`status.xsl`:

```xml
{{#include ../examples/scxml/status.xsl}}
```

- The parameters have types of the schema, in `as`: `vane:flag-key`,
  `vane:state` and `vane:failure-count`. A value given on the command
  line with `--param` is text, and it is converted to the parameter's
  type when the transformation starts; a default in `select` is
  converted the same way.
- `xsl:copy-of` with `validation="strict"` validates the input, and the
  variable's type, `document-node(schema-element(sc:scxml))`, says that
  it must be a validated machine.
- `match="schema-element(sc:scxml)"` matches by declaration, not only
  by name; `match="element(*, sc:state)"` would match every element
  whose type is `sc:state` or derived from it, whatever its name.
- `xsl:validation="strict"` on `vane:status` validates the new
  element against its declaration as it is made: its facets, and its
  assertion.

```console
$ sxml transform --param=state=paused --param=failures=3 status.xsl flag.scxml
<?xml version="1.0" encoding="UTF-8"?>
<vane:status xmlns:vane="urn:example:vane">
  <vane:flag>new-checkout</vane:flag>
  <vane:state>paused</vane:state>
  <vane:failed-checks>3</vane:failed-checks>
  <vane:retry-after>300</vane:retry-after>
</vane:status>
```

A state the protocol doesn't have, or a negative count, is not a value
of the parameter's type, and the transformation stops before it begins,
with the type error for a parameter, `XTTE0590`:

```console
$ sxml transform --param=state=archived status.xsl flag.scxml
status.xsl: err:XTTE0590: The value does not match the required type of the parameter (XSLT 4.0, section 9.3)
$ sxml transform --param=failures=-1 status.xsl flag.scxml
status.xsl: err:XTTE0590: The value does not match the required type of the parameter (XSLT 4.0, section 9.3)
```

The parameters' types check each value on its own. What they make
together is checked when the status is built: a stylesheet with a
mistake, here one that always writes a `retry-after`, breaks the
status's assertion, and stops with a type error instead of writing an
invalid status:

```console
$ sed "s/test=\"\$state = 'paused'\"/test=\"true()\"/" status.xsl > always.xsl
$ sxml transform always.xsl flag.scxml
always.xsl:28:5: err:XTTE1510: cvc-assertion: element 'vane:status' breaks the assertion 'exists(vane:retry-after) = (vane:state = 'paused')' of an anonymous type derived from 'xs:anyType'
```

And an input that isn't valid is stopped where it comes in, by the
`xsl:copy-of`:

```console
$ sxml transform status.xsl bad-target.scxml
status.xsl:21:7: err:XTTE1510: cvc-identity-constraint.4.2.2: element 'Q{http://www.w3.org/2005/07/scxml}state': its key sequence is that of an earlier member (key 'Q{http://www.w3.org/2005/07/scxml}state-id')
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
