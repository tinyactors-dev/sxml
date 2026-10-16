# tree-sitter grammars (docs/DECISIONS.md, D12 and D15).
#
# Every grammars/<language>/ with a generated src/parser.c becomes the static
# library sxml_grammar_<language> (cmake/SxmlGrammar.cmake). A new grammar
# needs no edit here: generate it with tools/generate-grammars.sh and
# re-run CMake.

file(GLOB SXML_GRAMMAR_PARSERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/src/parser.c")
list(SORT SXML_GRAMMAR_PARSERS)
foreach(parser_path IN LISTS SXML_GRAMMAR_PARSERS)
    get_filename_component(source_directory "${parser_path}" DIRECTORY)
    get_filename_component(grammar_directory "${source_directory}" DIRECTORY)
    get_filename_component(language "${grammar_directory}" NAME)
    sxml_add_grammar(${language})
endforeach()
