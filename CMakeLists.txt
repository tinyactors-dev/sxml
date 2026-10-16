cmake_minimum_required(VERSION 3.24)

project(sxml
    VERSION 0.0.0
    DESCRIPTION "Small XML implementation: XDM 4.0, XPath 4.0, XQuery 4.0"
    LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SXML_BUILD_SHARED "Build sxml as a shared library instead of a static one" OFF)
option(SXML_ENABLE_SANITIZERS "Build every target with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SXML_BUILD_TESTS "Build the unit tests" ON)
option(SXML_BUILD_APPS "Build the sxml command-line program and its CLI framework (apps/)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SxmlCompilerFlags)
include(SxmlGrammar)

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

add_subdirectory(third_party)
add_subdirectory(grammars)

# The library. One module, one header pair: include/sxml/<module>.h and
# src/<module>.c (docs/C_GUIDELINES.md, "Modules and headers").
set(SXML_SOURCES
    src/allocator.c
    src/memory.c
    src/status.c
    src/string.c
    src/version.c
)

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
target_link_libraries(sxml PRIVATE sxml_tree_sitter)
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

include(SxmlLint)
