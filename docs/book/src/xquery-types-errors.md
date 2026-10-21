# Conditions, Types and Errors

<!-- examples: spec -->

Real documents are untidy. A section holds paragraphs, lists and code
in any order, a cross reference can point to an anchor that nobody
defined, a value can be text where a number was expected. This section
shows how a query chooses between cases, how it states the types it
expects, and how it deals with errors.

## Choosing by value: switch

`if` chooses between two branches; XPath's `if` was in
[Functions and Operators](xpath-functions.md). `switch` chooses between
many, by comparing one value with the values of its `case` clauses. A
`case` can list several values, and `default` is required. BCP 14 has
eleven keywords, of three strengths:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    for $keyword in distinct-values(//bcp14)
    return $keyword || ": " || (switch ($keyword)
      case "MUST" case "MUST NOT" case "REQUIRED" case "SHALL" case "SHALL NOT" return "absolute"
      case "SHOULD" case "SHOULD NOT" case "RECOMMENDED" case "NOT RECOMMENDED" return "strong"
      case "MAY" case "OPTIONAL" return "optional"
      default return "not a BCP 14 keyword")'
"MUST: absolute"
"SHOULD: strong"
"MUST NOT: absolute"
"MAY: optional"
"SHOULD NOT: strong"
```

The values are compared as `deep-equal` compares them, after an untyped
value (the text of an element, here) has been made a string. So a
string never matches a number: `switch ("3") case 3` takes the
`default`. The parentheses around the `switch` are needed because it
is the operand of `||`.

In XQuery 4.0, the value in parentheses can be left out. Then each
`case` is a condition, and the first one that is true wins:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    for $requirement in //t[@anchor]
    let $words := count(tokenize(normalize-space($requirement)))
    return `{$requirement/@anchor}: ` || (switch () {
      case $words lt 12 return "short"
      case $words lt 20 return "medium"
      default return "long"
    })'
"req-1: medium"
"req-2: medium"
"req-3: medium"
"req-4: long"
"req-5: short"
"req-9: short"
"req-6: long"
"req-10: medium"
"req-7: medium"
"req-8: medium"
"req-11: medium"
```

The braces around the cases are optional; they help when a `switch`
sits inside a larger expression.

## Choosing by type: typeswitch

`typeswitch` chooses by the *type* of a value. Each `case` names a
sequence type, and a variable in the `case` binds the value with that
type. A section of the draft holds blocks of several kinds:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    for $block in //section[@anchor = ("messages", "terminology")]/*
    return typeswitch ($block)
      case element(name) return "heading: " || $block
      case element(t) return "paragraph of " || count(tokenize(normalize-space($block))) || " words"
      case $list as element(dl) return "definition list of " || count($list/dt) || " terms"
      case $code as element(sourcecode) return "code in " || $code/@type || ", " || count(tokenize($code, "\n")[normalize-space()]) || " lines"
      default return "a " || name($block)'
"heading: Terminology"
"paragraph of 43 words"
"definition list of 4 terms"
"heading: Messages"
"paragraph of 13 words"
"code in abnf, 6 lines"
"a section"
"a section"
```

The cases are tried in order, and the first that matches wins; here a
`section` inside Messages matches none, and takes the default. Types
are checked as `instance of` checks them, so derived types count: an
`xs:integer` is also an `xs:decimal`, but an `xs:double` is neither:

```console
$ sxml xquery --method=adaptive --expression='
    for $value in (42, 4.2, 4.2e0)
    return typeswitch ($value)
      case xs:integer return "integer"
      case xs:decimal return "decimal"
      default return "other"'
"integer"
"decimal"
"other"
```

## Saying what you expect

A sequence type after `as` states what a variable, a parameter or a
function result must be. You can write one on `let` and `for` too:

```console
$ sxml xquery --method=adaptive --expression='let $count as xs:integer := 11 return $count + 1'
12
$ sxml xquery --method=adaptive --expression='let $count as xs:integer := "11" return $count + 1'
expression:1:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The value must already have the type, or be convertible to it by the
*coercion rules*, which are few: untyped values are cast to the
required type, integers and decimals are promoted to doubles where a
double is required, and nodes are atomized where an atomic value is
required. A string is never turned into a number. Untyped values are
what documents without a schema give you, so the year of a reference,
an attribute, works where an integer is required:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    let $year as xs:integer := //reference[@anchor = "RFC2119"]//date/@year
    return 2026 - $year'
29
```

`treat as` is a check without a conversion: it raises `err:XPDY0050`
when the value doesn't have the type, and otherwise gives the value
unchanged:

```console
$ sxml xquery --context-item=latch.xml --expression='//t[@anchor = "req-1"]/bcp14 treat as element(bcp14)'
<bcp14>MUST</bcp14>
$ sxml xquery --context-item=latch.xml --expression='//t[@anchor = "req-1"]/@anchor treat as element(bcp14)'
expression:1:1: err:XPDY0050: The value of a treat expression does not match its type (XPath 4.0, section 4.16.6)
```

## Naming types

When the same type appears in many signatures, give it a name.
`declare type` names any item type, and `enum` is the type of a few
fixed strings, which is just what the BCP 14 keywords are:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    declare type local:keyword as enum("MUST", "MUST NOT", "SHOULD", "SHOULD NOT", "MAY");
    declare function local:is-absolute($keyword as local:keyword) as xs:boolean {
      starts-with($keyword, "MUST")
    };
    local:is-absolute(//t[@anchor = "req-5"]/bcp14), local:is-absolute("MIGHT")'
expression:6:54: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The first call is fine: the `bcp14` element is atomized, and its text
"MUST NOT" is one of the strings of the enumeration. "MIGHT" is not, so
the second call fails, and with it the whole query. The draft's schema
makes the same rule for documents, in
[Simple Types and Facets](xsd-simple-types.md); the type in the query
makes it for the values a function is given.

`declare record` names a record type, and also declares a function of
the same name that makes such records. Fields can have default values,
and the function takes keyword arguments:

```console
$ sxml xquery --method=adaptive --expression='
    declare record local:requirement(anchor as xs:string, keyword as xs:string, section as xs:string? := ());
    local:requirement("req-12", "MUST"),
    local:requirement("req-13", "MAY", section := "Status"),
    { "anchor": "req-1" } instance of local:requirement'
{"anchor":"req-12","keyword":"MUST","section":()}
{"anchor":"req-13","keyword":"MAY","section":"Status"}
false()
```

The last map has no `keyword`, so it is not a `local:requirement`.

## Kinds of errors

Errors come in three kinds, and the second letter pair of the code
tells you which:

- **Static errors** (`XPST`, `XQST`) are found when the query is
  compiled, before anything runs: a syntax error, an unknown function,
  an undeclared variable.
- **Type errors** (`XPTY`, `XQTY`) mean a value has the wrong type for
  where it is used.
- **Dynamic errors** (`XPDY`, `XQDY`, and the `FO` codes of the
  function library) happen while the query runs: division by zero, a
  missing document, a value that can't be cast.

A query with a static error doesn't run at all, so a static error can't
be caught. Here is one that surprises many people: a function body has
no context item, so a path inside a function can't start from the
document:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare function local:requirements() { //t[@anchor] };
    count(local:requirements())'
expression:2:45: err:XPDY0002: the focus is absent in a function body
```

Pass the document to the function as a parameter, or bind it to a
global variable, whose value may use the context item.

## Catching errors

`try` evaluates an expression, and when it raises a dynamic or type
error, the first `catch` clause whose name matches the error code gives
the value instead. `*` matches every error, `err:*` every error of the
specifications, and `|` lists several codes. The number in an anchor
such as `req-7` is easy to take out, but a term's anchor has none:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    for $anchor in ("req-7", "term-lock")
    return try { xs:integer(substring-after($anchor, "req-")) } catch err:FORG0001 | err:XPTY0004 { -1 }'
7
-1
```

Inside a `catch`, variables describe the error: `$err:code` is its
name as an `xs:QName`, `$err:description` its message, `$err:value` the
value that came with it, and `$err:module`, `$err:line-number` and
`$err:column-number` say where it happened. `$err:map` holds all of them
in one map:

```console
$ sxml xquery --method=adaptive --expression='try { xs:integer(substring-after("term-lock", "req-")) } catch * { $err:map }'
{"code":#Q{http://www.w3.org/2005/xqt-errors}FORG0001,"description":"the value is not in the lexical space or value space of the target type","module":"expression","line-number":1,"column-number":7}
```

`#Q{...}FORG0001` is how the adaptive method writes an `xs:QName`.

## Raising your own errors

`error` raises an error with a code of your own, a description and,
optionally, a value. Give your codes a namespace of your own, so they
can't be confused with the standard ones; the specification project
uses `urn:example:spec`. This query shows what each cross reference of
the draft will say when it is rendered, and turns a reference to a
missing anchor into a line of the report instead of a failure:

`resolve.xq`:

```xquery
{{#include ../examples/spec/resolve.xq}}
```

```console
$ sxml xquery --context-item=latch.xml resolve.xq
intro: the term "Controller"
terminology: [RFC2119]
terminology: [RFC8174]
terminology: the term "Lock"
req-1: the term "Lock"
messages: [RFC5234]
req-4: the term "Code"
req-5: the term "Lockout"
req-5: Section "Lockout"
req-9: [RFC6238]
req-7: the term "Lockout"
```

Every reference of the draft resolves. To see the error at work, break a
copy of the draft: `sed` renames one target to an anchor that nobody
defines.

```console
$ sed 's/target="term-code"/target="term-pin"/' latch.xml > broken.xml
$ sxml xquery --context-item=broken.xml resolve.xq | grep req-4
req-4: No element has the anchor term-pin
```

The query went on after the error, and reported every reference; the
`catch` caught `spec:dangling` and nothing else, so a mistake in the
query itself would still stop it. The third argument of `error`, the
target, is `$err:value` in the `catch`, for a caller that wants the
anchor rather than the message.

`otherwise` gives its right side when the left side is empty, which
makes "this, or else fail" short to write. An error that nobody catches
ends the query, and the `sxml` command reports it with its code and
description, and exits with status 1:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare namespace spec = "urn:example:spec";
    //*[@anchor = "req-12"] otherwise error(xs:QName("spec:dangling"), "No element has the anchor req-12")'; echo "exit status $?"
expression:3:39: spec:dangling: No element has the anchor req-12
exit status 1
```

## Errors on purpose

Raising errors is the right thing to do much more often than you might
think. A query that silently returns nothing for bad input is hard to
debug; one that says "No element has the anchor term-pin" is not. Some
habits help:

- Declare the types of parameters and results. The error then happens
  at the call that is wrong, not three functions later.
- Use `exactly-one`, `one-or-more` and `zero-or-one` when a path must
  find a certain number of nodes; they raise an error otherwise.
- Catch only the errors you expect, by their codes, and let the others
  through.

`try` can end with a `finally` clause, which runs whether or not an
error happened, for work that must be done either way, such as a
message. Its value must be empty. The month of a reference is a name,
so it isn't an integer:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    try { xs:integer(//reference[@anchor = "RFC2119"]//date/@month) }
    catch err:FORG0001 { "the month is a name" }
    finally { message("checked the month") }'
"checked the month"
"the month is a name"
```

`fn:trace` and `fn:message` are for debugging output. `trace` returns
its input unchanged and writes it, after its label, to standard error;
`message` writes its input and returns nothing. The values are written
as the adaptive method shows them, so strings are in quotes:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    let $total := trace(count(//t[@anchor]), "requirements")
    return (message("halfway"), $total * 2)'
requirements: 11
"halfway"
22
```

A program that embeds sxml receives the same output through a trace
sink of its own.
