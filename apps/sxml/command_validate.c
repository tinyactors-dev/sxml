/*
 * sxml validate --schema=SCHEMA [--schema=SCHEMA]... [--mode=MODE]
 * [--format=FORMAT] FILE...: validates every FILE against the XSD 1.1
 * schema the SCHEMA documents form (apps/sxml/README.md; docs/tasks/phase-5.md,
 * "sxml validate"; include/sxml/schema.h, D165, D173).
 *
 * Golden file: command_check.c, whose report shapes, statuses and exit
 * codes it extends; the processor and its local-file resolver are
 * query_command.h's.
 *
 * - The SCHEMA documents are read first and compiled into one schema
 *   (XSD 1.1 Part 1, 4.2 and 4.3), with every document they include,
 *   import, redefine or override, read through the local-file resolver
 *   relative to the referring document. Its errors are reported as
 *   "SCHEMA:LINE:COLUMN: CONSTRAINT: description", and then no FILE is
 *   read.
 * - Each FILE is parsed and validated in one pass: the XML parser feeds
 *   the validator, whose result builder is the null builder, so no tree is
 *   built (schema.h). Every error goes to the report sink and is reported
 *   as "FILE:LINE:COLUMN: CONSTRAINT: description". The validation root is
 *   the document element, assessed strictly or laxly as --mode says (Part
 *   1, 5.2). xsi:schemaLocation and xsi:noNamespaceSchemaLocation hints are
 *   not followed (Part 1, 4.3.2 leaves it to the processor; schema.h): the
 *   schema is the one the command line names.
 * - JSON format: one object on standard output,
 *
 *     {"schema":{"documents":["po.xsd"],"status":"valid"},
 *      "files":[{"file":"a.xml","status":"valid"},
 *               {"file":"b.xml","status":"invalid","errors":[{"code":
 *                "cvc-complex-type.2.4","line":3,"column":5,
 *                "byte_offset":40,"message":"..."}]}]}
 *
 *   whose errors are kept in an arena until their file is written; fields
 *   are only ever added.
 *
 * Exit code: 3 when a SCHEMA or FILE cannot be read, else 4 when sxml
 * failed (a limit, out of memory), else 1 when the schema or a FILE is
 * invalid or a FILE is not well-formed, else 0.
 */
#include "cli_arguments.h"
#include "program.h"
#include "query_command.h"
#include "sxml/memory.h"
#include "sxml/schema.h"
#include "sxml/xml.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* As sxml check: entity expansion is bounded, nesting is not. */
enum { SXML_VALIDATE_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* The arena blocks that keep the errors of the JSON report. */
enum { SXML_VALIDATE_ERROR_BLOCK_BYTE_COUNT = 16 * 1024 };

static const sxml_byte_view sxml_validate_modes[] = {
    SXML_BYTE_VIEW_INITIALIZER("strict"),
    SXML_BYTE_VIEW_INITIALIZER("lax"),
};

static const sxml_byte_view sxml_validate_formats[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_validate_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("schema"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("SCHEMA"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = {.bytes = NULL, .byte_count = 0},
        .multiplicity = SXML_CLI_OPTION_AT_LEAST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("XSD schema document; - reads standard input"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("mode"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("MODE"),
        .choices = {.texts = sxml_validate_modes,
                    .text_count = sizeof sxml_validate_modes / sizeof sxml_validate_modes[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("strict"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Assessment of the document element"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
        .choices = {.texts = sxml_validate_formats,
                    .text_count = sizeof sxml_validate_formats / sizeof sxml_validate_formats[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Report format"),
    },
};

static const sxml_cli_positional sxml_validate_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .multiplicity = SXML_CLI_POSITIONAL_ONE_OR_MORE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("XML document to validate; - reads standard input"),
    },
};

static const sxml_byte_view sxml_validate_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml validate --schema=books.xsd books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml validate --schema=books.xsd bad-books.xml"),
    SXML_BYTE_VIEW_INITIALIZER("sxml validate --schema=books.xsd --format=json books.xml"),
};

/* What happened to the schema or to one file. */
typedef enum sxml_validate_outcome {
    SXML_VALIDATE_OUTCOME_VALID,
    SXML_VALIDATE_OUTCOME_INVALID,
    /* Lax assessment found no declaration for the document element. */
    SXML_VALIDATE_OUTCOME_NOT_KNOWN,
    SXML_VALIDATE_OUTCOME_NOT_WELL_FORMED,
    SXML_VALIDATE_OUTCOME_UNREADABLE,
    /* A limit, out of memory, a feature sxml does not have yet. */
    SXML_VALIDATE_OUTCOME_FAILED,
} sxml_validate_outcome;

/* One error kept for the JSON report, in the run's arena. */
typedef struct sxml_validate_error {
    struct sxml_validate_error *next;
    /* The schema document of a schema error; empty for a file's. */
    sxml_byte_view document;
    sxml_byte_view code;
    sxml_byte_view description;
    uint32_t line_number;
    uint32_t column_number;
    size_t byte_offset;
} sxml_validate_error;

/* The SCHEMA documents of the command line. */
typedef struct sxml_validate_schema_files {
    /* Borrowed from the command line. */
    sxml_byte_view *names;
    /* Owned: the file URIs, empty for standard input. */
    sxml_byte_view *uris;
    /* Owned: the documents read, the schema_documents of the schema. */
    sxml_schema_document *documents;
    size_t file_count;
} sxml_validate_schema_files;

/* The state of one run of the command. */
typedef struct sxml_validate_run {
    const sxml_cli_invocation *invocation;
    sxml_query_run query;
    sxml_validate_schema_files schema_files;
    sxml_schema *schema;
    sxml_xml_parser *parser;
    sxml_validation_mode validation_mode;
    bool writes_json;
    /* The errors of the schema or of the file being validated, for JSON. */
    sxml_arena *error_arena;
    sxml_validate_error *first_error;
    sxml_validate_error *last_error;
    /* Errors received for the schema or the file being validated. */
    size_t error_count;
    /* The FILE being validated; empty while the schema is compiled. */
    sxml_byte_view file;
    size_t reported_file_count;
    bool has_unreadable_input;
    bool has_failure;
    bool has_invalid_input;
} sxml_validate_run;

/* The schema or one file, with what happened to it. */
typedef struct sxml_validate_report {
    /* The FILE; empty for the schema. */
    sxml_byte_view file;
    sxml_validate_outcome outcome;
    /* NOT_WELL_FORMED, UNREADABLE (in text) and FAILED: what went wrong. */
    const sxml_diagnostic *diagnostic;
} sxml_validate_report;

static sxml_byte_view sxml_validate_outcome_name(sxml_validate_outcome outcome) {
    switch (outcome) {
    case SXML_VALIDATE_OUTCOME_VALID:
        return SXML_TEXT("valid");
    case SXML_VALIDATE_OUTCOME_INVALID:
        return SXML_TEXT("invalid");
    case SXML_VALIDATE_OUTCOME_NOT_KNOWN:
        return SXML_TEXT("not-known");
    case SXML_VALIDATE_OUTCOME_NOT_WELL_FORMED:
        return SXML_TEXT("not-well-formed");
    case SXML_VALIDATE_OUTCOME_UNREADABLE:
        return SXML_TEXT("unreadable");
    case SXML_VALIDATE_OUTCOME_FAILED:
        break;
    }
    return SXML_TEXT("failed");
}

/* ------------------------------------------------------------- Names */

/* The schema document a report's source name (an absolute URI) names: a
   SCHEMA as the command line gave it, else the file of the URI (an
   included or imported document). Only standard input has no URI: the
   empty name is "-" when it is a SCHEMA, else unknown (empty). */
static sxml_byte_view sxml_validate_document_name(const sxml_validate_run *run,
                                                  sxml_byte_view source_name) {
    const sxml_validate_schema_files *files = &run->schema_files;
    if (source_name.byte_count == 0) {
        for (size_t file_index = 0; file_index < files->file_count; file_index += 1) {
            if (sxml_byte_view_equals(files->names[file_index], SXML_TEXT("-"))) {
                return files->names[file_index];
            }
        }
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    for (size_t file_index = 0; file_index < files->file_count; file_index += 1) {
        if (sxml_byte_view_equals(source_name, files->uris[file_index])) {
            return files->names[file_index];
        }
    }
    return sxml_query_file_of_uri(run->invocation, source_name);
}

/* ----------------------------------------------------------- Errors */

/* Copies text into the error arena. */
static sxml_status sxml_validate_keep_text(sxml_validate_run *run, sxml_byte_view text,
                                           sxml_byte_view *kept) {
    if (text.byte_count == 0) {
        *kept = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_status status = sxml_arena_allocate(
        run->error_arena, (sxml_allocation_request){.byte_count = text.byte_count, .alignment = 1},
        &memory);
    if (status == SXML_STATUS_OK) {
        memcpy(memory, text.bytes, text.byte_count);
        *kept = (sxml_byte_view){.bytes = memory, .byte_count = text.byte_count};
    }
    return status;
}

/* Keeps one error for the JSON report. */
static sxml_status sxml_validate_keep_error(sxml_validate_run *run,
                                            const sxml_validate_error *error) {
    void *memory = NULL;
    sxml_status status = sxml_arena_allocate(
        run->error_arena,
        (sxml_allocation_request){.byte_count = sizeof(sxml_validate_error),
                                  .alignment = alignof(sxml_validate_error)},
        &memory);
    sxml_validate_error *kept = memory;
    if (status == SXML_STATUS_OK) {
        *kept = *error;
        kept->next = NULL;
        status = sxml_validate_keep_text(run, error->document, &kept->document);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_validate_keep_text(run, error->code, &kept->code);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_validate_keep_text(run, error->description, &kept->description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (run->last_error == NULL) {
        run->first_error = kept;
    } else {
        run->last_error->next = kept;
    }
    run->last_error = kept;
    return SXML_STATUS_OK;
}

/* "NAME[:LINE:COLUMN]: " on standard error; "sxml validate: " for an
   unknown NAME. */
static void sxml_validate_write_place(const sxml_validate_run *run, sxml_byte_view name,
                                      const sxml_source_location *location) {
    sxml_cli_writer *error = run->invocation->standard_error;
    if (name.byte_count == 0) {
        sxml_cli_writer_write(error, run->invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" validate: "));
        return;
    }
    sxml_cli_writer_write_escaped(error, name);
    if (location->line_number != 0) {
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, location->line_number);
        sxml_cli_writer_write_byte(error, ':');
        sxml_cli_writer_write_decimal(error, location->column_number);
    }
    sxml_cli_writer_write(error, SXML_TEXT(": "));
}

/* The report sink of the compilation and of each validation: text goes
   to standard error at once, JSON waits in the arena. */
/* A report's description without the constraint name that some
   descriptions start with ("cvc-minInclusive-valid: the value ..."), so
   that it is not shown twice. */
static sxml_byte_view sxml_validate_description(const sxml_schema_report *report) {
    const sxml_byte_view code = report->code.local_name;
    sxml_byte_view description = report->description;
    if (code.byte_count > 0 && description.byte_count > code.byte_count + 2 &&
        memcmp(description.bytes, code.bytes, code.byte_count) == 0 &&
        description.bytes[code.byte_count] == ':' &&
        description.bytes[code.byte_count + 1] == ' ') {
        description.bytes += code.byte_count + 2;
        description.byte_count -= code.byte_count + 2;
    }
    return description;
}

static sxml_status sxml_validate_receive_report(void *state, const sxml_schema_report *report) {
    sxml_validate_run *run = state;
    run->error_count += 1;
    const sxml_byte_view description = sxml_validate_description(report);
    const bool is_schema_error = run->file.byte_count == 0;
    const sxml_byte_view name = is_schema_error
                                    ? sxml_validate_document_name(run, report->location.source_name)
                                    : run->file;
    if (run->writes_json) {
        return sxml_validate_keep_error(
            run, &(sxml_validate_error){
                     .next = NULL,
                     .document = is_schema_error ? name : (sxml_byte_view){.bytes = NULL,
                                                                           .byte_count = 0},
                     .code = report->code.local_name,
                     .description = description,
                     .line_number = report->location.line_number,
                     .column_number = report->location.column_number,
                     .byte_offset = report->location.byte_offset});
    }
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_validate_write_place(run, name, &report->location);
    if (report->code.local_name.byte_count > 0) {
        sxml_cli_writer_write_escaped(error, report->code.local_name);
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, description);
    sxml_cli_writer_write_byte(error, '\n');
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations sxml_validate_report_sink_operations = {
    .receive = sxml_validate_receive_report,
};

/* Forgets the errors kept for the last report. */
static void sxml_validate_clear_errors(sxml_validate_run *run) {
    sxml_arena_reset(run->error_arena);
    run->first_error = NULL;
    run->last_error = NULL;
    run->error_count = 0;
}

/* ---------------------------------------------------------- Reports */

static void sxml_validate_write_json_location(sxml_cli_writer *output,
                                              const sxml_validate_error *error) {
    if (error->line_number == 0) {
        return;
    }
    sxml_cli_writer_write(output, SXML_TEXT(",\"line\":"));
    sxml_cli_writer_write_decimal(output, error->line_number);
    sxml_cli_writer_write(output, SXML_TEXT(",\"column\":"));
    sxml_cli_writer_write_decimal(output, error->column_number);
    sxml_cli_writer_write(output, SXML_TEXT(",\"byte_offset\":"));
    sxml_cli_writer_write_decimal(output, error->byte_offset);
}

/* ,"errors":[...] for the errors kept, when there are any. */
static void sxml_validate_write_json_errors(const sxml_validate_run *run) {
    sxml_cli_writer *output = run->invocation->standard_output;
    if (run->first_error == NULL) {
        return;
    }
    sxml_cli_writer_write(output, SXML_TEXT(",\"errors\":["));
    for (const sxml_validate_error *error = run->first_error; error != NULL;
         error = error->next) {
        if (error != run->first_error) {
            sxml_cli_writer_write_byte(output, ',');
        }
        sxml_cli_writer_write_byte(output, '{');
        if (error->document.byte_count > 0) {
            sxml_cli_writer_write(output, SXML_TEXT("\"document\":"));
            sxml_cli_writer_write_json_string(output, error->document);
            sxml_cli_writer_write_byte(output, ',');
        }
        sxml_cli_writer_write(output, SXML_TEXT("\"code\":"));
        sxml_cli_writer_write_json_string(output, error->code);
        sxml_validate_write_json_location(output, error);
        sxml_cli_writer_write(output, SXML_TEXT(",\"message\":"));
        sxml_cli_writer_write_json_string(output, error->description);
        sxml_cli_writer_write_byte(output, '}');
    }
    sxml_cli_writer_write_byte(output, ']');
}

/* The diagnostic fields of a not well-formed or failed file: line,
   column, byte_offset (when known) and message. */
static void sxml_validate_write_json_diagnostic(sxml_cli_writer *output,
                                                const sxml_diagnostic *diagnostic) {
    const sxml_validate_error located = {.next = NULL,
                                         .document = {.bytes = NULL, .byte_count = 0},
                                         .code = {.bytes = NULL, .byte_count = 0},
                                         .description = diagnostic->description,
                                         .line_number = diagnostic->location.line_number,
                                         .column_number = diagnostic->location.column_number,
                                         .byte_offset = diagnostic->location.byte_offset};
    sxml_validate_write_json_location(output, &located);
    sxml_cli_writer_write(output, SXML_TEXT(",\"message\":"));
    sxml_cli_writer_write_json_string(output, diagnostic->description);
}

static void sxml_validate_write_json(const sxml_validate_run *run,
                                     const sxml_validate_report *report) {
    sxml_cli_writer *output = run->invocation->standard_output;
    if (report->file.byte_count == 0) {
        sxml_cli_writer_write(output, SXML_TEXT("{\"schema\":{\"documents\":["));
        const sxml_validate_schema_files *files = &run->schema_files;
        for (size_t file_index = 0; file_index < files->file_count; file_index += 1) {
            if (file_index > 0) {
                sxml_cli_writer_write_byte(output, ',');
            }
            sxml_cli_writer_write_json_string(output, files->names[file_index]);
        }
        sxml_cli_writer_write(output, SXML_TEXT("],\"status\":"));
    } else {
        sxml_cli_writer_write(output, run->reported_file_count == 0 ? SXML_TEXT("{\"file\":")
                                                                    : SXML_TEXT(",{\"file\":"));
        sxml_cli_writer_write_json_string(output, report->file);
        sxml_cli_writer_write(output, SXML_TEXT(",\"status\":"));
    }
    sxml_cli_writer_write_json_string(output, sxml_validate_outcome_name(report->outcome));
    if (report->diagnostic != NULL) {
        sxml_validate_write_json_diagnostic(output, report->diagnostic);
    }
    sxml_validate_write_json_errors(run);
    sxml_cli_writer_write(output, report->file.byte_count == 0 ? SXML_TEXT("},\"files\":[")
                                                               : SXML_TEXT("}"));
}

/* The text of a report; the errors themselves were written as they came. */
static void sxml_validate_write_text(const sxml_validate_run *run,
                                     const sxml_validate_report *report) {
    sxml_cli_writer *error = run->invocation->standard_error;
    const sxml_diagnostic *diagnostic = report->diagnostic;
    switch (report->outcome) {
    case SXML_VALIDATE_OUTCOME_VALID:
    case SXML_VALIDATE_OUTCOME_INVALID:
        return;
    case SXML_VALIDATE_OUTCOME_NOT_KNOWN:
        sxml_cli_writer_write_escaped(error, report->file);
        sxml_cli_writer_write_line(
            error, SXML_TEXT(": validity not known: the schema has no declaration of the "
                             "document element (lax assessment)"));
        return;
    case SXML_VALIDATE_OUTCOME_UNREADABLE:
        sxml_cli_writer_write(error, run->invocation->program->name);
        sxml_cli_writer_write(error, SXML_TEXT(" validate: cannot read "));
        sxml_cli_writer_write_quoted(error, diagnostic->location.source_name);
        sxml_cli_writer_write_byte(error, '\n');
        return;
    case SXML_VALIDATE_OUTCOME_NOT_WELL_FORMED:
    case SXML_VALIDATE_OUTCOME_FAILED:
        break;
    }
    const sxml_byte_view name =
        report->file.byte_count > 0
            ? report->file
            : sxml_validate_document_name(run, diagnostic->location.source_name);
    sxml_validate_write_place(run, name, &diagnostic->location);
    sxml_cli_writer_write_line(error, diagnostic->description);
}

/* Reports the schema or one file, and forgets its errors. */
static void sxml_validate_submit_report(sxml_validate_run *run,
                                        const sxml_validate_report *report) {
    switch (report->outcome) {
    case SXML_VALIDATE_OUTCOME_VALID:
    case SXML_VALIDATE_OUTCOME_NOT_KNOWN:
        break;
    case SXML_VALIDATE_OUTCOME_INVALID:
    case SXML_VALIDATE_OUTCOME_NOT_WELL_FORMED:
        run->has_invalid_input = true;
        break;
    case SXML_VALIDATE_OUTCOME_UNREADABLE:
        run->has_unreadable_input = true;
        break;
    case SXML_VALIDATE_OUTCOME_FAILED:
        run->has_failure = true;
        break;
    }
    if (run->writes_json) {
        sxml_validate_write_json(run, report);
    } else {
        sxml_validate_write_text(run, report);
    }
    if (report->file.byte_count > 0) {
        run->reported_file_count += 1;
    }
    sxml_validate_clear_errors(run);
}

/* The outcome of a failure that is not the input's: an unreadable file,
   else a failure of sxml. */
static sxml_validate_outcome sxml_validate_failure_outcome(sxml_status status) {
    return status == SXML_STATUS_INPUT_OUTPUT_FAILED ? SXML_VALIDATE_OUTCOME_UNREADABLE
                                                     : SXML_VALIDATE_OUTCOME_FAILED;
}

/* ----------------------------------------------------------- Schema */

/* Reads the SCHEMA documents. Reports each that cannot be read; returns
   false when one could not. */
static bool sxml_validate_read_schema_files(sxml_validate_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_validate_schema_files *files = &run->schema_files;
    bool has_read_all = true;
    for (size_t file_index = 0; file_index < files->file_count; file_index += 1) {
        const sxml_byte_view name = files->names[file_index];
        sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
        sxml_status status = sxml_cli_file_reader_read(
            invocation->file_reader,
            &(sxml_cli_file_read_request){.path = name, .allocator = invocation->allocator},
            &contents);
        if (status == SXML_STATUS_OK && !sxml_byte_view_equals(name, SXML_TEXT("-"))) {
            status = sxml_query_make_file_uri(invocation, name, &files->uris[file_index]);
        }
        files->documents[file_index] = (sxml_schema_document){
            .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
            .document_text = contents,
            .document_node = {.representation = 0},
            .document_uri = files->uris[file_index],
            .checks_target_namespace = false,
            .target_namespace_uri = {.bytes = NULL, .byte_count = 0}};
        if (status != SXML_STATUS_OK) {
            has_read_all = false;
            /* Text reports name the file that cannot be read; JSON
               reports it as the schema's status. */
            if (!run->writes_json) {
                sxml_validate_write_text(
                    run, &(sxml_validate_report){
                             .file = name,
                             .outcome = sxml_validate_failure_outcome(status),
                             .diagnostic = &(sxml_diagnostic){
                                 .description = sxml_status_name(status),
                                 .location = {.source_name = name,
                                              .line_number = 0,
                                              .column_number = 0,
                                              .byte_offset = 0}}});
            }
            if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
                run->has_unreadable_input = true;
            } else {
                run->has_failure = true;
            }
        }
    }
    return has_read_all;
}

/* Compiles the schema and reports it. Returns true when it is valid. */
static bool sxml_validate_compile_schema(sxml_validate_run *run) {
    if (!sxml_validate_read_schema_files(run)) {
        if (run->writes_json) {
            const bool is_unreadable = run->has_unreadable_input;
            run->has_unreadable_input = false;
            run->has_failure = false;
            sxml_validate_submit_report(
                run, &(sxml_validate_report){
                         .file = {.bytes = NULL, .byte_count = 0},
                         .outcome = is_unreadable ? SXML_VALIDATE_OUTCOME_UNREADABLE
                                                  : SXML_VALIDATE_OUTCOME_FAILED,
                         .diagnostic = NULL});
        }
        return false;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_status status = sxml_schema_create(
        run->invocation->allocator,
        &(sxml_schema_configuration){
            .processor = run->query.processor,
            .schema_documents = {.schema_documents = run->schema_files.documents,
                                 .schema_document_count = run->schema_files.file_count},
            .base_uri = {.bytes = NULL, .byte_count = 0},
            .report_sink = {.state = run, .operations = &sxml_validate_report_sink_operations},
            .maximum_content_model_state_count = 0,
            .error_record = &error_record},
        &run->schema);
    sxml_validate_outcome outcome = SXML_VALIDATE_OUTCOME_VALID;
    const sxml_diagnostic *diagnostic = NULL;
    if (status == SXML_STATUS_STATIC_ERROR) {
        outcome = SXML_VALIDATE_OUTCOME_INVALID;
        if (run->error_count == 0) {
            /* An error the sink did not receive: a document that is not a
               schema document at all (schema.h). */
            const sxml_status report_status = sxml_validate_receive_report(
                run, &(sxml_schema_report){.code = error_record.code,
                                           .description = error_record.diagnostic.description,
                                           .location = error_record.diagnostic.location});
            if (report_status != SXML_STATUS_OK) {
                outcome = SXML_VALIDATE_OUTCOME_FAILED;
            }
        }
    } else if (status != SXML_STATUS_OK) {
        outcome = sxml_validate_failure_outcome(status);
        if (error_record.diagnostic.description.byte_count == 0) {
            error_record.diagnostic.description = sxml_status_name(status);
        }
        diagnostic = &error_record.diagnostic;
    }
    sxml_validate_submit_report(
        run, &(sxml_validate_report){.file = {.bytes = NULL, .byte_count = 0},
                                     .outcome = outcome,
                                     .diagnostic = diagnostic});
    return outcome == SXML_VALIDATE_OUTCOME_VALID;
}

/* ------------------------------------------------------------ Files */

/* What the parse of a file and its validation gave. */
typedef struct sxml_validate_parse_result {
    sxml_status status;
    sxml_diagnostic diagnostic;
    sxml_error_record error_record;
    sxml_validation_outcome outcome;
} sxml_validate_parse_result;

/* Parses contents into a new validator. */
static void sxml_validate_parse(sxml_validate_run *run, sxml_byte_view contents,
                                sxml_validate_parse_result *result) {
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    if (!sxml_byte_view_equals(run->file, SXML_TEXT("-"))) {
        result->status = sxml_query_make_file_uri(run->invocation, run->file, &uri);
        if (result->status != SXML_STATUS_OK) {
            return;
        }
    }
    sxml_schema_validator *validator = NULL;
    result->status = sxml_schema_validator_create(
        run->invocation->allocator,
        &(sxml_schema_validator_configuration){
            .schema = run->schema,
            .validation_mode = run->validation_mode,
            .governing_type = {.index = 0},
            .result_builder = {.state = NULL, .operations = NULL},
            .report_sink = {.state = run, .operations = &sxml_validate_report_sink_operations},
            .error_record = &result->error_record},
        &validator);
    if (result->status == SXML_STATUS_OK) {
        result->status = sxml_xml_parser_parse(
            run->parser,
            &(sxml_xml_parse_request){.input = contents,
                                      .source_name = run->file,
                                      .document_uri = uri,
                                      .base_uri = uri,
                                      .encoding_name = {.bytes = NULL, .byte_count = 0},
                                      .diagnostic = &result->diagnostic},
            sxml_schema_validator_as_tree_builder(validator));
    }
    if (result->status == SXML_STATUS_OK) {
        result->status = sxml_schema_validator_get_outcome(validator, &result->outcome);
    }
    sxml_schema_validator_destroy(validator);
    sxml_allocator_release(run->invocation->allocator, (void *)uri.bytes);
}

/* What a validated file's result means. */
static sxml_validate_outcome sxml_validate_outcome_of(sxml_validate_parse_result *result) {
    if (result->status == SXML_STATUS_OK) {
        switch (result->outcome.validity) {
        case SXML_VALIDITY_VALID:
            return SXML_VALIDATE_OUTCOME_VALID;
        case SXML_VALIDITY_NOT_KNOWN:
            return SXML_VALIDATE_OUTCOME_NOT_KNOWN;
        case SXML_VALIDITY_INVALID:
            break;
        }
        return SXML_VALIDATE_OUTCOME_INVALID;
    }
    /* The parser's own failures fill its diagnostic (include/sxml/xml.h);
       the validator's (the result builder of the parse) do not. */
    if (result->diagnostic.description.byte_count > 0) {
        if (result->status == SXML_STATUS_MALFORMED_INPUT ||
            result->status == SXML_STATUS_INVALID_UTF8 ||
            result->status == SXML_STATUS_UNSUPPORTED_FEATURE) {
            return SXML_VALIDATE_OUTCOME_NOT_WELL_FORMED;
        }
        return sxml_validate_failure_outcome(result->status);
    }
    result->diagnostic.location.line_number = 0;
    if (result->status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        /* D162: identity constraints and assertions until packets 5.8 and
           5.9 land. */
        result->diagnostic.description =
            SXML_TEXT("validation needs an XSD 1.1 feature that sxml does not have yet");
    } else if (result->status == SXML_STATUS_LIMIT_EXCEEDED) {
        result->diagnostic.description = SXML_TEXT("a limit was exceeded");
    } else if (result->status == SXML_STATUS_OUT_OF_MEMORY) {
        result->diagnostic.description = SXML_TEXT("out of memory");
    } else {
        result->diagnostic.description = sxml_status_name(result->status);
    }
    return sxml_validate_failure_outcome(result->status);
}

/* Reads, validates and reports one file. */
static void sxml_validate_file(sxml_validate_run *run, sxml_byte_view file) {
    const sxml_allocator allocator = run->invocation->allocator;
    run->file = file;
    sxml_byte_view contents = {.bytes = NULL, .byte_count = 0};
    const sxml_status read_status = sxml_cli_file_reader_read(
        run->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = file, .allocator = allocator}, &contents);
    if (read_status != SXML_STATUS_OK) {
        sxml_validate_submit_report(
            run, &(sxml_validate_report){
                     .file = file,
                     .outcome = sxml_validate_failure_outcome(read_status),
                     .diagnostic = &(sxml_diagnostic){
                         .description = SXML_TEXT("the file cannot be read"),
                         .location = {.source_name = file,
                                      .line_number = 0,
                                      .column_number = 0,
                                      .byte_offset = 0}}});
        return;
    }
    sxml_validate_parse_result result;
    memset(&result, 0, sizeof(result));
    result.diagnostic.location.source_name = file;
    sxml_validate_parse(run, contents, &result);
    const sxml_validate_outcome outcome = sxml_validate_outcome_of(&result);
    sxml_validate_submit_report(run, &(sxml_validate_report){
                                  .file = file,
                                  .outcome = outcome,
                                  .diagnostic = outcome == SXML_VALIDATE_OUTCOME_VALID ||
                                                        outcome == SXML_VALIDATE_OUTCOME_INVALID ||
                                                        outcome == SXML_VALIDATE_OUTCOME_NOT_KNOWN
                                                    ? NULL
                                                    : &result.diagnostic});
    sxml_allocator_release(allocator, (void *)contents.bytes);
}

/* ------------------------------------------------------- The command */

/* Checks what the option table cannot, standard input at most once, and
   fills run->schema_files.names. Returns
   SXML_CLI_EXIT_SUCCESS to go on. */
static sxml_cli_exit_code sxml_validate_read_command_line(sxml_validate_run *run) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    if (sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name = SXML_TEXT("schema"), .occurrence_index = 0},
            &match) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    /* At least one: the option table says so. */
    const size_t schema_count = match.occurrence_count;
    sxml_validate_schema_files *files = &run->schema_files;
    void *name_memory = NULL;
    void *uri_memory = NULL;
    void *document_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        invocation->allocator,
        (sxml_allocation_request){.byte_count = schema_count * sizeof(sxml_byte_view),
                                  .alignment = alignof(sxml_byte_view)},
        &name_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = schema_count * sizeof(sxml_byte_view),
                                      .alignment = alignof(sxml_byte_view)},
            &uri_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            invocation->allocator,
            (sxml_allocation_request){.byte_count = schema_count * sizeof(sxml_schema_document),
                                      .alignment = alignof(sxml_schema_document)},
            &document_memory);
    }
    files->names = name_memory;
    files->uris = uri_memory;
    files->documents = document_memory;
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    memset(uri_memory, 0, schema_count * sizeof(sxml_byte_view));
    memset(document_memory, 0, schema_count * sizeof(sxml_schema_document));
    files->file_count = schema_count;
    size_t standard_input_count = 0;
    for (size_t schema_index = 0; schema_index < schema_count; schema_index += 1) {
        if (sxml_cli_arguments_find_option(
                invocation->arguments,
                (sxml_cli_option_lookup){.long_name = SXML_TEXT("schema"),
                                         .occurrence_index = schema_index},
                &match) != SXML_STATUS_OK) {
            return SXML_CLI_EXIT_INTERNAL_ERROR;
        }
        files->names[schema_index] = match.value;
        standard_input_count += sxml_byte_view_equals(match.value, SXML_TEXT("-")) ? 1 : 0;
    }
    const size_t file_count = sxml_cli_arguments_positional_count(invocation->arguments);
    for (size_t file_index = 0; file_index < file_count; file_index += 1) {
        sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
        if (sxml_cli_arguments_get_positional(invocation->arguments, file_index, &file) ==
                SXML_STATUS_OK &&
            sxml_byte_view_equals(file, SXML_TEXT("-"))) {
            standard_input_count += 1;
        }
    }
    if (standard_input_count > 1) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("standard input can be read once: give - at most once"));
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* Reads the choice option long_name; true when its value is choice. */
static bool sxml_validate_option_is(const sxml_cli_invocation *invocation,
                                    sxml_byte_view long_name, sxml_byte_view choice) {
    sxml_cli_option_match match = {.is_present = false, .occurrence_count = 0};
    return sxml_cli_arguments_find_option(
               invocation->arguments,
               (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
               &match) == SXML_STATUS_OK &&
           sxml_byte_view_equals(match.value, choice);
}

/* Creates the arena of the JSON errors and the parser. */
static sxml_status sxml_validate_start(sxml_validate_run *run) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_status status = sxml_arena_create(
        allocator,
        &(sxml_arena_configuration){.block_allocator = allocator,
                                    .block_byte_count = SXML_VALIDATE_ERROR_BLOCK_BYTE_COUNT,
                                    .maximum_block_byte_count = 0,
                                    .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &run->error_arena);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){
                .allocator = allocator,
                .name_pool = sxml_processor_get_name_pool(run->query.processor),
                .limits = {.maximum_depth = 0,
                           .maximum_entity_expansion_byte_count =
                               SXML_VALIDATE_MAXIMUM_EXPANSION_BYTE_COUNT}},
            &run->parser);
    }
    return status;
}

/* Releases what the run holds. */
static void sxml_validate_finish(sxml_validate_run *run) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_xml_parser_destroy(run->parser);
    sxml_schema_destroy(run->schema);
    sxml_arena_destroy(run->error_arena);
    sxml_validate_schema_files *files = &run->schema_files;
    for (size_t file_index = 0; file_index < files->file_count; file_index += 1) {
        sxml_allocator_release(allocator, (void *)files->uris[file_index].bytes);
        sxml_allocator_release(allocator,
                               (void *)files->documents[file_index].document_text.bytes);
    }
    sxml_allocator_release(allocator, files->names);
    sxml_allocator_release(allocator, files->uris);
    sxml_allocator_release(allocator, files->documents);
    (void)sxml_query_run_finish(&run->query, SXML_CLI_EXIT_SUCCESS);
}

static sxml_cli_exit_code sxml_run_validate_command(const sxml_cli_invocation *invocation) {
    sxml_validate_run run;
    memset(&run, 0, sizeof(run));
    run.invocation = invocation;
    run.query.invocation = invocation;
    run.query.command_name = SXML_TEXT("validate");
    run.query.parameter_binding = SXML_PARAMETER_BINDING_NONE;
    run.writes_json = sxml_validate_option_is(invocation, SXML_TEXT("format"), SXML_TEXT("json"));
    run.validation_mode =
        sxml_validate_option_is(invocation, SXML_TEXT("mode"), SXML_TEXT("lax"))
            ? SXML_VALIDATION_MODE_LAX
            : SXML_VALIDATION_MODE_STRICT;
    sxml_cli_exit_code exit_code = sxml_validate_read_command_line(&run);
    if (exit_code == SXML_CLI_EXIT_SUCCESS) {
        exit_code = sxml_query_run_start(&run.query);
    }
    if (exit_code == SXML_CLI_EXIT_SUCCESS && sxml_validate_start(&run) != SXML_STATUS_OK) {
        exit_code = SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        sxml_validate_finish(&run);
        return exit_code;
    }
    if (sxml_validate_compile_schema(&run)) {
        const size_t file_count = sxml_cli_arguments_positional_count(invocation->arguments);
        for (size_t file_index = 0; file_index < file_count; file_index += 1) {
            sxml_byte_view file = {.bytes = NULL, .byte_count = 0};
            if (sxml_cli_arguments_get_positional(invocation->arguments, file_index, &file) ==
                SXML_STATUS_OK) {
                sxml_validate_file(&run, file);
            }
        }
    }
    if (run.writes_json) {
        sxml_cli_writer_write_line(invocation->standard_output, SXML_TEXT("]}"));
    }
    sxml_validate_finish(&run);
    if (run.has_unreadable_input) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (run.has_failure) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    return run.has_invalid_input ? SXML_CLI_EXIT_OPERATION_FAILED : SXML_CLI_EXIT_SUCCESS;
}

const sxml_cli_command sxml_validate_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("validate"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Validate XML documents against an XSD 1.1 schema"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Validates each FILE against the XSD 1.1 schema that the SCHEMA\n"
        "documents form, together with every document they include, import,\n"
        "redefine or override (local files, relative to the document that\n"
        "refers to them). Each FILE is parsed and validated in one pass; no\n"
        "document is written. xsi:schemaLocation hints in a FILE are not\n"
        "followed: the schema is the one --schema names.\n"
        "\n"
        "The schema is compiled first. Each of its errors is reported on\n"
        "standard error as\n"
        "  SCHEMA:LINE:COLUMN: CONSTRAINT: description\n"
        "and then no FILE is read. Prints nothing when every FILE is valid.\n"
        "Each error in a FILE is reported as\n"
        "  FILE:LINE:COLUMN: CONSTRAINT: description\n"
        "every error, not only the first. CONSTRAINT is the name XSD 1.1\n"
        "gives the rule, such as cvc-complex-type.2.4. A FILE that is not\n"
        "well-formed is reported as 'sxml check' reports it.\n"
        "\n"
        "With --mode=strict (the default) the document element of each FILE\n"
        "must have a global declaration in the schema. With --mode=lax a\n"
        "document element without one is assessed laxly: what has a\n"
        "declaration is validated, and the validity of the FILE is \"not\n"
        "known\": it is reported, but does not fail. With --format=json, one\n"
        "JSON object on standard output lists the schema and every FILE\n"
        "with its \"status\" and \"errors\".\n"
        "\n"
        "Exit status: 0 when every FILE is valid (or not known, with\n"
        "--mode=lax); 1 when the schema or a FILE is invalid or a FILE is not\n"
        "well-formed; 3 when a SCHEMA or a FILE cannot be read; 4 when a\n"
        "limit is exceeded or validation needs a feature sxml does not have\n"
        "yet."),
    .options = sxml_validate_options,
    .option_count = sizeof sxml_validate_options / sizeof sxml_validate_options[0],
    .positionals = sxml_validate_positionals,
    .positional_count = sizeof sxml_validate_positionals / sizeof sxml_validate_positionals[0],
    .examples = {.texts = sxml_validate_examples,
                 .text_count = sizeof sxml_validate_examples / sizeof sxml_validate_examples[0]},
    .run = sxml_run_validate_command,
};
