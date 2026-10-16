# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

- `string.h` implemented in `src/string.c` (D27): strict UTF-8 validation,
  ASCII flag, codepoint count, `codepoint_at`, iterator, codepoint
  comparison; 29 cases in `tests/string_test.c` pass under `debug`,
  `debug-gcc` and `wasm`; lint clean.
- 0.5–0.6 done: core headers `status.h` (statuses, `SXML_NODISCARD`, error
  record and diagnostic, D21), `view.h` (D19), `memory.h` (views, checked
  arithmetic, memory requirements, allocator interface without reallocate
  (D22), default and counting allocators (D23), arena (D24)) and `string.h`
  (D25, designed, not implemented). Golden module `src/memory.c` +
  `src/allocator.c` (+ `src/status.c`) with 47 unit test cases in
  `tests/memory_test.c`, `allocator_test.c`, `status_test.c`; 5/5 suites pass
  under `debug`, `debug-gcc`, `release`, `wasm` and a shared GCC build; lint
  clean. New lint `pointer-parameter-without-length` (D26). Failure-path
  convention: outputs untouched on failure (D20).
- Open questions for 0.8: should `string.h` be implemented before the Phase 0
  gate; is the borrowed-view lifetime of error records (D21) acceptable for
  host bindings, which will copy them anyway.
- Conformance suites vendored under `third_party/` (D13 revised);
  `tools/update-test-suites.sh` replaces the fetch script.
- 0.1–0.4 done: CMake presets `debug`, `debug-gcc`, `release`, `msvc`, `wasm`
  (D14); test runner with one executable per module (D16); `tools/lint.sh`
  with clang-tidy and ast-grep rules (D17); CI workflow (D18, not yet run on
  GitHub). tree-sitter v0.27.1 vendored and built on every preset, wasm
  included; grammar generation script and `sxml_add_grammar` ready (D15).
  2/2 tests pass under ASan+UBSan (Clang, GCC), release and WASI.
- 0.9 done: `tools/fetch-test-suites.sh` pins qt4tests and xslt40-test (D13);
  `docs/QT4_CATALOG.md` documents the catalog; `tests/qt4/torture-slice.txt`
  lists 198 XP40 tests, checked by `tools/check-test-list.py`.
- Repository bootstrapped with plan, guidelines, glossary and decisions.
- Next: Phase 0 tasks 0.1–0.4.
