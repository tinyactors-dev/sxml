/*
 * sxml encode [EXI OPTIONS] [--include-cookie] [--include-options]
 * [--include-schema-id] [--canonical [--omit-options-document] [--utc-time]]
 * [--self-contained-element=NAME]...
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
    SXML_QUERY_FLAG_OPTION("include-schema-id",
                           "Write the schemaId into the header's options"),
    SXML_QUERY_FLAG_OPTION("canonical", "Write Canonical EXI"),
    SXML_QUERY_FLAG_OPTION("omit-options-document",
                           "Canonical EXI without the options document in the header"),
    SXML_QUERY_FLAG_OPTION("utc-time", "Canonical EXI with date-times in UTC"),
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("self-contained-element"), .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_REPEATABLE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "An element to write self-contained, NCName or Q{uri}local (with --self-contained)"),
    },
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

/* A name given on the command line: an NCName in no namespace or an EQName
   Q{uri}local (golden: command_transform.c, sxml_transform_read_name).
   False when it is neither. */
static bool sxml_encode_read_name(const sxml_query_run *run, sxml_byte_view text,
                                  sxml_name_identifier *name) {
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view local_name = text;
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        const uint8_t *close = memchr(text.bytes + 2, '}', text.byte_count - 2);
        if (close == NULL) {
            return false;
        }
        namespace_uri = (sxml_byte_view){.bytes = text.bytes + 2,
                                         .byte_count = (size_t)(close - text.bytes) - 2};
        local_name = (sxml_byte_view){.bytes = close + 1,
                                      .byte_count = text.byte_count -
                                                    (size_t)(close - text.bytes) - 1};
    }
    if (local_name.byte_count == 0 || memchr(local_name.bytes, ':', local_name.byte_count) != NULL ||
        memchr(local_name.bytes, '{', local_name.byte_count) != NULL) {
        return false;
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(run->processor);
    sxml_namespace_uri_identifier namespace_identifier = {.index = 0};
    return sxml_name_pool_intern_namespace_uri(name_pool, namespace_uri,
                                               &namespace_identifier) == SXML_STATUS_OK &&
           sxml_name_pool_intern_name(
               name_pool,
               &(sxml_name_request){.namespace_uri = namespace_identifier,
                                    .local_name = local_name},
               name) == SXML_STATUS_OK;
}

/* The --self-contained-element names, in allocator memory (release
   names->element_names). A name that is neither an NCName nor an EQName is
   a usage error. */
static sxml_cli_exit_code sxml_encode_read_self_contained(const sxml_query_run *run,
                                                          sxml_element_name_view *names) {
    const sxml_cli_invocation *invocation = run->invocation;
    *names = (sxml_element_name_view){.element_names = NULL, .element_name_count = 0};
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("self-contained-element"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        !match.is_present) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    void *memory = NULL;
    if (sxml_allocator_allocate(invocation->allocator,
                                (sxml_allocation_request){
                                    .byte_count = match.occurrence_count * sizeof(sxml_name_identifier),
                                    .alignment = _Alignof(sxml_name_identifier)},
                                &memory) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_name_identifier *identifiers = memory;
    for (size_t occurrence = 0; occurrence < match.occurrence_count; occurrence += 1) {
        sxml_cli_option_match value = {.is_present = false, .occurrence_count = 0};
        if (sxml_cli_arguments_find_option(invocation->arguments,
                                           (sxml_cli_option_lookup){.long_name = SXML_TEXT("self-contained-element"),
                                                                    .occurrence_index = occurrence},
                                           &value) != SXML_STATUS_OK ||
            !sxml_encode_read_name(run, value.value, &identifiers[occurrence])) {
            sxml_allocator_release(invocation->allocator, memory);
            return sxml_cli_invocation_report_usage_error(
                invocation, SXML_TEXT("--self-contained-element needs an NCName or Q{uri}local"));
        }
    }
    *names = (sxml_element_name_view){.element_names = identifiers, .element_name_count = match.occurrence_count};
    return SXML_CLI_EXIT_SUCCESS;
}

/* Encodes document to the run's result writer. */
typedef struct sxml_encode_input {
    sxml_item document;
    sxml_element_name_view self_contained_elements;
} sxml_encode_input;

static sxml_status sxml_encode_document(sxml_query_run *run, sxml_encode_input input,
                                        sxml_diagnostic *diagnostic) {
    const sxml_item document = input.document;
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
                     .includes_schema_identifier =
                         sxml_query_has_option(invocation, SXML_TEXT("include-schema-id"))},
                .canonicalization = {.is_canonical = sxml_query_has_option(invocation, SXML_TEXT("canonical")),
                                     .omits_options_document =
                                         sxml_query_has_option(invocation, SXML_TEXT("omit-options-document")),
                                     .uses_utc_time = sxml_query_has_option(invocation, SXML_TEXT("utc-time"))},
                .self_contained_elements = input.self_contained_elements,
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
    sxml_element_name_view self_contained = {.element_names = NULL, .element_name_count = 0};
    const sxml_cli_exit_code names_exit_code = sxml_encode_read_self_contained(run, &self_contained);
    if (names_exit_code != SXML_CLI_EXIT_SUCCESS) {
        sxml_item_release(document);
        return names_exit_code;
    }
    const sxml_status status = sxml_encode_document(
        run, (sxml_encode_input){.document = document, .self_contained_elements = self_contained}, &diagnostic);
    sxml_allocator_release(run->invocation->allocator, (void *)self_contained.element_names);
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
        "--include-options (and the schemaId with --include-schema-id).\n"
        "--include-cookie begins the stream with \"$EXI\".\n"
        "\n"
        "--canonical writes Canonical EXI, one stream for one document and one\n"
        "set of options; --omit-options-document leaves its options document\n"
        "out of the header, --utc-time writes its date-times in UTC.\n"
        "--self-contained-element=NAME (repeatable, with --self-contained)\n"
        "writes each element NAME so that it can be read on its own.\n"
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
