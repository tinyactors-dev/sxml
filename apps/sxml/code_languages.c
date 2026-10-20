/*
 * --code-language (code_languages.h; packet 7.23, D288).
 *
 * Golden file: apps/sxml/query_command.c (reading a repeatable option and
 * its diagnostics).
 */
#include "code_languages.h"

#include "cli_arguments.h"
#include "cli_output.h"
#include "sxml/memory.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The grammar's entry point (tree-sitter: tree_sitter_NAME). */
typedef const struct TSLanguage *(*sxml_code_language_entry)(void);

/* The longest language name. */
#define SXML_CODE_LANGUAGE_NAME_CAPACITY ((size_t)128)

/* One --code-language value and what is known of it. */
typedef struct sxml_code_language_load {
    const sxml_code_language_context *context;
    sxml_byte_view library_path;
    /* The language name derived from the file name. */
    char name[SXML_CODE_LANGUAGE_NAME_CAPACITY];
    size_t name_byte_count;
} sxml_code_language_load;

static bool sxml_code_language_has_prefix(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0;
}

static bool sxml_code_language_has_suffix(sxml_byte_view text, sxml_byte_view suffix) {
    return text.byte_count >= suffix.byte_count &&
           memcmp(text.bytes + text.byte_count - suffix.byte_count, suffix.bytes,
                  suffix.byte_count) == 0;
}

/* The directory part of path, its final separator included; empty for a
   name without one. */
static sxml_byte_view sxml_code_language_directory(sxml_byte_view path) {
    size_t length = path.byte_count;
    while (length > 0 && path.bytes[length - 1] != '/' && path.bytes[length - 1] != '\\') {
        length -= 1;
    }
    return (sxml_byte_view){.bytes = path.bytes, .byte_count = length};
}

/* Derives the language name from the library's file name; false when it
   names none. */
static bool sxml_code_language_derive_name(sxml_code_language_load *load) {
    const sxml_byte_view directory = sxml_code_language_directory(load->library_path);
    sxml_byte_view name = {.bytes = load->library_path.bytes + directory.byte_count,
                           .byte_count = load->library_path.byte_count - directory.byte_count};
    static const sxml_byte_view extensions[] = {
        SXML_BYTE_VIEW_INITIALIZER(".so"), SXML_BYTE_VIEW_INITIALIZER(".dylib"),
        SXML_BYTE_VIEW_INITIALIZER(".dll")};
    bool has_extension = false;
    for (size_t extension_index = 0; extension_index < sizeof(extensions) / sizeof(extensions[0]);
         extension_index += 1) {
        if (!has_extension && sxml_code_language_has_suffix(name, extensions[extension_index])) {
            name.byte_count -= extensions[extension_index].byte_count;
            has_extension = true;
        }
    }
    if (!has_extension) {
        return false;
    }
    if (sxml_code_language_has_prefix(name, SXML_TEXT("lib"))) {
        name.bytes += 3;
        name.byte_count -= 3;
    }
    if (sxml_code_language_has_prefix(name, SXML_TEXT("tree-sitter-"))) {
        name.bytes += 12;
        name.byte_count -= 12;
    }
    if (name.byte_count == 0 || name.byte_count >= sizeof(load->name)) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        uint8_t byte = name.bytes[byte_index];
        if (byte == '-') {
            byte = '_';
        }
        const bool is_letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (!is_letter && !(byte >= '0' && byte <= '9') && byte != '_') {
            return false;
        }
        load->name[byte_index] = (char)byte;
    }
    load->name_byte_count = name.byte_count;
    return true;
}

static sxml_byte_view sxml_code_language_name(const sxml_code_language_load *load) {
    return (sxml_byte_view){.bytes = (const uint8_t *)load->name,
                            .byte_count = load->name_byte_count};
}

/* Writes "PROGRAM COMMAND: --code-language=PATH: " to standard error. */
static void sxml_code_language_write_prefix(const sxml_code_language_load *load) {
    const sxml_cli_invocation *invocation = load->context->invocation;
    sxml_cli_writer *error = invocation->standard_error;
    sxml_cli_writer_write(error, invocation->program->name);
    sxml_cli_writer_write_byte(error, ' ');
    sxml_cli_writer_write(error, load->context->command_name);
    sxml_cli_writer_write(error, SXML_TEXT(": --code-language="));
    sxml_cli_writer_write(error, load->library_path);
    sxml_cli_writer_write(error, SXML_TEXT(": "));
}

/* Up to three views joined into allocator memory. */
typedef struct sxml_code_language_parts {
    sxml_byte_view parts[4];
} sxml_code_language_parts;

static sxml_status sxml_code_language_join(sxml_allocator allocator,
                                           const sxml_code_language_parts *parts,
                                           sxml_byte_view *joined) {
    size_t byte_count = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        byte_count += parts->parts[part_index].byte_count;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = byte_count + 1, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t offset = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        const sxml_byte_view part = parts->parts[part_index];
        if (part.byte_count > 0) {
            memcpy(bytes + offset, part.bytes, part.byte_count);
            offset += part.byte_count;
        }
    }
    *joined = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

/* The query files of a language, by kind. */
enum {
    SXML_CODE_LANGUAGE_QUERY_HIGHLIGHTS,
    SXML_CODE_LANGUAGE_QUERY_LOCALS,
    SXML_CODE_LANGUAGE_QUERY_INJECTIONS,
    SXML_CODE_LANGUAGE_QUERY_COUNT,
};

static const sxml_byte_view sxml_code_language_query_files[SXML_CODE_LANGUAGE_QUERY_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("highlights.scm"), SXML_BYTE_VIEW_INITIALIZER("locals.scm"),
    SXML_BYTE_VIEW_INITIALIZER("injections.scm")};

/* The query texts read, and the directory they came from. */
typedef struct sxml_code_language_queries {
    sxml_byte_view texts[SXML_CODE_LANGUAGE_QUERY_COUNT];
    sxml_byte_view directory;
} sxml_code_language_queries;

/* Reads directory/FILE into *text; an unreadable file is no query. */
typedef struct sxml_code_language_query_read {
    const sxml_code_language_load *load;
    sxml_byte_view directory;
    size_t query_kind;
} sxml_code_language_query_read;

static sxml_status sxml_code_language_read_query(sxml_code_language_query_read read,
                                                 sxml_byte_view *text) {
    const sxml_cli_invocation *invocation = read.load->context->invocation;
    *text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_byte_view path = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_code_language_join(
        invocation->allocator,
        &(sxml_code_language_parts){.parts = {read.directory,
                                              sxml_code_language_query_files[read.query_kind],
                                              {.bytes = NULL, .byte_count = 0},
                                              {.bytes = NULL, .byte_count = 0}}},
        &path);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = path, .allocator = invocation->allocator}, text);
    sxml_allocator_release(invocation->allocator, (void *)path.bytes);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        *text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    return status;
}

static void sxml_code_language_release_queries(sxml_allocator allocator,
                                               sxml_code_language_queries *queries) {
    for (size_t query_kind = 0; query_kind < SXML_CODE_LANGUAGE_QUERY_COUNT; query_kind += 1) {
        sxml_allocator_release(allocator, (void *)queries->texts[query_kind].bytes);
        queries->texts[query_kind] = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    sxml_allocator_release(allocator, (void *)queries->directory.bytes);
    queries->directory = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* Reads the queries from the first directory that has highlights.scm:
   "queries/" beside the library, then "../queries/NAME/". */
static sxml_status sxml_code_language_read_queries(const sxml_code_language_load *load,
                                                   sxml_code_language_queries *queries) {
    const sxml_allocator allocator = load->context->invocation->allocator;
    const sxml_byte_view directory = sxml_code_language_directory(load->library_path);
    const sxml_code_language_parts candidates[2] = {
        {.parts = {directory, SXML_TEXT("queries/"), {.bytes = NULL, .byte_count = 0},
                   {.bytes = NULL, .byte_count = 0}}},
        {.parts = {directory, SXML_TEXT("../queries/"), sxml_code_language_name(load),
                   SXML_TEXT("/")}},
    };
    for (size_t candidate_index = 0; candidate_index < 2; candidate_index += 1) {
        sxml_status status =
            sxml_code_language_join(allocator, &candidates[candidate_index], &queries->directory);
        for (size_t query_kind = 0;
             status == SXML_STATUS_OK && query_kind < SXML_CODE_LANGUAGE_QUERY_COUNT;
             query_kind += 1) {
            status = sxml_code_language_read_query(
                (sxml_code_language_query_read){
                    .load = load, .directory = queries->directory, .query_kind = query_kind},
                &queries->texts[query_kind]);
            if (status == SXML_STATUS_OK && query_kind == SXML_CODE_LANGUAGE_QUERY_HIGHLIGHTS &&
                queries->texts[query_kind].bytes == NULL) {
                break;
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (queries->texts[SXML_CODE_LANGUAGE_QUERY_HIGHLIGHTS].bytes != NULL) {
            return SXML_STATUS_OK;
        }
        sxml_code_language_release_queries(allocator, queries);
    }
    return SXML_STATUS_OK;
}

/* Loads the library and calls its entry point. */
static sxml_cli_exit_code sxml_code_language_load_grammar(const sxml_code_language_load *load,
                                                          const struct TSLanguage **grammar) {
    const sxml_cli_invocation *invocation = load->context->invocation;
    sxml_cli_writer *error = invocation->standard_error;
    if (invocation->library_loader.operations == NULL) {
        sxml_code_language_write_prefix(load);
        sxml_cli_writer_write_line(error, SXML_TEXT("this platform loads no libraries"));
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    static const sxml_byte_view prefix = SXML_BYTE_VIEW_INITIALIZER("tree_sitter_");
    uint8_t function_name[SXML_CODE_LANGUAGE_NAME_CAPACITY + 16];
    for (size_t byte_index = 0; byte_index < prefix.byte_count; byte_index += 1) {
        function_name[byte_index] = prefix.bytes[byte_index];
    }
    for (size_t byte_index = 0; byte_index < load->name_byte_count; byte_index += 1) {
        function_name[prefix.byte_count + byte_index] = (uint8_t)load->name[byte_index];
    }
    sxml_cli_library_function_lookup lookup = {.function = NULL,
                                               .reason = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status = invocation->library_loader.operations->find_function(
        invocation->library_loader.state,
        &(sxml_cli_library_function_request){
            .library_path = load->library_path,
            .function_name = {.bytes = function_name,
                              .byte_count = prefix.byte_count + load->name_byte_count}},
        &lookup);
    if (status != SXML_STATUS_OK || lookup.function == NULL) {
        sxml_code_language_write_prefix(load);
        sxml_cli_writer_write(error, SXML_TEXT("cannot load the library"));
        if (lookup.reason.byte_count > 0) {
            sxml_cli_writer_write(error, SXML_TEXT(": "));
            sxml_cli_writer_write(error, lookup.reason);
        }
        sxml_cli_writer_write_byte(error, '\n');
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    /* The library's documentation (tree-sitter's) gives the type. */
    const sxml_code_language_entry entry = (sxml_code_language_entry)lookup.function;
    *grammar = entry();
    return SXML_CLI_EXIT_SUCCESS;
}

/* Writes what sxml_source_code_check_language found. */
typedef struct sxml_code_language_problem {
    const sxml_code_language_load *load;
    const sxml_code_language_queries *queries;
} sxml_code_language_problem;

static void sxml_code_language_report(sxml_code_language_problem problem,
                                      const sxml_source_code_language_report *report) {
    sxml_cli_writer *error = problem.load->context->invocation->standard_error;
    sxml_code_language_write_prefix(problem.load);
    if (report->problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_ABI) {
        sxml_cli_writer_write(error, SXML_TEXT("the grammar's ABI is "));
        sxml_cli_writer_write_decimal(error, report->abi_version);
        sxml_cli_writer_write(error, SXML_TEXT("; sxml reads 13 to 15"));
        sxml_cli_writer_write_byte(error, '\n');
        return;
    }
    size_t query_kind = SXML_CODE_LANGUAGE_QUERY_HIGHLIGHTS;
    sxml_byte_view kind = SXML_TEXT("highlights");
    if (report->problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_LOCALS_QUERY) {
        query_kind = SXML_CODE_LANGUAGE_QUERY_LOCALS;
        kind = SXML_TEXT("locals");
    } else if (report->problem == SXML_SOURCE_CODE_LANGUAGE_PROBLEM_INJECTIONS_QUERY) {
        query_kind = SXML_CODE_LANGUAGE_QUERY_INJECTIONS;
        kind = SXML_TEXT("injections");
    }
    sxml_cli_writer_write(error, SXML_TEXT("the "));
    sxml_cli_writer_write(error, kind);
    sxml_cli_writer_write(error, SXML_TEXT(" query does not fit the grammar at byte "));
    sxml_cli_writer_write_decimal(error, report->byte_offset);
    sxml_cli_writer_write(error, SXML_TEXT(" of "));
    sxml_cli_writer_write(error, problem.queries->directory);
    sxml_cli_writer_write(error, sxml_code_language_query_files[query_kind]);
    sxml_cli_writer_write_byte(error, '\n');
}

/* Loads one --code-language into language; the query texts and the name
   it refers to are allocator memory the list releases. */
static sxml_cli_exit_code sxml_code_language_load_one(sxml_code_language_load *load,
                                                      sxml_source_code_language *language) {
    const sxml_cli_invocation *invocation = load->context->invocation;
    const sxml_allocator allocator = invocation->allocator;
    if (!sxml_code_language_derive_name(load)) {
        sxml_code_language_write_prefix(load);
        sxml_cli_writer_write_line(
            invocation->standard_error,
            SXML_TEXT("the file name names no language (NAME.so, libtree-sitter-NAME.so, "
                      ".dylib or .dll)"));
        return SXML_CLI_EXIT_USAGE_ERROR;
    }
    const struct TSLanguage *grammar = NULL;
    sxml_cli_exit_code exit_code = sxml_code_language_load_grammar(load, &grammar);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    sxml_code_language_queries queries = {.texts = {{.bytes = NULL, .byte_count = 0}},
                                          .directory = {.bytes = NULL, .byte_count = 0}};
    sxml_byte_view name = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_code_language_read_queries(load, &queries);
    if (status == SXML_STATUS_OK) {
        status = sxml_code_language_join(
            allocator,
            &(sxml_code_language_parts){.parts = {sxml_code_language_name(load),
                                                  {.bytes = NULL, .byte_count = 0},
                                                  {.bytes = NULL, .byte_count = 0},
                                                  {.bytes = NULL, .byte_count = 0}}},
            &name);
    }
    if (status != SXML_STATUS_OK) {
        sxml_code_language_release_queries(allocator, &queries);
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    *language = (sxml_source_code_language){
        .language_name = name,
        .grammar = grammar,
        .highlights_query = queries.texts[SXML_CODE_LANGUAGE_QUERY_HIGHLIGHTS],
        .locals_query = queries.texts[SXML_CODE_LANGUAGE_QUERY_LOCALS],
        .injections_query = queries.texts[SXML_CODE_LANGUAGE_QUERY_INJECTIONS],
        .injection_regex = {.bytes = NULL, .byte_count = 0}};
    sxml_source_code_language_report report = {
        .problem = SXML_SOURCE_CODE_LANGUAGE_PROBLEM_NONE, .abi_version = 0, .byte_offset = 0};
    status = sxml_source_code_check_language(language, &report);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        sxml_code_language_report(
            (sxml_code_language_problem){.load = load, .queries = &queries}, &report);
        exit_code = SXML_CLI_EXIT_OPERATION_FAILED;
    } else if (status != SXML_STATUS_OK) {
        exit_code = SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    /* The texts stay with the language; the directory is not needed. */
    sxml_allocator_release(allocator, (void *)queries.directory.bytes);
    return exit_code;
}

sxml_cli_exit_code sxml_code_languages_load(const sxml_code_language_context *context,
                                            sxml_code_language_list *list) {
    const sxml_cli_invocation *invocation = context->invocation;
    *list = (sxml_code_language_list){.languages = NULL, .language_count = 0};
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("code-language"),
                                     .occurrence_index = 0},
            &match) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const size_t count = match.occurrence_count;
    if (count == 0) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    void *memory = NULL;
    if (sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = count * sizeof(sxml_source_code_language),
                                      .alignment = alignof(sxml_source_code_language)},
            &memory) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    list->languages = memory;
    for (size_t language_index = 0; language_index < count; language_index += 1) {
        sxml_cli_option_match occurrence = {.is_present = false, .occurrence_count = 0};
        if (sxml_cli_arguments_find_option(
                invocation->arguments,
                (sxml_cli_option_lookup){.long_name = SXML_TEXT("code-language"),
                                         .occurrence_index = language_index},
                &occurrence) != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        sxml_code_language_load load = {.context = context,
                                        .library_path = occurrence.value,
                                        .name = {0},
                                        .name_byte_count = 0};
        sxml_source_code_language *language = &list->languages[language_index];
        *language = (sxml_source_code_language){.language_name = {.bytes = NULL, .byte_count = 0},
                                                .grammar = NULL};
        list->language_count = language_index + 1;
        const sxml_cli_exit_code exit_code = sxml_code_language_load_one(&load, language);
        if (exit_code != SXML_CLI_EXIT_SUCCESS) {
            return exit_code;
        }
        for (size_t earlier_index = 0; earlier_index < language_index; earlier_index += 1) {
            if (sxml_byte_view_equals(list->languages[earlier_index].language_name,
                                      language->language_name)) {
                return sxml_cli_invocation_report_usage_error(
                    invocation, SXML_TEXT("--code-language names one language twice"));
            }
        }
    }
    return SXML_CLI_EXIT_SUCCESS;
}

void sxml_code_languages_release(sxml_allocator allocator, sxml_code_language_list *list) {
    for (size_t language_index = 0; language_index < list->language_count; language_index += 1) {
        const sxml_source_code_language *language = &list->languages[language_index];
        sxml_allocator_release(allocator, (void *)language->language_name.bytes);
        sxml_allocator_release(allocator, (void *)language->highlights_query.bytes);
        sxml_allocator_release(allocator, (void *)language->locals_query.bytes);
        sxml_allocator_release(allocator, (void *)language->injections_query.bytes);
    }
    sxml_allocator_release(allocator, list->languages);
    *list = (sxml_code_language_list){.languages = NULL, .language_count = 0};
}
