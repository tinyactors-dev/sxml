/*
 * The error part of the function library (packet 1.20, D78): fn:error,
 * fn:trace and fn:message (F&O 4.0, sections 3 and 21.2). The trace
 * functions hand their input to the trace sink of the evaluation
 * environment (src/processor_internal.h; packet 2.12, D131).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 */
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"

/* fn:error($code := (), $description := (), $value := ()) (F&O 4.0,
   section 3.1.1): raises the error; without a code it is err:FOER0000.
   The error value is present only when $value is given. */
static sxml_status call_error(const sxml_function_call *call, sxml_sequence *result) {
    (void)result;
    sxml_optional_item code;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &code);
    sxml_string description = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_error_code error_code = {
        .namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
        .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")};
    if (code.has_item) {
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(code.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_QNAME) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
        sxml_name_parts parts;
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(
                name_pool, datum.datum_details.qname.expanded_name, &parts);
        }
        if (status == SXML_STATUS_OK) {
            error_code.local_name = parts.local_name;
            status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                      &error_code.namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_prefix(
                name_pool, datum.datum_details.qname.namespace_prefix,
                &error_code.namespace_prefix);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (call->arguments.sequence_count < 2) {
        description.utf8 = SXML_BYTE_VIEW_FROM_LITERAL("fn:error was called");
    }
    const bool has_value = call->arguments.sequence_count >= 3;
    return sxml_item_heap_raise_error(
        call->item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = error_code,
            .description = description.utf8,
            .has_value = has_value,
            .value = has_value ? call->arguments.sequences[2] : sxml_sequence_empty(),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        call->error_record);
}

/* Hands $input and $label to the trace sink of the evaluation, as an
   event of kind; the null sink, and a call outside the processor's
   evaluations, discard it. */
static sxml_status send_trace_event(const sxml_function_call *call,
                                    sxml_trace_event_kind kind) {
    const sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
    if (loader == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_string label = {.utf8 = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status = sxml_function_library_read_string_argument(call, 1, &label);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_processor_document_loader_trace(
        loader, &(sxml_trace_event){
                    .trace_event_kind = kind,
                    .label = label.utf8,
                    .value = sxml_function_library_get_argument(call, 0),
                    .is_terminating = false,
                    .error_code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                                   .local_name = {.bytes = NULL, .byte_count = 0},
                                   .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
                    .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                 .line_number = 0,
                                 .column_number = 0,
                                 .byte_offset = 0},
                    .processor = NULL});
}

/* fn:trace($input as item()*, $label as xs:string? := ()) as item()*
   (F&O 4.0, section 21.2.1): returns $input; the trace sink receives it
   with the label. */
static sxml_status call_trace(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_status status = send_trace_event(call, SXML_TRACE_EVENT_KIND_TRACE);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence input = sxml_function_library_get_argument(call, 0);
    sxml_sequence_retain(input);
    *result = input;
    return SXML_STATUS_OK;
}

/* fn:message($input as item()*, $label as xs:string? := ()) as
   empty-sequence() (F&O 4.0, section 21.2.2): the trace sink receives
   $input with the label; the result is empty. */
static sxml_status call_message(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_status status = send_trace_event(call, SXML_TRACE_EVENT_KIND_MESSAGE);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_empty();
    }
    return status;
}

static const sxml_function_parameter error_parameters[] = {
    SXML_OPTIONAL_PARAMETER("code", "xs:QName?"),
    SXML_OPTIONAL_PARAMETER("description", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("value", "item()*")};
static const sxml_function_parameter trace_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_OPTIONAL_PARAMETER("label", "xs:string?")};

#define SXML_TRACE_PROPERTIES                                                                      \
    (SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC)

static const sxml_native_function error_functions[] = {
    SXML_FUNCTION_ROW("error", error_parameters, "xs:error", 0, call_error),
    /* F&O 4.0, section 21.2: nondeterministic; the output is a side
       effect, so a call is never evaluated at compile time. */
    SXML_FUNCTION_ROW("trace", trace_parameters, "item()*", SXML_TRACE_PROPERTIES, call_trace),
    SXML_FUNCTION_ROW("message", trace_parameters, "empty-sequence()", SXML_TRACE_PROPERTIES,
                      call_message),
};

const sxml_native_function_view sxml_function_library_error =
    SXML_LIBRARY_PART(error_functions);
