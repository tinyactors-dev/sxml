/*
 * fn:parse-html and fn:html-doc (F&O 4.0, 17.3.2 and 17.3.3): the HTML
 * feature's library functions (src/html_internal.h, D191). Not a library
 * part of the registry: its row view is sxml_html_library_functions, which
 * the processor serves only when its configuration names
 * sxml_html_parser_feature(); without the feature both functions are
 * absent (XPST0017).
 *
 * Golden file: src/function_library_json.c (fn:parse-xml: input as a
 * string or binary, the options map, the document node).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "html_internal.h"
#include "processor_internal.h"

/* The options of fn:parse-html (F&O 4.0, 17.3.2); fn:html-doc takes the
   same. */
enum {
    SXML_HTML_OPTION_ENCODING,
    SXML_HTML_OPTION_FAIL_ON_ERROR,
    SXML_HTML_OPTION_COUNT,
};

static const sxml_option_definition html_options[SXML_HTML_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("encoding", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("fail-on-error", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

static const sxml_static_error html_parse_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0011"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The input is not a well-formed HTML document, and fail-on-error is true (F&O 4.0, "
        "fn:parse-html; HTML Living Standard, 13.2.2)"),
};

static const sxml_static_error html_encoding_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0011"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The input cannot be decoded with the encoding option: it names no encoding the "
        "Encoding Standard defines and sxml decodes (F&O 4.0, fn:parse-html)"),
};

static const sxml_static_error html_not_retrieved_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOUT1170"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The resource cannot be retrieved (F&O 4.0, fn:html-doc, as fn:unparsed-binary)"),
};

/* The input and options of one call. */
typedef struct sxml_html_call_input {
    sxml_byte_view input;
    bool is_string;
    sxml_byte_view base_uri;
    sxml_byte_view document_uri;
} sxml_html_call_input;

/* Reads the options, parses and writes the document node to *result. */
static sxml_status parse_html_input(const sxml_function_call *call,
                                    const sxml_html_call_input *input, sxml_sequence *result) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_option_values values = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {html_options, SXML_HTML_OPTION_COUNT}},
        &values);
    sxml_byte_view encoding_name = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK && values.is_present[SXML_HTML_OPTION_ENCODING]) {
        encoding_name = sxml_function_library_option_string(
                            (sxml_option_lookup){.values = &values,
                                                 .option_index = SXML_HTML_OPTION_ENCODING})
                            .utf8;
    }
    sxml_html_document_outcome outcome = {.failure = SXML_HTML_DOCUMENT_FAILURE_NONE,
                                          .document_node = {.representation = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_html_parse_document(
            environment->processor,
            &(sxml_html_document_request){
                .input = {.input = input->input,
                          .is_string = input->is_string,
                          .encoding_name = encoding_name},
                .fails_on_parse_error = sxml_function_library_option_is_true(
                    (sxml_option_lookup){.values = &values,
                                         .option_index = SXML_HTML_OPTION_FAIL_ON_ERROR}),
                .base_uri = input->base_uri,
                .document_uri = input->document_uri},
            &outcome);
    }
    sxml_function_library_release_options(&values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (outcome.failure) {
    case SXML_HTML_DOCUMENT_FAILURE_NONE:
        *result = sxml_sequence_from_item(outcome.document_node);
        return SXML_STATUS_OK;
    case SXML_HTML_DOCUMENT_FAILURE_UNKNOWN_ENCODING:
        return sxml_function_library_report(call, &html_encoding_error);
    case SXML_HTML_DOCUMENT_FAILURE_PARSE_ERROR:
        return sxml_function_library_report(call, &html_parse_error);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* fn:parse-html($value as (xs:string | xs:hexBinary | xs:base64Binary)?,
   $options as map(*)? := {}) as document-node(*:html)? (F&O 4.0,
   17.3.2). */
static sxml_status call_parse_html(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item value = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!value.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_string = datum.datum_kind == SXML_DATUM_KIND_STRING;
    if (!is_string && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return parse_html_input(
        call,
        &(sxml_html_call_input){
            .input = is_string ? datum.datum_details.string.utf8 : datum.datum_details.binary,
            .is_string = is_string,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .document_uri = {.bytes = NULL, .byte_count = 0}},
        result);
}

/* fn:html-doc($source as xs:string?, $options as map(*)? := {}) as
   document-node(*:html)? (F&O 4.0, 17.3.3): fn:unparsed-binary($source)
   => fn:parse-html($options); the document has the resource's URI. */
static sxml_status call_html_doc(const sxml_function_call *call, sxml_sequence *result) {
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
        return sxml_function_library_report(call, &html_not_retrieved_error);
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
        status = sxml_function_library_report(call, &html_not_retrieved_error);
    } else {
        sxml_byte_view document_uri = retrieved.resource.resource_uri;
        if (document_uri.byte_count == 0) {
            document_uri = retrieved.resolution.uri;
        }
        status = parse_html_input(call,
                                  &(sxml_html_call_input){.input = retrieved.content,
                                                          .is_string = false,
                                                          .base_uri = document_uri,
                                                          .document_uri = document_uri},
                                  result);
    }
    sxml_processor_document_loader_release(loader, &retrieved);
    return status;
}

static const sxml_function_parameter parse_html_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("value"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("(xs:string | xs:hexBinary | xs:base64Binary)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED,
     .default_function_number = 0},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("options"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY,
     .default_function_number = 0},
};

static const sxml_function_parameter html_doc_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("source"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:string?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED,
     .default_function_number = 0},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("options"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("map(*)?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY,
     .default_function_number = 0},
};

static const sxml_native_function html_functions[] = {
    SXML_FUNCTION_ROW("parse-html", parse_html_parameters, "document-node(*:html)?",
                      SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC, call_parse_html),
    SXML_FUNCTION_ROW("html-doc", html_doc_parameters, "document-node(*:html)?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                          SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
                      call_html_doc),
};

const sxml_native_function_view sxml_html_library_functions = {
    .native_functions = html_functions,
    .native_function_count = sizeof(html_functions) / sizeof(html_functions[0]),
};
