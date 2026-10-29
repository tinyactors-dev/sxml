<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# The sxml command: design

This page is for developers of the `sxml` program. The user reference, with
every command, its options and examples, is
[apps/sxml/README.md](../apps/sxml/README.md) (D181).

## Design rules

These rules are binding for every command (D44). Sources: Command Line
Interface Guidelines (https://clig.dev), POSIX.1-2024 XBD 12.2 "Utility Syntax
Guidelines", GNU coding standards "Command-Line Interfaces", NO_COLOR
(https://no-color.org).

### One intent, one action

Every invocation does exactly what its words say, and nothing else.

- **Each command is one verb doing one thing.** `sxml check` never also prints
  the document. Two behaviours get two commands or an explicit flag, never a
  mode guessed from the input (file or expression, XML or JSON).
- **The command line fully determines behaviour.** No configuration files. The
  environment changes presentation only (color, once there is any).
- **No hidden side effects.** sxml writes a file only when the command line
  names it (see "Written files" below), refuses to replace an existing file
  unless `--force` is given, never deletes anything, and never touches the
  network: `fn:doc`, `fn:unparsed-text` and friends resolve local files only.
- **Standard input is read only when `-` is given** as a file operand.
- **Nothing is advertised that is not implemented.** A command or option enters
  the program's table with its working code; planned ones are listed only under
  "Planned commands" below.
- **No abbreviations, no auto-correction.** `--form` is not `--format` (unlike
  GNU getopt_long); a typo gets a "did you mean" suggestion and exit code 2,
  never a guess. An unknown option with no close match among the command's own
  that another command has is named as theirs: `sxml xpath: unknown option
  '--expression'; it is an option of 'sxml xquery'`. Commands have no aliases
  except `help`/`--help` and `version`/`--version`, which the guidelines ask
  for.
- **No repeated single options.** `--format=json --format=text` is a usage
  error, not "last one wins"; collecting options (`--param`) say "repeatable",
  and one that must be given at least once (`--schema`) says "required;
  repeatable".

### Syntax

- `sxml [-h | --help | --version] COMMAND [ARGUMENT...]`. Options before the
  command belong to `sxml`; after it, to the command.
- Long options: `--name`, `--name=value`, `--name value`. Short options: `-q`,
  combined `-qv`, `-o FILE` or `-oFILE` (POSIX guidelines 3, 5, 6). Every short
  option has a long form.
- Options and operands may be mixed after the command (GNU argument
  permutation). `--` ends options: everything after it is an operand. `-` alone
  is an operand meaning standard input. An argument that starts with `-` (such
  as `-1`) must follow `--` to be an operand.
- A value-taking option always consumes the next argument: `--expression -1`.
- `-h`/`--help` works on every level and wins over everything else on the
  command line before `--`, including errors: `sxml version --bogus --help`
  shows help. `sxml help COMMAND` equals `sxml COMMAND --help`.
- `sxml` with no arguments is a usage error (exit 2) that prints the usage
  line: a script that forgot the command must not succeed.

### Written files

Decided in D217 (packet cli-2). Three options name files to write; nothing else
writes one, except the EXPath File functions that `--allow-file-access` grants
(D200).

- **`--output=FILE` (`-o FILE`)** on `xpath`, `xquery`, `transform` and
  `serialize`: the result goes to FILE instead of standard output, byte for
  byte what standard output would get. `-` and the empty name are usage errors
  (standard output is the default).
- **`--result-directory=DIR`** on `transform`, for `xsl:result-document` with
  an `href`: DIR's file URI is the base output URI (XSLT 4.0, 2.4.6), so
  relative hrefs resolve against it; a result document whose absolute URI is
  not a file below DIR (`..`, another directory, another scheme, an escaped
  `/`) is an error, exit 3. DIR must exist; directories below it are made as
  hrefs need. Without the option, each result document is evaluated, reported
  on standard error ("not written; give --result-directory") and dropped.
  `--output` alone does not write result documents next to FILE: two intents,
  two options.
- **`--allow-put`** on `xquery`, for `fn:put` (XQUF 3.0, 6.1; D205): the
  document goes to the local file its `file:` URI names, resolved against the
  static base URI (the working directory), serialized with fn:put's `$params`.
  Other schemes are an error, exit 3. Without the flag fn:put raises FOUP0002,
  as the library does for a host that stores nothing. `--allow-file-access`
  does not imply it: that flag grants the EXPath File functions, a different
  feature.

Every written file:

- is written to a temporary file beside it and moved into place in one step
  (POSIX `rename`, or `link` when not replacing, so a file that appeared
  meanwhile is not overwritten; `MoveFileEx` on Windows), so no reader sees it
  half written;
- is committed only when the whole command succeeds; on any error every
  temporary file is removed and no file changes;
- replaces an existing file only with `--force` (exit 3 otherwise, checked
  before the work starts and again when committing). `--force` without one of
  these options is a usage error.

### Collections

Decided in D298 (packet cli-3). F&O 4.0 (17.1.3, 17.1.4) leaves what a
collection URI names to the implementation; in `sxml` it names a local
directory, read as `fn:doc` reads a file: no flag, no network.

- **A collection URI is a directory**, relative to the static base URI (the
  working directory, or the stylesheet's or module's location), with optional
  parameters after `?`, separated by `;` or `&`: `select=PATTERN` (file names;
  `*` any characters, `?` one; default `*.xml`), `recurse=yes|no`
  (subdirectories too; default `no`) and `on-error=fail|ignore` (default
  `fail`). Another parameter, a fragment, another scheme than `file:` or a path
  that is not a directory raise FODC0002 with the reason.
- **`fn:uri-collection`** gives the `xs:anyURI` of every selected file, sorted
  by path relative to the directory (byte order, which is code point order);
  nothing is read. The URI is the directory's URI and the relative path, the
  form `fn:doc` maps back to the same file.
- **`fn:collection`** gives the document node of each of those files, in the
  same order, parsed as XML as `fn:doc` parses a file (external entities
  through the same resolver). Their document URIs are those URIs, and `fn:doc`
  of one gives the same node, whichever is called first. A file that cannot be
  read or is not well-formed makes the collection unavailable (FODC0002, naming
  `FILE:LINE:COLUMN`), or with `on-error=ignore` is left out. A collection
  holds documents only: JSON, text and other files are reached through
  `fn:uri-collection` and `fn:json-doc`, `fn:unparsed-text` or
  `fn:unparsed-binary`, one intent each, never a format guessed from a file
  name.
- **The default collection** (`fn:collection()`, `fn:uri-collection()`, XPath
  4.0, 2.2.2) is the one `--collection=DIR` names on `xpath`, `xquery` and
  `transform`: DIR relative to the working directory, optionally followed by
  the parameters above (`--collection='docs?recurse=yes'`). A DIR that is not a
  directory exits 3, an unknown parameter is a usage error; without the option
  there is no default collection (FODC0002).
- Symbolic links are followed; with `recurse=yes` a directory reached twice (a
  link back up) is listed once.
- Listing needs the platform's file system
  (`apps/sxml/platform_file_system.c`); where it has none (Windows today,
  D200), every collection raises FODC0002.

### Limits

- **The person running a command bounds its time, not sxml.** The commands set
  no bound on evaluation steps (their one processor configuration is in
  `query_command.c`): a query runs to its end, and a run that takes too long is
  stopped with Ctrl-C (SIGINT) like any other program. The library's default
  bound of 10^8 steps is for programs that embed sxml and the JavaScript
  package; the conformance drivers set their own. `sxml_processor_limits` has
  no value for "no bound" (0 means the default), so the command asks for the
  largest count (D314).
- **Input stays bounded.** Entity expansion is bounded to 4 MiB in every
  command that parses XML; a limit that is exceeded exits 4.

### Source locations

- **A document keeps the line and column of its nodes when the program can ask
  for them** (D313, D318). `xpath`, `xquery` and `transform` take
  `--locations=automatic|always|never`, the processor's
  `sxml_source_location_policy`: `automatic`, the default, keeps them for the
  documents a program reads (`--context-item`, INPUT, `fn:doc`, `fn:parse-xml`,
  `--collection`) when it calls `fn:location`, can reach functions dynamically
  (`fn:function-lookup`, `fn:load-xquery-module`, `fn:transform`,
  `xsl:evaluate`) or imports a schema; they cost 32 bytes a node (+45 % peak
  memory on a large document). `always` keeps them for every program, `never`
  for none.
- **Compile first, then parse.** `xpath` and `xquery` compile before they parse
  `--context-item`, as `transform` does before INPUT, so the program decides; a
  static error is reported before a context document that does not parse.
- **An error that names a node says where it is.** When the value of a dynamic
  error (`$err:value`: the third argument of `fn:error`, an `xsl:message`'s
  select) holds a node whose document kept its locations, a second line follows
  the error: `books.xml:4:3: the node the error names`. For a program that
  cannot ask for locations, `--locations=always` gives it.

### Output

- Results go to standard output; diagnostics, warnings and usage errors to
  standard error. Help requested with `--help` or `help` goes to standard
  output (it is the result) and exits 0.
- Usage errors: `sxml version: unknown option '--fromat'; did you mean
  '--format'?`, the usage line, `Try 'sxml version --help'.` Control bytes from
  arguments are shown as `\xHH`.
- Commands whose output another program may parse offer `--format=json` (sxml's
  own reports) next to the default `--format=text`. The JSON shape is a stable
  interface: fields are only ever added. Query results are not reports: they
  are serialized with `--method=` (Serialization 4.0 methods).
- Help texts: a one-line summary that starts with a verb, a description in
  short paragraphs ending with the command's exit status, and examples that
  work on the sample files of `apps/sxml/README.md`. That page and the help
  agree: a change to one is a change to the other.
- No color, no pager, no progress output today. When color arrives it is off
  unless the stream is a terminal, off when `NO_COLOR` is set and not empty or
  `TERM=dumb`, and `--color=auto|always|never` overrides both.

## Planned commands

None today. The options the commands share: `--format=text|json` (report
format), `--context-item=FILE` (parse FILE, or stdin for `-`, as the context
item), `--param=NAME=VALUE` (repeatable; binds external variable `$NAME` to
`xs:untypedAtomic(VALUE)`), `--method=adaptive|xml|xhtml|html|text|json|csv`
(serialization of results), `--output=FILE` and `--force` (write results to a
file; replace it), `--locations=automatic|always|never` (source locations of
input documents, D318). `--output`, `--force` and result documents arrived with
packet cli-2 (D217).

The QT4 and XSLT 4.0 conformance drivers are not `sxml` commands but developer
tools under `tests/` built with the same framework (D44).

## Implementation map

- `apps/cli_framework/`: declarative command tables (`cli_command.h`), the
  argument parser (`cli_arguments.h`), help and usage errors (`cli_help.h`),
  "did you mean" (`cli_suggestion.h`), output streams and writers
  (`cli_output.h`), dispatch and exit codes (`cli_run.h`). It never prints or
  opens files: all I/O goes through `sxml_cli_output_stream`.
- `apps/sxml/`: the program table with the program's description and examples
  (`program.c`), the command handlers (`command_<name>.c`), `main.c`, and the
  platform layer: `platform_stdio.c` (stdio and the working directory, for the
  URIs of relative file names), `platform_file_system.c` (EXPath File) and
  `platform_file_writer.c` (the file writer of `--output`, `--result-directory`
  and `--allow-put`). The query commands `xpath`, `xquery` and `serialize`
  share (and `transform` reuses) their processor, the local-file resolver,
  `--param`, `--context-item`, `--locations`, the `--method` choices, error
  reports and result writing through `query_command.h`. A new command is one
  `command_<name>.c` plus one table entry, imitating `command_version.c`. Files
  are read through the invocation's `sxml_cli_file_reader` (`cli_command.h`),
  which the platform layer implements over stdio (`command_check.c` uses it),
  and written through its `sxml_cli_file_writer` (begin, commit, abandon;
  `query_command.h`'s `sxml_query_run_begin_file` and `sxml_query_run_finish`).
- Tests, all in `tests/cli/` (registered from `apps/CMakeLists.txt`, outside
  the `tests/*_test.c` glob): framework unit tests `cli_*_test.c`, and
  end-to-end golden tests (`sxml_add_cli_test`: arguments, exit code,
  `expected/<name>.stdout` and `.stderr`; `SXML_UPDATE_GOLDEN=1` rewrites;
  `WORKSPACE` runs in an empty directory and compares the files written with
  `expected/<name>.files`). They run in `tests/cli/input/`, which holds the
  input files. The help texts have golden files too (`expected/*_help.stdout`):
  a change to a help text updates its golden file and `apps/sxml/README.md`
  together, and the README's examples are re-run on a release build and their
  true output pasted.
