# Lint gates (docs/C_GUIDELINES.md, "Enforcement"; docs/DECISIONS.md, D17).
#
#   lint-clang-tidy  clang-tidy with .clang-tidy over project translation units
#                    (src/, tests/, apps/, bindings/, and one generated unit per public
#                    header).
#   lint-ast-grep    the structural rules in tools/ast-grep/ over include/,
#                    src/, tests/, apps/ and bindings/.
#   lint             both.
#
# Third-party and generated grammar code is never linted (D12).

find_program(SXML_CLANG_TIDY_EXECUTABLE NAMES clang-tidy clang-tidy-18 clang-tidy-19 clang-tidy-20)
find_program(SXML_AST_GREP_EXECUTABLE ast-grep
    HINTS "${PROJECT_SOURCE_DIR}/tools/node_modules/.bin")

file(GLOB_RECURSE SXML_LINT_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.c"
    "${PROJECT_SOURCE_DIR}/tests/*.c"
    "${PROJECT_SOURCE_DIR}/bindings/*.c")
if(SXML_BUILD_APPS)
    file(GLOB_RECURSE SXML_LINT_APP_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/apps/*.c")
    list(APPEND SXML_LINT_SOURCES ${SXML_LINT_APP_SOURCES})
endif()
list(APPEND SXML_LINT_SOURCES ${SXML_HEADER_CHECK_SOURCES})

if(SXML_CLANG_TIDY_EXECUTABLE)
    add_custom_target(lint-clang-tidy
        COMMAND "${SXML_CLANG_TIDY_EXECUTABLE}"
            -p "${PROJECT_BINARY_DIR}"
            --quiet
            "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
            ${SXML_LINT_SOURCES}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "clang-tidy over project code"
        VERBATIM)
else()
    add_custom_target(lint-clang-tidy
        COMMAND "${CMAKE_COMMAND}" -E echo "clang-tidy not found"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
endif()

if(SXML_AST_GREP_EXECUTABLE)
    add_custom_target(lint-ast-grep
        COMMAND "${SXML_AST_GREP_EXECUTABLE}" scan
            --config "${PROJECT_SOURCE_DIR}/tools/ast-grep/sgconfig.yml"
            --error
            include src tests apps bindings
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "ast-grep rules from tools/ast-grep over project code"
        VERBATIM)
else()
    add_custom_target(lint-ast-grep
        COMMAND "${CMAKE_COMMAND}" -E echo "ast-grep not found; run: npm ci --prefix tools"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
endif()

add_custom_target(lint DEPENDS lint-clang-tidy lint-ast-grep)
