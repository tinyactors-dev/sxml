/*
 * The URI part of the function library (packet 1.23, D88):
 * fn:static-base-uri (F&O 4.0, "fn:static-base-uri", among the functions
 * that read the context) and fn:resolve-uri (F&O 4.0, "fn:resolve-uri",
 * among the functions on xs:anyURI); fn:encode-for-uri, fn:iri-to-uri and
 * fn:escape-html-uri (F&O 4.0, sections 6.2 to 6.4; packet 1.23b, D91).
 *
 * Golden file: src/function_library_context.c (rows, reading the dynamic
 * context). The static base URI comes from the dynamic context of the
 * evaluation (sxml_dynamic_context_get_static_base_uri), which the
 * processor sets from the expression's static context (D88). Resolution is
 * RFC 3986, section 5.2 (sxml_uri_resolve, src/xdm_node.c).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

static const sxml_static_error no_base_uri_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FONS0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The static base URI is absent, so a relative URI cannot be resolved "
        "(F&O 4.0, fn:resolve-uri)"),
};

static const sxml_static_error relative_base_uri_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The base URI is not an absolute URI (F&O 4.0, fn:resolve-uri)"),
};

/* Makes an xs:anyURI item of text, owned, as a singleton sequence. */
static sxml_status make_any_uri(sxml_item_heap *item_heap, sxml_byte_view text,
                                sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = text,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:static-base-uri() as xs:anyURI? (F&O 4.0, "fn:static-base-uri"): the
   empty sequence when the static base URI is absent. */
static sxml_status call_static_base_uri(const sxml_function_call *call,
                                        sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    const sxml_byte_view base_uri =
        sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    if (base_uri.byte_count == 0) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return make_any_uri(call->item_heap, base_uri, result);
}

/*
 * fn:resolve-uri($href as xs:string?, $base as xs:string? := ()) as
 * xs:anyURI? (F&O 4.0, "fn:resolve-uri"): the empty sequence for an empty
 * $href; an absolute $href unchanged; otherwise $href resolved against
 * $base, or against the static base URI when $base is absent or empty
 * (FONS0005 when that is absent too). A relative $base is FORG0002.
 */
static sxml_status call_resolve_uri(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item href_item;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &href_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!href_item.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_string href;
    status = sxml_function_library_read_string_argument(call, 0, &href);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_uri_has_scheme(href.utf8)) {
        return make_any_uri(call->item_heap, href.utf8, result);
    }
    sxml_optional_item base_item;
    status = sxml_function_library_read_optional_item(call, 1, &base_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view base_uri = {.bytes = NULL, .byte_count = 0};
    if (base_item.has_item) {
        sxml_string base;
        status = sxml_function_library_read_string_argument(call, 1, &base);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        base_uri = base.utf8;
    } else if (call->dynamic_context != NULL) {
        base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    }
    if (base_uri.byte_count == 0 && !base_item.has_item) {
        return sxml_function_library_report(call, &no_base_uri_error);
    }
    if (!sxml_uri_has_scheme(base_uri)) {
        return sxml_function_library_report(call, &relative_base_uri_error);
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0},
                                      .memory = NULL};
    status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = base_uri,
                                                             .reference = href.utf8,
                                                             .allocator = allocator},
                              &resolution);
    if (status == SXML_STATUS_OK) {
        status = make_any_uri(call->item_heap, resolution.uri, result);
        sxml_allocator_release(allocator, resolution.memory);
    }
    return status;
}

/* ------------------------------------------------------------- Escaping */

/* Which bytes a URI escaping function percent-encodes. */
typedef enum escaping_kind {
    /* fn:encode-for-uri: all but RFC 3986 unreserved characters. */
    SXML_ESCAPING_KIND_ENCODE_FOR_URI,
    /* fn:iri-to-uri: outside U+0020 to U+007E, and < > " space { } | \ ^ `
       (RFC 3987, section 3.1). */
    SXML_ESCAPING_KIND_IRI_TO_URI,
    /* fn:escape-html-uri: outside U+0020 to U+007E. */
    SXML_ESCAPING_KIND_ESCAPE_HTML_URI,
} escaping_kind;

static bool is_kept(escaping_kind kind, uint8_t byte) {
    const bool is_printable_ascii = byte >= 0x20 && byte <= 0x7E;
    switch (kind) {
    case SXML_ESCAPING_KIND_ENCODE_FOR_URI:
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
               byte == '~';
    case SXML_ESCAPING_KIND_IRI_TO_URI:
        return is_printable_ascii && byte != '<' && byte != '>' && byte != '"' && byte != ' ' &&
               byte != '{' && byte != '}' && byte != '|' && byte != '\\' && byte != '^' &&
               byte != '`';
    case SXML_ESCAPING_KIND_ESCAPE_HTML_URI:
        return is_printable_ascii;
    }
    return false;
}

/* The $value of an escaping function with every byte that kind does not
   keep written as %HH, upper-case hexadecimal of its UTF-8 octets (F&O 4.0,
   sections 6.2, 6.3 and 6.4: "must always generate hexadecimal values
   using the upper-case letters A-F"); "" for the empty sequence. */
static sxml_status call_escaping(const sxml_function_call *call, escaping_kind kind,
                                 sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const char hexadecimal_digits[] = "0123456789ABCDEF";
    sxml_library_text text = {.item_heap = call->item_heap};
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < value.utf8.byte_count;
         byte_index += 1) {
        const uint8_t byte = value.utf8.bytes[byte_index];
        if (is_kept(kind, byte)) {
            status = sxml_function_library_text_append(
                &text, (sxml_byte_view){.bytes = &value.utf8.bytes[byte_index], .byte_count = 1});
            continue;
        }
        const uint8_t escape[3] = {'%', (uint8_t)hexadecimal_digits[byte >> 4U],
                                   (uint8_t)hexadecimal_digits[byte & 0x0FU]};
        status = sxml_function_library_text_append(
            &text, (sxml_byte_view){.bytes = escape, .byte_count = sizeof(escape)});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* fn:encode-for-uri($value) (F&O 4.0, section 6.2). */
static sxml_status call_encode_for_uri(const sxml_function_call *call, sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_ENCODE_FOR_URI, result);
}

/* fn:iri-to-uri($value) (F&O 4.0, section 6.3). */
static sxml_status call_iri_to_uri(const sxml_function_call *call, sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_IRI_TO_URI, result);
}

/* fn:escape-html-uri($value) (F&O 4.0, section 6.4). */
static sxml_status call_escape_html_uri(const sxml_function_call *call,
                                        sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_ESCAPE_HTML_URI, result);
}

static const sxml_function_parameter escaping_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
static const sxml_function_parameter resolve_uri_parameters[] = {
    SXML_REQUIRED_PARAMETER("href", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("base", "xs:string?"),
};

static const sxml_native_function uri_functions[] = {
    SXML_NULLARY_FUNCTION_ROW("static-base-uri", "xs:anyURI?",
                              SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                              call_static_base_uri),
    SXML_FUNCTION_ROW("resolve-uri", resolve_uri_parameters, "xs:anyURI?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_resolve_uri),
    SXML_FUNCTION_ROW("encode-for-uri", escaping_parameters, "xs:string", 0, call_encode_for_uri),
    SXML_FUNCTION_ROW("iri-to-uri", escaping_parameters, "xs:string", 0, call_iri_to_uri),
    SXML_FUNCTION_ROW("escape-html-uri", escaping_parameters, "xs:string", 0,
                      call_escape_html_uri),
};

const sxml_native_function_view sxml_function_library_uri = SXML_LIBRARY_PART(uri_functions);
