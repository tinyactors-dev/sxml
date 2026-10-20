# Sxml for Swift

sxml for Swift: the module `Sxml`, a Swift package over the sxml C library.
It has XML parsing, XPath 4.0 with the F&O 4.0 function library,
Serialization 4.0, XQuery 4.0 with the Update Facility 3.0 and Full Text
3.0, XSLT 4.0, XSD 1.1 validation, and every optional feature except
EXPath File. It is the API of the JavaScript package
([`bindings/javascript`](../javascript/README.md)) in Swift's own idiom:
named arguments with defaults, `throws`, and ARC.

```swift
import Sxml

let processor = try Processor()
let book = try processor.parse("<book><title>XPath</title><year>2026</year></book>")
let expression = try processor.compile("//title/string(), xs:integer(//year) + $n", variables: ["n"])
let result = try expression.evaluate(context: book, variables: ["n": 1])
print(result)
// "XPath"
// 2.027e3
```

- [Getting the package](#getting-the-package)
- [Optional features](#optional-features)
- [Processors and values](#processors-and-values)
- [Output](#output)
- [XQuery](#xquery)
- [XSLT](#xslt)
- [XSD validation](#xsd-validation)
- [Resources](#resources)
- [Errors](#errors)
- [Memory and threads](#memory-and-threads)
- [API summary](#api-summary)
- [Development](#development)

## Getting the package

`Package.swift` is at the root of the sxml repository, and the product is
the library `Sxml`. The package uses Swift 6 (tools version 6.0, Swift 6
language mode).

- **Linux**: the package links `libsxml_host.a`, which the sxml CMake build
  makes. `SXML_HOST_LIBRARY_PATH` names its directory. Because the
  manifest then uses unsafe flags, build in the sxml repository itself, as
  the root package:

  ```sh
  cmake --preset release && cmake --build --preset release --target sxml_host_swift_library
  SXML_HOST_LIBRARY_PATH=$PWD/build/release/bindings/swift/lib swift build
  ```

- **iOS and macOS**: the package will link `Sxml.xcframework`, which is not
  built yet. Until then, the package builds only on Linux.

## Optional features

`Sxml.version` is the library's version (`"0.2.0"`), and `Sxml.features`
lists its optional features:

```swift
print(Sxml.features)
// ["test", "expath-binary", "html", "xslt-transform", "timezone-database", "dtd-validation", "uca-collation", "full-text", "invisible-xml", "localization", "builtin-schemas"]
```

`Processor(features:)` takes a set of these names. `nil`, the default,
means all of them. A name that is not in the list throws
`SxmlUsageError.unknownFeature`. Without a feature, sxml behaves as the
specifications say a processor without it does. The features are those of
the JavaScript package's full module; its README describes each one.

## Processors and values

A processor holds names, types and values. Every value, expression,
query, stylesheet and schema belongs to the processor that made it.

- `processor.parse(xml, uri:)` parses a document and returns its document
  node. `xml` is a `String`, or `[UInt8]` in any encoding that the document
  declares.
- `processor.compile(expression, variables:, namespaces:, baseURI:)`
  compiles an XPath 4.0 expression once. Its `evaluate(context:,
  variables:)` can run many times.
- `processor.evaluate(expression, context:, variables:, namespaces:,
  baseURI:)` compiles and evaluates once.
- `processor.value(data)` converts Swift data to a value.

Variable names are NCNames or `Q{uri}local`. A value to bind is anything
that conforms to `XdmInput`:

- a `Value` of this processor, as it is;
- a `String`, as an `xs:string`;
- `Bool`, `Int`, `Double`, `[XdmInput?]` and `[String: XdmInput?]`, as
  `fn:parse-json` reads their JSON: dictionaries are maps, arrays are
  arrays, and numbers are `xs:double`;
- `nil`, the empty sequence.

`namespaces` binds prefixes for the expression, besides the predeclared
ones (`xs`, `fn`, `map`, `array`, `math`, `err`, ...). A processor with the
`expath-binary` feature also binds `bin`, as the `sxml` command does, and
`namespaces` may rebind it:

```swift
print(try processor.evaluate("bin:hex(\"CAFE\") => bin:to-octets()"))
// 202
// 254
```

## Output

`Value` is `CustomStringConvertible`: its `description` is the value
written by the adaptive method of Serialization 4.0. `value.serialize(_:)`
takes the parameters of `fn:serialize`. Keys may be in camelCase, and the
yes/no parameters take `Bool`:

```swift
print(try book.serialize(["method": "xml", "indent": true, "omitXmlDeclaration": false]))
// <?xml version="1.0" encoding="UTF-8"?>
// <book>
//   <title>XPath</title>
//   <year>2026</year>
// </book>
print(try processor.evaluate("map { \"a\": [1, 2] }").serialize(["method": "json"]))
// {"a":[1,2]}
```

## XQuery

`processor.compileQuery(text, namespaces:, baseURI:)` compiles an XQuery
4.0 main module once. The query has these members:

- `evaluate(context:, variables:, updates:, messages:)` returns a value.
  The variables are the external variables of the prolog.
- `serialize(_:)` serializes a value with the serialization parameters of
  the query's output declarations.
- `run(...)` does both.
- `isUpdating` says whether the query is an updating query.

A library module that the query imports is read through the resolver, with
kind `.xqueryModule`.

```swift
let books = try processor.parse("""
    <books>
      <book year="1999"><title>XSLT</title></book>
      <book year="2026"><title>XQuery 4.0</title></book>
    </books>
    """)
let query = try processor.compileQuery("""
    declare option output:method "json";
    declare variable $min external := 0;
    array { //book[@year >= $min] ! map { "title": string(title), "year": xs:integer(@year) } }
    """)
print(try query.run(context: books, variables: ["min": 2000]))
// [{"title":"XQuery 4.0","year":2026}]
```

Full Text searches text with `contains text`:

```swift
print(try processor.compileQuery("\"Parsing large files\" contains text \"parse\" using stemming").run())
// true
```

An updating query returns the empty sequence. sxml never changes a tree:
each tree that the query changes is built again as a new tree. An
`UpdateSink` receives the results after the evaluation:

- `updatedTree` gets each new tree, with the original root.
- `putDocument` gets each document that `fn:put` stores, with its URI and
  a `serialize()`. Without a `putDocument`, `fn:put` raises `FOUP0002`.

What the sink does not take is released.

```swift
let update = try processor.compileQuery("insert node <book year=\"2027\"><title>Next</title></book> into /books")
_ = try update.evaluate(context: books, updates: UpdateSink(updatedTree: { tree in
    print(try processor.evaluate("count(//book)", context: tree.updated), try processor.evaluate("count(//book)", context: tree.original))
}))
// 3 2
```

The output of `fn:trace` and `fn:message` goes to `messages`, a closure
that gets each `Message` after the evaluation, also when the evaluation
failed:

```swift
_ = try processor.compileQuery("trace(count(//book), \"books\")")
    .evaluate(context: books, messages: { message in print("\(message.code): \(message.text)") })
// books: 2
```

## XSLT

`processor.compileStylesheet(text, baseURI:, staticParameters:)` compiles
an XSLT 4.0 stylesheet once. The modules it includes and imports are read
through the resolver, with kind `.xsltModule`, and the packages it uses
with kind `.xsltPackage`. Its `transform(...)` returns a `TransformResult`
with `output`, `value` and `resultDocuments`:

- `source` is the global context item. Templates are applied to it, in
  `initialMode` or in the default mode. A string names a mode, and
  `.unnamed` selects the unnamed mode. To apply templates to something
  else, give `initialMatchSelection`.
- `initialTemplate` calls a named template instead. Without a source,
  `xsl:initial-template` is called. `initialFunction` with `arguments`
  calls a public stylesheet function.
- `parameters`, `templateParameters` and `tunnelParameters` bind the
  parameters by name.
- `delivery` says how the principal result is delivered. With
  `.serialized` (the default), `output` is the result as the unnamed
  `xsl:output` serializes it. With `.raw`, `value` is the result as the
  stylesheet made it, and with `.tree` it is one document node.
  `serialization` replaces the parameters of the `xsl:output`.
- `resultDocuments` has each `xsl:result-document`, with its URI resolved
  against `baseOutputURI`. `resultDocumentDelivery` says how they are
  delivered, in the same way as `delivery`.
- `messages` gets the output of `xsl:message` and `xsl:assert`.

```swift
let stylesheet = try processor.compileStylesheet("""
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
    </xsl:stylesheet>
    """)
let transformed = try stylesheet.transform(
    source: books, parameters: ["heading": "Library"], baseOutputURI: "https://example.com/out/")
print(transformed.output ?? "")
// <h1>Library</h1>
for document in transformed.resultDocuments {
    print("\(document.uri): \(document.output ?? "")")
}
// https://example.com/out/book1.txt: XSLT
// https://example.com/out/book2.txt: XQuery 4.0
```

## XSD validation

`processor.compileSchema(documents, baseURI:)` compiles an XSD 1.1 schema
from one or more schema documents. A document is a `String` with its
text, or a `SchemaDocument`: `.text(text, uri:)`, `.bytes(bytes, uri:)`,
or `.resolved(uri)`, which the resolver is asked for. The documents that a
schema document includes or imports are read through the resolver, with
kind `.schemaDocument`. If the schema has errors, a `SchemaError` is
thrown. Its `reports` has every error, and its `error` is the `SxmlError`
of the first.

`schema.validate(input, mode:, type:, uri:)` validates XML text (a
`String` or `[UInt8]`), or a document or element node. Text is parsed and
validated in one pass, so each report has a line and a column. The
`ValidationResult` has these fields:

- `validity`: `.valid`, `.invalid` or `.notKnown` (lax validation of an
  element that has no declaration). An invalid document is a result, not
  an error.
- `reports`: each error found.
- `document`: unless the document is invalid, the validated copy, whose
  elements and attributes have the types of the schema.

`type` validates the root against a named type of the schema.

```swift
let schema = try processor.compileSchema("""
    <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
      <xs:element name="book">
        <xs:complexType>
          <xs:sequence><xs:element name="title" type="xs:string"/></xs:sequence>
          <xs:attribute name="year" type="xs:gYear" use="required"/>
        </xs:complexType>
      </xs:element>
    </xs:schema>
    """)
let report = try schema.validate("<book year=\"soon\">\n  <author/>\n</book>", uri: "book.xml")
print(report.validity)
// invalid
for entry in report.reports {
    print("\(entry.lineNumber):\(entry.columnNumber) \(entry.code)")
}
// 1:7 cvc-datatype-valid.1.2.1
// 2:3 cvc-complex-type.2.4

let valid = try schema.validate("<book year=\"2026\"><title>XQuery 4.0</title></book>")
print(try processor.evaluate("/book/@year instance of attribute(*, xs:gYear)", context: valid.document))
// true()
```

## Resources

`fn:doc`, `fn:unparsed-text` and the other resource functions ask the
processor's resolver, a closure `(ResourceRequest) throws -> Resource?`.
The request has `kind`, `uriReference` and `absoluteURI`. The answer is
`.text(String)`, `.bytes([UInt8])`, or `nil` when the resource is not
available. Nothing else is read.

```swift
let files = ["https://example.com/data.xml": "<data><item>1</item><item>2</item></data>"]
let processor = try Processor(resolver: { request in files[request.absoluteURI].map(Resource.text) })
print(try processor.evaluate("doc(\"data.xml\")//item/string()", baseURI: "https://example.com/"))
// "1"
// "2"
```

`ResourceKind` has the kinds of the JavaScript package, with its names as
raw values: `.xmlDocument` (`"xml-document"`), `.text`, `.binary`,
`.collection`, `.uriCollection`, `.externalEntity`, `.xqueryModule`,
`.xsltModule`, `.xsltPackage` and `.schemaDocument`. Three more kinds are
for the tables of the optional features. The features read the tables
when they are first used, and without them they behave as a processor
without the feature does:

- `.timezoneData`: a file of the IANA Time Zone Database. `absoluteURI`
  is `urn:sxml:timezone-data:` followed by a zoneinfo path, such as
  `Europe/Paris` (a TZif file) or `zone1970.tab`. Answer with the file's
  bytes.
- `.collationData`: the collation tables. Answer with the text of the
  file that `absoluteURI` names, or `nil`. When the resolver has no answer
  for the DUCET (`allkeys.txt` of the Unicode Collation Algorithm 16.0.0),
  the processor's `collationData` gives it. The default, `.bundled`, is
  the copy that the package ships as a resource. `.bytes(...)` gives
  another copy, and `.none` gives none (UCA collation URIs then use sxml's
  fallback, and `fallback=no` raises `FOCH0002`).
- `.localeData`: a file of the Unicode CLDR. `absoluteURI` is
  `urn:sxml:locale-data:` followed by a path below CLDR's `common/`
  directory, such as `main/de.xml` or `supplemental/supplementalData.xml`.
  Answer with the file's bytes. Without it, dates are formatted in English
  after `[Language: en]`.

```swift
let uca = "http://www.w3.org/2013/collation/UCA?numeric=yes"
print(try processor.evaluate("sort(('Chapter 10', 'Chapter 2', 'chapter 1'), '\(uca)')"))
// "chapter 1"
// "Chapter 2"
// "Chapter 10"
```

The resolver is called synchronously during a call on the processor. It
must not use that processor: such a call throws
`SxmlUsageError.resolverUsesProcessor`. For the same reason, the resolver
must not capture the processor strongly, which would also be a reference
cycle. If the resolver throws, the call stops and rethrows the same
error.

## Errors

Failures throw an `SxmlError`. It has these fields:

- `status`, a `Status` such as `.dynamicError`;
- `code`, such as `"err:FOAR0001"`, also as `codeNamespaceURI` and
  `codeLocalName`;
- `description`, sxml's description, which is also the error's text;
- `sourceName` (`"expression"`, `"query"`, or the URI of a module or
  stylesheet), `lineNumber` and `columnNumber`;
- `message`, which has the location, the code and the description.

```swift
do {
    _ = try processor.evaluate("1 idiv 0")
} catch let error as SxmlError {
    print(error.code)
    // err:FOAR0001
    print(error.message)
    // 1:1: err:FOAR0001: division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)
}
```

A schema that does not compile throws a `SchemaError` (see
[XSD validation](#xsd-validation)). A misuse of the package throws an
`SxmlUsageError`: a resolver that uses its processor, a value of another
processor, an unknown feature, or input that is not `XdmInput` data. The
calls use untyped `throws`, because a resolver's own error is rethrown as
it is.

## Memory and threads

The classes `Processor`, `XPathExpression`, `XQuery`, `Stylesheet`,
`Schema` and `Value` own memory in sxml. ARC releases it when the last
reference goes away, so there is no `close()` or `dispose()`. Each object
keeps its processor alive, so a processor is destroyed after the last
object it made. The values that a call gives you (results, updated trees,
result documents) are yours, as any other value.

A processor is used by one thread at a time. None of these classes is
`Sendable`, so Swift 6 rejects sharing them across isolation domains when
it compiles. Separate processors can run in parallel.

## API summary

| Name | Description |
| --- | --- |
| `Sxml.version`, `Sxml.features` | The library version and its optional features |
| `Processor(resolver:features:collationData:)` | A new processor |
| `Processor.parse(_:uri:)` | A document node |
| `Processor.compile(_:variables:namespaces:baseURI:)` | An `XPathExpression`, with `evaluate(context:variables:)` and `variables` |
| `Processor.evaluate(_:context:variables:namespaces:baseURI:)` | Compiles and evaluates an XPath expression once |
| `Processor.compileQuery(_:namespaces:baseURI:)` | An `XQuery`, with `evaluate`, `serialize`, `run` and `isUpdating` |
| `Processor.compileStylesheet(_:baseURI:staticParameters:)` | A `Stylesheet`, with `transform` |
| `Processor.compileSchema(_:baseURI:)` | A `Schema`, with `validate(_:mode:type:uri:)` |
| `Processor.value(_:)` | Swift data as a value |
| `Processor.serialize(_:options:)` | The same as `value.serialize(options)` |
| `Value.description`, `Value.serialize(_:)` | The value's text |
| `UpdateSink`, `UpdatedTree`, `PutDocument`, `Message` | What a call delivers after it |
| `TransformResult`, `ResultDocument`, `ValidationResult`, `SchemaReport` | Results of a transformation and a validation |
| `SxmlError`, `SchemaError`, `SxmlUsageError` | The errors that the package throws |

## Development

The package's sources are in `bindings/swift/`: `Sources/CSxmlHost` (the
module map over the host shim, `bindings/host/sxml_host.h`),
`Sources/Sxml` and `Tests/SxmlTests`. The DUCET in
`Sources/Sxml/Resources/allkeys.txt` is a copy of
`third_party/unicode-collation/allkeys.txt` (the same git blob), and a
test checks that the two are the same.

ctest runs the tests (Swift Testing) on Linux as `swift/linux`, against
the archive of the same build tree. Under the `debug` preset they run with
AddressSanitizer, and every test checks that its processor leaves no
allocation. The test is skipped when no `swift` is found (`SXML_SWIFT`
names one).

```sh
cmake --preset debug -DSXML_SWIFT=/path/to/swift
cmake --build --preset debug --target sxml_host_swift_library
ctest --preset debug -R swift/linux
```
