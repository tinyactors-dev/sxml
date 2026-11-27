/*
 * sxml decode [EXI OPTIONS] [--output=FILE] INPUT: reads INPUT as an EXI
 * stream and writes the document as XML text (apps/sxml/README.md, "sxml
 * decode"; include/sxml/exi.h; D333).
 *
 * Golden file: command_serialize.c, which it is with the document decoded
 * from EXI (query_command.h, run->reads_exi) instead of parsed.
 *
 * - The EXI options are those of the stream's header; a stream whose
 *   header carries none is read with the options the flags give
 *   (query_exi.c), as the encoder wrote it.
 * - The text is the XML output method's, with its default parameters,
 *   and a line feed.
 * - A stream that is not EXI, or not one EXI 1.0 allows, is reported as
 *   "INPUT: byte N: message" (exit 1).
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_decode_options[] = {
    SXML_QUERY_EXI_OPTIONS,
    SXML_QUERY_OUTPUT_OPTION,
    SXML_QUERY_FORCE_OPTION,
};

static const sxml_cli_positional sxml_decode_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("INPUT"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("EXI stream to decode; - reads standard input"),
    },
};

static const sxml_byte_view sxml_decode_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml decode orders.exi"),
    SXML_BYTE_VIEW_INITIALIZER(
        "sxml decode --alignment=compression --output=orders.xml orders.exi"),
};

/* Decodes INPUT and writes it. */
static sxml_cli_exit_code sxml_decode_file(sxml_query_run *run, sxml_byte_view file) {
    sxml_item document = {.representation = 0};
    const sxml_cli_exit_code exit_code = sxml_query_run_parse_document(run, file, &document);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
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

static sxml_cli_exit_code sxml_run_decode_command(const sxml_cli_invocation *invocation) {
    sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &file) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("decode");
    run.parameter_binding = SXML_PARAMETER_BINDING_NONE;
    run.reads_exi = true;
    sxml_cli_exit_code exit_code = sxml_query_run_start(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_decode_file(&run, file);
    }
    return sxml_query_run_finish(&run, exit_code);
}

const sxml_cli_command sxml_decode_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("decode"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Write an EXI stream as an XML document"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Reads INPUT as an EXI 1.0 stream (Efficient XML Interchange) and\n"
        "writes the document to standard output as XML text, by the xml\n"
        "method with its default parameters.\n"
        "\n"
        "A stream whose header carries its options is read with them. One\n"
        "that does not is read with the options its encoder was given, which\n"
        "the flags repeat: --alignment, --preserve, --fragment, --strict,\n"
        "--block-size, --value-max-length and --value-partition-capacity, as\n"
        "for sxml encode. --schema=FILE reads it with the schema-informed\n"
        "grammars of the XSD in FILE. Without --preserve=prefixes the decoder\n"
        "makes the prefixes up (ns0, ns1, ...).\n"
        "\n"
        "--output=FILE (-o FILE) writes the document to FILE instead, and only\n"
        "when the command succeeds; a FILE that exists is replaced only with\n"
        "--force.\n"
        "\n"
        "A stream that is not EXI, or not well-formed EXI, is reported on\n"
        "standard error as\n"
        "  INPUT: byte N: message\n"
        "\n"
        "Exit status: 0 on success, 1 when INPUT is not an EXI stream sxml can\n"
        "read, 2 for a bad option value, 3 when a file cannot be read or\n"
        "written."),
    .options = sxml_decode_options,
    .option_count = sizeof sxml_decode_options / sizeof sxml_decode_options[0],
    .positionals = sxml_decode_positionals,
    .positional_count = sizeof sxml_decode_positionals / sizeof sxml_decode_positionals[0],
    .examples = {.texts = sxml_decode_examples,
                 .text_count = sizeof sxml_decode_examples / sizeof sxml_decode_examples[0]},
    .run = sxml_run_decode_command,
};
