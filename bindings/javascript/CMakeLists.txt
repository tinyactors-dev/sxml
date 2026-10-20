# The JavaScript package (D10, D138): the host shim (bindings/host, D238)
# under the export names of the JavaScript shim, linked with the library
# into the WebAssembly modules, and the TypeScript package in src/ that
# loads them.
#
#   sxml_javascript              the export files as a library, in every
#                                build, so that -Werror and the lint gates
#                                see them.
#   sxml_javascript_module       under WASI only: sxml.wasm, the one module
#                                (D272), a reactor (no main) exporting the
#                                shim's functions (sxml_javascript_exports.c
#                                and sxml_javascript_full_exports.c) with
#                                every language and every optional feature
#                                (sxml_host_full_features.c, D189).
#
# sxml_javascript_host.c is the resolver the package's processors use: it
# calls the import sxml_host.resolve.
#
# Tests (WASI only, Node.js): javascript/package runs test/package.test.ts
# and javascript/languages test/full.test.ts on the module;
# javascript/typecheck runs tsc (after `npm ci` here); javascript/size_budget
# checks the size budgets of D93, D138, D189 and D272.

add_library(sxml_javascript STATIC sxml_javascript_exports.c sxml_javascript_full_exports.c
    sxml_javascript_host.c)
target_link_libraries(sxml_javascript PUBLIC sxml_host)
sxml_apply_project_flags(sxml_javascript)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    return()
endif()

# One module with everything (D272): the shim's exports for XPath and for
# XQuery, XSLT and XSD, and every optional feature. The host shim keeps
# its two source lists (bindings/host/CMakeLists.txt); this links both.
add_executable(sxml_javascript_module sxml_javascript_exports.c sxml_javascript_full_exports.c
    sxml_javascript_host.c ${SXML_HOST_CORE_SOURCES} ${SXML_HOST_FULL_SOURCES}
    "${SXML_HOST_INCLUDE_DIRECTORY}/sxml_host_full_features.c")
target_include_directories(sxml_javascript_module PRIVATE "${SXML_HOST_INCLUDE_DIRECTORY}")
target_link_libraries(sxml_javascript_module PRIVATE sxml)
set_target_properties(sxml_javascript_module PROPERTIES OUTPUT_NAME sxml SUFFIX ".wasm")
# The linker map beside the module names what each object adds, for
# javascript/size_budget's growth guard (D225).
target_link_options(sxml_javascript_module PRIVATE -mexec-model=reactor -Wl,--strip-all
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/sxml.map")
sxml_apply_project_flags(sxml_javascript_module)

if(SXML_BUILD_TESTS AND SXML_NODE_EXECUTABLE)
    # The module's feature list is every feature (D189, D272).
    add_test(NAME javascript/package
        COMMAND "${SXML_NODE_EXECUTABLE}" --expose-gc --test --test-reporter=dot
            "${CMAKE_CURRENT_SOURCE_DIR}/test/package.test.ts"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(javascript/package PROPERTIES
        ENVIRONMENT "SXML_WASM_PATH=$<TARGET_FILE:sxml_javascript_module>;SXML_WASM_FEATURES=test,expath-binary,html,xslt-transform,timezone-database,dtd-validation,uca-collation,full-text,invisible-xml,localization,builtin-schemas")
    # XQuery, XSLT and XSD (test/full.test.ts).
    add_test(NAME javascript/languages
        COMMAND "${SXML_NODE_EXECUTABLE}" --expose-gc --test --test-reporter=dot
            "${CMAKE_CURRENT_SOURCE_DIR}/test/full.test.ts"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(javascript/languages PROPERTIES
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
    # XSLT and the pattern and value template grammars (packet 4.15, D157),
    # and with sxml validate, which links schema compilation and validation
    # (packet 5.10, D176; the CLI module's budget is 7,000,000 / 1,500,000
    # since D202), and with the DUCET it embeds for the UCA collation
    # feature (packet 7.15a, D213: 7,500,000 / 1,650,000).
    # The JavaScript module: D184's core and full modules became one in
    # D272, with every language and every optional feature; its budget is
    # the full module's, 8,000,000 / 2,800,000 (D235: XPath with Full Text
    # being a parser of its own, +500,913; earlier D207, D208, D216, D225),
    # and rises as features land (D208). The gzipped limits keep D207's
    # ratio of 0.35.
    # The CLI module embeds the DUCET (D213) and links every language and
    # feature: 9,000,000 / 2,000,000 (D208); 9,500,000 / 2,100,000 since
    # D235, for the same parser.
    # test/size-baseline/sxml.txt is the module's code and data per object
    # at the last audit or budget raise (D225, D272): the test prints the
    # largest growths since then, and names twenty when the module is over
    # budget. Refresh it with the budget (size-budget.mjs --write-baseline).
    if(TARGET sxml_cli)
        add_test(NAME javascript/size_budget
            COMMAND "${SXML_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/size-budget.mjs"
                "$<TARGET_FILE:sxml_cli>" 9500000 2100000
                "$<TARGET_FILE:sxml_javascript_module>" 8000000 2800000)
    endif()
endif()
