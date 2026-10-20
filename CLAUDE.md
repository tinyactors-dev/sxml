# sxml

sxml (small XML implementation) is an embeddable XML toolchain in portable C:
XML parsing, the XDM 4.0 value model, XPath 4.0, XQuery 4.0, and later XSLT 4.0
and schema validation. One C codebase builds native libraries for Linux, macOS,
Windows, iOS and Android, and a WebAssembly module for browsers and Node.js.

## Read before any task

1. `docs/PLAN.md` — phases, gates, scope and non-goals.
2. `docs/C_GUIDELINES.md` — mandatory style and interface rules.
3. `docs/GLOSSARY.md` — the one spelling of every domain term.
4. `docs/DECISIONS.md` — decisions already made; do not reopen them.
5. The task file for the current phase in `docs/tasks/`.

## Current phase

**Phase 6 planning and conformance sweeps.** Phases 0 to 5 and 7 were closed
by agent review, as the owner chose (D11, D161, D169, D182, D237); gate boxes
that need long fuzzing or the full lint of all presets wait for the owner's
machine (D181). CI is disabled for now (D123). Verification is in
proportion (D181). The substrate headers are frozen (D99); all public APIs
are stable. Optional features keep the core `sxml.wasm` lean (D189, D208).
Agent prompts and the work queue live in `docs/agents/`.

## Design principle

Optimize sxml for conceptual smallness, not code size. Implement the XML
ecosystem as a small set of orthogonal, reusable mechanisms: one data model
(XDM), one shared execution foundation (the IR and its evaluator), and thin
language-specific layers for XPath, XQuery and XSLT. Every new feature extends
or composes existing abstractions rather than introducing parallel machinery.
Follow the standards faithfully, preserve their semantics, and prefer general
mechanisms over special cases. Complexity should grow sublinearly with
functionality.

## Non-negotiable rules

- Every public symbol starts with `sxml_`; macros and enum constants with `SXML_`.
- Spell identifiers out. No `ctx`, `buf`, `len`, `str`, `ptr`, `tmp`, `idx`,
  `cnt`, `num`, `cfg`, `res`, `ret`, `val`, `msg`, `err`, and no single-letter
  names, including loop counters.
- At most three parameters per function and per function-pointer type.
  Group arguments into named structs and call them with designated initializers.
- Library code never calls `malloc`, `free`, `abort`, `exit` or prints.
  Memory comes from the caller or an `sxml_allocator`.
- Every object type supports `initialize`/`finalize` on caller memory and
  `create`/`destroy` through an allocator, plus a `memory_requirements` query.
- No global mutable state.
- No C recursion, direct or indirect (also not through function pointers):
  use loops with explicit stacks in allocator memory (D71).
- A pointer to data always travels with its length in a view struct.
- New domain terms go into `docs/GLOSSARY.md` before they appear in code.

## Headers are the contract

Public headers live in `include/sxml/`. Design tasks change headers;
implementation tasks must not. If an implementation task seems to need a header
change, stop and write the reason in `docs/DECISIONS.md` under "Proposed".

## Definition of done for any change

- Builds with `-Werror` under Clang and GCC (flags in `docs/C_GUIDELINES.md`).
- `clang-tidy` reports nothing (configuration in `.clang-tidy`).
- All tests pass under AddressSanitizer and UndefinedBehaviorSanitizer.
- The change matches the golden files named in the task.
- Conformance counts, once the test driver exists, have not regressed.

## Working style

- Small steps. One module or one header per change.
- Before writing code, state which golden file you are imitating.
- When the specification decides a behaviour, cite it in a comment
  (for example, the XDM 4.0 or F&O 4.0 section).
- Commit and push to `main` early and often, at every working milestone, so
  work never sits only in a session's working tree. Commit with an explicit
  pathspec (`git commit -m "..." -- files`); other agents share the tree.
- If something in these documents is unclear or contradictory, ask rather than guess.
