# Functions and Modules

<!-- examples: xquery -->

As queries grow, you'll want to name the pieces. The prolog of a query
declares functions and variables, and a *library module* collects
declarations that several queries share.

## Declaring functions

A function declared in a query's prolog has a name with a prefix. For
your own functions in a main query, the prefix `local` is ready to use.
Declared functions can call themselves:

`factorial.xq`:

```xquery
{{#include ../examples/xquery/factorial.xq}}
```

```console
$ sxml xquery --method=adaptive factorial.xq
1
2
6
24
120
265252859812191058636308480000000
```

The types in the signature are checked: a string is not converted to an
integer, so `local:factorial("5")` is a type error. Integers have no
fixed size, so 30! is exact.

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
and a namespace URI. It contains only declarations:

`catalog.xqm`:

```xquery
{{#include ../examples/xquery/catalog.xqm}}
```

A query imports it by its namespace, and `at` says where the file is:

`describe.xq`:

```xquery
{{#include ../examples/xquery/describe.xq}}
```

```console
$ sxml xquery --context-item=library.xml describe.xq
Structure and Interpretation of Computer Programs by Harold Abelson and Gerald Jay Sussman (1986)
```

The `sxml` command reads the module from a local file, relative to the
working directory. Programs that embed sxml decide themselves where
modules come from, through a resolver (Chapter 8).

## External variables

`describe.xq` declares `$id` as `external`, with a default value. An
external variable is a parameter of the whole query. `--param` sets it:

```console
$ sxml xquery --context-item=library.xml --param=id=dune describe.xq
Dune by Frank Herbert (1965)
```

When the type doesn't fit, the error says where:

```console
$ sxml xquery --context-item=library.xml --param=id=nothing describe.xq
describe.xq:8:1: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

There is no book with the id `nothing`, so `catalog:book` returns the
empty sequence, and `catalog:describe` requires exactly one book.

## Typed functions catch mistakes

The signatures in `catalog.xqm` say exactly what each function takes
and returns: `element(book)?` is zero or one `book` element. They make a
module self-documenting, and they turn mistakes into clear errors at the
place they happen instead of wrong results later. In XQuery 4.0,
parameters can also have default values, and arguments can be passed by
name:

```console
$ sxml xquery --expression='
    declare function local:greet($name as xs:string, $greeting as xs:string := "Hello") {
      $greeting || ", " || $name || "!"
    };
    local:greet("Ada"), local:greet("Alan", greeting := "Hi")'
Hello, Ada! Hi, Alan!
```
