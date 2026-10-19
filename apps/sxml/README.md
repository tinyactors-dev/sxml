# sxml

`sxml` is the command-line program of the sxml library. It checks XML
documents, validates them against XSD 1.1 schemas, evaluates XPath 4.0
expressions and XQuery 4.0 queries, runs XSLT 4.0 stylesheets, and writes
the results to standard output.

```console
$ sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'
"Godel, Escher, Bach"
"The Art of Computer Programming"
```

- [Install](#install)
- [Usage](#usage)
- [Sample files](#sample-files)
- Commands: [xpath](#sxml-xpath), [xquery](#sxml-xquery),
  [transform](#sxml-transform), [serialize](#sxml-serialize),
  [check](#sxml-check), [validate](#sxml-validate),
  [version](#sxml-version), [help](#sxml-help)
- [Exit codes](#exit-codes)

The design rules that every command follows are in
[docs/CLI.md](../../docs/CLI.md).

## Install

Build `sxml` from the repository with CMake 3.24 or later, Ninja and a C17
compiler:

```sh
cmake --preset release
cmake --build --preset release
export PATH="$PWD/build/release/apps:$PATH"
sxml --version
```

## Usage

```
sxml [-h | --help | --version] COMMAND [ARGUMENT...]
```

- Options before COMMAND belong to `sxml`. Options after it belong to the
  command.
- Write long options as `--name=value` or `--name value`. `-h` is
  `--help`.
- Options and operands can be mixed. `--` ends the options: every argument
  after it is an operand, also when it starts with `-`.
- A FILE operand of `-` reads standard input. `sxml` reads standard input
  only then.
- `sxml COMMAND --help` and `sxml help COMMAND` show the help of one
  command. `--help` wins over all other arguments.
- Results go to standard output. Errors and diagnostics go to standard
  error.
- `sxml` reads documents, schemas, modules and stylesheets from local files
  only. It does not use the network and reads no configuration files. It
  writes files only when `sxml xpath` or `sxml xquery` is given
  `--allow-file-access`.

## Sample files

The examples on this page use these files.

`books.xml`:

```xml
<books>
  <book year="1979"><title>Godel, Escher, Bach</title><price>22.50</price></book>
  <book year="1968"><title>The Art of Computer Programming</title><price>180.00</price></book>
  <book year="1986"><title>Structure and Interpretation of Computer Programs</title><price>55.00</price></book>
</books>
```

`titles.xq`:

```xquery
<titles>{
  for $book in //book
  order by xs:integer($book/@year)
  return <title year="{$book/@year}">{string($book/title)}</title>
}</titles>
```

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

`hello.xsl`:

```xml
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">Hello from XSLT 4.0</xsl:template>
</xsl:stylesheet>
```

`page.xml`:

```xml
<html><head><title>Hi</title></head><body><p>One<br/>two</p></body></html>
```

`books.xsd`, an XSD schema for `books.xml`:

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

`bad-books.xml`, which is not valid against `books.xsd`:

```xml
<books>
  <book year="1979"><title>Godel, Escher, Bach</title><price>cheap</price></book>
  <book year="soon"><price>180.00</price></book>
</books>
```

## sxml xpath

Evaluate an XPath 4.0 expression.

```
sxml xpath [--context-item=FILE] [--param=NAME=VALUE]... [--method=METHOD] [--allow-file-access] EXPRESSION
```

`sxml xpath` evaluates EXPRESSION as XPath 4.0, with the F&O 4.0 function
library, and writes the result to standard output. By default, the
adaptive output method writes the result, one item on each line.

| Option | Description |
| --- | --- |
| `--context-item=FILE` | Parse FILE and make it the context item. Without this option, there is no context item. |
| `--param=NAME=VALUE` | Bind `$NAME` to `xs:untypedAtomic(VALUE)`. Repeatable. |
| `--method=METHOD` | Serialization method: `adaptive` (default), `xml`, `xhtml`, `html`, `text` or `json`, each with its default parameters |
| `--allow-file-access` | Let the expression read and write files with the EXPath File functions (`file:`). Without this option, they do not exist. |

- `fn:doc` and the other resource functions read local files.
- The EXPath Binary 4.0 functions are available with the prefix `bin`
  (`http://expath.org/ns/binary`). With `--allow-file-access`, the EXPath
  File 4.0 functions are available with the prefix `file`
  (`http://expath.org/ns/file`). Relative paths are resolved against the
  working directory.
- `fn:transform` runs an XSLT 4.0 stylesheet from the expression; its
  `stylesheet-location` and `source-location` are read as `fn:doc` reads
  files.
- `fn:civil-timezone` and the `$place` argument of `fn:format-dateTime`,
  `fn:format-date` and `fn:format-time` read the timezone database in
  `/usr/share/zoneinfo`: an IANA zone name such as `Europe/Paris`, or a
  country code such as `us` for `[ZN]`. Without it, only `UTC` is known.
- An EXPRESSION that starts with `-` must come after `--`.
- An error is reported on standard error as
  `expression:LINE:COLUMN: err:CODE: description`.

Exit status: 0 on success; 1 for a static, type or dynamic error
(serialization errors included) or a context document that is not
well-formed; 2 for a bad `--param` or `--method`; 3 when a file cannot be
read; 4 when a limit is exceeded.

```console
$ sxml xpath '1 to 3'
1
2
3
$ sxml xpath --context-item=books.xml 'sum(//price ! xs:decimal(.))'
257.5
$ sxml xpath --context-item=books.xml --method=xml '//book[1]/title'
<title>Godel, Escher, Bach</title>
$ sxml xpath --param=n=5 'xs:integer($n) * 2'
10
$ sxml xpath --method=json 'map { "a": [1, 2] }'
{"a":[1,2]}
$ sxml xpath -- '-1 + 3'
2
$ echo '<p>hi</p>' | sxml xpath --context-item=- 'string(/p)'
"hi"
$ sxml xpath '1 div 0'
expression:1:1: err:FOAR0001: division by zero (F&O 4.0, op:numeric-divide, op:numeric-integer-divide, op:numeric-mod)
$ sxml xpath "bin:hex('CAFE') => bin:to-octets()"
202
254
$ sxml xpath --allow-file-access "file:exists('books.xml'), file:size('books.xml')"
true()
306
$ sxml xpath "file:exists('books.xml')"
expression:1:1: err:XPST0017: no function with this name and arity is known
```

## sxml xquery

Run an XQuery 4.0 query.

```
sxml xquery [--context-item=FILE] [--expression=TEXT] [--param=NAME=VALUE]... [--method=METHOD] [--allow-file-access] [QUERY-FILE]
```

`sxml xquery` runs the XQuery 4.0 main module in QUERY-FILE, or the query
that `--expression` gives, and writes the result to standard output. Give
exactly one of the two. The output declarations of the query control the
serialization. The default method is xml.

| Option | Description |
| --- | --- |
| `--context-item=FILE` | Parse FILE and make it the context item |
| `--expression=TEXT` | The query text, instead of QUERY-FILE |
| `--param=NAME=VALUE` | Bind the external variable `$NAME`, if the query declares it, to `xs:untypedAtomic(VALUE)`. Repeatable. |
| `--method=METHOD` | Replace the serialization method of the query: `adaptive`, `xml`, `xhtml`, `html`, `text` or `json` |
| `--allow-file-access` | Let the query read and write files with the EXPath File functions, as for [sxml xpath](#sxml-xpath) |

- `import module ... at` reads library modules from local files, relative
  to the working directory. `import schema ... at` reads schema documents
  in the same way, so `validate` expressions and schema types are
  available.
- `fn:doc` reads local files.
- An updating query (the XQuery Update Facility 3.0: insert, delete,
  replace, rename, updating functions) writes the context item's document
  as its updates left it, in place of its value, which is empty. The file
  itself is not changed; copy-modify and transform-with work in any query.
- An error is reported on standard error as
  `QUERY-FILE:LINE:COLUMN: err:CODE: description`, or with `expression`
  in place of QUERY-FILE for `--expression`. A static error in a library
  module names the file of that module.

Exit status: as for [sxml xpath](#sxml-xpath).

```console
$ sxml xquery --context-item=books.xml titles.xq
<titles><title year="1968">The Art of Computer Programming</title><title year="1979">Godel, Escher, Bach</title><title year="1986">Structure and Interpretation of Computer Programs</title></titles>
$ sxml xquery --expression='for $i in 1 to 3 return <i>{$i}</i>'
<i>1</i><i>2</i><i>3</i>
$ sxml xquery --context-item=books.xml --method=text --expression='string-join(//title, "; ")'
Godel, Escher, Bach; The Art of Computer Programming; Structure and Interpretation of Computer Programs
$ sxml xquery --param=n=5 --expression='declare variable $n external; $n * 2'
10
$ sxml xquery --context-item=books.xml --method=text --expression='copy $b := //book[1] modify replace value of node $b/title with "TAOCP" return string($b/title)'
TAOCP
$ sxml xquery --context-item=books.xml --expression='import schema default element namespace "" at "books.xsd"; (validate { /books })//book[1]/@year instance of attribute(*, xs:gYear)'
true
$ sxml xquery --allow-file-access --method=text --expression='file:write-text-lines("notes.txt", ("one", "two")), file:read-text-lines("notes.txt")[2]'
two
```

## sxml transform

Transform a document with an XSLT 4.0 stylesheet.

```
sxml transform [--initial-mode=NAME] [--initial-template=NAME] [--method=METHOD] [--param=NAME=VALUE]... STYLESHEET [INPUT]
```

`sxml transform` compiles the XSLT 4.0 stylesheet STYLESHEET and runs one
transformation.

- With INPUT, the document becomes the global context item. The
  transformation applies templates to it in the default mode, or in the
  mode that `--initial-mode` names. With `--initial-template`, it calls
  that template instead, with INPUT as the context item.
- Without INPUT, it calls the template that `--initial-template` names,
  or else `xsl:initial-template`, without a context item.

The principal result goes to standard output. The unnamed `xsl:output` of
the stylesheet controls the serialization. A final line feed is added
when the result does not end with one. An `xsl:result-document` without
`href` is part of the principal result. Result documents with an `href`
are not written. `xsl:message` output goes to standard error, one message
on each line.

A stylesheet, or an element, with `version="1.0"` runs with XSLT 1.0
behaviour (XSLT 4.0, 3.8.1): its XPath expressions use XPath 1.0
compatibility mode, `xsl:value-of` and attribute value templates take the
first item, and an extension function without an implementation raises
XTDE1425 only when called.

| Option | Description |
| --- | --- |
| `--initial-mode=NAME` | The mode whose template rules process INPUT, instead of the default mode. Needs INPUT. Cannot be used with `--initial-template`. |
| `--initial-template=NAME` | The named template to call, instead of applying templates to INPUT |
| `--method=METHOD` | Replace the serialization method of the stylesheet: `adaptive`, `xml`, `xhtml`, `html`, `text` or `json` |
| `--param=NAME=VALUE` | Bind the stylesheet parameter or static parameter `$NAME` to `xs:untypedAtomic(VALUE)`. Repeatable. |

- NAME is an NCName or `Q{uri}local`.
- Included and imported modules, schemas that `xsl:import-schema` names,
  and documents that `fn:doc` or `document()` reads are local files.
  Relative references resolve against the path of the stylesheet.
- An error is reported on standard error as
  `STYLESHEET:LINE:COLUMN: err:CODE: description`.

Exit status: 0 on success; 1 for a static, type or dynamic error
(`xsl:message terminate="yes"` included) or a STYLESHEET or INPUT that is
not well-formed; 2 for a bad option; 3 when a file cannot be read; 4 when
a limit is exceeded.

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
$ sxml transform --param='heading=Reading list' list.xsl books.xml | grep h1
    <h1>Reading list</h1>
$ sxml transform --method=xml list.xsl - < books.xml | head -2
<?xml version="1.0" encoding="UTF-8"?>
<html>
$ sxml transform hello.xsl
Hello from XSLT 4.0
$ sxml transform --initial-template=main hello.xsl
hello.xsl: err:XTDE0040: the stylesheet has no template of the initial template's name (XSLT 4.0, 2.4.4)
```

## sxml serialize

Parse an XML document and write it again.

```
sxml serialize [--indent] [--method=METHOD] FILE
```

`sxml serialize` parses FILE and writes the document to standard output
with a Serialization 4.0 method and the default parameters of that
method. A FILE that is not well-formed is reported on standard error as
`FILE:LINE:COLUMN: message`.

| Option | Description |
| --- | --- |
| `--indent` | Indent the output |
| `--method=METHOD` | Serialization method: `xml` (default), `adaptive`, `xhtml`, `html`, `text` or `json` |

Exit status: 0 on success; 1 when FILE is not well-formed or cannot be
serialized; 3 when FILE cannot be read.

```console
$ echo '<a><b>1</b><b>2</b></a>' | sxml serialize --indent -
<a>
  <b>1</b>
  <b>2</b>
</a>
$ sxml serialize --method=html page.xml
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hi</title></head><body><p>One<br>two</p></body></html>
```

## sxml check

Check that XML documents are well-formed.

```
sxml check [--external-entities] [--validate-dtd] [--format=FORMAT] FILE...
```

`sxml check` checks that each FILE is a well-formed XML 1.0 document that
also conforms to Namespaces in XML 1.0. The supported encodings are UTF-8,
UTF-16, ISO-8859-1 and US-ASCII. The external DTD subset and external
entities are read only with `--external-entities`.

When every FILE is well-formed, `sxml check` writes nothing. Each problem
is reported on standard error as `FILE:LINE:COLUMN: message`.

| Option | Description |
| --- | --- |
| `--external-entities` | Read the external DTD subset and external entities as local files |
| `--validate-dtd` | Also check that each FILE is valid against its DTD |
| `--format=FORMAT` | Report format: `text` (default) or `json` |

Each flag does one thing:

- `--external-entities` reads the external DTD subset, external parameter
  entities and external parsed entities. A system identifier is resolved
  against the file that declares it (for standard input, against the
  working directory), and only local files are read. Without the flag,
  nothing outside FILE is read, and references to external entities are
  skipped (XML 1.0, section 4.4.3).
- `--validate-dtd` also checks that each FILE is valid against its DTD
  (XML 1.0, section 5.1), and reports the first validity error of each
  FILE as `FILE:LINE:COLUMN: CODE: message`. CODE is the XSD constraint
  that sxml checks the declarations with (`cvc-...`), or `xml-validity`
  for the constraints of XML 1.0 itself. A FILE without a document type
  declaration is not valid. The flag reads no external entity: give
  `--external-entities` too when the DTD is in an external subset.

With `--format=json`, one JSON object on standard output lists each FILE
with its `status`: `well-formed`, `not-well-formed`, `unreadable` or
`failed`; with `--validate-dtd`, `valid` and `invalid` take the place of
`well-formed`. A file that is not well-formed also has `line`, `column`,
`byte_offset` and `message`; an invalid file has `code` too. Later
versions can add fields to this object. Existing fields do not change.

Exit status: 0 when every FILE is well-formed (with `--validate-dtd`,
valid); 1 when a FILE is not; 3 when a FILE cannot be read; 4 when a
limit is exceeded (entity expansion beyond 4 MiB).

```console
$ sxml check books.xml
$ echo '<a><b/></a' > broken.xml
$ sxml check broken.xml
broken.xml:2:1: not well-formed: the document ends or breaks off before a construct is complete (XML 1.0, section 2.1)
$ sxml check --format=json books.xml broken.xml
{"files":[{"file":"books.xml","status":"well-formed"},{"file":"broken.xml","status":"not-well-formed","line":2,"column":1,"byte_offset":11,"message":"not well-formed: the document ends or breaks off before a construct is complete (XML 1.0, section 2.1)"}]}
$ sxml check missing.xml
sxml check: cannot read 'missing.xml'
```

With a DTD (the files of `tests/cli/input/dtd/`): `article.xml` names the
external subset `article.dtd`, which declares the external entity
`chapter.ent`; `broken-entity.xml` refers to an entity that is not
well-formed; `invalid.xml` has a second `title` that its DTD does not
allow.

```console
$ sxml check article.xml broken-entity.xml
$ sxml check --external-entities article.xml broken-entity.xml
broken-entity.xml:5:7: an external parsed entity referenced in content must match [78] extParsedEnt (XML 1.0, section 4.3.2)
$ sxml check --validate-dtd --external-entities article.xml invalid.xml
invalid.xml:6:28: cvc-complex-type.2.4: element 'title' is not allowed here; expected no element
$ sxml check --validate-dtd article.xml
article.xml:3:1: cvc-elt.1: no global declaration governs element 'article'
$ sxml check --validate-dtd --external-entities --format=json article.xml invalid.xml
{"files":[{"file":"article.xml","status":"valid"},{"file":"invalid.xml","status":"invalid","line":6,"column":28,"byte_offset":126,"code":"cvc-complex-type.2.4","message":"element 'title' is not allowed here; expected no element"}]}
```

## sxml validate

Validate XML documents against an XSD 1.1 schema.

```
sxml validate --schema=SCHEMA [--schema=SCHEMA]... [--mode=MODE] [--format=FORMAT] FILE...
```

`sxml validate` validates each FILE against the XSD 1.1 schema that the
SCHEMA documents make. The schema contains all of them, and each document
that they include, import, redefine or override. These are local files,
relative to the document that refers to them. Each FILE is parsed and
validated in one pass. `sxml validate` only checks: it writes no
document.

The schema is compiled first. Each error in it is reported on standard
error as `SCHEMA:LINE:COLUMN: CONSTRAINT: description`, where SCHEMA is
the document that has the error. For a SCHEMA that is not well-formed,
the location is where the parser stopped. When the schema has errors, no
FILE is read. A document that an `xs:include` or `xs:import` names but
that cannot be read is not an error by itself (XSD 1.1 Part 1, 4.2.3):
its components are missing, and `src-resolve` is reported where they are
used.

When every FILE is valid, `sxml validate` writes nothing. Each error in a
FILE is reported on standard error as
`FILE:LINE:COLUMN: CONSTRAINT: description`. All errors are reported, not
only the first. CONSTRAINT is the name that XSD 1.1 gives the rule that
failed, for example `cvc-complex-type.2.4` (XSD 1.1 Part 1, appendix C).
The description names the element or attribute, the value that is not
valid, and what was expected: the elements that the content model allows
there, the type, or the facet. A FILE that is not well-formed is reported
as `sxml check` reports it.

| Option | Description |
| --- | --- |
| `--schema=SCHEMA` | An XSD schema document; `-` reads standard input. Required; repeatable: all of them make one schema. |
| `--mode=MODE` | `strict` (default): the schema must have a global declaration for the document element of each FILE. `lax`: a document element without a declaration is assessed laxly, and the validity of the FILE is "not known". |
| `--format=FORMAT` | Report format: `text` (default) or `json` |

- With `--mode=lax`, the elements and attributes that have a declaration
  are validated, wherever they are. A FILE whose validity is not known is
  reported on standard error as `FILE: validity not known: ...`, but it
  does not fail.
- `sxml validate` does not follow `xsi:schemaLocation` and
  `xsi:noNamespaceSchemaLocation` hints in a FILE: the schema is the one
  that `--schema` names. To use a hinted document, name it with
  `--schema`.
- Standard input is read at most once. `-` given two times, as a SCHEMA
  and a FILE or as two of either, is a usage error.

With `--format=json`, one JSON object on standard output gives the
`schema` with its `documents` and its `status`, and lists each FILE in
`files` with its `status`:

- The status of the schema is `valid`, `invalid`, `unreadable` or
  `failed`. An invalid schema has `errors`, each with `document`, `code`
  (the constraint name), `line`, `column`, `byte_offset` and `message`,
  and `files` is empty.
- The status of a FILE is `valid`, `invalid`, `not-known` (with
  `--mode=lax`), `not-well-formed`, `unreadable` or `failed`. An invalid
  FILE has `errors` as above, without `document`. A FILE that is not
  well-formed has `line`, `column`, `byte_offset` and `message`, as for
  `sxml check`.
- Later versions can add fields to this object. Existing fields do not
  change.

Exit status: 0 when every FILE is valid (or not known, with
`--mode=lax`); 1 when the schema or a FILE is not valid or a FILE is not
well-formed; 2 for a usage error; 3 when a SCHEMA or a FILE cannot be
read; 4 when a limit is exceeded or validation needs a feature that sxml
does not have yet. 3 has priority over 4, and 4 over 1.

```console
$ sxml validate --schema=books.xsd books.xml
$ sxml validate --schema=books.xsd bad-books.xml
bad-books.xml:2:55: cvc-datatype-valid.1.2.1: element 'price' has the value 'cheap', which is not valid for type 'xs:decimal'
bad-books.xml:3:9: cvc-datatype-valid.1.2.1: attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'
bad-books.xml:3:21: cvc-complex-type.2.4: element 'price' is not allowed here; expected 'title'
$ sxml validate --schema=books.xsd --format=json books.xml
{"schema":{"documents":["books.xsd"],"status":"valid"},"files":[{"file":"books.xml","status":"valid"}]}
$ sxml validate --schema=books.xsd --format=json bad-books.xml
{"schema":{"documents":["books.xsd"],"status":"valid"},"files":[{"file":"bad-books.xml","status":"invalid","errors":[{"code":"cvc-datatype-valid.1.2.1","line":2,"column":55,"byte_offset":62,"message":"element 'price' has the value 'cheap', which is not valid for type 'xs:decimal'"},{"code":"cvc-datatype-valid.1.2.1","line":3,"column":9,"byte_offset":98,"message":"attribute 'year' has the value 'soon', which is not valid for type 'xs:gYear'"},{"code":"cvc-complex-type.2.4","line":3,"column":21,"byte_offset":110,"message":"element 'price' is not allowed here; expected 'title'"}]}]}
$ echo '<shelf><books/></shelf>' | sxml validate --schema=books.xsd --mode=lax -
-:1:8: cvc-complex-type.2.4: the content of element 'books' is incomplete; expected 'book'
$ echo '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:element name="books" type="catalog"/></xs:schema>' | sxml validate --schema=- books.xml
-:1:56: src-resolve: A QName value names no component of its kind: catalog
$ sxml validate --schema=books.xsd missing.xml
sxml validate: cannot read 'missing.xml'
```

## sxml version

Print the sxml version.

```
sxml version [--format=FORMAT]
sxml --version
```

| Option | Description |
| --- | --- |
| `--format=FORMAT` | `text` (default) or `json` |

```console
$ sxml version
sxml 0.2.0
$ sxml version --format=json
{"name":"sxml","version":"0.2.0"}
```

## sxml help

Show help for sxml or for one command.

```
sxml help [COMMAND]
sxml --help
sxml COMMAND --help
```

Help goes to standard output, and the exit status is 0. The help of each
command has a usage line, a description, the options and examples.

```console
$ sxml help version
sxml version - Print the sxml version

Usage: sxml version [--format=FORMAT]

Prints the version of the sxml library, the same line as
'sxml --version'. With --format=json, prints one JSON object with
the fields "name" and "version".

Options:
  -h, --help           Show this help and exit
      --format=FORMAT  Output format (one of: text, json; default: text)

Examples:
  sxml version
  sxml version --format=json
```

A mistyped command or option is a usage error. `sxml` suggests the
nearest name, but does not run it:

```console
$ sxml chek books.xml
sxml: unknown command 'chek'; did you mean 'check'?
Usage: sxml [--version] COMMAND [ARGUMENT...]
Try 'sxml --help'.
```

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | The operation failed: a document is not well-formed or not valid, a schema has errors, or a query or stylesheet raised a static, dynamic or type error |
| 2 | Usage error: unknown command or option, missing or extra operand, bad option value |
| 3 | Input or output error: a file cannot be read, or a write to standard output or standard error failed (closed pipe, full disk) |
| 4 | Internal error: out of memory, a limit was exceeded, a bug in sxml |
