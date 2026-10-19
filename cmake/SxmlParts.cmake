# Part registries (docs/DECISIONS.md, D45, D101, D103, D227).
#
# A library part is a file src/function_library_<part>.c that defines
#     const sxml_native_function_view sxml_function_library_<part> = ...
# and may define
#     const sxml_language_set sxml_function_library_<part>_languages = ...;
# naming the languages it serves (SXML_LANGUAGE_XPATH, SXML_LANGUAGE_XQUERY,
# SXML_LANGUAGE_XSLT joined by "|"; all three when absent). An evaluator
# part is a file src/evaluator_<part>.c that defines
#     const sxml_evaluator_part sxml_evaluator_part_<part> = ...
# and may define sxml_evaluator_part_<part>_languages the same way. A
# lowering part is a file src/<name>_lower.c or src/<name>_lower_<part>.c
# that defines
#     const sxml_xpath_lowering_part sxml_xpath_lowering_part_<part> = ...
# whose .languages field names its languages.
# The sxml_generate_*_part_registry() functions read those files and write
# translation units into the build tree, so adding a part edits no other
# file: one with every part (for tests and checks), and one per language
# with the parts serving it (D227). A language's entry point names its own
# list, so a module links only the parts of the languages it offers: the
# core JavaScript module, which offers XPath, links no XQuery-only or
# XSLT-only part. Each function sets outputs_variable in the caller's scope
# to the files it wrote. Each part file is a configure dependency, so a
# changed definition re-runs CMake; file(CONFIGURE) rewrites an output only
# when its content changes.

set(SXML_PART_LANGUAGES XPATH XQUERY XSLT)

# Sets output_variable in the caller's scope to the languages that the C
# expression of a language set names: each SXML_LANGUAGE_<language> in it,
# or all three for SXML_LANGUAGE_SET_ALL. part_label names the part in the
# error for an expression that names none.
function(sxml_part_language_names expression part_label output_variable)
    set(languages "")
    if(expression MATCHES "SXML_LANGUAGE_SET_ALL")
        set(languages ${SXML_PART_LANGUAGES})
    else()
        foreach(language IN LISTS SXML_PART_LANGUAGES)
            if(expression MATCHES "SXML_LANGUAGE_${language}([^A-Z_]|$)")
                list(APPEND languages ${language})
            endif()
        endforeach()
    endif()
    if(NOT languages)
        message(FATAL_ERROR "${part_label}: its languages name no language")
    endif()
    set(${output_variable} ${languages} PARENT_SCOPE)
endfunction()

# Sets output_variable in the caller's scope to the C expression of the
# languages: SXML_LANGUAGE_<language> joined by " | ".
function(sxml_part_language_bits languages output_variable)
    set(language_bits "")
    foreach(language IN LISTS languages)
        if(language_bits)
            string(APPEND language_bits " | ")
        endif()
        string(APPEND language_bits "SXML_LANGUAGE_${language}")
    endforeach()
    set(${output_variable} "${language_bits}" PARENT_SCOPE)
endfunction()

# Writes the library part registry to output_path and the per-language
# lists beside it (output_path's stem, "_", the language in lower case).
function(sxml_generate_library_part_registry source_directory output_path outputs_variable)
    file(GLOB part_files CONFIGURE_DEPENDS "${source_directory}/function_library_*.c")
    list(SORT part_files)
    set(declarations "")
    set(table_rows "")
    foreach(language IN LISTS SXML_PART_LANGUAGES)
        set(language_declarations_${language} "")
        set(language_rows_${language} "")
        set(language_count_${language} 0)
    endforeach()
    set(part_count 0)
    foreach(part_file IN LISTS part_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${part_file}")
        file(READ "${part_file}" part_text)
        string(REGEX MATCH
            "const sxml_native_function_view[ \t\r\n]+sxml_function_library_([a-z0-9_]+)[ \t\r\n]*="
            definition "${part_text}")
        if(NOT definition)
            continue()
        endif()
        set(part_name "${CMAKE_MATCH_1}")
        set(declaration
            "extern const sxml_native_function_view sxml_function_library_${part_name};\n")
        string(APPEND declarations "${declaration}")
        set(declared_languages "NULL")
        string(REGEX MATCH
            "const sxml_language_set[ \t\r\n]+sxml_function_library_${part_name}_languages[ \t\r\n]*=([^;]*);"
            language_definition "${part_text}")
        if(language_definition)
            sxml_part_language_names("${CMAKE_MATCH_1}" "${part_file}" languages)
            string(APPEND declarations
                "extern const sxml_language_set sxml_function_library_${part_name}_languages;\n")
            set(declared_languages "&sxml_function_library_${part_name}_languages")
        else()
            set(languages ${SXML_PART_LANGUAGES})
        endif()
        sxml_part_language_bits("${languages}" language_bits)
        foreach(language IN LISTS languages)
            string(APPEND language_declarations_${language} "${declaration}")
            string(APPEND language_rows_${language}
                "    &sxml_function_library_${part_name},\n")
            math(EXPR language_count_${language} "${language_count_${language}} + 1")
        endforeach()
        string(APPEND table_rows
            "    {.part_name = SXML_BYTE_VIEW_INITIALIZER(\"${part_name}\"),\n"
            "     .native_functions = &sxml_function_library_${part_name},\n"
            "     .languages = (sxml_language_set)(${language_bits}),\n"
            "     .declared_languages = ${declared_languages}},\n")
        math(EXPR part_count "${part_count} + 1")
    endforeach()
    if(part_count EQUAL 0)
        message(FATAL_ERROR "no library part in ${source_directory}")
    endif()
    get_filename_component(output_directory "${output_path}" DIRECTORY)
    get_filename_component(output_stem "${output_path}" NAME_WE)
    set(outputs "${output_path}")
    set(language_cases "")
    foreach(language IN LISTS SXML_PART_LANGUAGES)
        string(TOLOWER "${language}" language_name)
        set(language_output "${output_directory}/${output_stem}_${language_name}.c")
        list(APPEND outputs "${language_output}")
        string(APPEND language_cases
            "    case SXML_LANGUAGE_${language}:\n"
            "        return *sxml_function_library_${language_name}_parts();\n")
        set(language_declarations "${language_declarations_${language}}")
        set(language_rows "${language_rows_${language}}")
        set(language_count "${language_count_${language}}")
        # A NULL entry ends the list, so it is never an empty array.
        file(CONFIGURE OUTPUT "${language_output}" @ONLY CONTENT
"/* Generated by cmake/SxmlParts.cmake from src/function_library_*.c
   (docs/DECISIONS.md, D101, D227): the library parts serving @language_name@.
   Do not edit. */
#include \"function_library_parts_internal.h\"

#include <stddef.h>

@language_declarations@
static const sxml_native_function_view *const sxml_@language_name@_library_parts[] = {
@language_rows@    NULL,
};

static const sxml_native_function_view_list sxml_@language_name@_library_list = {
    .native_function_views = sxml_@language_name@_library_parts,
    .native_function_view_count = @language_count@};

const sxml_native_function_view_list *sxml_function_library_@language_name@_parts(void) {
    return &sxml_@language_name@_library_list;
}
")
    endforeach()
    file(CONFIGURE OUTPUT "${output_path}" @ONLY CONTENT
"/* Generated by cmake/SxmlParts.cmake from src/function_library_*.c
   (docs/DECISIONS.md, D45, D101). Do not edit. */
#include \"function_library_parts_internal.h\"

#include <stddef.h>

@declarations@
static const sxml_function_library_part sxml_registered_library_parts[] = {
@table_rows@};

sxml_function_library_part_view sxml_function_library_registered_parts(void) {
    return (sxml_function_library_part_view){
        .parts = sxml_registered_library_parts,
        .part_count = @part_count@};
}

sxml_native_function_view_list sxml_function_library_parts_for_language(
    sxml_language language) {
    switch (language) {
@language_cases@    }
    return (sxml_native_function_view_list){.native_function_views = NULL,
                                            .native_function_view_count = 0};
}
")
    set(${outputs_variable} ${outputs} PARENT_SCOPE)
endfunction()

# Writes a registry of parts that are tables of one type to output_path, and
# the per-language lists beside it (D101, D103, D227). The arguments:
#   SOURCE_DIRECTORY, OUTPUT_PATH  where to look and what to write;
#   GLOBS                          the part files, relative to the directory;
#   PART_TYPE, PREFIX              each part is "const PART_TYPE PREFIX<part> =";
#   LANGUAGES_FROM                 FIELD: its .languages field names its
#                                  languages, and must; VARIABLE: an optional
#                                  "const sxml_language_set PREFIX<part>_languages
#                                  = ...;" does, all three when absent;
#   HEADER, ENTRY_TYPE, VIEW_TYPE  the internal header and the generated types;
#   REGISTRY_FUNCTION              the function giving every part;
#   LANGUAGE_FUNCTION              the per-language function's name, with
#                                  LANGUAGE for the language in lower case;
#   SOURCES                        what the comments name as the sources;
#   OUTPUTS                        the variable set to the written files.
function(sxml_generate_table_part_registry)
    cmake_parse_arguments(PARSE_ARGV 0 registry ""
        "SOURCE_DIRECTORY;OUTPUT_PATH;PART_TYPE;PREFIX;LANGUAGES_FROM;HEADER;ENTRY_TYPE;VIEW_TYPE;REGISTRY_FUNCTION;LANGUAGE_FUNCTION;SOURCES;DECISIONS;OUTPUTS"
        "GLOBS")
    set(globs "")
    foreach(glob IN LISTS registry_GLOBS)
        list(APPEND globs "${registry_SOURCE_DIRECTORY}/${glob}")
    endforeach()
    file(GLOB part_files CONFIGURE_DEPENDS ${globs})
    list(SORT part_files)
    set(declarations "")
    set(table_rows "")
    foreach(language IN LISTS SXML_PART_LANGUAGES)
        set(language_declarations_${language} "")
        set(language_rows_${language} "")
        set(language_count_${language} 0)
    endforeach()
    set(part_count 0)
    foreach(part_file IN LISTS part_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${part_file}")
        file(READ "${part_file}" part_text)
        string(REGEX MATCH
            "const ${registry_PART_TYPE}[ \t\r\n]+${registry_PREFIX}([a-z0-9_]+)[ \t\r\n]*="
            definition "${part_text}")
        if(NOT definition)
            continue()
        endif()
        set(part_name "${CMAKE_MATCH_1}")
        set(declaration "extern const ${registry_PART_TYPE} ${registry_PREFIX}${part_name};\n")
        string(APPEND declarations "${declaration}")
        if(registry_LANGUAGES_FROM STREQUAL "FIELD")
            string(REGEX MATCH
                "const ${registry_PART_TYPE}[ \t\r\n]+${registry_PREFIX}${part_name}[ \t\r\n]*=[ \t\r\n]*{[^}]*\\.languages[ \t\r\n]*=([^,}]*)"
                language_definition "${part_text}")
            if(NOT language_definition)
                message(FATAL_ERROR "${part_file}: part ${part_name} sets no .languages")
            endif()
            sxml_part_language_names("${CMAKE_MATCH_1}" "${part_file}" languages)
        else()
            string(REGEX MATCH
                "const sxml_language_set[ \t\r\n]+${registry_PREFIX}${part_name}_languages[ \t\r\n]*=([^;]*);"
                language_definition "${part_text}")
            if(language_definition)
                sxml_part_language_names("${CMAKE_MATCH_1}" "${part_file}" languages)
            else()
                set(languages ${SXML_PART_LANGUAGES})
            endif()
        endif()
        sxml_part_language_bits("${languages}" language_bits)
        string(CONCAT row
            "    {.part_name = SXML_BYTE_VIEW_INITIALIZER(\"${part_name}\"),\n"
            "     .part = &${registry_PREFIX}${part_name},\n"
            "     .languages = (sxml_language_set)(${language_bits})},\n")
        string(APPEND table_rows "${row}")
        foreach(language IN LISTS languages)
            string(APPEND language_declarations_${language} "${declaration}")
            string(APPEND language_rows_${language} "${row}")
            math(EXPR language_count_${language} "${language_count_${language}} + 1")
        endforeach()
        math(EXPR part_count "${part_count} + 1")
    endforeach()
    get_filename_component(output_directory "${registry_OUTPUT_PATH}" DIRECTORY)
    get_filename_component(output_stem "${registry_OUTPUT_PATH}" NAME_WE)
    set(outputs "${registry_OUTPUT_PATH}")
    set(header "${registry_HEADER}")
    set(entry_type "${registry_ENTRY_TYPE}")
    set(view_type "${registry_VIEW_TYPE}")
    set(sources "${registry_SOURCES}")
    set(decisions "${registry_DECISIONS}")
    foreach(language IN LISTS SXML_PART_LANGUAGES)
        string(TOLOWER "${language}" language_name)
        string(REPLACE "LANGUAGE" "${language_name}" language_function
            "${registry_LANGUAGE_FUNCTION}")
        set(language_output "${output_directory}/${output_stem}_${language_name}.c")
        list(APPEND outputs "${language_output}")
        set(language_declarations "${language_declarations_${language}}")
        set(language_rows "${language_rows_${language}}")
        set(language_count "${language_count_${language}}")
        # A zero entry ends the table, so it is never an empty array.
        file(CONFIGURE OUTPUT "${language_output}" @ONLY CONTENT
"/* Generated by cmake/SxmlParts.cmake from @sources@
   (docs/DECISIONS.md, @decisions@, D227): the parts serving @language_name@.
   Do not edit. */
#include \"@header@\"

#include <stddef.h>

@language_declarations@
static const @entry_type@ sxml_@language_name@_parts[] = {
@language_rows@    {.part_name = {.bytes = NULL, .byte_count = 0}, .part = NULL, .languages = 0},
};

static const @view_type@ sxml_@language_name@_part_view = {
    .parts = sxml_@language_name@_parts,
    .part_count = @language_count@};

const @view_type@ *@language_function@(void) {
    return &sxml_@language_name@_part_view;
}
")
    endforeach()
    set(registry_function "${registry_REGISTRY_FUNCTION}")
    file(CONFIGURE OUTPUT "${registry_OUTPUT_PATH}" @ONLY CONTENT
"/* Generated by cmake/SxmlParts.cmake from @sources@
   (docs/DECISIONS.md, @decisions@). Do not edit. */
#include \"@header@\"

#include <stddef.h>

@declarations@
static const @entry_type@ sxml_registered_parts[] = {
@table_rows@    {.part_name = {.bytes = NULL, .byte_count = 0}, .part = NULL, .languages = 0},
};

@view_type@ @registry_function@(void) {
    return (@view_type@){
        .parts = sxml_registered_parts,
        .part_count = @part_count@};
}
")
    set(${registry_OUTPUTS} ${outputs} PARENT_SCOPE)
endfunction()

# Writes the evaluator part registry to output_path (D101) and the
# per-language lists beside it (D227).
function(sxml_generate_evaluator_part_registry source_directory output_path outputs_variable)
    sxml_generate_table_part_registry(
        SOURCE_DIRECTORY "${source_directory}" OUTPUT_PATH "${output_path}"
        GLOBS "evaluator_*.c"
        PART_TYPE sxml_evaluator_part PREFIX sxml_evaluator_part_
        LANGUAGES_FROM VARIABLE
        HEADER evaluator_machine_internal.h
        ENTRY_TYPE sxml_evaluator_registered_part
        VIEW_TYPE sxml_evaluator_registered_part_view
        REGISTRY_FUNCTION sxml_evaluator_registered_parts
        LANGUAGE_FUNCTION sxml_evaluator_LANGUAGE_parts
        SOURCES "src/evaluator_*.c" DECISIONS D101
        OUTPUTS outputs)
    set(${outputs_variable} ${outputs} PARENT_SCOPE)
endfunction()

# Writes the lowering part registry to output_path (D103): every
# src/*_lower.c and src/*_lower_*.c that defines
#     const sxml_xpath_lowering_part sxml_xpath_lowering_part_<part> = ...
# and the per-language lists beside it (D227).
function(sxml_generate_lowering_part_registry source_directory output_path outputs_variable)
    sxml_generate_table_part_registry(
        SOURCE_DIRECTORY "${source_directory}" OUTPUT_PATH "${output_path}"
        GLOBS "*_lower.c" "*_lower_*.c"
        PART_TYPE sxml_xpath_lowering_part PREFIX sxml_xpath_lowering_part_
        LANGUAGES_FROM FIELD
        HEADER xpath_lower_internal.h
        ENTRY_TYPE sxml_xpath_lowering_registered_part
        VIEW_TYPE sxml_xpath_lowering_part_view
        REGISTRY_FUNCTION sxml_xpath_lowering_registered_parts
        LANGUAGE_FUNCTION sxml_xpath_lowering_LANGUAGE_parts
        SOURCES "the files src/<name>_lower[_<part>].c" DECISIONS D103
        OUTPUTS outputs)
    set(${outputs_variable} ${outputs} PARENT_SCOPE)
endfunction()
