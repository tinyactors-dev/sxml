# Vendored third-party code. Exempt from docs/C_GUIDELINES.md and the lint
# gates (D12); built with sxml_apply_third_party_flags.

# tree-sitter runtime, pinned in third_party/tree-sitter/VERSION.
# The single translation unit lib.c includes every runtime source.
# TREE_SITTER_FEATURE_WASM stays undefined: grammars are compiled in, never
# loaded as WebAssembly at run time.
add_library(sxml_tree_sitter STATIC tree-sitter/lib/src/lib.c)
target_include_directories(sxml_tree_sitter
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter/lib/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter/lib/src")
# Same feature macros as the upstream Makefile; the runtime uses POSIX
# functions such as fdopen.
target_compile_definitions(sxml_tree_sitter PRIVATE
    _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE _DARWIN_C_SOURCE)
set_target_properties(sxml_tree_sitter PROPERTIES C_VISIBILITY_PRESET hidden)
sxml_apply_third_party_flags(sxml_tree_sitter)
