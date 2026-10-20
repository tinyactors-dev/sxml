# tree-sitter grammars (docs/DECISIONS.md, D12 and D15).
#
# Every grammars/<language>/ with a generated src/parser.c becomes the static
# library sxml_grammar_<language> (cmake/SxmlGrammar.cmake) and joins the
# link line of the library sxml, so a front end in src/ may call any
# tree_sitter_<language>(). A new grammar needs no edit here or in the root
# CMakeLists.txt: generate it with tools/generate-grammars.sh and re-run
# CMake.

file(GLOB SXML_GRAMMAR_PARSERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/src/parser.c")
list(SORT SXML_GRAMMAR_PARSERS)
set(SXML_GRAMMAR_LIBRARIES "")
foreach(parser_path IN LISTS SXML_GRAMMAR_PARSERS)
    get_filename_component(source_directory "${parser_path}" DIRECTORY)
    get_filename_component(grammar_directory "${source_directory}" DIRECTORY)
    get_filename_component(language "${grammar_directory}" NAME)
    sxml_add_grammar(${language})
    list(APPEND SXML_GRAMMAR_LIBRARIES sxml_grammar_${language})
endforeach()
# The vendored grammars of sxml:parse-code (third_party/tree-sitter-grammars,
# packet 7.22, D285) join the same list as sxml_source_grammar_<language>;
# src/source_code_languages.c (tools/generate-source-code-languages.py)
# names their entry points.
file(GLOB SXML_SOURCE_GRAMMAR_PARSERS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/third_party/tree-sitter-grammars/*/src/parser.c")
list(SORT SXML_SOURCE_GRAMMAR_PARSERS)
foreach(parser_path IN LISTS SXML_SOURCE_GRAMMAR_PARSERS)
    get_filename_component(source_directory "${parser_path}" DIRECTORY)
    get_filename_component(grammar_directory "${source_directory}" DIRECTORY)
    get_filename_component(language "${grammar_directory}" NAME)
    sxml_add_grammar(${language} "${grammar_directory}" sxml_source_grammar_${language})
    list(APPEND SXML_GRAMMAR_LIBRARIES sxml_source_grammar_${language})
endforeach()
set_property(GLOBAL PROPERTY SXML_GRAMMAR_LIBRARIES ${SXML_GRAMMAR_LIBRARIES})

# The root CMakeLists.txt defines sxml after this directory, so the link is
# deferred to the end of the root directory. A grammar the root already
# links is linked once more, which changes nothing.
function(sxml_link_grammars)
    get_property(libraries GLOBAL PROPERTY SXML_GRAMMAR_LIBRARIES)
    target_link_libraries(sxml PRIVATE ${libraries})
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL sxml_link_grammars)
