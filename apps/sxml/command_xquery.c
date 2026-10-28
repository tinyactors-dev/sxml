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
 * - --context-item=FILE parses FILE as the initial context value, after
 *   the query is compiled, so that under --locations=automatic the query
 *   decides whether it keeps locations (D313).
 * - The result is serialized with the parameters the query's output
 *   declarations give (5.22; method xml by default); --method=M replaces
 *   the method.
 * - An updating query (XQuery Update Facility 3.0, D187) writes the
 *   context item's document as its updates left it, in place of its
 *   value, which is empty; the file itself is not changed.
 * - fn:put (XQUF 3.0, 6.1; D205) writes the document to the local file
 *   its file: URI names only with --allow-put; without it the sink takes
 *   no documents and fn:put raises FOUP0002.
 * - --output=FILE (-o FILE) writes the result to FILE instead of standard
 *   output. Every file, puts included, is written only when the query
 *   succeeds; --force lets them replace files (query_command.h).
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
    SXML_QUERY_LOCATIONS_OPTION,
    SXML_QUERY_FILE_ACCESS_OPTION,
    SXML_QUERY_COLLECTION_OPTION,
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("allow-put"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_NONE,
        .value_name = {.bytes = NULL, .byte_count = 0},
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Let fn:put write the documents it stores to local files"),
    },
    SXML_CODE_LANGUAGE_OPTION,
    SXML_QUERY_OUTPUT_OPTION,
    SXML_QUERY_FORCE_OPTION,
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
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --context-item=draft.xml requirements.xq"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --expression='for $i in 1 to 3 return <i>{$i}</i>'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --param=n=5 --expression='declare variable $n external; $n * 2'"),
    SXML_BYTE_VIEW_INITIALIZER("sxml xquery --allow-put --context-item=draft.xml --expression='put(., \"copy.xml\")'"),
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

/* The update sink of an updating query: keeps the new tree of the
   context item's document (include/sxml/xquery.h, D187). */
typedef struct sxml_xquery_updated_context {
    sxml_node_reference context_node;
    bool has_update;
    sxml_item updated_root;
    /* For fn:put (--allow-put): the run whose files it writes, the
       evaluation's error record, and the reader of fn:put's $params,
       made on the first put. */
    sxml_query_run *run;
    sxml_error_record *error_record;
    sxml_serialization_parameter_reader *parameter_reader;
} sxml_xquery_updated_context;

static sxml_status sxml_xquery_receive_tree(void *state,
                                            const sxml_xquery_updated_tree *updated_tree) {
    sxml_xquery_updated_context *context = state;
    sxml_node_reference original;
    if (sxml_item_get_node(updated_tree->original_root, &original) == SXML_STATUS_OK &&
        context->context_node.tree != NULL && original.tree == context->context_node.tree) {
        sxml_item_retain(updated_tree->updated_root);
        context->updated_root = updated_tree->updated_root;
        context->has_update = true;
    }
    return SXML_STATUS_OK;
}

/* The file fn:put's URI names (XQUF 3.0, 6.1: an absolute URI, resolved
   against the static base URI), owned: the percent-decoded path of a
   file:// URI, relative to the working directory when it is inside it.
   INVALID_ARGUMENT for another URI. */
static sxml_status sxml_xquery_put_path(const sxml_cli_invocation *invocation, sxml_byte_view uri,
                                        sxml_byte_view *path) {
    const sxml_byte_view scheme = SXML_TEXT("file://");
    if (uri.byte_count <= scheme.byte_count ||
        memcmp(uri.bytes, scheme.bytes, scheme.byte_count) != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_byte_view file = sxml_query_file_of_uri(invocation, uri);
    if (file.byte_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_query_decode_uri_path(invocation, file, path);
}

/* Serializes the node of a put into the file at path, with the
   parameters its $params give (XQUF 3.0, 6.1: "serialization
   parameters"). */
static sxml_status sxml_xquery_write_put(sxml_xquery_updated_context *context,
                                         const sxml_xquery_put_document *put_document,
                                         sxml_byte_view path) {
    sxml_query_run *run = context->run;
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(run->processor);
    sxml_status status = SXML_STATUS_OK;
    if (context->parameter_reader == NULL) {
        status = sxml_serialization_parameter_reader_create(
            run->invocation->allocator,
            &(sxml_serialization_parameter_reader_configuration){
                .allocator = run->invocation->allocator, .item_heap = item_heap},
            &context->parameter_reader);
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_read(
            context->parameter_reader,
            &(sxml_serialization_parameter_read_request){
                .options = put_document->serialization_parameters,
                .error_record = context->error_record},
            &parameters);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cli_output_stream stream = {.state = NULL, .operations = NULL};
    const sxml_cli_exit_code exit_code = sxml_query_run_begin_file(run, path, &stream);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code == SXML_CLI_EXIT_INPUT_OUTPUT_ERROR ? SXML_STATUS_INPUT_OUTPUT_FAILED
                                                             : SXML_STATUS_OUT_OF_MEMORY;
    }
    return sxml_serialize_sequence(
        item_heap,
        &(sxml_serialization_request){.value = sxml_sequence_from_item(put_document->node),
                                      .serialization_parameters = parameters,
                                      .error_record = context->error_record},
        sxml_query_library_stream(&stream));
}

/* fn:put with --allow-put: the document is written to the file its URI
   names when the query succeeds; an existing file only with --force. */
static sxml_status sxml_xquery_receive_put(void *state,
                                           const sxml_xquery_put_document *put_document) {
    sxml_xquery_updated_context *context = state;
    const sxml_cli_invocation *invocation = context->run->invocation;
    sxml_byte_view path = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_xquery_put_path(invocation, put_document->uri, &path);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        sxml_query_run_write_prefix(context->run);
        sxml_cli_writer_write(invocation->standard_error, SXML_TEXT("fn:put cannot store "));
        sxml_cli_writer_write_quoted(invocation->standard_error, put_document->uri);
        sxml_cli_writer_write_line(invocation->standard_error,
                                   SXML_TEXT(": only file URIs name local files"));
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_write_put(context, put_document, path);
    }
    sxml_allocator_release(invocation->allocator, (void *)path.bytes);
    return status;
}

static const sxml_xquery_update_sink_operations sxml_xquery_sink_operations = {
    .receive_updated_tree = sxml_xquery_receive_tree, .receive_put_document = NULL};

/* With --allow-put (D205 leaves storing to the host). */
static const sxml_xquery_update_sink_operations sxml_xquery_put_sink_operations = {
    .receive_updated_tree = sxml_xquery_receive_tree,
    .receive_put_document = sxml_xquery_receive_put};

/* What an updating query writes: the context item's document as updated,
   or as it was; nothing without one. */
static sxml_sequence sxml_xquery_updated_output(const sxml_query_run *run,
                                                const sxml_xquery_updated_context *updated) {
    if (updated->has_update) {
        return sxml_sequence_from_item(updated->updated_root);
    }
    const sxml_dynamic_context_configuration dynamic_context = sxml_query_run_dynamic_context(run);
    if (!dynamic_context.has_context_value) {
        return sxml_sequence_empty();
    }
    sxml_sequence_retain(dynamic_context.context_value);
    return dynamic_context.context_value;
}

/* Compiles, evaluates and writes the query. */
static sxml_cli_exit_code sxml_xquery_evaluate(sxml_query_run *run,
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
    if (status == SXML_STATUS_OK) {
        /* After compiling: the query decides whether the document keeps
           its locations (D313). */
        const sxml_cli_exit_code context_exit_code = sxml_query_run_read_context_item(run);
        if (context_exit_code != SXML_CLI_EXIT_SUCCESS) {
            sxml_xquery_query_destroy(query);
            return context_exit_code;
        }
    }
    sxml_sequence result = sxml_sequence_empty();
    const sxml_dynamic_context_configuration dynamic_context = sxml_query_run_dynamic_context(run);
    sxml_xquery_updated_context updated = {.context_node = {.tree = NULL},
                                           .has_update = false,
                                           .updated_root = {.representation = 0},
                                           .run = run,
                                           .error_record = &error_record,
                                           .parameter_reader = NULL};
    const bool allows_put = sxml_query_has_option(invocation, SXML_TEXT("allow-put"));
    sxml_item context_item = {.representation = 0};
    if (dynamic_context.has_context_value &&
        sxml_sequence_get_item(dynamic_context.context_value,
                               (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &context_item) == SXML_STATUS_OK &&
        sxml_item_get_node(context_item, &updated.context_node) != SXML_STATUS_OK) {
        updated.context_node.tree = NULL;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query,
            &(sxml_xquery_evaluation_request){
                .dynamic_context = dynamic_context,
                .error_record = &error_record,
                .update_sink = {.state = &updated,
                                .operations = allows_put ? &sxml_xquery_put_sink_operations
                                                         : &sxml_xquery_sink_operations}},
            &result);
    }
    if (status == SXML_STATUS_OK && sxml_xquery_query_is_updating(query)) {
        sxml_sequence_release(result);
        result = sxml_xquery_updated_output(run, &updated);
        updated.has_update = false;
    }
    if (updated.has_update) {
        sxml_item_release(updated.updated_root);
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
    sxml_serialization_parameter_reader_destroy(updated.parameter_reader);
    /* Reported before the query is released: the location of an error
       raised in a library module borrows the module's name from it. */
    const sxml_cli_exit_code exit_code =
        status == SXML_STATUS_OK
            ? SXML_CLI_EXIT_SUCCESS
            : sxml_query_run_fail(run, &(sxml_query_failure){.source_name = query_text->source_name,
                                                             .error_record = &error_record,
                                                             .status = status,
                                                             .has_live_location = query != NULL});
    sxml_xquery_query_destroy(query);
    return exit_code;
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
    exit_code = sxml_query_run_finish(&run, exit_code);
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
        "directory; fn:doc reads local files. --allow-file-access lets the\n"
        "query read and write files with the EXPath File functions\n"
        "(namespace http://expath.org/ns/file); without it they do not exist.\n"
        "\n"
        "An updating query (the XQuery Update Facility 3.0) writes the\n"
        "context item's document as its updates left it; FILE itself is\n"
        "not changed. fn:put stores documents only with --allow-put, which\n"
        "writes each to the local file its URI names (relative URIs are in\n"
        "the working directory); without it fn:put raises FOUP0002.\n"
        "\n"
        "--output=FILE (-o FILE) writes the result to FILE instead. Files are\n"
        "written only when the query succeeds; one that exists is replaced\n"
        "only with --force.\n"
        "\n"
        "--locations=WHEN: whether the documents read keep the line and column\n"
        "of each node, for fn:location: automatic (the default) when the query\n"
        "calls fn:location, can look up functions or imports a schema,\n"
        "always, or never.\n"
        "\n"
        "An error is reported on standard error as\n"
        "  QUERY-FILE:LINE:COLUMN: err:CODE: description\n"
        "and, when its value is a node whose document kept its locations,\n"
        "  FILE:LINE:COLUMN: the node the error names\n"
        "\n"
        "Exit status: 0 on success, 1 for an error in the query or a context\n"
        "document that is not well-formed, 3 when a file cannot be read or\n"
        "written."),
    .options = sxml_xquery_options,
    .option_count = sizeof sxml_xquery_options / sizeof sxml_xquery_options[0],
    .positionals = sxml_xquery_positionals,
    .positional_count = sizeof sxml_xquery_positionals / sizeof sxml_xquery_positionals[0],
    .examples = {.texts = sxml_xquery_examples,
                 .text_count = sizeof sxml_xquery_examples / sizeof sxml_xquery_examples[0]},
    .run = sxml_run_xquery_command,
};
