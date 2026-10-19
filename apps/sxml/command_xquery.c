/*
 * sxml xquery [--context-item=FILE] [--param=NAME=VALUE]... [--method=M]
 * (QUERY-FILE | --expression=TEXT): evaluates an XQuery 4.0 main module
 * and writes its result to standard output (apps/sxml/README.md,
 * "sxml xquery"; include/sxml/xquery.h, D146).
 *
 * Golden file: command_xpath.c, with which it shares the processor, the
 * files fn:doc reads, --param and --context-item, error reports and
 * writing the result (query_command.h).
 *
 * - The query is the text of QUERY-FILE (- for standard input) or of
 *   --expression; both or neither is a usage error.
 * - --param=NAME=VALUE, repeatable, binds the external variable $NAME the
 *   query declares (XQuery 4.0, 5.16) to xs:untypedAtomic(VALUE).
 * - --context-item=FILE parses FILE as the initial context value.
 * - The result is serialized with the parameters the query's output
 *   declarations give (5.22; method xml by default); --method=M replaces
 *   the method.
 *
 * Errors as for sxml xpath, with the query file's name (or "expression")
 * in place of "expression": "QUERY-FILE:LINE:COLUMN: err:CODE:
 * description", exit 1; an unreadable file exits 3; a limit or out of
 * memory exits 4.
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/memory.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xquery.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_xquery_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("context-item"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "XML document to use as the context item; - reads standard input"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("expression"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("TEXT"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The query itself, instead of QUERY-FILE"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("param"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME=VALUE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_REPEATABLE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Bind external variable $NAME to xs:untypedAtomic(VALUE)"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("method"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("METHOD"),
        .choices = {.texts = sxml_query_methods, .text_count = SXML_QUERY_METHOD_COUNT},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Serialization method of the result, replacing the query's"),
    },
};

static const sxml_cli_positional sxml_xquery_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("QUERY-FILE"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File holding the XQuery 4.0 main module; - reads standard input"),
    },
};

static const sxml_byte_view sxml_xquery_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --context-item=books.xml titles.xq"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --expression='for $i in 1 to 3 return <i>{$i}</i>'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --param=n=5 --expression='declare variable $n external; $n * 2'"),
};

/* The query text: QUERY-FILE's contents (owned, released by the caller)
   or --expression's value (borrowed). */
typedef struct sxml_xquery_text {
    sxml_byte_view text;
    sxml_byte_view source_name;
    bool is_owned;
} sxml_xquery_text;

/* Reads the query text. Returns SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_xquery_read_text(const sxml_query_run *run,
                                                sxml_xquery_text *query_text) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_option_match expression = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("expression"), .occurrence_index = 0},
            &expression) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const bool has_file = sxml_cli_arguments_positional_count(invocation->arguments) > 0;
    if (has_file == expression.is_present) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give exactly one of QUERY-FILE and --expression"));
    }
    if (expression.is_present) {
        *query_text = (sxml_xquery_text){
            .text = expression.value, .source_name = SXML_TEXT("expression"), .is_owned = false};
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &file) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = invocation->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *error = invocation->standard_error;
        sxml_cli_writer_write(error, invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" xquery: cannot read "));
        sxml_cli_writer_write_quoted(error, file);
        sxml_cli_writer_write_byte(error, '\n');
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    *query_text = (sxml_xquery_text){.text = contents, .source_name = file, .is_owned = true};
    return SXML_CLI_EXIT_SUCCESS;
}

/* Compiles, evaluates and writes the query. */
static sxml_cli_exit_code sxml_xquery_evaluate(const sxml_query_run *run,
                                               const sxml_xquery_text *query_text) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        invocation->allocator,
        &(sxml_xquery_query_configuration){.processor = run->processor,
                                           .expression_text = query_text->text,
                                           .source_name = query_text->source_name,
                                           .static_context = sxml_query_run_static_context(run),
                                           .error_record = &error_record},
        &query);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query,
            &(sxml_xquery_evaluation_request){.dynamic_context =
                                                  sxml_query_run_dynamic_context(run),
                                              .error_record = &error_record},
            &result);
    }
    if (status == SXML_STATUS_OK) {
        /* The query's output declarations (XQuery 4.0, 5.22); --method
           replaces the method (D106). */
        sxml_serialization_parameters parameters =
            sxml_xquery_query_get_serialization_parameters(query);
        (void)sxml_query_find_method(invocation, &parameters.serialization_method);
        status = sxml_query_run_write(run, &(sxml_query_output){.result = result,
                                                                .serialization_parameters =
                                                                    parameters,
                                                                .error_record = &error_record});
        sxml_sequence_release(result);
    }
    sxml_xquery_query_destroy(query);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    return sxml_query_run_fail(run, &(sxml_query_failure){.source_name = query_text->source_name,
                                                          .error_record = &error_record,
                                                          .status = status});
}

static sxml_cli_exit_code sxml_run_xquery_command(const sxml_cli_invocation *invocation) {
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("xquery");
    run.parameter_binding = SXML_PARAMETER_BINDING_BIND;
    sxml_xquery_text query_text = {.text = {.bytes = NULL, .byte_count = 0},
                                   .source_name = {.bytes = NULL, .byte_count = 0},
                                   .is_owned = false};
    sxml_cli_exit_code exit_code = sxml_xquery_read_text(&run, &query_text);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_query_run_start(&run);
    }
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_xquery_evaluate(&run, &query_text);
    }
    sxml_query_run_finish(&run);
    if (query_text.is_owned) {
        sxml_allocator_release(invocation->allocator, (void *)query_text.text.bytes);
    }
    return exit_code;
}

const sxml_cli_command sxml_xquery_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("xquery"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Run an XQuery 4.0 query"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Runs the XQuery 4.0 main module in QUERY-FILE, or the query that\n"
        "--expression gives, and writes the result to standard output. The\n"
        "result is serialized as the query's output declarations say (method\n"
        "xml unless they name another); --method replaces the method.\n"
        "\n"
        "--context-item=FILE parses FILE and makes it the context item.\n"
        "--param=NAME=VALUE binds the external variable $NAME, if the query\n"
        "declares it, to xs:untypedAtomic(VALUE). Library modules (import\n"
        "module ... at) are read from local files relative to the working\n"
        "directory; fn:doc reads local files.\n"
        "\n"
        "An error is reported on standard error as\n"
        "  QUERY-FILE:LINE:COLUMN: err:CODE: description\n"
        "\n"
        "Exit status: 0 on success, 1 for an error in the query or a context\n"
        "document that is not well-formed, 3 when a file cannot be read."),
    .options = sxml_xquery_options,
    .option_count = sizeof sxml_xquery_options / sizeof sxml_xquery_options[0],
    .positionals = sxml_xquery_positionals,
    .positional_count = sizeof sxml_xquery_positionals / sizeof sxml_xquery_positionals[0],
    .examples = {.texts = sxml_xquery_examples,
                 .text_count = sizeof sxml_xquery_examples / sizeof sxml_xquery_examples[0]},
    .run = sxml_run_xquery_command,
};
