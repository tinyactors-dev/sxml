/*
 * What sxml xpath and sxml xquery share (query_command.h; docs/CLI.md).
 *
 * Moved from command_xpath.c, which it imitates. The library is used
 * through its public API only: one processor (include/sxml/processor.h)
 * whose resource resolver reads local files through the invocation's file
 * reader, so fn:doc sees local files and nothing else (docs/CLI.md, "No
 * hidden side effects").
 */
#include "query_command.h"

#include "cli_arguments.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/types.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* As sxml check: entity expansion is bounded, nesting is not. */
enum { SXML_QUERY_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

const sxml_byte_view sxml_query_methods[SXML_QUERY_METHOD_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("adaptive"), SXML_BYTE_VIEW_INITIALIZER("xml"),
    SXML_BYTE_VIEW_INITIALIZER("xhtml"),    SXML_BYTE_VIEW_INITIALIZER("html"),
    SXML_BYTE_VIEW_INITIALIZER("text"),     SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_serialization_method sxml_query_method_values[SXML_QUERY_METHOD_COUNT] = {
    SXML_SERIALIZATION_METHOD_ADAPTIVE, SXML_SERIALIZATION_METHOD_XML,
    SXML_SERIALIZATION_METHOD_XHTML,    SXML_SERIALIZATION_METHOD_HTML,
    SXML_SERIALIZATION_METHOD_TEXT,     SXML_SERIALIZATION_METHOD_JSON,
};

/* --------------------------------------------------------------- Files */

/* fn:doc reads local files: a URI reference without a scheme is a path
   relative to the working directory, a "file://" URI its path (on
   Windows, file:///C:/work its C:/work), and a "file:" URI without an
   authority (sxml transform's base URI for a relative stylesheet path
   when the working directory is unknown) its relative path. */
static sxml_byte_view sxml_query_local_path(const sxml_resource_request *request) {
    const sxml_byte_view file_scheme = SXML_TEXT("file://");
    const sxml_byte_view relative_scheme = SXML_TEXT("file:");
    sxml_byte_view uri = request->absolute_uri.byte_count > 0 ? request->absolute_uri
                                                              : request->uri_reference;
    if (uri.byte_count > file_scheme.byte_count &&
        memcmp(uri.bytes, file_scheme.bytes, file_scheme.byte_count) == 0) {
        sxml_byte_view path = {.bytes = uri.bytes + file_scheme.byte_count,
                               .byte_count = uri.byte_count - file_scheme.byte_count};
        if (path.byte_count >= 3 && path.bytes[0] == '/' && path.bytes[2] == ':' &&
            ((path.bytes[1] >= 'A' && path.bytes[1] <= 'Z') ||
             (path.bytes[1] >= 'a' && path.bytes[1] <= 'z'))) {
            /* /C:/work: a drive letter (RFC 8089, appendix E.2). */
            path.bytes += 1;
            path.byte_count -= 1;
        }
        return path;
    }
    if (uri.byte_count > relative_scheme.byte_count &&
        memcmp(uri.bytes, relative_scheme.bytes, relative_scheme.byte_count) == 0) {
        return (sxml_byte_view){.bytes = uri.bytes + relative_scheme.byte_count,
                                .byte_count = uri.byte_count - relative_scheme.byte_count};
    }
    for (size_t byte_index = 0; byte_index < uri.byte_count; byte_index += 1) {
        const uint8_t byte = uri.bytes[byte_index];
        if (byte == ':') {
            /* Another scheme: not a local file. */
            return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
        if (byte == '/' || byte == '?' || byte == '#') {
            break;
        }
    }
    return uri;
}

static sxml_status sxml_query_resolve_file(void *state, const sxml_resource_request *request,
                                           sxml_resource *resource) {
    const sxml_query_file_resolver *resolver = state;
    const sxml_byte_view path = sxml_query_local_path(request);
    /* "-" names standard input on the command line only. */
    if (path.byte_count == 0 || sxml_byte_view_equals(path, SXML_TEXT("-"))) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("Only local files are available");
        return SXML_STATUS_OK;
    }
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_cli_file_reader_read(
        resolver->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = path, .allocator = resolver->invocation->allocator},
        &contents);
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        resource->resource_form = SXML_RESOURCE_FORM_NONE;
        resource->description = SXML_TEXT("The file cannot be read");
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = contents;
    return SXML_STATUS_OK;
}

static void sxml_query_release_file(void *state, const sxml_resource *resource) {
    const sxml_query_file_resolver *resolver = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(resolver->invocation->allocator, (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations sxml_query_file_resolver_operations = {
    .resolve = sxml_query_resolve_file,
    .release = sxml_query_release_file,
};

static bool sxml_query_starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

sxml_status sxml_query_make_file_uri(const sxml_cli_invocation *invocation, sxml_byte_view path,
                                     sxml_byte_view *uri) {
    const sxml_byte_view directory = invocation->working_directory;
    const bool is_absolute = path.byte_count > 0 && path.bytes[0] == '/';
    const bool has_directory = !is_absolute && directory.byte_count > 0;
    sxml_byte_view parts[4] = {
        has_directory || is_absolute ? SXML_TEXT("file://") : SXML_TEXT("file:"),
        {.bytes = NULL, .byte_count = 0},
        {.bytes = NULL, .byte_count = 0},
        path};
    if (has_directory) {
        /* "C:/work" of Windows becomes file:///C:/work. */
        parts[1] = directory.bytes[0] == '/' ? directory : SXML_TEXT("/");
        parts[2] = directory.bytes[0] == '/' ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                             : directory;
    }
    size_t byte_count = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        byte_count += parts[part_index].byte_count;
    }
    const bool needs_separator =
        has_directory && directory.bytes[directory.byte_count - 1] != '/';
    byte_count += needs_separator ? 1 : 0;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        invocation->allocator, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t written_count = 0;
    for (size_t part_index = 0; part_index < 4; part_index += 1) {
        if (part_index == 3 && needs_separator) {
            bytes[written_count] = '/';
            written_count += 1;
        }
        if (parts[part_index].byte_count > 0) {
            memcpy(bytes + written_count, parts[part_index].bytes, parts[part_index].byte_count);
            written_count += parts[part_index].byte_count;
        }
    }
    *uri = (sxml_byte_view){.bytes = bytes, .byte_count = written_count};
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_query_file_of_uri(const sxml_cli_invocation *invocation, sxml_byte_view uri) {
    sxml_byte_view name = uri;
    const sxml_byte_view directory = invocation->working_directory;
    const sxml_byte_view absolute_scheme = SXML_TEXT("file://");
    const sxml_byte_view relative_scheme = SXML_TEXT("file:");
    if (sxml_query_starts_with(name, absolute_scheme)) {
        name.bytes += absolute_scheme.byte_count;
        name.byte_count -= absolute_scheme.byte_count;
        if (directory.byte_count > 0 && directory.bytes[0] != '/' &&
            sxml_query_starts_with(name, SXML_TEXT("/"))) {
            /* file:///C:/work: a Windows path. */
            name.bytes += 1;
            name.byte_count -= 1;
        }
        if (directory.byte_count > 0 && sxml_query_starts_with(name, directory) &&
            name.byte_count > directory.byte_count &&
            (directory.bytes[directory.byte_count - 1] == '/' ||
             name.bytes[directory.byte_count] == '/')) {
            const size_t skipped_count =
                directory.byte_count + (directory.bytes[directory.byte_count - 1] == '/' ? 0 : 1);
            name.bytes += skipped_count;
            name.byte_count -= skipped_count;
        }
    } else if (sxml_query_starts_with(name, relative_scheme)) {
        name.bytes += relative_scheme.byte_count;
        name.byte_count -= relative_scheme.byte_count;
    }
    return name;
}

/* -------------------------------------------------------------- Output */

static sxml_status sxml_query_write_output(void *state, sxml_byte_view bytes) {
    sxml_cli_writer *writer = state;
    sxml_cli_writer_write(writer, bytes);
    return writer->status == SXML_STATUS_OK ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations sxml_query_output_operations = {
    .write = sxml_query_write_output,
};

sxml_status sxml_query_run_write(const sxml_query_run *run, const sxml_query_output *output) {
    sxml_cli_writer *writer = run->invocation->standard_output;
    sxml_status status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(run->processor),
        &(sxml_serialization_request){.value = output->result,
                                      .serialization_parameters =
                                          output->serialization_parameters,
                                      .error_record = output->error_record},
        (sxml_output_stream){.state = writer, .operations = &sxml_query_output_operations});
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK &&
        sxml_sequence_count(output->result, NULL, &item_count) == SXML_STATUS_OK &&
        item_count > 0) {
        sxml_cli_writer_write_byte(writer, '\n');
        status =
            writer->status == SXML_STATUS_OK ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return status;
}

bool sxml_query_find_method(const sxml_cli_invocation *invocation,
                            sxml_serialization_method *method) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("method"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        match.value.byte_count == 0) {
        return false;
    }
    for (size_t method_index = 0; method_index < SXML_QUERY_METHOD_COUNT; method_index += 1) {
        if (sxml_byte_view_equals(match.value, sxml_query_methods[method_index])) {
            *method = sxml_query_method_values[method_index];
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- Errors */

sxml_cli_exit_code sxml_query_run_fail(const sxml_query_run *run,
                                       const sxml_query_failure *failure) {
    const sxml_status status = failure->status;
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    sxml_cli_writer *error = run->invocation->standard_error;
    const sxml_error_record *error_record = failure->error_record;
    const sxml_diagnostic *diagnostic = &error_record->diagnostic;
    /* A static error in an XQuery library module names that module (D148);
       other locations may borrow from a unit released by now. */
    const bool names_its_source = status == SXML_STATUS_STATIC_ERROR &&
                                  diagnostic->location.line_number != 0 &&
                                  diagnostic->location.source_name.byte_count > 0;
    sxml_cli_writer_write_escaped(error, names_its_source ? diagnostic->location.source_name
                                                          : failure->source_name);
    if (diagnostic->location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic->location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic->location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    if (sxml_status_has_error_record(status)) {
        const sxml_error_code *code = &error_record->code;
        if (code->namespace_prefix.byte_count > 0) {
            sxml_cli_writer_write_escaped(error, code->namespace_prefix);
            sxml_cli_writer_write_byte(error, ':');
        } else if (code->namespace_uri.byte_count > 0) {
            sxml_cli_writer_write(error, SXML_TEXT("Q{"));
            sxml_cli_writer_write_escaped(error, code->namespace_uri);
            sxml_cli_writer_write_byte(error, '}');
        }
        sxml_cli_writer_write_escaped(error, code->local_name);
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, diagnostic->description.byte_count > 0
                                             ? diagnostic->description
                                             : sxml_status_name(status));
    sxml_cli_writer_write_byte(error, '\n');
    if (sxml_status_has_error_record(status) || status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* ---------------------------------------------------------- Parameters */

/* True for NAME of --param=NAME=VALUE: an NCName, checked on ASCII (bytes
   from 0x80 are accepted as name characters; XML 1.0 [4], [4a]). */
static bool sxml_query_is_parameter_name(sxml_byte_view name) {
    if (name.byte_count == 0) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
        const uint8_t byte = name.bytes[byte_index];
        const bool is_start = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                              byte == '_' || byte >= 0x80;
        const bool is_other = (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
        if (!is_start && (byte_index == 0 || !is_other)) {
            return false;
        }
    }
    return true;
}

/* One --param split at its first "=". */
typedef struct sxml_query_parameter {
    sxml_byte_view name;
    sxml_byte_view value;
} sxml_query_parameter;

static bool sxml_query_split_parameter(sxml_byte_view text, sxml_query_parameter *parameter) {
    const uint8_t *equals = text.byte_count == 0 ? NULL : memchr(text.bytes, '=', text.byte_count);
    if (equals == NULL) {
        return false;
    }
    const size_t name_byte_count = (size_t)(equals - text.bytes);
    *parameter = (sxml_query_parameter){
        .name = {.bytes = text.bytes, .byte_count = name_byte_count},
        .value = {.bytes = equals + 1, .byte_count = text.byte_count - name_byte_count - 1}};
    return sxml_query_is_parameter_name(parameter->name);
}

/* Reads the --param options into run (declarations and bindings). Returns
   SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_query_read_parameters(sxml_query_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("param"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const size_t count = match.occurrence_count;
    if (count == 0) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    void *declaration_memory = NULL;
    void *binding_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        invocation->allocator,
        (sxml_allocation_request){.byte_count = count * sizeof(sxml_variable_declaration),
                                  .alignment = alignof(sxml_variable_declaration)},
        &declaration_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = count * sizeof(sxml_variable_binding),
                                      .alignment = alignof(sxml_variable_binding)},
            &binding_memory);
        if (status != SXML_STATUS_OK) {
            sxml_allocator_release(invocation->allocator, declaration_memory);
        }
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    run->declarations = declaration_memory;
    run->bindings = binding_memory;
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(run->processor);
    for (size_t parameter_index = 0; parameter_index < count; parameter_index += 1) {
        sxml_cli_option_match occurrence = {.is_present = false, .occurrence_count = 0};
        status = sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("param"),
                                     .occurrence_index = parameter_index},
            &occurrence);
        sxml_query_parameter parameter;
        if (status != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        if (!sxml_query_split_parameter(occurrence.value, &parameter)) {
            return sxml_cli_invocation_report_usage_error(
                invocation, SXML_TEXT("--param needs NAME=VALUE with NAME an NCName"));
        }
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            sxml_processor_get_name_pool(run->processor),
            &(sxml_name_request){.namespace_uri = {.index = 0}, .local_name = parameter.name},
            &name);
        sxml_item value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = {.utf8 = parameter.value,
                                                           .codepoint_count = 0,
                                                           .is_ascii = false}}}},
                &value);
        }
        if (status == SXML_STATUS_INVALID_ARGUMENT || status == SXML_STATUS_INVALID_UTF8) {
            return sxml_cli_invocation_report_usage_error(
                invocation, SXML_TEXT("--param VALUE must be text of XML characters"));
        }
        if (status != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        for (size_t earlier_index = 0; earlier_index < parameter_index; earlier_index += 1) {
            if (run->declarations[earlier_index].variable_name.index == name.index) {
                sxml_item_release(value);
                return sxml_cli_invocation_report_usage_error(
                    invocation, SXML_TEXT("--param names one variable twice"));
            }
        }
        run->declarations[parameter_index] = (sxml_variable_declaration){
            .variable_name = name,
            .declared_type = {.item_type =
                                  sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
                              .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}};
        run->bindings[parameter_index] = (sxml_variable_binding){
            .variable_name = name, .value = sxml_sequence_from_item(value)};
        run->parameter_count = parameter_index + 1;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* ------------------------------------------------------- Context item */

sxml_cli_exit_code sxml_query_run_parse_document(sxml_query_run *run, sxml_byte_view file,
                                                 sxml_item *document) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = invocation->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *error = invocation->standard_error;
        sxml_cli_writer_write(error, invocation->program->name);
        sxml_cli_writer_write_byte(error, ' ');
        sxml_cli_writer_write(error, run->command_name);
        sxml_cli_writer_write(error, SXML_TEXT(": cannot read "));
        sxml_cli_writer_write_quoted(error, file);
        sxml_cli_writer_write_byte(error, '\n');
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_diagnostic diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                                  .location = {.source_name = file,
                                               .line_number = 0,
                                               .column_number = 0,
                                               .byte_offset = 0}};
    status = sxml_processor_parse_document(
        run->processor,
        &(sxml_xml_parse_request){.input = contents,
                                  .source_name = file,
                                  .document_uri = {.bytes = NULL, .byte_count = 0},
                                  .base_uri = {.bytes = NULL, .byte_count = 0},
                                  .encoding_name = {.bytes = NULL, .byte_count = 0},
                                  .diagnostic = &diagnostic},
        document);
    sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_cli_writer *error = invocation->standard_error;
    sxml_cli_writer_write_escaped(error, file);
    if (diagnostic.location.line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic.location.line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, diagnostic.location.column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
    sxml_cli_writer_write_line(error, diagnostic.description.byte_count > 0
                                          ? diagnostic.description
                                          : sxml_status_name(status));
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* Parses the --context-item document. Returns SXML_CLI_EXIT_SUCCESS to go
   on. */
static sxml_cli_exit_code sxml_query_read_context_item(sxml_query_run *run) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    const sxml_status status = sxml_cli_arguments_find_option(
        run->invocation->arguments,
        (sxml_cli_option_lookup){.long_name = SXML_TEXT("context-item"), .occurrence_index = 0},
        &match);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        /* The command has no --context-item (sxml transform: INPUT). */
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (!match.is_present) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    const sxml_cli_exit_code exit_code =
        sxml_query_run_parse_document(run, match.value, &run->context_item);
    run->has_context_item = exit_code == SXML_CLI_EXIT_SUCCESS;
    return exit_code;
}

/* -------------------------------------------------------------- The run */

sxml_cli_exit_code sxml_query_run_start(sxml_query_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    run->resolver = (sxml_query_file_resolver){.invocation = invocation};
    const sxml_status status = sxml_processor_create(
        invocation->allocator,
        &(sxml_processor_configuration){
            .allocator = invocation->allocator,
            .limits = {.xml_limits = {.maximum_depth = 0,
                                      .maximum_entity_expansion_byte_count =
                                          SXML_QUERY_MAXIMUM_EXPANSION_BYTE_COUNT}},
            .resource_resolver = {.state = &run->resolver,
                                  .operations = &sxml_query_file_resolver_operations},
            .clock = sxml_system_clock()},
        &run->processor);
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (run->parameter_binding == SXML_PARAMETER_BINDING_NONE) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_cli_exit_code exit_code = sxml_query_read_parameters(run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_query_read_context_item(run);
    }
    return exit_code;
}

void sxml_query_run_finish(sxml_query_run *run) {
    const sxml_allocator allocator = run->invocation->allocator;
    for (size_t parameter_index = 0; parameter_index < run->parameter_count;
         parameter_index += 1) {
        sxml_sequence_release(run->bindings[parameter_index].value);
    }
    sxml_allocator_release(allocator, run->declarations);
    sxml_allocator_release(allocator, run->bindings);
    if (run->has_context_item) {
        sxml_item_release(run->context_item);
    }
    if (run->processor != NULL) {
        sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(run->processor));
        sxml_processor_destroy(run->processor);
    }
}

sxml_static_context_configuration sxml_query_run_static_context(const sxml_query_run *run) {
    const bool declares = run->parameter_binding == SXML_PARAMETER_BINDING_DECLARE;
    return (sxml_static_context_configuration){
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
        .variable_declarations = {.variable_declarations = declares ? run->declarations : NULL,
                                  .variable_declaration_count =
                                      declares ? run->parameter_count : 0}};
}

sxml_dynamic_context_configuration sxml_query_run_dynamic_context(const sxml_query_run *run) {
    return (sxml_dynamic_context_configuration){
        .has_context_value = run->has_context_item,
        .context_value = run->has_context_item ? sxml_sequence_from_item(run->context_item)
                                               : sxml_sequence_empty(),
        .variable_bindings = {.variable_bindings = run->bindings,
                              .variable_binding_count = run->parameter_count}};
}
