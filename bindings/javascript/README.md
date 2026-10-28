# @tinyactors/sxml

sxml for JavaScript, in Node.js and browsers. It is the sxml C library
compiled to WebAssembly, with a small TypeScript wrapper and no runtime
dependencies. It is one module, `sxml.wasm` (7.7 MB, 1.7 MB gzipped),
with everything sxml has: XML parsing, XPath 4.0 with the F&O 4.0 function
library, XQuery 4.0 with the Update Facility 3.0, XQuery and XPath Full
Text 3.0, XSLT 4.0, XSD 1.1 validation, Serialization 4.0, and every
optional feature except EXPath File, which reads files.

```js
import { load } from '@tinyactors/sxml';

const sxml = await load();
const processor = sxml.createProcessor();

const book = processor.parse('<book><title>XPath</title><year>2026</year></book>');
const result = processor.evaluate('//title/string(), xs:integer(//year) + $n', {
  context: book,
  variables: { n: 1 },
});
result.toString(); // '"XPath"\n2.027e3', the adaptive method
```

- [Loading](#loading)
- [Optional features](#optional-features)
- [Processors and values](#processors-and-values)
- [Output](#output)
- [XQuery](#xquery)
- [XSLT](#xslt)
- [XSD validation](#xsd-validation)
- [Code languages](#code-languages)
- [Resources](#resources)
- [Errors](#errors)
- [Memory](#memory)
- [API summary](#api-summary)
- [Development](#development)

## Loading

`load()` loads `sxml.wasm`, next to the package's JavaScript. To load it
from elsewhere, pass `load({ wasm })` with a URL, a path, the bytes, a
`WebAssembly.Module` or a `fetch()` response:

```js
const sxml = await load({ wasm: await fetch('/assets/sxml.wasm') });
```

The package exports the module as `@tinyactors/sxml/sxml.wasm`;
`@tinyactors/sxml/sxml-full.wasm`, the name of the full module of earlier
builds, is the same file.

One loaded module serves any number of processors. `sxml.version` is the
library's version (`'0.2.0'`).

## Optional features

Some parts of sxml are optional features: large tables and functions that
a processor may go without. The module has every one except EXPath File,
which reads files. `sxml.features` lists them, and `createProcessor({
features })` picks some of them (by default all). Without a feature, sxml behaves
as the specifications say a processor without it does: its functions do
not exist (`err:XPST0017`), or the specification's fallback applies.

| Feature | What it adds |
| --- | --- |
| `html` | `fn:parse-html` and `fn:html-doc`: HTML parsed as the HTML Living Standard says, in every encoding of the Encoding Standard |
| `invisible-xml` | `fn:invisible-xml`: Invisible XML grammars that turn text into XML |
| `xslt-transform` | `fn:transform`: run an XSLT stylesheet from XPath or XQuery |
| `expath-binary` | The EXPath Binary 4.0 functions, namespace `http://expath.org/ns/binary`, bound to the prefix `bin` |
| `full-text` | XQuery and XPath Full Text 3.0: `contains text` and score variables in XPath and XQuery, with stemming, stop words and thesauri |
| `dtd-validation` | The `dtd-validation` option of `fn:doc` and `fn:parse-xml` |
| `builtin-schemas` | The schemas that `fn:json-to-xml` with `validate`, the typed results of `fn:analyze-string`, and lax validation without an in-scope schema use |
| `source-code` | `tree-sitter:parse` and `tree-sitter:languages` (namespace `urn:sxml:tree-sitter`, prefix `tree-sitter`): source code in JSON, JavaScript, TypeScript, TSX, Python, C, CSS, HTML, Go or Markdown as a document of its syntax tree, with highlights and code inside code, and any other tree-sitter language from its WebAssembly file (see [Code languages](#code-languages)) |
| `uca-collation` | UCA collation URIs (Unicode Collation Algorithm 16.0) over the DUCET, which the package ships; the CLDR root collation and the CLDR tailorings by language when the resolver gives their files (resource kind `'collation-data'`) |
| `timezone-database` | IANA time zone names in `fn:civil-timezone` and in the `$place` argument of `fn:format-dateTime` (resource kind `'timezone-data'`) |
| `localization` | Month and day names, calendars and number words in other languages in `fn:format-date`, `fn:format-integer` and `xsl:number` (resource kind `'locale-data'`) |
| `test` | Nothing: tests use it to check that a feature list reaches the processor |

`uca-collation`, `timezone-database` and `localization` read their
tables through the resolver (see [Resources](#resources)) when they are
first used. Without the tables they behave as a processor without the
feature does.

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

`namespaces` binds prefixes for the expression, besides the predeclared
ones (`xs`, `fn`, `map`, `array`, `math`, `err`, ...). A processor with
the `expath-binary` feature also binds `bin`, as the `sxml` command does;
`namespaces` may rebind it:

```js
processor.evaluate('bin:hex("CAFE") => bin:to-octets()').toString(); // '202\n254'
processor.evaluate('//p:item', { context: document, namespaces: { p: 'urn:p' } });
```

## Output

`value.toString()` writes the value by the adaptive method of Serialization
4.0. `value.serialize(options)` takes the parameters of `fn:serialize`, and
keys may be in camelCase:

```js
book.serialize({ method: 'xml', indent: true, omitXmlDeclaration: false });
processor.evaluate('map { "a": [1, 2] }').serialize({ method: 'json' }); // '{"a":[1,2]}'
```

## Streaming

The methods that return a string hold the whole output twice: in the
module's memory, which grows to fit it and never shrinks, and as the
string. For large outputs, write to a sink instead (D294):

| Method | Writes |
| --- | --- |
| `value.serializeTo(sink, options)`, `processor.serializeTo(value, sink, options)` | What `serialize(options)` returns |
| `query.runTo(sink, options)`, `query.serializeTo(value, sink)` | What `run` and `serialize` return |
| `transform({ output: sink })` | The principal result, with delivery `'serialized'`; `output` is then undefined |
| `transform({ resultDocumentOutput: (uri) => sink })` | Each serialized `xsl:result-document`; `undefined` gathers it in `resultDocuments` as before |
| `putDocument.serializeTo(sink)` | What `putDocument.serialize()` returns |

A sink is a function `(chunk: Uint8Array) => void`, or an object with
`write(chunk)` and, optionally, `close()` (after the output is complete)
and `abort(reason)` (when the call fails, or a result document is
abandoned). The chunks are 64 KiB by default; set
`createProcessor({ outputChunkByteCount })` to change that. Each chunk is
the sink's own copy, and the buffer in the module is reused for the next
one. A sink must not use the processor. If a sink throws, the call stops
and rethrows the same exception.

```js
import { writeSync } from 'node:fs';
query.runTo((chunk) => writeSync(fd, chunk));
```

Chunks are cut at byte counts, not at characters. `textSink(onText)`
decodes UTF-8 with `TextDecoder` in stream mode, so a character whose
bytes two chunks share is given to `onText` once both have arrived. Use
it, or your own streaming decoder, rather than decoding each chunk alone.

**Backpressure.** An evaluation runs synchronously inside WebAssembly,
so a sink is called during the call and cannot make the evaluation wait.
A sink that consumes each chunk at once keeps memory bounded by one
chunk: `fs.writeSync`, a hash, a decoder feeding a parser, or
`postMessage` from a worker to a thread that reads concurrently. The
adapters `writableStreamSink(writableStream)` (await its `done`),
`nodeStreamSink(writable)` and `outputReadableStream((sink) => ...)`
queue the chunks in JavaScript memory until the call returns and the
event loop drains them. They still save the copy in the module's memory
and the string.

**Input.** A resolver may answer `'xml-document'` with a `ChunkSource`:
an iterable of `Uint8Array` or string chunks, such as a generator that
reads a file with `fs.readSync`. sxml reads it as it parses, and closes
it (`return()`) when done. A document that XSLT streams is never whole in
memory: `xsl:source-document streamable="yes"`, or the principal source
that `transform({ streamedSource: uri })` names, when the initial mode is
streamable. `fn:doc` and other uses read the chunks whole into the
module, but not into JavaScript. For the other kinds, the package joins
the chunks first. `Processor.parse` takes the whole text: the C API has
no public incremental parse (D233), so for a large document, evaluate
`doc($uri)` with a chunk source.

Measured with Node.js 22, for output of 108.7 MB (1.8 million elements;
module memory, peak JavaScript heap):

| Case | String API | Sink |
| --- | --- | --- |
| `value.serialize` / `serializeTo` (tree of 1.8 M elements) | 749 MB, 113 MB | 481 MB, 4 MB |
| `query.run` / `runTo` | 749 MB, 113 MB | 481 MB, 4 MB |
| `transform` from a template | 749 MB, 113 MB | 481 MB, 4 MB |
| `transform` with `streamedSource` (95 MB source) | 387 MB, 113 MB (source as a string) | 11 MB, 15 MB (chunk source) |

## XQuery

`processor.compileQuery(text, { namespaces, baseUri })` compiles an XQuery
4.0 main module once. Its `evaluate({ context, variables, updates,
messages })` returns a value, `serialize(value)` serializes a value with
the serialization parameters of the query's output declarations, and
`run(options)` does both. The variables are the external variables of the
prolog. A library module that the query imports is read through the
resolver, with kind `'xquery-module'`.

```js
const sxml = await load();
const processor = sxml.createProcessor();

const books = processor.parse(`<books>
  <book year="1999"><title>XSLT</title></book>
  <book year="2026"><title>XQuery 4.0</title></book>
</books>`);
const query = processor.compileQuery(`
  declare option output:method "json";
  declare variable $min external := 0;
  array { //book[@year >= $min] ! map { "title": string(title), "year": xs:integer(@year) } }`);
query.run({ context: books, variables: { min: 2000 } });
// '[{"title":"XQuery 4.0","year":2026}]'
```

Full Text searches text with `contains text`:

```js
processor.compileQuery('"Parsing large files" contains text "parse" using stemming').run();
// 'true'
```

An updating query (the XQuery Update Facility 3.0) returns the empty
sequence, and `query.isUpdating` is `true`. sxml never changes a tree:
each tree that the query changes is built again as a new tree, and it
goes to `updates.updatedTree` with the original root. The documents that
`fn:put` stores go to `updates.putDocument`, with their URI and a
`serialize()`. Without a `putDocument`, `fn:put` raises `FOUP0002`.

```js
const update = processor.compileQuery('insert node <book year="2027"><title>Next</title></book> into /books');
update.evaluate({
  context: books,
  updates: {
    updatedTree: ({ original, updated }) => {
      console.log(processor.evaluate('count(//book)', { context: updated }).toString());
      // 3: the new tree; original, the books document, still has 2
    },
  },
});
```

`fn:trace` and `fn:message` output goes to `messages`, a function that
receives each message after the evaluation, also when the evaluation
failed.

## XSLT

`processor.compileStylesheet(text, { baseUri, staticParameters })` compiles
an XSLT 4.0 stylesheet once. The modules it includes and imports are read
through the resolver, with kind `'xslt-module'`, and the packages it uses
with kind `'xslt-package'`. Its `transform(options)` returns `{ output,
value, resultDocuments }`:

- `source` is the global context item. Templates are applied to it, in
  `initialMode` (`'#unnamed'` selects the unnamed mode) or in the default
  mode. To apply templates to something else, give `initialMatchSelection`.
- `initialTemplate` calls a named template instead. Without a source,
  `xsl:initial-template` is called. `initialFunction` with `args` calls a
  public stylesheet function.
- `parameters`, `templateParameters` and `tunnelParameters` bind the
  parameters by name.
- `delivery` says how the principal result is delivered. With
  `'serialized'` (the default), `output` is the result as the unnamed
  `xsl:output` serializes it. With `'raw'`, `value` is the result as the
  stylesheet made it, and with `'tree'` it is one document node.
  `serialization` replaces the parameters of the `xsl:output`.
- `resultDocuments` has each `xsl:result-document`, with its URI resolved
  against `baseOutputUri`. `resultDocumentDelivery` says how they are
  delivered, in the same way as `delivery`.
- `messages` receives the `xsl:message` and `xsl:assert` output.
- `output` and `resultDocumentOutput` write the serialized results to
  sinks a chunk at a time, and `streamedSource` names a principal source
  to stream (see Streaming).

```js
const stylesheet = processor.compileStylesheet(`
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" html-version="5"/>
  <xsl:param name="heading" select="'Books'"/>
  <xsl:template match="/books">
    <h1><xsl:value-of select="$heading"/></h1>
    <xsl:for-each select="book">
      <xsl:result-document href="book{position()}.txt" method="text">
        <xsl:value-of select="title"/>
      </xsl:result-document>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>`);
const result = stylesheet.transform({
  source: books,
  parameters: { heading: 'Library' },
  baseOutputUri: 'https://example.com/out/',
});
result.output; // '<h1>Library</h1>'
result.resultDocuments.map(({ uri, output }) => `${uri}: ${output}`);
// ['https://example.com/out/book1.txt: XSLT', 'https://example.com/out/book2.txt: XQuery 4.0']
```

A stylesheet with `version="1.0"` runs with XSLT 1.0 behaviour, and
`exsl:node-set`, `exsl:object-type` and `exsl:document` of EXSLT Common
are available.

## XSD validation

`processor.compileSchema(documents, { baseUri })` compiles an XSD 1.1 schema
from one or more schema documents. A document is its text, or `{ text,
uri }`, or `{ uri }` alone, which the resolver is asked for. The documents
that a schema document includes or imports are read through the resolver,
with kind `'schema-document'`. If the schema has errors, a `SchemaError` is
thrown, and its `reports` has every error.

`schema.validate(input, { mode, type, uri })` validates XML text, or a
document or element node. Text is parsed and validated in one pass, so
each report has a line and a column. The result is `{ validity, reports,
document }`. `validity` is `'valid'`, `'invalid'` or `'not-known'` (lax
validation of an element that has no declaration). An invalid document is
a result, not an exception. Unless the document is invalid, `document` is
the validated copy, and its elements and attributes have the types of the
schema. `type` validates the root against a named type of the schema.

```js
const schema = processor.compileSchema(`
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="book">
    <xs:complexType>
      <xs:sequence><xs:element name="title" type="xs:string"/></xs:sequence>
      <xs:attribute name="year" type="xs:gYear" use="required"/>
    </xs:complexType>
  </xs:element>
</xs:schema>`);
const report = schema.validate('<book year="soon">\n  <author/>\n</book>', { uri: 'book.xml' });
report.validity; // 'invalid'
report.reports.map(({ lineNumber, columnNumber, code }) => `${lineNumber}:${columnNumber} ${code}`);
// ['1:7 cvc-datatype-valid.1.2.1', '2:3 cvc-complex-type.2.4']

const valid = schema.validate('<book year="2026"><title>XQuery 4.0</title></book>');
processor.evaluate('/book/@year instance of attribute(*, xs:gYear)', { context: valid.document }).toString();
// 'true()'
```

## Code languages

`tree-sitter:parse` (the `source-code` feature; the prefix `tree-sitter`
is bound to `urn:sxml:tree-sitter`, and `sxml`, of the `sxml:token`
elements, to `urn:sxml`) parses a dozen languages built in. Any other tree-sitter
grammar can be loaded from its WebAssembly language file,
`tree-sitter-NAME.wasm`, as `tree-sitter build --wasm` writes it (many
grammar repositories ship it with their releases), together with the
texts of its queries:

```js
const rust = await sxml.loadCodeLanguage({
  wasm: await fetch('/grammars/tree-sitter-rust.wasm'),
  highlights: await (await fetch('/grammars/rust/highlights.scm')).text(),
  // locals, injections and injectionRegex are optional, as is name
});
rust.name; // 'rust', from the file's tree_sitter_rust()
const processor = sxml.createProcessor({ codeLanguages: [rust] });
processor.evaluate('tree-sitter:parse("fn main() {}", "rust")//function_item/identifier ! string()');
```

`loadCodeLanguage` links the file into the running `sxml.wasm`: the
file is a WebAssembly side module (its `dylink.0` section), which shares
the module's memory, function table, stack and C library, so sxml's
tree-sitter runs its lexer and external scanner directly. It then checks
the grammar's ABI (13 to 15, tree-sitter 0.20 and later) and that each
query fits the grammar, and rejects with an `Error` that says what is
wrong: a file without `dylink.0`, one that needs other WebAssembly
libraries, one whose scanner imports a C function outside tree-sitter's
list for language files (`malloc`, `calloc`, `realloc`, `free`, the
`mem*` and `str*` functions, `isw*`, `towlower`, `towupper`; `abort` and
`__assert_fail` trap), another ABI, a query that does not fit (with its
byte offset), or a bad name. A loaded language stays in memory as long
as its `Sxml` and can be given to any number of its processors; one with
the name of a built-in language replaces it there.

## Resources

`fn:doc`, `fn:unparsed-text` and the other resource functions ask the
processor's resolver. The resolver gets `{ kind, uriReference,
absoluteUri }` and returns a string, bytes, or `undefined` when the
resource is not available. Nothing else is read.

```js
const processor = sxml.createProcessor({
  resolver: ({ kind, absoluteUri }) => files.get(absoluteUri),
});
processor.evaluate('doc("data.xml")//item', { baseUri: 'https://example.com/' });
```

The kinds are `'xml-document'`, `'text'`, `'binary'`, `'collection'`,
`'uri-collection'`, `'external-entity'`, `'xquery-module'`,
`'xslt-module'`, `'xslt-package'` and `'schema-document'`, and three for
the tables of the optional features:

- `'timezone-data'`: a file of the IANA Time Zone Database. `absoluteUri`
  is `urn:sxml:timezone-data:` followed by a zoneinfo path, such as
  `Europe/Paris` (a TZif file) or `zone1970.tab`. Answer with the file's
  bytes.
- `'collation-data'`: the collation table. Answer with the text of the
  file that `absoluteUri` names, or `undefined`. When the resolver has no
  answer, the package uses the DUCET (`allkeys.txt` of the Unicode
  Collation Algorithm 16.0.0) that it ships next to the modules; to load
  it from elsewhere, pass `load({ collationData })` with a URL, a path or
  the bytes, or `false` for none.
- `'locale-data'`: a file of the Unicode CLDR. `absoluteUri` is
  `urn:sxml:locale-data:` followed by a path below CLDR's `common/`
  directory, such as `main/de.xml` or `supplemental/supplementalData.xml`.
  Answer with the file's bytes. Without it, dates are formatted in
  English after `[Language: en]`.

The resolver is called synchronously during an evaluation. If it throws,
the evaluation stops and the same exception is rethrown. For a large XML
document, it may answer with an iterable of chunks (see Streaming).

## Errors

Failures throw an `SxmlError`. Its fields are `status`, for example
`'DYNAMIC_ERROR'`, and `code`, for example `'err:FOAR0001'`, also as
`codeNamespaceUri` and `codeLocalName`. It also has `description`,
`sourceName` (`'expression'`, `'query'`, or the URI of a module or
stylesheet), `lineNumber` and `columnNumber`.

```js
try {
  processor.evaluate('1 div 0');
} catch (error) {
  error.code; // 'err:FOAR0001'
}
```

## The step limit

An evaluation stops with an error after a hundred million evaluation
steps, a few seconds of work, so a query or stylesheet that would loop
for hours does not hang the page or the server. Set
`createProcessor({ maximumEvaluationStepCount })` to a whole number to
raise or lower it (the C processor limit of the same name; 0 is the
default). A streamed document grants 500 steps more for each element it
reads, so a streamed transformation of a long input runs within the
default bound, while a loop that reads no more input still stops (D295).

```js
const processor = sxml.createProcessor({ maximumEvaluationStepCount: 1_000_000_000 });
```

## Source locations

`fn:location` gives the line and column of a node of a parsed document
when the document kept them. `createProcessor({ locations })` decides
which do: `'automatic'` (the default) keeps them for a document parsed
before any program is compiled with the processor, and for the
documents of a program that can ask for them (it calls `fn:location`,
can look up functions with `fn:function-lookup`, `fn:transform` or
`xsl:evaluate`, or validates); `'always'` keeps them for every document;
`'never'` for none. They cost 32 bytes a node (D313, D318).

```js
const processor = sxml.createProcessor({ locations: 'always' });
const book = processor.parse('<book>\n  <title>XPath</title>\n</book>');
processor.evaluate('location(//title)?line-number', { context: book }).toString(); // '2'
```

## Memory

Processors, expressions, queries, stylesheets, schemas and values own
memory inside the WebAssembly module. Call `dispose()` when you are done
with one. Anything not disposed is freed when it is garbage collected.
Disposing a processor also frees everything it made. The values that a
query or a transformation gives you (results, updated trees, result
documents) are yours to dispose.

## API summary

| Name | Description |
| --- | --- |
| `load({ wasm, collationData })` | Loads a module; returns an `Sxml` |
| `Sxml.version`, `Sxml.features` | The library version and the module's optional features |
| `Sxml.createProcessor({ resolver, features, codeLanguages, outputChunkByteCount, maximumEvaluationStepCount, locations })` | A new `Processor` |
| `Sxml.loadCodeLanguage({ wasm, name, highlights, locals, injections, injectionRegex })` | A `CodeLanguage` (`name`, `abiVersion`) from a tree-sitter WebAssembly language file |
| `Processor.parse(xml, { uri })` | A document node |
| `Processor.compile(expression, options)` | An `XPathExpression`, with `evaluate(options)` and `variables` |
| `Processor.evaluate(expression, options)` | Compiles and evaluates an XPath expression once |
| `Processor.compileQuery(text, options)` | An `XQuery`, with `evaluate`, `serialize`, `run` and `isUpdating` |
| `Processor.compileStylesheet(text, options)` | A `Stylesheet`, with `transform` |
| `Processor.compileSchema(documents, options)` | A `Schema`, with `validate` |
| `Processor.value(data)` | JavaScript data as a value |
| `Processor.serialize(value, options)` | The same as `value.serialize(options)` |
| `Value.toString()`, `Value.serialize(options)` | The value's text |
| `Value.serializeTo(sink, options)`, `Processor.serializeTo`, `XQuery.runTo`, `XQuery.serializeTo` | The same, written to an `OutputSink` a chunk at a time |
| `textSink`, `writableStreamSink`, `nodeStreamSink`, `outputReadableStream` | Sinks that decode text, or write to WHATWG and Node.js streams |
| `dispose()` | On each object: frees its memory |
| `SxmlError`, `SchemaError` | The errors that the package throws |

## Development

The package lives in `bindings/javascript/` of the sxml repository. ctest
builds and tests it under the `wasm` preset:

```sh
cmake --preset wasm && cmake --build --preset wasm && ctest --preset wasm -R javascript
# javascript/languages runs test/full.test.ts: XQuery, XSLT and XSD
npm ci --prefix bindings/javascript   # adds the javascript/typecheck test
```

`npm run build`, with `SXML_WASM_PATH` set to the built `sxml.wasm`,
writes `dist/` with the module and `allkeys.txt`. The steps for a release are in
`docs/RELEASE.md`.
