# Common brief for every sxml agent

## Where and how

- The repository's working tree is shared by several agents at once.
  All work happens directly on `main`: never create branches or
  worktrees (a hook in `.claude/hooks/main-only.py` blocks it).
- Own only the files your packet names. Never reset, check out, stash or
  commit someone else's changes.
- Use the session's scratchpad for temporary files. Name your directories
  after your packet (`build-5.13`, `5.13-work`), keep them small, and delete
  them before your final report: the disk is shared and limited. Delete
  only directories named after your own packet: never the scratchpad
  itself or a wildcard over it (a whole-scratchpad delete has happened).

## Read first

`CLAUDE.md`, `docs/PLAN.md`, `docs/C_GUIDELINES.md`, `docs/GLOSSARY.md`,
`docs/DECISIONS.md` (the records your packet cites, and the latest ones),
`docs/LOG.md`, the task file of your phase in `docs/tasks/`, and the headers
you implement or use.

## Rules

- Follow `CLAUDE.md` and `docs/C_GUIDELINES.md` exactly: spelled-out names,
  at most three parameters, no `malloc`/`free` outside `src/allocator.c`,
  no globals, views with lengths, the object lifecycle, contract comments,
  specification citations. New terms go into the glossary first.
- **No C recursion** (D71), not even through function pointers or
  operation tables. Use loops with explicit stacks in allocator memory.
- **Conceptual smallness** (CLAUDE.md): one data model, one execution
  foundation, thin language layers. Extend existing mechanisms; never add
  parallel machinery.
- **Delayed features** (D162): a feature sxml does not have yet is
  rejected or ignored in one place, with the error the specification
  gives, so adding it later replaces that one place.
- **Honest outcomes**: a test that crashes, leaks or hits a limit is an
  error. Never reclassify outcomes to meet a gate (D178); fix the cause or
  report it.
- **Size** (D208, D272): every shipped build has every language and
  feature, and there is one JavaScript module, `sxml.wasm`. Its budget
  and the CLI's rise as features land; raise them in the same change and
  say so. A large growth is reported (D225's map guard), never a reason
  to leave a feature out.
- **Do not wait on the owner** (D208): take the proposed default for an
  open question that is not irreversible, record it, and go on.
- **The book** (D257): `docs/book/` shows real output. A change to CLI
  output or to an error message the book shows updates the book in the same
  commit; `tools/check-book-examples.sh BUILD-DIR` checks it (`--update`
  rewrites the outputs for review).
- **The CLI** (`apps/`, rules in `docs/CLI.md`) uses only the public API.
  When your work makes a capability real, extend the matching command with
  end-to-end tests. Never add a command or flag for something that does not
  work yet.

## Records

- Decisions another agent needs go into `docs/DECISIONS.md` as a new
  numbered record (check the highest number first). Items for the owner go
  under "## Proposed".
- Add a short entry to `docs/LOG.md`, newest first.
- Commit messages are the record of the work: say what changed, why, the
  golden file imitated, the specification sections, and before/after
  counts. Every message ends, after a blank line, with the attribution
  lines the session gives you. Never write a model name anywhere else.

## Committing in the shared tree

- All agents share one git index. Commit with an explicit pathspec in one
  command: `git commit -m "..." -- path1 path2`. Never `git add` and commit
  in two commands, and never `git commit -a`.
- Do not stage files for later (`git add`, `git mv`) in the shared index:
  someone else's index commit takes them. Rename with `git mv` and commit
  it in the same command (`git mv OLD NEW && git commit -m ... -- OLD NEW`).
- **Never commit a shared ledger (DECISIONS, GLOSSARY, LOG, QUEUE, task
  files) by pathspec**: that takes everyone's uncommitted text with it,
  which has happened several times.
- For part of a shared ledger (DECISIONS, GLOSSARY, LOG), stage only your
  hunk (`git apply --cached` of your patch, or `git update-index` of HEAD's
  file plus your insertion) and then commit the index with **no pathspec**
  (`git commit -m "..."`) in the same command: a commit with a pathspec
  takes the named files from the working tree and ignores what you staged.
  Check `git show --stat HEAD` before pushing. Read a ledger before writing
  it, and never write it back from an older copy.
- Commit and push at every working milestone, not only at the end:
  sessions get interrupted. Push with `git push origin main`; if it is
  rejected, `git pull --rebase --autostash origin main` and push again.
  Finish a rebase you start in the same command sequence: a rebase left
  half-done in the shared tree blocks every agent and hides others'
  uncommitted files in its autostash. If you find one, tell the
  orchestrator instead of continuing or aborting it.

## Final report

At most 350 words, for the orchestrator: what was done (files and
commits), decisions recorded, deviations, before/after numbers, open
problems, and anything that blocks other packets.
