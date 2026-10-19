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
enum { SXML_QUERY_METHOD_COUNT = 6 };
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
    sxml_namespace_binding feature_namespaces[2];
    size_t feature_namespace_count;
} sxml_query_run;

/* Makes the processor and reads --param and --context-item (unless
   parameter_binding is NONE). Returns
   SXML_CLI_EXIT_SUCCESS to go on; otherwise the diagnostic is written.
   Call sxml_query_run_finish in every case. */
sxml_cli_exit_code sxml_query_run_start(sxml_query_run *run);

/* Parses FILE (- for standard input) into a document node, owned, at
   *document. Returns SXML_CLI_EXIT_SUCCESS, or reports "cannot read" (3)
   or "FILE:LINE:COLUMN: message" (1 when not well-formed, 4 at a limit). */
sxml_cli_exit_code sxml_query_run_parse_document(sxml_query_run *run, sxml_byte_view file,
                                                 sxml_item *document);

/* Releases what the run holds. */
void sxml_query_run_finish(sxml_query_run *run);

/* The static context with the --param declarations (none for BIND). */
sxml_static_context_configuration sxml_query_run_static_context(const sxml_query_run *run);

/* The dynamic context with the --param bindings and the context item. */
sxml_dynamic_context_configuration sxml_query_run_dynamic_context(const sxml_query_run *run);

/* The method --method names; false when it is not given. */
bool sxml_query_find_method(const sxml_cli_invocation *invocation,
                            sxml_serialization_method *method);

/* A query error to report: "SOURCE:LINE:COLUMN: err:CODE: description". */
typedef struct sxml_query_failure {
    /* "expression", or the query file's name. */
    sxml_byte_view source_name;
    const sxml_error_record *error_record;
    sxml_status status;
} sxml_query_failure;

/* Reports failure on standard error (nothing for an output failure) and
   returns its exit code: 1 for a static, type or dynamic error, 3 for
   input/output, 4 otherwise. */
sxml_cli_exit_code sxml_query_run_fail(const sxml_query_run *run,
                                       const sxml_query_failure *failure);

/* A result to write to standard output, followed by a line feed when it
   is not empty. */
typedef struct sxml_query_output {
    sxml_sequence result;
    sxml_serialization_parameters serialization_parameters;
    sxml_error_record *error_record;
} sxml_query_output;

SXML_NODISCARD sxml_status sxml_query_run_write(const sxml_query_run *run,
                                                const sxml_query_output *output);
