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
context and variables. The examples read the Vane draft of [Finding
Things with XPath](xpath.md) and the traces of [Project: Comparing
Traces Before and After a Change](traces.md); the example directory
links to both files.

`hello.mjs`:

```js
{{#include ../examples/javascript/hello.mjs}}
```

```console
$ node hello.mjs
MUST: [ '"req-1"', '"req-2"', '"req-4"', '"req-6"', '"req-7"' ]
MAY: [ '"req-9"', '"req-11"' ]
[{"keyword":"MUST","count":5},{"keyword":"SHOULD","count":3},{"keyword":"MUST NOT","count":1},{"keyword":"MAY","count":2},{"keyword":"SHOULD NOT","count":1}]
```

`toString()` writes a value by the adaptive method, as the `sxml`
command does, and `serialize` takes the parameters of `fn:serialize`.

## XQuery, XSLT and XSD

`compileQuery`, `compileStylesheet` and `compileSchema` compile the
other languages, again once, to run many times. JavaScript values
become XPath values: strings are strings, numbers are doubles, objects
are maps and arrays are arrays. So a query can take a whole OTLP file,
parsed by `JSON.parse`, as a variable:

`languages.mjs`:

```js
{{#include ../examples/javascript/languages.mjs}}
```

```console
$ node languages.mjs
[{"service":"storefront","spans":1},{"service":"vane","spans":12},{"service":"vane-console","spans":1}]
Flag States: 2 MUST
Rollout: 1 MUST
Status: 1 MUST
Pause: 1 MUST
invalid [ 'cvc-enumeration-valid' ]
```

The Vane service recorded twelve of the fourteen spans; the backend SDK
in the storefront and the console recorded one each, their calls to it.

`transform` also takes an initial mode or template, stylesheet
parameters, and returns the result documents of `xsl:result-document`.
An updating query hands each changed tree, and each document `fn:put`
stores, to callbacks you give it.

## Resources and errors

A processor's `resolver` answers every request for a resource. It gets
the kind of resource and its absolute URI, and returns a string, bytes,
or `undefined` when there is no such resource. `json-doc` and
`unparsed-text` ask for the kind `text`. In a browser, the resolver can
answer from `fetch` results you loaded before, from IndexedDB, or from
memory. Failures throw an `SxmlError` with the error code and location:

`resources.mjs`:

```js
{{#include ../examples/javascript/resources.mjs}}
```

```console
$ node resources.mjs
[ '"storefront"', '"vane"', '"vane-console"' ]
err:FOUT1170 1 1
```

## Languages from tree-sitter's WebAssembly files

`tree-sitter:parse` (see [Source Code as XML](source-code.md)) knows a
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
processor.evaluate('tree-sitter:parse("fn main() {}", "rust")//function_item/identifier ! string()');
```

- `wasm` is the file, as for `load({ wasm })`: a URL or path, the bytes,
  a `WebAssembly.Module` or a `fetch()` response.
- `highlights`, `locals` and `injections` are the texts of the grammar's
  `queries/highlights.scm`, `locals.scm` and `injections.scm`, and
  `injectionRegex` is the `injection-regex` of its `tree-sitter.json`.
  The package reads no files of its own: in Node.js read them with
  `readFile`, in a browser `fetch` them. Without queries, the language
  parses but has no highlights.
- `name` is the language's name in `tree-sitter:parse`. By default it is
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

## Large outputs and inputs

The methods that return a string hold the whole output twice: once in
the module's memory, which never shrinks again, and once as the string.
For a large output, give a sink instead. `value.serializeTo(sink)`,
`query.runTo(sink)` and `transform({ output })` write the output a chunk
at a time, 64 KiB by default (`createProcessor({ outputChunkByteCount
})`). A sink is a function that gets each chunk as a `Uint8Array`, or an
object with `write`, and optionally `close` and `abort`. Each chunk is
copied out of the module, which reuses its buffer for the next one.
`transform({ resultDocumentOutput })` chooses a sink for each
`xsl:result-document` by its URI.

On the input side, a resolver may answer an XML document with an
iterable of chunks, such as a generator. sxml reads it as it parses.
When a stylesheet's mode is streamable, `transform({ streamedSource })`
reads the principal source this way and serializes the result as it is
made, so neither the source nor the result is ever whole in memory.
Here a generator makes an archive of 200,000 traces in the normal form
of [Spans as a Tree](traces-tree.md), and a streamable mode picks the
slow requests out of it, as in [Streaming Large Trace
Files](traces-streaming.md):

`streaming.mjs`:

```js
{{#include ../examples/javascript/streaming.mjs}}
```

```console
$ node streaming.mjs
813465 bytes; 10 MiB of module memory
[ '≥ ', '70 m', 's: 1', '2 ', '€' ]
```

The evaluation runs synchronously inside WebAssembly, so a sink cannot
make it wait. A sink that consumes each chunk at once, like `writeSync`
to a file, a hash, or a parser fed by a decoder, keeps memory bounded by
one chunk. Sinks that queue chunks hold them in JavaScript memory until
the call returns. That includes `writableStreamSink` for a WHATWG
`WritableStream`, `nodeStreamSink` for a Node.js stream and
`outputReadableStream`. They still save the copy in the module's memory
and the string. `textSink` decodes UTF-8 in TextDecoder's stream mode,
so a character split between two chunks comes out whole.

A long stream takes many evaluation steps. Every evaluation stops after
a hundred million of them ("The step limit" in "Performance and
Memory"), but a streamed document grants 500 more for each element it
reads, so a streamed transformation of a long input runs within the
bound, while a loop that reads nothing more still stops. For other work
that needs more, or to keep untrusted queries shorter, set the bound
with `createProcessor({ maximumEvaluationStepCount })`.

## Memory

Processors, compiled expressions and values hold memory inside the
WebAssembly module. Call `dispose()` when you are done with one;
disposing a processor frees everything it made. Whatever is not disposed
is freed when JavaScript's garbage collector collects it.

The package's README, `bindings/javascript/README.md`, describes the
whole API.
