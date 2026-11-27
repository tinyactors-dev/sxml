/*
 * sxml encode [EXI OPTIONS] [--include-cookie] [--include-options]
 * [--output=FILE] INPUT: parses INPUT as XML and writes its EXI stream
 * (apps/sxml/README.md, "sxml encode"; include/sxml/exi.h; D333).
 *
 * Golden file: command_serialize.c (the processor, the parse of INPUT and
 * --output come from query_command.h; the EXI options from query_exi.c).
 *
 * - The stream goes to standard output, or to the --output file, as bytes
 *   with no line feed after them.
 * - --include-cookie writes "$EXI" first (EXI 1.0, 5.1); --include-options
 *   writes the options into the header (5.4), so that a decoder needs no
 *   flags to read it.
 * - Content the options cannot represent (strict grammars without a
 *   production for it, options EXI 1.0 does not allow) is reported as
 *   "sxml encode: INPUT: description" (exit 1).
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/exi.h"
#include "sxml/xdm.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_encode_options[] = {
    SXML_QUERY_EXI_OPTIONS,
    SXML_QUERY_FLAG_OPTION("include-cookie",
                           "Begin the stream with the EXI cookie $EXI"),
    SXML_QUERY_FLAG_OPTION("include-options",
                           "Write the EXI options into the stream's header"),
    SXML_QUERY_OUTPUT_OPTION,
    SXML_QUERY_FORCE_OPTION,
};

static const sxml_cli_positional sxml_encode_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("INPUT"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("XML document to encode; - reads standard input"),
    },
};

static const sxml_byte_view sxml_encode_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml encode --output=orders.exi orders.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml encode --alignment=compression --preserve=comments,prefixes "
                               "--output=orders.exi orders.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml encode --include-options orders.xml | sxml decode -"),
};

/* Reports a failure of the encoder: "sxml encode: INPUT: description". */
static void sxml_encode_report(const sxml_query_run *run, sxml_byte_view file,
                               sxml_byte_view description) {
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_query_run_write_prefix(run);
    sxml_cli_writer_write_escaped(error, file);
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    sxml_cli_writer_write_line(error, description);
}

/* Encodes document to the run's result writer. */
static sxml_status sxml_encode_document(sxml_query_run *run, sxml_item document,
                                        sxml_diagnostic *diagnostic) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_node_reference root = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_status status = sxml_item_get_node(document, &root);
    sxml_exi_encoder *encoder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_create(
            invocation->allocator,
            &(sxml_exi_encoder_configuration){.allocator = invocation->allocator}, &encoder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_encode(
            encoder,
            &(sxml_exi_encode_request){
                .root = root,
                .options = &run->exi_options,
                .schema = run->exi_schema,
                .header_options =
                    {.includes_cookie =
                         sxml_query_has_option(invocation, SXML_TEXT("include-cookie")),
                     .includes_options =
                         sxml_query_has_option(invocation, SXML_TEXT("include-options")),
                     .includes_schema_identifier = false},
                .canonicalization = {.is_canonical = false,
                                     .omits_options_document = false,
                                     .uses_utc_time = false},
                .self_contained_elements = {.element_names = NULL, .element_name_count = 0},
                .diagnostic = diagnostic},
            sxml_query_run_output_stream(run));
    }
    sxml_exi_encoder_destroy(encoder);
    return status;
}

/* Parses INPUT and writes its stream. */
static sxml_cli_exit_code sxml_encode_file(sxml_query_run *run, sxml_byte_view file) {
    sxml_item document = {.representation = 0};
    const sxml_cli_exit_code exit_code = sxml_query_run_parse_document(run, file, &document);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                                  .location = {.source_name = file,
                                               .line_number = 0,
                                               .column_number = 0,
                                               .byte_offset = 0}};
    const sxml_status status = sxml_encode_document(run, document, &diagnostic);
    sxml_item_release(document);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    sxml_encode_report(run, file,
                       diagnostic.description.byte_count > 0 ? diagnostic.description
                                                             : sxml_status_name(status));
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_INVALID_ARGUMENT ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

static sxml_cli_exit_code sxml_run_encode_command(const sxml_cli_invocation *invocation) {
    sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &file) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("encode");
    run.parameter_binding = SXML_PARAMETER_BINDING_NONE;
    sxml_cli_exit_code exit_code = sxml_query_run_start(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_encode_file(&run, file);
    }
    return sxml_query_run_finish(&run, exit_code);
}

const sxml_cli_command sxml_encode_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("encode"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Write an XML document as an EXI stream"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Parses INPUT as XML and writes it as an EXI 1.0 stream (Efficient XML\n"
        "Interchange) to standard output: bytes, with no line feed after them.\n"
        "\n"
        "The EXI options say what the stream keeps and how it is laid out:\n"
        "--alignment=bit-packed (the default), byte-alignment, pre-compression\n"
        "or compression (DEFLATE); --preserve=LIST, a comma-separated list of\n"
        "comments, pis, dtd, prefixes and lexical-values (none by default);\n"
        "--fragment for a stream of elements with no document around them;\n"
        "--strict; --block-size=N, --value-max-length=N and\n"
        "--value-partition-capacity=N. --schema=FILE encodes with the\n"
        "schema-informed grammars of the XSD in FILE.\n"
        "\n"
        "A decoder needs the options the stream was written with: give it the\n"
        "same flags, or write them into the stream's header with\n"
        "--include-options. --include-cookie begins the stream with \"$EXI\".\n"
        "\n"
        "--output=FILE (-o FILE) writes the stream to FILE instead, and only\n"
        "when the command succeeds; a FILE that exists is replaced only with\n"
        "--force.\n"
        "\n"
        "Exit status: 0 on success, 1 when INPUT is not well-formed or the\n"
        "options cannot represent it, 2 for a bad option value, 3 when a file\n"
        "cannot be read or written."),
    .options = sxml_encode_options,
    .option_count = sizeof sxml_encode_options / sizeof sxml_encode_options[0],
    .positionals = sxml_encode_positionals,
    .positional_count = sizeof sxml_encode_positionals / sizeof sxml_encode_positionals[0],
    .examples = {.texts = sxml_encode_examples,
                 .text_count = sizeof sxml_encode_examples / sizeof sxml_encode_examples[0]},
    .run = sxml_run_encode_command,
};
