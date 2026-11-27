/*
 * The EXI options of sxml encode, sxml decode and --input-format=exi
 * (query_command.h, SXML_QUERY_EXI_OPTIONS; apps/sxml/README.md, "EXI
 * options"; include/sxml/exi.h; D333), and decoding an input document
 * from an EXI stream.
 *
 * Golden file: query_command.c (the options read with the CLI framework,
 * the document parsed with the processor, the diagnostic of an input that
 * is not well-formed).
 *
 * - --alignment, --preserve, --fragment, --strict, --block-size,
 *   --value-max-length and --value-partition-capacity are the options of
 *   EXI 1.0, section 5.4: the encoder writes the stream with them, and
 *   the decoder reads a stream whose header carries no options with them
 *   (known out of band).
 * - --schema=FILE is the XSD of the stream's schema-informed grammars,
 *   compiled once with the run's processor (include/sxml/schema.h).
 * - A value that is not one (an unknown fidelity name, a count that is
 *   not a whole number of 32 bits, a block size of 0) is a usage error.
 */
#include "cli_arguments.h"
#include "cli_help.h"
#include "query_command.h"
#include "sxml/schema.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

const sxml_byte_view sxml_query_exi_alignments[SXML_QUERY_EXI_ALIGNMENT_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("bit-packed"),
    SXML_BYTE_VIEW_INITIALIZER("byte-alignment"),
    SXML_BYTE_VIEW_INITIALIZER("pre-compression"),
    SXML_BYTE_VIEW_INITIALIZER("compression"),
};

static const sxml_exi_alignment sxml_query_exi_alignment_values[SXML_QUERY_EXI_ALIGNMENT_COUNT] = {
    SXML_EXI_ALIGNMENT_BIT_PACKED,
    SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT,
    SXML_EXI_ALIGNMENT_PRE_COMPRESSION,
    SXML_EXI_ALIGNMENT_COMPRESSION,
};

const sxml_byte_view sxml_query_input_formats[SXML_QUERY_INPUT_FORMAT_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("xml"),
    SXML_BYTE_VIEW_INITIALIZER("exi"),
};

/* The names --preserve lists, in the order of sxml_exi_fidelity. */
enum { SXML_QUERY_FIDELITY_COUNT = 5 };
static const sxml_byte_view sxml_query_fidelity_names[SXML_QUERY_FIDELITY_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("comments"), SXML_BYTE_VIEW_INITIALIZER("pis"),
    SXML_BYTE_VIEW_INITIALIZER("dtd"),      SXML_BYTE_VIEW_INITIALIZER("prefixes"),
    SXML_BYTE_VIEW_INITIALIZER("lexical-values"),
};

/* The value of an option the run reads. */
typedef struct sxml_query_exi_value {
    sxml_byte_view long_name;
    sxml_byte_view value;
} sxml_query_exi_value;

/* The value of the option long_name; false when it is not given. */
static bool sxml_query_exi_find(const sxml_query_run *run, sxml_query_exi_value *option) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            run->invocation->arguments,
            (sxml_cli_option_lookup){.long_name = option->long_name, .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        !match.is_present) {
        return false;
    }
    option->value = match.value;
    return true;
}

/* Reports a usage error: "sxml COMMAND: invalid value 'VALUE' for option
   '--NAME': EXPECTED", the usage line and the help hint. */
static sxml_cli_exit_code sxml_query_exi_usage_error(const sxml_query_run *run,
                                                     const sxml_query_exi_value *option,
                                                     sxml_byte_view expected) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_writer *error = invocation->standard_error;
    const sxml_cli_help_request request = {.program = invocation->program,
                                           .command = invocation->command};
    sxml_cli_write_diagnostic_prefix(error, request);
    sxml_cli_writer_write(error, SXML_TEXT("invalid value "));
    sxml_cli_writer_write_quoted(error, option->value);
    sxml_cli_writer_write(error, SXML_TEXT(" for option '--"));
    sxml_cli_writer_write(error, option->long_name);
    sxml_cli_writer_write(error, SXML_TEXT("': "));
    sxml_cli_writer_write_line(error, expected);
    sxml_cli_write_usage_line(error, request);
    sxml_cli_write_help_hint(error, request);
    return SXML_CLI_EXIT_USAGE_ERROR;
}

/* Reads a whole number from 0 to 2^32 - 1 written in decimal digits. */
static bool sxml_query_exi_read_count(sxml_byte_view text, uint32_t *count) {
    if (text.byte_count == 0) {
        return false;
    }
    uint64_t value = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t digit = text.bytes[byte_index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10 + (uint64_t)(digit - '0');
        if (value > UINT32_MAX) {
            return false;
        }
    }
    *count = (uint32_t)value;
    return true;
}

/* A count option of the EXI options: where it goes, and its least value. */
typedef struct sxml_query_exi_count {
    sxml_byte_view long_name;
    bool *is_given;
    uint32_t *count;
    uint32_t minimum;
} sxml_query_exi_count;

static sxml_cli_exit_code sxml_query_exi_read_count_option(const sxml_query_run *run,
                                                           const sxml_query_exi_count *count) {
    sxml_query_exi_value option = {.long_name = count->long_name,
                                   .value = {.bytes = NULL, .byte_count = 0}};
    if (!sxml_query_exi_find(run, &option)) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    uint32_t value = 0;
    if (!sxml_query_exi_read_count(option.value, &value) || value < count->minimum) {
        return sxml_query_exi_usage_error(
            run, &option,
            count->minimum > 0 ? SXML_TEXT("a whole number from 1 to 4294967295")
                               : SXML_TEXT("a whole number from 0 to 4294967295"));
    }
    *count->is_given = true;
    *count->count = value;
    return SXML_CLI_EXIT_SUCCESS;
}

/* Reads --preserve=LIST into options->fidelity. */
static sxml_cli_exit_code sxml_query_exi_read_preserve(const sxml_query_run *run,
                                                       sxml_exi_options *options) {
    sxml_query_exi_value option = {.long_name = SXML_TEXT("preserve"),
                                   .value = {.bytes = NULL, .byte_count = 0}};
    if (!sxml_query_exi_find(run, &option)) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    bool *const flags[SXML_QUERY_FIDELITY_COUNT] = {
        &options->fidelity.preserves_comments,
        &options->fidelity.preserves_processing_instructions,
        &options->fidelity.preserves_dtd,
        &options->fidelity.preserves_prefixes,
        &options->fidelity.preserves_lexical_values,
    };
    size_t start = 0;
    while (start <= option.value.byte_count && option.value.byte_count > 0) {
        size_t end = start;
        while (end < option.value.byte_count && option.value.bytes[end] != ',') {
            end += 1;
        }
        const sxml_byte_view name = {.bytes = option.value.bytes + start,
                                     .byte_count = end - start};
        bool is_known = false;
        for (size_t name_index = 0; name_index < SXML_QUERY_FIDELITY_COUNT; name_index += 1) {
            if (sxml_byte_view_equals(name, sxml_query_fidelity_names[name_index])) {
                *flags[name_index] = true;
                is_known = true;
            }
        }
        if (!is_known) {
            return sxml_query_exi_usage_error(
                run, &option,
                SXML_TEXT("a comma-separated list of comments, pis, dtd, prefixes and "
                          "lexical-values"));
        }
        start = end + 1;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* Whether the command has the option long_name. */
static bool sxml_query_exi_declares(const sxml_query_run *run, sxml_byte_view long_name) {
    const sxml_cli_command *command = run->invocation->command;
    for (size_t option_index = 0; option_index < command->option_count; option_index += 1) {
        if (sxml_byte_view_equals(command->options[option_index].long_name, long_name)) {
            return true;
        }
    }
    return false;
}

sxml_cli_exit_code sxml_query_run_read_exi_options(sxml_query_run *run) {
    if (!sxml_query_exi_declares(run, SXML_TEXT("alignment"))) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_exi_options *options = &run->exi_options;
    memset(options, 0, sizeof(*options));
    sxml_query_exi_value option = {.long_name = SXML_TEXT("input-format"),
                                   .value = {.bytes = NULL, .byte_count = 0}};
    if (sxml_query_exi_find(run, &option) &&
        sxml_byte_view_equals(option.value, SXML_TEXT("exi"))) {
        run->reads_exi = true;
    }
    option = (sxml_query_exi_value){.long_name = SXML_TEXT("alignment"),
                                    .value = {.bytes = NULL, .byte_count = 0}};
    if (sxml_query_exi_find(run, &option)) {
        for (size_t alignment_index = 0; alignment_index < SXML_QUERY_EXI_ALIGNMENT_COUNT;
             alignment_index += 1) {
            if (sxml_byte_view_equals(option.value, sxml_query_exi_alignments[alignment_index])) {
                options->alignment = sxml_query_exi_alignment_values[alignment_index];
            }
        }
    }
    options->is_fragment = sxml_query_has_option(run->invocation, SXML_TEXT("fragment"));
    options->is_strict = sxml_query_has_option(run->invocation, SXML_TEXT("strict"));
    sxml_cli_exit_code exit_code = sxml_query_exi_read_preserve(run, options);
    const sxml_query_exi_count counts[] = {
        {.long_name = SXML_TEXT("block-size"),
         .is_given = &options->has_block_size,
         .count = &options->block_size,
         .minimum = 1},
        {.long_name = SXML_TEXT("value-max-length"),
         .is_given = &options->has_value_max_length,
         .count = &options->value_max_length,
         .minimum = 0},
        {.long_name = SXML_TEXT("value-partition-capacity"),
         .is_given = &options->has_value_partition_capacity,
         .count = &options->value_partition_capacity,
         .minimum = 0},
    };
    for (size_t count_index = 0;
         count_index < sizeof counts / sizeof counts[0] && exit_code == SXML_CLI_EXIT_SUCCESS;
         count_index += 1) {
        exit_code = sxml_query_exi_read_count_option(run, &counts[count_index]);
    }
    return exit_code;
}

sxml_cli_exit_code sxml_query_run_compile_exi_schema(sxml_query_run *run) {
    sxml_query_exi_value option = {.long_name = SXML_TEXT("schema"),
                                   .value = {.bytes = NULL, .byte_count = 0}};
    if (!sxml_query_exi_declares(run, SXML_TEXT("alignment")) ||
        !sxml_query_exi_find(run, &option)) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_writer *error = invocation->standard_error;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = option.value, .allocator = invocation->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_query_run_write_prefix(run);
        sxml_cli_writer_write(error, SXML_TEXT("cannot read "));
        sxml_cli_writer_write_quoted(error, option.value);
        sxml_cli_writer_write_byte(error, '\n');
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    if (!sxml_byte_view_equals(option.value, SXML_TEXT("-")) &&
        sxml_query_make_file_uri(invocation, option.value, &uri) != SXML_STATUS_OK) {
        sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_schema_document document = {
        .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
        .document_text = contents,
        .document_node = {.representation = 0},
        .document_uri = uri,
        .checks_target_namespace = false,
        .target_namespace_uri = {.bytes = NULL, .byte_count = 0}};
    status = sxml_schema_create(
        invocation->allocator,
        &(sxml_schema_configuration){
            .processor = run->processor,
            .schema_documents = {.schema_documents = &document, .schema_document_count = 1},
            .base_uri = {.bytes = NULL, .byte_count = 0},
            .report_sink = {.state = NULL, .operations = NULL},
            .maximum_content_model_state_count = 0,
            .error_record = &error_record,
            .schema_location_hint_node = {.representation = 0}},
        &run->exi_schema);
    sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
    sxml_allocator_release(invocation->allocator, (void *)uri.bytes);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    run->exi_schema = NULL;
    sxml_cli_writer_write_escaped(error, option.value);
    if (error_record.diagnostic.location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, error_record.diagnostic.location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, error_record.diagnostic.location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    sxml_cli_writer_write_line(error, error_record.diagnostic.description.byte_count > 0
                                          ? error_record.diagnostic.description
                                          : sxml_status_name(status));
    return status == SXML_STATUS_STATIC_ERROR ? SXML_CLI_EXIT_OPERATION_FAILED
                                              : SXML_CLI_EXIT_INTERNAL_ERROR;
}

sxml_cli_exit_code sxml_query_run_decode_exi(sxml_query_run *run,
                                             const sxml_query_exi_input *input,
                                             sxml_item *document) {
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                                  .location = {.source_name = input->file,
                                               .line_number = 0,
                                               .column_number = 0,
                                               .byte_offset = 0}};
    const sxml_status status = sxml_processor_parse_exi_document(
        run->processor,
        &(sxml_exi_decode_request){
            .input = input->contents,
            .options = &run->exi_options,
            .schema = run->exi_schema,
            .schema_resolver = {.state = NULL, .operations = NULL},
            .document_uri = input->uri,
            .base_uri = input->uri,
            .diagnostic = &diagnostic},
        document);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_cli_writer_write_escaped(error, input->file);
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    if (status == SXML_STATUS_MALFORMED_INPUT ||
        status == SXML_STATUS_UNEXPECTED_END_OF_INPUT) {
        sxml_cli_writer_write(error, SXML_TEXT("byte "));
        sxml_cli_writer_write_decimal(error, (size_t)diagnostic.location.byte_offset);
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_line(error, diagnostic.description.byte_count > 0
                                          ? diagnostic.description
                                          : sxml_status_name(status));
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_UNEXPECTED_END_OF_INPUT ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE || status == SXML_STATUS_INVALID_UTF8) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}
