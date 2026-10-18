# Releasing sxml

How a release is cut. The owner tags it and publishes it, and agents
prepare everything up to that point (D138). The first public release is
0.2.0, made at the end of Phase 2 (PLAN.md, phase 2 gate).

## What a version promises

sxml follows Semantic Versioning (`include/sxml/version.h`). The public
headers (`include/sxml/`) are stable from 0.2.0 on (D47):

- A release that only adds declarations raises the minor version (the
  patch version for fixes that add nothing). It also raises
  `SXML_API_VERSION` to the release that added them.
- A release that changes or removes a declaration needs a decision record
  in `docs/DECISIONS.md`. Before 1.0.0 it raises the minor version, and
  from 1.0.0 the major version. The changelog lists the change under
  "Changed" or "Removed".
- The frozen substrate headers (D99) change only additively, and each change
  has a record.
- `xquery.h` and `xslt.h` were provisional until the Phase 3 and Phase 4
  gates (D151) and are stable since (D161, D169).

The JavaScript package (`bindings/javascript/`) has the same version as the
library. Its API grows only additively within a minor version, as the
headers do.

## Checklist

1. **Gate.** The phase gate's boxes in `docs/tasks/` are ticked, except
   the box for the tag.
2. **Version.** Set the same version in four places:
   - `SXML_VERSION_MAJOR`, `_MINOR` and `_PATCH` in
     `include/sxml/version.h`, and `SXML_API_VERSION` when the release adds
     declarations;
   - `project(sxml VERSION ...)` in `CMakeLists.txt`;
   - `"version"` in `bindings/javascript/package.json` and its
     `package-lock.json`;
   - the expected version in `tests/version_test.c`,
     `tests/cli/expected/version_*.stdout` and
     `bindings/javascript/test/package.test.ts`.
3. **Changelog.** In `CHANGELOG.md`, the section for the release gets
   the date in place of "unreleased", and its link at the bottom names
   the tag.
4. **Build and test** from a clean tree. The definition of done
   (CLAUDE.md) must hold:

   ```sh
   for preset in debug debug-gcc wasm release; do
       cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset
   done
   tools/lint.sh
   npm ci --prefix bindings/javascript && ctest --preset wasm -R javascript
   ```

   `javascript/size_budget` must pass (D93, D138). Regenerate
   `docs/CONFORMANCE.md` (D94) and quote its XP40 line in the release
   notes.
5. **Commit** the version and the changelog as "Release X.Y.Z", then push
   to `main`.
6. **Tag** (owner): `git tag -a vX.Y.Z -m "sxml X.Y.Z"`, then
   `git push origin vX.Y.Z`. Create a GitHub release from the tag with the
   changelog section as its notes. Attach `sxml.wasm` and the package
   tarball from step 7.
7. **Publish the JavaScript package** (owner). Do this from the tagged
   commit:

   ```sh
   cmake --preset wasm && cmake --build --preset wasm --target sxml_javascript_module
   cd bindings/javascript
   npm ci
   SXML_WASM_PATH=../../build/wasm/bindings/javascript/sxml.wasm npm run build
   npm pack            # check the tarball: dist/*.js, dist/*.d.ts, dist/sxml.wasm
   npm publish         # needs the npm account and the package name
   ```

   The npm name `sxml` belongs to another project. The package is named
   `sxml-js` until the owner chooses a name or a scope (D138).
8. **After the release**, open a new "unreleased" section in
   `CHANGELOG.md`.
