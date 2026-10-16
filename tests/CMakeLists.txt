# Unit tests (docs/DECISIONS.md, D16): one executable per module,
# tests/<module>_test.c, registered as one ctest test named <module>.

add_library(sxml_test_runner STATIC test_runner.c)
target_include_directories(sxml_test_runner PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
sxml_apply_project_flags(sxml_test_runner)

# sxml_add_test(<module>): builds tests/<module>_test.c against sxml and the
# runner, and registers it with ctest. Test code may see the library's
# internal headers in src/, and through them the tree-sitter runtime and,
# when vendored, libmpdec.
function(sxml_add_test module)
    add_executable(${module}_test "${module}_test.c")
    target_link_libraries(${module}_test PRIVATE sxml sxml_test_runner sxml_tree_sitter)
    if(TARGET sxml_mpdecimal)
        target_link_libraries(${module}_test PRIVATE sxml_mpdecimal)
    endif()
    target_include_directories(${module}_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    sxml_apply_project_flags(${module}_test)
    add_test(NAME ${module} COMMAND ${module}_test)
endfunction()

# Every tests/<name>_test.c is the ctest test <name> (D45). The glob is not
# recursive: tests/cli/ is registered from apps/CMakeLists.txt (D44), and
# tests/qt4/driver from the root CMakeLists.txt.
file(GLOB SXML_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.c")
list(SORT SXML_TEST_SOURCES)
foreach(test_source IN LISTS SXML_TEST_SOURCES)
    get_filename_component(test_file_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" module "${test_file_name}")
    sxml_add_test(${module})
endforeach()

# Fuzz targets (D45): only with SXML_BUILD_FUZZERS, once tests/fuzz/ exists.
if(SXML_BUILD_FUZZERS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
    add_subdirectory(fuzz)
endif()
