# Appendix A: The Command-Line Reference

The full reference of the `sxml` command, with every option of every
command, its exit codes and many examples, is the command's manual,
[`apps/sxml/README.md`](https://github.com/tinyactors-dev/sxml/blob/main/apps/sxml/README.md)
in the repository. It is also `USAGE.md` in each release archive, and
`sxml help COMMAND` prints the help of one command.

At a glance:

| Command | What it does | Taught in |
| --- | --- | --- |
| `sxml xpath [OPTIONS] EXPRESSION` | Evaluates an XPath 4.0 expression | [Finding Things with XPath](xpath.md) |
| `sxml xquery [OPTIONS] [QUERY-FILE]` | Runs an XQuery 4.0 query, from a file or `--expression` | [Writing Programs with XQuery](xquery.md) |
| `sxml transform [OPTIONS] STYLESHEET [INPUT]` | Runs an XSLT 4.0 transformation | [Transforming with XSLT](xslt.md) |
| `sxml validate [OPTIONS] FILE...` | Validates documents against an XSD 1.1 schema | [Validating with XSD](xsd.md) |
| `sxml check [OPTIONS] FILE...` | Checks that documents are well-formed, or valid against their DTD | [The sxml Command](the-sxml-command.md) |
| `sxml serialize [OPTIONS] FILE` | Parses a document and writes it again | [The sxml Command](the-sxml-command.md) |
| `sxml version` | Prints the version | |
| `sxml help [COMMAND]` | Shows help | |

The options you'll use most:

| Option | Commands | Meaning |
| --- | --- | --- |
| `--context-item=FILE` | `xpath`, `xquery` | The document the expression starts from |
| `--expression=TEXT` | `xquery` | The query text, instead of a query file |
| `--param=NAME=VALUE` | `xpath`, `xquery`, `transform` | Binds `$NAME` to VALUE as `xs:untypedAtomic`; an external variable or stylesheet parameter declared with an `as` type converts it, and text that does not convert is an error (`FORG0001` for a query, `XTTE0590` for a stylesheet parameter) |
| `--method=METHOD` | `xpath`, `xquery`, `transform`, `serialize` | The output method: `adaptive`, `xml`, `xhtml`, `html`, `text`, `json` or `csv` |
| `--output=FILE`, `-o FILE` | `xpath`, `xquery`, `transform`, `serialize` | Writes the result to a file, only when the command succeeds |
| `--force` | the same | Lets `--output` replace a file that exists |
| `--result-directory=DIR` | `transform` | Writes result documents below DIR |
| `--initial-template=NAME`, `--initial-mode=NAME` | `transform` | Where the transformation starts |
| `--allow-put` | `xquery` | Lets `fn:put` write files |
| `--allow-file-access` | `xpath`, `xquery` | Makes the EXPath File functions available |
| `--collection=DIR` | `xpath`, `xquery`, `transform` | The XML files of DIR are the default collection, `collection()` |
| `--code-language=LIBRARY` | `xpath`, `xquery`, `transform` | Loads a tree-sitter language from a shared library for `tree-sitter:parse`; repeatable |
| `--external-entities`, `--validate-dtd` | `check` | Reads external DTDs and entities; validates against the DTD |
| `--schema=SCHEMA` | `validate` | A schema document; repeat it to combine several into one schema |
| `--use-schema-location` | `validate` | Also reads the schema documents a document's `xsi:schemaLocation` names |
| `--mode=lax` | `validate` | Lax validation |
| `--indent` | `serialize` | Indents the output |
| `--format=json` | `check`, `validate`, `version` | A report as JSON, for programs |

| Exit status | Meaning |
| --- | --- |
| 0 | Success |
| 1 | The operation failed: not well-formed, not valid, or an error in a query or stylesheet |
| 2 | Usage error |
| 3 | A file could not be read or written |
| 4 | An internal error or an exceeded limit |
