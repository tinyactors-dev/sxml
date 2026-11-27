<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Releasing sxml

How a release is cut. Agents prepare a release up to its release commit, a
workflow tags and publishes it from that commit, and the owner approves the npm
package (D138). The first public release is 0.2.0, made at the end of Phase 2
(PLAN.md, phase 2 gate).

## What a version promises

sxml follows Semantic Versioning (`include/sxml/version.h`). The public headers
(`include/sxml/`) are stable from 0.2.0 on (D47):

- A release that only adds declarations raises the minor version (the patch
  version for fixes that add nothing). It also raises `SXML_API_VERSION` to the
  release that added them.
- A release that changes or removes a declaration needs a decision record in
  `docs/DECISIONS.md`. Before 1.0.0 it raises the minor version, and from 1.0.0
  the major version. The changelog lists the change under "Changed" or
  "Removed".
- The frozen substrate headers (D99) change only additively, and each change
  has a record.
- `xquery.h`, `xslt.h` and `schema.h` were provisional until the Phase 3, 4 and
  5 gates (D151, D165) and are stable since (D161, D169, D182).

The JavaScript package (`bindings/javascript/`) has the same version as the
library. Its API grows only additively within a minor version, as the headers
do.

## Checklist

1. **Gate.** The phase gate's boxes in `docs/tasks/` are ticked, except the box
   for the tag.
2. **Version.** Set the same version in four places:

   - `SXML_VERSION_MAJOR`, `_MINOR` and `_PATCH` in `include/sxml/version.h`,
     and `SXML_API_VERSION` when the release adds declarations;
   - `project(sxml VERSION ...)` in `CMakeLists.txt`;
   - `"version"` in `bindings/javascript/package.json` and its
     `package-lock.json`;
   - the expected version in `tests/version_test.c`,
     `tests/cli/expected/version_*.stdout` and
     `bindings/javascript/test/package.test.ts`.
3. **Changelog.** In `CHANGELOG.md`, the section for the release gets the date
   in place of "unreleased", and its link at the bottom names the tag.
4. **Build and test** from a clean tree. The definition of done (CLAUDE.md)
   must hold:

   ```sh
   for preset in debug wasm release; do
       cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset
   done
   tools/lint.sh
   npm ci --prefix bindings/javascript && ctest --preset wasm -R javascript
   ```

   `javascript/size_budget` must pass (D93, D138). Regenerate
   `docs/CONFORMANCE.md` (D94) and quote its XP40 line in the release notes.
5. **Commit** the version and the changelog as "Release X.Y.Z", then push to
   `main`.
6. **Tag and GitHub release** (`.github/workflows/release.yml`, no computer
   needed). The push of the commit "Release X.Y.Z" starts it: when no tag
   vX.Y.Z exists, it tags that commit (`sxml X.Y.Z`), makes the GitHub release
   vX.Y.Z with the version's section of `CHANGELOG.md` as its notes
   (`tools/release-notes.sh X.Y.Z`), starts `npm.yml` for the tag (step 7), and
   builds and attaches the `sxml` archives with `SHA256SUMS` (`mise run dist`).
   The run's summary links the release. "Run workflow" on `release.yml` with a
   version does the steps again for that version, for a run that failed.
7. **Publish the JavaScript package.** `release.yml` starts
   `.github/workflows/npm.yml` (D332) for the tag (a tag pushed by hand starts
   it too), which runs `tools/npm-package.sh publish`: it builds the package
   from the tag, runs its tests (ctest `javascript/*`), packs it, installs the
   tarball into a fresh project as a check and stages it on npm with trusted
   publishing. The owner then approves the staged version with 2FA: npmjs.com,
   the package, Staged Packages; or `npm stage list @tinyactors/sxml` and `npm
   stage approve ID`. To build and check the tarball without publishing, run
   `mise run npm:pack -- vX.Y.Z`; it lands in `build/npm/vX.Y.Z/`. A run that
   failed, or a tag older than the workflow, is published with "Run workflow"
   on `npm.yml` and the tag.

   Before pushing the release commit, check the package's trusted publisher on
   npmjs.com (the package, Settings): GitHub Actions, `tinyactors-dev/sxml`,
   workflow `npm.yml`, no environment, `npm stage publish` only. npm drops a
   new one that no publish validates within two days, so one set up after a
   release has expired by the next: delete it and add it again, and the
   release's run validates it.

   The npm name `sxml` belongs to another project. The package is named
   `@tinyactors/sxml` (D239).
8. **After the release**, open a new "unreleased" section in `CHANGELOG.md`.
