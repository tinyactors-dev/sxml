# dev.tinyactors.sxml

sxml for Kotlin on the JVM, and on Android once the Android library is
built (packet 6.6). It is the sxml C library with every language and
every optional feature except EXPath File, in one native library,
`libsxml_kotlin.so`, under a small Kotlin API. The API is that of the
JavaScript package, [`@tinyactors/sxml`](../javascript/README.md), in
Kotlin's idiom: named arguments with defaults instead of option objects,
`AutoCloseable` instead of `dispose()`, and exceptions with the same
fields.

```kotlin
import dev.tinyactors.sxml.Processor

Processor().use { processor ->
    val book = processor.parse("<book><title>XPath</title><year>2026</year></book>")
    val expression = processor.compile("//title/string(), xs:integer(//year) + \$n", variables = listOf("n"))
    expression.evaluate(context = book, variables = mapOf("n" to 1)).use { result ->
        result.toString()                          // "\"XPath\"\n2.027e3", the adaptive method
        result.serialize(mapOf("method" to "text")) // "XPath 2027"
    }
}
```

- [The library](#the-library)
- [Optional features](#optional-features)
- [Processors and values](#processors-and-values)
- [Output](#output)
- [XQuery](#xquery)
- [XSLT](#xslt)
- [XSD validation](#xsd-validation)
- [Resources](#resources)
- [Errors](#errors)
- [Memory](#memory)
- [Threads](#threads)
- [API summary](#api-summary)
- [Development](#development)

## The library

The binding loads `libsxml_kotlin.so` the first time it is used, with
`System.loadLibrary("sxml_kotlin")`, or with `System.load` of the path in
the system property `sxml.library.path`. There is no `load()` to call.
`Sxml.version` is the library's version (`"0.2.0"`).

The library exports the JNI entry points only. Text crosses as UTF-8
bytes, never as JNI's modified UTF-8, so U+0000 and supplementary
characters are not changed on the way.

## Optional features

`Sxml.features` lists the library's optional features:

```kotlin
Sxml.features
// [test, expath-binary, html, xslt-transform, timezone-database, dtd-validation,
//  uca-collation, full-text, invisible-xml, localization, builtin-schemas,
//  source-code]
```

`Processor(features = setOf(...))` picks some of them (`null`, the
default, is all). A name the library does not have is
`IllegalArgumentException`. Without a feature, sxml behaves as the
specifications say a processor without it does. The features are those
of the JavaScript package's full module, described in its
[README](../javascript/README.md#optional-features).

## Processors and values

A processor holds names, types and values. Every value, expression,
query, stylesheet and schema belongs to the processor that made it.

- `parse(xml, uri = null)` parses a document (a `String`, or a
  `ByteArray` in any encoding the document declares) and returns its
  document node.
- `compile(expression, variables = emptyList(), namespaces = emptyMap(),
  baseUri = null)` compiles an XPath 4.0 expression once. Its
  `evaluate(context = null, variables = emptyMap())` can run many times.
- `evaluate(expression, context, variables, namespaces, baseUri)`
  compiles and evaluates once.
- `value(data)` converts Kotlin data to a value.

Variable names are NCNames or `Q{uri}local`. An input (a context or a
variable's value) is a `Value` of this processor, a `String` (an
`xs:string`), or data that `fn:parse-json` reads from its JSON: `Number`,
`Boolean`, `null` (the empty sequence), `List`, `Array` and `Map` with
`String` keys. Anything else is `IllegalArgumentException`.

`namespaces` binds prefixes for the expression, besides the predeclared
ones. A processor with the `expath-binary` feature also binds `bin`, as
the `sxml` command does:

```kotlin
processor.evaluate("bin:hex(\"CAFE\") => bin:to-octets()").toString() // "202\n254"
```

## Output

`value.toString()` writes the value by the adaptive method of
Serialization 4.0. `value.serialize(options)` takes the parameters of
`fn:serialize` (method `xml` unless they name another), and keys may be in
camelCase:

```kotlin
book.serialize(mapOf("method" to "xml", "indent" to true, "omitXmlDeclaration" to false))
// <?xml version="1.0" encoding="UTF-8"?>
// <book>
//   <title>XPath</title>
// </book>
processor.evaluate("map { \"a\": [1, 2] }").serialize(mapOf("method" to "json")) // {"a":[1,2]}
```

## XQuery

`compileQuery(query, namespaces = emptyMap(), baseUri = null)` compiles an
XQuery 4.0 main module once. Its `evaluate(context, variables, updates,
messages)` returns a value, `serialize(value)` serializes a value with
the serialization parameters of the query's output declarations, and
`run(...)` does both. The variables are the external variables of the
prolog. A library module that the query imports is read through the
resolver, with kind `XQUERY_MODULE`.

```kotlin
val books = processor.parse("""<books>
  <book year="1999"><title>XSLT</title></book>
  <book year="2026"><title>XQuery 4.0</title></book>
</books>""")
val query = processor.compileQuery("""
  declare option output:method "json";
  declare variable ${'$'}min external := 0;
  array { //book[@year >= ${'$'}min] ! map { "title": string(title), "year": xs:integer(@year) } }""")
query.run(context = books, variables = mapOf("min" to 2000))
// [{"title":"XQuery 4.0","year":2026}]

processor.compileQuery("\"Parsing large files\" contains text \"parse\" using stemming").run() // true
```

An updating query (the XQuery Update Facility 3.0) returns the empty
sequence, and `isUpdating` is `true`. sxml never changes a tree: each
tree the query changes is built again as a new tree, and it goes to the
`UpdateSink`'s `updatedTree` with the original root. The documents that
`fn:put` stores go to its `putDocument`, with their URI and a
`serialize()`. Without a `putDocument`, `fn:put` raises `FOUP0002`.

```kotlin
val update = processor.compileQuery("insert node <book year=\"2027\"><title>Next</title></book> into /books")
update.evaluate(
    context = books,
    updates = UpdateSink(updatedTree = { tree ->
        tree.use {
            processor.evaluate("count(//book)", context = tree.updated) // 3
            processor.evaluate("count(//book)", context = tree.original) // 2
        }
    }),
).close()
```

`fn:trace` and `fn:message` output goes to `messages`, a function that
receives each `Message` after the evaluation, also when it failed.

## XSLT

`compileStylesheet(stylesheet, baseUri = null, staticParameters =
emptyMap())` compiles an XSLT 4.0 stylesheet once (a `String`, or a
`ByteArray` in its declared encoding). The modules it includes and
imports are read through the resolver, with kind `XSLT_MODULE`, and the
packages it uses with kind `XSLT_PACKAGE`. Its `transform(...)` returns a
`TransformResult` with `output`, `value` and `resultDocuments`:

- `source` is the global context item. Templates are applied to it, in
  `initialMode` (`InitialMode.Named(name)`, or `InitialMode.Unnamed` for
  the unnamed mode) or in the default mode. To apply templates to
  something else, give `initialMatchSelection`.
- `initialTemplate` calls a named template instead. Without a source,
  `xsl:initial-template` is called. `initialFunction` with `args` calls a
  public stylesheet function.
- `parameters`, `templateParameters` and `tunnelParameters` bind the
  parameters by name.
- `delivery` says how the principal result is delivered. With
  `ResultDelivery.SERIALIZED` (the default), `output` is the result as the
  unnamed `xsl:output` serializes it. With `RAW`, `value` is the result as
  the stylesheet made it, and with `TREE` it is one document node.
  `serialization` replaces the parameters of the `xsl:output`.
- `resultDocuments` has each `xsl:result-document`, with its URI resolved
  against `baseOutputUri`. `resultDocumentDelivery` says how they are
  delivered, in the same way as `delivery`.
- `messages` receives the `xsl:message` and `xsl:assert` output.

```kotlin
val stylesheet = processor.compileStylesheet("""
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" html-version="5"/>
  <xsl:param name="heading" select="'Books'"/>
  <xsl:template match="/books">
    <h1><xsl:value-of select="${'$'}heading"/></h1>
    <xsl:for-each select="book">
      <xsl:result-document href="book{position()}.txt" method="text">
        <xsl:value-of select="title"/>
      </xsl:result-document>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>""")
val result = stylesheet.transform(
    source = books,
    parameters = mapOf("heading" to "Library"),
    baseOutputUri = "https://example.com/out/",
)
result.output // <h1>Library</h1>
result.resultDocuments.map { "${it.uri}: ${it.output}" }
// [https://example.com/out/book1.txt: XSLT, https://example.com/out/book2.txt: XQuery 4.0]
```

## XSD validation

`compileSchema(documents, baseUri = null)` compiles an XSD 1.1 schema from
one or more `SchemaDocument`s: `SchemaDocument.Text(text, uri)`, or
`SchemaDocument.Resolved(uri)`, which the resolver is asked for. One
document, or one document's text, may be passed alone. The documents a
schema document includes or imports are read through the resolver, with
kind `SCHEMA_DOCUMENT`. If the schema has errors, a `SchemaException` is
thrown, and its `reports` has every error.

`schema.validate(input, mode, type, uri)` validates XML text (a `String`
or a `ByteArray`), or a document or element node. Text is parsed and
validated in one pass, so each report has a line and a column. The
`ValidationResult` has `validity` (`VALID`, `INVALID` or `NOT_KNOWN`),
`reports` and `document`. An invalid document is a result, not an
exception. Unless the document is invalid, `document` is the validated
copy, whose elements and attributes have the types of the schema. `type`
validates the root against a named type of the schema, and
`ValidationMode.LAX` validates laxly.

```kotlin
val schema = processor.compileSchema("""
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="book">
    <xs:complexType>
      <xs:sequence><xs:element name="title" type="xs:string"/></xs:sequence>
      <xs:attribute name="year" type="xs:gYear" use="required"/>
    </xs:complexType>
  </xs:element>
</xs:schema>""")
val report = schema.validate("<book year=\"soon\">\n  <author/>\n</book>", uri = "book.xml")
report.validity // INVALID
report.reports.map { "${it.lineNumber}:${it.columnNumber} ${it.code}" }
// [1:7 cvc-datatype-valid.1.2.1, 2:3 cvc-complex-type.2.4]

val valid = schema.validate("<book year=\"2026\"><title>XQuery 4.0</title></book>")
processor.evaluate("/book/@year instance of attribute(*, xs:gYear)", context = valid.document) // true()
```

## Resources

`fn:doc`, `fn:unparsed-text` and the other resource functions ask the
processor's resolver, a `fun interface Resolver`. It gets a
`ResourceRequest(kind, uriReference, absoluteUri)` and returns
`Resource.Text`, `Resource.Bytes`, or `null` when the resource is not
available. Nothing else is read.

```kotlin
val files = mapOf("https://example.com/data.xml" to "<data><item>1</item><item>2</item></data>")
Processor(resolver = { request -> files[request.absoluteUri]?.let { Resource.Text(it) } }).use { processor ->
    processor.evaluate("doc(\"data.xml\")//item ! string()", baseUri = "https://example.com/")
    // "1"
    // "2"
}
```

`ResourceKind` has every kind of the C library: `XML_DOCUMENT`, `TEXT`,
`BINARY`, `COLLECTION`, `URI_COLLECTION`, `XQUERY_MODULE`, `XSLT_MODULE`,
`XSLT_PACKAGE`, `SCHEMA_DOCUMENT`, `EXTERNAL_ENTITY`,
`FULL_TEXT_STOP_WORDS`, `FULL_TEXT_THESAURUS`, and three for the tables of
the optional features:

- `TIMEZONE_DATA`: a file of the IANA Time Zone Database. `absoluteUri` is
  `urn:sxml:timezone-data:` followed by a zoneinfo path, such as
  `Europe/Paris` (a TZif file) or `zone1970.tab`. Answer with the file's
  bytes, for example from `/usr/share/zoneinfo`.
- `COLLATION_DATA`: the collation table. When the resolver has no answer,
  the processor's `collationData` answers for the DUCET: by default
  `CollationData.Bundled`, the class path resource
  `dev/tinyactors/sxml/allkeys.txt` (the Unicode Collation Algorithm
  16.0.0's `allkeys.txt`, which the build copies beside the library);
  `CollationData.Bytes(bytes)` gives another copy, and
  `CollationData.None` none, so that UCA collation URIs fall back.
- `LOCALE_DATA`: a file of the Unicode CLDR. `absoluteUri` is
  `urn:sxml:locale-data:` followed by a path below CLDR's `common/`
  directory, such as `main/de.xml`. Answer with the file's bytes. Without
  it, dates are formatted in English after `[Language: en]`.

The resolver runs during the call, on the calling thread, and must not
use its processor (`IllegalStateException`). If it throws, the call stops
and rethrows the same exception.

## Errors

Failures throw an `SxmlException`. Its fields are `status`, a `Status`
such as `DYNAMIC_ERROR`, and `code`, such as `err:FOAR0001`, also as
`codeNamespaceUri` and `codeLocalName`. It also has `description`,
`sourceName` (`expression`, `query`, or the URI of a module or
stylesheet), `lineNumber` and `columnNumber`. A failed allocation is
`SxmlException` with `Status.OUT_OF_MEMORY`, not `OutOfMemoryError`.

```kotlin
try {
    processor.evaluate("1 div 0")
} catch (error: SxmlException) {
    error.code    // err:FOAR0001
    error.message // 1:1: err:FOAR0001: division by zero (F&O 4.0, op:numeric-divide, ...)
}
```

A schema that does not compile throws a `SchemaException`, an
`SxmlException` with every report in `reports`. Misuse is a Kotlin
exception: `IllegalStateException` for an object used after it is closed
or a resolver that uses its processor, `IllegalArgumentException` for an
unknown feature, a value of another processor or input that is not
JSON-shaped.

## Memory

`Processor`, `XPathExpression`, `XQuery`, `Stylesheet`, `Schema` and
`Value` own native memory and are `AutoCloseable`: close them, or use
`use {}`. `close()` may be called more than once. An object that is never
closed is released after it becomes unreachable, and closing a processor
releases everything it made. The values a call gives you (results,
updated trees, put documents, result documents, validated documents) are
yours to close; `TransformResult`, `ValidationResult`, `UpdatedTree` and
`PutDocument` close theirs. `Processor.liveAllocationCount()` is for
tests.

## Threads

A processor is used by one thread at a time. Every call on a processor,
or on what it made, holds the processor's lock, so threads that share a
processor take turns; separate processors run in parallel. Callbacks
(messages, update sinks) run after the call, without the lock.

## API summary

| Name | Description |
| --- | --- |
| `Sxml.version`, `Sxml.features` | The library version and its optional features |
| `Processor(resolver, features, collationData)` | A new processor |
| `Processor.parse(xml, uri)` | A document node |
| `Processor.compile(expression, variables, namespaces, baseUri)` | An `XPathExpression`, with `evaluate(context, variables)` and `variables` |
| `Processor.evaluate(expression, ...)` | Compiles and evaluates an XPath expression once |
| `Processor.compileQuery(query, namespaces, baseUri)` | An `XQuery`, with `evaluate`, `serialize`, `run` and `isUpdating` |
| `Processor.compileStylesheet(stylesheet, baseUri, staticParameters)` | A `Stylesheet`, with `transform` |
| `Processor.compileSchema(documents, baseUri)` | A `Schema`, with `validate` |
| `Processor.value(data)` | Kotlin data as a value |
| `Processor.serialize(value, options)` | The same as `value.serialize(options)`; adaptive without options |
| `Value.toString()`, `Value.serialize(options)` | The value's text |
| `close()` | On each object: releases its memory |
| `SxmlException`, `SchemaException` | The errors the binding throws |

## Development

The binding lives in `bindings/kotlin/` of the sxml repository: the JNI
glue (`src/main/c/sxml_kotlin_jni.c`, over the host shim of
`bindings/host`), the Kotlin sources and the tests. The tests have the
names of the JavaScript package's `test/package.test.ts` and
`test/full.test.ts`, plus the binding's own. ctest builds the library and
runs them with kotlinc and `kotlin.test`, without Gradle or network, under
`-Xcheck:jni`:

```sh
cmake --preset debug && cmake --build --preset debug
SXML_KOTLINC=/path/to/kotlinc/bin/kotlinc ctest --preset debug -R kotlin/jvm
```

It needs a JDK 21 (CMake finds JNI's headers in it) and Kotlin 2.2.20,
and is skipped with the reason when either is missing.
`tools/run-kotlin-tests.sh LIBRARY OUTPUT-DIRECTORY [FILTER]` runs the
cases whose name contains the filter.
