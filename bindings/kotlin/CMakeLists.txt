# The Kotlin binding (docs/GLOSSARY.md, "kotlin_package"; D238, D239):
# package dev.tinyactors.sxml over libsxml_kotlin.so.
#
#   sxml_kotlin   libsxml_kotlin.so: the library with every optional
#                 feature but EXPath File, the host shim (sxml_host) and
#                 the JNI glue (src/main/c/sxml_kotlin_jni.c), linked
#                 statically; only the JNI entry points are exported
#                 (sxml_kotlin.map). Built where JNI's headers are found
#                 (a JDK, or the Android NDK's sysroot).
#
# Resource: resources/dev/tinyactors/sxml/allkeys.txt in the build tree,
# the DUCET that CollationData.Bundled reads from the class path.
#
# Test: kotlin/jvm compiles src/main/kotlin and src/test/kotlin with
# kotlinc and runs them on the JVM against sxml_kotlin
# (tools/run-kotlin-tests.sh). It is skipped, with the reason, when JNI,
# kotlinc or java is not found, so that builds without them stay green.

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    return()
endif()

set(sxml_kotlin_has_jni FALSE)
if(ANDROID)
    # jni.h is in the NDK's sysroot.
    set(sxml_kotlin_has_jni TRUE)
else()
    find_package(JNI QUIET)
    if(JNI_FOUND)
        set(sxml_kotlin_has_jni TRUE)
    endif()
endif()

if(sxml_kotlin_has_jni)
    add_library(sxml_kotlin SHARED src/main/c/sxml_kotlin_jni.c)
    if(NOT ANDROID)
        target_include_directories(sxml_kotlin SYSTEM PRIVATE ${JNI_INCLUDE_DIRS})
    endif()
    target_link_libraries(sxml_kotlin PRIVATE sxml_host)
    # The static libraries linked into a shared one.
    set_target_properties(sxml_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(sxml_kotlin PROPERTIES
        C_VISIBILITY_PRESET hidden
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sxml_kotlin.map")
    if(NOT APPLE AND NOT MSVC)
        target_link_options(sxml_kotlin PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/sxml_kotlin.map")
    endif()
    sxml_apply_project_flags(sxml_kotlin)
endif()

# The DUCET of the uca-collation feature as the class path resource
# dev/tinyactors/sxml/allkeys.txt (CollationData.Bundled; D213), copied
# from the vendored file; the tests put the directory on the class path,
# and the Android library packs it (packet 6.6).
set(sxml_kotlin_resources "${CMAKE_CURRENT_BINARY_DIR}/resources")
configure_file("${PROJECT_SOURCE_DIR}/third_party/unicode-collation/allkeys.txt"
    "${sxml_kotlin_resources}/dev/tinyactors/sxml/allkeys.txt" COPYONLY)

if(NOT SXML_BUILD_TESTS OR ANDROID OR CMAKE_CROSSCOMPILING)
    return()
endif()

# The features sxml_all_optional_features() names, as
# javascript/package_full lists them for sxml-full.wasm.
set(sxml_kotlin_features
    "test,expath-binary,html,xslt-transform,timezone-database,dtd-validation,uca-collation,full-text,invisible-xml,localization,builtin-schemas")

if(sxml_kotlin_has_jni)
    set(sxml_kotlin_library "$<TARGET_FILE:sxml_kotlin>")
else()
    set(sxml_kotlin_library "")
endif()

# Under the sanitizers the JVM must load their runtime first (a shared
# library does not link it), and must keep its own SIGSEGV handling.
set(sxml_kotlin_preload "")
if(SXML_ENABLE_SANITIZERS AND NOT MSVC)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(sxml_kotlin_runtime_name "libclang_rt.asan-${CMAKE_SYSTEM_PROCESSOR}.so")
    else()
        set(sxml_kotlin_runtime_name "libasan.so")
    endif()
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" "-print-file-name=${sxml_kotlin_runtime_name}"
        OUTPUT_VARIABLE sxml_kotlin_preload
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

add_test(NAME kotlin/jvm
    COMMAND "${PROJECT_SOURCE_DIR}/tools/run-kotlin-tests.sh"
        "${sxml_kotlin_library}" "${CMAKE_CURRENT_BINARY_DIR}/kotlin-tests")
set_tests_properties(kotlin/jvm PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SXML_KOTLIN_FEATURES=${sxml_kotlin_features};SXML_KOTLIN_PRELOAD=${sxml_kotlin_preload};SXML_KOTLIN_RESOURCES=${sxml_kotlin_resources}")
