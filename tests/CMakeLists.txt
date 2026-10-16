# Unit tests (docs/DECISIONS.md, D16): one executable per module,
# tests/<module>_test.c, registered as one ctest test named <module>.

add_library(sxml_test_runner STATIC test_runner.c)
target_include_directories(sxml_test_runner PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
sxml_apply_project_flags(sxml_test_runner)

# sxml_add_test(<module>): builds tests/<module>_test.c against sxml and the
# runner, and registers it with ctest. Test code may see the library's
# internal headers in src/.
function(sxml_add_test module)
    add_executable(${module}_test "${module}_test.c")
    target_link_libraries(${module}_test PRIVATE sxml sxml_test_runner)
    target_include_directories(${module}_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    sxml_apply_project_flags(${module}_test)
    add_test(NAME ${module} COMMAND ${module}_test)
endfunction()

sxml_add_test(version)

sxml_add_test(tree_sitter_runtime)
target_link_libraries(tree_sitter_runtime_test PRIVATE sxml_tree_sitter)
