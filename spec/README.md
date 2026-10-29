# The spec as articles

`spec/articles/` holds the project's specification (the Markdown under
`docs/`: decisions, glossary, tasks, log, plan, guidelines, queue) as one
XML file per **article**. Agents ask questions with queries, not by
reading kilobytes of Markdown, and change articles with node updates,
not with text tools. Each fact is in one article, and the pages people
read are assembled from the articles.

This is a **shadow copy** for now: `docs/` is still the source (see
"The shadow period").

## Layout

| Path | What it is |
| --- | --- |
| `spec/articles/` | The articles, one directory per kind (below) |
| `spec/schema/spec.xsd` | The vocabulary, in XSD 1.1: the contract queries and updates rely on |
| `spec/lib/spec.xqm` | The XQuery library: loading, lookup, references, Markdown, prose, checks, `spec:store` |
| `spec/queries/*.xq` | One main module per command of `tools/spec/spec` |
| `tools/spec/spec` | The command: every read and write |
| `tools/spec/import.py`, `tools/spec/markdown.py` | The import from `docs/` (the shadow period only) |
| `tools/spec/test.sh` | The tests |
| `.claude/hooks/spec-nodes-only.py` | Refuses text tools on `spec/articles/` |

## Words

- **Article**: one file. Its **kind** is its root element, its
  **identifier** its `@id`, its **key** `kind:identifier`.

  | Kind | Directory | Identifier | Was |
  | --- | --- | --- | --- |
  | `decision` | `decisions/` | `D238` | a `### Dn.` of `docs/DECISIONS.md` |
  | `proposal` | `proposals/` | a slug of its title | an item of its "Proposed" list |
  | `term` | `terms/` | its first name, kept when it is renamed | a row of `docs/GLOSSARY.md` |
  | `packet` | `packets/` | `6.2`, `perf-15` | a `### <packet>` of `docs/tasks/` |
  | `log-entry` | `log/` | a slug of its heading | a `##` section of `docs/LOG.md` |
  | `document` | `documents/` | `PLAN`, `tasks/phase-6` | a Markdown file: its own text, and an **include** where each article taken out of it was |

- **Section**: a heading of a document, named `document#slug`
  (`tasks/phase-6#packets`).
- **Reference**: `<ref kind="decision" to="D238"/>` in text, or
  `<term name="host_shim"/>` for a term written as code. An include is
  where an article appears in a document, not a citation.
- **Summary**: one sentence an agent reads before deciding to read more.
  None is written yet: until one is, the start of the text stands in,
  shown after a `~`.
- **State**: a decision's `@status` (`accepted`, `superseded` with
  `@superseded-by`); a packet's `<status value="done" date="..."/>` at the
  start of its body (it renders as `**Done** 2026-10-20`).
- **Canonical form**: a file as `spec:put` writes it: indented, except inside the elements that hold text, with
  a final newline. `spec:store` writes nothing else, so one changed field
  is one changed line in `git diff`.

## Reading

`tools/spec/spec help` lists everything. NAME is an identifier
(`D238`, `6.2`, `host_shim`, `PLAN`), a key (`packet:6.2`) or a section.
Each read takes about 0.2 seconds for all 1,603 articles.

```console
$ tools/spec/spec show D238                  # what, state, summary: three lines
$ tools/spec/spec show D238 --depth=1        # and what it cites, what cites it
$ tools/spec/spec show D238 --depth=2        # and its text as Markdown
$ tools/spec/spec show tasks/phase-6 --depth=1   # a document's outline
$ tools/spec/spec refs D71                   # every citation of D71, in context
$ tools/spec/spec brief 6.3                  # all of a packet, one line on each thing it cites
$ tools/spec/spec status                     # packets and gate boxes per phase
$ tools/spec/spec status phase-6
$ tools/spec/spec list decision --since=2026-10-28
$ tools/spec/spec list --missing-summary decision
$ tools/spec/spec find tree-sitter pin       # Full Text: all words, stemmed
$ tools/spec/spec markdown DECISIONS --output=/tmp/DECISIONS.md
```

Any question the commands do not answer is an XPath or XQuery over the
documented vocabulary. `$spec` is the loaded spec (`spec:load()`),
`$articles` its documents, and `spec.xqm` is imported:

```console
$ tools/spec/spec query '$articles/decision[.//ref[@to = "D71"]]/@id ! string()'
$ tools/spec/spec query 'spec:cited-by($spec, spec:one($spec, "D99")) ! spec:line(.)' --method=text
$ build/release/apps/sxml xpath --collection='spec/articles?recurse=yes' 'count(//packet)'
```

## Writing

Prose arguments are a small Markdown: paragraphs and `- ` lists split by
blank lines, ```` ``` ```` fences, `` `code` `` (a term reference when it
names a term), `**strong**`, `*em*`, `[text](url)`, `[[kind:id]]` for any
reference, and decision numbers (`D238`), which always become references.
`-` reads the value from standard input.

```console
$ tools/spec/spec amend D238 --summary='Kotlin, Swift and JavaScript share one C layer, the `host_shim` (D138).'
$ tools/spec/spec add decision --title='...' --after='D11 D181' --body=- < body.md
$ tools/spec/spec add term rule_bucket --definition='...' --after=item_requirement
$ tools/spec/spec supersede D99 --by=D332
$ tools/spec/spec rename host_shim binding_shim     # and every use, in one write
$ tools/spec/spec set-status 6.10 done
$ tools/spec/spec update 6.11 'insert node <summary>...</summary> after title'
$ tools/spec/spec apply 'for $p in $articles/packet[...] return $p transform with { ... }'
```

`update` and `apply` take any XQuery Update, so no change needs a text
tool. Every write goes through `spec:store`: each changed article must
validate against the schema, and `spec:check` must find no error that it
did not find before. If either fails, nothing is written, and the
refusal says why:

```console
$ tools/spec/spec amend D238 --summary='See D9999.'
spec: refused: error: decision:D238 refers to decision:D9999, which does not exist
```

A new article goes into its document (a decision at the end of
`DECISIONS#accepted`, a term into the glossary table), because an
article no document includes is an error.

## Nodes, never text

Files under `spec/articles/` are read and written as nodes only. The
hook `.claude/hooks/spec-nodes-only.py` refuses `Read`, `Edit`, `Write`
and `Grep` on them, and the shell commands that would treat them as text
(`cat`, `sed`, `grep`, `awk`, `python`, `cp`, redirection, `find -exec`,
`xargs`), and names the command that does the same with nodes. It lets
through `tools/spec/spec`, the `sxml` command, `git`, and the commands
that only list names (`ls`, `wc`, `du`, `find`). An edit that still gets
past it is found by `check`: a file not in canonical form, an invalid
article or a broken reference is an error.

## Checks and tests

- `tools/spec/spec check` validates every file against the schema, then
  reports files not in canonical form, duplicate keys and term names,
  references to nothing, and articles no document includes (all errors),
  and terms defined twice (warnings). It exits with status 1 on an error.
- `tools/spec/spec roundtrip` assembles each document and compares it,
  word by word, with its `docs/` source. It ignores whitespace, wrapping,
  backslash escapes, table rules and the numbers of ordered items. Today
  all 23 documents are the same, 320,000 words.
- `tools/spec/test.sh` runs the import, the round trip, every read and
  write and their refusals in a temporary copy, then the hook's cases.

## The shadow period

Until the switch, `docs/` is the source, and other work edits it on
`main` every day:

1. `tools/spec/import.py` makes `spec/articles/` again from `docs/`,
   replacing everything in it (about 3 seconds). Run it, then
   `tools/spec/spec roundtrip`, after `docs/` changes, and commit the
   articles with the change. `tools/spec/test.sh` fails while the
   committed articles are not the import of `docs/`.
2. Writes through `tools/spec/spec` work on the shadow copy, but the next
   import replaces them. Use them to try the tools, not to change the
   spec.

To make the articles the source:

1. Stop edits to the imported files under `docs/` for one commit; import
   and round-trip one last time.
2. Remove the `source` attributes (one `apply`), `tools/spec/import.py`
   and `tools/spec/markdown.py`, and the round trip.
3. Generate the Markdown with `tools/spec/spec markdown` (for GitHub and
   the book), with a check that what is committed is what is generated,
   or remove it.
4. Point `CLAUDE.md`, `docs/agents/` and the packet briefs at
   `tools/spec/spec`, in place of the file names.

## What the import found

- **Decision numbers cited that do not exist**: D186, D192, D194 and
  D204 (D237 calls them unused, left by renumbering, and the week of
  2026-10-19 cites D192), D242 and D244 (cited by D247). `check` reports
  these 7 references. `tools/spec/test.sh` allows exactly these until
  they are resolved.
- **Terms defined twice**: 13 glossary names have two rows, among them
  `arity_range`, `invocation`, `node_kind`, `tee` and `variable_binding`.
- **A broken glossary row**: `injection_regex` writes `^(js|javascript)$`
  in a table without escaping the `|`, so GitHub splits the row and drops
  "javascript)$` (D288)". The import reads it as written, and the
  assembled table escapes the `|`.
- **Packet state is mostly not recorded where the packet is**: 172 of
  180 packets have no `**Done**` or `**Paused**` line. Their state is in
  the prose of `docs/agents/QUEUE.md` and `docs/LOG.md`.
- **No summaries**: 494 decisions, proposals and packets have none. A
  summary is the first thing an agent reads, so writing them is the
  first useful work after the switch (`list --missing-summary`).
