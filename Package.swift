// swift-tools-version: 6.0
// The Swift package of sxml (D238): module Sxml over the host shim
// (bindings/host/sxml_host.h), which it imports through the module map
// CSxmlHost. SwiftPM needs the manifest at the repository root.
//
// On Linux, SXML_HOST_LIBRARY_PATH names the directory of libsxml_host.a,
// the archive bindings/swift/CMakeLists.txt makes (the ctest swift/linux
// sets it):
//
//     cmake --build build/release --target sxml_host_swift_library
//     SXML_HOST_LIBRARY_PATH=$PWD/build/release/bindings/swift/lib swift test
//
// The debug preset's archive has ASan and UBSan in it: add
// --sanitize=address (whose runtime has UBSan's handlers) and
// --disable-xctest (XCTest's runner leaks; the tests use Swift Testing).
//
// Without it, Apple platforms link the XCFramework of packet 6.9.
import PackageDescription

let libraryPath = Context.environment["SXML_HOST_LIBRARY_PATH"] ?? ""
let includePath = Context.packageDirectory + "/include"

// The C headers the module map's header includes (include/sxml/), and the
// archive. Unsafe flags are allowed in a root package only; a package that
// depends on this one uses the XCFramework (6.9).
let swiftSettings: [SwiftSetting] =
    [.swiftLanguageMode(.v6)] + (libraryPath.isEmpty ? [] : [.unsafeFlags(["-Xcc", "-I" + includePath])])
let linkerSettings: [LinkerSetting] = libraryPath.isEmpty ? [] : [.unsafeFlags(["-L" + libraryPath])]

let package = Package(
    name: "Sxml",
    products: [
        .library(name: "Sxml", targets: ["Sxml"])
    ],
    targets: [
        .systemLibrary(
            name: "CSxmlHost",
            path: "bindings/swift/Sources/CSxmlHost"),
        .target(
            name: "Sxml",
            dependencies: ["CSxmlHost"],
            path: "bindings/swift/Sources/Sxml",
            swiftSettings: swiftSettings,
            linkerSettings: linkerSettings),
        .testTarget(
            name: "SxmlTests",
            dependencies: ["Sxml"],
            path: "bindings/swift/Tests/SxmlTests",
            swiftSettings: swiftSettings),
    ]
)
