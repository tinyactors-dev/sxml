# sxml-js

sxml for JavaScript: XML parsing, XPath 4.0 with the F&O 4.0 function
library, and serialization, in Node.js and browsers. It is the sxml C
library compiled to WebAssembly (`sxml.wasm`, about 600 KB gzipped) with a
small TypeScript wrapper. It has no runtime dependencies.

```js
import { load } from 'sxml-js';

const sxml = await load();
const processor = sxml.createProcessor();

const book = processor.parse('<book><title>XPath</title><year>2026</year></book>');
const result = processor.evaluate('//title/string(), xs:integer(//year) + $n', {
  context: book,
  variables: { n: 1 },
});
result.toString(); // '"XPath"\n2.027e3', the adaptive method
```

## Loading

`load()` finds `sxml.wasm` next to the package's JavaScript. To load it from
elsewhere, pass `load({ wasm })` with a URL, a path, the bytes, a
`WebAssembly.Module` or a `fetch()` response. One loaded module serves any
number of processors.

## Optional features

Some parts of sxml are optional features: large tables and front ends that
not every page needs. `sxml.wasm`, the core module, has none of them.
`sxml-full.wasm`, next to it, has every one: load it with
`load({ wasm: new URL('sxml-full.wasm', import.meta.resolve('sxml-js')) })`
or from your own URL. `sxml.features` lists the features of the loaded
module, and `createProcessor({ features })` picks some of them (by default
all). Without a feature, sxml behaves as the specifications say a processor
without it does. Today the only feature is `test`, which adds nothing; the
features to come are listed in sxml's `docs/tasks/phase-7.md`.

## Processors and values

A processor holds names, types and values. Every value and expression
belongs to the processor that made it.

- `processor.parse(xml, { uri })` parses a document (a string, or bytes in
  any encoding the document declares) and returns its document node.
- `processor.compile(expression, { variables, namespaces, baseUri })`
  compiles an XPath 4.0 expression once. Its `evaluate({ context,
  variables })` can run many times.
- `processor.evaluate(expression, options)` compiles and evaluates once.
- `processor.value(data)` converts JavaScript data to a value.

Variable names are NCNames or `Q{uri}local`. A value to bind is either a
value from this processor or JavaScript data. A string becomes an
`xs:string`. Other data becomes what `fn:parse-json` makes of its JSON:
objects are maps, arrays are arrays, numbers are `xs:double`, and `null` is
the empty sequence.

## Output

`value.toString()` writes the value by the adaptive method of Serialization
4.0. `value.serialize(options)` takes the parameters of `fn:serialize`, and
keys may be in camelCase:

```js
document.serialize({ method: 'xml', indent: true, omitXmlDeclaration: false });
JSON.parse(processor.evaluate('map { "a": [1, 2] }').serialize({ method: 'json' }));
```

## Resources

`fn:doc`, `fn:unparsed-text` and the other resource functions ask the
processor's resolver. The resolver returns a string, bytes, or `undefined`
when the resource is not available. Nothing else is read.

```js
const processor = sxml.createProcessor({
  resolver: ({ kind, absoluteUri }) => files.get(absoluteUri),
});
processor.evaluate('doc("data.xml")//item', { baseUri: 'https://example.com/' });
```

The resolver is called synchronously during an evaluation. If it throws,
the evaluation stops and the same exception is rethrown.

## Errors

Failures throw an `SxmlError`. Its fields are `status`, for example
`'DYNAMIC_ERROR'`, and `code`, for example `'err:FOAR0001'`. It also has
`description`, `lineNumber` and `columnNumber`.

## Memory

Processors, expressions and values own memory inside the WebAssembly module.
Call `dispose()` when you are done with one. Anything not disposed is freed
when it is garbage collected. Disposing a processor also frees every value
and expression it made.

## Development

The package lives in `bindings/javascript/` of the sxml repository. ctest
builds and tests it under the `wasm` preset:

```sh
cmake --preset wasm && cmake --build --preset wasm && ctest --preset wasm -R javascript
npm ci --prefix bindings/javascript   # adds the javascript/typecheck test
```

`npm run build`, with `SXML_WASM_PATH` and `SXML_FULL_WASM_PATH` set to
the built `sxml.wasm` and `sxml-full.wasm`, writes `dist/`. The steps for a release are in `docs/RELEASE.md`.
