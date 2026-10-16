# libFuzzer targets (packet 1.26; docs/C_GUIDELINES.md, "Runtime checks":
# fuzzing of every parser entry point). Built only with SXML_BUILD_FUZZERS,
# by the fuzz preset (Clang, AddressSanitizer, UndefinedBehaviorSanitizer,
# the library instrumented with -fsanitize=fuzzer-no-link).
#
# Every tests/fuzz/<name>_fuzz.c is the executable <name>_fuzz, linked with
# the shared fixture. Each is also a ctest test that runs a few thousand
# generated inputs; tools/fuzz.sh runs them for real, seeded from the
# vendored test suites.

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SXML_BUILD_FUZZERS needs Clang (libFuzzer)")
endif()

add_library(sxml_fuzz_fixture STATIC fuzz_fixture.c)
target_link_libraries(sxml_fuzz_fixture PUBLIC sxml)
target_include_directories(sxml_fuzz_fixture PUBLIC "${PROJECT_SOURCE_DIR}/src")
sxml_apply_project_flags(sxml_fuzz_fixture)

file(GLOB SXML_FUZZ_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_fuzz.c")
list(SORT SXML_FUZZ_SOURCES)
foreach(fuzz_source IN LISTS SXML_FUZZ_SOURCES)
    get_filename_component(fuzz_target "${fuzz_source}" NAME_WE)
    add_executable(${fuzz_target} "${fuzz_source}")
    target_link_libraries(${fuzz_target} PRIVATE sxml_fuzz_fixture sxml sxml_tree_sitter)
    if(TARGET sxml_mpdecimal)
        target_link_libraries(${fuzz_target} PRIVATE sxml_mpdecimal)
    endif()
    target_include_directories(${fuzz_target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer)
    sxml_apply_project_flags(${fuzz_target})
    add_test(NAME ${fuzz_target}
             COMMAND ${fuzz_target} -runs=2000 -seed=1 -max_len=4096
                     -rss_limit_mb=2048 -timeout=10
                     -artifact_prefix=${CMAKE_CURRENT_BINARY_DIR}/)
endforeach()
