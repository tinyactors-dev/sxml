# @tinyactors/sxml

sxml for JavaScript, in Node.js and browsers. It is the sxml C library
compiled to WebAssembly, with a small TypeScript wrapper and no runtime
dependencies. It comes as two modules:

| Module | What it has | Size | Gzipped |
| --- | --- | --: | --: |
| `sxml.wasm` (core) | XML parsing, XPath 4.0 with the F&O 4.0 function library, Serialization 4.0 | 2.1 MB | 730 KB |
| `sxml-full.wasm` (full) | The core, plus XQuery 4.0 with the Update Facility 3.0, XQuery and XPath Full Text 3.0, XSLT 4.0, XSD 1.1 validation, and every optional feature except EXPath File | 7.6 MB | 1.7 MB |

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
- [Resources](#resources)
- [Errors](#errors)
- [Memory](#memory)
- [API summary](#api-summary)
- [Development](#development)

## Loading

`load()` loads `sxml.wasm`, next to the package's JavaScript. To load the
full module, or a module from elsewhere, pass `load({ wasm })` with a URL,
a path, the bytes, a `WebAssembly.Module` or a `fetch()` response:

```js
const sxml = await load({ wasm: new URL('sxml-full.wasm', import.meta.resolve('@tinyactors/sxml')) });
```

One loaded module serves any number of processors. `sxml.version` is the
library's version (`'0.2.0'`).

## Optional features

Some parts of sxml are optional features: large tables and functions that
not every page needs. The core module has none of them, and the full
module has every one except EXPath File, which reads files. `sxml.features`
lists the features of the loaded module, and `createProcessor({ features
})` picks some of them (by default all). Without a feature, sxml behaves
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
| `uca-collation` | UCA collation URIs (Unicode Collation Algorithm 16.0) over the DUCET, which the package ships; the CLDR root collation and the CLDR tailorings by language when the resolver gives their files (resource kind `'collation-data'`) |
| `timezone-database` | IANA time zone names in `fn:civil-timezone` and in the `$place` argument of `fn:format-dateTime` (resource kind `'timezone-data'`) |
| `localization` | Month and day names, calendars and number words in other languages in `fn:format-date`, `fn:format-integer` and `xsl:number` (resource kind `'locale-data'`) |
| `test` | Nothing: tests use it to check that a feature list reaches the processor |

`uca-collation`, `timezone-database` and `localization` read their
tables through the resolver (see [Resources](#resources)) when they are
first used. Without the tables they behave as the core module does.

In the core module, `compileQuery`, `compileStylesheet` and
`compileSchema` throw an error that says they need `sxml-full.wasm`.

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
processor.evaluate('bin:hex("CAFE") => bin:to-octets()').toString(); // '202\n254', with the full module
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

## XQuery

`processor.compileQuery(text, { namespaces, baseUri })` compiles an XQuery
4.0 main module once. Its `evaluate({ context, variables, updates,
messages })` returns a value, `serialize(value)` serializes a value with
the serialization parameters of the query's output declarations, and
`run(options)` does both. The variables are the external variables of the
prolog. A library module that the query imports is read through the
resolver, with kind `'xquery-module'`.

```js
const sxml = await load({ wasm: new URL('sxml-full.wasm', import.meta.resolve('@tinyactors/sxml')) });
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
the evaluation stops and the same exception is rethrown.

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
| `Sxml.createProcessor({ resolver, features })` | A new `Processor` |
| `Processor.parse(xml, { uri })` | A document node |
| `Processor.compile(expression, options)` | An `XPathExpression`, with `evaluate(options)` and `variables` |
| `Processor.evaluate(expression, options)` | Compiles and evaluates an XPath expression once |
| `Processor.compileQuery(text, options)` | An `XQuery`, with `evaluate`, `serialize`, `run` and `isUpdating` (full module) |
| `Processor.compileStylesheet(text, options)` | A `Stylesheet`, with `transform` (full module) |
| `Processor.compileSchema(documents, options)` | A `Schema`, with `validate` (full module) |
| `Processor.value(data)` | JavaScript data as a value |
| `Processor.serialize(value, options)` | The same as `value.serialize(options)` |
| `Value.toString()`, `Value.serialize(options)` | The value's text |
| `dispose()` | On each object: frees its memory |
| `SxmlError`, `SchemaError` | The errors that the package throws |

## Development

The package lives in `bindings/javascript/` of the sxml repository. ctest
builds and tests it under the `wasm` preset:

```sh
cmake --preset wasm && cmake --build --preset wasm && ctest --preset wasm -R javascript
# javascript/languages_full runs test/full.test.ts on sxml-full.wasm
npm ci --prefix bindings/javascript   # adds the javascript/typecheck test
```

`npm run build`, with `SXML_WASM_PATH` and `SXML_FULL_WASM_PATH` set to
the built `sxml.wasm` and `sxml-full.wasm`, writes `dist/` with both
modules and `allkeys.txt`. The steps for a release are in
`docs/RELEASE.md`.
