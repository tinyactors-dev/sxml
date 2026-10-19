/*
 * sxml transform [--param=NAME=VALUE]... [--initial-template=NAME]
 * [--initial-mode=NAME] [--method=M] [--result-directory=DIR]
 * [--output=FILE] [--force] STYLESHEET [INPUT]: runs an XSLT 4.0
 * transformation and writes its principal result to standard output
 * (apps/sxml/README.md, "sxml transform"; include/sxml/xslt.h, D112, D157).
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
 *   When the stylesheet streams its initial mode
 *   (sxml_xslt_stylesheet_streams_source), an INPUT file is a streamed
 *   principal source instead, read through the resolver as it is parsed,
 *   and there is no global context item (D228).
 * - --param=NAME=VALUE, repeatable, binds the stylesheet parameter or
 *   static parameter $NAME to xs:untypedAtomic(VALUE), which the
 *   parameter's "as" type converts.
 * - The principal result is serialized as the stylesheet's unnamed
 *   xsl:output says (XSLT 4.0, 26.1); --method=M replaces the method. A
 *   result document without href is the principal result.
 * - --output=FILE (-o FILE) writes the principal result to FILE; its URI
 *   is then the base output URI (2.4.6).
 * - --result-directory=DIR writes result documents with an href (26.2):
 *   DIR's URI is the base output URI, and a result document whose
 *   absolute URI is not a file below it is refused (exit 3). Without the
 *   option they are reported and dropped (docs/CLI.md: files only where
 *   the command line names them).
 * - Every file is written only when the transformation succeeds; --force
 *   lets them replace files (query_command.h).
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
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("result-directory"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("DIR"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Write result documents with an href into the existing directory DIR"),
    },
    SXML_QUERY_OUTPUT_OPTION,
    SXML_QUERY_FORCE_OPTION,
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
    SXML_BYTE_VIEW_INITIALIZER("sxml transform --result-directory=site --output=site/index.html chapters.xsl books.xml"),
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

/* ---------------------------------------------------- Result documents */

/* The result document handler (include/sxml/xslt.h): with
   --result-directory, each result document is written into DIR, at its
   absolute URI's place below DIR's URI, the base output URI (XSLT 4.0,
   26.2.2); one whose URI is not below DIR is refused. Without it, each is
   reported as not written. Files are committed with the principal result,
   when the transformation succeeds (sxml_query_run_finish). */
typedef struct sxml_transform_results {
    sxml_query_run *run;
    /* --result-directory as given, and its normalized file URI; empty
       without the option. */
    sxml_byte_view directory;
    sxml_byte_view directory_uri;
    /* The stream of the result document being written (they do not
       overlap). */
    sxml_cli_output_stream current_stream;
} sxml_transform_results;

/* Writes "sxml transform: result document 'URI' " to standard error. */
static void sxml_transform_report_result(const sxml_transform_results *results,
                                         sxml_byte_view uri) {
    sxml_cli_writer *error = results->run->invocation->standard_error;
    sxml_query_run_write_prefix(results->run);
    sxml_cli_writer_write(error, SXML_TEXT("result document "));
    sxml_cli_writer_write_quoted(error, uri);
    sxml_cli_writer_write_byte(error, ' ');
}

/* Creates the directories of path below the result directory (the
   prefix of directory_byte_count bytes exists). */
static sxml_status sxml_transform_create_directories(const sxml_transform_results *results,
                                                     sxml_byte_view path) {
    const sxml_cli_file_writer writer = results->run->invocation->file_writer;
    for (size_t byte_index = results->directory.byte_count + 1; byte_index < path.byte_count;
         byte_index += 1) {
        if (path.bytes[byte_index] == '/') {
            const sxml_status status = writer.operations->create_directory(
                writer.state, (sxml_byte_view){.bytes = path.bytes, .byte_count = byte_index});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* The path of the result document at uri below the result directory,
   owned: DIR/RELATIVE with RELATIVE percent-decoded. INVALID_ARGUMENT when
   uri is not below DIR's URI or a part of RELATIVE is empty, "." or "..". */
static sxml_status sxml_transform_result_path(const sxml_transform_results *results,
                                              sxml_byte_view uri, sxml_byte_view *path) {
    const sxml_cli_invocation *invocation = results->run->invocation;
    const sxml_byte_view prefix = results->directory_uri;
    if (uri.byte_count <= prefix.byte_count ||
        memcmp(uri.bytes, prefix.bytes, prefix.byte_count) != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view relative = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_query_decode_uri_path(
        invocation,
        (sxml_byte_view){.bytes = uri.bytes + prefix.byte_count,
                         .byte_count = uri.byte_count - prefix.byte_count},
        &relative);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t part_start = 0;
    for (size_t byte_index = 0; byte_index <= relative.byte_count && status == SXML_STATUS_OK;
         byte_index += 1) {
        if (byte_index == relative.byte_count || relative.bytes[byte_index] == '/') {
            const sxml_byte_view part = {.bytes = relative.bytes + part_start,
                                         .byte_count = byte_index - part_start};
            if (part.byte_count == 0 || sxml_byte_view_equals(part, SXML_TEXT(".")) ||
                sxml_byte_view_equals(part, SXML_TEXT(".."))) {
                status = SXML_STATUS_INVALID_ARGUMENT;
            }
            part_start = byte_index + 1;
        }
    }
    sxml_byte_view directory = results->directory;
    while (directory.byte_count > 1 && directory.bytes[directory.byte_count - 1] == '/') {
        directory.byte_count -= 1;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = directory.byte_count + 1 + relative.byte_count,
                                      .alignment = 1},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        uint8_t *bytes = memory;
        memcpy(bytes, directory.bytes, directory.byte_count);
        bytes[directory.byte_count] = '/';
        memcpy(bytes + directory.byte_count + 1, relative.bytes, relative.byte_count);
        *path = (sxml_byte_view){.bytes = bytes,
                                 .byte_count = directory.byte_count + 1 + relative.byte_count};
    }
    sxml_allocator_release(invocation->allocator, (void *)relative.bytes);
    return status;
}

static sxml_status sxml_transform_open_result(void *state,
                                              const sxml_xslt_result_document_request *request,
                                              sxml_xslt_result_document_target *target) {
    sxml_transform_results *results = state;
    sxml_cli_writer *error = results->run->invocation->standard_error;
    if (results->directory.byte_count == 0) {
        /* Evaluated and dropped: sxml writes files only where the command
           line says (docs/CLI.md). */
        sxml_transform_report_result(results, request->href);
        sxml_cli_writer_write_line(error, SXML_TEXT("not written; give --result-directory"));
        target->result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW;
        return SXML_STATUS_OK;
    }
    sxml_byte_view path = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_transform_result_path(results, request->absolute_uri, &path);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        sxml_transform_report_result(
            results, sxml_query_file_of_uri(results->run->invocation, request->absolute_uri));
        sxml_cli_writer_write(error, SXML_TEXT("is not a file in --result-directory "));
        sxml_cli_writer_write_quoted(error, results->directory);
        sxml_cli_writer_write_byte(error, '\n');
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_transform_create_directories(results, path);
        if (status != SXML_STATUS_OK) {
            sxml_query_run_write_prefix(results->run);
            sxml_cli_writer_write(error, SXML_TEXT("cannot make the directory of "));
            sxml_cli_writer_write_quoted(error, path);
            sxml_cli_writer_write_byte(error, '\n');
        }
    }
    if (status == SXML_STATUS_OK) {
        const sxml_cli_exit_code exit_code =
            sxml_query_run_begin_file(results->run, path, &results->current_stream);
        status = exit_code == SXML_CLI_EXIT_SUCCESS            ? SXML_STATUS_OK
                 : exit_code == SXML_CLI_EXIT_INPUT_OUTPUT_ERROR ? SXML_STATUS_INPUT_OUTPUT_FAILED
                                                                 : SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_allocator_release(results->run->invocation->allocator, (void *)path.bytes);
    if (status == SXML_STATUS_OK) {
        target->result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
        target->output_stream = sxml_query_library_stream(&results->current_stream);
    }
    return status;
}

static sxml_status sxml_transform_close_result(void *state,
                                               const sxml_xslt_result_document *result_document) {
    /* The file is committed or abandoned with the run's other files. */
    (void)state;
    (void)result_document;
    return SXML_STATUS_OK;
}

static const sxml_xslt_result_document_handler_operations sxml_transform_result_operations = {
    .open = sxml_transform_open_result,
    .close = sxml_transform_close_result,
};

/* Reads --result-directory into results: DIR must be a directory. Returns
   SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_transform_read_results(sxml_query_run *run,
                                                      sxml_transform_results *results) {
    const sxml_cli_invocation *invocation = run->invocation;
    results->run = run;
    const sxml_byte_view directory =
        sxml_transform_option_value(invocation, SXML_TEXT("result-directory"));
    if (!sxml_query_has_option(invocation, SXML_TEXT("result-directory"))) {
        return SXML_CLI_EXIT_SUCCESS;
    }
    if (directory.byte_count == 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("--result-directory needs a directory name"));
    }
    sxml_cli_file_kind kind = SXML_CLI_FILE_KIND_NONE;
    const sxml_cli_file_writer writer = invocation->file_writer;
    if (writer.operations == NULL ||
        writer.operations->find_kind(writer.state, directory, &kind) != SXML_STATUS_OK ||
        kind != SXML_CLI_FILE_KIND_DIRECTORY) {
        sxml_query_run_write_prefix(run);
        sxml_cli_writer_write(invocation->standard_error, SXML_TEXT("--result-directory "));
        sxml_cli_writer_write_quoted(invocation->standard_error, directory);
        sxml_cli_writer_write_line(invocation->standard_error, SXML_TEXT(" is not a directory"));
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_query_make_file_uri(invocation, directory, &uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_query_normalize_directory_uri(invocation, uri, &results->directory_uri);
        sxml_allocator_release(invocation->allocator, (void *)uri.bytes);
    }
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        /* The working directory is unknown: no absolute base output URI. */
        sxml_query_run_write_prefix(run);
        sxml_cli_writer_write_line(invocation->standard_error,
                                   SXML_TEXT("cannot tell the working directory"));
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    results->directory = directory;
    return SXML_CLI_EXIT_SUCCESS;
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
    /* INPUT, read once the stylesheet is compiled: as a tree, the global
       context item, or as a streamed principal source (D228). */
    bool has_input;
    sxml_byte_view input_file;
    bool has_input_tree;
    sxml_item input;
    sxml_transform_results results;
    /* The base output URI (XSLT 4.0, 2.4.6): the result directory's URI,
       else the --output file's; empty without either. Owned. */
    sxml_byte_view base_output_uri;
} sxml_transform_plan;

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
    if (sxml_query_make_file_uri(invocation, plan->stylesheet_file, &plan->stylesheet_uri) !=
        SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_cli_exit_code results_exit_code = sxml_transform_read_results(run, &plan->results);
    if (results_exit_code != SXML_CLI_EXIT_SUCCESS) {
        return results_exit_code;
    }
    if (plan->results.directory.byte_count == 0 && run->output_path.byte_count > 0 &&
        invocation->working_directory.byte_count > 0 &&
        sxml_query_make_file_uri(invocation, run->output_path, &plan->base_output_uri) !=
            SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (plan->has_input &&
        sxml_cli_arguments_get_positional(invocation->arguments, 1, &plan->input_file) !=
            SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* Reads INPUT for request: when the stylesheet streams it (its initial
   mode is streamable, sxml_xslt_stylesheet_streams_source) and it is a
   file, as the URI of a streamed principal source, written to *uri
   (owned), so the document is never built whole (XSLT 4.0, 2.4.3 and 18;
   D228); otherwise parsed into plan->input, the global context item.
   Returns SXML_CLI_EXIT_SUCCESS to go on. */
typedef struct sxml_transform_input {
    const sxml_xslt_stylesheet *stylesheet;
    const sxml_xslt_transformation_request *request;
    sxml_byte_view *uri;
} sxml_transform_input;

static sxml_cli_exit_code sxml_transform_read_input(sxml_query_run *run,
                                                    sxml_transform_plan *plan,
                                                    const sxml_transform_input *input) {
    bool streams = false;
    if (input->request->invocation_kind == SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES &&
        !sxml_byte_view_equals(plan->input_file, SXML_TEXT("-")) &&
        sxml_xslt_stylesheet_streams_source(input->stylesheet, input->request, &streams) !=
            SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    /* A file that cannot be found is reported as before, by the parse. */
    const sxml_cli_file_writer writer = run->invocation->file_writer;
    sxml_cli_file_kind kind = SXML_CLI_FILE_KIND_NONE;
    if (streams && (writer.operations == NULL ||
                    writer.operations->find_kind(writer.state, plan->input_file, &kind) !=
                        SXML_STATUS_OK ||
                    kind != SXML_CLI_FILE_KIND_FILE)) {
        streams = false;
    }
    if (streams) {
        return sxml_query_make_file_uri(run->invocation, plan->input_file, input->uri) ==
                       SXML_STATUS_OK
                   ? SXML_CLI_EXIT_SUCCESS
                   : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_cli_exit_code exit_code =
        sxml_query_run_parse_document(run, plan->input_file, &plan->input);
    plan->has_input_tree = exit_code == SXML_CLI_EXIT_SUCCESS;
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

/* The file to name in an error report: STYLESHEET for the principal
   module (or no location), and for another module the file its URI names
   (sxml_query_file_of_uri). */
static sxml_byte_view sxml_transform_module_file(const sxml_cli_invocation *invocation,
                                                 const sxml_transform_module *module) {
    const sxml_byte_view name = module->module_uri;
    if (name.byte_count == 0 || sxml_byte_view_equals(name, module->plan->stylesheet_file) ||
        sxml_byte_view_equals(name, module->plan->stylesheet_uri)) {
        return module->plan->stylesheet_file;
    }
    return sxml_query_file_of_uri(invocation, name);
}

static sxml_cli_exit_code sxml_transform_run(sxml_query_run *run, sxml_transform_plan *plan) {
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
    sxml_transform_output output = {.writer = run->result_writer,
                                    .has_written = false,
                                    .ends_with_line_feed = false};
    sxml_transform_messages messages = {.writer = invocation->standard_error};
    sxml_cli_exit_code input_exit_code = SXML_CLI_EXIT_SUCCESS;
    sxml_byte_view streamed_source_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context = parameters;
        dynamic_context.trace_sink =
            (sxml_trace_sink){.state = &messages, .operations = &sxml_transform_message_sink};
        const bool applies_templates =
            plan->has_input && plan->initial_template.index == 0;
        sxml_xslt_transformation_request request = {
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
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
                .base_output_uri = plan->results.directory_uri.byte_count > 0
                                       ? plan->results.directory_uri
                                       : plan->base_output_uri,
                .result_document_handler = {.state = (void *)&plan->results,
                                            .operations = &sxml_transform_result_operations}};
        if (plan->has_input) {
            input_exit_code = sxml_transform_read_input(
                run, plan,
                &(sxml_transform_input){
                    .stylesheet = stylesheet, .request = &request, .uri = &streamed_source_uri});
        }
        if (input_exit_code == SXML_CLI_EXIT_SUCCESS) {
            request.streamed_source_uri = streamed_source_uri;
            request.dynamic_context.has_context_value = plan->has_input_tree;
            request.dynamic_context.context_value = plan->has_input_tree
                                                        ? sxml_sequence_from_item(plan->input)
                                                        : sxml_sequence_empty();
            sxml_sequence principal_result = sxml_sequence_empty();
            status = sxml_xslt_stylesheet_transform(stylesheet, &request, &principal_result);
            sxml_sequence_release(principal_result);
        }
    }
    sxml_allocator_release(invocation->allocator, (void *)streamed_source_uri.bytes);
    if (input_exit_code != SXML_CLI_EXIT_SUCCESS) {
        sxml_xslt_stylesheet_destroy(stylesheet);
        return input_exit_code;
    }
    if (status == SXML_STATUS_OK && output.has_written && !output.ends_with_line_feed) {
        sxml_cli_writer_write_byte(run->result_writer, '\n');
        if (run->result_writer->status != SXML_STATUS_OK) {
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
    if (plan.has_input_tree) {
        sxml_item_release(plan.input);
    }
    sxml_allocator_release(invocation->allocator, (void *)plan.stylesheet_text.bytes);
    sxml_allocator_release(invocation->allocator, (void *)plan.stylesheet_uri.bytes);
    sxml_allocator_release(invocation->allocator, (void *)plan.base_output_uri.bytes);
    sxml_allocator_release(invocation->allocator, (void *)plan.results.directory_uri.bytes);
    exit_code = sxml_query_run_finish(&run, exit_code);
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
        "When templates are applied to an INPUT file in a mode declared\n"
        "streamable=\"yes\" whose template rules are all guaranteed-streamable,\n"
        "INPUT is streamed and is not the global context item.\n"
        "\n"
        "The principal result goes to standard output, or to the file that\n"
        "--output names, serialized as the stylesheet's xsl:output says;\n"
        "--method replaces the method. xsl:message output goes to standard\n"
        "error. --param=NAME=VALUE binds the stylesheet parameter $NAME to\n"
        "xs:untypedAtomic(VALUE). Included and imported modules and documents\n"
        "are read from local files.\n"
        "\n"
        "Result documents with an href are written only with\n"
        "--result-directory=DIR: DIR, which must exist, is the base output\n"
        "URI, so an href resolves against it, and a result document whose URI\n"
        "is not a file below DIR is an error. Directories below DIR are made\n"
        "as needed. Without the option each is reported on standard error\n"
        "and not written.\n"
        "\n"
        "Files are written only when the transformation succeeds; one that\n"
        "exists is replaced only with --force.\n"
        "\n"
        "An error is reported on standard error as\n"
        "  STYLESHEET:LINE:COLUMN: err:CODE: description\n"
        "\n"
        "Exit status: 0 on success, 1 for an error in the stylesheet or a\n"
        "document that is not well-formed, 3 when a file cannot be read or\n"
        "written."),
    .options = sxml_transform_options,
    .option_count = sizeof sxml_transform_options / sizeof sxml_transform_options[0],
    .positionals = sxml_transform_positionals,
    .positional_count = sizeof sxml_transform_positionals / sizeof sxml_transform_positionals[0],
    .examples = {.texts = sxml_transform_examples,
                 .text_count = sizeof sxml_transform_examples / sizeof sxml_transform_examples[0]},
    .run = sxml_run_transform_command,
};
