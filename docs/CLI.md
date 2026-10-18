# The sxml command

`sxml` checks, queries and transforms XML documents from the command line.
It runs XPath 4.0 expressions, XQuery 4.0 queries and XSLT 4.0 stylesheets
with the sxml library, and writes the results to standard output.

```console
$ sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'
"Godel, Escher, Bach"
"The Art of Computer Programming"
```

- [Getting sxml](#getting-sxml)
- [Usage](#usage)
- Commands: [check](#sxml-check), [xpath](#sxml-xpath),
  [xquery](#sxml-xquery), [transform](#sxml-transform),
  [serialize](#sxml-serialize), [version](#sxml-version),
  [help](#sxml-help)
- [Exit codes](#exit-codes)
- [Design](#design)

## Getting sxml

Build it from the repository with CMake 3.24 or later, Ninja and a C17
compiler:

```sh
cmake --preset release
cmake --build --preset release
build/release/apps/sxml --version
```

## Usage

```
sxml [-h | --help | --version] COMMAND [ARGUMENT...]
```

- Options before COMMAND belong to `sxml`; options after it belong to the
  command.
- Long options are written `--name=value` or `--name value`. `-h` is
  `--help`.
- Options and operands can be mixed. `--` ends the options: everything
  after it is an operand, even when it starts with `-`.
- A FILE operand of `-` reads standard input. sxml reads standard input
  only then.
- `sxml COMMAND --help` and `sxml help COMMAND` show the help of one
  command. `--help` wins over everything else on the command line.
- Results go to standard output. Errors and diagnostics go to standard
  error.
- sxml reads documents, modules and stylesheets from local files only. It
  never uses the network, never writes files, and reads no configuration
  files.

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

## sxml check

Check that XML documents are well-formed.

```
sxml check [--format=FORMAT] FILE...
```

Checks that each FILE is a well-formed XML 1.0 document that also conforms
to Namespaces in XML 1.0. The supported encodings are UTF-8, UTF-16,
ISO-8859-1 and US-ASCII. The external DTD subset and external entities are
not read.

sxml prints nothing when every FILE is well-formed. Each problem is
reported on standard error as `FILE:LINE:COLUMN: message`.

| Option | Description |
| --- | --- |
| `--format=FORMAT` | Report format: `text` (default) or `json` |

With `--format=json`, one JSON object on standard output lists every FILE
with its `status`: `well-formed`, `not-well-formed`, `unreadable` or
`failed`. A file that is not well-formed also has `line`, `column`,
`byte_offset` and `message`. Fields may be added to this object in later
versions; existing fields do not change.

Exit status: 0 when every FILE is well-formed, 1 when one is not, 3 when
one cannot be read, 4 when a limit is exceeded (entity expansion beyond
4 MiB).

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

## sxml xpath

Evaluate an XPath 4.0 expression.

```
sxml xpath [--context-item=FILE] [--param=NAME=VALUE]... [--method=METHOD] EXPRESSION
```

Evaluates EXPRESSION as XPath 4.0, with the F&O 4.0 function library, and
writes the result to standard output. By default the result is written by
the adaptive output method, one item per line.

| Option | Description |
| --- | --- |
| `--context-item=FILE` | Parse FILE and make it the context item. Without it there is no context item. |
| `--param=NAME=VALUE` | Bind `$NAME` to `xs:untypedAtomic(VALUE)`. Repeatable. |
| `--method=METHOD` | Serialization method: `adaptive` (default), `xml`, `xhtml`, `html`, `text` or `json`, each with its default parameters |

`fn:doc` and the other resource functions read local files. An EXPRESSION
that starts with `-` must follow `--`. An error is reported on standard
error as `expression:LINE:COLUMN: err:CODE: description`.

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
```

## sxml xquery

Run an XQuery 4.0 query.

```
sxml xquery [--context-item=FILE] [--expression=TEXT] [--param=NAME=VALUE]... [--method=METHOD] [QUERY-FILE]
```

Runs the XQuery 4.0 main module in QUERY-FILE, or the query that
`--expression` gives, and writes the result to standard output. Give
exactly one of the two. The result is serialized as the query's output
declarations say: by the xml method unless they name another.

| Option | Description |
| --- | --- |
| `--context-item=FILE` | Parse FILE and make it the context item |
| `--expression=TEXT` | The query itself, instead of QUERY-FILE |
| `--param=NAME=VALUE` | Bind the external variable `$NAME`, if the query declares it, to `xs:untypedAtomic(VALUE)`. Repeatable. |
| `--method=METHOD` | Replace the query's serialization method: `adaptive`, `xml`, `xhtml`, `html`, `text` or `json` |

Library modules that `import module ... at` names are read from local
files, relative to the working directory. `fn:doc` reads local files. An
error is reported on standard error as
`QUERY-FILE:LINE:COLUMN: err:CODE: description` (`expression` for
`--expression`). A static error in a library module names that module's
file.

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
```

## sxml transform

Transform a document with an XSLT 4.0 stylesheet.

```
sxml transform [--initial-mode=NAME] [--initial-template=NAME] [--method=METHOD] [--param=NAME=VALUE]... STYLESHEET [INPUT]
```

Compiles the XSLT 4.0 stylesheet STYLESHEET and runs one transformation.

- With INPUT, the document becomes the global context item. Templates are
  applied to it in the default mode, or in the mode that `--initial-mode`
  names. With `--initial-template`, that template is called instead, with
  INPUT as the context item.
- Without INPUT, the template that `--initial-template` names, else
  `xsl:initial-template`, is called without a context item.

The principal result goes to standard output, serialized as the
stylesheet's unnamed `xsl:output` says, with a final line feed when it has
none. An `xsl:result-document` without `href` is part of the principal
result; result documents with an `href` are not written. `xsl:message`
output goes to standard error, one message per line.

| Option | Description |
| --- | --- |
| `--initial-mode=NAME` | Mode whose template rules process INPUT, instead of the default mode. Needs INPUT; cannot be combined with `--initial-template`. |
| `--initial-template=NAME` | Named template to call, instead of applying templates to INPUT |
| `--method=METHOD` | Replace the stylesheet's serialization method: `adaptive`, `xml`, `xhtml`, `html`, `text` or `json` |
| `--param=NAME=VALUE` | Bind the stylesheet parameter or static parameter `$NAME` to `xs:untypedAtomic(VALUE)`. Repeatable. |

Names are NCNames or `Q{uri}local`. Included and imported modules, and
documents read with `fn:doc` or `document()`, are local files; relative
references resolve against the stylesheet's own path. An error is reported
on standard error as `STYLESHEET:LINE:COLUMN: err:CODE: description`.

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

Parse an XML document and write it out again.

```
sxml serialize [--indent] [--method=METHOD] FILE
```

Parses FILE and writes the document to standard output with a
Serialization 4.0 method and that method's default parameters. A FILE
that is not well-formed is reported on standard error as
`FILE:LINE:COLUMN: message`.

| Option | Description |
| --- | --- |
| `--indent` | Indent the output |
| `--method=METHOD` | Serialization method: `xml` (default), `adaptive`, `xhtml`, `html`, `text` or `json` |

Exit status: 0 on success, 1 when FILE is not well-formed or cannot be
serialized, 3 when FILE cannot be read.

```console
$ echo '<a><b>1</b><b>2</b></a>' | sxml serialize --indent -
<a>
  <b>1</b>
  <b>2</b>
</a>
$ cat page.xml
<html><head><title>Hi</title></head><body><p>One<br/>two</p></body></html>
$ sxml serialize --method=html page.xml
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hi</title></head><body><p>One<br>two</p></body></html>
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

Help goes to standard output and exits 0. Every command's help has a
usage line, a description, its options and examples.

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

A mistyped command or option is a usage error. sxml suggests the closest
name but never runs it:

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
| 1 | The operation failed: a document is not well-formed, or a query or stylesheet raised a static, dynamic or type error |
| 2 | Usage error: unknown command or option, missing or extra operand, bad option value |
| 3 | Input or output error: a file cannot be read, or a write to standard output or standard error failed (closed pipe, full disk) |
| 4 | Internal error: out of memory, a limit was exceeded, a bug in sxml |

## Design

These rules are binding for every command (D44). Sources: Command Line
Interface Guidelines (https://clig.dev), POSIX.1-2024 XBD 12.2 "Utility
Syntax Guidelines", GNU coding standards "Command-Line Interfaces",
NO_COLOR (https://no-color.org).

### One intent, one action

Every invocation does exactly what its words say, and nothing else.

- **Each command is one verb doing one thing.** `sxml check` never also
  prints the document. Two behaviours get two commands or an explicit flag,
  never a mode guessed from the input (file or expression, XML or JSON).
- **The command line fully determines behaviour.** No configuration files.
  The environment changes presentation only (color, once there is any).
- **No hidden side effects.** sxml writes a file only when `--output=FILE`
  names it, refuses to replace an existing file unless `--force` is given,
  never deletes anything, and never touches the network: `fn:doc`,
  `fn:unparsed-text` and friends resolve local files only.
- **Standard input is read only when `-` is given** as a file operand.
- **Nothing is advertised that is not implemented.** A command or option
  enters the program's table with its working code; planned ones are
  listed only under "Planned commands" below.
- **No abbreviations, no auto-correction.** `--form` is not `--format`
  (unlike GNU getopt_long); a typo gets a "did you mean" suggestion and exit
  code 2, never a guess. Commands have no aliases except `help`/`--help` and
  `version`/`--version`, which the guidelines ask for.
- **No repeated single options.** `--format=json --format=text` is a usage
  error, not "last one wins"; collecting options (`--param`) say
  "repeatable".

### Syntax

- `sxml [-h | --help | --version] COMMAND [ARGUMENT...]`. Options before the
  command belong to `sxml`; after it, to the command.
- Long options: `--name`, `--name=value`, `--name value`. Short options:
  `-q`, combined `-qv`, `-o FILE` or `-oFILE` (POSIX guidelines 3, 5, 6).
  Every short option has a long form.
- Options and operands may be mixed after the command (GNU argument
  permutation). `--` ends options: everything after it is an operand.
  `-` alone is an operand meaning standard input. An argument that starts
  with `-` (such as `-1`) must follow `--` to be an operand.
- A value-taking option always consumes the next argument: `--expression -1`.
- `-h`/`--help` works on every level and wins over everything else on the
  command line before `--`, including errors: `sxml version --bogus --help`
  shows help. `sxml help COMMAND` equals `sxml COMMAND --help`.
- `sxml` with no arguments is a usage error (exit 2) that prints the usage
  line: a script that forgot the command must not succeed.

### Output

- Results go to standard output; diagnostics, warnings and usage errors to
  standard error. Help requested with `--help` or `help` goes to standard
  output (it is the result) and exits 0.
- Usage errors: `sxml version: unknown option '--fromat'; did you mean
  '--format'?`, the usage line, `Try 'sxml version --help'.` Control bytes
  from arguments are shown as `\xHH`.
- Commands whose output another program may parse offer `--format=json`
  (sxml's own reports) next to the default `--format=text`. The JSON shape is
  a stable interface: fields are only ever added. Query results are not
  reports: they are serialized with `--method=` (Serialization 4.0 methods).
- Help texts: a one-line summary that starts with a verb, a description in
  short paragraphs ending with the command's exit status, and examples that
  work on the sample files of this page. This page and the help agree.
- No color, no pager, no progress output today. When color arrives it is off
  unless the stream is a terminal, off when `NO_COLOR` is set and not empty
  or `TERM=dumb`, and `--color=auto|always|never` overrides both.

### Planned commands

Later phases add these, with exactly these synopses. Shared options:
`--format=text|json` (report format), `--context-item=FILE` (parse FILE, or
stdin for `-`, as the context item), `--param=NAME=VALUE` (repeatable;
binds external variable `$NAME` to `xs:untypedAtomic(VALUE)`),
`--method=adaptive|xml|json|text` (serialization of results),
`--output=FILE` and `--force` (write results to a file; replace it).

| Phase | Synopsis | Exit 1 when |
| --- | --- | --- |
| 3 | `--output=FILE [--force]` for `xquery` and `serialize` (the CLI framework has no file writer yet) | as the command |
| 4 | `--output=FILE [--force]` for `transform`, and writing its result documents below FILE's directory (the CLI framework has no file writer yet) | as the command |
| 5 | `sxml validate --schema=XSD [--format=text\|json] FILE...` | a FILE is not valid against XSD |

The QT4 and XSLT 4.0 conformance drivers are not `sxml` commands but
developer tools under `tests/` built with the same framework (D44).

### Implementation map

- `apps/cli_framework/`: declarative command tables (`cli_command.h`), the
  argument parser (`cli_arguments.h`), help and usage errors (`cli_help.h`),
  "did you mean" (`cli_suggestion.h`), output streams and writers
  (`cli_output.h`), dispatch and exit codes (`cli_run.h`). It never prints or
  opens files: all I/O goes through `sxml_cli_output_stream`.
- `apps/sxml/`: the program table with the program's description and
  examples (`program.c`), the command handlers (`command_<name>.c`),
  `main.c`, and the only file that touches stdio, `platform_stdio.c`. The
  query commands `xpath`, `xquery` and `serialize` share (and `transform`
  reuses) their processor, the local-file resolver, `--param`,
  `--context-item`, the `--method` choices, error reports and result
  writing through `query_command.h`. A new command is one
  `command_<name>.c` plus one table entry, imitating `command_version.c`.
  Files are read through the invocation's `sxml_cli_file_reader`
  (`cli_command.h`), which the platform layer implements over stdio
  (`command_check.c` uses it).
- Tests, all in `tests/cli/` (registered from `apps/CMakeLists.txt`, outside
  the `tests/*_test.c` glob): framework unit tests `cli_*_test.c`, and
  end-to-end golden tests (`sxml_add_cli_test`: arguments, exit code,
  `expected/<name>.stdout` and `.stderr`; `SXML_UPDATE_GOLDEN=1` rewrites).
  They run in `tests/cli/input/`, which holds the input files. The help
  texts have golden files too (`expected/*_help.stdout`): a change to a
  help text updates its golden file and this page together.
