# Phase 0: Substrate design

Goal: reviewed public headers for the substrate, a golden module, and CI that
enforces the guidelines on every change. No XPath evaluation yet.

Do the tasks in order. Finish each with a commit and a short entry in
`docs/LOG.md`. Stop at the gate.

## Tasks

- [x] **0.1 Build skeleton.** CMake (minimum 3.24), C17, `include/sxml/`,
  `src/`, `tests/`. Presets: `debug` (ASan + UBSan), `release`, `wasm`
  (WASI SDK toolchain file). Compiler flags from `docs/C_GUIDELINES.md`.
- [x] **0.2 Test harness.** A dependency-free unit test runner in `tests/`,
  run by `ctest`. One trivial passing test.
- [x] **0.3 Lint gates.** Wire `.clang-tidy` into the build as a target.
  Add ast-grep rules in `tools/ast-grep/` for: banned abbreviations,
  `malloc`/`free` outside `src/allocator.c`, more than three parameters,
  bare `bool` parameters.
- [x] **0.4 CI.** GitHub Actions: Linux (Clang and GCC, debug preset), macOS
  (Clang), Windows (MSVC, build only), WASI build. Each runs build, lints, tests.
- [ ] **0.5 Core headers (design task).** Write, with contract comments, no
  implementation yet:
  - `include/sxml/status.h` — `sxml_status`, `SXML_NODISCARD`, error record.
  - `include/sxml/memory.h` — byte and memory views, `sxml_memory_requirements`,
    `sxml_allocator` interface, checked size arithmetic helpers.
  - `include/sxml/string.h` — UTF-8 strings with the ASCII-only flag (D6).
- [ ] **0.6 Golden module.** Implement `memory.h` fully in `src/memory.c` and
  `src/allocator.c` (default allocator and a test double that counts and can
  fail allocations), with tests. This module becomes the golden file all later
  work imitates.
- [ ] **0.7 Substrate headers (design task).** Write, with contract comments:
  - `include/sxml/types.h` — type registry and `sxml_type_identifier`.
  - `include/sxml/xdm.h` — items, sequences (empty, single item, vector, range,
    lazy), atomic items with type identifiers, function items, maps (D7),
    arrays, records.
  - `include/sxml/tree.h` — tree interface, node references
    (`tree_identifier` + `node_index`), document order across trees.
  Each object type follows the lifecycle rules: requirements query,
  `initialize`/`finalize`, `create`/`destroy`.
- [ ] **0.8 Header self-review.** Run the review rubric from
  `docs/C_GUIDELINES.md` over every header and fix findings. List open
  questions at the end of `docs/LOG.md`.
- [x] **0.9 Torture slice selection.** Choose 100–200 tests from the W3C QT4
  test suite that stress the value model (higher-order functions, maps, arrays,
  JSON, multi-document node order, casting, ranges, try/catch). Record the list
  in `tests/qt4/torture-slice.txt` with one line per test explaining why.

## Gate

- [ ] Human review of all headers in `include/sxml/` completed.
- [ ] CI green on all platforms.
- [ ] Decisions D3–D10 in `docs/DECISIONS.md` confirmed or revised.

Only after every gate box is ticked by a human does Phase 1 begin.
