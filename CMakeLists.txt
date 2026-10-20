cmake_minimum_required(VERSION 3.24)

project(sxml
    VERSION 0.2.0
    DESCRIPTION "Small XML implementation: XDM 4.0, XPath 4.0, XQuery 4.0"
    LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SXML_BUILD_SHARED "Build sxml as a shared library instead of a static one" OFF)
option(SXML_ENABLE_SANITIZERS "Build every target with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SXML_BUILD_TESTS "Build the unit tests" ON)
option(SXML_BUILD_APPS "Build the sxml command-line program and its CLI framework (apps/)" ON)
option(SXML_BUILD_FUZZERS "Build the fuzz targets in tests/fuzz/ (needs a compiler with libFuzzer)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SxmlCompilerFlags)
include(SxmlGrammar)
include(SxmlParts)

# Sanitizers apply to every target, third-party code included, so that the
# instrumented runtime sees all memory accesses (docs/C_GUIDELINES.md,
# "Runtime checks").
if(SXML_ENABLE_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# WASI test executables run under Node.js unless the toolchain chose an emulator.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI" AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    find_program(SXML_NODE_EXECUTABLE node)
    if(SXML_NODE_EXECUTABLE)
        set(CMAKE_CROSSCOMPILING_EMULATOR
            "${SXML_NODE_EXECUTABLE};--no-warnings;${CMAKE_CURRENT_SOURCE_DIR}/tools/run-wasi.mjs")
    endif()
endif()

# A shared library embeds the static third-party and grammar libraries.
if(SXML_BUILD_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(third_party)
add_subdirectory(grammars)

# The library: every src/*.c (D45). One module, one header pair:
# include/sxml/<module>.h and src/<module>.c, with parts in
# src/<module>_<part>.c (docs/C_GUIDELINES.md, "Modules and headers"). A new
# source file needs no edit here; CONFIGURE_DEPENDS re-runs CMake when the
# set of files changes.
file(GLOB SXML_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(SORT SXML_SOURCES)
# The part registries (D101, D103): generated from the library parts
# (src/function_library_<part>.c), evaluator parts (src/evaluator_<part>.c)
# and lowering parts (src/<name>_lower[_<part>].c) that define a part, so a
# new part needs no edit here or anywhere else. Each registry is one file
# with every part and one per language (D227).
sxml_generate_library_part_registry("${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/function_library_parts.c" sxml_library_part_sources)
sxml_generate_evaluator_part_registry("${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/evaluator_parts.c" sxml_evaluator_part_sources)
sxml_generate_lowering_part_registry("${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lowering_parts.c" sxml_lowering_part_sources)
list(APPEND SXML_SOURCES ${sxml_library_part_sources} ${sxml_evaluator_part_sources}
    ${sxml_lowering_part_sources})

if(SXML_BUILD_SHARED)
    add_library(sxml SHARED ${SXML_SOURCES})
    target_compile_definitions(sxml PUBLIC SXML_SHARED)
else()
    add_library(sxml STATIC ${SXML_SOURCES})
endif()
target_compile_definitions(sxml PRIVATE SXML_BUILDING_LIBRARY)
target_include_directories(sxml
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The XML, XPath, JSON and XQuery parsers are generated tree-sitter
# grammars (D12, D15, D111); decimal arithmetic is libmpdec once
# third_party/mpdecimal/ is vendored (D42, D49).
target_link_libraries(sxml PRIVATE
    sxml_tree_sitter sxml_grammar_xml sxml_grammar_xpath sxml_grammar_json
    sxml_grammar_xquery sxml_grammar_xslt_pattern)
if(TARGET sxml_mpdecimal)
    target_link_libraries(sxml PRIVATE sxml_mpdecimal)
endif()
set_target_properties(sxml PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
sxml_apply_project_flags(sxml)

# Every public header must compile on its own (docs/C_GUIDELINES.md,
# "Headers are self-contained"). One generated translation unit per header
# includes it twice, which also checks the include guard.
file(GLOB SXML_PUBLIC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sxml/*.h")
set(SXML_HEADER_CHECK_SOURCES "")
foreach(header_path IN LISTS SXML_PUBLIC_HEADERS)
    get_filename_component(header_name "${header_path}" NAME_WE)
    set(check_source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_name}.c")
    file(CONFIGURE OUTPUT "${check_source}"
        CONTENT "#include \"sxml/${header_name}.h\"\n#include \"sxml/${header_name}.h\"\n\n/* Keeps the unit non-empty for headers that only define macros. */\ntypedef int sxml_header_check_${header_name};\n")
    list(APPEND SXML_HEADER_CHECK_SOURCES "${check_source}")
endforeach()
add_library(sxml_header_check OBJECT ${SXML_HEADER_CHECK_SOURCES})
target_include_directories(sxml_header_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
sxml_apply_project_flags(sxml_header_check)

if(SXML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The sxml command-line program and its tests (docs/CLI.md, D44). Uses only
# the public API. After tests/, whose test runner its tests link.
if(SXML_BUILD_APPS)
    add_subdirectory(apps)
endif()

# The host shim (D238) under the bindings, in every build; before the
# JavaScript package, whose modules link its sources, and after tests/,
# whose host_shim test links it.
add_subdirectory(bindings/host)

# The JavaScript package (D10, D138): the export files in every build,
# sxml.wasm and its Node.js tests under WASI. After apps/: its size test
# also measures the CLI module.
add_subdirectory(bindings/javascript)

# The Swift package (D238; Package.swift at the root): on Linux, the host
# shim merged into one archive and the ctest swift/linux.
add_subdirectory(bindings/swift)

# The QT4 test-suite driver (D44, D45): a developer tool built on the CLI
# framework, so it comes after apps/. Joins the build once its
# CMakeLists.txt exists.
if(SXML_BUILD_TESTS AND SXML_BUILD_APPS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt4/driver/CMakeLists.txt")
    add_subdirectory(tests/qt4/driver)
endif()

# The XSLT 4.0 test-suite driver (D117): a sibling of the QT4 driver that
# links its library, so it comes after it.
if(SXML_BUILD_TESTS AND SXML_BUILD_APPS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/xslt/driver/CMakeLists.txt")
    add_subdirectory(tests/xslt/driver)
endif()

# The XSD 1.1 test-suite driver (packet 5.3, D168): another sibling that
# links the QT4 driver's library.
if(SXML_BUILD_TESTS AND SXML_BUILD_APPS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/xsd/driver/CMakeLists.txt")
    add_subdirectory(tests/xsd/driver)
endif()

# The iXML community test suite (packet 7.18, D215): run with the sxml
# command, so it comes after apps/.
if(SXML_BUILD_TESTS AND SXML_BUILD_APPS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ixml/CMakeLists.txt")
    add_subdirectory(tests/ixml)
endif()

include(SxmlLint)
