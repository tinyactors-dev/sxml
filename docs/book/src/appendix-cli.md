# Appendix A: The Command-Line Reference

The full reference of the `sxml` command, with every option of every
command, its exit codes and many examples, is the command's manual,
[`apps/sxml/README.md`](https://github.com/dhamidi/sxml/blob/main/apps/sxml/README.md)
in the repository. It is also `USAGE.md` in each release archive, and
`sxml help COMMAND` prints the help of one command.

At a glance:

| Command | What it does | Chapter |
| --- | --- | --- |
| `sxml xpath [OPTIONS] EXPRESSION` | Evaluates an XPath 4.0 expression | [2](xpath.md) |
| `sxml xquery [OPTIONS] [QUERY-FILE]` | Runs an XQuery 4.0 query, from a file or `--expression` | [3](xquery.md) |
| `sxml transform [OPTIONS] STYLESHEET [INPUT]` | Runs an XSLT 4.0 transformation | [4](xslt.md) |
| `sxml validate [--schema=SCHEMA]... FILE...` | Validates documents against an XSD 1.1 schema | [5](xsd.md) |
| `sxml check [OPTIONS] FILE...` | Checks that documents are well-formed, or valid against their DTD | [1](the-sxml-command.md) |
| `sxml serialize [OPTIONS] FILE` | Parses a document and writes it again | [1](the-sxml-command.md) |
| `sxml version` | Prints the version | |
| `sxml help [COMMAND]` | Shows help | |

The options you'll use most:

| Option | Commands | Meaning |
| --- | --- | --- |
| `--context-item=FILE` | `xpath`, `xquery` | The document the expression starts from |
| `--expression=TEXT` | `xquery` | The query text, instead of a query file |
| `--param=NAME=VALUE` | `xpath`, `xquery`, `transform` | Binds a variable or stylesheet parameter to an untyped value |
| `--method=METHOD` | `xpath`, `xquery`, `transform`, `serialize` | The output method: `adaptive`, `xml`, `xhtml`, `html`, `text`, `json` or `csv` |
| `--output=FILE`, `-o FILE` | `xpath`, `xquery`, `transform`, `serialize` | Writes the result to a file, only when the command succeeds |
| `--force` | the same | Lets `--output` replace a file that exists |
| `--result-directory=DIR` | `transform` | Writes result documents below DIR |
| `--initial-template=NAME`, `--initial-mode=NAME` | `transform` | Where the transformation starts |
| `--allow-put` | `xquery` | Lets `fn:put` write files |
| `--allow-file-access` | `xpath`, `xquery` | Makes the EXPath File functions available |
| `--collection=DIR` | `xpath`, `xquery`, `transform` | The XML files of DIR are the default collection, `collection()` |
| `--external-entities`, `--validate-dtd` | `check` | Reads external DTDs and entities; validates against the DTD |
| `--schema=SCHEMA` | `validate` | A schema document; repeat it to combine several into one schema |
| `--use-schema-location` | `validate` | Also reads the schema documents a document's `xsi:schemaLocation` names |
| `--mode=lax` | `validate` | Lax validation |
| `--format=json` | `check`, `validate`, `version` | A report as JSON, for programs |

| Exit status | Meaning |
| --- | --- |
| 0 | Success |
| 1 | The operation failed: not well-formed, not valid, or an error in a query or stylesheet |
| 2 | Usage error |
| 3 | A file could not be read or written |
| 4 | An internal error or an exceeded limit |
