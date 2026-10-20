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

[Documents, Parameters and the Context](xquery-inputs.md) has more on
external variables and their types.

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

## Private functions and module variables

A library module can declare variables as well as functions, and it can
keep helpers to itself. `%private` is an *annotation*: it makes a
function or variable visible only inside its own module. This module
computes when loans are due:

`loans.xqm`:

```xquery
{{#include ../examples/xquery/loans.xqm}}
```

The importing query sees `loans:due` and `$loans:days`, but not the
private helpers:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace loans = "https://example.org/loans" at "loans.xqm";
    loans:due(<loan date="2026-10-01"/>), $loans:days'
xs:date("2026-10-22")
21
$ sxml xquery --method=adaptive --expression='
    import module namespace loans = "https://example.org/loans" at "loans.xqm";
    loans:start(<loan date="2026-10-01"/>)'
expression:3:5: err:XPST0017: no function with this name and arity is known
```

Keeping helpers private lets you change them later without breaking the
queries that import the module.

## Functions as values

A declared function is a value too. `loans:due#1` is a reference to the
function `loans:due` with one parameter, which you can pass to a
higher-order function such as `for-each`. Parameters can have function
types, written `fn(...) as ...`:

```console
$ sxml xquery --method=adaptive --expression='
    declare function local:twice($f as fn(xs:integer) as xs:integer, $x as xs:integer) as xs:integer {
      $f($f($x))
    };
    local:twice(fn($n) { $n * 3 }, 2)'
18
```

Chapter 2's [Functions as Values](xpath-higher-order.md) explains inline
functions, partial application and the higher-order functions of the
library; all of it works in XQuery.

## When an import fails

The namespace in `import module` must be the one that the module
declares, and the file must exist. Both mistakes are static errors,
found before the query runs:

```console
$ sxml xquery --expression='
    import module namespace loans = "https://example.org/loans" at "lending.xqm";
    1'
expression:2:5: err:XQST0059: no module is found for an imported namespace
$ sxml xquery --expression='
    import module namespace loans = "https://example.org/lending" at "loans.xqm";
    1'
expression:2:5: err:XQST0059: the module found is not a library module of the imported namespace
```

An error inside a module is reported with the module's own file name,
line and column, so you know where to look.
