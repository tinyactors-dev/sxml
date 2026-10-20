/*
 * The resource part of the function library (packet 2.12, D131): the
 * functions that read the evaluation environment (src/processor_internal.h)
 * rather than their arguments. F&O 4.0, section 17.1 "Functions giving
 * access to external information": fn:collection, fn:uri-collection,
 * fn:unparsed-text, fn:unparsed-text-lines, fn:unparsed-text-available,
 * fn:unparsed-binary, fn:environment-variable,
 * fn:available-environment-variables; the text of section 17.4.5
 * fn:json-doc, whose row is the json part's (D143); section 20.1.3
 * fn:default-language. fn:trace and fn:message, which hand their
 * input to the environment's trace sink, are in src/function_library_error.c.
 *
 * Golden file: src/function_library_math.c (rows, comments); rows as in
 * src/function_library_internal.h.
 *
 * sxml never opens a file: every resource comes from the resolver of the
 * evaluation environment, which a dynamic context made by the processor
 * carries. Without one (a unit test's dynamic context) nothing is
 * available, there are no environment variables, the default language is
 * "en".
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/string.h"
#include "text_decoding_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

static const sxml_static_error text_not_retrieved_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOUT1170"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The resource cannot be retrieved (F&O 4.0, fn:unparsed-text)"),
};

static const sxml_static_error text_encoding_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOUT1190"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The encoding is not supported, the bytes do not decode in it, or a "
        "character is not permitted (F&O 4.0, fn:unparsed-text)"),
};

static const sxml_static_error text_undecodable_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOUT1200"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No encoding was given and the bytes are not UTF-8 (F&O 4.0, "
        "fn:unparsed-text)"),
};

/* Raises a dynamic error with code_local_name and the resolver's
   description, or the static error's own when there is none. */
typedef struct described_error {
    const sxml_static_error *error;
    sxml_byte_view description;
} described_error;

static sxml_status report_described(const sxml_function_call *call, described_error error) {
    if (error.description.byte_count == 0) {
        return sxml_function_library_report(call, error.error);
    }
    return sxml_item_heap_raise_error(
        call->item_heap,
        &(sxml_error_raise_request){
            .error_status = error.error->error_status,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = error.error->code_local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = error.description,
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        call->error_record);
}

/* --------------------------------------------------------------- Options */

/* The options of fn:unparsed-text and its relatives (F&O 4.0, section
   17.1.5), after the option parameter conventions. */
typedef struct text_options {
    sxml_byte_view encoding_name;
    bool normalizes_newlines;
    bool has_fallback;
    /* fn:json-doc: any codepoint JSON allows passes (F&O 4.0, section
       17.4.5), not only XML characters. */
    bool permits_any_character;
    /* The options read (by text_option_definitions), owned; the
       encoding name borrows from them. */
    sxml_option_values option_values;
} text_options;

/* Which function reads a text resource: its options and checks differ. */
typedef enum text_function {
    SXML_TEXT_FUNCTION_UNPARSED_TEXT,
    SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES,
    SXML_TEXT_FUNCTION_JSON_DOC,
} text_function;

/* The options of fn:unparsed-text and fn:unparsed-text-lines (F&O 4.0,
   sections 17.1.5 and 17.1.6), by position; -lines reads only the first
   two, so normalize-newlines is an unknown option there. */
enum {
    SXML_TEXT_OPTION_ENCODING,
    SXML_TEXT_OPTION_FALLBACK,
    SXML_TEXT_OPTION_NORMALIZE_NEWLINES,
};

static const sxml_option_definition text_option_definitions[] = {
    SXML_FUNCTION_OPTION("encoding", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("fallback", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("normalize-newlines", SXML_TYPE_XS_BOOLEAN,
                         SXML_OCCURRENCE_EXACTLY_ONE),
};

/*
 * Reads $options (argument 1) of fn:unparsed-text and fn:unparsed-text-
 * lines: a string $S is the map {"encoding": $S}; a map is read by the
 * option parameter conventions (F&O 4.0, section 1.7) with the keys
 * encoding, fallback and normalize-newlines (not for -lines, which always
 * normalizes: XPTY0004, as QT4 fn-unparsed-text-lines-40-014 expects)
 * (F&O 4.0, sections 17.1.5 and 17.1.6). fn:json-doc's options are
 * fn:parse-json's. Release options->option_values on every path.
 */
static sxml_status read_text_options(const sxml_function_call *call, text_function function,
                                     text_options *options) {
    const bool is_lines = function == SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES;
    *options = (text_options){.encoding_name = {.bytes = NULL, .byte_count = 0},
                              .normalizes_newlines = is_lines,
                              .has_fallback = false,
                              .permits_any_character = function == SXML_TEXT_FUNCTION_JSON_DOC};
    if (function == SXML_TEXT_FUNCTION_JSON_DOC) {
        return SXML_STATUS_OK;
    }
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_ATOMIC) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(argument.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING) {
            options->encoding_name = datum.datum_details.string.utf8;
        }
        return status;
    }
    sxml_option_values *values = &options->option_values;
    status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){
            .argument_index = 1,
            .definitions = {text_option_definitions,
                            is_lines ? SXML_TEXT_OPTION_NORMALIZE_NEWLINES
                                     : sizeof(text_option_definitions) /
                                           sizeof(text_option_definitions[0])}},
        values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence encoding = values->values[SXML_TEXT_OPTION_ENCODING];
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(encoding) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_get_datum(encoding.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING) {
        options->encoding_name = datum.datum_details.string.utf8;
    }
    options->has_fallback = sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values, .option_index = SXML_TEXT_OPTION_FALLBACK});
    if (!is_lines) {
        options->normalizes_newlines = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = values,
                                 .option_index = SXML_TEXT_OPTION_NORMALIZE_NEWLINES});
    }
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- Decoding */

/* Why a text resource gave no string. */
typedef enum text_failure {
    SXML_TEXT_FAILURE_NONE,
    /* FOUT1170: not retrieved. */
    SXML_TEXT_FAILURE_NOT_RETRIEVED,
    /* FOUT1190: unsupported encoding, bytes that do not decode in the
       encoding candidate, a character not permitted. */
    SXML_TEXT_FAILURE_ENCODING,
    /* FOUT1200: no encoding candidate and bytes that are not UTF-8. */
    SXML_TEXT_FAILURE_UNDECODABLE,
} text_failure;

/* The failure a decoding failure is (src/text_decoding_internal.h). */
static text_failure text_failure_of(sxml_text_decoding_failure failure) {
    switch (failure) {
    case SXML_TEXT_DECODING_FAILURE_ENCODING:
        return SXML_TEXT_FAILURE_ENCODING;
    case SXML_TEXT_DECODING_FAILURE_UNDECODABLE:
        return SXML_TEXT_FAILURE_UNDECODABLE;
    case SXML_TEXT_DECODING_FAILURE_NONE:
        break;
    }
    return SXML_TEXT_FAILURE_NONE;
}

/* ---------------------------------------------------------- Text reading */

/* What fn:unparsed-text read: the text, or why not. */
typedef struct text_outcome {
    text_failure failure;
    /* The resolver's description of a failure to retrieve, borrowed until
       the retrieved resource is released; may be empty. */
    sxml_byte_view description;
    /* The decoded text, in item heap memory; release it on every path. */
    sxml_library_text text;
} text_outcome;

/* The dynamic error a failure raises. */
static sxml_status report_text_failure(const sxml_function_call *call,
                                       const text_outcome *outcome) {
    switch (outcome->failure) {
    case SXML_TEXT_FAILURE_NOT_RETRIEVED:
        return report_described(call, (described_error){.error = &text_not_retrieved_error,
                                                        .description = outcome->description});
    case SXML_TEXT_FAILURE_ENCODING:
        return sxml_function_library_report(call, &text_encoding_error);
    case SXML_TEXT_FAILURE_UNDECODABLE:
        return sxml_function_library_report(call, &text_undecodable_error);
    case SXML_TEXT_FAILURE_NONE:
        break;
    }
    return SXML_STATUS_OK;
}

/* A text resource to read and decode. */
typedef struct text_request {
    sxml_byte_view uri_reference;
    const text_options *options;
} text_request;

/* Decodes a retrieved resource: the encoding named outside the content
   (a charset), else the encoding option (F&O 4.0, section 17.1.5). */
static sxml_status decode_retrieved(const sxml_retrieved_resource *retrieved,
                                    const text_options *options, text_outcome *outcome) {
    const sxml_byte_view candidate = retrieved->encoding_name.byte_count > 0
                                         ? retrieved->encoding_name
                                         : options->encoding_name;
    sxml_text_decoding decoding;
    if (!sxml_text_decoding_choose(
            (sxml_text_decoding_choice){.content = retrieved->content, .candidate = candidate},
            &decoding)) {
        outcome->failure = SXML_TEXT_FAILURE_ENCODING;
        return SXML_STATUS_OK;
    }
    sxml_text_decoding_failure failure = SXML_TEXT_DECODING_FAILURE_NONE;
    const sxml_status status =
        sxml_text_decode(&(sxml_text_decoding_request){
                             .content = retrieved->content,
                             .decoding = decoding,
                             .normalizes_newlines = options->normalizes_newlines,
                             .has_fallback = options->has_fallback,
                             .permits_any_character = options->permits_any_character},
                         &outcome->text, &failure);
    outcome->failure = text_failure_of(failure);
    return status;
}

/*
 * Retrieves and decodes request->uri_reference (F&O 4.0, section
 * 17.1.5) into *outcome, whose text the caller releases. A failure the
 * function raises (FOUT1170, FOUT1190, FOUT1200) is in outcome->failure,
 * not the status, so fn:unparsed-text-available can answer false. An
 * unsupported encoding name is FOUT1190 even before retrieval.
 */
static sxml_status read_text(const sxml_function_call *call, const text_request *request,
                             text_outcome *outcome) {
    *outcome = (text_outcome){.failure = SXML_TEXT_FAILURE_NONE,
                              .description = {.bytes = NULL, .byte_count = 0},
                              .text = {.item_heap = call->item_heap,
                                       .bytes = NULL,
                                       .byte_count = 0,
                                       .byte_capacity = 0}};
    if (request->options->encoding_name.byte_count > 0 &&
        sxml_xml_classify_encoding_name(request->options->encoding_name) ==
            SXML_XML_ENCODING_FAMILY_UNSUPPORTED) {
        outcome->failure = SXML_TEXT_FAILURE_ENCODING;
        return SXML_STATUS_OK;
    }
    sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
    if (loader == NULL) {
        outcome->failure = SXML_TEXT_FAILURE_NOT_RETRIEVED;
        return SXML_STATUS_OK;
    }
    sxml_retrieved_resource retrieved;
    sxml_status status = sxml_processor_document_loader_retrieve(
        loader,
        &(sxml_resource_retrieval_request){
            .resource_kind = SXML_RESOURCE_KIND_TEXT,
            .uri_reference = request->uri_reference,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .encoding_name = request->options->encoding_name},
        &retrieved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (retrieved.is_available) {
        status = decode_retrieved(&retrieved, request->options, outcome);
        sxml_processor_document_loader_release(loader, &retrieved);
        return status;
    }
    outcome->failure = SXML_TEXT_FAILURE_NOT_RETRIEVED;
    /* The description is borrowed until release; report it before. */
    outcome->description = retrieved.description;
    status = report_text_failure(call, outcome);
    sxml_processor_document_loader_release(loader, &retrieved);
    outcome->description = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    return status;
}

/* Reads $source and $options and the text; *has_source is false for an
   empty $source. */
typedef struct text_reading {
    bool has_source;
    text_options options;
    text_outcome outcome;
} text_reading;

static void text_reading_release(text_reading *reading) {
    sxml_function_library_release_options(&reading->options.option_values);
    sxml_function_library_text_release(&reading->outcome.text);
}

/*
 * The shared start of fn:unparsed-text, -lines, -available and
 * fn:json-doc: reads the arguments and the resource. An untrusted call
 * raises FODC0016 (XPath 4.0, 2.2.3: no external resources are available
 * to untrusted code); a retrieval failure is raised as FOUT1170 with the
 * resolver's description (the status says so); a decoding failure is
 * left in reading->outcome.failure.
 */
static sxml_status start_text_reading(const sxml_function_call *call, text_function function,
                                      text_reading *reading) {
    memset(reading, 0, sizeof(*reading));
    reading->outcome.text.item_heap = call->item_heap;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_dynamic_context_check_trust(call->dynamic_context, call->error_record);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_optional_item source;
    status = sxml_function_library_read_optional_item(call, 0, &source);
    if (status == SXML_STATUS_OK) {
        status = read_text_options(call, function, &reading->options);
    }
    if (status != SXML_STATUS_OK || !source.has_item) {
        return status;
    }
    reading->has_source = true;
    sxml_string uri_reference;
    status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
    if (status == SXML_STATUS_OK) {
        status = read_text(call,
                           &(text_request){.uri_reference = uri_reference.utf8,
                                           .options = &reading->options},
                           &reading->outcome);
    }
    return status;
}

/* fn:unparsed-text($source as xs:string?, $options as (xs:string |
   map(*))? := {}) as xs:string? (F&O 4.0, section 17.1.5). */
static sxml_status call_unparsed_text(const sxml_function_call *call, sxml_sequence *result) {
    text_reading reading;
    sxml_status status = start_text_reading(call, SXML_TEXT_FUNCTION_UNPARSED_TEXT, &reading);
    if (status == SXML_STATUS_OK && reading.has_source) {
        if (reading.outcome.failure != SXML_TEXT_FAILURE_NONE) {
            status = report_text_failure(call, &reading.outcome);
        } else {
            status = sxml_function_library_make_string(
                call->item_heap, sxml_function_library_text_view(&reading.outcome.text),
                result);
        }
    } else if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_empty();
    }
    text_reading_release(&reading);
    return status;
}

/* The lines of text, a line ending at each LF; a last empty line is
   dropped (F&O 4.0, section 17.1.6). */
static sxml_status make_lines(const sxml_function_call *call, sxml_byte_view text,
                              sxml_sequence *result) {
    size_t line_count = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        line_count += text.bytes[byte_index] == '\n' ? 1U : 0U;
    }
    if (text.byte_count > 0 && text.bytes[text.byte_count - 1] != '\n') {
        line_count += 1;
    }
    if (line_count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(line_count, sizeof(sxml_item), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *lines = memory;
    size_t made_count = 0;
    size_t line_start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && made_count < line_count;
         byte_index += 1) {
        if (byte_index < text.byte_count && text.bytes[byte_index] != '\n') {
            continue;
        }
        sxml_sequence line = sxml_sequence_empty();
        status = sxml_function_library_make_string(
            call->item_heap,
            (sxml_byte_view){.bytes = text.bytes + line_start,
                             .byte_count = byte_index - line_start},
            &line);
        if (status == SXML_STATUS_OK) {
            lines[made_count] = line.content;
            made_count += 1;
        }
        line_start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = lines, .item_count = made_count}, result);
    }
    for (size_t line_index = 0; line_index < made_count; line_index += 1) {
        sxml_item_release(lines[line_index]);
    }
    sxml_allocator_release(allocator, memory);
    return status;
}

/* fn:unparsed-text-lines($source as xs:string?, $options as (xs:string |
   map(*))? := {}) as xs:string* (F&O 4.0, section 17.1.6): the text with
   newlines normalized, split at each line feed. */
static sxml_status call_unparsed_text_lines(const sxml_function_call *call,
                                            sxml_sequence *result) {
    text_reading reading;
    sxml_status status = start_text_reading(call, SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES, &reading);
    if (status == SXML_STATUS_OK && reading.has_source) {
        if (reading.outcome.failure != SXML_TEXT_FAILURE_NONE) {
            status = report_text_failure(call, &reading.outcome);
        } else {
            status = make_lines(call, sxml_function_library_text_view(&reading.outcome.text),
                                result);
        }
    } else if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_empty();
    }
    text_reading_release(&reading);
    return status;
}

/* fn:unparsed-text-available($source as xs:string?, $options as
   (xs:string | map(*))? := {}) as xs:boolean (F&O 4.0, section 17.1.7):
   whether fn:unparsed-text would return a string; its dynamic errors
   (FOUT1170, FOUT1190, FOUT1200) mean false. */
static sxml_status call_unparsed_text_available(const sxml_function_call *call,
                                                sxml_sequence *result) {
    text_reading reading;
    sxml_status status = start_text_reading(call, SXML_TEXT_FUNCTION_UNPARSED_TEXT, &reading);
    bool is_available = false;
    if (status == SXML_STATUS_OK) {
        is_available = reading.has_source && reading.outcome.failure == SXML_TEXT_FAILURE_NONE;
    } else if (status == SXML_STATUS_DYNAMIC_ERROR) {
        /* FOUT1170 was raised for a resource not retrieved, FODC0016 for
           an untrusted call. */
        status = SXML_STATUS_OK;
    }
    text_reading_release(&reading);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_available});
    }
    return status;
}

/* fn:unparsed-binary($source as xs:string?) as xs:base64Binary? (F&O
   4.0, section 17.1.8): the bytes as they are; FOUT1170 when they cannot
   be retrieved. */
static sxml_status call_unparsed_binary(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item source = {.has_item = false};
    sxml_status status = sxml_dynamic_context_check_trust(call->dynamic_context, call->error_record);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 0, &source);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!source.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_string uri_reference;
    status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
    sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (loader == NULL) {
        return sxml_function_library_report(call, &text_not_retrieved_error);
    }
    sxml_retrieved_resource retrieved;
    status = sxml_processor_document_loader_retrieve(
        loader,
        &(sxml_resource_retrieval_request){
            .resource_kind = SXML_RESOURCE_KIND_BINARY,
            .uri_reference = uri_reference.utf8,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .encoding_name = {.bytes = NULL, .byte_count = 0}},
        &retrieved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!retrieved.is_available) {
        status = report_described(call, (described_error){.error = &text_not_retrieved_error,
                                                          .description = retrieved.description});
    } else {
        sxml_item binary = {.representation = 0};
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
                .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                          .datum_details = {.binary = retrieved.content}}},
            &binary);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(binary);
        }
    }
    sxml_processor_document_loader_release(loader, &retrieved);
    return status;
}

/* -------------------------------------------------------------- JSON doc */

/* The text of fn:json-doc's source (F&O 4.0, section 17.4.5:
   fn:unparsed-text($source)), declared in
   src/function_library_core_internal.h. */
sxml_status sxml_function_library_read_json_document(const sxml_function_call *call,
                                                     sxml_sequence *text) {
    *text = sxml_sequence_empty();
    text_reading reading;
    sxml_status status = start_text_reading(call, SXML_TEXT_FUNCTION_JSON_DOC, &reading);
    if (status == SXML_STATUS_OK && reading.has_source &&
        reading.outcome.failure != SXML_TEXT_FAILURE_NONE) {
        status = report_text_failure(call, &reading.outcome);
    } else if (status == SXML_STATUS_OK && reading.has_source) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&reading.outcome.text), text);
    }
    text_reading_release(&reading);
    return status;
}

/* ----------------------------------------------------------- Collections */

static const sxml_static_error collection_not_available_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "No collection is available at the URI (F&O 4.0, fn:collection)"),
};

/* fn:collection and fn:uri-collection: the collection of resource_kind
   that $source names, or the default collection for the empty sequence
   (F&O 4.0, sections 17.1.3 and 17.1.4); FODC0002 when there is none. */
static sxml_status read_collection(const sxml_function_call *call,
                                   sxml_resource_kind resource_kind, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item source = {.has_item = false};
    sxml_status status = sxml_dynamic_context_check_trust(call->dynamic_context, call->error_record);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 0, &source);
    }
    sxml_string uri_reference = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK && source.has_item) {
        status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
    if (loader == NULL) {
        return sxml_function_library_report(call, &collection_not_available_error);
    }
    sxml_loaded_collection collection;
    status = sxml_processor_document_loader_load_collection(
        loader,
        &(sxml_collection_request){
            .resource_kind = resource_kind,
            .is_default_collection = !source.has_item,
            .uri_reference = uri_reference.utf8,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context)},
        &collection);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!collection.is_available) {
        return report_described(call, (described_error){.error = &collection_not_available_error,
                                                        .description = collection.description});
    }
    *result = collection.items;
    return SXML_STATUS_OK;
}

/* fn:collection($source as xs:string? := ()) as item()* (F&O 4.0,
   section 17.1.3). */
static sxml_status call_collection(const sxml_function_call *call, sxml_sequence *result) {
    return read_collection(call, SXML_RESOURCE_KIND_COLLECTION, result);
}

/* fn:uri-collection($source as xs:string? := ()) as xs:anyURI* (F&O
   4.0, section 17.1.4). */
static sxml_status call_uri_collection(const sxml_function_call *call, sxml_sequence *result) {
    return read_collection(call, SXML_RESOURCE_KIND_URI_COLLECTION, result);
}

/* ------------------------------------------------- Environment variables */

/* The environment variables of the evaluation; none without the
   processor's environment. */
static sxml_environment_variable_view environment_variables_of(
    const sxml_function_call *call) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL) {
        return (sxml_environment_variable_view){.environment_variables = NULL,
                                                .environment_variable_count = 0};
    }
    return environment->environment_variables;
}

/* fn:environment-variable($name as xs:string) as xs:string? (F&O 4.0,
   section 17.1.10): names compared byte for byte, the collation that
   keeps them unique (D104). */
static sxml_status call_environment_variable(const sxml_function_call *call,
                                             sxml_sequence *result) {
    sxml_string name;
    const sxml_status status = sxml_function_library_read_string_argument(call, 0, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_environment_variable_view variables = environment_variables_of(call);
    for (size_t variable_index = 0; variable_index < variables.environment_variable_count;
         variable_index += 1) {
        const sxml_environment_variable *variable =
            &variables.environment_variables[variable_index];
        if (sxml_byte_view_equals(variable->variable_name, name.utf8)) {
            return sxml_function_library_make_string(call->item_heap, variable->variable_value,
                                                     result);
        }
    }
    *result = sxml_sequence_empty();
    return SXML_STATUS_OK;
}

/* fn:available-environment-variables() as xs:string* (F&O 4.0, section
   17.1.11): the names, in the order the host listed them. */
static sxml_status call_available_environment_variables(const sxml_function_call *call,
                                                        sxml_sequence *result) {
    const sxml_environment_variable_view variables = environment_variables_of(call);
    if (variables.environment_variable_count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(variables.environment_variable_count, sizeof(sxml_item), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *names = memory;
    size_t made_count = 0;
    while (status == SXML_STATUS_OK && made_count < variables.environment_variable_count) {
        sxml_sequence name = sxml_sequence_empty();
        status = sxml_function_library_make_string(
            call->item_heap, variables.environment_variables[made_count].variable_name, &name);
        if (status == SXML_STATUS_OK) {
            names[made_count] = name.content;
            made_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = names, .item_count = made_count}, result);
    }
    for (size_t name_index = 0; name_index < made_count; name_index += 1) {
        sxml_item_release(names[name_index]);
    }
    sxml_allocator_release(allocator, memory);
    return status;
}

/* fn:default-language() as xs:language (F&O 4.0, section 20.1.3): never
   absent; "en" unless the host gives another. */
static sxml_status call_default_language(const sxml_function_call *call,
                                         sxml_sequence *result) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    const sxml_byte_view language = environment == NULL ? SXML_BYTE_VIEW_FROM_LITERAL("en")
                                                        : environment->default_language;
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_LANGUAGE),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = language}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter text_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "(xs:string | map(*))?")};
static const sxml_function_parameter binary_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?")};
static const sxml_function_parameter collection_parameters[] = {
    SXML_OPTIONAL_PARAMETER("source", "xs:string?")};
static const sxml_function_parameter environment_variable_parameters[] = {
    SXML_REQUIRED_PARAMETER("name", "xs:string")};

/* Reads the evaluation environment: never evaluated at compile time. */
#define SXML_RESOURCE_PROPERTIES SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT
/* F&O 4.0 classifies these as nondeterministic too (PR 2767). */
#define SXML_NONDETERMINISTIC_RESOURCE_PROPERTIES                                                  \
    (SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC)

static const sxml_native_function resource_functions[] = {
    SXML_FUNCTION_ROW("unparsed-text", text_parameters, "xs:string?",
                      SXML_NONDETERMINISTIC_RESOURCE_PROPERTIES, call_unparsed_text),
    SXML_FUNCTION_ROW("unparsed-text-lines", text_parameters, "xs:string*",
                      SXML_NONDETERMINISTIC_RESOURCE_PROPERTIES, call_unparsed_text_lines),
    SXML_FUNCTION_ROW("unparsed-text-available", text_parameters, "xs:boolean",
                      SXML_NONDETERMINISTIC_RESOURCE_PROPERTIES, call_unparsed_text_available),
    SXML_FUNCTION_ROW("unparsed-binary", binary_parameters, "xs:base64Binary?",
                      SXML_NONDETERMINISTIC_RESOURCE_PROPERTIES, call_unparsed_binary),
    SXML_FUNCTION_ROW("collection", collection_parameters, "item()*", SXML_RESOURCE_PROPERTIES,
                      call_collection),
    SXML_FUNCTION_ROW("uri-collection", collection_parameters, "xs:anyURI*",
                      SXML_RESOURCE_PROPERTIES, call_uri_collection),
    SXML_FUNCTION_ROW("environment-variable", environment_variable_parameters, "xs:string?",
                      SXML_RESOURCE_PROPERTIES, call_environment_variable),
    SXML_NULLARY_FUNCTION_ROW("available-environment-variables", "xs:string*",
                              SXML_RESOURCE_PROPERTIES, call_available_environment_variables),
    SXML_NULLARY_FUNCTION_ROW("default-language", "xs:language", SXML_RESOURCE_PROPERTIES,
                              call_default_language),
};

const sxml_native_function_view sxml_function_library_resource =
    SXML_LIBRARY_PART(resource_functions);
