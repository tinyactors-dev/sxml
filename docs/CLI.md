# The sxml command-line interface

`sxml` is the command-line program built on the sxml library (D44). It uses
only the public API in `include/sxml/` plus the CLI framework in
`apps/cli_framework/`. Sources: Command Line Interface Guidelines
(https://clig.dev), POSIX.1-2024 XBD 12.2 "Utility Syntax Guidelines", GNU
coding standards "Command-Line Interfaces", NO_COLOR (https://no-color.org).

## One intent, one action

Every invocation does exactly what its words say, and nothing else.

- **Each command is one verb doing one thing.** `sxml check` never also
  prints the document. Two behaviours get two commands or an explicit flag,
  never a mode guessed from the input (file or expression, XML or JSON).
- **The command line fully determines behaviour.** No configuration files.
  The environment changes presentation only (color, once there is any).
- **No hidden side effects.** sxml writes a file only when `--output=FILE`
  names it, refuses to replace an existing file unless `--force` is given,
  never deletes anything, and never touches the network: `fn:doc`,
  `fn:unparsed-text` and friends resolve local files only (Phase 1).
- **Standard input is read only when `-` is given** as a file operand.
- **Nothing is advertised that is not implemented.** A command or option
  enters the table with its working code; planned ones live only here.
- **No abbreviations, no auto-correction.** `--form` is not `--format`
  (unlike GNU getopt_long); a typo gets a "did you mean" suggestion and exit
  code 2, never a guess. Commands have no aliases except `help`/`--help` and
  `version`/`--version`, which the guidelines ask for.
- **No repeated single options.** `--format=json --format=text` is a usage
  error, not "last one wins"; collecting options (`--param`) say "repeatable".

## Syntax

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

## Output

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
- No color, no pager, no progress output today. When color arrives it is off
  unless the stream is a terminal, off when `NO_COLOR` is set and not empty
  or `TERM=dumb`, and `--color=auto|always|never` overrides both.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success: the operation ran and its answer is positive |
| 1 | Operation failed: input not well-formed or not valid, a query raised a static, dynamic or type error |
| 2 | Usage error: unknown command or option, missing or extra operand, bad option value |
| 3 | Input/output error: a file cannot be read or written, an output file exists, a write to stdout or stderr failed (closed pipe, full disk) |
| 4 | Internal error: out of memory, a limit was exceeded, a bug in sxml |

## Commands today

| Command | What it does |
| --- | --- |
| `sxml help [COMMAND]` | Prints help for sxml or one command to stdout |
| `sxml version [--format=text\|json]` | Prints the library version (`sxml_version_text`) |
| `sxml --version` | Same as `sxml version` |
| `sxml check [--format=text\|json] FILE...` | Checks that every FILE (`-`: standard input) is namespace-well-formed XML 1.0; nothing on stdout in text format, one `FILE:LINE:COLUMN: message` per failure on stderr; `--format=json` prints `{"files":[{"file":...,"status":...}]}` with status `well-formed`, `not-well-formed`, `unreadable` or `failed` (plus `line`, `column`, `byte_offset`, `message`). Exit 1 when a FILE is not well-formed, 3 when one cannot be read, 4 at a limit (entity expansion over 4 MiB) |
| `sxml xpath [--context-item=FILE] [--param=NAME=VALUE]... [--method=adaptive] EXPRESSION` | Evaluates an XPath 4.0 expression and writes the result to stdout by the adaptive method, one item per line; `--context-item` parses FILE (`-`: standard input) as the context item, `--param` (repeatable) binds `$NAME` to `xs:untypedAtomic(VALUE)`, `fn:doc` reads local files only. An error goes to stderr as `expression:LINE:COLUMN: err:CODE: description`. Exit 1 when the expression raises a static, type or dynamic error or the context document is not well-formed, 2 for a bad `--param`, 3 when a file cannot be read, 4 at a limit. `--method` grows to xml, json and text with Phase 3 |

## Planned commands

Later phases add these, with exactly these synopses. Shared options:
`--format=text|json` (report format), `--context-item=FILE` (parse FILE, or
stdin for `-`, as the context item), `--param=NAME=VALUE` (repeatable;
binds external variable `$NAME` to `xs:untypedAtomic(VALUE)`),
`--method=adaptive|xml|json|text` (serialization of results),
`--output=FILE` and `--force` (write results to a file; replace it).

| Phase | Synopsis | Exit 1 when |
| --- | --- | --- |
| 3 | `sxml xquery [--context-item=FILE] [--param=NAME=VALUE]... [--method=M] [--output=FILE [--force]] (QUERY-FILE \| --expression=TEXT)` | as for `xpath`; giving both or neither of QUERY-FILE and `--expression` is a usage error |
| 3 | `sxml serialize [--method=M] [--indent] [--output=FILE [--force]] FILE` | FILE is not well-formed |
| 4 | `sxml transform [--param=NAME=VALUE]... [--output=FILE [--force]] STYLESHEET [INPUT]` | the stylesheet raises a static or dynamic error; without INPUT the initial template is called without a context item |
| 5 | `sxml validate --schema=XSD [--format=text\|json] FILE...` | a FILE is not valid against XSD |

The QT4 and XSLT 4.0 conformance drivers are not `sxml` commands but
developer tools under `tests/` built with the same framework (D44).

## Implementation map

- `apps/cli_framework/`: declarative command tables (`cli_command.h`), the
  argument parser (`cli_arguments.h`), help and usage errors (`cli_help.h`),
  "did you mean" (`cli_suggestion.h`), output streams and writers
  (`cli_output.h`), dispatch and exit codes (`cli_run.h`). It never prints or
  opens files: all I/O goes through `sxml_cli_output_stream`.
- `apps/sxml/`: the program table and command handlers (`program.c`,
  `command_version.c`), `main.c`, and the only file that touches stdio,
  `platform_stdio.c`. A new command is one `command_<name>.c` plus one table
  entry, imitating `command_version.c`. Files are read through the
  invocation's `sxml_cli_file_reader` (`cli_command.h`), which the platform
  layer implements over stdio (`command_check.c` uses it).
- Tests, all in `tests/cli/` (registered from `apps/CMakeLists.txt`, outside
  the `tests/*_test.c` glob): framework unit tests `cli_*_test.c`, and
  end-to-end golden tests (`sxml_add_cli_test`: arguments, exit code,
  `expected/<name>.stdout` and `.stderr`; `SXML_UPDATE_GOLDEN=1` rewrites).
  They run in `tests/cli/input/`, which holds the input files.
