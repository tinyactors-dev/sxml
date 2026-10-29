# The spec

The project's specification (plan, guidelines, glossary, decisions,
tasks, log, agent prompts, the queue) is one XML file per **article**
under `spec/articles/`. Agents ask questions with queries, not by reading
kilobytes of Markdown, and change articles with node updates, not with
text tools. Each fact is in one article. The pages under `docs/` are
generated from the articles: each document writes one page.

`docs/CONFORMANCE.md` (written by `tools/conformance-report.py`) and the
book (`docs/book/`) are not part of the spec.

## Layout

| Path | What it is |
| --- | --- |
| `spec/articles/` | The articles, one directory per kind (below) |
| `spec/schema/spec.xsd` | The vocabulary, in XSD 1.1: the contract queries and updates rely on |
| `spec/lib/spec.xqm` | The XQuery library: loading, lookup, references, Markdown, the source form, checks, `spec:store` |
| `spec/queries/*.xq` | One main module per command of `tools/spec/spec` |
| `tools/spec/spec` | The command: every read and write |
| `tools/spec/test.sh` | The tests |
| `.claude/hooks/spec-nodes-only.py` | Refuses text tools on the articles and the pages |
| `docs/*.md`, `docs/tasks/`, `docs/agents/` | The generated pages |

## Words

- **Article**: one file. Its **kind** is its root element, its
  **identifier** its `@id`, its **key** `kind:identifier`.

  | Kind | Directory | Identifier |
  | --- | --- | --- |
  | `decision` | `decisions/` | `D238`, `D120b` |
  | `proposal` | `proposals/` | a slug of its title |
  | `term` | `terms/` | its first name, kept when it is renamed |
  | `packet` | `packets/` | `6.2`, `perf-15` |
  | `log-entry` | `log/` | `week-of-2026-10-26`, or a slug of its title |
  | `document` | `documents/` | `PLAN`, `tasks/phase-6`; `@file` is its page |

- **Section**: a heading of a document, named `document#id`
  (`tasks/phase-6#packets`).
- **Include** and **collect**: where articles appear in a document. An
  include places one article (`<include kind="packet" to="6.2"/>`); a
  collect places every article of a kind, decisions by number and the
  others by identifier (`DECISIONS` collects decisions and proposals,
  `GLOSSARY` terms), so a new decision or term changes no document.
- **Reference**: `<ref kind="decision" to="D238"/>` in text, or
  `<term name="host_shim"/>` for a term written as code. An include or a
  collect is a place, not a citation.
- **Summary**: one sentence an agent reads before deciding to read more.
  Until one is written, the start of the text stands in, shown after `~`.
- **State**: a decision's `@status`: `accepted`, `superseded` (with
  `@superseded-by`), or `unused` for a number no decision has; a
  packet's `<status value="done" date="..."/>` at the start of its body
  (on the page: `**Done** 2026-10-20`).
- **Source form**: an article's text as Markdown that reads back to the
  same nodes (`tools/spec/spec text`). `` `code` `` that names a term is
  that term, `D238` a decision, `[[kind:id]]` any other reference, and a
  line `[[include:kind:id]]`, `[[collect:kind]]` or `[[section:id]]`
  stands for an include, a collect or a subsection.
- **Canonical form**: a file as `spec:put` writes it: indented, except
  inside the elements that hold text, with a final newline. One changed
  field is one changed line in `git diff`.
- **Page**: the Markdown file a document's `@file` names, written by
  `tools/spec/spec generate`. Never edited.

## Reading

`tools/spec/spec help` lists everything. NAME is an identifier
(`D238`, `6.2`, `host_shim`, `PLAN`), a key (`packet:6.2`) or a section.
Each read takes about 0.2 seconds for all the articles.

```console
$ tools/spec/spec show D238                  # what, state, summary: three lines
$ tools/spec/spec show D238 --depth=1        # and what it cites, what cites it
$ tools/spec/spec show D238 --depth=2        # and its text
$ tools/spec/spec show tasks/phase-6 --depth=1   # a document's outline
$ tools/spec/spec refs D71                   # every citation of D71, in context
$ tools/spec/spec brief 6.3                  # all of a packet, one line on each thing it cites
$ tools/spec/spec status                     # packets and gate boxes per phase
$ tools/spec/spec status phase-6
$ tools/spec/spec list decision --since=2026-10-28
$ tools/spec/spec list --missing-summary decision
$ tools/spec/spec find tree-sitter pin       # Full Text: all words, stemmed
$ tools/spec/spec text 'agents/QUEUE#running'    # the source form, to edit
$ tools/spec/spec markdown C_GUIDELINES      # a page, as people read it
```

Any other question is an XPath or XQuery over the documented
vocabulary. `$spec` is the loaded spec (`spec:load()`), `$articles` its
documents, and `spec.xqm` is imported:

```console
$ tools/spec/spec query '$articles/decision[.//ref[@to = "D71"]]/@id ! string()'
$ tools/spec/spec query 'spec:cited-by($spec, spec:one($spec, "D99")) ! spec:line(.)' --method=text
$ build/release/apps/sxml xpath --collection='spec/articles?recurse=yes' 'count(//packet)'
```

## Writing

Text arguments are in source form; `-` reads the value from standard
input. To change a body or a section, take its source form, edit it,
and give it back:

```console
$ tools/spec/spec text 'agents/QUEUE#running' > running.md    # edit running.md
$ tools/spec/spec amend 'agents/QUEUE#running' --body=- < running.md
```

```console
$ tools/spec/spec add decision --title='...' --summary='...' --after='D11 D181' --body=- < body.md
$ tools/spec/spec add proposal --title='...' --body='...'
$ tools/spec/spec add term rule_bucket --definition='...'
$ tools/spec/spec add packet 7.31 --document=tasks/phase-7 --title='...' --body=- < packet.md
$ tools/spec/spec amend D238 --summary='Kotlin, Swift and JavaScript share one C layer, the `host_shim` (D138).'
$ tools/spec/spec log 'perf-18 done (D333): ...'
$ tools/spec/spec set-status 6.10 done
$ tools/spec/spec supersede D99 --by=D332
$ tools/spec/spec rename host_shim binding_shim     # and every use, in one write
$ tools/spec/spec remove some_term                  # refused while anything refers to it
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

After a write, `tools/spec/spec` generates the pages again, so `docs/`
always shows the articles.

## Nodes, never text

The articles and the pages are read and written through `tools/spec/spec`
only. The hook `.claude/hooks/spec-nodes-only.py` refuses `Read`, `Edit`,
`Write` and `Grep` on them (and on the page directories), and the shell
commands that would treat them as text (`cat`, `sed`, `grep`, `awk`,
`python`, `cp`, redirection, `find -exec`, `xargs`), and names the
command that does the same with nodes. It lets through `tools/spec/spec`,
the `sxml` command, `git`, the commands that only list names (`ls`, `wc`,
`du`, `find`), and searches of the whole repository. A change that still
gets past it is found by `check`: a file not in canonical form, an
invalid article, a broken reference or a stale page is an error.

## Committing

Commit the articles a change writes with the pages: `git commit -m "..."
-- spec/articles/decisions/D332.xml docs/`. Every write is whole and
checked, so a shared article (this week's log entry, a document) or a
page that holds another agent's finished write can be committed with
yours.

## Checks and tests

- `tools/spec/spec check` validates every file against the schema, then
  reports files not in canonical form, duplicate keys and term names,
  references to nothing, articles no document places, and stale pages
  (errors), and terms defined twice (warnings). It exits with status 1
  on an error.
- `tools/spec/spec selftest` renders every article, section, title,
  summary and definition in source form, reads it back, and compares
  the nodes: an edit of the source form loses nothing.
- `tools/spec/test.sh` runs the check, the self-test, every read and
  write and their refusals in a temporary copy, and the hook's cases.

## History

The articles were imported from the Markdown under `docs/` (a script,
since removed: `git log -- tools/spec/import.py`) and checked word by
word against it before the switch. The import found: cited decision
numbers no decision had (now `unused` decisions), 13 terms defined
twice (still warnings), a glossary row that GitHub split at an unescaped
`|`, and 172 of 180 packets whose state is only in the prose of the
queue and the log.
