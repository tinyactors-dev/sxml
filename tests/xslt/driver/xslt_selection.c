/*
 * The test selection of sxml-xslt-test (xslt_selection.h). Imitates the
 * selection of tests/qt4/driver/command_list.c.
 */
#include "xslt_selection.h"

#include "cli_arguments.h"
#include "test_list.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

/* The entries of a test list: a few kilobytes per thousand lines. */
enum { SXML_XSLT_TEST_LIST_ARENA_BLOCK_BYTE_COUNT = 16 * 1024 };

sxml_qt4_problem sxml_xslt_test_no_problem(void) {
    return (sxml_qt4_problem){.description = sxml_xslt_test_empty_text,
                              .subject = sxml_xslt_test_empty_text,
                              .location = {.source_name =
                                               sxml_xslt_test_empty_text,
                                           .line_number = 0,
                                           .column_number = 0,
                                           .byte_offset = 0}};
}

static void
sxml_xslt_test_write_problem(const sxml_xslt_test_selection *selection,
                             const sxml_qt4_problem *problem) {
    sxml_cli_writer *error = selection->invocation->standard_error;
    sxml_cli_writer_write(error, selection->invocation->program->name);
    sxml_cli_writer_write_byte(error, ' ');
    sxml_cli_writer_write(error, selection->command_name);
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    if (problem->location.source_name.byte_count != 0) {
        sxml_cli_writer_write_escaped(error, problem->location.source_name);
        if (problem->location.line_number != 0) {
            sxml_cli_writer_write_byte(error, ':');
            sxml_cli_writer_write_decimal(error, problem->location.line_number);
            if (problem->location.column_number != 0) {
                sxml_cli_writer_write_byte(error, ':');
                sxml_cli_writer_write_decimal(error,
                                              problem->location.column_number);
            }
        }
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, problem->description);
    if (problem->subject.byte_count != 0) {
        sxml_cli_writer_write_byte(error, ' ');
        sxml_cli_writer_write_quoted(error, problem->subject);
    }
    sxml_cli_writer_write_byte(error, '\n');
}

void sxml_xslt_test_selection_report_failure(sxml_xslt_test_selection *selection,
                                             sxml_status status,
                                             sxml_qt4_problem *problem) {
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        selection->has_unreadable_file = true;
    } else if (status == SXML_STATUS_MALFORMED_INPUT ||
               status == SXML_STATUS_INVALID_UTF8 ||
               status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        selection->has_problem = true;
    } else {
        selection->has_failed = true;
    }
    if (problem->description.byte_count == 0) {
        problem->description = sxml_status_name(status);
    }
    sxml_xslt_test_write_problem(selection, problem);
}

/* A name the selection asked for that the catalog does not have. */
typedef struct sxml_xslt_test_unknown_name {
    sxml_byte_view description;
    sxml_byte_view name;
    sxml_byte_view source_name;
    uint32_t line_number;
} sxml_xslt_test_unknown_name;

static void sxml_xslt_test_report_unknown(sxml_xslt_test_selection *selection,
                                          sxml_xslt_test_unknown_name unknown) {
    selection->has_problem = true;
    sxml_xslt_test_write_problem(
        selection,
        &(sxml_qt4_problem){.description = unknown.description,
                            .subject = unknown.name,
                            .location = {.source_name = unknown.source_name,
                                         .line_number = unknown.line_number,
                                         .column_number = 0,
                                         .byte_offset = 0}});
}

/* Opens test set test_set_index unless it is open already. Returns true
   when it is open. */
static bool sxml_xslt_test_open_test_set(sxml_xslt_test_selection *selection,
                                         size_t test_set_index) {
    size_t open_index = 0;
    if (sxml_xslt_test_catalog_get_open_test_set(selection->catalog,
                                                 &open_index) &&
        open_index == test_set_index) {
        return true;
    }
    sxml_qt4_problem problem = sxml_xslt_test_no_problem();
    const sxml_status status = sxml_xslt_test_catalog_open_test_set(
        selection->catalog,
        &(sxml_qt4_test_set_request){.test_set_index = test_set_index,
                                     .problem = &problem});
    if (status != SXML_STATUS_OK) {
        sxml_xslt_test_selection_report_failure(selection, status, &problem);
        return false;
    }
    return true;
}

static void sxml_xslt_test_walk_test_set(sxml_xslt_test_selection *selection,
                                         size_t test_set_index,
                                         sxml_xslt_test_visitor visitor) {
    if (!sxml_xslt_test_open_test_set(selection, test_set_index)) {
        return;
    }
    const size_t test_case_count =
        sxml_xslt_test_catalog_test_case_count(selection->catalog);
    for (size_t test_case_index = 0;
         test_case_index < test_case_count && !selection->has_failed;
         test_case_index += 1) {
        visitor.visit(visitor.state, selection, test_case_index);
    }
}

/* Walks the entries of a parsed test list. */
typedef struct sxml_xslt_test_list_walk {
    sxml_byte_view list_path;
    sxml_qt4_test_list test_list;
    sxml_xslt_test_visitor visitor;
} sxml_xslt_test_list_walk;

static void
sxml_xslt_test_walk_list_entries(sxml_xslt_test_selection *selection,
                                 const sxml_xslt_test_list_walk *walk) {
    for (size_t entry_index = 0;
         entry_index < walk->test_list.entry_count && !selection->has_failed;
         entry_index += 1) {
        const sxml_qt4_test_list_entry *entry =
            &walk->test_list.entries[entry_index];
        size_t test_set_index = 0;
        if (!sxml_xslt_test_catalog_find_test_set(
                selection->catalog, entry->test_set_name, &test_set_index)) {
            sxml_xslt_test_report_unknown(
                selection, (sxml_xslt_test_unknown_name){
                               .description = SXML_TEXT("unknown test set"),
                               .name = entry->test_set_name,
                               .source_name = walk->list_path,
                               .line_number = entry->line_number});
            continue;
        }
        if (!sxml_xslt_test_open_test_set(selection, test_set_index)) {
            continue;
        }
        size_t test_case_index = 0;
        if (!sxml_xslt_test_catalog_find_test_case(
                selection->catalog, entry->test_case_name, &test_case_index)) {
            sxml_xslt_test_report_unknown(
                selection, (sxml_xslt_test_unknown_name){
                               .description = SXML_TEXT("unknown test case"),
                               .name = entry->test_case_name,
                               .source_name = walk->list_path,
                               .line_number = entry->line_number});
            continue;
        }
        walk->visitor.visit(walk->visitor.state, selection, test_case_index);
    }
}

static void sxml_xslt_test_walk_test_list(sxml_xslt_test_selection *selection,
                                          sxml_byte_view list_path,
                                          sxml_xslt_test_visitor visitor) {
    const sxml_allocator allocator = selection->invocation->allocator;
    sxml_byte_view text = sxml_xslt_test_empty_text;
    sxml_status status = sxml_cli_file_reader_read(
        selection->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = list_path,
                                      .allocator = allocator},
        &text);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_problem problem = sxml_xslt_test_no_problem();
        problem.description = SXML_TEXT("cannot read");
        problem.subject = list_path;
        sxml_xslt_test_selection_report_failure(selection, status, &problem);
        return;
    }
    sxml_arena *arena = NULL;
    status = sxml_arena_create(
        allocator,
        &(sxml_arena_configuration){
            .block_allocator = allocator,
            .block_byte_count = SXML_XSLT_TEST_LIST_ARENA_BLOCK_BYTE_COUNT,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &arena);
    sxml_qt4_problem problem = sxml_xslt_test_no_problem();
    sxml_xslt_test_list_walk walk = {
        .list_path = list_path,
        .test_list = {.entries = NULL, .entry_count = 0},
        .visitor = visitor};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_test_list_parse(
            &(sxml_qt4_test_list_request){.text = text,
                                          .source_name = list_path,
                                          .arena = arena,
                                          .problem = &problem},
            &walk.test_list);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xslt_test_selection_report_failure(selection, status, &problem);
    }
    sxml_xslt_test_walk_list_entries(selection, &walk);
    sxml_arena_destroy(arena);
    sxml_allocator_release(allocator, (void *)text.bytes);
}

void sxml_xslt_test_selection_walk(
    sxml_xslt_test_selection *selection,
    const sxml_xslt_test_selection_request *request) {
    sxml_byte_view list_path = sxml_xslt_test_empty_text;
    if (sxml_cli_arguments_get_positional(selection->invocation->arguments, 0,
                                          &list_path) == SXML_STATUS_OK) {
        sxml_xslt_test_walk_test_list(selection, list_path, request->visitor);
        return;
    }
    if (request->has_test_set_name) {
        size_t test_set_index = 0;
        if (!sxml_xslt_test_catalog_find_test_set(
                selection->catalog, request->test_set_name, &test_set_index)) {
            sxml_xslt_test_report_unknown(
                selection,
                (sxml_xslt_test_unknown_name){
                    .description = SXML_TEXT("unknown test set"),
                    .name = request->test_set_name,
                    .source_name = sxml_xslt_test_empty_text,
                    .line_number = 0});
            return;
        }
        sxml_xslt_test_walk_test_set(selection, test_set_index,
                                     request->visitor);
        return;
    }
    const size_t test_set_count =
        sxml_xslt_test_catalog_test_set_count(selection->catalog);
    for (size_t test_set_index = 0;
         test_set_index < test_set_count && !selection->has_failed;
         test_set_index += 1) {
        sxml_xslt_test_walk_test_set(selection, test_set_index,
                                     request->visitor);
    }
}

sxml_cli_exit_code
sxml_xslt_test_selection_exit_code(const sxml_xslt_test_selection *selection) {
    if (selection->has_unreadable_file) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (selection->has_failed) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    return selection->has_problem ? SXML_CLI_EXIT_OPERATION_FAILED
                                  : SXML_CLI_EXIT_SUCCESS;
}
