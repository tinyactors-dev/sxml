# sxml_add_grammar(<language> [<grammar-directory> <target>])
#
# Builds the committed, generated tree-sitter parser for
# grammars/<language>/ (D12) into the static library sxml_grammar_<language>.
# The generator writes grammars/<language>/src/parser.c, an optional external
# scanner grammars/<language>/src/scanner.c, and grammars/<language>/src/tree_sitter/*.h.
# Regenerate with tools/generate-grammars.sh.
#
# With <grammar-directory> and <target>, builds the parser in
# <grammar-directory>/src into <target> instead: the vendored grammars of
# tree-sitter:parse (third_party/tree-sitter-grammars, D285), whose entry
# point tree_sitter_<language>() is renamed sxml_source_language_<language>()
# so that it never meets one of sxml's own grammars of the same name.

function(sxml_add_grammar language)
    if(ARGC GREATER 2)
        set(grammar_source_directory "${ARGV1}/src")
        set(grammar_target "${ARGV2}")
    else()
        set(grammar_source_directory "${PROJECT_SOURCE_DIR}/grammars/${language}/src")
        set(grammar_target sxml_grammar_${language})
    endif()
    if(NOT EXISTS "${grammar_source_directory}/parser.c")
        message(FATAL_ERROR
            "sxml_add_grammar(${language}): ${grammar_source_directory}/parser.c is missing; "
            "run tools/generate-grammars.sh")
    endif()
    set(grammar_sources "${grammar_source_directory}/parser.c")
    if(EXISTS "${grammar_source_directory}/scanner.c")
        list(APPEND grammar_sources "${grammar_source_directory}/scanner.c")
    endif()
    add_library(${grammar_target} STATIC ${grammar_sources})
    target_include_directories(${grammar_target} PRIVATE "${grammar_source_directory}")
    target_link_libraries(${grammar_target} PUBLIC sxml_tree_sitter)
    if(ARGC GREATER 2)
        target_compile_definitions(${grammar_target} PRIVATE
            "tree_sitter_${language}=sxml_source_language_${language}")
        set_target_properties(${grammar_target} PROPERTIES C_VISIBILITY_PRESET hidden)
        # Upstream scanners define their entry points with "()" rather than
        # "(void)", which Clang's -fsanitize=function reports when the
        # runtime calls them through its "(void)" pointers; the types are
        # compatible in C17, and vendored code stays unmodified (D12).
        target_compile_options(${grammar_target} PRIVATE
            $<$<C_COMPILER_ID:Clang,AppleClang>:-fno-sanitize=function>)
    endif()
    sxml_apply_third_party_flags(${grammar_target})
endfunction()
