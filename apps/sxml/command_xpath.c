/*
 * sxml xpath [--context-item=FILE] [--param=NAME=VALUE]... [--method=M]
 * EXPRESSION: evaluates an XPath 4.0 expression and writes its result to
 * standard output (docs/CLI.md, "Commands today"; include/sxml/xpath.h).
 *
 * Imitates command_check.c. The library is used through its public API
 * only: one processor (include/sxml/processor.h) whose resource resolver
 * reads local files through the invocation's file reader, so fn:doc sees
 * local files and nothing else (docs/CLI.md, "No hidden side effects").
 *
 * - --context-item=FILE parses FILE (- for standard input) as the context
 *   item; without it the focus is absent (XPDY0002 when used).
 * - --param=NAME=VALUE, repeatable, declares the external variable $NAME
 *   and binds it to xs:untypedAtomic(VALUE).
 * - --method=adaptive (the only method so far) writes the result by the
 *   adaptive method of Serialization 4.0, one item per line.
 *
 * Errors: a static, type or dynamic error is reported on standard error as
 * "expression:LINE:COLUMN: err:CODE: description" (exit 1); a context
 * document that is not well-formed as "FILE:LINE:COLUMN: message" (exit
 * 1); an unreadable file exits 3; a limit or out of memory exits 4.
 */
#include "cli_arguments.h"
#include "program.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* As sxml check: entity expansion is bounded, nesting is not. */
enum { SXML_XPATH_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

static const sxml_byte_view sxml_xpath_methods[] = {
    SXML_BYTE_VIEW_INITIALIZER("adaptive"),
};

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
        .choices = {.texts = sxml_xpath_methods,
                    .text_count = sizeof sxml_xpath_methods / sizeof sxml_xpath_methods[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("adaptive"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Serialization method of the result"),
    },
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
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --context-item=book.xml '//title/string()'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xpath --param=n=5 'xs:integer($n) * 2'"),
};

/* --------------------------------------------------------------- Files */

/* fn:doc reads local files: a URI reference without a scheme is a path
   relative to the working directory, a "file:" URI its path. */
static sxml_byte_view sxml_xpath_local_path(const sxml_resource_request *request) {
    const sxml_byte_view file_scheme = SXML_TEXT("file://");
    sxml_byte_view uri = request->absolute_uri.byte_count > 0 ? request->absolute_uri
                                                              : request->uri_reference;
    if (uri.byte_count > file_scheme.byte_count &&
        memcmp(uri.bytes, file_scheme.bytes, file_scheme.byte_count) == 0) {
        return (sxml_byte_view){.bytes = uri.bytes + file_scheme.byte_count,
                                .byte_count = uri.byte_count - file_scheme.byte_count};
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

/* The resolver's state: the files it read, released after use. */
typedef struct sxml_xpath_file_resolver {
    const sxml_cli_invocation *invocation;
} sxml_xpath_file_resolver;

static sxml_status sxml_xpath_resolve_file(void *state, const sxml_resource_request *request,
                                           sxml_resource *resource) {
    const sxml_xpath_file_resolver *resolver = state;
    const sxml_byte_view path = sxml_xpath_local_path(request);
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

static void sxml_xpath_release_file(void *state, const sxml_resource *resource) {
    const sxml_xpath_file_resolver *resolver = state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES) {
        sxml_allocator_release(resolver->invocation->allocator, (void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations sxml_xpath_file_resolver_operations = {
    .resolve = sxml_xpath_resolve_file,
    .release = sxml_xpath_release_file,
};

/* -------------------------------------------------------------- Output */

static sxml_status sxml_xpath_write_output(void *state, sxml_byte_view bytes) {
    sxml_cli_writer *writer = state;
    sxml_cli_writer_write(writer, bytes);
    return writer->status == SXML_STATUS_OK ? SXML_STATUS_OK : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations sxml_xpath_output_operations = {
    .write = sxml_xpath_write_output,
};

/* ------------------------------------------------------------ The run */

/* The state of one run of the command. */
typedef struct sxml_xpath_run {
    const sxml_cli_invocation *invocation;
    sxml_processor *processor;
    /* One per --param, in allocator memory. */
    sxml_variable_declaration *declarations;
    sxml_variable_binding *bindings;
    size_t parameter_count;
    bool has_context_item;
    sxml_item context_item;
} sxml_xpath_run;

/* Reports an error of the expression: "expression:LINE:COLUMN: err:CODE:
   description". */
static void sxml_xpath_report_error(const sxml_xpath_run *run,
                                    const sxml_error_record *error_record,
                                    sxml_status status) {
    sxml_cli_writer *error = run->invocation->standard_error;
    const sxml_diagnostic *diagnostic = &error_record->diagnostic;
    sxml_cli_writer_write(error, SXML_TEXT("expression"));
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
}

/* The exit code for a failed compile or evaluation. */
static sxml_cli_exit_code sxml_xpath_exit_code_of(sxml_status status) {
    if (sxml_status_has_error_record(status) || status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* True for NAME of --param=NAME=VALUE: an NCName, checked on ASCII (bytes
   from 0x80 are accepted as name characters; XML 1.0 [4], [4a]). */
static bool sxml_xpath_is_parameter_name(sxml_byte_view name) {
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
typedef struct sxml_xpath_parameter {
    sxml_byte_view name;
    sxml_byte_view value;
} sxml_xpath_parameter;

static bool sxml_xpath_split_parameter(sxml_byte_view text, sxml_xpath_parameter *parameter) {
    const uint8_t *equals = text.byte_count == 0 ? NULL : memchr(text.bytes, '=', text.byte_count);
    if (equals == NULL) {
        return false;
    }
    const size_t name_byte_count = (size_t)(equals - text.bytes);
    *parameter = (sxml_xpath_parameter){
        .name = {.bytes = text.bytes, .byte_count = name_byte_count},
        .value = {.bytes = equals + 1, .byte_count = text.byte_count - name_byte_count - 1}};
    return sxml_xpath_is_parameter_name(parameter->name);
}

/* Reads the --param options into run (declarations and bindings). Returns
   SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_xpath_read_parameters(sxml_xpath_run *run) {
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
        sxml_xpath_parameter parameter;
        if (status != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        if (!sxml_xpath_split_parameter(occurrence.value, &parameter)) {
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

/* Parses the --context-item document. Returns SXML_CLI_EXIT_SUCCESS to go
   on. */
static sxml_cli_exit_code sxml_xpath_read_context_item(sxml_xpath_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(invocation->arguments,
                                       (sxml_cli_option_lookup){.long_name =
                                                                    SXML_TEXT("context-item"),
                                                                .occurrence_index = 0},
                                       &match) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (!match.is_present) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    const sxml_byte_view file = match.value;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = invocation->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *error = invocation->standard_error;
        sxml_cli_writer_write(error, invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" xpath: cannot read "));
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
        &run->context_item);
    sxml_allocator_release(invocation->allocator, (void *)contents.bytes);
    if (status == SXML_STATUS_OK) {
        run->has_context_item = true;
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

/* Compiles, evaluates and writes the expression. */
static sxml_cli_exit_code sxml_xpath_evaluate(sxml_xpath_run *run, sxml_byte_view text) {
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
            .static_context = {.namespace_bindings = {.namespace_bindings = NULL,
                                                      .namespace_binding_count = 0},
                               .static_base_uri = {.bytes = NULL, .byte_count = 0},
                               .variable_declarations = {.variable_declarations =
                                                             run->declarations,
                                                         .variable_declaration_count =
                                                             run->parameter_count}},
            .error_record = &error_record},
        &expression);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){
                .dynamic_context = {.has_context_value = run->has_context_item,
                                    .context_value = run->has_context_item
                                                         ? sxml_sequence_from_item(run->context_item)
                                                         : sxml_sequence_empty(),
                                    .variable_bindings = {.variable_bindings = run->bindings,
                                                          .variable_binding_count =
                                                              run->parameter_count}},
                .error_record = &error_record},
            &result);
    }
    sxml_cli_writer *output = invocation->standard_output;
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(run->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = {.serialization_method =
                                                 SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                             .has_item_separator = false,
                                             .item_separator = {.bytes = NULL, .byte_count = 0}},
                .error_record = &error_record},
            (sxml_output_stream){.state = output, .operations = &sxml_xpath_output_operations});
        uint64_t item_count = 0;
        if (status == SXML_STATUS_OK &&
            sxml_sequence_count(result, NULL, &item_count) == SXML_STATUS_OK && item_count > 0) {
            sxml_cli_writer_write_byte(output, '\n');
        }
        sxml_sequence_release(result);
    }
    sxml_xpath_expression_destroy(expression);
    if (status == SXML_STATUS_OK) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (status != SXML_STATUS_INPUT_OUTPUT_FAILED) {
        sxml_xpath_report_error(run, &error_record, status);
    }
    return sxml_xpath_exit_code_of(status);
}

static void sxml_xpath_release_run(sxml_xpath_run *run) {
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
    sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(run->processor));
    sxml_processor_destroy(run->processor);
}

static sxml_cli_exit_code sxml_run_xpath_command(const sxml_cli_invocation *invocation) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &text) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_xpath_file_resolver resolver = {.invocation = invocation};
    sxml_xpath_run run = {.invocation = invocation,
                          .processor = NULL,
                          .declarations = NULL,
                          .bindings = NULL,
                          .parameter_count = 0,
                          .has_context_item = false,
                          .context_item = {.representation = 0}};
    const sxml_status status = sxml_processor_create(
        invocation->allocator,
        &(sxml_processor_configuration){
            .allocator = invocation->allocator,
            .limits = {.xml_limits = {.maximum_depth = 0,
                                      .maximum_entity_expansion_byte_count =
                                          SXML_XPATH_MAXIMUM_EXPANSION_BYTE_COUNT}},
            .resource_resolver = {.state = &resolver,
                                  .operations = &sxml_xpath_file_resolver_operations},
            .clock = sxml_system_clock()},
        &run.processor);
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_cli_exit_code exit_code = sxml_xpath_read_parameters(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_xpath_read_context_item(&run);
    }
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_xpath_evaluate(&run, text);
    }
    sxml_xpath_release_run(&run);
    return exit_code;
}

const sxml_cli_command sxml_xpath_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("xpath"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Evaluate an XPath 4.0 expression"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Compiles EXPRESSION as XPath 4.0 and writes its result to standard\n"
        "output by the adaptive serialization method, one item per line.\n"
        "--context-item=FILE parses FILE as the context item; without it the\n"
        "focus is absent. --param=NAME=VALUE binds $NAME to\n"
        "xs:untypedAtomic(VALUE). fn:doc reads local files only. An error of\n"
        "the expression is reported on standard error as\n"
        "expression:LINE:COLUMN: err:CODE: description and exits 1; an\n"
        "unreadable file exits 3. An EXPRESSION that starts with - must\n"
        "follow --."),
    .options = sxml_xpath_options,
    .option_count = sizeof sxml_xpath_options / sizeof sxml_xpath_options[0],
    .positionals = sxml_xpath_positionals,
    .positional_count = sizeof sxml_xpath_positionals / sizeof sxml_xpath_positionals[0],
    .examples = {.texts = sxml_xpath_examples,
                 .text_count = sizeof sxml_xpath_examples / sizeof sxml_xpath_examples[0]},
    .run = sxml_run_xpath_command,
};
