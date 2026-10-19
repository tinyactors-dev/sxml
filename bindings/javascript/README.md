# sxml-js

sxml for JavaScript: XML parsing, XPath 4.0 with the F&O 4.0 function
library, and serialization, in Node.js and browsers; with the full module,
also XQuery 4.0 with the Update Facility, XSLT 4.0 and XSD 1.1 validation.
It is the sxml C library compiled to WebAssembly (`sxml.wasm`, about 730 KB
gzipped; `sxml-full.wasm`, about 1.3 MB) with a small TypeScript wrapper.
It has no runtime dependencies.

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
without it does. The `test` feature adds nothing; the others, such as
`html`, `xslt-transform` and `uca-collation`, are described in sxml's
`docs/tasks/phase-7.md`.

The full module also has XQuery, XSLT and XSD validation (below). In the
core module, `compileQuery`, `compileStylesheet` and `compileSchema` throw
an error that says they need `sxml-full.wasm`.

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

## XQuery

`processor.compileQuery(text, { namespaces, baseUri })` compiles an XQuery
4.0 main module once. Its `evaluate({ context, variables })` returns a
value, and `run()` evaluates and serializes the result with the
serialization parameters of the query's output declarations. The
variables are the external variables of the prolog. A library module that
the query imports is read through the resolver, with kind
`'xquery-module'`.

```js
const sxml = await load({ wasm: new URL('sxml-full.wasm', import.meta.resolve('sxml-js')) });
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

An updating query (the XQuery Update Facility 3.0) returns the empty
sequence. sxml never changes a tree: each tree that the query changes is
built again as a new tree, and it goes to `updates.updatedTree` with the
original root. The documents that `fn:put` stores go to
`updates.putDocument`, with their URI and a `serialize()`. Without a
`putDocument`, `fn:put` raises `FOUP0002`.

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
receives each message after the evaluation.

## XSLT

`processor.compileStylesheet(text, { baseUri, staticParameters })` compiles
an XSLT 4.0 stylesheet once. The modules it includes and imports are read
through the resolver, with kind `'xslt-module'`. Its `transform(options)`
returns `{ output, value, resultDocuments }`:

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
`description`, `sourceName` (`'expression'`, `'query'`, or the URI of a
module or stylesheet), `lineNumber` and `columnNumber`.

## Memory

Processors, expressions, queries, stylesheets, schemas and values own
memory inside the WebAssembly module. Call `dispose()` when you are done
with one. Anything not disposed is freed when it is garbage collected.
Disposing a processor also frees everything it made. The values that a
query or a transformation gives you (results, updated trees, result
documents) are yours to dispose.

## Development

The package lives in `bindings/javascript/` of the sxml repository. ctest
builds and tests it under the `wasm` preset:

```sh
cmake --preset wasm && cmake --build --preset wasm && ctest --preset wasm -R javascript
# javascript/languages_full runs test/full.test.ts on sxml-full.wasm
npm ci --prefix bindings/javascript   # adds the javascript/typecheck test
```

`npm run build`, with `SXML_WASM_PATH` and `SXML_FULL_WASM_PATH` set to
the built `sxml.wasm` and `sxml-full.wasm`, writes `dist/`. The steps for a release are in `docs/RELEASE.md`.
