#pragma once

#include "cli_command.h"
#include "sxml/processor.h"

/*
 * The --code-language option of sxml xpath, sxml xquery and sxml
 * transform (apps/sxml/README.md; glossary `language_library`; packet
 * 7.23, D288): each names a tree-sitter grammar's shared library, which
 * the platform's library loader loads; with the queries found beside it,
 * it becomes a registered language of the processor
 * (sxml_processor_configuration.source_code_languages). Public API only.
 *
 * The language's name comes from the file name: without its directory and
 * its extension (.so, .dylib, .dll), without "lib" and then "tree-sitter-"
 * in front, with '-' made '_' ("libtree-sitter-c-sharp.so" is c_sharp);
 * the library exports tree_sitter_NAME(). The queries (highlights.scm,
 * locals.scm, injections.scm) are read from the directory "queries"
 * beside the library (a grammar's repository after `tree-sitter build`),
 * or else from "../queries/NAME" (Neovim's layout); without a
 * highlights.scm in either, the language has no queries.
 */

#define SXML_CODE_LANGUAGE_OPTION                                                                  \
    {                                                                                              \
        .long_name = SXML_BYTE_VIEW_INITIALIZER("code-language"), .short_name = 0,                 \
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,                                                    \
        .value_name = SXML_BYTE_VIEW_INITIALIZER("LIBRARY"),                                       \
        .choices = {.texts = NULL, .text_count = 0},                                               \
        .default_value = {.bytes = NULL, .byte_count = 0},                                         \
        .multiplicity = SXML_CLI_OPTION_REPEATABLE,                                                \
        .summary = SXML_BYTE_VIEW_INITIALIZER(                                                     \
            "Load a tree-sitter language for tree-sitter:parse from a shared library"),           \
    }

/* The languages a command line loads, in allocator memory: the language
   rows and the query texts they refer to. */
typedef struct sxml_code_language_list {
    sxml_source_code_language *languages;
    size_t language_count;
} sxml_code_language_list;

/* Where the diagnostics go: the invocation and the "PROGRAM COMMAND: "
   prefix's command name. */
typedef struct sxml_code_language_context {
    const sxml_cli_invocation *invocation;
    sxml_byte_view command_name;
} sxml_code_language_context;

/*
 * Loads every --code-language of the invocation into *list (empty
 * without one). Returns SXML_CLI_EXIT_SUCCESS, or writes the diagnostic
 * and returns SXML_CLI_EXIT_USAGE_ERROR (a file name that names no
 * language, a name given twice), SXML_CLI_EXIT_INPUT_OUTPUT_ERROR (a
 * library that does not load or lacks its function, a platform without
 * shared libraries), SXML_CLI_EXIT_OPERATION_FAILED (a grammar of another
 * ABI, a query that does not fit the grammar) or
 * SXML_CLI_EXIT_INTERNAL_ERROR. Call sxml_code_languages_release in
 * every case.
 */
sxml_cli_exit_code sxml_code_languages_load(const sxml_code_language_context *context,
                                            sxml_code_language_list *list);

/* Releases the list's memory; the processor using it must be gone. */
void sxml_code_languages_release(sxml_allocator allocator, sxml_code_language_list *list);
