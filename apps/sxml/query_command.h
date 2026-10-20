#pragma once

#include "cli_command.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * What the query commands, sxml xpath and sxml xquery, share
 * (apps/sxml/README.md): a processor whose resource resolver reads local
 * files only, the --param and --context-item options, the --method
 * choices, the report of a query error and its exit code, and writing a
 * result to standard output. Public API only.
 */

/* The values of --method: the Serialization 4.0 methods sxml writes. */
enum { SXML_QUERY_METHOD_COUNT = 7 };
extern const sxml_byte_view sxml_query_methods[SXML_QUERY_METHOD_COUNT];

/*
 * The --allow-file-access option of sxml xpath and sxml xquery (D184,
 * D200): the query may call the EXPath File functions, which read and
 * write the platform's files. Without it they are absent (XPST0017).
 */
#define SXML_QUERY_FILE_ACCESS_OPTION                                          \
    {                                                                          \
        .long_name = SXML_BYTE_VIEW_INITIALIZER("allow-file-access"),          \
        .short_name = 0, .value_kind = SXML_CLI_VALUE_KIND_NONE,               \
        .value_name = {.bytes = NULL, .byte_count = 0},                        \
        .choices = {.texts = NULL, .text_count = 0},                           \
        .default_value = {.bytes = NULL, .byte_count = 0},                     \
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,                          \
        .summary = SXML_BYTE_VIEW_INITIALIZER(                                 \
            "Let the query read and write files (EXPath File functions)"),     \
    }

/*
 * The --output and --force options of the commands that write a result
 * (docs/CLI.md, "No hidden side effects"): --output=FILE (-o FILE) writes
 * the result to FILE instead of standard output, only when the command
 * succeeds, and never over an existing file unless --force is given.
 */
#define SXML_QUERY_OUTPUT_OPTION                                               \
    {                                                                          \
        .long_name = SXML_BYTE_VIEW_INITIALIZER("output"), .short_name = 'o',  \
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,                                \
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),                      \
        .choices = {.texts = NULL, .text_count = 0},                           \
        .default_value = {.bytes = NULL, .byte_count = 0},                     \
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,                          \
        .summary = SXML_BYTE_VIEW_INITIALIZER(                                 \
            "Write the result to FILE instead of standard output"),            \
    }

#define SXML_QUERY_FORCE_OPTION                                                \
    {                                                                          \
        .long_name = SXML_BYTE_VIEW_INITIALIZER("force"), .short_name = 0,     \
        .value_kind = SXML_CLI_VALUE_KIND_NONE,                                \
        .value_name = {.bytes = NULL, .byte_count = 0},                        \
        .choices = {.texts = NULL, .text_count = 0},                           \
        .default_value = {.bytes = NULL, .byte_count = 0},                     \
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,                          \
        .summary = SXML_BYTE_VIEW_INITIALIZER(                                 \
            "Replace files that exist instead of refusing"),                   \
    }

/* What --param does with $NAME. */
typedef enum sxml_parameter_binding {
    /* Declared in the static context and bound (XPath: the expression has
       no prolog to declare it in). */
    SXML_PARAMETER_BINDING_DECLARE,
    /* Only bound: the query declares it external (XQuery 4.0, 5.16). */
    SXML_PARAMETER_BINDING_BIND,
    /* The command has neither --param nor --context-item (sxml
       serialize). */
    SXML_PARAMETER_BINDING_NONE,
} sxml_parameter_binding;

/* The resolver's state. */
typedef struct sxml_query_file_resolver {
    const sxml_cli_invocation *invocation;
} sxml_query_file_resolver;

/* Writes to *uri the file URI of a file named on the command line, the
   base URI its relative references resolve against (RFC 3986, section
   5.2): file://PATH for an absolute PATH; for a relative one, file:// with
   the working directory before it, so that "../" leaves the file's
   directory as it should; file:PATH when the working directory is
   unknown. *uri is owned: release its bytes through the invocation's
   allocator. Returns the allocator's failures. */
SXML_NODISCARD sxml_status sxml_query_make_file_uri(const sxml_cli_invocation *invocation,
                                                    sxml_byte_view path, sxml_byte_view *uri);

/* The file a URI names, for messages: for a URI that
   sxml_query_make_file_uri gave, or one resolved against it, its path
   without the scheme, relative to the working directory when it is
   inside it; another URI unchanged. Borrowed from uri. */
sxml_byte_view sxml_query_file_of_uri(const sxml_cli_invocation *invocation, sxml_byte_view uri);

/* A file the run writes (--output, a result document, an fn:put): its
   bytes go to a temporary file, which becomes the file only when the
   command succeeds (sxml_query_run_finish). */
typedef struct sxml_query_file {
    sxml_cli_file_write write;
    /* The path as the user or the URI gave it, owned. */
    sxml_byte_view path;
} sxml_query_file;

/* Percent-decodes the path part text of a file URI into allocator memory
   (release *path's bytes through the invocation's allocator). Returns
   SXML_STATUS_INVALID_ARGUMENT for a query, a fragment, a backslash, a
   malformed escape or one that decodes to NUL, "/" or "\\". */
SXML_NODISCARD sxml_status sxml_query_decode_uri_path(const sxml_cli_invocation *invocation,
                                                      sxml_byte_view text, sxml_byte_view *path);

/* Writes to *normalized, owned, the file:// URI of a directory with its
   dot segments and empty parts removed (RFC 3986, 5.2.4) and a final "/".
   SXML_STATUS_INVALID_ARGUMENT for a URI that is not file://. */
SXML_NODISCARD sxml_status sxml_query_normalize_directory_uri(
    const sxml_cli_invocation *invocation, sxml_byte_view uri, sxml_byte_view *normalized);

/* The library's output stream over *stream, which must outlive it. */
sxml_output_stream sxml_query_library_stream(const sxml_cli_output_stream *stream);

/* One run of a query command. Set invocation, command_name and
   parameter_binding, zero the rest, then call sxml_query_run_start. */
typedef struct sxml_query_run {
    const sxml_cli_invocation *invocation;
    /* "xpath" or "xquery", for messages. */
    sxml_byte_view command_name;
    sxml_parameter_binding parameter_binding;
    sxml_query_file_resolver resolver;
    sxml_processor *processor;
    /* One per --param, in allocator memory. */
    sxml_variable_declaration *declarations;
    sxml_variable_binding *bindings;
    size_t parameter_count;
    bool has_context_item;
    sxml_item context_item;
    /* The prefixes of the optional features' functions, in scope for
       every query: bin and file (D198, D200). */
    sxml_namespace_binding feature_namespaces[3];
    size_t feature_namespace_count;
    /* The working directory's file URI, file:///DIRECTORY/, owned; empty
       when the working directory is unknown (packet 7.10b). */
    sxml_byte_view static_base_uri;
    /* Where the result goes: standard output, or output_writer over the
       file --output names (output_path, borrowed; empty without it). */
    sxml_cli_writer *result_writer;
    sxml_cli_writer output_writer;
    sxml_byte_view output_path;
    /* --force: files that exist may be replaced. */
    bool replaces_existing;
    /* Standard error as the library's stream, for the processor's trace
       sink (fn:trace, fn:message): sxml_query_run_trace_sink. */
    sxml_output_stream trace_stream;
    /* The files being written, in allocator memory. */
    sxml_query_file *files;
    size_t file_count;
    size_t file_capacity;
} sxml_query_run;

/* Makes the processor, reads --output and --force (and begins writing
   the --output file), and reads --param and --context-item (unless
   parameter_binding is NONE). Returns SXML_CLI_EXIT_SUCCESS to go on;
   otherwise the diagnostic is written. Call sxml_query_run_finish in
   every case. */
sxml_cli_exit_code sxml_query_run_start(sxml_query_run *run);

/* Begins writing the file at path (copied) for the run, honouring
   --force, and writes its stream to *stream. Returns
   SXML_CLI_EXIT_SUCCESS, or reports "PROGRAM COMMAND: 'PATH' exists; give
   --force to replace it" or "cannot write 'PATH'" and returns
   SXML_CLI_EXIT_INPUT_OUTPUT_ERROR (SXML_CLI_EXIT_INTERNAL_ERROR without
   memory). */
sxml_cli_exit_code sxml_query_run_begin_file(sxml_query_run *run, sxml_byte_view path,
                                             sxml_cli_output_stream *stream);

/* Writes "PROGRAM COMMAND: " to standard error, the start of a
   diagnostic of the run's own. */
void sxml_query_run_write_prefix(const sxml_query_run *run);

/* Parses FILE (- for standard input) into a document node, owned, at
   *document. Returns SXML_CLI_EXIT_SUCCESS, or reports "cannot read" (3)
   or "FILE:LINE:COLUMN: message" (1 when not well-formed, 4 at a limit). */
sxml_cli_exit_code sxml_query_run_parse_document(sxml_query_run *run, sxml_byte_view file,
                                                 sxml_item *document);

/* Ends the run: with exit_code SXML_CLI_EXIT_SUCCESS, commits every file
   it writes, in the order they were begun (a failure is reported and
   exits 3); otherwise abandons them, so that a failed command leaves no
   file behind. Releases what the run holds and returns the exit code. */
sxml_cli_exit_code sxml_query_run_finish(sxml_query_run *run, sxml_cli_exit_code exit_code);

/* The run's trace sink: fn:trace and fn:message write each event to
   standard error as one line, "LABEL: VALUE" (include/sxml/processor.h,
   sxml_stream_trace_sink). The processor of the run has it. */
sxml_trace_sink sxml_query_run_trace_sink(const sxml_query_run *run);

/* The static context with the --param declarations (none for BIND). */
sxml_static_context_configuration sxml_query_run_static_context(const sxml_query_run *run);

/* The dynamic context with the --param bindings and the context item. */
sxml_dynamic_context_configuration sxml_query_run_dynamic_context(const sxml_query_run *run);

/* Whether the option long_name is given; false for an option the command
   does not have. */
bool sxml_query_has_option(const sxml_cli_invocation *invocation, sxml_byte_view long_name);

/* The method --method names; false when it is not given. */
bool sxml_query_find_method(const sxml_cli_invocation *invocation,
                            sxml_serialization_method *method);

/* A query error to report: "SOURCE:LINE:COLUMN: err:CODE: description". */
typedef struct sxml_query_failure {
    /* "expression", or the query file's name. */
    sxml_byte_view source_name;
    const sxml_error_record *error_record;
    sxml_status status;
    /* The compiled query the error came from is not released yet, so the
       location's source name (borrowed from it) can be read: an error
       raised in a library module then names that module's file. */
    bool has_live_location;
} sxml_query_failure;

/* Reports failure on standard error (nothing for an output failure) and
   returns its exit code: 1 for a static, type or dynamic error, 3 for
   input/output, 4 otherwise. */
sxml_cli_exit_code sxml_query_run_fail(const sxml_query_run *run,
                                       const sxml_query_failure *failure);

/* A result to write to the run's result writer (standard output or the
   --output file), followed by a line feed when it is not empty (and the
   method is not csv, whose records end with one). */
typedef struct sxml_query_output {
    sxml_sequence result;
    sxml_serialization_parameters serialization_parameters;
    sxml_error_record *error_record;
} sxml_query_output;

SXML_NODISCARD sxml_status sxml_query_run_write(const sxml_query_run *run,
                                                const sxml_query_output *output);
