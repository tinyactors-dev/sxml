# Embedding sxml in C

<!-- examples: c -->

Everything the `sxml` command does, it does through the sxml C library,
and your programs can use the same library. It is portable C17, has no
dependencies, never calls `malloc` behind your back, keeps no global
state, and never recurses, so a deeply nested document cannot overflow
your program's stack. This chapter shows the shape of the API with five
small programs. The examples are in `docs/book/examples/c`.

## Building against sxml

The simplest way to use sxml is to build it as part of your project with
CMake. Put the sxml source in your tree (as a git submodule, for
example) and add it as a subdirectory:

`CMakeLists.txt`:

```cmake
{{#include ../examples/c/CMakeLists.txt}}
```

`SXML_BUILD_TESTS` and `SXML_BUILD_APPS` leave out sxml's own tests and
the command, and the `sxml` target is the static library with every
header in `include/sxml/`. A program links only what it uses: one that
uses only XPath doesn't link XQuery, XSLT or XSD.

The examples' `CMakeLists.txt` takes the location of the sxml source as
`SXML_SOURCE`. Here `$SXML` is the root of an sxml checkout:

```console
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSXML_SOURCE="$SXML" > /dev/null
$ cmake --build build > /dev/null
```

The examples share a few lines of output code, in `output.h`:

```c
{{#include ../examples/c/output.h}}
```

`sxml_byte_view` is how the API passes text: a pointer and a length,
never a NUL-terminated string. Output goes to an `sxml_output_stream`,
an interface you implement; this one writes to `stdout`.

## A first program

`books.c`:

```c
{{#include ../examples/c/books.c}}
```

```console
$ ./build/books
"The Art of Computer Programming"
```

The steps are the same in every program:

1. Create a **processor**. It owns the names, types and values that
   everything else uses, and it holds what the host provides: the
   resource resolver, the clock, the trace sink and the limits.
2. Parse documents with the processor.
3. **Compile** an expression, a query or a stylesheet.
4. **Evaluate** it, with a dynamic context: the context item and the
   values of variables.
5. Serialize the result, or read its items with the functions of
   `sxml/xdm.h`.
6. Release values, destroy compiled objects, then the processor.

Every function returns an `sxml_status`. Arguments come in structs with
designated initializers, so a call says what each value is, and fields
you leave out are zero: the defaults.

## Compile once, run many times

Compiling parses the expression, resolves its names, checks its types
and optimizes it. Do it once, and evaluate the result as often as you
like, with different documents and different variables:

`compile_once.c`:

```c
{{#include ../examples/c/compile_once.c}}
```

```console
$ ./build/compile_once
library 1, before 1970:
"The Art of Computer Programming"
library 1, before 1966:

library 2, before 1970:
"Dune"
"The Left Hand of Darkness"
library 2, before 1966:
"Dune"
```

An external variable is *declared* when the expression is compiled, with
its type, and *bound* each time it is evaluated. Names are interned once
in the processor's name pool and then passed as small identifiers.
Small integers, doubles and booleans are items that need no memory;
strings and other values are made in the processor's item heap.

## Resources and XQuery

The processor's resource resolver answers every request for a resource:
`fn:doc`, `fn:unparsed-text`, imported modules, schemas, packages. sxml
reads nothing else. `sxml_table_resource_resolver` serves resources from
memory, which is all a test or a sandbox needs; for anything else,
implement `sxml_resource_resolver_operations` yourself.

This program also shows XQuery. A query is compiled and evaluated like
an expression, and it carries the serialization parameters its prolog
declares:

`resolver.c`:

```c
{{#include ../examples/c/resolver.c}}
```

```console
$ ./build/resolver
<loans>
  <loan member="ada">Dune</loan>
  <loan member="alan">Godel, Escher, Bach</loan>
  <loan member="grace">Dune</loan>
</loans>false
```

`secret.xml` is not in the table, so the query cannot see it, whatever
files there are on the machine. XSLT (`sxml/xslt.h`) and XSD
(`sxml/schema.h`) follow the same pattern: compile a stylesheet or a
schema once with a processor, then run transformations or validations
with it.

## Errors

When a function fails, its status says how, and the *error record* you
passed says what and where: the error code, the description and the
location in the expression text. Static errors come from compiling,
dynamic and type errors from evaluating:

`errors.c`:

```c
{{#include ../examples/c/errors.c}}
```

```console
$ ./build/errors
6 * 7 => 42
1 + => SXML_STATUS_STATIC_ERROR err:XPST0003 at 1:3: The text is not valid XPath 4.0 syntax (XPath 4.0, Appendix A)
1 div 0 => SXML_STATUS_DYNAMIC_ERROR err:FOAR0001 at 1:1: division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)
error(xs:QName('err:BOOK0001'), 'no such book') => SXML_STATUS_DYNAMIC_ERROR err:BOOK0001 at 1:1: no such book
some $n in 1 to 100000000000 satisfies $n = 0 => SXML_STATUS_LIMIT_EXCEEDED: the evaluation took more steps than maximum_evaluation_step_count allows
```

The last expression would run for a very long time. The processor's
*limits* bound the work of an evaluation, the depth of calls, the size of
documents and the memory of the item heap, so a program can run queries
it doesn't trust. A limit gives `SXML_STATUS_LIMIT_EXCEEDED`, not an
error code: it is not an error in the query, but a decision of the host.

## Memory and allocators

sxml takes all its memory from an `sxml_allocator` that you give it: a
pair of functions to allocate and release, with a state pointer. The
default allocator uses the C library's heap. Yours can use a pool, an
arena, a fixed budget, or memory that a game engine or a kernel hands
out. The counting allocator that comes with sxml counts the allocations,
which makes it easy to check that nothing leaks:

`memory.c`:

```c
{{#include ../examples/c/memory.c}}
```

```console
$ ./build/memory
333333833333500000
allocations: 178, still live: 0
```

A million squares took 178 allocations, because sequences such as
`1 to 1000000` are computed as they are read, not stored.

Every object type in sxml can also be placed in memory you provide,
without an allocator call for the object itself: each has a
`memory_requirements` function that says how much memory and what
alignment it needs, an `initialize` function that places it, and a
`finalize` function that ends it. `create` and `destroy`, used above,
are the same with the memory from an allocator.

## Optional features

Some parts of sxml are *optional features*: the HTML parser, Invisible
XML, EXPath Binary and File, UCA collations, the time zone database,
localized formatting and Full Text. A program names the features it
wants in the processor configuration:

```c
sxml_optional_feature features[] = {
    sxml_html_parser_feature(),
    sxml_invisible_xml_feature(),
};
sxml_processor_configuration configuration = {
    .allocator = allocator,
    .optional_features = {.optional_features = features,
                          .optional_feature_count = 2},
};
```

A program that never names a feature doesn't link it, which keeps small
programs small. `sxml_all_optional_features()` names them all, except
EXPath File: a feature that reads and writes files beyond the resolver
must be named on purpose, with an `sxml_file_system` that decides which
files it reaches.

Without a feature, sxml behaves as the specifications say a processor
without it behaves: `parse-html` doesn't exist, for example. The large
tables some features need, such as collation data and time zones, are
not compiled in. The feature asks the resolver for them, in their
standard formats, the first time they are needed.

## Threads

A processor and everything made with it are used by one thread at a
time. Separate processors share nothing, so a server runs one processor
per thread, with no locks.

## Where to go next

The public headers in `include/sxml/` are the reference. Each one
documents its types and functions, with the rules of the specification
they follow. Start with `processor.h` and `xpath.h`, then `xquery.h`,
`xslt.h` and `schema.h`.
