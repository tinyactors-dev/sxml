<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Phase 6: Platform bindings

Goal (PLAN.md): Kotlin and Swift bindings, Android and iOS builds, and binding
tests on device in CI. Design: D238. The bindings mirror the JavaScript package
(D138, D189, D216): the same concepts, the same defaults, the same errors, in
each language's own idiom. Rules for parallel packets are those of
`docs/tasks/phase-2.md` (D45, D96, D132: a packet starts when every packet in
its "After" list is committed). Phase 6 changes no public header and must not
regress any count of Phases 2 to 7.

## The design in one page (D238)

- **One host shim, three hosts.** The C layer under the JavaScript package
  (`bindings/javascript/sxml_javascript*.c`, D138, D216) already turns the
  public API into what a garbage-collected or reference-counted host needs:
  handles, flat request structs, one error record per processor, side results
  (D216), JSON-shaped input through `fn:parse-json`, and serialization into a
  processor-owned output. It builds natively in every preset. Packet 6.2 moves
  it to `bindings/host/` as the **host shim** (glossary `host_shim`, header
  `bindings/host/sxml_host.h`, not a public header) with the resolver as an
  interface instead of a WebAssembly import. Then:

  - JavaScript: `sxml.wasm` and `sxml-full.wasm` link the host shim and a thin
    export file. The export names stay `sxml_javascript_*`, so `src/index.ts`
    does not change.
  - Kotlin: a JNI glue file (glossary `jni_glue`) turns Kotlin request objects
    into host shim requests. Nothing else is in C.
  - Swift: the Swift package imports the host shim through a module map
    (`CSxmlHost`) and calls it directly. There is no Swift-specific C.

  No binding reimplements value conversion, side results or error copying. A
  fix in the shim fixes all three.
- **The same API in three languages** (below): a `Processor` (resolver,
  optional features), `parse`, `compile` once and `evaluate` many times,
  `value` from host data by the `fn:parse-json` rule, `serialize` with
  `fn:serialize`'s options, XQuery (`compileQuery`), XSLT
  (`compileStylesheet`), XSD (`compileSchema`), side results delivered after
  the call, errors with status, code, description, source name, line and
  column, and the DUCET shipped beside the library.
- **Ownership is explicit.**

  - Kotlin: every object that owns C memory is `AutoCloseable` (`use {}`). A
    `java.lang.ref.Cleaner` releases what is never closed, as the JavaScript
    package's `FinalizationRegistry` does. Objects made by a processor hold it
    strongly, so it is not collected while they live. Closing a processor
    closes everything it made.
  - Swift: ARC. `deinit` releases, and objects retain their processor, so the
    order is always right. There is no `close()`.
- **Threads** (D35: a processor is used by one thread at a time).

  - Kotlin: every call on a processor or its objects holds the processor's
    lock, Cleaner actions included. Separate processors run in parallel.
  - Swift: `Processor` and its objects are not `Sendable`. Strict concurrency
    (Swift 6 language mode) rejects sharing them across isolation domains at
    compile time.
- **Callbacks never recurse into the same processor** (D71). The resolver is
  the only callback that runs during a call. As in JavaScript, it must not use
  its own processor: the binding throws (Kotlin `IllegalStateException`, Swift
  `SxmlUsageError`). An exception it throws is kept by the binding, the shim
  stops with `SXML_STATUS_INPUT_OUTPUT_FAILED`, and the call rethrows the same
  exception. Messages, updated trees and result documents are side results,
  given to the host's callbacks after the call (D216).
- **Text crosses as UTF-8 bytes.** Kotlin passes `ByteArray`s
  (`encodeToByteArray`), never `jstring`. JNI's "modified UTF-8" encodes U+0000
  and supplementary characters differently. Swift passes `String.utf8`.
- **One library per platform**, with every language and every optional feature
  except EXPath File (as `sxml-full.wasm`):

  - Android and JVM: `libsxml_kotlin.so`, which is the library, the host shim
    and the JNI glue, statically linked. Only the JNI entry points are
    exported.
  - Apple: `Sxml.xcframework`, a static library of the library and the host
    shim, for iOS devices, iOS simulators and macOS.
  - Linux, for Swift: the CMake target `sxml_host`.

  A core-only native variant (D189's other link) is not planned. A packet adds
  one when a host asks for it.
- **Nothing changes in the C library but two Android fallbacks** (6.3). Bionic
  declares `aligned_alloc` only from API 28 and `timespec_get` from API 29. The
  library is C17 and compiles with `-Werror` for all four ABIs at API 29 as it
  is (6.1 probe). For `minSdk` 24 (the default taken, D208), `src/allocator.c`
  uses `posix_memalign` and `src/processor.c` uses `clock_gettime` when
  `__ANDROID_API__` is lower. Each is one branch beside the existing `_WIN32`
  one.

## What runs here and what waits for the owner (6.1 probe, 2026-10-20)

The agent environment is Linux x86_64 with 4 cores and network access (Maven
Central sometimes answers 429: retry). It has no KVM and no macOS.

| What | Here | Probe result |
| --- | --- | --- |
| NDK r28c cross-compile, arm64-v8a, armeabi-v7a, x86_64, x86 | yes | `-Werror`, `-Os`, shared `libsxml.so` with the JS shim: builds at API 29 for all four. At API 24 and 28, only `aligned_alloc` (allocator.c) and `timespec_get` (processor.c) fail. Stripped / gzip -9: arm64-v8a 7,693,024 / 1,910,096; armeabi-v7a 6,684,092 / 1,795,498; x86_64 7,923,608 / 1,983,466; x86 7,724,252 / 2,036,922. Needs only libc, libm and libdl |
| Kotlin/JVM with JNI (kotlinc 2.2.20, JDK 21) | yes | A Kotlin `external fun` calls C that runs the shim (parse, compile, evaluate, serialize) and calls back into a Kotlin lambda. 0 leaks |
| Android SDK (cmdline-tools, platform 35, build-tools 35), Gradle 8.14.3, AGP 8.10.1 | yes | `assembleRelease` builds an AAR with the four `.so` files (after a Maven Central 429 retry) |
| Android emulator | no | no `/dev/kvm`, and ARM images do not run on x86 hosts |
| Android device | no | owner or CI |
| Swift 6.2 on Linux, module map over the shim | yes | `swift build` with the shim and library linked through `module CSxmlHost [system]`: XPath with a variable, result `"2,4,6 from Swift"`, 0 leaks |
| iOS, macOS, Xcode, XCFramework, simulator | no | owner's machine |
| Device CI (GitHub Actions) | write only | CI is disabled (D123). The owner turns it on |

The probe tools were downloaded into the scratchpad and deleted. NDK r28c is
2.2 GB unpacked, Swift 6.2 3.2 GB, the Android SDK 0.5 GB and kotlinc 83 MB.
Packet 6.3 adds `tools/fetch-binding-toolchains.sh`, which fetches the pinned
versions into a directory the caller names. Agents use their scratchpad and
delete the tools afterwards, because the disk is shared.

**Pinned versions** (D238): NDK r28c (16 KB pages by default, which Android 15
requires of arm64 apps), Android platform 35, `minSdk` 24, Kotlin 2.2.20, JDK
21 (installed), Gradle 8.14.3 (installed), AGP 8.10.1, Swift 6.2 (Linux; on
macOS the Xcode 26 toolchain), iOS 15 and macOS 13 deployment targets.

## The binding APIs (D238)

The same names as the JavaScript package unless the language has its own idiom.
Options are named arguments with defaults, not option objects.

### Kotlin (`bindings/kotlin`, package `dev.tinyactors.sxml`)

```kotlin
val processor = Processor(resolver = { request -> files[request.absoluteUri] })
processor.use {
    val book = processor.parse("<book><title>XPath</title><year>2026</year></book>")
    val expression = processor.compile("//title/string(), xs:integer(//year) + \$n", variables = listOf("n"))
    expression.evaluate(context = book, variables = mapOf("n" to 1)).use { result ->
        result.toString()                          // adaptive method: "\"XPath\"\n2.027e3"
        result.serialize(mapOf("method" to "text")) // "XPath 2027"
    }
}
```

- `Sxml.version`, `Sxml.features` (the library's optional features, by name).
- `Processor(resolver: Resolver? = null, features: Set<String>? = null,
  collationData: CollationData = CollationData.Bundled)`. `features = null`
  means all; an unknown name is `IllegalArgumentException`.
  `CollationData.None` turns the shipped DUCET off, and
  `CollationData.Bytes(ByteArray)` supplies another one. The library loads with
  the class (`System.loadLibrary("sxml_kotlin")`). There is no `load()`,
  because the module is not fetched as a WebAssembly file is.
- `parse(xml: String | ByteArray, uri: String? = null): Value`;
  `compile(expression, variables: List<String> = emptyList(), namespaces:
  Map<String, String> = emptyMap(), baseUri: String? = null): XPathExpression`,
  whose `evaluate(context: Any? = null, variables: Map<String, Any?> =
  emptyMap())` returns a `Value`; `evaluate(expression, ...)` compiles and
  evaluates once; `value(data: Any?)`; `serialize(value, options)`.
- **Input** (`Any?`): a `Value` of this processor, a `String` (an `xs:string`),
  or JSON-shaped data that `fn:parse-json` reads: `Number`, `Boolean`, `null`
  (the empty sequence), `List<*>`, `Array<*>` and `Map<String, *>`. Anything
  else is `IllegalArgumentException`. This is the one rule of D138.
- `Value : AutoCloseable`: `toString()` (adaptive), `serialize(options:
  Map<String, Any?> = emptyMap())`, whose keys may be camelCase.
- XQuery: `compileQuery(text, namespaces, baseUri): XQuery`, with
  `evaluate(context, variables, updates: UpdateSink? = null, messages:
  ((Message) -> Unit)? = null)`, `serialize(value)`, `run(...)` and
  `isUpdating`. `UpdateSink` has `updatedTree(UpdatedTree)` and
  `putDocument(PutDocument)` with defaults; without a `putDocument`, fn:put
  raises FOUP0002 (D216).
- XSLT: `compileStylesheet(text, baseUri, staticParameters): Stylesheet`, with
  `transform(source, initialMatchSelection, initialMode, initialTemplate,
  initialFunction, args, parameters, templateParameters, tunnelParameters,
  baseOutputUri, delivery: ResultDelivery = SERIALIZED, resultDocumentDelivery,
  serialization, messages): TransformResult` (`output`, `value`,
  `resultDocuments`). `InitialMode.Unnamed` selects the unnamed mode.
- XSD: `compileSchema(documents: List<SchemaDocument>, baseUri): Schema`, where
  `SchemaDocument` is `Text(text, uri)` or `Resolved(uri)`, with
  `validate(input: Value | String | ByteArray, mode, type, uri):
  ValidationResult` (`validity: Validity`, `reports`, `document`).
- `fun interface Resolver { fun resolve(request: ResourceRequest): Resource?
  }`. `ResourceRequest(kind: ResourceKind, uriReference, absoluteUri)`.
  `Resource.Text(String)` or `Resource.Bytes(ByteArray)`. `ResourceKind` is an
  enum of the 13 kinds of the JavaScript package, `XML_DOCUMENT` to
  `LOCALE_DATA`.
- Errors: `open class SxmlException(status: Status, code: String,
  codeNamespaceUri, codeLocalName, description, sourceName, lineNumber,
  columnNumber) : Exception`; `class SchemaException(..., reports:
  List<SchemaReport>)`. `Status` is an enum of `sxml_status`'s names. A failed
  allocation is `SxmlException(OUT_OF_MEMORY)`, not `OutOfMemoryError`.
- Memory: `Processor`, `XPathExpression`, `XQuery`, `Stylesheet`, `Schema` and
  `Value` are `AutoCloseable`, and `close()` is idempotent. Using an object
  after it is closed is `IllegalStateException`. The values a call gives you
  (results, updated trees, result documents) are yours to close.
  `Processor.liveAllocationCount()` is for tests.

### Swift (`bindings/swift`, module `Sxml`; `Package.swift` at the repository root)

```swift
import Sxml

let processor = try Processor(resolver: { request in files[request.absoluteURI].map(Resource.text) })
let book = try processor.parse("<book><title>XPath</title><year>2026</year></book>")
let expression = try processor.compile("//title/string(), xs:integer(//year) + $n", variables: ["n"])
let result = try expression.evaluate(context: book, variables: ["n": 1])
print(result)                                  // adaptive method, CustomStringConvertible
try result.serialize(["method": "text"])         // "XPath 2027"
```

- `Sxml.version`, `Sxml.features`.
- `Processor(resolver: Resolver? = nil, features: Set<String>? = nil,
  collationData: CollationData = .bundled) throws`. `typealias Resolver =
  (ResourceRequest) throws -> Resource?`. `enum Resource { case text(String),
  bytes([UInt8]) }`. `enum ResourceKind: String` uses the JavaScript names
  (`"xml-document"`, ...).
- `parse(_ xml: String | [UInt8], uri:)`, `compile(_:variables:namespaces:
  baseURI:)`, `XPathExpression.evaluate(context:variables:)`,
  `evaluate(_:...)`, `value(_:)`, `serialize(_:options:)`.
- **Input**: `protocol XdmInput`. `Value`, `String`, `Bool`, `Int`, `Double`,
  `[XdmInput?]`, `[String: XdmInput?]` and `nil` (the empty sequence) conform,
  with the same `fn:parse-json` rule.
- XQuery, XSLT and XSD as in Kotlin, with Swift argument labels
  (`compileQuery(_:namespaces:baseURI:)`,
  `query.evaluate(context:variables:updates:messages:)`,
  `stylesheet.transform(source:...)`,
  `compileSchema(_:baseURI:).validate(_:mode:type:uri:)`). `UpdateSink` is a
  struct of two optional closures.
- Errors: `struct SxmlError: Error, CustomStringConvertible` (`status: Status`,
  `code`, `codeNamespaceURI`, `codeLocalName`, `description`, `sourceName`,
  `lineNumber`, `columnNumber`); `struct SchemaError: Error` (`error`,
  `reports`); `enum SxmlUsageError: Error` for misuse (a resolver that uses its
  processor, a value of another processor). The calls use untyped `throws`,
  because a resolver's own error is rethrown as it is.
- Memory: `final class`es under ARC. Objects retain their processor. A resolver
  must not capture its processor strongly (a cycle), and it may not use it
  anyway. The DUCET is a SwiftPM resource (`Bundle.module`).

## Tests

- **Parity by name**: every binding has the tests of the JavaScript package's
  `test/package.test.ts` and `test/full.test.ts`, with the same names, the same
  inputs and the same expected text, plus tests of its own ownership rules
  (close and Cleaner in Kotlin, ARC in Swift, threads). 6.12 compares the three
  lists.
- **Leaks and allocation failures** (D23), as the JavaScript tests do: every
  test ends with the processor's live allocation count 0, and one test runs the
  allocation-failure loop through `failingAllocationNumber`.
- **ctest**, so the usual gates see the bindings:

  - `kotlin/jvm`: the JNI glue built for the host. The tests are compiled by
    kotlinc (`SXML_KOTLINC`, or `kotlinc` on `PATH`) with `kotlin.test` and a
    small `main` runner, so ctest needs no Gradle and no network. It is skipped
    when kotlinc or JNI is not found.
  - `swift/linux`: `swift build` and `swift test` against `sxml_host` from the
    same build tree. It is skipped without `swift`.
  - `android/build`: under an Android preset, the four ABIs build with
    `-Werror` and their unit tests compile. A size report is printed.
- **On devices** (owner or CI):

  - Android instrumented tests: the same Kotlin test sources as an
    `androidTest` source set. They run on an x86_64 emulator at API 24 and API
    35, and on one arm64 device.
  - Swift: `xcodebuild test` on an iOS simulator and one iOS device; `swift
    test` on macOS.

## Parallelism plan

| Wave | Packets (run together) | Where |
| --- | --- | --- |
| 1 | 6.2 host shim, 6.3 Android build | here |
| 2 | 6.4 Kotlin core, 6.7 Swift core | here |
| 3 | 6.5 Kotlin full, 6.8 Swift full | here |
| 4 | 6.6 Android library (assemble here; device runs: owner), 6.9 Apple builds (owner) | here / owner |
| 5 | 6.10 device CI, 6.11 documentation and release steps | write here; run and publish: owner |
| 6 | 6.12 review (gate) | here, with the device boxes deferred to the owner (D181) |

Dependencies ("After"):

- 6.2 and 6.3 after 6.1.
- 6.4 and 6.7 after 6.2 and 6.3.
- 6.5 after 6.4; 6.8 after 6.7.
- 6.6 after 6.3 and 6.5; 6.9 after 6.8.
- 6.10 after 6.6 and 6.9's scripts; 6.11 after 6.6 and 6.8.
- 6.12 after everything.

Critical path: 6.2, 6.4, 6.5, 6.6, 6.10, 6.12. The Swift line (6.7, 6.8) runs
beside it.

## Packets

### 6.1 Phase 6 plan (design)
**Done** 2026-10-20 (D238): this file, the probe above, glossary terms
`host_shim`, `jni_glue`, `native_request`, `kotlin_package`, `swift_package`,
`device_test`, and the PLAN.md row.

### 6.2 The host shim (implementation, here)
**Done** 2026-10-20 (D240): `bindings/host/` and target `sxml_host`, the
resolver as a state and a function, three JavaScript export files,
`tests/host_shim_test.c`; `sxml.wasm` +388 bytes, `sxml-full.wasm` +561. After
6.1. Owns new `bindings/host/` (`sxml_host.h`, `sxml_host_internal.h`,
`sxml_host.c`, `sxml_host_xquery.c`, `sxml_host_xslt.c`, `sxml_host_schema.c`,
`sxml_host_full.c`, `sxml_host_core_features.c`, `sxml_host_full_features.c`,
`CMakeLists.txt`), and the JavaScript shim files it replaces in
`bindings/javascript/` (they become `sxml_javascript_exports.c` and
`sxml_javascript_host.c`: the export names and the `sxml_host.resolve` import),
`bindings/javascript/CMakeLists.txt`, and the top-level `CMakeLists.txt` line
that adds `bindings/host`.

- Moves the shim as it is, renamed `sxml_host_*`. It is still written against
  the public API only (D138).
- The processor configuration gains `sxml_host_resolver { void *state; uint32_t
  (*resolve)(void *state, const sxml_host_resource_request *,
  sxml_host_resource *); }`. Under WASI, the JavaScript export passes a
  function that calls the import.
- The static assertions on 32-bit fields apply under `__wasm__` only.
- Glossary: `javascript_shim` now names the export file.
- Done when:

  - `src/index.ts` is unchanged;
  - `javascript/package`, `javascript/package_full` and
    `javascript/languages_full` pass under the `wasm` preset (WASI SDK and Node
    are installed here);
  - `sxml.wasm` and `sxml-full.wasm` are within 1 KB of their size before
    (report both);
  - a native unit test (`tests/host_shim_test.c`) runs parse, compile, evaluate
    and serialize through a resolver function under the `debug` preset, with 0
    leaks.

Golden: `bindings/javascript/sxml_javascript.c`.

### 6.3 Android build and the two fallbacks (implementation, here)
**Paused** 2026-10-20 (D251, D255): the fallbacks, the presets,
`tools/build-android.sh` and `tools/fetch-binding-toolchains.sh` are committed.
All four ABIs build with `-Werror` at android-24, and LOAD is 0x4000 on
arm64-v8a and x86_64. Device runs are the owner's (no KVM here). Resume from
`bindings/kotlin/DESIGN.md`. After 6.1, and after any running packet commits
its changes to `src/processor.c` (it was being edited when this plan was
written). Owns:

- `src/allocator.c` (`sxml_heap_allocate`) and `src/processor.c` (the system
  clock), one `__ANDROID_API__` branch each;
- `CMakePresets.json` (new presets `android-arm64-v8a`, `android-armeabi-v7a`,
  `android-x86_64` and `android-x86`, toolchain
  `$env{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake`,
  `ANDROID_PLATFORM` android-24, MinSizeRel);
- new `tools/build-android.sh` (all four ABIs, a size report);
- new `tools/fetch-binding-toolchains.sh` (`ndk`, `android-sdk`, `kotlin`,
  `swift` into a named directory, with pinned versions and SHA-256 sums);
- `docs/C_GUIDELINES.md` (the Android platform note).

Done when:

- all four ABIs build with `-Werror` at android-24, the library and the unit
  tests (built, not run);
- `readelf` shows 16 KB `LOAD` alignment on arm64-v8a and x86_64;
- the `debug` preset still passes the allocator and clock tests.

Golden: the `_WIN32` branch of `src/allocator.c`.

### 6.4 Kotlin core (implementation, here)
**Done** 2026-10-20 (D247): `bindings/kotlin/` (glue, `sxml_kotlin.map`, the
binding, 36 tests), `tools/run-kotlin-tests.sh`, ctest `kotlin/jvm`; Kotlin
2.2.20 on JDK 21, 0 leaks. The JVM half only; Android is 6.6's. After 6.2 and
6.3. Owns new `bindings/kotlin/`: `CMakeLists.txt` (target `sxml_kotlin`, a
shared library of the library, the host shim and the glue, with a version
script exporting `Java_*` and `JNI_OnLoad` only),
`src/main/c/sxml_kotlin_jni.c`, `src/main/kotlin/` (Processor, Value,
XPathExpression, Resolver, ResourceKind, Status, SxmlException, the internal
`Native` object and request classes), `src/test/kotlin/` (the package tests),
and `tools/run-kotlin-tests.sh`.

- **JNI rules** (D238): every native is `(JNIEnv *environment, jclass owner,
  jobject request)`, three parameters, where `request` is an internal
  `native_request` object.

  - Its fields are read with field IDs that a per-processor table caches. The
    table lives in the processor's glue state, not in globals.
  - Results and failures go back into the same object: the handle, a
    `NativeError` and the side results.
  - Text is `ByteArray` UTF-8.
  - The resolver bridge is a Kotlin method on the processor's internal state.
    It catches what the user's resolver throws, keeps it, and returns a
    failure. The call then rethrows it from Kotlin, so the glue never handles a
    pending JVM exception.
- The glue follows `docs/C_GUIDELINES.md` (spelled-out names, no globals, no
  recursion). It is linted like the rest of `bindings/`.
- Done when:

  - ctest `kotlin/jvm` passes the XPath half of the parity list, the close and
    Cleaner tests and a two-thread test, with 0 leaks;
  - the allocation-failure loop passes;
  - clang-tidy is clean on the glue.

Golden: `bindings/javascript/src/index.ts` for the shape and the host shim for
the C.

### 6.5 Kotlin: XQuery, XSLT, XSD and resource data (implementation, here)
**Done** 2026-10-20 (D250): `compileQuery`, `compileStylesheet`,
`compileSchema` over the side results, seven JNI entry points, the DUCET class
path resource, `bindings/kotlin/README.md`; `kotlin/jvm` 65 tests (25 of
`full.test.ts`), 0 leaks with LeakSanitizer on; the host shim refuses U+0000.
The examples above serialize by the text method (the JSON method raised
SERE0023 on two items). After 6.4. Owns the Kotlin and glue files for
`compileQuery`, `compileStylesheet` and `compileSchema`: side results,
`UpdateSink`, `Message`, `TransformResult` and `ValidationResult`. It also adds
the DUCET as a Java resource (`dev/tinyactors/sxml/allkeys.txt`, copied from
`third_party/unicode-collation/allkeys.txt` by the build) and `CollationData`.
Done when `kotlin/jvm` passes the whole parity list (`full.test.ts` included)
with 0 leaks. Golden: `sxml_javascript_xquery.c` and its TypeScript
counterpart.

### 6.6 Android library (implementation; assemble here, devices: owner)
After 6.3 and 6.5. Owns:

- `bindings/kotlin/android/`: a Gradle project, AGP with `externalNativeBuild`
  running the top-level CMake with `SXML_BUILD_TESTS=OFF`,
  `SXML_BUILD_APPS=OFF` and `SXML_BUILD_KOTLIN=ON` for the four ABIs;
- the shared Kotlin sources by `srcDir`, consumer ProGuard rules (keep the
  native methods and the request classes);
- `androidTest`, which is the same test sources run by an instrumentation
  runner;
- `bindings/kotlin/size-budget.txt`.

Done here when:

- `assembleRelease` and `assembleAndroidTest` succeed;
- the AAR holds the four `.so` files and `allkeys.txt`;
- the size report is below the budget, proposed (D208) at 9,000,000 bytes
  stripped and 2,400,000 gzipped for `libsxml_kotlin.so` on arm64-v8a (the
  probe's whole `libsxml.so`: 7,693,024 and 1,910,096).

Owner: `connectedAndroidTest` on an x86_64 emulator at API 24 and 35 and on one
arm64 device.

### 6.7 Swift core (implementation, here)
**Paused** 2026-10-20 (D255): done as below; the Swift line (6.7 to 6.9)
resumes from `bindings/swift/DESIGN.md`. **Done** 2026-10-20 (D241):
`Package.swift`, `bindings/swift/` (module map, `Sources/Sxml`, 27 tests),
ctest `swift/linux` over a merged `libsxml_host.a`; Swift 6.2 on Linux, 0
leaks. After 6.2 and 6.3. Owns:

- `Package.swift` at the repository root (SwiftPM needs it there);
- `bindings/swift/Sources/CSxmlHost/` (`module.modulemap` over `sxml_host.h`);
- `bindings/swift/Sources/Sxml/`;
- `bindings/swift/Tests/SxmlTests/`;
- `bindings/swift/CMakeLists.txt`, which builds `sxml_host` and adds the ctest
  `swift/linux` running `swift test` with the include and library paths of the
  build tree. The manifest reads `SXML_HOST_LIBRARY_PATH` for them; without it,
  Apple platforms use the XCFramework of 6.9.

The Swift code uses Swift 6 language mode. `CSxmlHost` is imported with
`internal import`, so no C type appears in the module's interface. Done when
`swift/linux` passes the XPath half of the parity list and the ARC tests with 0
leaks. Golden: `src/index.ts`; the probe's module map.

### 6.8 Swift: XQuery, XSLT, XSD and resource data (implementation, here)
**Paused** 2026-10-20 (D255): done as below; resume from
`bindings/swift/DESIGN.md`. **Done** 2026-10-20 (D248): `compileQuery`,
`compileStylesheet`, `compileSchema`, the DUCET as a SwiftPM resource;
`swift/linux` passes 55 tests (the whole parity list) with 0 leaks. After 6.7.
As 6.5 for Swift. The DUCET is a SwiftPM resource. Done when `swift/linux`
passes the whole parity list with 0 leaks.

### 6.9 Apple builds and iOS tests (owner's machine)
**Paused** 2026-10-20 (D255): not started. The steps, in order, are in
`bindings/swift/DESIGN.md`, section 3.1. After 6.8. Owns:

- `cmake/apple.cmake` (CMake's `CMAKE_SYSTEM_NAME` iOS with `CMAKE_OSX_SYSROOT`
  iphoneos and iphonesimulator, and macOS);
- `tools/build-xcframework.sh` (static `sxml_host` for ios-arm64,
  ios-arm64_x86_64-simulator and macos-arm64_x86_64, then `xcodebuild
  -create-xcframework`);
- the manifest's `binaryTarget` for Apple platforms (a local path, and the
  release URL and checksum once releases exist).

An agent may write the scripts here; they are not run here. Done when
`xcodebuild test` passes on an iOS simulator and one device, and `swift test`
passes on macOS, with 0 leaks. This is also the first macOS build of the
library (D123's known risk).

### 6.10 Device CI (write here; the owner enables it)
After 6.6 and 6.9's scripts. Owns new `.github/workflows/bindings.yml`,
disabled as D123 asks (`workflow_dispatch` only) until the owner turns CI back
on. The jobs:

- Linux: the `kotlin/jvm` and `swift/linux` ctests;
- Android: `reactivecircus/android-emulator-runner` on an ubuntu runner with
  KVM, x86_64, API 24 and 35;
- macOS: `xcodebuild test` on an iOS simulator, and `swift test`;
- device farms (Firebase Test Lab for Android, a physical iOS device) as jobs
  that need secrets the owner adds.

The workflow is checked here with `actionlint`, if that can be fetched, and not
run.

### 6.11 Documentation and release steps (here; publishing: owner)
After 6.6 and 6.8. Owns:

- `bindings/kotlin/README.md` and `bindings/swift/README.md`, written after the
  JavaScript README's sections;
- the bindings' part of `docs/RELEASE.md`: Maven Central coordinates
  `dev.tinyactors:sxml-android` and `:sxml-jvm`, signing, the Swift package tag
  and the XCFramework checksum;
- the `README.md` and `CHANGELOG.md` lines.

The owner publishes.

### 6.12 Phase 6 review (gate)
After everything. Checks the parity lists, ownership, the JNI rules, the
licence notes of shipped data, sizes and the gate below. Records a D-record.
The device boxes are deferred to the owner's machine (D181).

## Gate (proposed in D238)

- [ ] `kotlin/jvm` and `swift/linux` pass the whole parity list, with 0 leaks
      and the allocation-failure loop (here).
- [ ] All four Android ABIs build with `-Werror` at API 24. The AAR assembles,
      and `libsxml_kotlin.so` is within its size budget (here).
- [ ] Android instrumented tests pass on an x86_64 emulator (API 24 and 35) and
      one arm64 device (owner, CI).
- [ ] Swift tests pass on macOS, an iOS simulator and one iOS device (owner,
      CI).
- [ ] `.github/workflows/bindings.yml` is green once the owner enables CI
      (owner).
- [ ] The JavaScript package's tests and sizes are unchanged by the host shim.
      The Phase 2 to 7 counts have not dropped (here).
- [ ] Review agent pass (D11), findings fixed.
