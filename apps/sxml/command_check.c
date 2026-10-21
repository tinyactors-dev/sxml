/*
 * sxml check [--external-entities] [--validate-dtd] [--format=text|json]
 * FILE...: checks that every FILE is namespace-well-formed XML 1.0 or
 * 1.1 (XML 1.1, 4.3.4; D195), and
 * with --validate-dtd valid against its DTD (apps/sxml/README.md, "sxml
 * check"; include/sxml/xml.h, sxml_xml_parser_check; D44, D199).
 *
 * Imitates command_version.c; the processor and its local-file resolver
 * are query_command.h's, as in command_validate.c. Every FILE is checked,
 * in order, even after a failure. Text format: nothing on standard output;
 * one "FILE:LINE:COLUMN: message" line on standard error per failure
 * ("FILE:LINE:COLUMN: CODE: message" when it is not valid). JSON format:
 * one object on standard output,
 *
 *     {"files":[{"file":"a.xml","status":"well-formed"},
 *               {"file":"b.xml","status":"not-well-formed","line":1,
 *                "column":5,"byte_offset":4,"message":"..."}]}
 *
 * with "status" one of "well-formed", "not-well-formed", "unreadable" and
 * "failed" (a limit or out of memory); with --validate-dtd "valid" and
 * "invalid" (with "code") take the place of "well-formed". Fields are only
 * ever added.
 *
 * One intent per flag (D44): without --external-entities nothing outside
 * FILE is read; with it, the external DTD subset and external entities
 * are read as local files, relative to the entity that refers to them
 * (XML 1.0, 4.2.2), and nothing else. --validate-dtd validates against
 * the DTD that was read (XML 1.0, 5.1), and reports the first error of
 * each FILE; it reads no external entity by itself.
 *
 * Exit code: 3 when a file cannot be read, else 4 when sxml failed (a
 * limit, out of memory), else 1 when a file is not well-formed or not
 * valid, else 0.
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/schema.h"
#include "sxml/xml.h"

#include <string.h>

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
        .long_name = SXML_BYTE_VIEW_INITIALIZER("external-entities"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_NONE,
        .value_name = {.bytes = NULL, .byte_count = 0},
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Read the external DTD subset and external entities as local files"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("validate-dtd"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_NONE,
        .value_name = {.bytes = NULL, .byte_count = 0},
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Also check that each FILE is valid against its DTD"),
    },
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
    SXML_BYTE_VIEW_INITIALIZER("sxml check draft.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml check --format=json draft.xml broken.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml check --validate-dtd --external-entities article.xml"),
};

/* What happened to one file. */
typedef enum sxml_check_outcome {
    SXML_CHECK_OUTCOME_WELL_FORMED,
    SXML_CHECK_OUTCOME_NOT_WELL_FORMED,
    SXML_CHECK_OUTCOME_UNREADABLE,
    SXML_CHECK_OUTCOME_FAILED,
    /* --validate-dtd: in the place of WELL_FORMED. */
    SXML_CHECK_OUTCOME_VALID,
    SXML_CHECK_OUTCOME_INVALID,
} sxml_check_outcome;

/* The state of one run of the command. */
typedef struct sxml_check_run {
    const sxml_cli_invocation *invocation;
    sxml_xml_parser *parser;
    /* With --external-entities or --validate-dtd: the processor whose
       resolver reads local files, and whose name pool the parser uses. */
    sxml_query_run query;
    bool has_processor;
    bool reads_external_entities;
    bool validates;
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
    /* INVALID: the constraint broken (cvc-..., xml-validity). */
    sxml_byte_view code;
} sxml_check_report;

static sxml_byte_view sxml_check_outcome_name(sxml_check_outcome outcome) {
    switch (outcome) {
    case SXML_CHECK_OUTCOME_WELL_FORMED:
        return SXML_TEXT("well-formed");
    case SXML_CHECK_OUTCOME_NOT_WELL_FORMED:
        return SXML_TEXT("not-well-formed");
    case SXML_CHECK_OUTCOME_UNREADABLE:
        return SXML_TEXT("unreadable");
    case SXML_CHECK_OUTCOME_VALID:
        return SXML_TEXT("valid");
    case SXML_CHECK_OUTCOME_INVALID:
        return SXML_TEXT("invalid");
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
        if (report->code.byte_count > 0) {
            sxml_cli_writer_write(output, SXML_TEXT(",\"code\":"));
            sxml_cli_writer_write_json_string(output, report->code);
        }
        sxml_cli_writer_write(output, SXML_TEXT(",\"message\":"));
        sxml_cli_writer_write_json_string(output, diagnostic->description);
    }
    sxml_cli_writer_write_byte(output, '}');
}

static void sxml_check_write_text(const sxml_check_run *run,
                                  const sxml_check_report *report) {
    if (report->outcome == SXML_CHECK_OUTCOME_WELL_FORMED ||
        report->outcome == SXML_CHECK_OUTCOME_VALID) {
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
    if (report->code.byte_count > 0) {
        sxml_cli_writer_write_escaped(error, report->code);
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_line(error, diagnostic->description);
}

static void sxml_check_report_file(sxml_check_run *run,
                                   const sxml_check_report *report) {
    switch (report->outcome) {
    case SXML_CHECK_OUTCOME_WELL_FORMED:
    case SXML_CHECK_OUTCOME_VALID:
        break;
    case SXML_CHECK_OUTCOME_NOT_WELL_FORMED:
    case SXML_CHECK_OUTCOME_INVALID:
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

/* One FILE as read. */
typedef struct sxml_check_input {
    sxml_byte_view file;
    sxml_byte_view contents;
} sxml_check_input;

/* What the parse of a FILE, and its validation, gave. */
typedef struct sxml_check_parse_result {
    sxml_status status;
    sxml_diagnostic diagnostic;
    /* --validate-dtd: the first validity error (a null report sink stops
       the parse at it, include/sxml/schema.h), and the outcome. */
    sxml_error_record error_record;
    bool has_validity_outcome;
    sxml_validation_outcome validity_outcome;
} sxml_check_parse_result;

/* Parses input into result, with the entity resolver and the DTD
   validator the flags ask for. */
static void sxml_check_parse(sxml_check_run *run, sxml_check_input input,
                             sxml_check_parse_result *result) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_xml_parse_request request = {
        .input = input.contents,
        .source_name = input.file,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .encoding_name = {.bytes = NULL, .byte_count = 0},
        .diagnostic = &result->diagnostic,
        .entity_resolver = {.state = NULL, .operations = NULL},
        .validator = {.state = NULL, .operations = NULL}};
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    if (run->reads_external_entities) {
        /* The base URI of the external subset's and entities' system
           identifiers (XML 1.0, 4.2.2); standard input has none, so its
           are relative to the working directory. */
        if (!sxml_byte_view_equals(input.file, SXML_TEXT("-"))) {
            result->status = sxml_query_make_file_uri(run->invocation, input.file, &uri);
            if (result->status != SXML_STATUS_OK) {
                return;
            }
        }
        request.document_uri = uri;
        request.base_uri = uri;
        request.entity_resolver = sxml_processor_entity_resolver(run->query.processor);
    }
    sxml_dtd_validator *validator = NULL;
    if (run->validates) {
        result->status = sxml_dtd_validator_create(
            allocator,
            &(sxml_dtd_validator_configuration){
                .processor = run->query.processor,
                .report_sink = {.state = NULL, .operations = NULL},
                .error_record = &result->error_record},
            &validator);
        if (result->status == SXML_STATUS_OK) {
            request.validator = sxml_dtd_validator_as_xml_validator(validator);
        }
    }
    if (result->status == SXML_STATUS_OK) {
        result->status = sxml_xml_parser_check(run->parser, &request);
    }
    if (result->status == SXML_STATUS_OK && validator != NULL) {
        result->status = sxml_dtd_validator_get_outcome(validator, &result->validity_outcome);
        result->has_validity_outcome = result->status == SXML_STATUS_OK;
    }
    sxml_dtd_validator_destroy(validator);
    sxml_allocator_release(allocator, (void *)uri.bytes);
}

/* --validate-dtd: VALID or INVALID in the place of WELL_FORMED, and
   INVALID for the validator's first error (SXML_STATUS_DYNAMIC_ERROR with
   its error record), whose code and diagnostic are reported. */
static void sxml_check_judge_validity(sxml_check_parse_result *result,
                                      sxml_check_outcome *outcome, sxml_byte_view *code) {
    if (result->status == SXML_STATUS_DYNAMIC_ERROR) {
        *outcome = SXML_CHECK_OUTCOME_INVALID;
        *code = result->error_record.code.local_name;
        result->diagnostic.description = result->error_record.diagnostic.description;
        result->diagnostic.location = result->error_record.diagnostic.location;
        return;
    }
    if (*outcome != SXML_CHECK_OUTCOME_WELL_FORMED) {
        return;
    }
    if (result->has_validity_outcome &&
        result->validity_outcome.validity == SXML_VALIDITY_VALID) {
        *outcome = SXML_CHECK_OUTCOME_VALID;
        return;
    }
    *outcome = SXML_CHECK_OUTCOME_INVALID;
    result->diagnostic.location.line_number = 0;
    result->diagnostic.description = SXML_TEXT("the document is not valid");
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
    sxml_check_parse_result result;
    memset(&result, 0, sizeof(result));
    result.diagnostic.location.source_name = file;
    sxml_check_parse(run, (sxml_check_input){.file = file, .contents = contents}, &result);
    sxml_check_outcome outcome = sxml_check_outcome_of(result.status);
    sxml_byte_view code = {.bytes = NULL, .byte_count = 0};
    if (run->validates) {
        sxml_check_judge_validity(&result, &outcome, &code);
    }
    if (outcome == SXML_CHECK_OUTCOME_FAILED &&
        result.diagnostic.description.byte_count == 0) {
        result.diagnostic.description = sxml_status_name(result.status);
    }
    sxml_check_report_file(
        run, &(sxml_check_report){.file = file,
                                  .outcome = outcome,
                                  .diagnostic = outcome == SXML_CHECK_OUTCOME_WELL_FORMED ||
                                                        outcome == SXML_CHECK_OUTCOME_VALID
                                                    ? NULL
                                                    : &result.diagnostic,
                                  .code = code});
    sxml_allocator_release(allocator, (void *)contents.bytes);
}

/* Whether the command line has the flag long_name. */
static bool sxml_check_has_flag(const sxml_cli_invocation *invocation, sxml_byte_view long_name) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    return sxml_cli_arguments_find_option(
               invocation->arguments,
               (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
               &match) == SXML_STATUS_OK &&
           match.is_present;
}

/* Makes the parser, and the processor when a flag needs one. Returns
   SXML_CLI_EXIT_SUCCESS to go on; call sxml_check_run_finish in every
   case. */
static sxml_cli_exit_code sxml_check_run_start(sxml_check_run *run,
                                               sxml_name_pool **own_name_pool) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_name_pool *name_pool = NULL;
    if (run->reads_external_entities || run->validates) {
        run->query.invocation = invocation;
        run->query.command_name = SXML_TEXT("check");
        run->query.parameter_binding = SXML_PARAMETER_BINDING_NONE;
        run->has_processor = true;
        const sxml_cli_exit_code exit_code = sxml_query_run_start(&run->query);
        if (exit_code != SXML_CLI_EXIT_SUCCESS) {
            return exit_code;
        }
        name_pool = sxml_processor_get_name_pool(run->query.processor);
    } else if (sxml_name_pool_create(invocation->allocator,
                                     &(sxml_name_pool_configuration){
                                         .allocator = invocation->allocator,
                                         .maximum_byte_count = 0},
                                     own_name_pool) == SXML_STATUS_OK) {
        name_pool = *own_name_pool;
    } else {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_status status = sxml_xml_parser_create(
        invocation->allocator,
        &(sxml_xml_parser_configuration){
            .allocator = invocation->allocator,
            .name_pool = name_pool,
            .limits = {.maximum_depth = 0,
                       .maximum_entity_expansion_byte_count =
                           SXML_CHECK_MAXIMUM_EXPANSION_BYTE_COUNT}},
        &run->parser);
    return status == SXML_STATUS_OK ? SXML_CLI_EXIT_SUCCESS : SXML_CLI_EXIT_INTERNAL_ERROR;
}

static sxml_cli_exit_code
sxml_run_check_command(const sxml_cli_invocation *invocation) {
    sxml_cli_option_match format = {
        .is_present = false,
        .occurrence_count = 0,
        .value = {.bytes = NULL, .byte_count = 0},
    };
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("format"),
                                     .occurrence_index = 0},
            &format) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_check_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.reads_external_entities = sxml_check_has_flag(invocation, SXML_TEXT("external-entities"));
    run.validates = sxml_check_has_flag(invocation, SXML_TEXT("validate-dtd"));
    run.writes_json = sxml_byte_view_equals(format.value, SXML_TEXT("json"));
    sxml_name_pool *own_name_pool = NULL;
    const sxml_cli_exit_code start_code = sxml_check_run_start(&run, &own_name_pool);
    if (start_code == SXML_CLI_EXIT_SUCCESS) {
        const size_t file_count = sxml_cli_arguments_positional_count(invocation->arguments);
        for (size_t file_index = 0; file_index < file_count; file_index++) {
            sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
            if (sxml_cli_arguments_get_positional(invocation->arguments, file_index, &file) ==
                SXML_STATUS_OK) {
                sxml_check_file(&run, file);
            }
        }
        if (run.writes_json) {
            sxml_cli_writer_write_line(invocation->standard_output, SXML_TEXT("]}"));
        }
    }
    sxml_xml_parser_destroy(run.parser);
    sxml_name_pool_destroy(own_name_pool);
    if (run.has_processor) {
        (void)sxml_query_run_finish(&run.query, SXML_CLI_EXIT_SUCCESS);
    }
    if (start_code != SXML_CLI_EXIT_SUCCESS) {
        return start_code;
    }
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
        "Checks that each FILE is a well-formed XML 1.0 or XML 1.1 document\n"
        "(by the version its XML declaration names) that also conforms to\n"
        "Namespaces in XML. No tree is built. The supported encodings are\n"
        "UTF-8, UTF-16, ISO-8859-1 and US-ASCII. The external DTD subset and\n"
        "external entities are read only with --external-entities, and then\n"
        "only as local files, relative to the file that refers to them.\n"
        "\n"
        "With --validate-dtd, each FILE must also be valid against its DTD\n"
        "(XML 1.0, section 5.1); the first error of each FILE is reported. A\n"
        "FILE without a document type declaration is not valid. An external\n"
        "DTD subset is part of the DTD only with --external-entities.\n"
        "\n"
        "Prints nothing when every FILE is well-formed (valid). Each problem is\n"
        "reported on standard error as\n"
        "  FILE:LINE:COLUMN: message\n"
        "With --format=json, one JSON object on standard output lists every\n"
        "FILE with its \"status\".\n"
        "\n"
        "Exit status: 0 when every FILE is well-formed (valid), 1 when one is\n"
        "not, 3 when one cannot be read."),
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
