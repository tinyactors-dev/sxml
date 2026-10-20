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

## Languages from tree-sitter's WebAssembly files

`sxml:parse-code` (see [Source Code as XML](source-code.md)) knows a
dozen languages. tree-sitter has grammars for hundreds more, and
`tree-sitter build --wasm` compiles one to a WebAssembly language file,
`tree-sitter-rust.wasm` for Rust; grammar repositories often ship that
file with their releases. `sxml.loadCodeLanguage` links such a file into
the running `sxml.wasm`, and `createProcessor({ codeLanguages })` gives
the languages to a processor:

```js
const sxml = await load();
const rust = await sxml.loadCodeLanguage({
  wasm: fetch('/grammars/tree-sitter-rust.wasm'),
  highlights: await (await fetch('/grammars/rust/highlights.scm')).text(),
});
const processor = sxml.createProcessor({ codeLanguages: [rust] });
processor.evaluate('sxml:parse-code("fn main() {}", "rust")//function_item/identifier ! string()');
```

- `wasm` is the file, as for `load({ wasm })`: a URL or path, the bytes,
  a `WebAssembly.Module` or a `fetch()` response.
- `highlights`, `locals` and `injections` are the texts of the grammar's
  `queries/highlights.scm`, `locals.scm` and `injections.scm`, and
  `injectionRegex` is the `injection-regex` of its `tree-sitter.json`.
  The package reads no files of its own: in Node.js read them with
  `readFile`, in a browser `fetch` them. Without queries, the language
  parses but has no highlights.
- `name` is the language's name in `sxml:parse-code`. By default it is
  the `NAME` of the file's function `tree_sitter_NAME`. A language with
  the name of a built-in one replaces it in the processors it is given to.
- The grammar must be generated for tree-sitter's language ABI 13 to 15
  (tree-sitter 0.20 and later). A file that is not a tree-sitter
  language file, one that needs other WebAssembly libraries, or one whose
  external scanner calls C functions outside tree-sitter's list for
  language files, is refused, and the error says why.
- A loaded language stays in memory as long as its `sxml`; it can be
  given to any number of processors.

To try it without a network, compile the JSON grammar from sxml's own
sources as `tree-sitter build --wasm` does, with the WASI SDK that also
builds `sxml.wasm`:

`code-language.mjs`:

```js
{{#include ../examples/javascript/code-language.mjs}}
```

```console
$ "$WASI_SDK_PREFIX/bin/clang" --target=wasm32-wasip1 -fPIC -shared -Os -nostdlib \
    -fvisibility=hidden -Wl,--no-entry -Wl,--allow-undefined -Wl,--export=tree_sitter_json \
    -o tree-sitter-json.wasm "$SXML/third_party/tree-sitter-grammars/json/src/parser.c"
$ node code-language.mjs
my-json 14
<document xmlns:sxml="urn:sxml"><array>[<number highlight="number">1</number>, <true highlight="boolean">true</true>]</array></document>
sxml: the highlights query does not fit the grammar at byte 1
```

## Memory

Processors, compiled expressions and values hold memory inside the
WebAssembly module. Call `dispose()` when you are done with one;
disposing a processor frees everything it made. Whatever is not disposed
is freed when JavaScript's garbage collector collects it.

The package's README, `bindings/javascript/README.md`, describes the
whole API.
