# The JavaScript package (D10, D138): the shim sxml_javascript.c, linked
# with the library into sxml.wasm, and the TypeScript package in src/ that
# loads it.
#
#   sxml_javascript         the shim as a library, in every build, so that
#                           -Werror and the lint gates see it.
#   sxml_javascript_module  under WASI only: sxml.wasm, a reactor module
#                           (no main) exporting the shim's functions.
#
# Tests (WASI only, Node.js): javascript/package runs test/package.test.ts;
# javascript/typecheck runs tsc (after `npm ci` here); javascript/size_budget
# checks the size budget of D93 and D138.

add_library(sxml_javascript STATIC sxml_javascript.c)
target_link_libraries(sxml_javascript PUBLIC sxml)
sxml_apply_project_flags(sxml_javascript)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    return()
endif()

add_executable(sxml_javascript_module sxml_javascript.c)
target_link_libraries(sxml_javascript_module PRIVATE sxml)
set_target_properties(sxml_javascript_module PROPERTIES OUTPUT_NAME sxml SUFFIX ".wasm")
target_link_options(sxml_javascript_module PRIVATE -mexec-model=reactor -Wl,--strip-all)
sxml_apply_project_flags(sxml_javascript_module)

if(SXML_BUILD_TESTS AND SXML_NODE_EXECUTABLE)
    add_test(NAME javascript/package
        COMMAND "${SXML_NODE_EXECUTABLE}" --expose-gc --test --test-reporter=dot
            "${CMAKE_CURRENT_SOURCE_DIR}/test/package.test.ts"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(javascript/package PROPERTIES
        ENVIRONMENT "SXML_WASM_PATH=$<TARGET_FILE:sxml_javascript_module>")
    # The TypeScript compiler's checks, when `npm ci` has installed it here.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/bin/tsc")
        add_test(NAME javascript/typecheck
            COMMAND "${SXML_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/bin/tsc"
                -p "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json")
    endif()
    # The CLI's budget grew with sxml xquery, which links the XQuery
    # grammar (packet 3.12, D146), and with sxml transform, which links
    # XSLT and the pattern and value template grammars (packet 4.15, D157;
    # the owner confirms both under Proposed).
    if(TARGET sxml_cli)
        add_test(NAME javascript/size_budget
            COMMAND "${SXML_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/size-budget.mjs"
                "$<TARGET_FILE:sxml_cli>" 5000000 1100000
                "$<TARGET_FILE:sxml_javascript_module>" 2000000 700000)
    endif()
endif()
