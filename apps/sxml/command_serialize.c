/*
 * sxml serialize [--method=M] [--indent] FILE: parses FILE and writes the
 * document to standard output by a Serialization 4.0 method
 * (apps/sxml/README.md, "sxml serialize"; include/sxml/serialize.h).
 *
 * Golden file: command_xpath.c (the processor and the parse of FILE come
 * from query_command.h).
 *
 * - FILE (- for standard input) is parsed as namespace-well-formed XML
 *   1.0; one that is not is reported as "FILE:LINE:COLUMN: message"
 *   (exit 1).
 * - --method=M: xml (the default), xhtml, html, text, json or adaptive,
 *   with the method's default parameters.
 * - --indent sets the indent parameter to yes (Serialization 4.0, 5.1.5).
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_serialize_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("indent"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_NONE,
        .value_name = {.bytes = NULL, .byte_count = 0},
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Indent the output"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("method"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("METHOD"),
        .choices = {.texts = sxml_query_methods, .text_count = SXML_QUERY_METHOD_COUNT},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Serialization method"),
    },
};

static const sxml_cli_positional sxml_serialize_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("XML document to write; - reads standard input"),
    },
};

static const sxml_byte_view sxml_serialize_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml serialize --indent books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml serialize --method=json books.xml"),
};

/* Parses FILE and writes it. */
static sxml_cli_exit_code sxml_serialize_file(sxml_query_run *run, sxml_byte_view file) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_item document = {.representation = 0};
    const sxml_cli_exit_code exit_code = sxml_query_run_parse_document(run, file, &document);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    (void)sxml_query_find_method(invocation, &parameters.serialization_method);
    sxml_cli_option_match indent = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("indent"), .occurrence_index = 0},
            &indent) == SXML_STATUS_OK &&
        indent.is_present) {
        parameters.indent = SXML_SERIALIZATION_FLAG_YES;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_status status = sxml_query_run_write(
        run, &(sxml_query_output){.result = sxml_sequence_from_item(document),
                                  .serialization_parameters = parameters,
                                  .error_record = &error_record});
    sxml_item_release(document);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    return sxml_query_run_fail(
        run, &(sxml_query_failure){
                 .source_name = file, .error_record = &error_record, .status = status});
}

static sxml_cli_exit_code sxml_run_serialize_command(const sxml_cli_invocation *invocation) {
    sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &file) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("serialize");
    run.parameter_binding = SXML_PARAMETER_BINDING_NONE;
    sxml_cli_exit_code exit_code = sxml_query_run_start(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_serialize_file(&run, file);
    }
    sxml_query_run_finish(&run);
    return exit_code;
}

const sxml_cli_command sxml_serialize_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("serialize"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Parse an XML document and write it out again"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Parses FILE and writes the document to standard output with the\n"
        "Serialization 4.0 method that --method names (xml by default) and the\n"
        "method's default parameters. --indent indents the output.\n"
        "\n"
        "A FILE that is not well-formed is reported on standard error as\n"
        "  FILE:LINE:COLUMN: message\n"
        "\n"
        "Exit status: 0 on success, 1 when FILE is not well-formed or cannot be\n"
        "serialized, 3 when FILE cannot be read."),
    .options = sxml_serialize_options,
    .option_count = sizeof sxml_serialize_options / sizeof sxml_serialize_options[0],
    .positionals = sxml_serialize_positionals,
    .positional_count = sizeof sxml_serialize_positionals / sizeof sxml_serialize_positionals[0],
    .examples = {.texts = sxml_serialize_examples,
                 .text_count = sizeof sxml_serialize_examples / sizeof sxml_serialize_examples[0]},
    .run = sxml_run_serialize_command,
};
