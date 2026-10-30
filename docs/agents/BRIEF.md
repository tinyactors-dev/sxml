<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Common brief for every sxml agent

## Where and how

- The repository's working tree is shared by several agents at once. All work
  happens directly on `main`: never create branches or worktrees (a hook in
  `.claude/hooks/main-only.py` blocks it).
- Own only the files your packet names. Never reset, check out, stash or commit
  someone else's changes.
- Use the session's scratchpad for temporary files. Name your directories after
  your packet (`build-5.13`, `5.13-work`), keep them small, and delete them
  before your final report: the disk is shared and limited. Delete only
  directories named after your own packet: never the scratchpad itself or a
  wildcard over it (a whole-scratchpad delete has happened).

## Read first

`CLAUDE.md`; then, with `tools/spec/spec`: `brief N.M` for your packet (its
text, one line on each decision and packet it cites, the terms it uses), `show
NAME --depth=2` for any of those, `list decision --since=DATE` for the latest
records, and `markdown PLAN` and `markdown C_GUIDELINES` once. Then the headers
you implement or use.

## Rules

- Follow `CLAUDE.md` and `docs/C_GUIDELINES.md` exactly: spelled-out names, at
  most three parameters, no `malloc`/`free` outside `src/allocator.c`, no
  globals, views with lengths, the object lifecycle, contract comments,
  specification citations. New terms go into the glossary first.
- **No C recursion** (D71), not even through function pointers or operation
  tables. Use loops with explicit stacks in allocator memory.
- **Conceptual smallness** (CLAUDE.md): one data model, one execution
  foundation, thin language layers. Extend existing mechanisms; never add
  parallel machinery.
- **Delayed features** (D162): a feature sxml does not have yet is rejected or
  ignored in one place, with the error the specification gives, so adding it
  later replaces that one place.
- **Honest outcomes**: a test that crashes, leaks or hits a limit is an error.
  Never reclassify outcomes to meet a gate (D178); fix the cause or report it.
- **Size** (D208, D272): every shipped build has every language and feature,
  and there is one JavaScript module, `sxml.wasm`. Its budget and the CLI's
  rise as features land; raise them in the same change and say so. A large
  growth is reported (D225's map guard), never a reason to leave a feature out.
- **Do not wait on the owner** (D208): take the proposed default for an open
  question that is not irreversible, record it, and go on.
- **The book** (D257): `docs/book/` shows real output. A change to CLI output
  or to an error message the book shows updates the book in the same commit;
  `tools/check-book-examples.sh BUILD-DIR` checks it (`--update` rewrites the
  outputs for review).
- **The CLI** (`apps/`, rules in `docs/CLI.md`) uses only the public API. When
  your work makes a capability real, extend the matching command with
  end-to-end tests. Never add a command or flag for something that does not
  work yet.

## Records

- Decisions another agent needs are new decisions: `tools/spec/spec add
  decision --title=... --summary=... --after='D12 D99' --body=-` (the number is
  taken for you). Give each a one-sentence summary: it is what other agents
  read first. Items for the owner are proposals: `tools/spec/spec add
  proposal`.
- A decision that reports measurements (passes, sizes, timings) says where they
  came from: `tools/spec/spec add-source DN --what=... --tool=... --build=...
  --at=COMMIT` for each run. Text that states a current value (the passes now,
  a budget, a slice's size) cites its fact (`{{ID}}`, `tools/spec/spec show
  FACTS`, `add fact` for a new one) instead of copying the number.
- Add a short entry to the log: `tools/spec/spec log TEXT` (this week's entry,
  newest first).
- Commit messages are the record of the work: say what changed, why, the golden
  file imitated, the specification sections, and before/after counts. Every
  message ends, after a blank line, with the attribution lines the session
  gives you. Never write a model name anywhere else.

## Committing in the shared tree

- All agents share one git index. Commit with an explicit pathspec in one
  command: `git commit -m "..." -- path1 path2`. Never `git add` and commit in
  two commands, and never `git commit -a`.
- Do not stage files for later (`git add`, `git mv`) in the shared index:
  someone else's index commit takes them. Rename with `git mv` and commit it in
  the same command (`git mv OLD NEW && git commit -m ... -- OLD NEW`).
- Never move the shared branch back (`git reset`, `git commit --amend`, `git
  rebase -i` of pushed commits): another agent's commit may be on top. A commit
  that took something by mistake stays; report it.
- The spec is one file per article, and every write of `tools/spec/spec` is
  whole and checked. Commit the spec as a whole, new articles included, in one
  command: `git add spec/articles docs && git commit -m "..." -- spec/articles
  docs` (a pathspec alone leaves new files out). It may hold another agent's
  finished writes; committing them with yours is harmless.
- For part of a shared file outside the spec (a driver's tables,
  `src/system_property.c`), stage only your hunk (`git apply --cached` of your
  patch) and then commit the index with **no pathspec** (`git commit -m "..."`)
  in the same command: a commit with a pathspec takes the named files from the
  working tree, others' unfinished edits too. Check `git show --stat HEAD`
  before pushing.
- Commit and push at every working milestone, not only at the end: sessions get
  interrupted. Push with `git push origin main`; if it is rejected, `git pull
  --rebase --autostash origin main` and push again. Finish a rebase you start
  in the same command sequence: a rebase left half-done in the shared tree
  blocks every agent and hides others' uncommitted files in its autostash. If
  you find one, tell the orchestrator instead of continuing or aborting it.

## Final report

At most 350 words, for the orchestrator: what was done (files and commits),
decisions recorded, deviations, before/after numbers, open problems, and
anything that blocks other packets.
