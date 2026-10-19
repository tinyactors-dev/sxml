# How to work a packet

1. Follow `BRIEF.md`.
2. Read your packet in `docs/tasks/phase-N.md`, the rules for parallel
   packets there (D45), and every decision the packet cites.
3. Own only the files the packet lists, plus single insertions in the
   shared ledgers. If you need a file another packet owns and has not
   committed, wait for its commit (`git status`, `git log`); never edit it.
   Write the need under "## Proposed" in `docs/DECISIONS.md` if it blocks you.
4. Implementation packets do not change public headers (`include/sxml/`).
   If one must change, write the reason under "## Proposed" and stop that
   part. Design packets may change the headers they own. The substrate
   headers (status, view, memory, string, name, types, tree, xdm, export)
   are frozen (D99): additive changes only, each with a decision record.
5. Name the golden file you imitate in your commit message, and cite the
   specification sections in comments.
6. Build in private: never build in the shared `build/` directories,
   because other agents' unfinished files would break the globbed build.
   Use `tools/private-build.sh <scratchpad>/build-<packet> --preset debug
   -- <your changed or new files>`. It builds HEAD plus your files in a
   snapshot, incrementally on later runs.
7. Verify in proportion (D181). The owner runs the full checks on a machine
   of their own, so a packet does not re-run everything:
   - the `debug` preset (Clang with ASan and UBSan) builds with `-Werror`,
     and the unit and CLI tests your change touches pass;
   - `clang-tidy` is clean on your files, and `tools/check-call-cycles.py`
     finds no cycle when you add or change calls between functions;
   - the conformance test sets your change affects are run with the
     release driver (`sxml-qt4 run`, `sxml-xslt-test run`, `sxml-xsd-test
     run` on those sets) and give no fewer passes than before; give
     before/after counts in the commit message and the LOG entry;
   - `debug-gcc` and `wasm` are built only when the change is
     platform-sensitive or may grow `sxml.wasm` noticeably;
   - full suite runs, all presets, the full lint and fuzzing are left to
     the owner's machine, unless the packet is a gate review that says
     otherwise. Never regenerate `docs/CONFORMANCE.md` from a partial run.
8. Tick the packet in the task file, commit and push.
9. Keep at most one debug and one release snapshot at a time (each is about
   4 GB); the disk has filled up more than once. Delete your snapshots and
   scratch directories, then send the final report.
