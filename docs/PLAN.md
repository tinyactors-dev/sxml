# sxml Project Plan

## Goal

Build sxml (small XML implementation), an embeddable XML toolchain that brings
XPath 4.0, XQuery 4.0, XSLT 4.0 and schema validation to every platform:
browsers, Node.js, iOS, Android, macOS, Linux and Windows. The core is portable
C following `docs/C_GUIDELINES.md`, compiled to native libraries and to
WebAssembly from one codebase.

Success is measured, not judged: each phase ends when a named subset of the W3C
QT4 test suite passes, and the public pass count is the project's progress bar.

The central design bet is a single XDM 4.0 value model shared by every language.
XQuery and XSLT both lower into one intermediate representation, so there is one
optimizer and one evaluator, not three engines.

## Architecture

```
  C library API        WebAssembly package        Swift / Kotlin (Phase 6)
        \                      |                        /
     XPath 4.0             XQuery 4.0               XSLT 4.0
        \                      |                        /
                 Shared IR and optimizer
                           |
        Evaluator  ->  F&O 4.0 function library    Schema validation (Phase 5)
                           |                              |
  Substrate (frozen after Phase 1):                        v
    XDM 4.0 values | XML tree (parser, arena) | Type registry | Memory interfaces
```

Schema validation arrives late but needs no substrate change: it registers its
types in the type registry that atomic values already point into.

## Scope for year one

In scope:

- XML 1.0 parsing with namespaces, into an arena-based tree.
- The XDM 4.0 value model: sequences, nodes, atomic items, maps, arrays,
  records, function items, JSON values.
- XPath 4.0 with the full F&O 4.0 function library.
- XQuery 4.0 core language, including modules and higher-order functions.
- C library builds for Linux, macOS and Windows; a WebAssembly build for
  browsers and Node.js.

Not in year one (each needs a decision record to be pulled in). These are
delayed, not dropped: schema awareness is Phase 5 and the rest Phase 7 (D162).

- Schema-aware processing and XSD 1.1 validation.
- XSLT streaming and the XQuery Update Facility.
- Collations beyond Unicode codepoint order.
- A bytecode VM or any form of code generation.
- Swift and Kotlin bindings, and Invisible XML.

## Phases

| Phase | Delivers | Gate |
| --- | --- | --- |
| 0. Substrate design | Public headers for the XDM value model, tree and memory interfaces; glossary; golden files; CI with lints and sanitizers | Headers reviewed by a human; CI green on a compiling skeleton |
| 1. Torture slice | Minimal XML parser and an XPath subset that exercises every part of the value model, in native and WebAssembly builds | 100–200 hand-picked QT4 tests pass under sanitizers; substrate frozen |
| 2. XPath 4.0 | Full XPath 4.0 syntax and the F&O 4.0 function library; first public release | Agreed pass rate on QT4 XPath and function test sets |
| 3. XQuery 4.0 | FLWOR, constructors, prolog, modules, serialization | Agreed pass rate on QT4 XQuery test sets |
| 4. XSLT 4.0 | Templates, modes, packages, lowered into the shared IR | Agreed pass rate on XSLT 4.0 test sets |
| 5. Schema validation | XSD 1.1 validation and schema-aware typing | XSD conformance subset; schema-aware QT4 tests |
| 6. Platform bindings | Swift and Kotlin bindings, iOS and Android builds | Binding tests on device in CI |
| 7. Deferred features | What D97 left out of Phases 2–4: full UCA, HTML parsing, invisible XML, streaming, the Update Facility, Full Text, EXPath, compatibility modes, timezone database, other calendars and languages, fn:transform (D162) | Each feature's QT4 or XSLT test sets pass at the agreed rate |

Phases 0 to 3 are year one. Pass-rate targets for Phases 2 and 3 are set at the
end of Phase 1.

## How work is done

1. Headers are designed first and frozen.
2. Work comes in packets: one module or one specification chapter plus its test subset.
3. Golden files set the style.
4. Every change is gated by lints, sanitizers and tests.
5. A separate reviewer checks each change against the guidelines.
6. Substrate changes after the freeze need a decision record.

## Tracking progress

- Conformance dashboard: pass counts per test set, published by CI on every merge.
- Weekly log in `docs/LOG.md`: what passed, what's next, what is blocked.
- Decision records in `docs/DECISIONS.md`.
- Phase review at each gate.

## Risks

| Risk | Mitigation |
| --- | --- |
| Endless redesign of the foundation | Time-boxed Phase 0; decision records; substrate frozen at the Phase 1 gate |
| Substrate proves wrong after the freeze | Torture slice targets the hardest parts of the value model first |
| Scope creep | Written non-goals; pulling one in requires a decision record |
| Agent-written code drifts | Headers first, golden files, lint gates, review pass |
| Memory-safety bugs on untrusted input | Sanitizers, fuzzing, checked size arithmetic |
| Decimal, date/time and regex take longer than planned | Each is its own packet, scheduled early in Phase 2 |
| WebAssembly size | Size budget in CI; optional features as separate modules |
| Specifications still changing | Track QT4 drafts; draft-only features behind flags |
