# The JavaScript package (D10, D138): the shim sxml_javascript.c, linked
# with the library into the WebAssembly modules, and the TypeScript package
# in src/ that loads them.
#
#   sxml_javascript              the shim as a library, in every build, so
#                                that -Werror and the lint gates see it.
#   sxml_javascript_module       under WASI only: sxml.wasm, the core module,
#                                a reactor (no main) exporting the shim's
#                                functions, with no optional feature
#                                (sxml_javascript_core_features.c, D189).
#   sxml_javascript_full_module  under WASI only: sxml-full.wasm, the same
#                                shim with every optional feature
#                                (sxml_javascript_full_features.c).
#
# Tests (WASI only, Node.js): javascript/package and javascript/package_full
# run test/package.test.ts on each module; javascript/typecheck runs tsc
# (after `npm ci` here); javascript/size_budget checks the size budgets of
# D93, D138 and D189.

add_library(sxml_javascript STATIC sxml_javascript.c sxml_javascript_core_features.c)
target_link_libraries(sxml_javascript PUBLIC sxml)
sxml_apply_project_flags(sxml_javascript)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    return()
endif()

# One build, two links (D184, D189): the modules differ only in which
# features file they link, so neither has an #if.
foreach(variant IN ITEMS core full)
    if(variant STREQUAL "core")
        set(module_target sxml_javascript_module)
        set(module_name sxml)
    else()
        set(module_target sxml_javascript_full_module)
        set(module_name sxml-full)
    endif()
    add_executable(${module_target} sxml_javascript.c sxml_javascript_${variant}_features.c)
    target_link_libraries(${module_target} PRIVATE sxml)
    set_target_properties(${module_target} PROPERTIES OUTPUT_NAME ${module_name} SUFFIX ".wasm")
    target_link_options(${module_target} PRIVATE -mexec-model=reactor -Wl,--strip-all)
    sxml_apply_project_flags(${module_target})
endforeach()

if(SXML_BUILD_TESTS AND SXML_NODE_EXECUTABLE)
    add_test(NAME javascript/package
        COMMAND "${SXML_NODE_EXECUTABLE}" --expose-gc --test --test-reporter=dot
            "${CMAKE_CURRENT_SOURCE_DIR}/test/package.test.ts"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(javascript/package PROPERTIES
        ENVIRONMENT "SXML_WASM_PATH=$<TARGET_FILE:sxml_javascript_module>")
    # The full module: the same tests, plus its feature list (D189).
    add_test(NAME javascript/package_full
        COMMAND "${SXML_NODE_EXECUTABLE}" --expose-gc --test --test-reporter=dot
            "${CMAKE_CURRENT_SOURCE_DIR}/test/package.test.ts"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(javascript/package_full PROPERTIES
        ENVIRONMENT "SXML_WASM_PATH=$<TARGET_FILE:sxml_javascript_full_module>;SXML_WASM_FEATURES=test,expath-binary,html,xslt-transform")
    # The TypeScript compiler's checks, when `npm ci` has installed it here.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/bin/tsc")
        add_test(NAME javascript/typecheck
            COMMAND "${SXML_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/typescript/bin/tsc"
                -p "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json")
    endif()
    # The CLI's budget grew with sxml xquery, which links the XQuery
    # grammar (packet 3.12, D146), and with sxml transform, which links
    # XSLT and the pattern and value template grammars (packet 4.15, D157),
    # and with sxml validate, which links schema compilation and validation
    # (packet 5.10, D176; the CLI module's budget is 7,000,000 / 1,500,000
    # since D202).
    # The owner's budgets for the JavaScript modules (D184): 2,100,000
    # bytes stripped for the core sxml.wasm and 3,000,000 for the full
    # module; the gzipped limits keep D138's ratio of 700,000 to 2,000,000
    # (D189); raised to 2,200,000 / 770,000 and 4,000,000 / 1,400,000
    # in D207.
    if(TARGET sxml_cli)
        add_test(NAME javascript/size_budget
            COMMAND "${SXML_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/size-budget.mjs"
                "$<TARGET_FILE:sxml_cli>" 7000000 1500000
                "$<TARGET_FILE:sxml_javascript_module>" 2200000 770000
                "$<TARGET_FILE:sxml_javascript_full_module>" 4000000 1400000)
    endif()
endif()
