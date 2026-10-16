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

**Phase 1: torture slice.** Work only on the packets in
`docs/tasks/phase-1.md`. The Phase 0 gate was closed by agent review, as the
owner chose (D11); a human review of those gate boxes is still welcome. Do not
start Phase 2 work until the Phase 1 gate is closed the same way.

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
- If something in these documents is unclear or contradictory, ask rather than guess.
