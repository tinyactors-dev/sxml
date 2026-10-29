/*
 * sxml-schematron-test: the Schematron test driver (packet 7.28; D315,
 * D323), a developer tool, not shipped. It runs sxml's Schematron
 * (include/sxml/schematron.h) over
 *
 *   run [--suite=DIR] [--set=NAME] [--case=NAME] [--deviations=FILE]
 *       the conformance suite of tests/schematron (README.md there): each
 *       case compiled, validated with SVRL, and compared with its expected
 *       result by compare.xq; one line per case, a summary, exit status 1
 *       when a case fails that the deviation list does not name;
 *   vefa --schematron=FILE [--quiet] TESTFILE...
 *       the VEFA unit tests of EN 16931 (src/test): each test's document
 *       validated, each expected rule id counted (success: must not fire;
 *       error, warning, fatal: must fire, NUMBER times when given); one
 *       line per mismatch, a summary with the checks and the time;
 *   report --schematron=FILE [--phase=PHASE] [--fired] [--svrl] INSTANCE...
 *       the findings (and with --fired the fired rules) of each INSTANCE,
 *       one line each, for the differential test against SchXslt
 *       (tests/schematron/driver/differential.py); with --svrl the SVRL.
 *
 * The suite's cases and the VEFA tests are read by XQuery (cases.xq,
 * compare.xq, vefa.xq beside this file, read from the suite's driver/
 * directory): the driver is the glue between them and the C API.
 * Golden file: tests/xsd/driver (a sibling driver); this one is plain
 * stdio, as a test tool.
 */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700 /* NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) */
#endif

#include "sxml/processor.h"
#include "sxml/schematron.h"
#include "sxml/xml.h"
#include "sxml/xquery.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SVRL_NAMESPACE "http://purl.oclc.org/dsdl/svrl"

/* ----------------------------------------------------------- Memory */

/* Memory from the default allocator; the driver stops when there is
   none. */
static void *allocate_bytes(size_t byte_count) {
    void *memory = NULL;
    if (sxml_allocator_allocate(sxml_default_allocator(),
                                (sxml_allocation_request){.byte_count = byte_count == 0 ? 1 : byte_count,
                                                          .alignment = alignof(max_align_t)},
                                &memory) != SXML_STATUS_OK) {
        (void)fputs("sxml-schematron-test: out of memory\n", stderr);
        exit(4);
    }
    return memory;
}

static void release_bytes(const void *memory) {
    sxml_allocator_release(sxml_default_allocator(), (void *)memory);
}

/* ------------------------------------------------------------- Text */

/* A growing text in malloc memory. */
typedef struct text_buffer {
    char *bytes;
    size_t byte_count;
    size_t capacity;
} text_buffer;

static void text_append(text_buffer *buffer, sxml_byte_view text) {
    if (buffer->bytes == NULL || buffer->byte_count + text.byte_count + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity < 256 ? 256 : buffer->capacity;
        while (buffer->byte_count + text.byte_count + 1 > capacity) {
            capacity *= 2;
        }
        char *bytes = allocate_bytes(capacity);
        if (buffer->byte_count > 0) {
            memcpy(bytes, buffer->bytes, buffer->byte_count);
        }
        release_bytes(buffer->bytes);
        buffer->bytes = bytes;
        buffer->capacity = capacity;
    }
    char *bytes = buffer->bytes;
    if (bytes == NULL) {
        return;
    }
    if (text.byte_count > 0 && text.bytes != NULL) {
        memcpy(bytes + buffer->byte_count, text.bytes, text.byte_count);
    }
    buffer->byte_count += text.byte_count;
    bytes[buffer->byte_count] = '\0';
}

static void text_append_c(text_buffer *buffer, const char *text) {
    text_append(buffer, sxml_byte_view_from_c_string(text));
}

/* An XQuery string literal of text: quotes doubled, "&" as "&amp;". */
static void text_append_literal(text_buffer *buffer, sxml_byte_view text) {
    text_append_c(buffer, "\"");
    for (size_t position = 0; position < text.byte_count; position += 1) {
        const char character = (char)text.bytes[position];
        if (character == '"') {
            text_append_c(buffer, "\"\"");
        } else if (character == '&') {
            text_append_c(buffer, "&amp;");
        } else {
            text_append(buffer, (sxml_byte_view){.bytes = text.bytes + position, .byte_count = 1});
        }
    }
    text_append_c(buffer, "\"");
}

static void text_clear(text_buffer *buffer) {
    buffer->byte_count = 0;
    if (buffer->bytes != NULL) {
        buffer->bytes[0] = '\0';
    }
}

static sxml_byte_view text_view(const text_buffer *buffer) {
    return (sxml_byte_view){.bytes = (const uint8_t *)buffer->bytes,
                            .byte_count = buffer->byte_count};
}

static void print_view(FILE *stream, sxml_byte_view text) {
    if (text.byte_count > 0) {
        (void)fwrite(text.bytes, 1, text.byte_count, stream);
    }
}

/* The whole of a file; false when it cannot be read. */
static bool read_file(const char *path, text_buffer *contents) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    text_clear(contents);
    /* Small: WASI's default stack is 64 KB, and the wasm build runs this
       driver too. */
    char block[4096];
    size_t count = 0;
    while ((count = fread(block, 1, sizeof block, file)) > 0) {
        text_append(contents, (sxml_byte_view){.bytes = (const uint8_t *)block, .byte_count = count});
    }
    const bool has_failed = ferror(file) != 0;
    (void)fclose(file);
    return !has_failed;
}

/* file://PATH as a path: the scheme dropped, %XX decoded. */
static bool path_of_uri(sxml_byte_view uri, text_buffer *path) {
    static const char scheme[] = "file://";
    if (uri.byte_count < sizeof scheme - 1 || memcmp(uri.bytes, scheme, sizeof scheme - 1) != 0) {
        return false;
    }
    text_clear(path);
    for (size_t position = sizeof scheme - 1; position < uri.byte_count; position += 1) {
        char character = (char)uri.bytes[position];
        if (character == '#') {
            break;
        }
        if (character == '%' && position + 2 < uri.byte_count) {
            const char digits[3] = {(char)uri.bytes[position + 1], (char)uri.bytes[position + 2], 0};
            character = (char)strtol(digits, NULL, 16);
            position += 2;
        }
        text_append(path, (sxml_byte_view){.bytes = (const uint8_t *)&character, .byte_count = 1});
    }
    return true;
}

enum { SXML_DRIVER_PATH_BYTE_COUNT = 4096 };

/* The absolute form of a path in absolute; NULL when unknown. */
static const char *absolute_path(const char *path, char absolute[SXML_DRIVER_PATH_BYTE_COUNT]) {
#if defined(_WIN32)
    return _fullpath(absolute, path, SXML_DRIVER_PATH_BYTE_COUNT);
#else
    return realpath(path, absolute);
#endif
}

/* A path, and whether it names a directory (its URI ends in "/"). */
typedef struct path_request {
    const char *path;
    bool is_directory;
} path_request;

/* The file URI of a path: absolute, "/" added for a directory. */
static void uri_of_path(path_request request, text_buffer *uri) {
    const char *path = request.path;
    const bool is_directory = request.is_directory;
    char absolute_bytes[SXML_DRIVER_PATH_BYTE_COUNT];
    const char *absolute = absolute_path(path, absolute_bytes);
    text_clear(uri);
    text_append_c(uri, "file://");
    text_append_c(uri, absolute != NULL ? absolute : path);
    if (is_directory) {
        text_append_c(uri, "/");
    }
}

/* -------------------------------------------------------- Resolver */

/* Local files for file:// URIs: sch:include, sch:extends href, fn:doc. */
typedef struct file_resolver {
    text_buffer path;
} file_resolver;

static sxml_status resolve_file(void *state, const sxml_resource_request *request,
                                sxml_resource *resource) {
    file_resolver *resolver = state;
    if (request->resource_kind != SXML_RESOURCE_KIND_XML_DOCUMENT &&
        request->resource_kind != SXML_RESOURCE_KIND_TEXT &&
        request->resource_kind != SXML_RESOURCE_KIND_BINARY &&
        request->resource_kind != SXML_RESOURCE_KIND_XSLT_MODULE &&
        request->resource_kind != SXML_RESOURCE_KIND_SCHEMA_DOCUMENT) {
        return SXML_STATUS_OK;
    }
    if (!path_of_uri(request->absolute_uri, &resolver->path)) {
        return SXML_STATUS_OK;
    }
    text_buffer contents = {0};
    if (!read_file(resolver->path.bytes, &contents)) {
        release_bytes(contents.bytes);
        return SXML_STATUS_OK;
    }
    resource->resource_form = SXML_RESOURCE_FORM_BYTES;
    resource->content = text_view(&contents);
    if (contents.bytes == NULL) {
        resource->content = (sxml_byte_view){.bytes = (const uint8_t *)"", .byte_count = 0};
    }
    return SXML_STATUS_OK;
}

static void release_file(void *state, const sxml_resource *resource) {
    (void)state;
    if (resource->resource_form == SXML_RESOURCE_FORM_BYTES && resource->content.byte_count > 0) {
        release_bytes((void *)resource->content.bytes);
    }
}

static const sxml_resource_resolver_operations file_resolver_operations = {
    .resolve = resolve_file, .release = release_file};

/* ---------------------------------------------------------- Driver */

typedef struct suite_counts {
    size_t pass_count;
    size_t fail_count;
    size_t deviation_count;
    size_t implementation_defined_count;
    size_t refused_count;
} suite_counts;

typedef struct driver {
    sxml_processor *processor;
    file_resolver resolver;
    /* The suite directory's file URI, ending in "/". */
    text_buffer suite_uri;
    /* The suite directory as given. */
    const char *suite_directory;
    /* Scratch texts. */
    text_buffer query;
    text_buffer svrl;
    /* run: the deviation list and the counts. */
    text_buffer deviations;
    suite_counts counts;
    text_buffer schema_text;
    text_buffer instance_text;
} driver;

static void print_record(FILE *stream, const sxml_error_record *record) {
    print_view(stream, record->code.namespace_prefix);
    if (record->code.namespace_prefix.byte_count > 0) {
        (void)fputc(':', stream);
    }
    print_view(stream, record->code.local_name);
    (void)fputs(": ", stream);
    print_view(stream, record->diagnostic.description);
    if (record->diagnostic.location.line_number > 0) {
        (void)fprintf(stream, " (line %u, column %u)", record->diagnostic.location.line_number,
                record->diagnostic.location.column_number);
    }
}

/* Compiles and evaluates query text with the context value context (the
   empty sequence for none); *result is owned. */
typedef struct query_run {
    sxml_byte_view text;
    sxml_sequence context;
    bool has_context;
} query_run;

static sxml_status evaluate_query(driver *state, const query_run *run, sxml_sequence *result) {
    sxml_error_record record;
    memset(&record, 0, sizeof record);
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        sxml_default_allocator(),
        &(sxml_xquery_query_configuration){
            .processor = state->processor,
            .expression_text = run->text,
            .source_name = sxml_byte_view_from_c_string("driver query"),
            .static_context = {.static_base_uri = text_view(&state->suite_uri)},
            .error_record = &record},
        &query);
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_query_evaluate(
            query,
            &(sxml_xquery_evaluation_request){
                .dynamic_context = {.has_context_value = run->has_context,
                                    .context_value = run->context,
                                    .resource_resolver = {.state = &state->resolver,
                                                          .operations = &file_resolver_operations}},
                .error_record = &record},
            result);
        sxml_xquery_query_destroy(query);
    }
    if (status != SXML_STATUS_OK) {
        (void)fputs("sxml-schematron-test: query failed: ", stderr);
        print_record(stderr, &record);
        (void)fputc('\n', stderr);
    }
    return status;
}

/* The string value of the item at index of a sequence of strings; empty
   for another item. Borrowed from the sequence. */
static sxml_byte_view string_at(sxml_sequence sequence, uint64_t index) {
    sxml_item item;
    if (sxml_sequence_get_item(sequence, (sxml_item_request){.item_index = index}, &item) !=
        SXML_STATUS_OK) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return datum.datum_details.string.utf8;
}

static uint64_t count_of(sxml_sequence sequence) {
    uint64_t item_count = 0;
    if (sxml_sequence_count(sequence, NULL, &item_count) != SXML_STATUS_OK) {
        return 0;
    }
    return item_count;
}

/* The query file NAME of the driver directory, then call appended by the
   caller. */
static bool load_query(driver *state, const char *name) {
    text_buffer path = {0};
    text_append_c(&path, state->suite_directory);
    text_append_c(&path, "/driver/");
    text_append_c(&path, name);
    const bool has_read = read_file(path.bytes, &state->query);
    if (!has_read) {
        (void)fprintf(stderr, "sxml-schematron-test: cannot read %s\n", path.bytes);
    }
    release_bytes(path.bytes);
    return has_read;
}

static sxml_status write_text(void *state, sxml_byte_view bytes) {
    text_append(state, bytes);
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations text_stream_operations = {.write = write_text};

/* A document's text and its URI. */
typedef struct document_text {
    sxml_byte_view text;
    sxml_byte_view uri;
} document_text;

/* Parses a text as a document with its URI. */
static sxml_status parse_document(driver *state, document_text source, sxml_item *document) {
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof diagnostic);
    return sxml_processor_parse_document(state->processor,
                                         &(sxml_xml_parse_request){.input = source.text,
                                                                   .source_name = source.uri,
                                                                   .document_uri = source.uri,
                                                                   .base_uri = source.uri,
                                                                   .diagnostic = &diagnostic},
                                         document);
}

static bool create_driver(driver *state) {
    const sxml_status status = sxml_processor_create(
        sxml_default_allocator(),
        &(sxml_processor_configuration){
            .allocator = sxml_default_allocator(),
            .resource_resolver = {.state = &state->resolver,
                                  .operations = &file_resolver_operations}},
        &state->processor);
    if (status != SXML_STATUS_OK) {
        (void)fputs("sxml-schematron-test: cannot make a processor\n", stderr);
        return false;
    }
    return true;
}

static void destroy_driver(driver *state) {
    sxml_processor_destroy(state->processor);
    release_bytes(state->resolver.path.bytes);
    release_bytes(state->suite_uri.bytes);
    release_bytes(state->query.bytes);
    release_bytes(state->svrl.bytes);
    release_bytes(state->deviations.bytes);
    release_bytes(state->schema_text.bytes);
    release_bytes(state->instance_text.bytes);
}

/* A schematron compiled from a file; NULL with *record set when it
   fails. */
typedef struct compile_request {
    sxml_byte_view schema_uri;
    sxml_byte_view phase_id;
    sxml_schematron_parameter_view parameters;
    /* Every compile error is printed to standard error. */
    bool prints_errors;
} compile_request;

/* Counts compile errors: the driver compiles with a sink, so that
   compilation goes on after the first (D315), on every case. */
/* The errors of one compile: counted, and printed for report and vefa
   (each on its own line, as "URI:LINE:COLUMN: CODE: description"). */
typedef struct compile_errors {
    size_t error_count;
    bool prints_errors;
} compile_errors;

static sxml_status count_compile_error(void *state, const sxml_schema_report *report) {
    compile_errors *errors = state;
    /* schematron:compatibility is a warning (D331). */
    const bool is_warning =
        sxml_byte_view_equals(report->code.local_name, sxml_byte_view_from_c_string("compatibility")) &&
        sxml_byte_view_equals(report->code.namespace_uri,
                              sxml_byte_view_from_c_string("urn:sxml:schematron"));
    if (!is_warning) {
        errors->error_count += 1;
    }
    if (errors->prints_errors) {
        (void)fprintf(stderr, "%.*s:%u:%u: %s%.*s:%.*s: %.*s\n",
                      (int)report->location.source_name.byte_count,
                      (const char *)report->location.source_name.bytes,
                      report->location.line_number, report->location.column_number,
                      is_warning ? "warning: " : "",
                      (int)report->code.namespace_prefix.byte_count,
                      (const char *)report->code.namespace_prefix.bytes,
                      (int)report->code.local_name.byte_count,
                      (const char *)report->code.local_name.bytes,
                      (int)report->description.byte_count,
                      (const char *)report->description.bytes);
    }
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations compile_sink_operations = {
    .receive = count_compile_error};

static sxml_schematron *compile_schema(driver *state, const compile_request *request,
                                       sxml_error_record *record) {
    compile_errors errors = {.error_count = 0, .prints_errors = request->prints_errors};
    text_buffer path = {0};
    sxml_schematron *schematron = NULL;
    memset(record, 0, sizeof *record);
    if (!path_of_uri(request->schema_uri, &path) || !read_file(path.bytes, &state->schema_text)) {
        record->code.local_name = sxml_byte_view_from_c_string("unreadable");
        release_bytes(path.bytes);
        return NULL;
    }
    release_bytes(path.bytes);
    const sxml_status status = sxml_schematron_create(
        sxml_default_allocator(),
        &(sxml_schematron_configuration){
            .processor = state->processor,
            .document_source = SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT,
            .document_text = text_view(&state->schema_text),
            .document_uri = request->schema_uri,
            .phase_id = request->phase_id,
            .parameters = request->parameters,
            .report_sink = {.state = &errors, .operations = &compile_sink_operations},
            .error_record = record},
        &schematron);
    if (status != SXML_STATUS_OK) {
        if (record->code.local_name.byte_count == 0) {
            record->code.local_name = sxml_status_name(status);
        }
        return NULL;
    }
    return schematron;
}

/* ------------------------------------------------------- run: suite */

enum { SXML_DRIVER_MAXIMUM_FIELD_COUNT = 64 };

/* The tab-separated fields of a line of cases.xq. */
typedef struct case_fields {
    sxml_byte_view fields[SXML_DRIVER_MAXIMUM_FIELD_COUNT];
    size_t field_count;
} case_fields;

static void split_fields(sxml_byte_view line, case_fields *fields) {
    fields->field_count = 0;
    size_t start = 0;
    for (size_t position = 0; position <= line.byte_count; position += 1) {
        if (position == line.byte_count || line.bytes[position] == '\t') {
            if (fields->field_count < SXML_DRIVER_MAXIMUM_FIELD_COUNT) {
                fields->fields[fields->field_count] =
                    (sxml_byte_view){.bytes = line.bytes + start, .byte_count = position - start};
                fields->field_count += 1;
            }
            start = position + 1;
        }
    }
}

enum {
    SXML_DRIVER_FIELD_SET,
    SXML_DRIVER_FIELD_CASE,
    SXML_DRIVER_FIELD_SCHEMA,
    SXML_DRIVER_FIELD_INSTANCE,
    SXML_DRIVER_FIELD_PHASE,
    SXML_DRIVER_FIELD_APPLICABILITY,
    SXML_DRIVER_FIELD_FIRST_PARAMETER
};

/* The deviation list: lines "CASE RECORD REASON..."; # starts a comment. */
static bool is_deviation(const text_buffer *deviations, sxml_byte_view case_name) {
    const char *line = deviations->bytes;
    while (line != NULL && *line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t line_length = end != NULL ? (size_t)(end - line) : strlen(line);
        if (line[0] != '#' && line_length > case_name.byte_count &&
            memcmp(line, case_name.bytes, case_name.byte_count) == 0 &&
            line[case_name.byte_count] == ' ') {
            return true;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}

/* What happened to a case: the outcome for compare.xq and its detail. */
typedef struct case_outcome {
    const char *outcome;
    text_buffer detail;
    sxml_item svrl_document;
    bool has_svrl_document;
    /* compare.xq's verdict. */
    text_buffer verdict;
} case_outcome;

static void record_detail(case_outcome *outcome, const sxml_error_record *record) {
    text_clear(&outcome->detail);
    text_append(&outcome->detail, record->code.local_name);
    if (record->diagnostic.description.byte_count > 0) {
        text_append_c(&outcome->detail, " ");
        text_append(&outcome->detail, record->diagnostic.description);
    }
    for (size_t position = 0; position < outcome->detail.byte_count; position += 1) {
        if (outcome->detail.bytes[position] == '\n' || outcome->detail.bytes[position] == '\t') {
            outcome->detail.bytes[position] = ' ';
        }
    }
}

/* Compiles and validates one case into *outcome. */
static void run_case(driver *state, const case_fields *fields, case_outcome *outcome) {
    sxml_schematron_parameter parameters[SXML_DRIVER_MAXIMUM_FIELD_COUNT / 2];
    size_t parameter_count = 0;
    for (size_t index = SXML_DRIVER_FIELD_FIRST_PARAMETER; index + 1 < fields->field_count; index += 2) {
        parameters[parameter_count] = (sxml_schematron_parameter){
            .name = fields->fields[index], .value = fields->fields[index + 1]};
        parameter_count += 1;
    }
    sxml_error_record record;
    sxml_schematron *schematron = compile_schema(
        state,
        &(compile_request){.schema_uri = fields->fields[SXML_DRIVER_FIELD_SCHEMA],
                           .phase_id = fields->fields[SXML_DRIVER_FIELD_PHASE],
                           .parameters = {.parameters = parameters,
                                          .parameter_count = parameter_count}},
        &record);
    if (schematron == NULL) {
        outcome->outcome = "compile-failed";
        record_detail(outcome, &record);
        return;
    }
    text_buffer path = {0};
    sxml_item instance;
    memset(&instance, 0, sizeof instance);
    if (!path_of_uri(fields->fields[SXML_DRIVER_FIELD_INSTANCE], &path) ||
        !read_file(path.bytes, &state->instance_text) ||
        parse_document(state, (document_text){.text = text_view(&state->instance_text), .uri = fields->fields[SXML_DRIVER_FIELD_INSTANCE]}, &instance) != SXML_STATUS_OK) {
        outcome->outcome = "instance-unreadable";
        text_clear(&outcome->detail);
        release_bytes(path.bytes);
        sxml_schematron_destroy(schematron);
        return;
    }
    release_bytes(path.bytes);
    text_clear(&state->svrl);
    memset(&record, 0, sizeof record);
    sxml_schematron_outcome validation;
    const sxml_status status = sxml_schematron_validate_node(
        schematron,
        &(sxml_schematron_validation_request){
            .node = instance,
            .svrl_output_stream = {.state = &state->svrl, .operations = &text_stream_operations},
            .error_record = &record},
        &validation);
    sxml_item_release(instance);
    sxml_schematron_destroy(schematron);
    if (status != SXML_STATUS_OK) {
        outcome->outcome = "validate-failed";
        if (record.code.local_name.byte_count == 0) {
            record.code.local_name = sxml_status_name(status);
        }
        record_detail(outcome, &record);
        return;
    }
    outcome->outcome = "ran";
    text_clear(&outcome->detail);
    if (parse_document(state,
                       (document_text){.text = text_view(&state->svrl),
                                       .uri = sxml_byte_view_from_c_string("file:///svrl.xml")},
                       &outcome->svrl_document) != SXML_STATUS_OK) {
        outcome->outcome = "svrl-not-well-formed";
        return;
    }
    outcome->has_svrl_document = true;
}

/* Evaluates the assert-svrl expressions items[1...] of a verdict over the
   SVRL; the first false one is written to *failed (empty when all
   hold). */
/* A verdict of compare.xq and the SVRL it holds for. */
typedef struct svrl_check {
    sxml_sequence verdict;
    sxml_item svrl_document;
} svrl_check;

static void check_svrl_assertions(driver *state, svrl_check check, text_buffer *failed) {
    const sxml_sequence verdict = check.verdict;
    const sxml_item svrl_document = check.svrl_document;
    text_clear(failed);
    const uint64_t item_count = count_of(verdict);
    for (uint64_t index = 1; index < item_count && failed->byte_count == 0; index += 1) {
        const sxml_byte_view expression = string_at(verdict, index);
        text_buffer query = {0};
        text_append_c(&query, "declare namespace svrl = \"" SVRL_NAMESPACE "\"; boolean(");
        text_append(&query, expression);
        text_append_c(&query, ")");
        sxml_sequence result = sxml_sequence_empty();
        const sxml_status status = evaluate_query(
            state,
            &(query_run){.text = text_view(&query),
                         .context = sxml_sequence_from_item(svrl_document),
                         .has_context = true},
            &result);
        bool holds = false;
        sxml_item item;
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK && count_of(result) == 1 &&
            sxml_sequence_get_item(result, (sxml_item_request){.item_index = 0}, &item) ==
                SXML_STATUS_OK &&
            sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
            datum.datum_kind == SXML_DATUM_KIND_BOOLEAN) {
            holds = datum.datum_details.boolean;
        }
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(result);
        }
        if (!holds) {
            text_append_c(failed, "fail: SVRL assertion false: ");
            text_append(failed, expression);
        }
        release_bytes(query.bytes);
    }
}

/* Compares one case and prints its line. Returns false when the driver
   cannot go on. */
static bool compare_case(driver *state, const case_fields *fields, case_outcome *outcome) {
    text_buffer *verdict_text = &outcome->verdict;
    text_buffer *query = &state->query;
    if (!load_query(state, "compare.xq")) {
        return false;
    }
    text_append_c(query, "\nlocal:compare(");
    text_append_literal(query, text_view(&state->suite_uri));
    text_append_c(query, ", ");
    text_append_literal(query, fields->fields[SXML_DRIVER_FIELD_SET]);
    text_append_c(query, ", ");
    text_append_literal(query, fields->fields[SXML_DRIVER_FIELD_CASE]);
    text_append_c(query, ", ");
    text_append_literal(query, fields->fields[SXML_DRIVER_FIELD_INSTANCE]);
    text_append_c(query, ", ");
    text_append_literal(query, sxml_byte_view_from_c_string(outcome->outcome));
    text_append_c(query, ", ");
    text_append_literal(query, text_view(&outcome->detail));
    text_append_c(query, outcome->has_svrl_document ? ", .)" : ", ())");
    sxml_sequence verdict = sxml_sequence_empty();
    sxml_status status = evaluate_query(
        state,
        &(query_run){.text = text_view(query),
                     .context = outcome->has_svrl_document
                                    ? sxml_sequence_from_item(outcome->svrl_document)
                                    : sxml_sequence_empty(),
                     .has_context = outcome->has_svrl_document},
        &verdict);
    if (status != SXML_STATUS_OK) {
        return false;
    }
    text_clear(verdict_text);
    text_append(verdict_text, string_at(verdict, 0));
    if (strcmp(verdict_text->bytes, "pass") == 0 && outcome->has_svrl_document) {
        text_buffer failed = {0};
        check_svrl_assertions(
            state, (svrl_check){.verdict = verdict, .svrl_document = outcome->svrl_document},
            &failed);
        if (failed.byte_count > 0) {
            text_clear(verdict_text);
            text_append(verdict_text, text_view(&failed));
        }
        release_bytes(failed.bytes);
    }
    sxml_sequence_release(verdict);
    return true;
}

typedef struct run_options {
    const char *set_name;
    const char *case_name;
    const char *deviations_path;
} run_options;

static void print_case_line(const char *verdict, const case_fields *fields, const char *detail) {
    (void)printf("%-22s %-22.*s %.*s", verdict, (int)fields->fields[SXML_DRIVER_FIELD_SET].byte_count,
           (const char *)fields->fields[SXML_DRIVER_FIELD_SET].bytes,
           (int)fields->fields[SXML_DRIVER_FIELD_CASE].byte_count,
           (const char *)fields->fields[SXML_DRIVER_FIELD_CASE].bytes);
    if (detail != NULL && detail[0] != '\0') {
        (void)printf(" -- %.400s", detail);
    }
    (void)putchar('\n');
}

/* One line of cases.xq, run, compared and counted. */
static bool run_suite_case(driver *state, sxml_byte_view line) {
    const text_buffer *deviations = &state->deviations;
    suite_counts *counts = &state->counts;
    case_fields fields;
    split_fields(line, &fields);
    if (fields.field_count < SXML_DRIVER_FIELD_FIRST_PARAMETER) {
        (void)fputs("sxml-schematron-test: a malformed case line\n", stderr);
        return false;
    }
    case_outcome outcome;
    memset(&outcome, 0, sizeof outcome);
    run_case(state, &fields, &outcome);
    const sxml_byte_view applicability = fields.fields[SXML_DRIVER_FIELD_APPLICABILITY];
    bool can_go_on = true;
    if (applicability.byte_count > 3 && memcmp(applicability.bytes, "run", 3) != 0) {
        /* A binding sxml does not read: the compile must refuse it. */
        text_buffer detail = {0};
        text_append(&detail, applicability);
        text_append_c(&detail, "; ");
        text_append_c(&detail, outcome.outcome);
        const bool is_refused = strcmp(outcome.outcome, "compile-failed") == 0;
        print_case_line(is_refused ? "not applicable" : "fail", &fields, detail.bytes);
        if (is_refused) {
            counts->refused_count += 1;
        } else {
            counts->fail_count += 1;
        }
        release_bytes(detail.bytes);
    } else {
        const text_buffer *verdict = &outcome.verdict;
        can_go_on = compare_case(state, &fields, &outcome);
        if (can_go_on) {
            const bool is_pass = strcmp(verdict->bytes, "pass") == 0;
            const bool is_open = strncmp(verdict->bytes, "implementation-defined", 22) == 0;
            const bool is_listed = is_deviation(deviations, fields.fields[SXML_DRIVER_FIELD_CASE]);
            if (is_pass) {
                counts->pass_count += 1;
                print_case_line(is_listed ? "pass (listed)" : "pass", &fields, NULL);
            } else if (is_open) {
                counts->implementation_defined_count += 1;
                print_case_line("implementation-defined", &fields, verdict->bytes + 24);
            } else if (is_listed) {
                counts->deviation_count += 1;
                print_case_line("deviation", &fields, verdict->bytes);
            } else {
                counts->fail_count += 1;
                print_case_line("fail", &fields, verdict->bytes);
            }
        }
    }
    if (outcome.has_svrl_document) {
        sxml_item_release(outcome.svrl_document);
    }
    release_bytes(outcome.detail.bytes);
    release_bytes(outcome.verdict.bytes);
    (void)fflush(stdout);
    return can_go_on;
}

static int run_suite(driver *state, const run_options *options) {
    text_buffer *deviations = &state->deviations;
    if (options->deviations_path != NULL && !read_file(options->deviations_path, deviations)) {
        (void)fprintf(stderr, "sxml-schematron-test: cannot read %s\n", options->deviations_path);
        return 3;
    }
    if (!load_query(state, "cases.xq")) {
        return 3;
    }
    text_append_c(&state->query, "\nlocal:cases(");
    text_append_literal(&state->query, text_view(&state->suite_uri));
    text_append_c(&state->query, ", ");
    text_append_literal(&state->query, sxml_byte_view_from_c_string(options->set_name));
    text_append_c(&state->query, ", ");
    text_append_literal(&state->query, sxml_byte_view_from_c_string(options->case_name));
    text_append_c(&state->query, ")");
    sxml_sequence lines = sxml_sequence_empty();
    if (evaluate_query(state, &(query_run){.text = text_view(&state->query)}, &lines) !=
        SXML_STATUS_OK) {
        return 4;
    }
    const suite_counts *counts = &state->counts;
    const uint64_t line_count = count_of(lines);
    bool can_go_on = true;
    for (uint64_t index = 0; index < line_count && can_go_on; index += 1) {
        /* The line is copied: the queries reuse the scratch texts. */
        text_buffer line = {0};
        text_append(&line, string_at(lines, index));
        can_go_on = run_suite_case(state, text_view(&line));
        release_bytes(line.bytes);
    }
    sxml_sequence_release(lines);
    (void)printf("cases %llu: pass %zu, fail %zu, deviation %zu, implementation-defined %zu, "
           "not applicable %zu\n",
           (unsigned long long)line_count, counts->pass_count, counts->fail_count,
           counts->deviation_count, counts->implementation_defined_count, counts->refused_count);
    if (!can_go_on) {
        return 4;
    }
    return counts->fail_count > 0 ? 1 : 0;
}

/* ------------------------------------------------------- vefa, report */

/* Counts the findings per assertion id. */
enum { SXML_DRIVER_MAXIMUM_COUNTED_ID_COUNT = 4096 };

typedef struct id_counts {
    char *ids[SXML_DRIVER_MAXIMUM_COUNTED_ID_COUNT];
    size_t counts[SXML_DRIVER_MAXIMUM_COUNTED_ID_COUNT];
    size_t id_count;
} id_counts;

static size_t *count_slot(id_counts *table, sxml_byte_view identifier) {
    for (size_t index = 0; index < table->id_count; index += 1) {
        if (strlen(table->ids[index]) == identifier.byte_count &&
            memcmp(table->ids[index], identifier.bytes, identifier.byte_count) == 0) {
            return &table->counts[index];
        }
    }
    if (table->id_count == SXML_DRIVER_MAXIMUM_COUNTED_ID_COUNT) {
        return NULL;
    }
    char *copy = allocate_bytes(identifier.byte_count + 1);
    if (identifier.byte_count > 0) {
        memcpy(copy, identifier.bytes, identifier.byte_count);
    }
    copy[identifier.byte_count] = '\0';
    table->ids[table->id_count] = copy;
    table->counts[table->id_count] = 0;
    table->id_count += 1;
    return &table->counts[table->id_count - 1];
}

static void clear_counts(id_counts *table) {
    for (size_t index = 0; index < table->id_count; index += 1) {
        release_bytes(table->ids[index]);
    }
    table->id_count = 0;
}

static sxml_status count_report(void *state, const sxml_schematron_report *report) {
    size_t *slot = count_slot(state, report->id);
    if (slot != NULL) {
        *slot += 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_schematron_report_sink_operations count_sink_operations = {
    .receive = count_report};

typedef struct vefa_totals {
    size_t check_count;
    size_t pass_count;
    size_t fail_count;
    size_t engine_error_count;
    size_t test_count;
} vefa_totals;

/* One VEFA test: its expectations (from vefa.xq: an id, a kind and a
   count, each a tab-separated field, the count empty for "at least
   once"), the findings counted by id, and its name. */
typedef struct vefa_test {
    sxml_byte_view expectations;
    const id_counts *counts;
    const char *name;
} vefa_test;

/* How often the rule identifier fired. */
static size_t fired_count(const id_counts *counts, sxml_byte_view identifier) {
    for (size_t slot_index = 0; slot_index < counts->id_count; slot_index += 1) {
        if (strlen(counts->ids[slot_index]) == identifier.byte_count &&
            memcmp(counts->ids[slot_index], identifier.bytes, identifier.byte_count) == 0) {
            return counts->counts[slot_index];
        }
    }
    return 0;
}

/* Checks the expectations of one test against its counts. */
static void check_expectations(const vefa_test *test, vefa_totals *totals) {
    case_fields fields;
    split_fields(test->expectations, &fields);
    for (size_t index = 0; index + 2 < fields.field_count; index += 3) {
        const sxml_byte_view identifier = fields.fields[index];
        const sxml_byte_view kind = fields.fields[index + 1];
        const sxml_byte_view count_text = fields.fields[index + 2];
        if (identifier.byte_count == 0) {
            continue;
        }
        size_t expected_count = 0;
        for (size_t position = 0; position < count_text.byte_count; position += 1) {
            expected_count = expected_count * 10U + (size_t)(count_text.bytes[position] - '0');
        }
        const size_t fired = fired_count(test->counts, identifier);
        bool holds = false;
        if (sxml_byte_view_equals(kind, sxml_byte_view_from_c_string("success"))) {
            holds = fired == 0;
        } else if (count_text.byte_count > 0) {
            holds = fired == expected_count;
        } else {
            holds = fired > 0;
        }
        totals->check_count += 1;
        if (holds) {
            totals->pass_count += 1;
            continue;
        }
        totals->fail_count += 1;
        (void)printf("FAIL %s: %.*s expected %.*s %.*s: fired %zu\n", test->name,
                     (int)identifier.byte_count, (const char *)identifier.bytes,
                     (int)kind.byte_count, (const char *)kind.bytes, (int)count_text.byte_count,
                     (const char *)count_text.bytes, fired);
    }
}

typedef struct vefa_options {
    const char *schematron_path;
    bool is_quiet;
    char **test_files;
    size_t test_file_count;
} vefa_options;

static double seconds_since(struct timespec start) {
    struct timespec now;
    (void)timespec_get(&now, TIME_UTC);
    return (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

/* Validates the tests of one VEFA file. */
/* The rules compiled and the totals of a vefa run. */
typedef struct vefa_run {
    const sxml_schematron *schematron;
    vefa_totals totals;
} vefa_run;

static bool run_vefa_file(driver *state, vefa_run *run, const char *test_file) {
    const sxml_schematron *schematron = run->schematron;
    vefa_totals *totals = &run->totals;
    text_buffer uri = {0};
    uri_of_path((path_request){.path = test_file, .is_directory = false}, &uri);
    if (!load_query(state, "vefa.xq")) {
        release_bytes(uri.bytes);
        return false;
    }
    text_append_c(&state->query, "\nlocal:tests(");
    text_append_literal(&state->query, text_view(&uri));
    text_append_c(&state->query, ")");
    release_bytes(uri.bytes);
    sxml_sequence tests = sxml_sequence_empty();
    if (evaluate_query(state, &(query_run){.text = text_view(&state->query)}, &tests) !=
        SXML_STATUS_OK) {
        return false;
    }
    const uint64_t item_count = count_of(tests);
    static id_counts counts;
    const char *base_name = strrchr(test_file, '/') != NULL ? strrchr(test_file, '/') + 1 : test_file;
    for (uint64_t index = 0; index + 1 < item_count; index += 2) {
        sxml_item document;
        if (sxml_sequence_get_item(tests, (sxml_item_request){.item_index = index}, &document) !=
            SXML_STATUS_OK) {
            break;
        }
        char test_name[512];
        (void)snprintf(test_name, sizeof test_name, "%s#%llu", base_name,
                 (unsigned long long)(index / 2));
        clear_counts(&counts);
        sxml_error_record record;
        memset(&record, 0, sizeof record);
        sxml_schematron_outcome outcome;
        totals->test_count += 1;
        const sxml_status status = sxml_schematron_validate_node(
            schematron,
            &(sxml_schematron_validation_request){
                .node = document,
                .report_sink = {.state = &counts, .operations = &count_sink_operations},
                .error_record = &record},
            &outcome);
        if (status != SXML_STATUS_OK) {
            totals->engine_error_count += 1;
            (void)printf("ENGINE-ERROR %s: ", test_name);
            print_record(stdout, &record);
            (void)putchar('\n');
            continue;
        }
        check_expectations(&(vefa_test){.expectations = string_at(tests, index + 1),
                                         .counts = &counts,
                                         .name = test_name},
                           totals);
    }
    clear_counts(&counts);
    sxml_sequence_release(tests);
    return true;
}

static int run_vefa(driver *state, const vefa_options *options) {
    text_buffer schema_uri = {0};
    uri_of_path((path_request){.path = options->schematron_path, .is_directory = false}, &schema_uri);
    struct timespec start;
    (void)timespec_get(&start, TIME_UTC);
    sxml_error_record record;
    sxml_schematron *schematron = compile_schema(
        state, &(compile_request){.schema_uri = text_view(&schema_uri), .prints_errors = true},
        &record);
    const double compile_seconds = seconds_since(start);
    release_bytes(schema_uri.bytes);
    if (schematron == NULL) {
        (void)fprintf(stderr, "sxml-schematron-test: %s: ", options->schematron_path);
        print_record(stderr, &record);
        (void)fputc('\n', stderr);
        return 1;
    }
    vefa_run run;
    memset(&run, 0, sizeof run);
    run.schematron = schematron;
    const vefa_totals *totals = &run.totals;
    (void)timespec_get(&start, TIME_UTC);
    bool can_go_on = true;
    for (size_t index = 0; index < options->test_file_count && can_go_on; index += 1) {
        can_go_on = run_vefa_file(state, &run, options->test_files[index]);
    }
    const double validate_seconds = seconds_since(start);
    sxml_schematron_destroy(schematron);
    (void)printf("tests %zu, checks %zu: pass %zu, fail %zu, engine errors %zu", totals->test_count,
                 totals->check_count, totals->pass_count, totals->fail_count,
                 totals->engine_error_count);
    if (!options->is_quiet) {
        (void)printf(" (compile %.3f s, validation %.3f s)", compile_seconds, validate_seconds);
    }
    (void)putchar('\n');
    if (!can_go_on) {
        return 4;
    }
    return totals->fail_count + totals->engine_error_count > 0 ? 1 : 0;
}

/* report: one line per finding (and fired rule), sorted by the caller. */
static sxml_status print_report(void *state, const sxml_schematron_report *report) {
    (void)state;
    static const char *const kind_names[] = {"failed-assert", "successful-report", "fired-rule"};
    (void)printf("%s\t", kind_names[report->report_kind]);
    print_view(stdout, report->pattern_id);
    (void)putchar('\t');
    if (report->report_kind == SXML_SCHEMATRON_REPORT_KIND_FIRED_RULE) {
        print_view(stdout, report->context_text);
    } else {
        print_view(stdout, report->id);
    }
    (void)putchar('\t');
    print_view(stdout, report->node_path);
    (void)putchar('\n');
    return SXML_STATUS_OK;
}

static const sxml_schematron_report_sink_operations print_sink_operations = {
    .receive = print_report};

typedef struct report_options {
    const char *schematron_path;
    const char *phase_id;
    bool reports_fired_rules;
    bool writes_svrl;
    char **instances;
    size_t instance_count;
} report_options;

static int run_report(driver *state, const report_options *options) {
    text_buffer uri = {0};
    uri_of_path((path_request){.path = options->schematron_path, .is_directory = false}, &uri);
    sxml_error_record record;
    sxml_schematron *schematron = compile_schema(
        state,
        &(compile_request){.schema_uri = text_view(&uri),
                           .phase_id = sxml_byte_view_from_c_string(options->phase_id),
                           .prints_errors = true},
        &record);
    if (schematron == NULL) {
        release_bytes(uri.bytes);
        (void)fprintf(stderr, "sxml-schematron-test: %s: ", options->schematron_path);
        print_record(stderr, &record);
        (void)fputc('\n', stderr);
        return 1;
    }
    int exit_code = 0;
    for (size_t index = 0; index < options->instance_count; index += 1) {
        uri_of_path((path_request){.path = options->instances[index], .is_directory = false}, &uri);
        sxml_item instance;
        if (!read_file(options->instances[index], &state->instance_text) ||
            parse_document(state, (document_text){.text = text_view(&state->instance_text), .uri = text_view(&uri)}, &instance) !=
                SXML_STATUS_OK) {
            (void)fprintf(stderr, "sxml-schematron-test: cannot read %s\n", options->instances[index]);
            exit_code = 3;
            continue;
        }
        (void)printf("instance\t%s\n", options->instances[index]);
        memset(&record, 0, sizeof record);
        sxml_schematron_outcome outcome;
        const sxml_status status = sxml_schematron_validate_node(
            schematron,
            &(sxml_schematron_validation_request){
                .node = instance,
                .report_sink = {.state = NULL,
                                .operations = options->writes_svrl ? NULL : &print_sink_operations},
                .svrl_output_stream = {.state = &state->svrl,
                                       .operations = options->writes_svrl ? &text_stream_operations
                                                                          : NULL},
                .error_record = &record,
                .reports_fired_rules = options->reports_fired_rules},
            &outcome);
        if (options->writes_svrl) {
            print_view(stdout, text_view(&state->svrl));
            text_clear(&state->svrl);
        }
        sxml_item_release(instance);
        if (status != SXML_STATUS_OK) {
            (void)printf("error\t");
            print_record(stdout, &record);
            (void)putchar('\n');
            exit_code = 1;
        }
    }
    release_bytes(uri.bytes);
    sxml_schematron_destroy(schematron);
    return exit_code;
}

/* ------------------------------------------------------------- main */

/* The value of --name=VALUE in argument; NULL when argument is not that
   option. */
static const char *option_value(const char *argument, const char *name) {
    const size_t length = strlen(name);
    if (strncmp(argument, name, length) == 0 && argument[length] == '=') {
        return argument + length + 1;
    }
    return NULL;
}

static int usage(void) {
    (void)fputs("usage: sxml-schematron-test run [--suite=DIR] [--set=NAME] [--case=NAME] "
          "[--deviations=FILE]\n"
          "       sxml-schematron-test vefa --schematron=FILE [--quiet] TESTFILE...\n"
          "       sxml-schematron-test report --schematron=FILE [--phase=PHASE] [--fired] "
          "[--svrl] INSTANCE...\n",
          stderr);
    return 2;
}

int main(int argument_count, char **arguments) {
    if (argument_count < 2) {
        return usage();
    }
    const char *command = arguments[1];
    run_options run = {.set_name = "", .case_name = "", .deviations_path = NULL};
    vefa_options vefa = {.schematron_path = NULL, .is_quiet = false, .test_files = NULL,
                         .test_file_count = 0};
    report_options report = {.schematron_path = NULL, .phase_id = "",
                             .reports_fired_rules = false, .writes_svrl = false,
                             .instances = NULL,
                             .instance_count = 0};
    const char *suite_directory = "tests/schematron";
    char **positionals = (char **)allocate_bytes((size_t)argument_count * sizeof(char *));
    size_t positional_count = 0;
    for (int index = 2; index < argument_count; index += 1) {
        const char *argument = arguments[index];
        const char *suite_value = option_value(argument, "--suite");
        const char *set_value = option_value(argument, "--set");
        const char *case_value = option_value(argument, "--case");
        const char *deviations_value = option_value(argument, "--deviations");
        const char *schematron_value = option_value(argument, "--schematron");
        const char *phase_value = option_value(argument, "--phase");
        if (suite_value != NULL) {
            suite_directory = suite_value;
        } else if (set_value != NULL) {
            run.set_name = set_value;
        } else if (case_value != NULL) {
            run.case_name = case_value;
        } else if (deviations_value != NULL) {
            run.deviations_path = deviations_value;
        } else if (schematron_value != NULL) {
            vefa.schematron_path = schematron_value;
            report.schematron_path = schematron_value;
        } else if (phase_value != NULL) {
            report.phase_id = phase_value;
        } else if (strcmp(argument, "--fired") == 0) {
            report.reports_fired_rules = true;
        } else if (strcmp(argument, "--svrl") == 0) {
            report.writes_svrl = true;
        } else if (strcmp(argument, "--quiet") == 0) {
            vefa.is_quiet = true;
        } else if (argument[0] == '-' && argument[1] == '-') {
            release_bytes((void *)positionals);
            return usage();
        } else {
            positionals[positional_count] = arguments[index];
            positional_count += 1;
        }
    }
    driver state;
    memset(&state, 0, sizeof state);
    state.suite_directory = suite_directory;
    uri_of_path((path_request){.path = suite_directory, .is_directory = true}, &state.suite_uri);
    if (!create_driver(&state)) {
        release_bytes((void *)positionals);
        return 4;
    }
    int exit_code = 2;
    if (strcmp(command, "run") == 0 && positional_count == 0) {
        exit_code = run_suite(&state, &run);
    } else if (strcmp(command, "vefa") == 0 && vefa.schematron_path != NULL &&
               positional_count > 0) {
        vefa.test_files = positionals;
        vefa.test_file_count = positional_count;
        exit_code = run_vefa(&state, &vefa);
    } else if (strcmp(command, "report") == 0 && report.schematron_path != NULL &&
               positional_count > 0) {
        report.instances = positionals;
        report.instance_count = positional_count;
        exit_code = run_report(&state, &report);
    } else {
        exit_code = usage();
    }
    destroy_driver(&state);
    release_bytes((void *)positionals);
    return exit_code;
}
