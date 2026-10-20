# The Kotlin and Android binding: design and pause notes

Status on 2026-10-20: **paused by the owner**. The JVM binding is done
(6.4, 6.5). The Android native build is done (part of 6.3). The AAR (6.6),
device CI (6.10), docs and publishing (6.11) and the review (6.12) are not
started. This file is enough to resume. The decisions behind it are D238
(plan), D239 (names), D240 (host shim), D247 (Kotlin core), D250 (full
surface) and D251 (Android build). The task file is
`docs/tasks/phase-6.md`. The user-facing documentation is
`bindings/kotlin/README.md`.

## Shape

One C mechanism serves every host. The host shim (`bindings/host/`,
`sxml_host_*`, D240) turns the public API into handles, flat requests,
one error per processor, side results (D216) and JSON-shaped input.
Kotlin adds a single C file, the JNI glue. Nothing in `src/` or
`include/` is Kotlin-specific. The library itself has two Android
branches only (below).

```
dev.tinyactors.sxml (Kotlin)  ->  Native (external funs, request objects)
  -> sxml_kotlin_jni.c (JNI glue)  ->  sxml_host (host shim)  ->  libsxml
```

`libsxml_kotlin.so` is the library with every optional feature except
EXPath File, plus the host shim and the glue, linked statically. The
version script `sxml_kotlin.map` exports only `Java_*` and `JNI_OnLoad`.
It needs only libc, libm and libdl. The same `.so` serves Android and
the JVM.

## API (package `dev.tinyactors.sxml`)

The JavaScript package's concepts in Kotlin idiom. Options are named
arguments with defaults. `README.md` has the full reference.

- `Sxml.version`, `Sxml.features`.
- `Processor(resolver: Resolver? = null, features: Set<String>? = null,
  collationData: CollationData = CollationData.Bundled,
  failingAllocationNumber: Int = 0)`. `null` features means all of them.
  An unknown feature name is `IllegalArgumentException`.
- `parse(xml: String | ByteArray, uri)` returns a `Value`.
  `compile(expression, variables, namespaces, baseUri)` returns an
  `XPathExpression` with `evaluate(context, variables)`. There are also
  `evaluate(expression, ...)`, `value(data)` and `serialize(value,
  options)`.
- Input (`Any?`): a `Value` of this processor, a `String`, or
  JSON-shaped data (`Number`, `Boolean`, `null`, `List`, `Array`,
  `Map<String, *>`), read by the `fn:parse-json` rule (D138). Anything
  else is `IllegalArgumentException`.
- `Value.toString()` uses the adaptive method. `Value.serialize(options)`
  is `fn:serialize`: method xml by default, and keys may be camelCase.
- XQuery: `compileQuery` returns an `XQuery` with `evaluate(context,
  variables, updates: UpdateSink?, messages)`, `serialize`, `run` and
  `isUpdating`. `UpdateSink(updatedTree, putDocument)` takes two
  optional functions. Without `putDocument`, fn:put raises FOUP0002.
- XSLT: `compileStylesheet` returns a `Stylesheet`. Its `transform(...)`
  takes JavaScript's options as named arguments and returns a
  `TransformResult` (`output`, `value`, `resultDocuments`).
  `InitialMode` is `Named(name)` or `Unnamed`. `ResultDelivery` carries
  `sxml_xslt_result_delivery`'s numbers.
- XSD: `compileSchema(List<SchemaDocument> | one document | text)`,
  where a `SchemaDocument` is `Text(text or bytes, uri)` or
  `Resolved(uri)`. `Schema.validate(Value | String | ByteArray, mode,
  type, uri)` returns a `ValidationResult` (`validity`, `reports`,
  `document`).
- `fun interface Resolver { fun resolve(ResourceRequest): Resource? }`.
  A `Resource` is `Text` or `Bytes`. `ResourceKind` has all 15 kinds of
  `sxml_resource_kind`.
- Errors: `open class SxmlException(status, code, codeNamespaceUri,
  codeLocalName, description, sourceName, lineNumber, columnNumber)`.
  `SchemaException` is a subclass of it, as in JavaScript (Swift
  differs; 6.12 compares the two). Out of memory is
  `SxmlException(OUT_OF_MEMORY)`. Misuse is `IllegalStateException`
  (closed object, resolver using its processor) or
  `IllegalArgumentException`.

## JNI rules (D238, D247, D250)

- Every native is `(JNIEnv *, jclass, jobject request)`: three
  parameters. There are 17 entry points in `Native`:
  - 10 from 6.4: processor, parse, string, compile, evaluate, serialize,
    release, library and others;
  - 7 from 6.5: `queryCompile`, `queryEvaluate`, `querySerialize`,
    `stylesheetCompile`, `transform`, `schemaCompile`, `schemaValidate`.

  Also `JNI_OnLoad` (JNI 1.6).
- Request classes are in `Native.kt`:
  - `NativeRequest` (processor, handle, error) and its subclasses;
  - `SideResultRequest`, with `NativeSideResults`, the flat side
    results;
  - `NativeBindings`, names in pairs with one handle each.

  The glue reads these classes' fields by field ID. The field IDs are
  cached in a table in the processor's glue state, never in globals.
  Each entry point needs only one `GetFieldID` call, for `processor`.
- Results and failures go back into the same request object.
- Text is UTF-8 `ByteArray`, never `jstring`, because JNI's modified UTF-8
  differs for U+0000 and supplementary characters. The host shim refuses
  U+0000, U+FFFE and U+FFFF and checks UTF-8 (D250).
- The glue copies a request's texts into one block from the processor's
  allocator, so the allocation-failure loop also covers them.
- If a JVM exception is pending, the glue makes no further JNI call. It
  releases what it made, including side-result values, and returns.
- The resolver bridge is `ProcessorState.resolve(kind, uriReference,
  absoluteUri): Int`. It catches the user's exception and keeps it. The
  shim then fails with INPUT_OUTPUT_FAILED, and Kotlin rethrows the
  exception. The glue never handles a pending exception from a user.
- The glue follows `docs/C_GUIDELINES.md` (no globals, no recursion,
  spelled-out names). JNI's names carry
  `NOLINT(readability-identifier-naming)`.

## Ownership, threads, the cleaner

- `Processor`, `XPathExpression`, `XQuery`, `Stylesheet`, `Schema` and
  `Value` are `AutoCloseable`, and `close()` is idempotent. Using an
  object after `close()` is `IllegalStateException`.
  - Objects hold their processor strongly. Closing a processor closes
    everything it made.
  - Values that a call returns (results, updated trees, put documents,
    result documents, validated documents) belong to the caller.
    `TransformResult`, `ValidationResult`, `UpdatedTree` and
    `PutDocument` close their own values.
- Every call holds the processor's monitor (D35). Threads that share a
  processor take turns, and separate processors run in parallel.
  - The resolver runs during the call, on the calling thread, and must
    not use its processor (D71).
  - Messages and update sinks run after the call and outside the lock,
    so they may use the processor. If a callback throws, the values it
    was not given are left to the cleaner or to the processor's close.
- `NativeCleaner` is a `PhantomReference` with a `ReferenceQueue` and one
  daemon thread. It replaces `java.lang.ref.Cleaner`, which Android has
  only from API 33 (`minSdk` is 24). Its actions run under the
  processor's monitor, and `close()` runs the same action at once.
  `NativeHandle` holds the handle, the tracking and the registration for
  all five object kinds.

## Resources

- The DUCET: CMake copies `third_party/unicode-collation/allkeys.txt` to
  `<build>/bindings/kotlin/resources/dev/tinyactors/sxml/allkeys.txt`.
  It does so under Android builds too. `CollationData.Bundled` reads it
  from the class path. `CollationData.None` turns UCA off, and
  `CollationData.Bytes` supplies another DUCET.
- Time zone and locale data come through the resolver
  (`TIMEZONE_DATA`, `LOCALE_DATA`). The tests read them from
  `third_party/tzdata/zoneinfo` and `third_party/cldr/common` through
  the system property `sxml.sourceDirectory`.
- The library loads with `System.loadLibrary("sxml_kotlin")`, or with
  `System.load` of the system property `sxml.library.path`.

## Done

- **6.4** (D247): the glue, `sxml_kotlin.map`, the XPath half of the
  API, `tools/run-kotlin-tests.sh` and ctest `kotlin/jvm` (kotlinc,
  `kotlin.test`, `-Xcheck:jni`, no Gradle).
- **6.5** (D250): XQuery, XSLT, XSD, the side results and the DUCET
  resource. `kotlin/jvm` passes 65 tests under debug (ASan) with 0 leaks.
  LeakSanitizer is on, with `lsan-jvm.supp` suppressing only JVM frames.
  - 26 tests are named after `test/package.test.ts` and 25 after
    `full.test.ts`. The rest are the binding's own tests.
  - The allocation-failure loop is included.
- **6.3, part done** (D251; commit fb76abe):
  - `src/allocator.c` uses `posix_memalign` below API 28.
  - `src/processor.c` uses `clock_gettime(CLOCK_REALTIME)` below API 29,
    through `sxml_platform_utc_time`.
  - The `CMakePresets.json` presets are `android-arm64-v8a`,
    `android-armeabi-v7a`, `android-x86_64` and `android-x86`. They use
    the toolchain `$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake`
    at android-24, MinSizeRel.
  - `tools/build-android.sh` builds the four ABIs, checks the 16 KB LOAD
    alignment and the JNI-only exports, and writes `sizes.txt`.
  - `tools/fetch-binding-toolchains.sh` fetches the pinned tools and
    checks their SHA-256 sums.
  - `docs/C_GUIDELINES.md` has the Android platform note.

### Android build results (2026-10-20, NDK r28c, API 24, MinSizeRel)

All four ABIs build every target with `-Werror`: the library, the unit
tests, the apps, the conformance drivers and `libsxml_kotlin.so`, 568
targets each. The unit tests are built but not run.

| ABI | stripped | gzip -9 | LOAD alignment |
| --- | ---: | ---: | --- |
| arm64-v8a | 7,558,944 | 1,859,972 | 0x4000 |
| armeabi-v7a | 6,591,092 | 1,747,590 | 0x1000 |
| x86_64 | 7,789,056 | 1,928,181 | 0x4000 |
| x86 | 7,599,348 | 1,980,055 | 0x1000 |

The arm64-v8a size is within the proposed budget of 9,000,000 stripped
and 2,400,000 gzipped (D238). The 64-bit ABIs have 16 KB pages, which
Android 15 needs; the 32-bit ABIs do not need them. Each ABI's build
tree is 1.4 GB, and the four builds take about 2 minutes each on 4 cores.

**Not run: devices and emulators.** This environment has no `/dev/kvm`,
and ARM images do not run on x86 hosts. Running anything on Android needs
the owner's machine or CI with KVM.

## Next steps, in order

1. **The rest of 6.3.**
   - Tick 6.3 in the task file once its done-when is met. The builds,
     the 16 KB alignment and the debug allocator and clock tests are all
     met; only the tick is missing.
   - Optional: the ctest `android/build` that D238 lists. It would wrap
     `tools/build-android.sh` and be skipped (77) without
     `ANDROID_NDK_HOME`. No packet owns it yet: give it to 6.6 or 6.10.
2. **6.6: the AAR** (`bindings/kotlin/android/`).
   - A Gradle project (Gradle 8.14.3, AGP 8.10.1, compileSdk 35,
     minSdk 24, NDK r28c).
   - `externalNativeBuild` runs the top-level `CMakeLists.txt` with
     `SXML_BUILD_TESTS=OFF`, `SXML_BUILD_APPS=OFF` and
     `SXML_BUILD_KOTLIN=ON`, for `abiFilters` arm64-v8a, armeabi-v7a, x86_64 and x86. The target
     is `sxml_kotlin`. `SXML_BUILD_KOTLIN` does not exist yet:
     `bindings/kotlin/CMakeLists.txt` builds `sxml_kotlin` whenever
     `ANDROID` is set. Either add the option or drop it from the task
     file. Naming the target (`targets "sxml_kotlin"`) is enough.
   - The Kotlin sources are shared by `srcDir("../src/main/kotlin")`.
   - Java resources: `allkeys.txt` at `dev/tinyactors/sxml/`, from the
     build tree or `third_party/unicode-collation/`.
   - Consumer ProGuard rules: keep `Native`'s native methods, every
     request class (`NativeRequest` and subclasses, `SideResultRequest`,
     `NativeSideResults`, `NativeBindings`, `NativeError`) with all their
     fields, and `ProcessorState.resolve`. The glue looks them up by
     name (D250).
   - `androidTest`: the same test sources under an instrumentation
     runner, which runs `allSuites` (`packageSuites`, `fullSuites`,
     `bindingSuites`) from a JUnit test. The resource-data cases read
     `third_party/` through `sxml.sourceDirectory`. On a device, either
     skip them or ship the files as test assets.
   - `bindings/kotlin/size-budget.txt`: 9,000,000 stripped and
     2,400,000 gzip -9 for arm64-v8a.
   - Done here: `assembleRelease` and `assembleAndroidTest` succeed,
     the AAR holds the four `.so` files and `allkeys.txt`, and the sizes
     are within the budget. Maven Central sometimes answers 429: retry.
   - Owner: `connectedAndroidTest` on an x86_64 emulator at API 24 and
     35, and on one arm64 device.
3. **6.10: device CI** (`.github/workflows/bindings.yml`,
   `workflow_dispatch` only, D123). Android jobs:
   - `tools/fetch-binding-toolchains.sh ndk`, then
     `tools/build-android.sh`;
   - the AAR assemble;
   - `reactivecircus/android-emulator-runner` (x86_64, API 24 and 35)
     on an ubuntu runner with KVM;
   - Firebase Test Lab for an arm64 device, with secrets the owner adds;
   - the Linux `kotlin/jvm` ctest.

   Check the workflow with `actionlint`. Do not run it.
4. **6.11: docs and publishing.**
   - `bindings/kotlin/README.md` gains an Android section (Gradle
     dependency, ABIs, minSdk 24, ProGuard).
   - `docs/RELEASE.md` gets the Maven Central steps for
     `dev.tinyactors:sxml-android` (the AAR) and `dev.tinyactors:sxml-jvm`
     (the jar with `libsxml_kotlin.so` for each host OS; see open
     question 2) and the signing steps.
   - Add the `README.md` and `CHANGELOG.md` lines. The owner publishes.
5. **6.12: the review** checks the JNI rules, the parity list, the
   licence notes of the shipped DUCET, the sizes and the gate.

## Toolchains and commands that worked

All Linux x86_64. Fetch the tools into a scratch directory and delete
them afterwards, because the disk is shared.

| Tool | Version | Source | SHA-256 |
| --- | --- | --- | --- |
| NDK | r28c | `android-ndk-r28c-linux.zip` | `dfb20d39...aa552f` |
| cmdline-tools | 19.0 (13114758) | `commandlinetools-linux-13114758_latest.zip` | `7ec96528...5317ee` |
| Kotlin | 2.2.20 | `kotlin-compiler-2.2.20.zip` (GitHub) | `81f0264c...4420` |
| Swift | 6.2 (Ubuntu 24.04) | `swift-6.2-RELEASE-ubuntu24.04.tar.gz` | `8e3d63a3...bc077` |

The full sums are in `tools/fetch-binding-toolchains.sh`. The system
provides JDK 21 (OpenJDK 21.0.12), Gradle 8.14.3, Clang 18 and CMake
3.28. The SDK packages are `platforms;android-35`,
`build-tools;35.0.0` and `platform-tools`; the script installs them
with sdkmanager, which checks their sums.

```sh
# Toolchains (prints the variables to set)
tools/fetch-binding-toolchains.sh "$SCRATCH/tools" ndk android-sdk kotlin
export ANDROID_NDK_HOME="$SCRATCH/tools/android-ndk-r28c"

# Android: the four ABIs, sizes in BUILD/sizes.txt
tools/build-android.sh "$SCRATCH/android"            # or: ... DIR arm64-v8a
cmake --preset android-arm64-v8a -B DIR && cmake --build DIR --target sxml_kotlin

# JVM tests
cmake --preset debug && cmake --build --preset debug
SXML_KOTLINC="$SCRATCH/tools/kotlinc/bin/kotlinc" ctest --preset debug -R kotlin/jvm
```

Agents build from a private snapshot (`tools/private-build.sh`), never
in the shared `build/`. `tools/build-android.sh` also works inside a
snapshot.

## Open questions

1. **The ctest `android/build`** (D238) has no owner. The proposal is to
   give it to 6.6.
2. **Contents of `sxml-jvm`.** Which host libraries does it ship:
   linux-x86_64 only, or also linux-aarch64, macOS and Windows? The
   default taken is linux-x86_64 plus macOS arm64, built on the owner's
   machine. This is undecided and outward-facing, so the owner should
   confirm it.
3. **Hiding the 32-bit ABIs.** armeabi-v7a and x86 are built because
   minSdk 24 devices still use them. Dropping them would make the AAR
   about half the size.
4. **`SchemaException` versus Swift's separate `SchemaError`** is for 6.12
   to compare (D250).
5. **The task file's example** serializes with the text method, because
   JSON raises SERE0023 on two items (D250). Nothing else is open.
