# The Swift binding: design and how to resume it

The Swift binding (module `Sxml`) is paused (D255). This document holds
everything a later session needs to resume it: the intended design, what
is done, how to test it, the next steps in order, and the open questions.
Read it with `docs/tasks/phase-6.md` (packets 6.7 to 6.12) and the records
it cites: D238 (the plan), D239 (names and licence), D240 (the host shim),
D241 (6.7), D248 (6.8) and D250 (Kotlin's full surface, for comparison).

State at the pause (2026-10-20):

| Packet | State |
| --- | --- |
| 6.7 Swift core | done (D241) |
| 6.8 Swift full surface, the DUCET resource | done (D248) |
| 6.9 Apple builds, the XCFramework, iOS tests | not started; owner's Mac |
| 6.10 device CI (the macOS and iOS jobs) | not started |
| 6.11 documentation and release steps (the Swift part) | README done; release steps not written |
| 6.12 Phase 6 review | not started |

## 1. The design

### 1.1 Layers

```
Swift caller
  module Sxml            bindings/swift/Sources/Sxml/*.swift   (Swift only)
  module CSxmlHost       bindings/swift/Sources/CSxmlHost/module.modulemap
  host shim              bindings/host/sxml_host*.c, sxml_host.h (D240)
  sxml                   src/, include/sxml/
```

- **One host shim, three hosts** (D238). The shim turns the public C API
  into handles, flat request structs, one error record per processor,
  side results (D216), JSON-shaped input through `fn:parse-json`, and
  serialization into processor-owned output. JavaScript, Kotlin and Swift
  all sit on it. A fix in the shim fixes all three.
- **No Swift-specific C.** `CSxmlHost` is a Clang module whose header is
  `bindings/host/sxml_host.h` itself (`module CSxmlHost [system] { header
  "../../../host/sxml_host.h" link "sxml_host" link "m" export * }`).
  `sxml_host.h` includes the public headers of `include/sxml/`; the
  manifest adds `include/` to the include path (`-Xcc -I`).
- **No C type in the interface.** Every Swift file imports the module with
  `internal import CSxmlHost`. Handles are `OpaquePointer`s held privately
  by the classes.
- **Boundary rule.** Anything that is not Swift idiom (value conversion,
  error copying, side results, the U+0000 / U+FFFE / U+FFFF refusal and
  the UTF-8 check of `sxml_host_string`, D250) belongs in the shim, never
  in Swift. If Swift seems to need C of its own, change the shim for all
  hosts and record it.
- **Text crosses as UTF-8.** Swift passes `String.utf8` (copied into
  processor memory by `Scratch`, so the allocation-failure loop reaches
  the binding's own allocations too) and reads `sxml_byte_view`s back.
- **Swift 6.** Tools version 6.0, `.swiftLanguageMode(.v6)` on every
  target, strict concurrency. The ctest builds with
  `-Xswiftc -warnings-as-errors`.

### 1.2 The API surface

The JavaScript package's concepts (`bindings/javascript/src/index.ts`, the
golden file) in Swift's idiom: named arguments with defaults instead of
option objects, `throws`, ARC, acronyms in upper case (`baseURI`,
`absoluteURI`). `bindings/swift/README.md` documents every call with
examples; this is the list.

| Swift | Notes |
| --- | --- |
| `Sxml.version`, `Sxml.features` | `"0.2.0"`; the optional features by name (every one but EXPath File) |
| `Processor(resolver:features:collationData:failingAllocationNumber:) throws` | `features: nil` means all; unknown name: `SxmlUsageError.unknownFeature` |
| `parse(_: String \| [UInt8], uri:) -> Value` | the document node |
| `compile(_:variables:namespaces:baseURI:) -> XPathExpression` | `evaluate(context:variables:)`, `variables` |
| `evaluate(_:context:variables:namespaces:baseURI:) -> Value` | compile and evaluate once |
| `value(_:) -> Value`, `serialize(_:options:) -> String` | `fn:parse-json` rule; `fn:serialize` options, camelCase keys and `Bool` for yes/no |
| `compileQuery(_:namespaces:baseURI:) -> XQuery` | `evaluate(context:variables:updates:messages:)`, `serialize(_:)`, `run(...)`, `isUpdating` |
| `compileStylesheet(_: String \| [UInt8], baseURI:staticParameters:) -> Stylesheet` | `transform(source:initialMatchSelection:initialMode:initialTemplate:initialFunction:arguments:parameters:templateParameters:tunnelParameters:baseOutputURI:delivery:resultDocumentDelivery:serialization:messages:) -> TransformResult` |
| `compileSchema(_: String \| SchemaDocument \| [SchemaDocument], baseURI:) -> Schema` | `validate(_: String \| [UInt8] \| Value, mode:type:uri:) -> ValidationResult` |
| `liveAllocationCount()` | for tests |
| `Value: CustomStringConvertible` | `description` is the adaptive method; `serialize(_:)` |
| `XdmInput` | `Value`, `String`, `Bool`, `Int`, `Double`, `[XdmInput?]`, `[String: XdmInput?]`, `nil` |
| `UpdateSink(updatedTree:putDocument:)` | struct of two optional throwing closures; no `putDocument`: `fn:put` raises FOUP0002 |
| `UpdatedTree`, `PutDocument`, `Message` | side results, delivered after the call (D216) |
| `InitialMode` | `.named(String)`, `.unnamed`; a string literal names a mode |
| `ResultDelivery`, `ValidationMode`, `Validity` | enums; raw values of the C enums (`Validity`: the JavaScript names) |
| `TransformResult`, `ResultDocument`, `ValidationResult`, `SchemaReport` | results |
| `SxmlError`, `SchemaError`, `SxmlUsageError`, `Status` | errors (1.4) |
| `Resolver`, `ResourceRequest`, `Resource`, `ResourceKind`, `CollationData` | resources (1.5) |

Defaults taken under D208 (D241, D248) and worth keeping:
- Map keys of `[String: XdmInput?]` reach `fn:parse-json` in code point
  order; a Swift dictionary has no order.
- A `Value` inside an array or map is refused
  (`SxmlUsageError.unsupportedInput`): JSON-shaped data cannot hold it.
- `ResourceKind` and `Status` cases are camelCase with the JavaScript and
  `sxml_status` names as raw values.
- `PutDocument.serialize()` uses method xml with no parameters, as in
  JavaScript.
- With `expath-binary`, the prefix `bin` is bound for XPath, as the `sxml`
  command binds it (D235); `namespaces` may rebind it.

### 1.3 Ownership and threads

- **ARC.** `Processor`, `XPathExpression`, `XQuery`, `Stylesheet`,
  `Schema` and `Value` are `final class`es that release their shim handle
  in `deinit`. There is no `close()` or `dispose()`.
- **Objects retain their processor** (`public let processor: Processor`),
  so a processor is destroyed after the last object it made, and the
  order of destruction is always right. Values a call returns (results,
  updated trees, result documents, the validated copy) are ordinary
  values.
- **Not Sendable** (D35: one thread at a time per processor). Each class
  has `@available(*, unavailable) extension X: Sendable {}`, so Swift 6
  rejects sharing one across isolation domains at compile time. Separate
  processors may run in parallel. The plain data types (`SxmlError`,
  `ResourceRequest`, `Resource`, `Message`, `SchemaReport`, the enums) are
  `Sendable`.
- **The resolver** is the only callback during a call (D71). It must not
  use its processor: `Processor.enter()` throws
  `SxmlUsageError.resolverUsesProcessor` while `ProcessorState.isInResolver`
  is set. It must not capture its processor strongly either (a cycle).
  The shim's state pointer is an unretained `ProcessorState`, made before
  the processor, which holds the resolver, the collation choice and the
  pending error.
- **Side-result closures** (`messages`, the sink's closures) run after the
  call, with the failure read first, so they may use the processor. An
  error a closure throws replaces the call's own failure (D248).
- **Leak checks.** `Processor.deinit` passes the allocations left to an
  internal `leakObserver`; the tests set it through `@testable import`
  and expect 0.

### 1.4 Errors

- `struct SxmlError: Error, CustomStringConvertible, Sendable`: `status`,
  `code` (`"err:FOAR0001"`), `codeNamespaceURI`, `codeLocalName`,
  `description` (sxml's text, also the `CustomStringConvertible` text),
  `sourceName`, `lineNumber`, `columnNumber`, and `message` ("1:1:
  err:FOAR0001: ..."), as JavaScript's `message`. Out of memory is
  `SxmlError` with `.outOfMemory`.
- `struct SchemaError: Error` (`error: SxmlError`, `reports:
  [SchemaReport]`), thrown when a schema does not compile.
- `enum SxmlUsageError: Error`: `resolverUsesProcessor`,
  `valueOfAnotherProcessor`, `unknownFeature(String)`,
  `unsupportedInput(String)`.
- **Untyped `throws`**: a resolver's own error is rethrown as it is. The
  resolver function keeps it in `ProcessorState.pendingError`, returns
  `SXML_STATUS_INPUT_OUTPUT_FAILED`, and the call rethrows it.

**Open: SchemaError and the base error (D250).** JavaScript's
`SchemaError extends SxmlError` and Kotlin's `SchemaException` subclasses
`SxmlException`, so catching the base error also catches a schema that
does not compile. Swift's `SchemaError` is a separate struct, so
`catch let error as SxmlError` does not catch it.

*Recommendation for 6.12: take the JavaScript and Kotlin model.* A
caller's broad `catch` should behave the same in all three languages, and
JavaScript is the golden. Swift structs cannot inherit, so the Swift form
is: `compileSchema` throws an `SxmlError` that has a new field
`schemaReports: [SchemaReport]` (empty for every other error; the first
report's code, description and location are the error's own, as in
JavaScript), and `SchemaError` is removed. No Swift release has shipped,
so the change breaks nobody. Change `Schema.swift`, `Errors.swift`, the
test "reports every error of a schema that does not compile", the README's
"XSD validation" and "Errors" sections and the API summary, and record it.
The alternative, a class hierarchy (`class SxmlError: Error`), is not
idiomatic and complicates `Sendable`.

### 1.5 Resolvers and resources

- `typealias Resolver = (ResourceRequest) throws -> Resource?`.
  `ResourceRequest(kind:uriReference:absoluteURI:)`. `Resource` is
  `.text(String)` or `.bytes([UInt8])`; `nil` means not available.
  Nothing else is read: no file system, no network.
- `ResourceKind` has the 13 kinds of the JavaScript package, raw values
  the JavaScript names, in this order: `xmlDocument` (`"xml-document"`),
  `text`, `binary`, `collection`, `uriCollection`, `xqueryModule`,
  `xsltModule`, `xsltPackage`, `schemaDocument`, `externalEntity`,
  `timezoneData`, `collationData`, `localeData`. The order must stay that
  of the shim's `resource_kind` numbers: the resolver function indexes
  `ResourceKind.allCases` with it.
- The resolver function (`resolveResource` in `Processor.swift`) copies
  the answer into memory from `sxml_host_allocate` on the asking
  processor (the request names it, D240).
- **Feature tables**: `timezoneData` (`urn:sxml:timezone-data:` + a
  zoneinfo path), `localeData` (`urn:sxml:locale-data:` + a path below
  CLDR `common/`) and `collationData` are asked for when a feature first
  needs them. Without an answer the feature behaves as absent.

### 1.6 The bundled DUCET

- `CollationData`: `.bundled` (default), `.none`, `.bytes([UInt8])`. A
  resolver's own answer comes first; when it has none for the DUCET URI
  (`http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt`), the processor
  serves the choice.
- `.bundled` is `bindings/swift/Sources/Sxml/Resources/allkeys.txt`, a
  SwiftPM `.copy` resource read through `Bundle.module` once per process,
  when a UCA collation first asks for it (D213, D248).
- It is a **copy** of `third_party/unicode-collation/allkeys.txt` (the same
  git blob), not a symbolic link: SwiftPM copies a link into the bundle as
  a link, which then dangles. The test "the bundled DUCET is the vendored
  one, and UCA collations use it" checks that the two files are equal. **Whoever updates the DUCET updates
  both.**
- Licence: the DUCET is under the Unicode licence; the release must ship
  that notice with the package (6.12 checks licence notes of shipped
  data).

### 1.7 The manifest and the module map

`Package.swift` is at the repository root (SwiftPM needs it there). Its
targets: the system library `CSxmlHost`, the target `Sxml`
(`bindings/swift/Sources/Sxml`, the resource), and the test target
`SxmlTests`.

- **Linux (done).** `SXML_HOST_LIBRARY_PATH` names the directory of
  `libsxml_host.a`. `bindings/swift/CMakeLists.txt` makes it: target
  `sxml_host_swift_library` merges every static library `sxml_host`
  links (the shim, `sxml`, tree-sitter, the grammars found through the
  global property `SXML_GRAMMAR_LIBRARIES`, libmpdec) with an `ar -M`
  script. With the variable set, the manifest adds the unsafe flags
  `-Xcc -I include` and `-L <dir>`. Unsafe flags are allowed only in a
  root package, so this route serves development in the repository.
- **Apple (6.9, to do).** Without `SXML_HOST_LIBRARY_PATH`, `CSxmlHost`
  becomes a `binaryTarget` named `CSxmlHost` over `Sxml.xcframework`,
  whose static library is also `libsxml_host.a` and whose `Headers` hold
  `sxml_host.h`, `include/sxml/*.h` and a module map of its own. Then no
  unsafe flag is needed and a dependent package can use it. The manifest
  is evaluated on the host, so the choice is made by the variable (or
  `#if os(Linux)`), not per target platform; see 3.1.

### 1.8 Matching JavaScript and Kotlin

| Concept | JavaScript | Kotlin | Swift |
| --- | --- | --- | --- |
| Load | `await load()` | class load | none |
| Release | `dispose()`, `FinalizationRegistry` | `AutoCloseable`, `Cleaner` | ARC `deinit` |
| Threads | one realm | processor lock (D35) | not `Sendable`, compile time |
| Options | option objects | named arguments | argument labels |
| Input | JS data | `Any?` | `XdmInput` |
| Unnamed mode | `'#unnamed'` | `InitialMode.Unnamed` | `.unnamed` |
| Base error | `SxmlError` | `SxmlException` | `SxmlError` |
| Schema error | `SchemaError extends SxmlError` | `SchemaException : SxmlException` | `SchemaError` (separate; 1.4) |
| Misuse | `Error` | `IllegalStateException`, `IllegalArgumentException` | `SxmlUsageError` |
| URI names | `baseUri`, `absoluteUri` | `baseUri`, `absoluteUri` | `baseURI`, `absoluteURI` |
| DUCET | full module data | class path resource | SwiftPM resource |
| Tests | `package.test.ts`, `full.test.ts` | the same names | the same names |

## 2. What is done, and how to test it on Linux

### 2.1 Done

- **6.7 (D241)**: the manifest, the module map, the XPath half of the
  API, `bindings/swift/CMakeLists.txt` (the merged archive, ctest
  `swift/linux`), `PackageTests.swift` (27 tests: 24 by the names of
  `test/package.test.ts`, and three ARC tests in place of the two
  disposal tests).
- **6.8 (D248)**: XQuery, XSLT, XSD, the side results, the DUCET resource,
  `FullTests.swift` (28 tests: 24 by the names of `full.test.ts`, one ARC
  test in place of the disposal test, three resource-data tests). The
  core-module refusal has no counterpart: the native library has every
  language.
- `bindings/swift/README.md` (written after the JavaScript README's
  sections; part of 6.11).
- Result: ctest `swift/linux` under the `debug` preset (ASan) passes 55
  tests, both allocation-failure loops included, with 0 leaks, in about
  70 s.

Files: `Package.swift`; `bindings/swift/CMakeLists.txt`, `README.md`,
`Sources/CSxmlHost/module.modulemap`, `Sources/Sxml/{Processor, Value,
Input, Errors, Resolver, Scratch, SideResults, XQuery, Stylesheet,
Schema}.swift`, `Sources/Sxml/Resources/allkeys.txt`,
`Tests/SxmlTests/{PackageTests, FullTests}.swift`.

### 2.2 Test on Linux

Toolchain: **Swift 6.2** (`swift-6.2-RELEASE`, the Ubuntu 24.04 x86_64
tarball from download.swift.org, SHA-256 pinned in
`tools/fetch-binding-toolchains.sh`), Swift Testing 6.2. It is 3.2 GB
unpacked: fetch it into the session's scratchpad and delete it afterwards.

```sh
SCRATCH=/path/to/scratchpad/swift-work          # the session's scratchpad
tools/fetch-binding-toolchains.sh "$SCRATCH" swift
export PATH="$SCRATCH/swift-6.2-RELEASE-ubuntu24.04/usr/bin:$PATH"
swift --version                                 # Swift version 6.2

cmake --preset debug -DSXML_SWIFT="$(command -v swift)"
cmake --build --preset debug --target sxml_host_swift_library
ctest --preset debug -R swift/linux --output-on-failure

rm -rf "$SCRATCH"                               # the disk is shared
```

ctest runs, from the repository root:

```sh
SXML_HOST_LIBRARY_PATH=$PWD/build/debug/bindings/swift/lib \
SXML_HOST_FEATURES=test,expath-binary,html,xslt-transform,timezone-database,dtd-validation,uca-collation,full-text,invisible-xml,localization,builtin-schemas,source-code \
swift test --package-path . --scratch-path build/debug/bindings/swift/swiftpm \
  --disable-xctest -Xswiftc -warnings-as-errors --sanitize=address
```

- `--sanitize=address` comes from the debug preset's sanitizers. swiftc
  has no `-sanitize=undefined` on Linux, but the ASan runtime carries
  UBSan's handlers, so the C code (built with `-fno-sanitize-recover`)
  links and traps.
- `--disable-xctest`: XCTest's empty run leaks objects of its own, which
  LeakSanitizer reports. The tests use Swift Testing only.
- gold prints "Cannot export local symbol '__asan_...'" at the link:
  harmless.
- Without a `swift`, the test is skipped with code 77.
- A release build: `cmake --preset release && cmake --build --preset
  release --target sxml_host_swift_library`, then
  `SXML_HOST_LIBRARY_PATH=$PWD/build/release/bindings/swift/lib swift
  test --disable-xctest`.

## 3. Next steps, in order

### 3.1 Packet 6.9 on the owner's Mac

Needs Xcode 26 (its Swift 6.2 toolchain), deployment targets iOS 15 and
macOS 13 (D238). This is also the first macOS build of the library
(D123's known risk): expect portability fixes in `src/` first, each
recorded.

1. **`cmake/apple.cmake`**: a toolchain file for `CMAKE_SYSTEM_NAME` iOS
   (`CMAKE_OSX_SYSROOT` `iphoneos` and `iphonesimulator`) and macOS, with
   `CMAKE_OSX_ARCHITECTURES` and `CMAKE_OSX_DEPLOYMENT_TARGET` (15.0 /
   13.0), `SXML_BUILD_TESTS=OFF`, `SXML_BUILD_APPS=OFF`, MinSizeRel, and
   `-Werror` as everywhere.
2. **`tools/build-xcframework.sh`**:
   - build static `sxml_host` three times: `ios-arm64` (iphoneos),
     `ios-arm64_x86_64-simulator` (iphonesimulator, both architectures),
     `macos-arm64_x86_64`;
   - merge each slice's static libraries into one `libsxml_host.a` with
     **`libtool -static -o libsxml_host.a <archives>`**, not `ar -M`
     (Apple's `ar` has no MRI scripts). The list of archives is the one
     `bindings/swift/CMakeLists.txt` walks; reuse that walk (for example
     by letting that file run on Darwin with a `libtool` command instead
     of the MRI script) rather than writing a second list;
   - per slice, a `Headers/` directory with `sxml_host.h`, the
     `include/sxml/` tree it includes, and a `module.modulemap`:
     `module CSxmlHost [system] { header "sxml_host.h" export * }`.
     Adjust the include path inside `sxml_host.h` only if it does not
     resolve from there (it must stay the same header). Drop `link
     "sxml_host"`: SwiftPM links the XCFramework's library itself;
   - `xcodebuild -create-xcframework -library <slice>/libsxml_host.a
     -headers <slice>/Headers` (three times) `-output
     build/apple/Sxml.xcframework`;
   - print the slice sizes (stripped and gzip -9) for the record.
3. **The manifest's `binaryTarget`**: when `SXML_HOST_LIBRARY_PATH` is
   empty, replace the system library by
   `.binaryTarget(name: "CSxmlHost", path: "build/apple/Sxml.xcframework")`
   for local work, and `.binaryTarget(name: "CSxmlHost", url: ...,
   checksum: ...)` once releases exist (3.3). Keep the system library and
   the unsafe flags when the variable is set (Linux, and macOS
   development against a sanitized archive). Update the comments in
   `Package.swift` and the module map.
4. **`swift test` on macOS**:
   - against the XCFramework: `swift test --disable-xctest`;
   - with ASan, against a sanitized archive (the XCFramework is release):
     build `sxml_host_swift_library` under the `debug` preset on macOS
     (needs step 2's `libtool` route in `bindings/swift/CMakeLists.txt`
     and the Linux-only guard lifted for Darwin), then
     `SXML_HOST_LIBRARY_PATH=$PWD/build/debug/bindings/swift/lib swift
     test --disable-xctest --sanitize=address`. Apple's toolchain has no
     LeakSanitizer on macOS: the 0-leak check rests on `leakObserver`
     (sxml's own allocation count), which every test makes anyway.
     Optionally also `--sanitize=undefined`, which swiftc does have on
     Darwin;
   - expect 55 tests, 0 leaks.
5. **`xcodebuild test` on an iOS simulator and one device**, from the
   repository root (Xcode opens `Package.swift`):

   ```sh
   xcodebuild test -scheme Sxml-Package \
     -destination 'platform=iOS Simulator,name=iPhone 16'
   xcodebuild test -scheme Sxml-Package \
     -destination 'platform=iOS,id=<device UDID>'
   ```

   (`xcodebuild -list` names the scheme; it may be `Sxml`.) The device run
   needs a signing team. Known problem: the tests read
   `bindings/javascript/package.json` and `third_party/` through
   `#filePath` (`repositoryRoot`). The simulator can read the host's
   files; a device cannot. Before the device run, either add those files
   (the DUCET, the zoneinfo and CLDR files the three tests use, and the
   version) to the test target as `.copy` resources, or skip those tests
   when `repositoryRoot` is not readable, and record which.
6. **`Bundle.module`**: check that `CollationData.bundled` finds
   `allkeys.txt` on macOS, the simulator and the device, with the test
   "the bundled DUCET is the vendored one, and UCA collations use it"
   (D248). In a dependent app the resource still comes from the Sxml
   target's bundle.
7. Record a D-record (sizes, what changed in `src/` for macOS, the test
   counts), tick the 6.9 lines in `docs/tasks/phase-6.md`, a LOG entry.

Done when `xcodebuild test` passes on an iOS simulator and one device and
`swift test` passes on macOS, with 0 leaks.

### 3.2 Packet 6.10: the macOS and iOS jobs

In new `.github/workflows/bindings.yml` (shared with Kotlin's jobs),
`workflow_dispatch` only until the owner enables CI (D123). Check it
with `actionlint` here; do not run it.

- Linux: install Swift 6.2 (the pinned tarball, or
  `tools/fetch-binding-toolchains.sh`), then the commands of 2.2.
- macOS (`macos-15` or later with Xcode 26; `sudo xcode-select -s` the
  pinned Xcode): `tools/build-xcframework.sh`; `swift test
  --disable-xctest`; the ASan run of 3.1 step 4; `xcodebuild test` on an
  iOS simulator.
- A physical iOS device: a job that needs secrets the owner adds (signing
  certificate, provisioning profile, a device farm or a self-hosted
  runner). Write it guarded so it skips without the secrets.
- Upload `Sxml.xcframework.zip` as an artifact, so a release can take it
  from a green run.

### 3.3 Documentation and release steps (6.11's Swift part)

- `bindings/swift/README.md`: replace "the package will link
  `Sxml.xcframework`, which is not built yet" with how to depend on the
  package (`.package(url: "https://github.com/<org>/sxml", from:
  "X.Y.Z")`, product `Sxml`) once 3.1 is done; keep the Linux route. Note
  that a dependent Linux package cannot use unsafe flags (open question
  4.2).
- `docs/RELEASE.md`, a Swift section (written here, run by the owner):
  1. build the XCFramework from the release commit:
     `tools/build-xcframework.sh`;
  2. `cd build/apple && zip -r -y Sxml.xcframework.zip Sxml.xcframework`
     (`-y` keeps the framework's symbolic links);
  3. `swift package compute-checksum build/apple/Sxml.xcframework.zip`;
  4. put the release URL
     (`https://github.com/<org>/sxml/releases/download/vX.Y.Z/Sxml.xcframework.zip`)
     and the checksum into the manifest's `binaryTarget`, in the
     "Release X.Y.Z" commit;
  5. tag as the other packages are (`vX.Y.Z`, `docs/RELEASE.md` step 6;
     SwiftPM accepts the `v` prefix), and attach the zip to the GitHub
     release of that tag. The checksum in the tagged manifest must be
     that of the attached file: never rebuild the zip after committing;
  6. check from a scratch package that depends on the tag: `swift build`
     on macOS and an iOS build in Xcode.
  The URL changes with the move to the `tinyactors-dev` organization
  (D239); do the move before the first Swift release.
- `README.md` and `CHANGELOG.md` lines for the Swift package.

### 3.4 Packet 6.12: the Swift review items

- **Parity lists**: the names in `PackageTests.swift` and
  `FullTests.swift` against `test/package.test.ts`, `test/full.test.ts`
  and Kotlin's lists; each deviation (ARC tests for disposal, no
  core-module refusal) is recorded.
- **SchemaError** (1.4): decide between the two models; the
  recommendation is JavaScript's and Kotlin's.
- **Threads**: Kotlin has a two-thread test; Swift has none, because the
  rule is a compile-time one. Decide whether to add a test that two
  processors run in parallel tasks (each made inside its own task), and
  whether to check the non-`Sendable` rule with a compile-failure test.
- **Ownership**: every class releases in `deinit` and retains its
  processor; the resolver state is unretained and lives as long as the
  processor; no C type in the module interface (`internal import`).
- **The DUCET copy** equals the vendored file; its licence note ships.
- **Sizes**: record the XCFramework slices (no budget exists yet; propose
  one, as D238 did for `libsxml_kotlin.so`).
- **The task file's Swift example** (`docs/tasks/phase-6.md`) serializes
  by the text method now (D250); check it still matches the README.
- The gate boxes "Swift tests pass on macOS, an iOS simulator and one iOS
  device" and "bindings.yml is green" wait for the owner (D181).

## 4. Open questions and known gaps

1. **SchemaError vs SchemaException** (D250): see 1.4.
2. **Linux dependents.** On Linux the package works only as the root
   package, because `-Xcc -I` and `-L` are unsafe flags. A Linux package
   that depends on `Sxml` has no route. Options: an artifact bundle
   (SwiftPM's `.artifactbundle` for static libraries, Swift 6 supports
   it on Linux), or a C target in the package that compiles the library's
   sources (large, and it duplicates the CMake build). Not decided.
3. **The size of the clone.** A dependent package clones this whole
   repository (about 1 GB with the conformance suites and `third_party/`).
   A thin distribution repository (`sxml-swift`: the manifest, the Swift
   sources, the resource and the `binaryTarget` URL) would avoid it.
   Owner's choice, before the first Swift release.
4. **Tests on a device** read repository files through `#filePath`
   (3.1 step 5).
5. **macOS has never built the library** (D123). Platform branches may be
   needed (`src/allocator.c`, `src/processor.c` clock), as Android needed
   two (6.3).
6. **No size budget** for the XCFramework (3.4).
7. **The DUCET is in the repository twice** (one git blob). Updating
   `third_party/unicode-collation/allkeys.txt` without the copy fails the
   test, by design.
8. **EXPath File** is not in the native library, as in `sxml.wasm`;
   a host that wants it needs a feature flag and its own file access
   decision, not planned.
9. **No core-only variant** (D238): added only when a host asks.
10. **`Sxml.version`** is checked against
    `bindings/javascript/package.json`; on a device that file is not
    there (item 4).
