/*
 * sxml check [--format=text|json] FILE...: checks that every FILE is
 * namespace-well-formed XML 1.0 (docs/CLI.md, "Planned commands";
 * include/sxml/xml.h, sxml_xml_parser_check).
 *
 * Imitates command_version.c. Every FILE is checked, in order, even after a
 * failure. Text format: nothing on standard output; one
 * "FILE:LINE:COLUMN: message" line on standard error per failure. JSON
 * format: one object on standard output,
 *
 *     {"files":[{"file":"a.xml","status":"well-formed"},
 *               {"file":"b.xml","status":"not-well-formed","line":1,
 *                "column":5,"byte_offset":4,"message":"..."}]}
 *
 * with "status" one of "well-formed", "not-well-formed", "unreadable" and
 * "failed" (a limit or out of memory); fields are only ever added.
 *
 * Exit code: 3 when a file cannot be read, else 4 when sxml failed (a
 * limit, out of memory), else 1 when a file is not well-formed, else 0.
 */
#include "cli_arguments.h"
#include "program.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/xml.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Bounds for documents from the command line: deep nesting is fine (the
   parser walks iteratively), entity expansion is not unbounded. */
enum { SXML_CHECK_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

static const sxml_byte_view sxml_check_formats[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_check_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
        .choices = {.texts = sxml_check_formats,
                    .text_count = sizeof sxml_check_formats /
                                  sizeof sxml_check_formats[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Report format"),
    },
};

static const sxml_cli_positional sxml_check_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .multiplicity = SXML_CLI_POSITIONAL_ONE_OR_MORE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "XML document to check; - reads standard input"),
    },
};

static const sxml_byte_view sxml_check_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml check books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml check --format=json books.xml broken.xml"),
};

/* What happened to one file. */
typedef enum sxml_check_outcome {
    SXML_CHECK_OUTCOME_WELL_FORMED,
    SXML_CHECK_OUTCOME_NOT_WELL_FORMED,
    SXML_CHECK_OUTCOME_UNREADABLE,
    SXML_CHECK_OUTCOME_FAILED,
} sxml_check_outcome;

/* The state of one run of the command. */
typedef struct sxml_check_run {
    const sxml_cli_invocation *invocation;
    sxml_xml_parser *parser;
    bool writes_json;
    size_t reported_file_count;
    bool has_unreadable_file;
    bool has_failed_file;
    bool has_malformed_file;
} sxml_check_run;

/* One checked file and its result. */
typedef struct sxml_check_report {
    sxml_byte_view file;
    sxml_check_outcome outcome;
    const sxml_diagnostic *diagnostic;
} sxml_check_report;

static sxml_byte_view sxml_check_outcome_name(sxml_check_outcome outcome) {
    switch (outcome) {
    case SXML_CHECK_OUTCOME_WELL_FORMED:
        return SXML_TEXT("well-formed");
    case SXML_CHECK_OUTCOME_NOT_WELL_FORMED:
        return SXML_TEXT("not-well-formed");
    case SXML_CHECK_OUTCOME_UNREADABLE:
        return SXML_TEXT("unreadable");
    case SXML_CHECK_OUTCOME_FAILED:
        break;
    }
    return SXML_TEXT("failed");
}

static void sxml_check_write_json(sxml_check_run *run,
                                  const sxml_check_report *report) {
    sxml_cli_writer *output = run->invocation->standard_output;
    sxml_cli_writer_write(output, run->reported_file_count == 0
                                      ? SXML_TEXT("{\"files\":[")
                                      : SXML_TEXT(","));
    sxml_cli_writer_write(output, SXML_TEXT("{\"file\":"));
    sxml_cli_writer_write_json_string(output, report->file);
    sxml_cli_writer_write(output, SXML_TEXT(",\"status\":"));
    sxml_cli_writer_write_json_string(output,
                                      sxml_check_outcome_name(report->outcome));
    const sxml_diagnostic *diagnostic = report->diagnostic;
    if (diagnostic != NULL) {
        if (diagnostic->location.line_number != 0) {
            sxml_cli_writer_write(output, SXML_TEXT(",\"line\":"));
            sxml_cli_writer_write_decimal(output,
                                          diagnostic->location.line_number);
            sxml_cli_writer_write(output, SXML_TEXT(",\"column\":"));
            sxml_cli_writer_write_decimal(output,
                                          diagnostic->location.column_number);
            sxml_cli_writer_write(output, SXML_TEXT(",\"byte_offset\":"));
            sxml_cli_writer_write_decimal(output,
                                          diagnostic->location.byte_offset);
        }
        sxml_cli_writer_write(output, SXML_TEXT(",\"message\":"));
        sxml_cli_writer_write_json_string(output, diagnostic->description);
    }
    sxml_cli_writer_write_byte(output, '}');
}

static void sxml_check_write_text(const sxml_check_run *run,
                                  const sxml_check_report *report) {
    if (report->outcome == SXML_CHECK_OUTCOME_WELL_FORMED) {
        return;
    }
    sxml_cli_writer *error = run->invocation->standard_error;
    if (report->outcome == SXML_CHECK_OUTCOME_UNREADABLE) {
        sxml_cli_writer_write(error, run->invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" check: cannot read "));
        sxml_cli_writer_write_quoted(error, report->file);
        sxml_cli_writer_write_byte(error, '\n');
        return;
    }
    const sxml_diagnostic *diagnostic = report->diagnostic;
    sxml_cli_writer_write_escaped(error, report->file);
    if (diagnostic->location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic->location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error,
                                      diagnostic->location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    sxml_cli_writer_write_line(error, diagnostic->description);
}

static void sxml_check_report_file(sxml_check_run *run,
                                   const sxml_check_report *report) {
    switch (report->outcome) {
    case SXML_CHECK_OUTCOME_WELL_FORMED:
        break;
    case SXML_CHECK_OUTCOME_NOT_WELL_FORMED:
        run->has_malformed_file = true;
        break;
    case SXML_CHECK_OUTCOME_UNREADABLE:
        run->has_unreadable_file = true;
        break;
    case SXML_CHECK_OUTCOME_FAILED:
        run->has_failed_file = true;
        break;
    }
    if (run->writes_json) {
        sxml_check_write_json(run, report);
    } else {
        sxml_check_write_text(run, report);
    }
    run->reported_file_count += 1;
}

/* What a parser status means for the file. */
static sxml_check_outcome sxml_check_outcome_of(sxml_status status) {
    if (status == SXML_STATUS_OK) {
        return SXML_CHECK_OUTCOME_WELL_FORMED;
    }
    if (status == SXML_STATUS_MALFORMED_INPUT ||
        status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CHECK_OUTCOME_NOT_WELL_FORMED;
    }
    return SXML_CHECK_OUTCOME_FAILED;
}

/* Reads and checks one file, then reports it. */
static void sxml_check_file(sxml_check_run *run, sxml_byte_view file) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    const sxml_status read_status = sxml_cli_file_reader_read(
        run->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = allocator},
        &contents);
    if (read_status != SXML_STATUS_OK) {
        sxml_check_report_file(
            run, &(sxml_check_report){
                     .file = file,
                     .outcome = read_status == SXML_STATUS_INPUT_OUTPUT_FAILED
                                    ? SXML_CHECK_OUTCOME_UNREADABLE
                                    : SXML_CHECK_OUTCOME_FAILED,
                     .diagnostic = &(sxml_diagnostic){
                         .description = SXML_TEXT("the file cannot be read"),
                         .location = {.source_name = file,
                                      .line_number = 0,
                                      .column_number = 0,
                                      .byte_offset = 0}}});
        return;
    }
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = file,
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
    const sxml_status status = sxml_xml_parser_check(
        run->parser, &(sxml_xml_parse_request){
                         .input = contents,
                         .source_name = file,
                         .document_uri = {.bytes = NULL, .byte_count = 0},
                         .base_uri = {.bytes = NULL, .byte_count = 0},
                         .encoding_name = {.bytes = NULL, .byte_count = 0},
                         .diagnostic = &diagnostic});
    const sxml_check_outcome outcome = sxml_check_outcome_of(status);
    if (outcome == SXML_CHECK_OUTCOME_FAILED &&
        diagnostic.description.byte_count == 0) {
        diagnostic.description = sxml_status_name(status);
    }
    sxml_check_report_file(
        run, &(sxml_check_report){.file = file,
                                  .outcome = outcome,
                                  .diagnostic =
                                      outcome == SXML_CHECK_OUTCOME_WELL_FORMED
                                          ? NULL
                                          : &diagnostic});
    sxml_allocator_release(allocator, (void *)contents.bytes);
}

static sxml_cli_exit_code
sxml_run_check_command(const sxml_cli_invocation *invocation) {
    sxml_cli_option_match format = {
        .is_present = false,
        .occurrence_count = 0,
        .value = {.bytes = NULL, .byte_count = 0},
    };
    sxml_status status = sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = SXML_TEXT("format"),
                                 .occurrence_index = 0},
        &format);
    sxml_name_pool *name_pool = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_create(
            invocation->allocator,
            &(sxml_name_pool_configuration){.allocator = invocation->allocator,
                                            .maximum_byte_count = 0},
            &name_pool);
    }
    sxml_xml_parser *parser = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            invocation->allocator,
            &(sxml_xml_parser_configuration){
                .allocator = invocation->allocator,
                .name_pool = name_pool,
                .limits = {.maximum_depth = 0,
                           .maximum_entity_expansion_byte_count =
                               SXML_CHECK_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &parser);
    }
    if (status != SXML_STATUS_OK) {
        sxml_name_pool_destroy(name_pool);
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_check_run run = {
        .invocation = invocation,
        .parser = parser,
        .writes_json = sxml_byte_view_equals(format.value, SXML_TEXT("json")),
        .reported_file_count = 0,
        .has_unreadable_file = false,
        .has_failed_file = false,
        .has_malformed_file = false,
    };
    const size_t file_count =
        sxml_cli_arguments_positional_count(invocation->arguments);
    for (size_t file_index = 0; file_index < file_count; file_index++) {
        sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
        if (sxml_cli_arguments_get_positional(invocation->arguments, file_index,
                                              &file) == SXML_STATUS_OK) {
            sxml_check_file(&run, file);
        }
    }
    if (run.writes_json) {
        sxml_cli_writer_write_line(invocation->standard_output,
                                   SXML_TEXT("]}"));
    }
    sxml_xml_parser_destroy(parser);
    sxml_name_pool_destroy(name_pool);
    if (run.has_unreadable_file) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (run.has_failed_file) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    return run.has_malformed_file ? SXML_CLI_EXIT_OPERATION_FAILED
                                  : SXML_CLI_EXIT_SUCCESS;
}

const sxml_cli_command sxml_check_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("check"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Check that XML documents are well-formed"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Checks that each FILE is a well-formed XML 1.0 document that also\n"
        "conforms to Namespaces in XML 1.0. No tree is built. The supported\n"
        "encodings are UTF-8, UTF-16, ISO-8859-1 and US-ASCII. The external DTD\n"
        "subset and external entities are not read.\n"
        "\n"
        "Prints nothing when every FILE is well-formed. Each problem is\n"
        "reported on standard error as\n"
        "  FILE:LINE:COLUMN: message\n"
        "With --format=json, one JSON object on standard output lists every\n"
        "FILE with its \"status\".\n"
        "\n"
        "Exit status: 0 when every FILE is well-formed, 1 when one is not, 3\n"
        "when one cannot be read."),
    .options = sxml_check_options,
    .option_count = sizeof sxml_check_options / sizeof sxml_check_options[0],
    .positionals = sxml_check_positionals,
    .positional_count =
        sizeof sxml_check_positionals / sizeof sxml_check_positionals[0],
    .examples = {.texts = sxml_check_examples,
                 .text_count = sizeof sxml_check_examples /
                               sizeof sxml_check_examples[0]},
    .run = sxml_run_check_command,
};
