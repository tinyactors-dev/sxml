# Functions and Modules

<!-- examples: spec -->

As queries grow, you'll want to name the pieces. The prolog of a query
declares functions and variables, and a *library module* collects
declarations that several queries share. In this section the
specification project gets its module, `spec.xqm`, which the rest of the
chapter uses.

## Declaring functions

A function declared in a query's prolog has a name with a prefix. For
your own functions in a main query, the prefix `local` is ready to use.
Declared functions can call themselves, which suits a document whose
sections nest. A table of contents lists each section, and after it the
table of contents of the sections inside it:

`toc.xq`:

```xquery
{{#include ../examples/spec/toc.xq}}
```

```console
$ sxml xquery --context-item=latch.xml toc.xq
1. Introduction
1.1. Terminology
2. Lock States
3. Messages
3.1. Unlock
3.2. Status
4. Lockout
5. Security Considerations
6. IANA Considerations
```

The types in the signature are checked: `local:toc` takes sections and a
string, and gives strings. Passing a paragraph instead of a section
would be a type error at the call, not a wrong table later.

sxml evaluates function calls without growing the C stack, so deep
recursion is limited only by the memory sxml may use, not by the stack
of the program it runs in:

```console
$ sxml xquery --method=adaptive --expression='
    declare function local:count-down($n as xs:integer) as xs:integer {
      if ($n eq 0) then 0 else local:count-down($n - 1)
    };
    local:count-down(100000)'
0
```

## Library modules

A library module starts with `module namespace`, which names a prefix
and a namespace URI. It contains only declarations. This is the
project's module:

`spec.xqm`:

```xquery
{{#include ../examples/spec/spec.xqm}}
```

It knows what the rest of the project should not have to repeat: which
paragraphs are requirements, how sections and requirements are numbered,
and what a line of the requirements index holds, as the record type
`spec:entry`. A query imports it by its namespace, and `at` says where
the file is:

`requirement.xq`:

```xquery
{{#include ../examples/spec/requirement.xq}}
```

```console
$ sxml xquery --context-item=latch.xml requirement.xq
req-7 (MUST, section 4): After three rejected codes in a row, a lock MUST enter lockout.
```

The `sxml` command reads the module from a local file, relative to the
working directory. Programs that embed sxml decide themselves where
modules come from, through a resolver ([Files and the Outside
World](files.md)).

Try the functions one by one with `--expression`; that's the quickest
way to see what a module does:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    spec:section-number(//section[@anchor = "status"]),
    spec:requirement-number(//t[@anchor = "req-8"]),
    map:size(spec:anchor-map(/)),
    spec:entry(//t[@anchor = "req-9"])'
"3.2"
10
33
{"anchor":"req-9","number":6,"section":"3.1","keyword":"MAY","text":"A controller MAY send codes that are time-based one-time passwords [RFC6238]."}
```

The text of an entry is the paragraph on one line, with an editorial
note left out and an empty cross reference written as a renderer shows
it: `<xref target="RFC6238"/>` became "[RFC6238]".

## External variables

`requirement.xq` declares `$anchor` as `external`, with a default value.
An external variable is a parameter of the whole query. `--param` sets
it:

```console
$ sxml xquery --context-item=latch.xml --param=anchor=req-5 requirement.xq
req-5 (MUST NOT, section 3.1): A lock MUST NOT unlock while it is in lockout (Section 4).
```

When the type doesn't fit, the error says where:

```console
$ sxml xquery --context-item=latch.xml --param=anchor=req-12 requirement.xq
requirement.xq:7:1: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
$ sxml xquery --context-item=latch.xml --param=anchor=lockout requirement.xq
requirement.xq:7:1: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

There is no element with the anchor `req-12`, so the map lookup gives
the empty sequence; and `lockout` is a section, not a paragraph.
`spec:describe` requires exactly one `t` element, and says so in both
cases.

[Documents, Parameters and the Context](xquery-inputs.md) has more on
external variables and their types.

## Typed functions catch mistakes

The signatures in `spec.xqm` say exactly what each function takes and
returns: `element(t)*` is any number of paragraphs, `map(xs:string,
element())` a map from strings to elements, `spec:entry` a record with
five fields of known types. They make a module self-documenting, and
they turn mistakes into clear errors at the place they happen instead of
wrong results later. In XQuery 4.0, parameters can also have default
values, and arguments can be passed by name, as `spec:entry` is called
inside the module:

```console
$ sxml xquery --context-item=latch.xml --expression='
    declare function local:label($section as element(section), $separator as xs:string := ".") {
      string-join($section/ancestor-or-self::section ! (count(preceding-sibling::section) + 1), $separator)
    };
    local:label(//section[@anchor = "status"]), local:label(//section[@anchor = "status"], separator := "-")'
3.2 3-2
```

## Private functions and module variables

A library module can declare variables as well as functions, and it can
keep helpers to itself. `%private` is an *annotation*: it makes a
function or variable visible only inside its own module. `spec.xqm`
keeps `spec:section`, `spec:text` and `spec:xref-text` private, and
offers `$spec:keywords`:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    count($spec:keywords), $spec:keywords[1]'
11
"MUST"
$ sxml xquery --method=adaptive --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    spec:text(<t>A <bcp14>MUST</bcp14>.</t>)'
expression:3:5: err:XPST0017: no function with this name and arity is known
```

Keeping helpers private lets you change them later without breaking the
queries that import the module: how the text of a paragraph is made can
change, and `spec:entry` stays what it is.

## Functions as values

A declared function is a value too. `spec:requirement-number#1` is a
reference to the function with one parameter, which you can pass to a
higher-order function such as `for-each`:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    spec:requirements(/) => for-each(spec:requirement-number#1) => string-join(" ")'
"1 2 3 4 5 6 7 8 9 10 11"
```

Parameters can have function types, written `fn(...) as ...`. This
function counts the requirements that pass a test it is given:

```console
$ sxml xquery --context-item=latch.xml --method=adaptive --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    declare function local:count-where($draft as document-node(), $test as fn(element(t)) as xs:boolean) as xs:integer {
      count(spec:requirements($draft)[$test(.)])
    };
    local:count-where(/, fn($t) { spec:keywords($t) = "MUST" }),
    local:count-where(/, fn($t) { contains($t, "controller") })'
5
4
```

[Functions as Values](xpath-higher-order.md) explains inline functions,
partial application and the higher-order functions of the library; all
of it works in XQuery.

## When an import fails

The namespace in `import module` must be the one that the module
declares, and the file must exist. Both mistakes are static errors,
found before the query runs:

```console
$ sxml xquery --expression='
    import module namespace spec = "urn:example:spec" at "specs.xqm";
    1'
expression:2:5: err:XQST0059: no module is found for an imported namespace
$ sxml xquery --expression='
    import module namespace spec = "urn:example:specification" at "spec.xqm";
    1'
expression:2:5: err:XQST0059: the module found is not a library module of the imported namespace
```

An error inside a module is reported with the module's own file name,
line and column, so you know where to look. That holds for errors the
module raises as it runs, too: a paragraph that is in no section has no
section number, and `spec:section` promises one:

```console
$ sxml xquery --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    spec:describe(<t anchor="req-12">A lock <bcp14>MAY</bcp14> beep.</t>)'
spec.xqm:68:86: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

Line 68 is the declaration of `spec:section`, and column 86 its result
type, `element(section)`, which the empty result does not match.
