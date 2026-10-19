/*
 * sxml xpath [--context-item=FILE] [--param=NAME=VALUE]... [--method=M]
 * EXPRESSION: evaluates an XPath 4.0 expression and writes its result to
 * standard output (apps/sxml/README.md, "sxml xpath"; include/sxml/xpath.h).
 *
 * Imitates command_check.c. The processor, the files fn:doc reads, the
 * options --param and --context-item, error reports and writing the
 * result are shared with sxml xquery (query_command.h).
 *
 * - --context-item=FILE parses FILE (- for standard input) as the context
 *   item; without it the focus is absent (XPDY0002 when used).
 * - --param=NAME=VALUE, repeatable, declares the external variable $NAME
 *   and binds it to xs:untypedAtomic(VALUE).
 * - --method=M writes the result by the Serialization 4.0 method M:
 *   adaptive (the default; one item per line), xml, xhtml, html, text,
 *   json or csv, with that method's default parameters.
 * - --output=FILE (-o FILE) writes the result to FILE instead of standard
 *   output, when the expression succeeds; --force lets it replace a file
 *   (query_command.h).
 *
 * Errors: a static, type or dynamic error is reported on standard error as
 * "expression:LINE:COLUMN: err:CODE: description" (exit 1); a context
 * document that is not well-formed as "FILE:LINE:COLUMN: message" (exit
 * 1); an unreadable file exits 3; a limit or out of memory exits 4.
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_xpath_options[] = {
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
        .long_name = SXML_BYTE_VIEW_INITIALIZER("param"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME=VALUE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_REPEATABLE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Bind $NAME to xs:untypedAtomic(VALUE)"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("method"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("METHOD"),
        .choices = {.texts = sxml_query_methods, .text_count = SXML_QUERY_METHOD_COUNT},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("adaptive"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Serialization method of the result"),
    },
    SXML_QUERY_FILE_ACCESS_OPTION,
    SXML_QUERY_OUTPUT_OPTION,
    SXML_QUERY_FORCE_OPTION,
};

static const sxml_cli_positional sxml_xpath_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("EXPRESSION"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("XPath 4.0 expression to evaluate"),
    },
};

static const sxml_byte_view sxml_xpath_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath '1 to 3'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --context-item=books.xml '//book[@year < 1980]/title/string()'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --param=n=5 'xs:integer($n) * 2'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --method=json 'map { \"a\": [1, 2] }'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --context-item=books.xml --output=titles.txt '//title/string()'"),
};

/* Compiles, evaluates and writes the expression. */
static sxml_cli_exit_code sxml_xpath_evaluate(const sxml_query_run *run, sxml_byte_view text) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        invocation->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = run->processor,
            .expression_text = text,
            .source_name = SXML_TEXT("expression"),
            .static_context = sxml_query_run_static_context(run),
            .error_record = &error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){.dynamic_context =
                                                 sxml_query_run_dynamic_context(run),
                                             .error_record = &error_record},
            &result);
    }
    if (status == SXML_STATUS_OK) {
        sxml_serialization_parameters parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE;
        (void)sxml_query_find_method(invocation, &parameters.serialization_method);
        status = sxml_query_run_write(run, &(sxml_query_output){.result = result,
                                                                .serialization_parameters =
                                                                    parameters,
                                                                .error_record = &error_record});
        sxml_sequence_release(result);
    }
    sxml_xpath_expression_destroy(expression);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    return sxml_query_run_fail(run, &(sxml_query_failure){.source_name = SXML_TEXT("expression"),
                                                          .error_record = &error_record,
                                                          .status = status});
}

static sxml_cli_exit_code sxml_run_xpath_command(const sxml_cli_invocation *invocation) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &text) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("xpath");
    run.parameter_binding = SXML_PARAMETER_BINDING_DECLARE;
    sxml_cli_exit_code exit_code = sxml_query_run_start(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_xpath_evaluate(&run, text);
    }
    exit_code = sxml_query_run_finish(&run, exit_code);
    return exit_code;
}

const sxml_cli_command sxml_xpath_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("xpath"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Evaluate an XPath 4.0 expression"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Evaluates EXPRESSION as XPath 4.0 and writes the result to standard\n"
        "output, by the adaptive method with one item per line unless --method\n"
        "names another.\n"
        "\n"
        "--context-item=FILE parses FILE and makes it the context item; without\n"
        "it there is no context item. --param=NAME=VALUE binds $NAME to\n"
        "xs:untypedAtomic(VALUE). fn:doc reads local files. --allow-file-access\n"
        "lets the expression read and write files with the EXPath File\n"
        "functions (namespace http://expath.org/ns/file); without it they do\n"
        "not exist. An EXPRESSION that starts with - must follow --.\n"
        "\n"
        "--output=FILE (-o FILE) writes the result to FILE instead, and only\n"
        "when the command succeeds; a FILE that exists is replaced only with\n"
        "--force.\n"
        "\n"

        "An error is reported on standard error as\n"
        "  expression:LINE:COLUMN: err:CODE: description\n"
        "\n"
        "Exit status: 0 on success, 1 for an error in the expression or a\n"
        "context document that is not well-formed, 3 when a file cannot be\n"
        "read or written."),
    .options = sxml_xpath_options,
    .option_count = sizeof sxml_xpath_options / sizeof sxml_xpath_options[0],
    .positionals = sxml_xpath_positionals,
    .positional_count = sizeof sxml_xpath_positionals / sizeof sxml_xpath_positionals[0],
    .examples = {.texts = sxml_xpath_examples,
                 .text_count = sizeof sxml_xpath_examples / sizeof sxml_xpath_examples[0]},
    .run = sxml_run_xpath_command,
};
