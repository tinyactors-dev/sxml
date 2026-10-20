# sxml in JavaScript

<!-- examples: javascript -->
<!-- requires: javascript -->

The `@tinyactors/sxml` package runs the sxml library in Node.js and in
browsers, compiled to WebAssembly, with a small TypeScript wrapper and
no dependencies. It speaks the same languages as the C library and the
`sxml` command, and gives the same results. The examples are in
`docs/book/examples/javascript`.

## Getting the package

The package is in the sxml repository, in `bindings/javascript`. It is
not on npm yet; its README explains how to build it from the
repository. It needs Node.js 22.18 or later, or any current browser.

It comes with one WebAssembly module, `sxml.wasm`, which has everything
the `sxml` command has but EXPath File: XML parsing, XPath 4.0 with the
whole function library, XQuery 4.0, XSLT 4.0, XSD 1.1, Full Text, HTML,
Invisible XML, EXPath Binary, collations, time zones, localization and
serialization. `load()` loads it from beside the package's JavaScript;
`load({ wasm })` loads it from elsewhere.

## Processors and values

As in C, everything starts with a processor. `parse` makes a document,
`compile` compiles an expression once, and `evaluate` runs it with a
context and variables. JavaScript values become XPath values: strings
are strings, numbers are doubles, objects are maps and arrays are
arrays.

`hello.mjs`:

```js
{{#include ../examples/javascript/hello.mjs}}
```

```console
$ node hello.mjs
before 1970: [ '"The Art of Computer Programming"', '"Dune"' ]
before 1966: [ '"Dune"' ]
[{"title":"Godel, Escher, Bach","year":1979},{"title":"The Art of Computer Programming","year":1968},{"title":"Dune","year":1965}]
```

`toString()` writes a value by the adaptive method, as the `sxml`
command does, and `serialize` takes the parameters of `fn:serialize`.

## XQuery, XSLT and XSD

`compileQuery`, `compileStylesheet` and `compileSchema` compile the
other languages, again once, to run many times:

`languages.mjs`:

```js
{{#include ../examples/javascript/languages.mjs}}
```

```console
$ node languages.mjs
["Godel, Escher, Bach"]
Godel, Escher, Bach (1979)
Dune (1965)
invalid [ 'cvc-datatype-valid.1.2.1' ]
```

`transform` also takes an initial mode or template, stylesheet
parameters, and returns the result documents of `xsl:result-document`.
An updating query hands each changed tree, and each document `fn:put`
stores, to callbacks you give it.

## Resources and errors

A processor's `resolver` answers every request for a resource. It gets
the kind of resource and its absolute URI, and returns a string, bytes,
or `undefined` when there is no such resource. In a browser, it can
answer from `fetch` results you loaded before, from IndexedDB, or from
memory. Failures throw an `SxmlError` with the error code and location:

`resources.mjs`:

```js
{{#include ../examples/javascript/resources.mjs}}
```

```console
$ node resources.mjs
7.7
err:FODC0002 1 1
```

## Memory

Processors, compiled expressions and values hold memory inside the
WebAssembly module. Call `dispose()` when you are done with one;
disposing a processor frees everything it made. Whatever is not disposed
is freed when JavaScript's garbage collector collects it.

The package's README, `bindings/javascript/README.md`, describes the
whole API.
