# sxml

sxml is a small, embeddable XML toolchain written in portable C. It parses
XML, evaluates XPath 4.0 and XQuery 4.0, and runs XSLT 4.0 stylesheets, as
a library and as the `sxml` command.

## What it does

- **XML 1.0** with namespaces: parsing into a tree, from UTF-8, UTF-16,
  ISO-8859-1 and US-ASCII.
- **XPath 4.0** with the full F&O 4.0 function library: maps, arrays,
  higher-order functions, regular expressions, dates and times, decimal
  arithmetic.
- **XQuery 4.0**: FLWOR expressions with grouping and windows, node
  constructors, library modules, try/catch.
- **XSLT 4.0**: template rules and modes, grouping, sorting, keys,
  numbering, accumulators, packages and result documents.
- **Serialization 4.0**: the xml, xhtml, html, text, json, csv and adaptive
  output methods.
- **JSON and CSV**: `fn:parse-json`, `fn:json-to-xml`, `fn:xml-to-json`,
  `fn:parse-csv` and `fn:csv-to-xml`.

All four languages share one data model (XDM 4.0) and one evaluator.

## Where it runs

- **Linux, macOS and Windows**: a C library, static or shared, built with
  CMake.
- **Browsers and Node.js**: a WebAssembly module with a TypeScript
  wrapper, in [`bindings/javascript`](bindings/javascript).
- **iOS and Android**: planned. Swift and Kotlin bindings are not
  available yet.

## Quick start

You need CMake 3.24 or later, Ninja and a C17 compiler (Clang or GCC; MSVC
on Windows has its own preset, `msvc`).

```sh
cmake --preset release
cmake --build --preset release
export PATH="$PWD/build/release/apps:$PATH"
```

The examples below use this file, `books.xml`:

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

Evaluate XPath 4.0:

```console
$ sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'
"Godel, Escher, Bach"
"The Art of Computer Programming"
$ sxml xpath --context-item=books.xml 'sum(//price ! xs:decimal(.))'
257.5
$ sxml xpath --method=json 'map { "a": [1, 2] }'
{"a":[1,2]}
```

Run an XQuery 4.0 query. `titles.xq`:

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

Transform with XSLT 4.0. `list.xsl`:

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

`sxml help` lists every command; [docs/CLI.md](docs/CLI.md) is the full
reference.

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

```console
$ ./books
"The Art of Computer Programming"
```

XQuery (`sxml/xquery.h`) and XSLT (`sxml/xslt.h`) follow the same pattern:
compile once, evaluate many times, with the same processor, contexts and
serializer.

## Use it from JavaScript

The `sxml-js` package in [`bindings/javascript`](bindings/javascript) runs
the same library as WebAssembly. It covers XML parsing, XPath 4.0 and
serialization; XQuery and XSLT are not exposed in JavaScript yet. The
package is not on npm yet: build it from this repository as its README
describes.

```js
import { load } from 'sxml-js';

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

const data = processor.evaluate('map { "count": count(//book) }', { context: books });
console.log(data.serialize({ method: 'json' }));
// {"count":2}

processor.dispose();
```

The package's [README](bindings/javascript/README.md) describes its API.

## Standards conformance

sxml is tested against the W3C test suites for XPath, XQuery and XSLT 4.0.
Pass rates of the test cases that apply to sxml:

| Test suite | Test cases run | Passed | Rate |
| --- | --: | --: | --: |
| QT4, XPath 4.0 and F&O 4.0 (XP40) | 30,960 | 30,646 | 99.0 % |
| QT4, XQuery 4.0 (XQ40) | 40,447 | 40,025 | 99.0 % |
| XSLT 4.0 (XT40) | 8,484 | 7,879 | 92.9 % |

Test cases that need a feature sxml does not have (schema awareness,
XQuery Update, Full Text, EXPath modules) are not run.
[docs/CONFORMANCE.md](docs/CONFORMANCE.md) has the full results, by test
set and by failure cause.

## Design

- **Portable C17.** One code base for every platform, with no
  dependencies outside the repository.
- **No global state.** Every object lives in memory you control, and
  separate processors share nothing.
- **Caller-controlled memory.** sxml never calls `malloc` itself. Every
  object takes an `sxml_allocator`, or is placed in memory you provide
  after a `memory_requirements` query.
- **No recursion.** Parsers and the evaluator use explicit stacks, so
  deeply nested documents and queries do not overflow the C stack. Depth
  and work are bounded by limits you configure.
- **One engine.** XPath, XQuery and XSLT compile to one intermediate
  representation and run on one evaluator.
- **No network access.** sxml reads only what your resource resolver
  gives it.

## Status

sxml is under active development and has no tagged release yet. XML
parsing, XPath 4.0, XQuery 4.0 and serialization are implemented, and
their C headers are stable. XSLT 4.0 is still being finished: `xslt.h` may change.
Schema validation (XSD 1.1) and schema-aware processing are not available
yet.

## License

The repository does not have a license file yet.

## Documentation

- [docs/CLI.md](docs/CLI.md): the `sxml` command.
- [include/sxml/](include/sxml/): the C API. Each header documents its
  types and functions; start with `processor.h` and `xpath.h`.
- [bindings/javascript/README.md](bindings/javascript/README.md): the
  JavaScript package.
- [docs/CONFORMANCE.md](docs/CONFORMANCE.md): conformance results.
- [CHANGELOG.md](CHANGELOG.md): changes by release.
