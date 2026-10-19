# The sxml command: design

This page is for developers of the `sxml` program. The user reference,
with every command, its options and examples, is
[apps/sxml/README.md](../apps/sxml/README.md) (D181).

## Design rules

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
  "repeatable", and one that must be given at least once (`--schema`)
  says "required; repeatable".

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
  work on the sample files of `apps/sxml/README.md`. That page and the
  help agree: a change to one is a change to the other.
- No color, no pager, no progress output today. When color arrives it is off
  unless the stream is a terminal, off when `NO_COLOR` is set and not empty
  or `TERM=dumb`, and `--color=auto|always|never` overrides both.

## Planned commands

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

The QT4 and XSLT 4.0 conformance drivers are not `sxml` commands but
developer tools under `tests/` built with the same framework (D44).

## Implementation map

- `apps/cli_framework/`: declarative command tables (`cli_command.h`), the
  argument parser (`cli_arguments.h`), help and usage errors (`cli_help.h`),
  "did you mean" (`cli_suggestion.h`), output streams and writers
  (`cli_output.h`), dispatch and exit codes (`cli_run.h`). It never prints or
  opens files: all I/O goes through `sxml_cli_output_stream`.
- `apps/sxml/`: the program table with the program's description and
  examples (`program.c`), the command handlers (`command_<name>.c`),
  `main.c`, and the only file that touches stdio and the working
  directory (for the URIs of relative file names), `platform_stdio.c`. The
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
  help text updates its golden file and `apps/sxml/README.md` together,
  and the README's examples are re-run on a release build and their true
  output pasted.
