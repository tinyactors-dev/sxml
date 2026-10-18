/*
 * sxml transform [--param=NAME=VALUE]... [--initial-template=NAME]
 * [--initial-mode=NAME] [--method=M] STYLESHEET [INPUT]: runs an XSLT 4.0
 * transformation and writes its principal result to standard output
 * (docs/CLI.md, "Commands today"; include/sxml/xslt.h, D112, D157).
 *
 * Golden file: command_xquery.c, with which it shares the processor and
 * its local-file resolver, --param, --method, error reports and exit
 * codes (query_command.h).
 *
 * - STYLESHEET is the principal stylesheet module; the modules it includes
 *   and imports, and the documents fn:doc and document() read, are local
 *   files, relative to the working directory or to the stylesheet's
 *   directory as its relative hrefs say.
 * - INPUT (- for standard input) is parsed as the global context item:
 *   templates are applied to it in the initial mode (--initial-mode, else
 *   the default mode), or the named template --initial-template is called
 *   with it as the focus. Without INPUT the initial template is called
 *   (--initial-template, else xsl:initial-template) without a focus.
 * - --param=NAME=VALUE, repeatable, binds the stylesheet parameter or
 *   static parameter $NAME to xs:untypedAtomic(VALUE), which the
 *   parameter's "as" type converts.
 * - The principal result is serialized as the stylesheet's unnamed
 *   xsl:output says (XSLT 4.0, 26.1); --method=M replaces the method. A
 *   result document without href is the principal result; result
 *   documents with an href are not written (sxml writes files only where
 *   --output names them, which transform does not have yet).
 * - xsl:message and failed xsl:assert output goes to standard error, one
 *   message per line.
 *
 * Errors as for sxml xquery: "STYLESHEET:LINE:COLUMN: err:CODE:
 * description", exit 1; an unreadable file exits 3; a limit or out of
 * memory exits 4.
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_cli_option sxml_transform_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("initial-mode"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Mode whose template rules process INPUT, instead of the default mode"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("initial-template"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Named template to call, instead of applying templates to INPUT"),
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
            "Serialization method of the result, replacing the stylesheet's"),
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
            "Bind stylesheet parameter $NAME to xs:untypedAtomic(VALUE)"),
    },
};

static const sxml_cli_positional sxml_transform_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("STYLESHEET"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("File holding the principal stylesheet module"),
    },
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("INPUT"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "XML document to transform, the global context item; - reads standard input"),
    },
};

static const sxml_byte_view sxml_transform_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml transform list.xsl books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml transform --param='heading=Reading list' list.xsl books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml transform hello.xsl"),
    SXML_BYTE_VIEW_INITIALIZER("sxml transform --method=xml list.xsl - < books.xml"),
};

/* --------------------------------------------------------------- Output */

/* Standard output as an output stream; remembers whether the last byte
   written was a line feed. */
typedef struct sxml_transform_output {
    sxml_cli_writer *writer;
    bool has_written;
    bool ends_with_line_feed;
} sxml_transform_output;

static sxml_status sxml_transform_write(void *state, sxml_byte_view bytes) {
    sxml_transform_output *output = state;
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_cli_writer_write(output->writer, bytes);
    output->has_written = true;
    output->ends_with_line_feed = bytes.bytes[bytes.byte_count - 1] == '\n';
    return output->writer->status == SXML_STATUS_OK ? SXML_STATUS_OK
                                                    : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations sxml_transform_output_operations = {
    .write = sxml_transform_write,
};

/* ------------------------------------------------------------- Messages */

/* xsl:message output: each message as the text method writes it, on a
   line of its own on standard error. */
typedef struct sxml_transform_messages {
    sxml_cli_writer *writer;
} sxml_transform_messages;

static sxml_status sxml_transform_write_message(void *state, sxml_byte_view bytes) {
    sxml_transform_messages *messages = state;
    sxml_cli_writer_write_escaped(messages->writer, bytes);
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations sxml_transform_message_operations = {
    .write = sxml_transform_write_message,
};

static sxml_status sxml_transform_receive(void *state, const sxml_trace_event *event) {
    sxml_transform_messages *messages = state;
    if (event->trace_event_kind != SXML_TRACE_EVENT_KIND_XSLT_MESSAGE) {
        return SXML_STATUS_OK;
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_TEXT;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    /* A message that cannot be written as text is shown as far as it
       goes; the transformation goes on. */
    const sxml_status written = sxml_serialize_sequence(
        sxml_processor_get_item_heap(event->processor),
        &(sxml_serialization_request){.value = event->value,
                                      .serialization_parameters = parameters,
                                      .error_record = &error_record},
        (sxml_output_stream){.state = messages, .operations = &sxml_transform_message_operations});
    (void)written;
    sxml_cli_writer_write_byte(messages->writer, '\n');
    return SXML_STATUS_OK;
}

static const sxml_trace_sink_operations sxml_transform_message_sink = {
    .receive = sxml_transform_receive,
};

/* ---------------------------------------------------------------- Names */

/* The value of option long_name; empty when it is not given. */
static sxml_byte_view sxml_transform_option_value(const sxml_cli_invocation *invocation,
                                                  sxml_byte_view long_name) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
            &match) != SXML_STATUS_OK ||
        !match.is_present) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return match.value;
}

/* A name given on the command line: an NCName in no namespace or an EQName
   Q{uri}local (no prefixes: nothing binds them). False when it is
   neither. */
static bool sxml_transform_read_name(const sxml_query_run *run, sxml_byte_view text,
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

/* ---------------------------------------------------------- The command */

/* What the command line asks for. */
typedef struct sxml_transform_plan {
    sxml_byte_view stylesheet_file;
    sxml_byte_view stylesheet_text;
    /* The stylesheet's base URI, which its relative hrefs resolve against:
       file:PATH, or file://PATH for an absolute PATH; owned. */
    sxml_byte_view stylesheet_uri;
    sxml_name_identifier initial_template;
    sxml_name_identifier initial_mode;
    bool has_input;
    sxml_item input;
} sxml_transform_plan;

/* The stylesheet's base URI, which its relative hrefs resolve against
   (RFC 3986, section 5.2): file://PATH for an absolute PATH, and for a
   relative one file:// with the working directory before it, so that
   "../" in an href leaves the stylesheet's directory as it should;
   file:PATH when the working directory is unknown. */
static sxml_status sxml_transform_make_stylesheet_uri(const sxml_cli_invocation *invocation,
                                                      sxml_transform_plan *plan) {
    const sxml_byte_view path = plan->stylesheet_file;
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
    plan->stylesheet_uri = (sxml_byte_view){.bytes = bytes, .byte_count = written_count};
    return SXML_STATUS_OK;
}

/* Reads the operands and the names. Returns SXML_CLI_EXIT_SUCCESS to go
   on. */
static sxml_cli_exit_code sxml_transform_read_plan(sxml_query_run *run,
                                                   sxml_transform_plan *plan) {
    const sxml_cli_invocation *invocation = run->invocation;
    const sxml_byte_view template_text =
        sxml_transform_option_value(invocation, SXML_TEXT("initial-template"));
    const sxml_byte_view mode_text =
        sxml_transform_option_value(invocation, SXML_TEXT("initial-mode"));
    if (template_text.byte_count > 0 &&
        !sxml_transform_read_name(run, template_text, &plan->initial_template)) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("--initial-template needs an NCName or Q{uri}local"));
    }
    if (mode_text.byte_count > 0 &&
        !sxml_transform_read_name(run, mode_text, &plan->initial_mode)) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("--initial-mode needs an NCName or Q{uri}local"));
    }
    plan->has_input = sxml_cli_arguments_positional_count(invocation->arguments) > 1;
    if (mode_text.byte_count > 0 && (!plan->has_input || template_text.byte_count > 0)) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("--initial-mode applies templates to INPUT: give INPUT and "
                                  "no --initial-template"));
    }
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &plan->stylesheet_file) !=
        SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_status status = sxml_cli_file_reader_read(
        invocation->file_reader,
        &(sxml_cli_file_read_request){.path = plan->stylesheet_file,
                                      .allocator = invocation->allocator},
        &plan->stylesheet_text);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *error = invocation->standard_error;
        sxml_cli_writer_write(error, invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" transform: cannot read "));
        sxml_cli_writer_write_quoted(error, plan->stylesheet_file);
        sxml_cli_writer_write_byte(error, '\n');
        return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_CLI_EXIT_INPUT_OUTPUT_ERROR
                                                         : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (sxml_transform_make_stylesheet_uri(invocation, plan) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (!plan->has_input) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    sxml_byte_view input_file = {.bytes = NULL, .byte_count = 0};
    if (sxml_cli_arguments_get_positional(invocation->arguments, 1, &input_file) !=
        SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_cli_exit_code exit_code =
        sxml_query_run_parse_document(run, input_file, &plan->input);
    plan->has_input = exit_code == SXML_CLI_EXIT_SUCCESS;
    return exit_code;
}

/* The output definition of the principal result: the stylesheet's, with
   --method replacing its method. */
static sxml_status sxml_transform_output_definition(const sxml_query_run *run,
                                                    const sxml_xslt_stylesheet *stylesheet,
                                                    sxml_xslt_output_definition *definition) {
    const sxml_status status = sxml_xslt_stylesheet_get_output_definition(
        stylesheet, (sxml_name_identifier){.index = 0}, definition);
    if (status == SXML_STATUS_OK &&
        sxml_query_find_method(run->invocation,
                               &definition->serialization_parameters.serialization_method)) {
        definition->has_serialization_method = true;
    }
    return status;
}

/* Compiles the stylesheet, transforms and writes the result. */
/* A module whose file an error report names. */
typedef struct sxml_transform_module {
    /* The source name of the error's location: the stylesheet's file
       name, a module's URI, or empty. */
    sxml_byte_view module_uri;
    const sxml_transform_plan *plan;
} sxml_transform_module;

static bool sxml_transform_starts_with(sxml_byte_view text, sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 || memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

/* The file to name in an error report: STYLESHEET for the principal
   module (or no location), and for another module its path, relative to
   the working directory when it is inside it (the file:// URI the base
   URI of sxml_transform_make_stylesheet_uri gave it, without the
   scheme and the directory). */
static sxml_byte_view sxml_transform_module_file(const sxml_cli_invocation *invocation,
                                                 const sxml_transform_module *module) {
    sxml_byte_view name = module->module_uri;
    const sxml_byte_view directory = invocation->working_directory;
    if (name.byte_count == 0 || sxml_byte_view_equals(name, module->plan->stylesheet_file) ||
        sxml_byte_view_equals(name, module->plan->stylesheet_uri)) {
        return module->plan->stylesheet_file;
    }
    const sxml_byte_view absolute_scheme = SXML_TEXT("file://");
    const sxml_byte_view relative_scheme = SXML_TEXT("file:");
    if (sxml_transform_starts_with(name, absolute_scheme)) {
        name.bytes += absolute_scheme.byte_count;
        name.byte_count -= absolute_scheme.byte_count;
        if (directory.byte_count > 0 && directory.bytes[0] != '/' &&
            sxml_transform_starts_with(name, SXML_TEXT("/"))) {
            /* file:///C:/work: a Windows path. */
            name.bytes += 1;
            name.byte_count -= 1;
        }
        if (directory.byte_count > 0 && sxml_transform_starts_with(name, directory) &&
            name.byte_count > directory.byte_count &&
            (directory.bytes[directory.byte_count - 1] == '/' ||
             name.bytes[directory.byte_count] == '/')) {
            const size_t skipped_count =
                directory.byte_count + (directory.bytes[directory.byte_count - 1] == '/' ? 0 : 1);
            name.bytes += skipped_count;
            name.byte_count -= skipped_count;
        }
    } else if (sxml_transform_starts_with(name, relative_scheme)) {
        name.bytes += relative_scheme.byte_count;
        name.byte_count -= relative_scheme.byte_count;
    }
    return name;
}

static sxml_cli_exit_code sxml_transform_run(const sxml_query_run *run,
                                             const sxml_transform_plan *plan) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_dynamic_context_configuration parameters = sxml_query_run_dynamic_context(run);
    sxml_xslt_stylesheet *stylesheet = NULL;
    sxml_status status = sxml_xslt_stylesheet_create(
        invocation->allocator,
        &(sxml_xslt_stylesheet_configuration){
            .processor = run->processor,
            .principal_module_text = plan->stylesheet_text,
            .source_name = plan->stylesheet_file,
            /* Static parameters among the --param bindings (9.6); the
               others are ignored here and bound when priming. */
            .static_parameters = parameters.variable_bindings,
            .static_context = {.static_base_uri = plan->stylesheet_uri},
            .principal_module_source = SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT,
            .error_record = &error_record},
        &stylesheet);
    sxml_xslt_output_definition definition;
    memset(&definition, 0, sizeof(definition));
    if (status == SXML_STATUS_OK) {
        status = sxml_transform_output_definition(run, stylesheet, &definition);
    }
    sxml_transform_output output = {.writer = invocation->standard_output,
                                    .has_written = false,
                                    .ends_with_line_feed = false};
    sxml_transform_messages messages = {.writer = invocation->standard_error};
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context = parameters;
        dynamic_context.has_context_value = plan->has_input;
        dynamic_context.context_value =
            plan->has_input ? sxml_sequence_from_item(plan->input) : sxml_sequence_empty();
        dynamic_context.trace_sink =
            (sxml_trace_sink){.state = &messages, .operations = &sxml_transform_message_sink};
        const bool applies_templates =
            plan->has_input && plan->initial_template.index == 0;
        sxml_sequence principal_result = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .output_definition = &definition,
                .output_stream = {.state = &output,
                                  .operations = &sxml_transform_output_operations},
                .error_record = &error_record,
                .initial_mode_name = plan->initial_mode,
                .initial_template_name = applies_templates ? (sxml_name_identifier){.index = 0}
                                                           : plan->initial_template,
                .invocation_kind = applies_templates ? SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES
                                                     : SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED},
            &principal_result);
        sxml_sequence_release(principal_result);
    }
    if (status == SXML_STATUS_OK && output.has_written && !output.ends_with_line_feed) {
        sxml_cli_writer_write_byte(invocation->standard_output, '\n');
        if (invocation->standard_output->status != SXML_STATUS_OK) {
            status = SXML_STATUS_INPUT_OUTPUT_FAILED;
        }
    }
    sxml_cli_exit_code exit_code = SXML_CLI_EXIT_SUCCESS;
    /* An error in a module the stylesheet includes or imports names that
       module's file (its location is in the module). */
    const sxml_byte_view source_name = sxml_transform_module_file(
        invocation,
        &(sxml_transform_module){.module_uri = error_record.diagnostic.location.source_name,
                                 .plan = plan});
    error_record.diagnostic.location.source_name = source_name;
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8) {
        /* A stylesheet module that is not well-formed XML: reported as
           sxml check reports it, exit 1. */
        const sxml_cli_exit_code reported = sxml_query_run_fail(
            run, &(sxml_query_failure){.source_name = source_name,
                                       .error_record = &error_record,
                                       .status = status});
        (void)reported;
        exit_code = SXML_CLI_EXIT_OPERATION_FAILED;
    } else if (status != SXML_STATUS_OK) {
        exit_code = sxml_query_run_fail(
            run, &(sxml_query_failure){.source_name = source_name,
                                       .error_record = &error_record,
                                       .status = status});
    }
    sxml_xslt_stylesheet_destroy(stylesheet);
    return exit_code;
}

static sxml_cli_exit_code sxml_run_transform_command(const sxml_cli_invocation *invocation) {
    sxml_query_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.command_name = SXML_TEXT("transform");
    run.parameter_binding = SXML_PARAMETER_BINDING_BIND;
    sxml_transform_plan plan;
    memset(&plan, 0, sizeof(plan));
    sxml_cli_exit_code exit_code = sxml_query_run_start(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_transform_read_plan(&run, &plan);
    }
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_transform_run(&run, &plan);
    }
    if (plan.has_input) {
        sxml_item_release(plan.input);
    }
    sxml_allocator_release(invocation->allocator, (void *)plan.stylesheet_text.bytes);
    sxml_allocator_release(invocation->allocator, (void *)plan.stylesheet_uri.bytes);
    sxml_query_run_finish(&run);
    return exit_code;
}

const sxml_cli_command sxml_transform_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("transform"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Transform a document with an XSLT 4.0 stylesheet"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Compiles the XSLT 4.0 stylesheet STYLESHEET and runs it.\n"
        "\n"
        "With INPUT, the document becomes the global context item. Templates\n"
        "are applied to it in the default mode, or in the mode that\n"
        "--initial-mode names; or the template that --initial-template names is\n"
        "called with INPUT as the context item. Without INPUT, the template\n"
        "that --initial-template names, else xsl:initial-template, is called\n"
        "without a context item.\n"
        "\n"
        "The principal result goes to standard output, serialized as the\n"
        "stylesheet's xsl:output says; --method replaces the method. Result\n"
        "documents with an href are not written. xsl:message output goes to\n"
        "standard error. --param=NAME=VALUE binds the stylesheet parameter\n"
        "$NAME to xs:untypedAtomic(VALUE). Included and imported modules and\n"
        "documents are read from local files.\n"
        "\n"
        "An error is reported on standard error as\n"
        "  STYLESHEET:LINE:COLUMN: err:CODE: description\n"
        "\n"
        "Exit status: 0 on success, 1 for an error in the stylesheet or a\n"
        "document that is not well-formed, 3 when a file cannot be read."),
    .options = sxml_transform_options,
    .option_count = sizeof sxml_transform_options / sizeof sxml_transform_options[0],
    .positionals = sxml_transform_positionals,
    .positional_count = sizeof sxml_transform_positionals / sizeof sxml_transform_positionals[0],
    .examples = {.texts = sxml_transform_examples,
                 .text_count = sizeof sxml_transform_examples / sizeof sxml_transform_examples[0]},
    .run = sxml_run_transform_command,
};
