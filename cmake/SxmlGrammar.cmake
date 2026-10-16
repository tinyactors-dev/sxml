# sxml_add_grammar(<language>)
#
# Builds the committed, generated tree-sitter parser for
# grammars/<language>/ (D12) into the static library sxml_grammar_<language>.
# The generator writes grammars/<language>/src/parser.c, an optional external
# scanner grammars/<language>/src/scanner.c, and grammars/<language>/src/tree_sitter/*.h.
# Regenerate with tools/generate-grammars.sh.

function(sxml_add_grammar language)
    set(grammar_source_directory "${PROJECT_SOURCE_DIR}/grammars/${language}/src")
    if(NOT EXISTS "${grammar_source_directory}/parser.c")
        message(FATAL_ERROR
            "sxml_add_grammar(${language}): ${grammar_source_directory}/parser.c is missing; "
            "run tools/generate-grammars.sh")
    endif()
    set(grammar_sources "${grammar_source_directory}/parser.c")
    if(EXISTS "${grammar_source_directory}/scanner.c")
        list(APPEND grammar_sources "${grammar_source_directory}/scanner.c")
    endif()
    add_library(sxml_grammar_${language} STATIC ${grammar_sources})
    target_include_directories(sxml_grammar_${language} PRIVATE "${grammar_source_directory}")
    target_link_libraries(sxml_grammar_${language} PUBLIC sxml_tree_sitter)
    sxml_apply_third_party_flags(sxml_grammar_${language})
endfunction()
