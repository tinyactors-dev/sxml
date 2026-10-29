<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Distribution

Goal (D255, owner priority 2): the `sxml` command, built for every supported
platform as archives a user unpacks and puts on `PATH`, by one `mise` task.
Rules for parallel packets are those of `docs/tasks/phase-2.md` (D45).

## Distribution

### dist-1 A mise task that builds the sxml command for every platform (build)
**Done** 2026-10-20 After D255.

- **Mechanism**: `mise.toml` pins cmake, ninja, zig, node and the WASI SDK;
  `tools/dist.sh` is the one implementation behind the tasks `mise run dist`,
  `dist:<target>`, `dist:checksums`, `dist:verify` and `install`. The native
  targets are cross-compiled from Linux with `zig cc` through one toolchain
  file, `cmake/toolchains/zig.cmake`; wasm32-wasi uses `cmake/wasi-sdk.cmake`.
  No library or header changes.
- **Targets**: x86_64-linux and aarch64-linux (static, musl), x86_64-macos and
  aarch64-macos (macOS 11 and later), x86_64-windows and aarch64-windows
  (MinGW-w64, static), wasm32-wasi (the CLI module).
- **Owns**: `mise.toml`, `tools/dist.sh`, `cmake/toolchains/`, this file,
  README.md's "Install" section.
- **Done when**: every target builds with `-Werror`; each archive
  (`sxml-<version>-<target>.tar.gz`, `.zip` for Windows) holds the program,
  LICENSE, README.md and USAGE.md (apps/sxml/README.md); SHA256SUMS; the Linux
  programs, the aarch64 one under qemu-user, the Windows one under wine and the
  WASI module under Node.js run a `sxml xpath` smoke test; `file` names the
  macOS formats; README.md documents the build and the install. **Done**
  2026-10-20 (D256): seven archives, built with `-Werror` and no portability
  fix. Run here: x86_64-linux natively, aarch64-linux under qemu-user,
  x86_64-windows under wine, wasm32-wasi under Node.js (and wasmtime); `file`
  checks the macOS and aarch64-windows programs. Running those, macOS signing
  and notarization, and publishing are the owner's.
