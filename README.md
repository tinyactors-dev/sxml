# sxml

sxml is an XML toolchain in portable C. It parses XML and HTML, validates
documents against XSD 1.1 schemas and DTDs, and runs XPath 4.0, XQuery 4.0
and XSLT 4.0, from one small library with no dependencies. Use it from C,
from JavaScript in browsers and Node.js, from Swift, or as the `sxml`
command.

```console
$ sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'
"Godel, Escher, Bach"
"The Art of Computer Programming"
```

- [What it does](#what-it-does)
- [Where it runs](#where-it-runs)
- [Install](#install)
- [Quick start](#quick-start)
- [Examples](#examples): [streaming](#stream-large-documents),
  [HTML](#read-html), [Invisible XML](#parse-text-with-invisible-xml),
  [Full Text](#search-text)
- [Features by build](#features-by-build)
- [Use it from C](#use-it-from-c), [from JavaScript](#use-it-from-javascript)
  and [from Swift](#use-it-from-swift)
- [Conformance](#conformance)
- [Status](#status)

## What it does

sxml implements these standards. All of its languages share one data
model (XDM 4.0) and one evaluator.

| Area | Standard |
| --- | --- |
| Parsing | XML 1.0 (Fifth Edition) and XML 1.1, Namespaces in XML 1.0 and 1.1, from UTF-8, UTF-16, ISO-8859-1 and US-ASCII |
| DTDs | The internal and external subsets, external entities, attribute defaults, and DTD validation (XML 1.0, section 5) |
| HTML | HTML parsing as the HTML Living Standard specifies it, every encoding of the Encoding Standard: `fn:parse-html` and `fn:html-doc` |
| Data model | XDM 4.0 |
| Expressions | XPath 4.0 and the F&O 4.0 function library: maps, arrays, records, higher-order functions, regular expressions, dates and times, decimal arithmetic, JSON and CSV |
| Queries | XQuery 4.0, the XQuery Update Facility 3.0 (with `fn:put`), and XQuery and XPath Full Text 3.0 in XQuery and XPath, with stemming, stop words and thesauri |
| Transformations | XSLT 4.0, with streaming, packages, accumulators, `xsl:evaluate` and `fn:transform`; XSLT 1.0, 2.0 and 3.0 stylesheets; EXSLT Common |
| Schemas | XSD 1.1 validation; schema-aware XPath, XQuery and XSLT |
| Output | Serialization 4.0: the xml, xhtml, html, text, json, csv and adaptive methods, `disable-output-escaping` |
| Grammars | Invisible XML 1.0: `fn:invisible-xml` |
| Extensions | EXPath Binary 4.0 and EXPath File 4.0 |
| Text | Unicode 16.0; the Unicode Collation Algorithm with the CLDR root collation and CLDR tailorings; the IANA Time Zone Database; CLDR month and day names, calendars and number words in `fn:format-date` and `fn:format-integer` |

## Where it runs

| Target | What you get |
| --- | --- |
| Linux | The C library, static or shared, and the `sxml` command. Built and tested with Clang and GCC. |
| macOS, Windows | The `sxml` command, cross-compiled from Linux for x86_64 and AArch64 ([Install](#install)); the same C17 code, with an MSVC build preset. The x86_64 Windows program runs under wine; neither platform has been run natively yet. |
| Browsers and Node.js | The `@tinyactors/sxml` package: a TypeScript wrapper over two WebAssembly modules (below). |
| WASI | The `sxml` command as a WebAssembly program. |
| Kotlin on the JVM | The `dev.tinyactors.sxml` Kotlin binding over the native library (below). |
| Swift on Linux | The `Sxml` Swift package over the native library (below). |
| iOS, Android | Planned. The Swift package's iOS and macOS build and the Kotlin library for Android are not made yet. |

The WebAssembly modules, at commit `4f5a8bf` (stripped):

| Module | Contents | Size | Gzipped |
| --- | --- | --: | --: |
| `sxml.wasm` (JavaScript) | Everything: XML, XPath 4.0, XQuery 4.0, XSLT 4.0, XSD 1.1, serialization and every optional feature except EXPath File (D272) | 7,701,002 bytes | 1,713,241 bytes |
| `sxml` (WASI command) | Everything the native `sxml` command has | 9,295,080 bytes | 1,994,875 bytes |

## Install

Each release of the `sxml` command comes as one archive per platform,
named `sxml-<version>-<target>`:

| Target | Archive | Program |
| --- | --- | --- |
| `x86_64-linux`, `aarch64-linux` | `.tar.gz` | `sxml`, a static executable that runs on any Linux distribution |
| `x86_64-macos`, `aarch64-macos` | `.tar.gz` | `sxml`, for macOS 11 and later |
| `x86_64-windows`, `aarch64-windows` | `.zip` | `sxml.exe`, for Windows 10 and later |
| `wasm32-wasi` | `.tar.gz` | `sxml.wasm`, for any WASI preview 1 runtime |

Each archive holds one directory with the program, `LICENSE`, this
`README.md` and `USAGE.md`, the command's manual. Check the archive
against `SHA256SUMS`, unpack it and put the program on your `PATH`:

```sh
sha256sum --check --ignore-missing SHA256SUMS
tar -xzf sxml-0.2.0-x86_64-linux.tar.gz
install -m 755 sxml-0.2.0-x86_64-linux/sxml ~/.local/bin/sxml
sxml --version
```

On macOS, a downloaded program that is not notarized is quarantined;
`xattr -d com.apple.quarantine sxml` lets it run. Run the WASI module
with a WASI runtime that gives it the directories it reads, for example
`wasmtime --dir=. sxml.wasm xpath '1 + 1'`.

### Build the archives

The archives are built on Linux (or macOS) with [mise](https://mise.jdx.dev),
which fetches the pinned tools of `mise.toml`: CMake, Ninja, Zig, Node.js
and the WASI SDK. Zig cross-compiles every native target
(`cmake/toolchains/zig.cmake`), so no other SDK or sysroot is needed.

```sh
mise install                 # the tools, once
mise run dist                # every archive and SHA256SUMS, in build/dist/
mise run dist:aarch64-linux  # one target (mise tasks lists them)
mise run dist:verify         # run what this machine can: natively, qemu-user, wine, Node.js
mise run install             # build for this machine, install into ~/.local
mise run install -- /usr/local
```

`mise run install` puts the program in `PREFIX/bin` and the documents in
`PREFIX/share/doc/sxml`. Without mise, `tools/dist.sh` does the same with
the tools on your `PATH` (`tools/dist.sh build x86_64-linux`).

## Quick start

You need CMake 3.24 or later, Ninja and a C17 compiler (Clang or GCC).

```sh
cmake --preset release
cmake --build --preset release
export PATH="$PWD/build/release/apps:$PATH"
```

The examples use this file.

`books.xml`:

```xml
<books>
  <book year="1979"><title>Godel, Escher, Bach</title><price>22.50</price></book>
  <book year="1968"><title>The Art of Computer Programming</title><price>180.00</price></book>
  <book year="1986"><title>Structure and Interpretation of Computer Programs</title><price>55.00</price></book>
</books>
```

Check that a document is well-formed. No output means it is:

```console
$ sxml check books.xml
$ echo $?
0
```

Validate it against an XSD schema.

`books.xsd`:

```xml
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="books">
    <xs:complexType>
      <xs:sequence>
        <xs:element name="book" type="book" maxOccurs="unbounded"/>
      </xs:sequence>
    </xs:complexType>
  </xs:element>
  <xs:complexType name="book">
    <xs:sequence>
      <xs:element name="title" type="xs:string"/>
      <xs:element name="price" type="xs:decimal"/>
    </xs:sequence>
    <xs:attribute name="year" type="xs:gYear" use="required"/>
  </xs:complexType>
</xs:schema>
```

Errors name the line, the column and the XSD rule that failed:

```console
$ sxml validate --schema=books.xsd books.xml
$ echo '<books><book year="soon"><title>Dune</title></book></books>' | sxml validate --schema=books.xsd -
-:1:14: cvc-datatype-valid.1.2.1: attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'
-:1:8: cvc-complex-type.2.4: the content of element 'book' is incomplete; expected 'price'
```

Evaluate XPath 4.0:

```console
$ sxml xpath --context-item=books.xml 'sum(//price ! xs:decimal(.))'
257.5
$ sxml xpath --method=json 'map { "a": [1, 2] }'
{"a":[1,2]}
```

Run an XQuery 4.0 query.

`titles.xq`:

```xquery
<titles>{
  for $book in //book
  order by xs:integer($book/@year)
  return <title year="{$book/@year}">{string($book/title)}</title>
}</titles>
```

```console
$ sxml xquery --context-item=books.xml titles.xq
<titles><title year="1968">The Art of Computer Programming</title><title year="1979">Godel, Escher, Bach</title><title year="1986">Structure and Interpretation of Computer Programs</title></titles>
```

Transform with XSLT 4.0.

`list.xsl`:

```xml
<xsl:stylesheet version="4.0" expand-text="yes" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>
  <xsl:param name="heading" select="'Books'"/>
  <xsl:template match="/books">
    <html>
      <body>
        <h1>{$heading}</h1>
        <ul>
          <xsl:apply-templates select="book">
            <xsl:sort select="title"/>
          </xsl:apply-templates>
        </ul>
      </body>
    </html>
  </xsl:template>
  <xsl:template match="book">
    <li>{title} ({@year})</li>
  </xsl:template>
</xsl:stylesheet>
```

```console
$ sxml transform list.xsl books.xml
<!DOCTYPE html>
<html>
  <body>
    <h1>Books</h1>
    <ul>
      <li>Godel, Escher, Bach (1979)</li>
      <li>Structure and Interpretation of Computer Programs (1986)</li>
      <li>The Art of Computer Programming (1968)</li>
    </ul>
  </body>
</html>
```

`sxml help` lists every command. [The sxml Book](docs/book/src/SUMMARY.md)
teaches the languages step by step, on small projects.
[apps/sxml/README.md](apps/sxml/README.md) is the full reference.

## Examples

### Stream large documents

In an XSLT mode declared `streamable="yes"`, `sxml transform` reads its
input as it parses it and never builds a tree. Memory stays flat however
large the input is.

`total.xsl`:

```xml
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>
  <xsl:template match="/orders">
    <xsl:value-of select="format-number(sum(order/total), '#,##0.00')"/>
  </xsl:template>
</xsl:stylesheet>
```

`orders.xml` is 76 MB, with one million orders such as
`<order id="1"><customer>c1</customer><total>37.01</total></order>`:

```console
$ sxml transform total.xsl orders.xml
499,995,000.00
```

The streamed run uses 11 MB of memory at its peak. Without
`streamable="yes"`, the same stylesheet builds the tree and uses 597 MB.
A 974 MB document streams in 11 MB too. Accumulators, `xsl:fork`,
`xsl:iterate` and `xsl:source-document streamable="yes"` stream as well.
A stylesheet that is not guaranteed-streamable is rejected with the error
XSLT 4.0 gives (XTSE3430).

### Read HTML

`fn:parse-html` and `fn:html-doc` parse HTML as browsers do, into elements
in the XHTML namespace.

`shop.html`:

```html
<!doctype html><title>Shop</title>
<table><tr><td>Tea<td>3.50
<tr><td>Coffee<td>4.20</table>
```

```console
$ sxml xpath 'html-doc("shop.html")//*:tr ! string-join(*:td ! normalize-space(), ": ")'
"Tea: 3.50"
"Coffee: 4.20"
$ sxml xpath --method=xml "parse-html('<ul><li>Tea<li>Coffee</ul>')//*:ul"
<ul xmlns="http://www.w3.org/1999/xhtml"><li>Tea</li><li>Coffee</li></ul>
```

### Parse text with Invisible XML

`fn:invisible-xml` compiles an Invisible XML grammar into a function that
turns text into XML.

`date.ixml`:

```
date: day, -" ", month, -" ", year.
day: digit, digit?.
month: "January"; "February"; "March"; "April"; "May"; "June";
       "July"; "August"; "September"; "October"; "November"; "December".
year: digit, digit, digit, digit.
-digit: ["0"-"9"].
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("date.ixml"))("20 October 2026")'
<date><day>20</day><month>October</month><year>2026</year></date>
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("date.ixml"))("20 Octobre 2026")'
<failed xmlns:ixml="http://invisiblexml.org/NS" ixml:state="failed"><line>1</line><column>9</column><position>9</position><unexpected>"r"</unexpected><expected>"e"</expected></failed>
```

### Search text

XQuery and XPath Full Text adds `contains text` to XQuery and XPath:
words and phrases, `ftand`, `ftor` and `ftnot`, distances, windows,
wildcards, stemming, stop words, thesauri and scores.

`reviews.xml`:

```xml
<reviews>
  <review id="1">The parser handled every document we tried.</review>
  <review id="2">Parsing large files was slow at first.</review>
  <review id="3">Streaming keeps memory flat, even for huge inputs.</review>
</reviews>
```

```console
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "parse"]/@id/string()'
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "parse" using stemming]/@id/string()'
"2"
$ sxml xquery --context-item=reviews.xml --method=adaptive --expression='//review[. contains text "memory" ftand "huge" distance at most 5 words]/@id/string()'
"3"
$ sxml xpath --context-item=reviews.xml '//review[. contains text "parse" using stemming]/@id/string()'
"2"
```

## Features by build

Every build has everything (D272): the JavaScript package's one module,
`sxml.wasm`, the `sxml` command and the C library.

| Feature | `sxml.wasm` | `sxml` command | C library |
| --- | :-: | :-: | :-: |
| XML 1.0 and 1.1, namespaces, DTDs and entities | yes | yes | yes |
| XPath 4.0 and F&O 4.0, with JSON and CSV | yes | yes | yes |
| Serialization 4.0 | yes | yes | yes |
| XQuery 4.0 and the XQuery Update Facility 3.0 | yes | yes | yes |
| XQuery and XPath Full Text 3.0 | yes | yes | yes |
| XSLT 4.0, streaming, XSLT 1.0 and EXSLT Common | yes | yes | yes |
| `fn:transform` | yes | yes | yes |
| XSD 1.1 validation and schema awareness | yes | yes | yes |
| DTD validation | yes | yes | yes |
| HTML parsing | yes | yes | yes |
| Invisible XML | yes | yes | yes |
| EXPath Binary | yes | yes | yes |
| EXPath File | – | with `--allow-file-access` | yes [^file] |
| UCA collations with CLDR tailorings | yes [^data] | yes | yes [^data] |
| Time zones by name | yes [^data] | yes [^zoneinfo] | yes [^data] |
| Languages and calendars in the formatting functions | yes [^data] | yes | yes [^data] |

[^file]: A host names the EXPath File feature on purpose: it reads and
    writes files outside the resolver.
[^data]: The tables come from the host's resolver when they are first
    used. The JavaScript package ships the collation table.
[^zoneinfo]: Read from `/usr/share/zoneinfo`.

In the C library, the optional features are on when a program names them
in its processor configuration (`sxml_all_optional_features()` names them
all but EXPath File); the sxml command, the Kotlin and Swift bindings and
the JavaScript package name them all.

## Use it from C

A processor holds names, types and values. Compile an expression once,
then evaluate it against any number of documents:

```c
#include <stdio.h>

#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xpath.h"

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Serialized bytes go to an output stream; this one writes to stdout. */
static sxml_status write_to_stdout(void *state, sxml_byte_view bytes) {
    (void)state;
    size_t written = fwrite(bytes.bytes, 1, bytes.byte_count, stdout);
    return written == bytes.byte_count ? SXML_STATUS_OK
                                       : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations stdout_operations = {
    .write = write_to_stdout,
};

int main(void) {
    sxml_allocator allocator = sxml_default_allocator();
    sxml_error_record error_record = {0};

    /* A processor owns names, types and values. */
    sxml_processor *processor = NULL;
    sxml_status status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &processor);

    /* Parse a document. */
    sxml_item document = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            processor,
            &(sxml_xml_parse_request){
                .input = TEXT("<books><book year='1979'>Godel, Escher, Bach</book>"
                              "<book year='1968'>The Art of Computer Programming</book>"
                              "</books>"),
                .diagnostic = &error_record.diagnostic,
            },
            &document);
    }
    sxml_sequence context = sxml_sequence_from_item(document);

    /* Compile an XPath 4.0 expression once... */
    sxml_xpath_expression *expression = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_create(
            allocator,
            &(sxml_xpath_expression_configuration){
                .processor = processor,
                .expression_text = TEXT("//book[@year < 1975] ! string()"),
                .error_record = &error_record,
            },
            &expression);
    }

    /* ...and evaluate it against the document. */
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){
                .dynamic_context = {.has_context_value = true,
                                    .context_value = context},
                .error_record = &error_record,
            },
            &result);
    }

    /* Write the result by the adaptive method of Serialization 4.0. */
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE},
                .error_record = &error_record,
            },
            (sxml_output_stream){.operations = &stdout_operations});
        putchar('\n');
    }

    if (status != SXML_STATUS_OK) {
        sxml_byte_view description = error_record.diagnostic.description;
        fprintf(stderr, "%.*s\n", (int)description.byte_count,
                (const char *)description.bytes);
    }
    sxml_sequence_release(result);
    sxml_xpath_expression_destroy(expression);
    sxml_sequence_release(context);
    sxml_processor_destroy(processor);
    return status == SXML_STATUS_OK ? 0 : 1;
}
```

Build it against sxml from your own `CMakeLists.txt`:

```cmake
set(SXML_BUILD_TESTS OFF)
set(SXML_BUILD_APPS OFF)
add_subdirectory(sxml)
add_executable(books books.c)
target_link_libraries(books PRIVATE sxml)
```

```
$ ./books
"The Art of Computer Programming"
```

XQuery (`sxml/xquery.h`), XSLT (`sxml/xslt.h`) and XSD (`sxml/schema.h`)
follow the same pattern: compile once, run many times, with the same
processor, contexts and serializer. To turn on optional features, list
them in the processor configuration, for example
`.optional_features = sxml_all_optional_features()`.

## Use it from JavaScript

The `@tinyactors/sxml` package in [`bindings/javascript`](bindings/javascript)
runs the same library as WebAssembly, with no runtime dependencies. It is
not on npm yet: build it from this repository as its README describes.

```js
import { load } from '@tinyactors/sxml';

const sxml = await load();
const processor = sxml.createProcessor();

const books = processor.parse(
  '<books><book year="1979">Godel, Escher, Bach</book>' +
  '<book year="1968">The Art of Computer Programming</book></books>');
const titles = processor.evaluate('//book[@year < $year] ! string()', {
  context: books,
  variables: { year: 1975 },
});
console.log(titles.toString());
// "The Art of Computer Programming"
```

The same module runs XQuery, XSLT and XSD:

```js
const library = sxml.createProcessor();
const query = library.compileQuery('declare variable $n external; //book[@year < $n] ! string()');
query.run({ context: library.parse('<books><book year="1968">TAOCP</book></books>'), variables: { n: 1975 } });
// 'TAOCP'
```

The package's [README](bindings/javascript/README.md) describes its API.

## Use it from Kotlin

The Kotlin binding `dev.tinyactors.sxml` in
[`bindings/kotlin`](bindings/kotlin) has the same API in Kotlin's idiom,
over one native library, `libsxml_kotlin.so`, with every language. It
runs on the JVM now; the Android library is not made yet.

```kotlin
import dev.tinyactors.sxml.Processor

Processor().use { processor ->
    val books = processor.parse(
        "<books><book year=\"1979\">Godel, Escher, Bach</book>" +
            "<book year=\"1968\">The Art of Computer Programming</book></books>")
    val titles = processor.evaluate("//book[@year < \$year] ! string()", context = books, variables = mapOf("year" to 1975))
    println(titles)
    // "The Art of Computer Programming"

    val query = processor.compileQuery("declare variable \$n external; //book[@year < \$n] ! string()")
    println(query.run(context = processor.parse("<books><book year=\"1968\">TAOCP</book></books>"), variables = mapOf("n" to 1975)))
    // TAOCP
}
```

The binding's [README](bindings/kotlin/README.md) describes its API and
how to build it.

## Use it from Swift

The Swift package `Sxml` in [`bindings/swift`](bindings/swift), with
`Package.swift` at the root of this repository, has the same API in
Swift's idiom, over the native library with every language. It builds on
Linux now; iOS and macOS wait for its XCFramework.

```swift
import Sxml

let processor = try Processor()
let books = try processor.parse(
    "<books><book year=\"1979\">Godel, Escher, Bach</book>"
    + "<book year=\"1968\">The Art of Computer Programming</book></books>")
let titles = try processor.evaluate("//book[@year < $year] ! string()", context: books, variables: ["year": 1975])
print(titles)
// "The Art of Computer Programming"

let query = try processor.compileQuery("declare variable $n external; //book[@year < $n] ! string()")
print(try query.run(context: processor.parse("<books><book year=\"1968\">TAOCP</book></books>"), variables: ["n": 1975]))
// TAOCP
```

The package's [README](bindings/swift/README.md) describes its API and how
to build it.

## Conformance

sxml is tested against the W3C test suites. The counts are of a release
build at commit `e74d4ad` (2026-10-20). A test case counts as run unless
it needs a feature that sxml does not claim; a test case that expects an
error and gets another error code counts as passed, as the QT4 catalog
allows.

| Test suite | Run | Passed | Rate |
| --- | --: | --: | --: |
| QT4: XPath 4.0 and F&O 4.0 (XP40) | 32,841 | 32,815 | 99.92 % |
| QT4: XQuery 4.0 (XQ40), with Update and Full Text | 46,232 | 46,158 | 99.84 % |
| XSLT 4.0 (XT40) | 12,518 | 12,268 | 98.00 % |
| XSD 1.1 (W3C XSD test suite) | 41,610 | 41,599 | 99.97 % |

The schema-aware test cases among them: XPath passes 314 of 315, XQuery
1,446 of 1,516 and XSLT 940 of 971. Other suites:

| Test suite | Passed |
| --- | --: |
| W3C XML Conformance Test Suite: valid, invalid and not well-formed documents, XML 1.0 and 1.1 | 2,237 of 2,237 |
| html5lib: tokenizer | 7,045 of 7,045 |
| html5lib: tree construction | 1,749 of 1,753 |
| html5lib: encoding | 82 of 82 |
| Invisible XML community test suite | 886 of 886 run (21 need another Unicode version or input no xs:string holds) |
| tree-sitter grammars' own tests (corpus and highlights) | 1,410 of 1,419 run (19 the grammars' authors mark to skip) |

[docs/CONFORMANCE.md](docs/CONFORMANCE.md) has the QT4, XSLT and XSD
results by test set.

## Design

- **Portable C17.** One code base, with no dependencies outside the
  repository.
- **No global state.** Every object lives in memory you control, and
  separate processors share nothing.
- **Your memory.** sxml never calls `malloc` itself. Every object takes
  an `sxml_allocator`, or is placed in memory you provide after a
  `memory_requirements` query.
- **No recursion.** Parsers and the evaluator use explicit stacks, so
  deeply nested documents and queries do not overflow the C stack. Depth
  and work are bounded by limits you configure.
- **One engine.** XPath, XQuery and XSLT compile to one intermediate
  representation and run on one evaluator.
- **No network access.** sxml reads only what your resource resolver
  gives it.

## Status

sxml 0.2.0 is in active development and has no tagged release yet.
Everything above works in the library, the `sxml` command and, where the
table says so, in JavaScript. The public C headers are stable. Not done
yet: builds for macOS and Windows, bindings for iOS and Android,
and Android and iOS bindings (in progress).

## License

sxml is released under the MIT License; see [LICENSE](LICENSE).

## Documentation

The website, [sxml.tinyactors.dev](https://sxml.tinyactors.dev), serves
the book at [/book/](https://sxml.tinyactors.dev/book/). `mise run site`
builds both from `website/` and `docs/book` into `build/site`;
`mise run site:publish` builds and publishes them.

- [The sxml Book](docs/book/src/SUMMARY.md): a guide to sxml and its
  languages, from installing it to XPath, XQuery, XSLT, XSD, other
  formats, and embedding it in C and JavaScript, taught on projects
  around one made-up feature-flag service and its two SDKs: a
  specification kept in xml2rfc XML, SVG diagrams, a self-describing
  SCXML machine, a diff of call chains, a diff of OpenTelemetry traces,
  and SDK clients generated from a schema. Its examples are in
  `docs/book/examples/`, one directory per project, and
  `tools/check-book-examples.sh` re-runs them. Build it with
  `mise run book` (or `mdbook build docs/book`); `mise run book:serve`
  serves it locally.
- [apps/sxml/README.md](apps/sxml/README.md): the `sxml` command.
- [include/sxml/](include/sxml/): the C API. Each header documents its
  types and functions; start with `processor.h` and `xpath.h`.
- [bindings/javascript/README.md](bindings/javascript/README.md): the
  JavaScript package.
- [docs/CONFORMANCE.md](docs/CONFORMANCE.md): conformance results.
- [CHANGELOG.md](CHANGELOG.md): changes by release.
