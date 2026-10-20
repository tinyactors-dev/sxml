/*
 * The URI part of the function library (packet 1.23, D88):
 * fn:static-base-uri (F&O 4.0, "fn:static-base-uri", among the functions
 * that read the context) and fn:resolve-uri (F&O 4.0, section 7.5);
 * fn:encode-for-uri, fn:iri-to-uri and fn:escape-html-uri (F&O 4.0,
 * sections 7.2 to 7.4; packet 1.23b, D91); fn:decode-from-uri (section
 * 7.1), fn:parse-uri and fn:build-uri (section 7.6, "Parsing and building
 * URIs"; packet 2.11).
 *
 * Golden file: src/function_library_context.c (rows, reading the dynamic
 * context). The static base URI comes from the dynamic context of the
 * evaluation (sxml_dynamic_context_get_static_base_uri), which the
 * processor sets from the expression's static context (D88). Resolution is
 * RFC 3986, section 5.2 (sxml_uri_resolve, src/xdm_node.c).
 *
 * fn:parse-uri follows the transformations of F&O 4.0, section 7.6.2 step
 * by step; each regular expression there is matched by hand over the
 * octets (every character it names is ASCII), so no regex is compiled per
 * call. The parts are views into one working copy of the value (and one
 * rewritten copy for a Windows drive letter), owned by the parse.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

#include <stdalign.h>

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

static const sxml_static_error invalid_reference_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The reference or the base URI is not a valid IRI reference: a percent sign "
        "without two hexadecimal digits, a colon before the first slash that ends no "
        "scheme, or a second number sign (RFC 3987; F&O 4.0, fn:resolve-uri)"),
};

static bool is_hexadecimal_digit(uint8_t byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
           (byte >= 'a' && byte <= 'f');
}

/* False for text that no IRI reference (RFC 3987, 2.2) can be, whatever
   characters LEIRI also allows: a "%" not followed by two hexadecimal
   digits (pct-encoded), or a ":" before any "/", "?" or "#" whose prefix
   is no scheme (ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ); a relative
   reference's first segment has no colon, RFC 3986, 4.2), or a "#" in the
   fragment (ifragment is *( ipchar / "/" / "?" ), RFC 3987, 2.2). */
static bool is_iri_reference(sxml_byte_view text) {
    bool is_in_first_segment = true;
    bool is_in_fragment = false;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '#') {
            if (is_in_fragment) {
                return false;
            }
            is_in_fragment = true;
        }
        if (byte == '%' &&
            (byte_index + 2 >= text.byte_count || !is_hexadecimal_digit(text.bytes[byte_index + 1]) ||
             !is_hexadecimal_digit(text.bytes[byte_index + 2]))) {
            return false;
        }
        if (byte == '/' || byte == '?' || byte == '#') {
            is_in_first_segment = false;
        } else if (byte == ':' && is_in_first_segment) {
            if (byte_index == 0) {
                return false;
            }
            for (size_t scheme_index = 0; scheme_index < byte_index; scheme_index += 1) {
                const uint8_t scheme_byte = text.bytes[scheme_index];
                const bool is_letter = (scheme_byte >= 'A' && scheme_byte <= 'Z') ||
                                       (scheme_byte >= 'a' && scheme_byte <= 'z');
                const bool is_other = (scheme_byte >= '0' && scheme_byte <= '9') ||
                                      scheme_byte == '+' || scheme_byte == '-' ||
                                      scheme_byte == '.';
                if (!is_letter && (scheme_index == 0 || !is_other)) {
                    return false;
                }
            }
            is_in_first_segment = false;
        }
    }
    return true;
}

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
    if (!is_iri_reference(href.utf8)) {
        return sxml_function_library_report(call, &invalid_reference_error);
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
    if (!is_iri_reference(base_uri)) {
        return sxml_function_library_report(call, &invalid_reference_error);
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
    /* fn:build-uri, a segment of a hierarchical path: [#0-#20%/\?\#\[\]]. */
    SXML_ESCAPING_KIND_PATH_SEGMENT,
    /* fn:build-uri, a key or value of the query parameters:
       [#0-#20%=&\#\+\[\]]. */
    SXML_ESCAPING_KIND_QUERY_PARAMETER,
    /* fn:build-uri, the fragment: [#0-#20%\#\[\]]. */
    SXML_ESCAPING_KIND_FRAGMENT,
} escaping_kind;

/* The delimiters fn:build-uri escapes in every part: control characters,
   space, %, #, [ and ] (F&O 4.0, section 7.6.3). */
static bool is_build_delimiter(uint8_t byte) {
    return byte <= 0x20 || byte == '%' || byte == '#' || byte == '[' || byte == ']';
}

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
    case SXML_ESCAPING_KIND_PATH_SEGMENT:
        return !is_build_delimiter(byte) && byte != '/' && byte != '?';
    case SXML_ESCAPING_KIND_QUERY_PARAMETER:
        return !is_build_delimiter(byte) && byte != '=' && byte != '&' && byte != '+';
    case SXML_ESCAPING_KIND_FRAGMENT:
        return !is_build_delimiter(byte);
    }
    return false;
}

/* What to escape and how. */
typedef struct escaping_request {
    sxml_byte_view text;
    escaping_kind kind;
} escaping_request;

/* Appends request.text with every byte its kind does not keep written as
   %HH, upper-case hexadecimal of the UTF-8 octet (F&O 4.0, sections 7.2,
   7.3 and 7.4: "must always generate hexadecimal values using the
   upper-case letters A-F"). */
static sxml_status append_escaped(sxml_library_text *text, escaping_request request) {
    static const char hexadecimal_digits[] = "0123456789ABCDEF";
    sxml_status status = SXML_STATUS_OK;
    size_t kept_start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < request.text.byte_count;
         byte_index += 1) {
        const uint8_t byte = request.text.bytes[byte_index];
        if (is_kept(request.kind, byte)) {
            continue;
        }
        if (byte_index > kept_start) {
            status = sxml_function_library_text_append(
                text, (sxml_byte_view){.bytes = request.text.bytes + kept_start,
                                       .byte_count = byte_index - kept_start});
        }
        const uint8_t escape[3] = {'%', (uint8_t)hexadecimal_digits[byte >> 4U],
                                   (uint8_t)hexadecimal_digits[byte & 0x0FU]};
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(
                text, (sxml_byte_view){.bytes = escape, .byte_count = sizeof(escape)});
        }
        kept_start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK && request.text.byte_count > kept_start) {
        status = sxml_function_library_text_append(
            text, (sxml_byte_view){.bytes = request.text.bytes + kept_start,
                                   .byte_count = request.text.byte_count - kept_start});
    }
    return status;
}

/* The $value of an escaping function escaped by its kind; "" for the
   empty sequence. */
static sxml_status call_escaping(const sxml_function_call *call, escaping_kind kind,
                                 sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    status = append_escaped(&text, (escaping_request){.text = value.utf8, .kind = kind});
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* fn:encode-for-uri($value) (F&O 4.0, section 7.2). */
static sxml_status call_encode_for_uri(const sxml_function_call *call, sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_ENCODE_FOR_URI, result);
}

/* fn:iri-to-uri($value) (F&O 4.0, section 7.4). */
static sxml_status call_iri_to_uri(const sxml_function_call *call, sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_IRI_TO_URI, result);
}

/* fn:escape-html-uri($value) (F&O 4.0, section 7.3). */
static sxml_status call_escape_html_uri(const sxml_function_call *call,
                                        sxml_sequence *result) {
    return call_escaping(call, SXML_ESCAPING_KIND_ESCAPE_HTML_URI, result);
}

/* ---------------------------------------------------- Percent decoding */

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static bool is_ascii_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

static uint8_t hexadecimal_value(uint8_t byte) {
    if (is_ascii_digit(byte)) {
        return (uint8_t)(byte - '0');
    }
    return (uint8_t)((byte | 0x20U) - 'a' + 10);
}

/* What to decode (glossary: percent_decoding): form decoding turns each
   plus sign into a space first (F&O 4.0, section 7.6.2). */
typedef struct decoding_request {
    sxml_byte_view encoded;
    bool is_form_decoding;
} decoding_request;

/* The octets of the encoded text, read one at a time (F&O 4.0, section
   7.1): %HH is its octet; a % not followed by two hexadecimal digits,
   with up to two octets after it, stands for U+FFFD. */
typedef struct octet_reader {
    decoding_request request;
    size_t byte_offset;
} octet_reader;

typedef struct decoded_octet {
    bool has_octet;
    /* A broken escape: U+FFFD, not an octet. */
    bool is_broken_escape;
    uint8_t octet;
    /* Encoded bytes it takes. */
    size_t byte_count;
} decoded_octet;

static decoded_octet peek_octet(const octet_reader *reader) {
    const sxml_byte_view encoded = reader->request.encoded;
    const size_t offset = reader->byte_offset;
    if (offset >= encoded.byte_count) {
        return (decoded_octet){.has_octet = false};
    }
    const uint8_t byte = encoded.bytes[offset];
    if (byte != '%') {
        return (decoded_octet){
            .has_octet = true,
            .octet = byte == '+' && reader->request.is_form_decoding ? (uint8_t)' ' : byte,
            .byte_count = 1};
    }
    const size_t following = encoded.byte_count - offset - 1;
    if (following >= 2 && is_hexadecimal_digit(encoded.bytes[offset + 1]) &&
        is_hexadecimal_digit(encoded.bytes[offset + 2])) {
        return (decoded_octet){
            .has_octet = true,
            .octet = (uint8_t)((hexadecimal_value(encoded.bytes[offset + 1]) << 4U) |
                               hexadecimal_value(encoded.bytes[offset + 2])),
            .byte_count = 3};
    }
    return (decoded_octet){.has_octet = true,
                           .is_broken_escape = true,
                           .byte_count = 1 + (following < 2 ? following : 2)};
}

/* The second octet a UTF-8 lead octet allows, and how many follow it
   (Unicode 16.0, table 3-7, well-formed UTF-8 byte sequences). */
typedef struct utf8_lead {
    size_t continuation_count;
    uint8_t lowest_second;
    uint8_t highest_second;
    uint32_t initial_bits;
} utf8_lead;

static utf8_lead read_utf8_lead(uint8_t octet) {
    if (octet >= 0xC2 && octet <= 0xDF) {
        return (utf8_lead){1, 0x80, 0xBF, octet & 0x1FU};
    }
    if (octet >= 0xE0 && octet <= 0xEF) {
        return (utf8_lead){2, octet == 0xE0 ? (uint8_t)0xA0 : (uint8_t)0x80,
                           octet == 0xED ? (uint8_t)0x9F : (uint8_t)0xBF, octet & 0x0FU};
    }
    if (octet >= 0xF0 && octet <= 0xF4) {
        return (utf8_lead){3, octet == 0xF0 ? (uint8_t)0x90 : (uint8_t)0x80,
                           octet == 0xF4 ? (uint8_t)0x8F : (uint8_t)0xBF, octet & 0x07U};
    }
    return (utf8_lead){0, 0, 0, 0};
}

#define SXML_REPLACEMENT_CHARACTER ((uint32_t)0xFFFD)

/*
 * Appends the decoded text (F&O 4.0, section 7.1 "fn:decode-from-uri"):
 * the octets read as UTF-8, where an octet sequence that breaks off adds
 * one U+FFFD for the octets read so far and the breaking octet starts
 * anew; a codepoint that is not a permitted character (U+0000, U+FFFE,
 * U+FFFF) becomes U+FFFD too.
 */
static sxml_status append_decoded(sxml_library_text *text, decoding_request request) {
    octet_reader reader = {.request = request, .byte_offset = 0};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        const decoded_octet lead = peek_octet(&reader);
        if (!lead.has_octet) {
            break;
        }
        reader.byte_offset += lead.byte_count;
        uint32_t codepoint = SXML_REPLACEMENT_CHARACTER;
        if (!lead.is_broken_escape && lead.octet < 0x80) {
            codepoint = lead.octet == 0 ? SXML_REPLACEMENT_CHARACTER : lead.octet;
        } else if (!lead.is_broken_escape) {
            utf8_lead expected = read_utf8_lead(lead.octet);
            bool is_complete = expected.continuation_count > 0;
            codepoint = expected.initial_bits;
            for (size_t continuation_index = 0;
                 is_complete && continuation_index < expected.continuation_count;
                 continuation_index += 1) {
                const decoded_octet next = peek_octet(&reader);
                if (!next.has_octet || next.is_broken_escape ||
                    next.octet < expected.lowest_second || next.octet > expected.highest_second) {
                    is_complete = false;
                    break;
                }
                reader.byte_offset += next.byte_count;
                codepoint = (codepoint << 6U) | (next.octet & 0x3FU);
                expected.lowest_second = 0x80;
                expected.highest_second = 0xBF;
            }
            if (!is_complete || codepoint == 0xFFFE || codepoint == 0xFFFF) {
                codepoint = SXML_REPLACEMENT_CHARACTER;
            }
        }
        status = sxml_function_library_text_append_codepoint(text, codepoint);
    }
    return status;
}

/* Makes the decoded text an xs:string item, owned, in *result. */
static sxml_status make_decoded_string(const sxml_function_call *call, decoding_request request,
                                       sxml_sequence *result) {
    sxml_library_text text = {.item_heap = call->item_heap};
    sxml_status status = append_decoded(&text, request);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* Charges one step per 64 octets of a text the body walks (D89). */
static sxml_status charge_text(const sxml_function_call *call, sxml_byte_view text) {
    return sxml_function_library_charge_steps(call, (uint64_t)text.byte_count / 64U + 1U);
}

/* fn:decode-from-uri($value as xs:string?) as xs:string (F&O 4.0, section
   7.1): "" for the empty sequence; plus signs stay. */
static sxml_status call_decode_from_uri(const sxml_function_call *call,
                                        sxml_sequence *result) {
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = charge_text(call, value.utf8);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return make_decoded_string(
        call, (decoding_request){.encoded = value.utf8, .is_form_decoding = false}, result);
}

/* ------------------------------------------------------- URI structure */

/* A part of a URI that may be absent (the empty sequence). */
typedef struct uri_part {
    bool is_present;
    sxml_byte_view text;
} uri_part;

/* An xs:boolean? part. */
typedef struct uri_flag {
    bool is_present;
    bool is_true;
} uri_flag;

/* The parts fn:parse-uri finds (glossary: uri_structure), as text: the
   fragment and file path still encoded, the port still digits. */
typedef struct uri_structure {
    uri_part scheme;
    uri_flag absolute;
    uri_flag hierarchical;
    uri_part authority;
    uri_part userinfo;
    uri_part host;
    uri_part port;
    uri_part path;
    uri_part query;
    uri_part fragment;
    uri_part filepath;
    /* An authority with "[" but no matching "]" (FOUR0001). */
    bool has_unmatched_bracket;
} uri_structure;

/* The options of fn:parse-uri and fn:build-uri (F&O 4.0, sections 7.6.2
   and 7.6.3). */
typedef struct uri_options {
    bool allows_deprecated_features;
    bool omits_default_ports;
    bool is_unc_path;
} uri_options;

#define SXML_URI_BOOLEAN_OPTION(name_text)                                    \
    SXML_FUNCTION_OPTION(name_text, SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE)

static const sxml_option_definition uri_option_definitions[] = {
    SXML_URI_BOOLEAN_OPTION("allow-deprecated-features"),
    SXML_URI_BOOLEAN_OPTION("omit-default-ports"),
    SXML_URI_BOOLEAN_OPTION("unc-path"),
};

/* Reads the options map at argument_index. */
static sxml_status read_uri_options(const sxml_function_call *call, size_t argument_index,
                                    uri_options *options) {
    sxml_option_values values = {.is_present = {false}};
    const sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = argument_index,
                               .definitions = {uri_option_definitions,
                                               sizeof(uri_option_definitions) /
                                                   sizeof(uri_option_definitions[0])}},
        &values);
    *options = (uri_options){
        .allows_deprecated_features = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &values, .option_index = 0}),
        .omits_default_ports = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &values, .option_index = 1}),
        .is_unc_path = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &values, .option_index = 2})};
    sxml_function_library_release_options(&values);
    return status;
}

static sxml_byte_view view_from(sxml_byte_view text, size_t byte_offset) {
    return (sxml_byte_view){.bytes = text.bytes + byte_offset,
                            .byte_count = text.byte_count - byte_offset};
}

static sxml_byte_view view_before(sxml_byte_view text, size_t byte_count) {
    return (sxml_byte_view){.bytes = text.bytes, .byte_count = byte_count};
}

/* The offset of the first byte in text, or text.byte_count. */
typedef struct byte_search {
    sxml_byte_view text;
    uint8_t byte;
} byte_search;

static size_t find_byte(byte_search search) {
    for (size_t byte_index = 0; byte_index < search.text.byte_count; byte_index += 1) {
        if (search.text.bytes[byte_index] == search.byte) {
            return byte_index;
        }
    }
    return search.text.byte_count;
}

static size_t count_leading_slashes(sxml_byte_view text) {
    size_t slash_count = 0;
    while (slash_count < text.byte_count && text.bytes[slash_count] == '/') {
        slash_count += 1;
    }
    return slash_count;
}

/* ASCII case-insensitive equality ("Comparison of scheme and authority
   components is case insensitive", F&O 4.0, section 7.6.2). */
static bool equals_ignoring_case(sxml_byte_view text, sxml_byte_view lower_case) {
    if (text.byte_count != lower_case.byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        const uint8_t folded = byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte | 0x20U) : byte;
        if (folded != lower_case.bytes[byte_index]) {
            return false;
        }
    }
    return true;
}

static bool is_scheme(uri_part scheme, sxml_byte_view lower_case) {
    return scheme.is_present && equals_ignoring_case(scheme.text, lower_case);
}

/* The schemes F&O 4.0, section 7.6 requires to be non-hierarchical. */
static const sxml_byte_view non_hierarchical_schemes[] = {
    SXML_BYTE_VIEW_INITIALIZER("jar"), SXML_BYTE_VIEW_INITIALIZER("mailto"),
    SXML_BYTE_VIEW_INITIALIZER("news"), SXML_BYTE_VIEW_INITIALIZER("tag"),
    SXML_BYTE_VIEW_INITIALIZER("tel"), SXML_BYTE_VIEW_INITIALIZER("urn")};

/* The schemes sxml knows to be hierarchical (implementation-defined). */
static const sxml_byte_view hierarchical_schemes[] = {
    SXML_BYTE_VIEW_INITIALIZER("file"), SXML_BYTE_VIEW_INITIALIZER("ftp"),
    SXML_BYTE_VIEW_INITIALIZER("http"), SXML_BYTE_VIEW_INITIALIZER("https")};

/* A list of lower-case scheme names. */
typedef struct scheme_list {
    const sxml_byte_view *schemes;
    size_t scheme_count;
} scheme_list;

static bool is_listed_scheme(uri_part scheme, scheme_list list) {
    for (size_t scheme_index = 0; scheme_index < list.scheme_count; scheme_index += 1) {
        if (is_scheme(scheme, list.schemes[scheme_index])) {
            return true;
        }
    }
    return false;
}

static bool is_known_non_hierarchical(uri_part scheme) {
    return is_listed_scheme(
        scheme, (scheme_list){non_hierarchical_schemes,
                              sizeof(non_hierarchical_schemes) / sizeof(non_hierarchical_schemes[0])});
}

static bool is_known_hierarchical(uri_part scheme) {
    return is_listed_scheme(
        scheme, (scheme_list){hierarchical_schemes,
                              sizeof(hierarchical_schemes) / sizeof(hierarchical_schemes[0])});
}

/* The value of a port of at most 18 digits; false for anything else. */
typedef struct port_number {
    bool is_valid;
    uint64_t number;
} port_number;

static port_number read_port_number(sxml_byte_view digits) {
    port_number port = {.is_valid = digits.byte_count > 0 && digits.byte_count <= 18,
                        .number = 0};
    for (size_t byte_index = 0; port.is_valid && byte_index < digits.byte_count;
         byte_index += 1) {
        port.is_valid = is_ascii_digit(digits.bytes[byte_index]);
        port.number = port.number * 10U + (uint64_t)(digits.bytes[byte_index] - '0');
    }
    return port;
}

/* The scheme and port of a URI. */
typedef struct scheme_port {
    uri_part scheme;
    uint64_t port;
} scheme_port;

/* The default ports F&O 4.0 names for omit-default-ports: http 80, https
   443, ftp 21, ssh 22. */
static bool is_default_port(scheme_port candidate) {
    return (is_scheme(candidate.scheme, SXML_BYTE_VIEW_FROM_LITERAL("http")) &&
            candidate.port == 80) ||
           (is_scheme(candidate.scheme, SXML_BYTE_VIEW_FROM_LITERAL("https")) &&
            candidate.port == 443) ||
           (is_scheme(candidate.scheme, SXML_BYTE_VIEW_FROM_LITERAL("ftp")) &&
            candidate.port == 21) ||
           (is_scheme(candidate.scheme, SXML_BYTE_VIEW_FROM_LITERAL("ssh")) &&
            candidate.port == 22);
}

/* True when userinfo has a colon followed by at least one character: a
   password, a deprecated feature (RFC 3986, section 3.2.1). */
static bool has_password(sxml_byte_view userinfo) {
    const size_t colon = find_byte((byte_search){.text = userinfo, .byte = ':'});
    return colon + 1 < userinfo.byte_count;
}

/* ------------------------------------------------------------ Parsing */

/* The memory of one parse: the value with backslashes made slashes, and
   the string rewritten for a drive letter. Parts are views into these. */
typedef struct uri_parser {
    sxml_library_text working;
    sxml_library_text drive;
    uri_options options;
} uri_parser;

/* Host and port of an authority (F&O 4.0, section 7.6.2): tried after
   the userinfo first, then on the whole authority, as the optional
   group (([^@]*)@)? of the expressions does. */
typedef struct host_port {
    bool is_matched;
    sxml_byte_view host;
    uri_part port;
} host_port;

/* ^(\[[^\]]*\])(:([^:]*))?$ on candidate. */
static host_port match_bracketed_host(sxml_byte_view candidate) {
    host_port match = {.is_matched = false};
    if (candidate.byte_count == 0 || candidate.bytes[0] != '[') {
        return match;
    }
    const size_t close = find_byte((byte_search){.text = candidate, .byte = ']'});
    if (close == candidate.byte_count) {
        return match;
    }
    const sxml_byte_view after = view_from(candidate, close + 1);
    if (after.byte_count == 0) {
        return (host_port){.is_matched = true, .host = view_before(candidate, close + 1)};
    }
    const sxml_byte_view port = view_from(after, 1);
    if (after.bytes[0] != ':' ||
        find_byte((byte_search){.text = port, .byte = ':'}) != port.byte_count) {
        return match;
    }
    return (host_port){.is_matched = true,
                       .host = view_before(candidate, close + 1),
                       .port = {.is_present = true, .text = port}};
}

/* ^([^:]+)(:([^:]*))?$ on candidate. */
static host_port match_plain_host(sxml_byte_view candidate) {
    host_port match = {.is_matched = false};
    const size_t colon = find_byte((byte_search){.text = candidate, .byte = ':'});
    if (colon == 0) {
        return match;
    }
    if (colon == candidate.byte_count) {
        return (host_port){.is_matched = true, .host = candidate};
    }
    const sxml_byte_view port = view_from(candidate, colon + 1);
    if (find_byte((byte_search){.text = port, .byte = ':'}) != port.byte_count) {
        return match;
    }
    return (host_port){.is_matched = true,
                       .host = view_before(candidate, colon),
                       .port = {.is_present = true, .text = port}};
}

/* Userinfo, host and port from the authority. */
static void split_authority(const uri_parser *parser, uri_structure *structure) {
    const sxml_byte_view authority = structure->authority.text;
    const size_t at_sign = find_byte((byte_search){.text = authority, .byte = '@'});
    const bool has_userinfo = at_sign < authority.byte_count;
    if (has_userinfo) {
        const sxml_byte_view userinfo = view_before(authority, at_sign);
        /* A non-empty password is dropped unless deprecated features are
           allowed. */
        structure->userinfo = (uri_part){
            .is_present = parser->options.allows_deprecated_features || !has_password(userinfo),
            .text = userinfo};
    }
    const sxml_byte_view candidates[2] = {has_userinfo ? view_from(authority, at_sign + 1)
                                                       : authority,
                                          authority};
    const size_t candidate_count = has_userinfo ? 2 : 1;
    host_port match = {.is_matched = false};
    for (size_t candidate_index = 0; !match.is_matched && candidate_index < candidate_count;
         candidate_index += 1) {
        match = match_bracketed_host(candidates[candidate_index]);
    }
    if (!match.is_matched) {
        for (size_t candidate_index = 0; candidate_index < candidate_count;
             candidate_index += 1) {
            if (candidates[candidate_index].byte_count > 0 &&
                candidates[candidate_index].bytes[0] == '[') {
                structure->has_unmatched_bracket = true;
                return;
            }
        }
    }
    for (size_t candidate_index = 0; !match.is_matched && candidate_index < candidate_count;
         candidate_index += 1) {
        match = match_plain_host(candidates[candidate_index]);
    }
    if (!match.is_matched) {
        return;
    }
    structure->host = (uri_part){.is_present = true, .text = match.host};
    const port_number port = read_port_number(match.port.text);
    /* A port that is not a number is left out: the field is an
       xs:integer?. */
    structure->port = (uri_part){
        .is_present = match.port.is_present && port.is_valid &&
                      !(parser->options.omits_default_ports &&
                        is_default_port((scheme_port){.scheme = structure->scheme,
                                                      .port = port.number})),
        .text = match.port.text};
}

/* Strips the fragment and the query, then finds the scheme (F&O 4.0,
   section 7.6.2, steps 2 and 3); writes what is left to *rest. */
static void split_scheme(sxml_byte_view text, uri_structure *structure, sxml_byte_view *rest) {
    const size_t hash = find_byte((byte_search){.text = text, .byte = '#'});
    if (hash < text.byte_count) {
        const sxml_byte_view fragment = view_from(text, hash + 1);
        structure->fragment = (uri_part){.is_present = fragment.byte_count > 0, .text = fragment};
        text = view_before(text, hash);
    }
    const size_t question_mark = find_byte((byte_search){.text = text, .byte = '?'});
    if (question_mark < text.byte_count) {
        const sxml_byte_view query = view_from(text, question_mark + 1);
        structure->query = (uri_part){.is_present = query.byte_count > 0, .text = query};
        text = view_before(text, question_mark);
    }
    /* ^([a-zA-Z][A-Za-z0-9\+\-\.]+):(.*)$ */
    size_t scheme_end = 0;
    if (text.byte_count > 0 && is_ascii_letter(text.bytes[0])) {
        scheme_end = 1;
        while (scheme_end < text.byte_count &&
               (is_ascii_letter(text.bytes[scheme_end]) || is_ascii_digit(text.bytes[scheme_end]) ||
                text.bytes[scheme_end] == '+' || text.bytes[scheme_end] == '-' ||
                text.bytes[scheme_end] == '.')) {
            scheme_end += 1;
        }
    }
    if (scheme_end >= 2 && scheme_end < text.byte_count && text.bytes[scheme_end] == ':') {
        structure->scheme = (uri_part){.is_present = true, .text = view_before(text, scheme_end)};
        text = view_from(text, scheme_end + 1);
    }
    *rest = text;
}

/* A file path, a drive letter or a UNC path makes the scheme file (F&O
   4.0, section 7.6.2, step 5): ^(/)*([a-zA-Z][:|].*)$ (the
   specification writes "/" and "*" without parentheses) makes the string "/"
   and the match with its second character a colon. */
static sxml_status find_file_scheme(uri_parser *parser, uri_structure *structure,
                                    sxml_byte_view *rest) {
    static const sxml_byte_view file_scheme = SXML_BYTE_VIEW_INITIALIZER("file");
    if (structure->scheme.is_present && !is_scheme(structure->scheme, file_scheme)) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = *rest;
    const size_t slash_count = count_leading_slashes(text);
    if (slash_count + 1 < text.byte_count && is_ascii_letter(text.bytes[slash_count]) &&
        (text.bytes[slash_count + 1] == ':' || text.bytes[slash_count + 1] == '|')) {
        if (!structure->scheme.is_present) {
            structure->scheme = (uri_part){.is_present = true, .text = file_scheme};
        }
        const uint8_t prefix[3] = {'/', text.bytes[slash_count], ':'};
        sxml_status status = sxml_function_library_text_append(
            &parser->drive, (sxml_byte_view){.bytes = prefix, .byte_count = sizeof(prefix)});
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&parser->drive,
                                                       view_from(text, slash_count + 2));
        }
        if (status == SXML_STATUS_OK) {
            *rest = sxml_function_library_text_view(&parser->drive);
        }
        return status;
    }
    if (parser->options.is_unc_path && !structure->scheme.is_present) {
        structure->scheme = (uri_part){.is_present = true, .text = file_scheme};
    }
    return SXML_STATUS_OK;
}

/* The path and file path of a file URI (F&O 4.0, section 7.6.2, step 8). */
static void split_file_path(const uri_parser *parser, uri_structure *structure,
                            sxml_byte_view *rest) {
    const sxml_byte_view text = *rest;
    const size_t slash_count = count_leading_slashes(text);
    if (parser->options.is_unc_path && slash_count >= 2 && slash_count < text.byte_count) {
        /* ^(/)*(//[^/].*)$ */
        *rest = view_from(text, slash_count - 2);
    } else if (slash_count >= 1 && slash_count + 2 < text.byte_count &&
               is_ascii_letter(text.bytes[slash_count]) && text.bytes[slash_count + 1] == ':' &&
               text.bytes[slash_count + 2] == '/') {
        /* ^/(/)*[A-Za-z]:/ keeps one slash; the file path none. */
        *rest = view_from(text, slash_count - 1);
        structure->filepath = (uri_part){.is_present = true, .text = view_from(text, slash_count)};
        return;
    } else if (slash_count > 1) {
        *rest = view_from(text, slash_count - 1);
    }
    structure->filepath = (uri_part){.is_present = true, .text = *rest};
}

/* The authority of a hierarchical URI: ^//([^/]+)$ or ^//([^/]*)(/.*)$. */
static void split_hierarchical(uri_structure *structure, sxml_byte_view *rest) {
    const sxml_byte_view text = *rest;
    if (text.byte_count < 2 || text.bytes[0] != '/' || text.bytes[1] != '/') {
        return;
    }
    const sxml_byte_view after = view_from(text, 2);
    const size_t slash = find_byte((byte_search){.text = after, .byte = '/'});
    if (slash == after.byte_count && after.byte_count == 0) {
        return;
    }
    /* A zero-length authority is the empty sequence, as QT4 fn-parse-uri-048b
       expects. */
    structure->authority = (uri_part){.is_present = slash > 0, .text = view_before(after, slash)};
    *rest = view_from(after, slash);
}

/*
 * Parses value into *structure by the transformations of F&O 4.0, section
 * 7.6.2, in order. The parts are views into value and the parser's
 * memory.
 */
static sxml_status parse_structure(uri_parser *parser, sxml_byte_view value,
                                   uri_structure *structure) {
    *structure = (uri_structure){.has_unmatched_bracket = false};
    /* Step 1: backslashes become slashes. */
    sxml_status status = sxml_function_library_text_append(&parser->working, value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t byte_index = 0; byte_index < parser->working.byte_count; byte_index += 1) {
        if (parser->working.bytes[byte_index] == '\\') {
            parser->working.bytes[byte_index] = '/';
        }
    }
    sxml_byte_view rest = {.bytes = NULL, .byte_count = 0};
    split_scheme(sxml_function_library_text_view(&parser->working), structure, &rest);
    /* Step 4. */
    structure->absolute = (uri_flag){
        .is_present = structure->scheme.is_present && !structure->fragment.is_present,
        .is_true = true};
    status = find_file_scheme(parser, structure, &rest);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Step 6: known schemes; otherwise the string decides. */
    if (is_known_non_hierarchical(structure->scheme)) {
        structure->hierarchical = (uri_flag){.is_present = true, .is_true = false};
    } else if (is_known_hierarchical(structure->scheme)) {
        structure->hierarchical = (uri_flag){.is_present = true, .is_true = true};
    } else if (rest.byte_count > 0) {
        structure->hierarchical = (uri_flag){.is_present = true, .is_true = rest.bytes[0] == '/'};
    }
    /* Step 7. */
    if (structure->hierarchical.is_present && !structure->hierarchical.is_true) {
        structure->absolute.is_present = false;
    }
    /* Step 8. */
    if (is_scheme(structure->scheme, SXML_BYTE_VIEW_FROM_LITERAL("file"))) {
        split_file_path(parser, structure, &rest);
    } else if (structure->hierarchical.is_present && structure->hierarchical.is_true) {
        split_hierarchical(structure, &rest);
    }
    if (structure->authority.is_present) {
        split_authority(parser, structure);
    }
    /* Step 16: the path; without a scheme it is the file path too. */
    structure->path = (uri_part){.is_present = rest.byte_count > 0, .text = rest};
    if (!structure->scheme.is_present && rest.byte_count > 0) {
        structure->filepath = structure->path;
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------- Parse results */

/* The field values of fn:uri-structure-record, in field order (F&O 4.0,
   section 7.6.1). */
enum {
    SXML_URI_FIELD_URI,
    SXML_URI_FIELD_SCHEME,
    SXML_URI_FIELD_ABSOLUTE,
    SXML_URI_FIELD_HIERARCHICAL,
    SXML_URI_FIELD_AUTHORITY,
    SXML_URI_FIELD_USERINFO,
    SXML_URI_FIELD_HOST,
    SXML_URI_FIELD_PORT,
    SXML_URI_FIELD_PATH,
    SXML_URI_FIELD_QUERY,
    SXML_URI_FIELD_FRAGMENT,
    SXML_URI_FIELD_PATH_SEGMENTS,
    SXML_URI_FIELD_QUERY_PARAMETERS,
    SXML_URI_FIELD_FILEPATH,
    SXML_URI_FIELD_COUNT,
};

/* The names of the fields, in field order. */
static const sxml_byte_view uri_field_names[SXML_URI_FIELD_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("uri"),           SXML_BYTE_VIEW_INITIALIZER("scheme"),
    SXML_BYTE_VIEW_INITIALIZER("absolute"),      SXML_BYTE_VIEW_INITIALIZER("hierarchical"),
    SXML_BYTE_VIEW_INITIALIZER("authority"),     SXML_BYTE_VIEW_INITIALIZER("userinfo"),
    SXML_BYTE_VIEW_INITIALIZER("host"),          SXML_BYTE_VIEW_INITIALIZER("port"),
    SXML_BYTE_VIEW_INITIALIZER("path"),          SXML_BYTE_VIEW_INITIALIZER("query"),
    SXML_BYTE_VIEW_INITIALIZER("fragment"),      SXML_BYTE_VIEW_INITIALIZER("path-segments"),
    SXML_BYTE_VIEW_INITIALIZER("query-parameters"), SXML_BYTE_VIEW_INITIALIZER("filepath"),
};

static sxml_status make_part_string(const sxml_function_call *call, uri_part part,
                                    sxml_sequence *result) {
    if (!part.is_present) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return sxml_function_library_make_string(call->item_heap, part.text, result);
}

static sxml_status make_decoded_part(const sxml_function_call *call, uri_part part,
                                     sxml_sequence *result) {
    if (!part.is_present) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return make_decoded_string(
        call, (decoding_request){.encoded = part.text, .is_form_decoding = false}, result);
}

static sxml_sequence make_flag(uri_flag flag) {
    if (!flag.is_present) {
        return sxml_sequence_empty();
    }
    return sxml_function_library_make_boolean((sxml_boolean_result){.is_true = flag.is_true});
}

static sxml_status make_port(const sxml_function_call *call, uri_part part,
                             sxml_sequence *result) {
    *result = sxml_sequence_empty();
    if (!part.is_present) {
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
            .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                      .datum_details = {.integer = (int64_t)read_port_number(part.text).number}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The path tokenized on "/", each token URI decoded; () for no path. */
static sxml_status make_path_segments(const sxml_function_call *call, uri_part path,
                                      sxml_sequence *result) {
    *result = sxml_sequence_empty();
    if (!path.is_present) {
        return SXML_STATUS_OK;
    }
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    sxml_byte_view rest = path.text;
    bool has_token = status == SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && has_token) {
        const size_t slash = find_byte((byte_search){.text = rest, .byte = '/'});
        sxml_sequence segment = sxml_sequence_empty();
        status = make_decoded_string(
            call,
            (decoding_request){.encoded = view_before(rest, slash), .is_form_decoding = false},
            &segment);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, segment, call->error_record);
        }
        sxml_sequence_release(segment);
        has_token = slash < rest.byte_count;
        if (has_token) {
            rest = view_from(rest, slash + 1);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* The single item of a singleton sequence, retained into *item; the
   sequence is released. */
static sxml_status take_single_item(const sxml_function_call *call, sxml_sequence sequence,
                                    sxml_item *item) {
    const sxml_status status = sxml_sequence_get_item(
        sequence, (sxml_item_request){.item_index = 0, .error_record = call->error_record}, item);
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(*item);
    }
    sxml_sequence_release(sequence);
    return status;
}

/* One key=value of a query, its key and value form decoded; pairs with
   the same key are chained, first to last. */
typedef struct query_pair {
    sxml_item key;
    sxml_item value;
    size_t next_pair_index;
    size_t last_pair_index;
} query_pair;

#define SXML_NO_PAIR SIZE_MAX

/* The pairs of a query while query-parameters is built. */
typedef struct query_pairs {
    sxml_item_heap *item_heap;
    query_pair *pairs;
    size_t pair_count;
    size_t pair_capacity;
    /* The first pair of each key, in the order keys first appear. */
    size_t *first_pair_indices;
    size_t key_count;
    /* Key to the index of its first pair (an xs:integer). */
    sxml_item key_index;
} query_pairs;

static void release_query_pairs(query_pairs *pairs) {
    for (size_t pair_index = 0; pair_index < pairs->pair_count; pair_index += 1) {
        sxml_item_release(pairs->pairs[pair_index].key);
        sxml_item_release(pairs->pairs[pair_index].value);
    }
    sxml_item_heap_release_bytes(
        pairs->item_heap,
        (sxml_heap_bytes){.memory = pairs->pairs,
                          .byte_count = pairs->pair_capacity * sizeof(query_pair)});
    sxml_item_heap_release_bytes(
        pairs->item_heap,
        (sxml_heap_bytes){.memory = pairs->first_pair_indices,
                          .byte_count = pairs->pair_capacity * sizeof(size_t)});
    if (pairs->key_index.representation != 0) {
        sxml_item_release(pairs->key_index);
    }
}

static sxml_status allocate_array(sxml_item_heap *item_heap, sxml_allocation_request request,
                                  void **memory) {
    return sxml_item_heap_allocate_bytes(item_heap, request, memory);
}

/* Makes a token's key and value strings, form decoded (F&O 4.0, section
   7.6.2, step 17): before and after the first "=", or "" and the whole
   token. */
static sxml_status add_query_pair(const sxml_function_call *call, sxml_byte_view token,
                                  query_pairs *pairs) {
    const size_t equals_sign = find_byte((byte_search){.text = token, .byte = '='});
    const bool has_key = equals_sign < token.byte_count;
    const sxml_byte_view key_text = has_key ? view_before(token, equals_sign)
                                            : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const sxml_byte_view value_text = has_key ? view_from(token, equals_sign + 1) : token;
    sxml_sequence key = sxml_sequence_empty();
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = make_decoded_string(
        call, (decoding_request){.encoded = key_text, .is_form_decoding = true}, &key);
    if (status == SXML_STATUS_OK) {
        status = make_decoded_string(
            call, (decoding_request){.encoded = value_text, .is_form_decoding = true}, &value);
    }
    sxml_item key_item = {.representation = 0};
    sxml_item value_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = take_single_item(call, key, &key_item);
        key = sxml_sequence_empty();
    }
    if (status == SXML_STATUS_OK) {
        status = take_single_item(call, value, &value_item);
        value = sxml_sequence_empty();
    }
    sxml_sequence_release(key);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        if (key_item.representation != 0) {
            sxml_item_release(key_item);
        }
        return status;
    }
    const size_t pair_index = pairs->pair_count;
    pairs->pairs[pair_index] = (query_pair){.key = key_item,
                                            .value = value_item,
                                            .next_pair_index = SXML_NO_PAIR,
                                            .last_pair_index = pair_index};
    pairs->pair_count += 1;
    /* Chain the pair to the first one with its key, or record a new key. */
    sxml_map_lookup lookup = {.is_found = false};
    status = sxml_map_find(pairs->key_index, key_item, &lookup);
    if (status == SXML_STATUS_OK && lookup.is_found) {
        sxml_item first_item = {.representation = 0};
        status = sxml_sequence_get_item(
            lookup.entry.value,
            (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &first_item);
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(first_item, &datum);
        }
        if (status == SXML_STATUS_OK) {
            query_pair *first = &pairs->pairs[(size_t)datum.datum_details.integer];
            pairs->pairs[first->last_pair_index].next_pair_index = pair_index;
            first->last_pair_index = pair_index;
        }
        return status;
    }
    sxml_item index_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                          .datum_details = {.integer = (int64_t)pair_index}}},
            &index_item);
    }
    sxml_item new_index = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_sequence index_value = sxml_sequence_from_item(index_item);
        status = sxml_map_put(call->item_heap,
                              &(sxml_map_update){.map = pairs->key_index,
                                                 .key = key_item,
                                                 .value = index_value},
                              &new_index);
        sxml_sequence_release(index_value);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_release(pairs->key_index);
        pairs->key_index = new_index;
        pairs->first_pair_indices[pairs->key_count] = pair_index;
        pairs->key_count += 1;
    }
    return status;
}

/* The map of each key to the values of its pairs, keys in the order they
   first appear. */
static sxml_status finish_query_parameters(const sxml_function_call *call,
                                           const query_pairs *pairs, sxml_sequence *result) {
    void *entry_memory = NULL;
    void *item_memory = NULL;
    sxml_status status = allocate_array(
        call->item_heap,
        (sxml_allocation_request){.byte_count = pairs->key_count * sizeof(sxml_map_entry),
                                  .alignment = alignof(sxml_map_entry)},
        &entry_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            call->item_heap,
            (sxml_allocation_request){.byte_count = pairs->pair_count * sizeof(sxml_item),
                                      .alignment = alignof(sxml_item)},
            &item_memory);
    }
    sxml_map_entry *entries = entry_memory;
    sxml_item *values = item_memory;
    size_t entry_count = 0;
    for (; status == SXML_STATUS_OK && entry_count < pairs->key_count; entry_count += 1) {
        size_t value_count = 0;
        for (size_t pair_index = pairs->first_pair_indices[entry_count];
             pair_index != SXML_NO_PAIR; pair_index = pairs->pairs[pair_index].next_pair_index) {
            values[value_count] = pairs->pairs[pair_index].value;
            value_count += 1;
        }
        entries[entry_count].key = pairs->pairs[pairs->first_pair_indices[entry_count]].key;
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = values, .item_count = value_count},
            &entries[entry_count].value);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = entries,
                                                .map_entry_count = entry_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                .error_record = call->error_record},
            &map);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_sequence_release(entries[entry_index].value);
    }
    sxml_item_heap_release_bytes(
        call->item_heap,
        (sxml_heap_bytes){.memory = entry_memory,
                          .byte_count = pairs->key_count * sizeof(sxml_map_entry)});
    sxml_item_heap_release_bytes(
        call->item_heap,
        (sxml_heap_bytes){.memory = item_memory,
                          .byte_count = pairs->pair_count * sizeof(sxml_item)});
    return status;
}

/* query-parameters (F&O 4.0, section 7.6.2, step 17): the query tokenized
   on "&"; () without a query. */
static sxml_status make_query_parameters(const sxml_function_call *call, uri_part query,
                                         sxml_sequence *result) {
    *result = sxml_sequence_empty();
    if (!query.is_present) {
        return SXML_STATUS_OK;
    }
    size_t token_count = 1;
    for (size_t byte_index = 0; byte_index < query.text.byte_count; byte_index += 1) {
        token_count += query.text.bytes[byte_index] == '&' ? 1U : 0U;
    }
    query_pairs pairs = {.item_heap = call->item_heap,
                         .pairs = NULL,
                         .pair_count = 0,
                         .pair_capacity = token_count,
                         .first_pair_indices = NULL,
                         .key_count = 0,
                         .key_index = {.representation = 0}};
    void *pair_memory = NULL;
    void *index_memory = NULL;
    sxml_status status = token_count > SIZE_MAX / sizeof(query_pair)
                             ? SXML_STATUS_ARITHMETIC_OVERFLOW
                             : allocate_array(call->item_heap,
                                              (sxml_allocation_request){
                                                  .byte_count = token_count * sizeof(query_pair),
                                                  .alignment = alignof(query_pair)},
                                              &pair_memory);
    pairs.pairs = pair_memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(call->item_heap,
                                (sxml_allocation_request){.byte_count = token_count * sizeof(size_t),
                                                          .alignment = alignof(size_t)},
                                &index_memory);
        pairs.first_pair_indices = index_memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                                .error_record = call->error_record},
            &pairs.key_index);
    }
    sxml_byte_view rest = query.text;
    for (size_t token_index = 0; status == SXML_STATUS_OK && token_index < token_count;
         token_index += 1) {
        const size_t ampersand = find_byte((byte_search){.text = rest, .byte = '&'});
        status = add_query_pair(call, view_before(rest, ampersand), &pairs);
        if (ampersand < rest.byte_count) {
            rest = view_from(rest, ampersand + 1);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = finish_query_parameters(call, &pairs, result);
    }
    release_query_pairs(&pairs);
    return status;
}

static const sxml_static_error unmatched_bracket_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOUR0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The authority of the URI has \"[\" without a matching \"]\" (F&O 4.0, fn:parse-uri)"),
};

/* The record of a parsed structure: one value per field. */
static sxml_status make_uri_record(const sxml_function_call *call,
                                   const uri_structure *structure, sxml_sequence *result) {
    sxml_sequence field_values[SXML_URI_FIELD_COUNT];
    for (size_t field_index = 0; field_index < SXML_URI_FIELD_COUNT; field_index += 1) {
        field_values[field_index] = sxml_sequence_empty();
    }
    sxml_string value;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap, value.utf8,
                                                   &field_values[SXML_URI_FIELD_URI]);
    }
    const struct {
        size_t field_index;
        uri_part part;
    } string_fields[] = {
        {SXML_URI_FIELD_SCHEME, structure->scheme},       {SXML_URI_FIELD_AUTHORITY, structure->authority},
        {SXML_URI_FIELD_USERINFO, structure->userinfo},   {SXML_URI_FIELD_HOST, structure->host},
        {SXML_URI_FIELD_PATH, structure->path},           {SXML_URI_FIELD_QUERY, structure->query},
    };
    for (size_t string_index = 0;
         status == SXML_STATUS_OK && string_index < sizeof(string_fields) / sizeof(string_fields[0]);
         string_index += 1) {
        status = make_part_string(call, string_fields[string_index].part,
                                  &field_values[string_fields[string_index].field_index]);
    }
    field_values[SXML_URI_FIELD_ABSOLUTE] = make_flag(structure->absolute);
    field_values[SXML_URI_FIELD_HIERARCHICAL] = make_flag(structure->hierarchical);
    if (status == SXML_STATUS_OK) {
        status = make_port(call, structure->port, &field_values[SXML_URI_FIELD_PORT]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_decoded_part(call, structure->fragment, &field_values[SXML_URI_FIELD_FRAGMENT]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_decoded_part(call, structure->filepath, &field_values[SXML_URI_FIELD_FILEPATH]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_path_segments(call, structure->path,
                                    &field_values[SXML_URI_FIELD_PATH_SEGMENTS]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_query_parameters(call, structure->query,
                                       &field_values[SXML_URI_FIELD_QUERY_PARAMETERS]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("uri-structure-record"),
                .field_values = {.sequences = field_values,
                                 .sequence_count = SXML_URI_FIELD_COUNT}},
            result);
    }
    for (size_t field_index = 0; field_index < SXML_URI_FIELD_COUNT; field_index += 1) {
        sxml_sequence_release(field_values[field_index]);
    }
    return status;
}

/* fn:parse-uri($value as xs:string?, $options as map(*)? := {}) as
   fn:uri-structure-record? (F&O 4.0, section 7.6.2): the empty sequence
   for an empty $value. */
static sxml_status call_parse_uri(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    uri_parser parser = {.working = {.item_heap = call->item_heap},
                         .drive = {.item_heap = call->item_heap}};
    status = read_uri_options(call, 1, &parser.options);
    sxml_string value;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 0, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = charge_text(call, value.utf8);
    }
    uri_structure structure;
    if (status == SXML_STATUS_OK) {
        status = parse_structure(&parser, value.utf8, &structure);
    }
    if (status == SXML_STATUS_OK && structure.has_unmatched_bracket) {
        status = sxml_function_library_report(call, &unmatched_bracket_error);
    }
    if (status == SXML_STATUS_OK) {
        status = make_uri_record(call, &structure, result);
    }
    sxml_function_library_text_release(&parser.working);
    sxml_function_library_text_release(&parser.drive);
    return status;
}

/* ------------------------------------------------------------ Building */

/* A field of the $parts record: present when supplied and not empty
   (F&O 4.0, section 7.6.3). The value is borrowed from the record. */
typedef struct uri_field {
    bool is_present;
    sxml_sequence value;
} uri_field;

/* The fields of $parts by position in uri_field_names. */
typedef struct uri_fields {
    uri_field fields[SXML_URI_FIELD_COUNT];
} uri_fields;

static sxml_status read_uri_fields(const sxml_function_call *call, sxml_item parts,
                                   uri_fields *fields) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < SXML_URI_FIELD_COUNT;
         field_index += 1) {
        fields->fields[field_index] = (uri_field){.is_present = false,
                                                  .value = sxml_sequence_empty()};
        sxml_sequence key = sxml_sequence_empty();
        status = sxml_function_library_make_string(call->item_heap, uri_field_names[field_index],
                                                   &key);
        sxml_item key_item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = take_single_item(call, key, &key_item);
            key = sxml_sequence_empty();
        }
        sxml_sequence_release(key);
        sxml_map_lookup lookup = {.is_found = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_find(parts, key_item, &lookup);
            sxml_item_release(key_item);
        }
        sxml_emptiness emptiness = {.is_empty = true};
        if (status == SXML_STATUS_OK && lookup.is_found) {
            status = sxml_sequence_test_emptiness(lookup.entry.value, call->error_record,
                                                  &emptiness);
        }
        if (status == SXML_STATUS_OK && !emptiness.is_empty) {
            fields->fields[field_index] = (uri_field){.is_present = true,
                                                      .value = lookup.entry.value};
        }
    }
    return status;
}

/* The single atomic datum of a present field (string, boolean or
   integer by the record type). */
typedef struct field_datum_request {
    const uri_fields *fields;
    size_t field_index;
} field_datum_request;

static sxml_status read_field_datum(const sxml_function_call *call, field_datum_request request,
                                    sxml_atomic_datum *datum) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        request.fields->fields[request.field_index].value,
        (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, datum);
    }
    return status;
}

/* The text of a present xs:string field; absent otherwise. */
static sxml_status read_field_text(const sxml_function_call *call, field_datum_request request,
                                   uri_part *part) {
    *part = (uri_part){.is_present = false};
    if (!request.fields->fields[request.field_index].is_present) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    const sxml_status status = read_field_datum(call, request, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING) {
        *part = (uri_part){.is_present = true, .text = datum.datum_details.string.utf8};
    }
    return status;
}

/* The URI under construction and what it is made from. */
typedef struct uri_builder {
    sxml_library_text text;
    const uri_fields *fields;
    uri_options options;
    uri_part scheme;
    bool is_non_hierarchical;
} uri_builder;

static sxml_status append_text(uri_builder *builder, sxml_byte_view text) {
    return sxml_function_library_text_append(&builder->text, text);
}

/* Each item of a present field appended in turn, the kind escaped, with
   separator between items. */
typedef struct joined_field {
    size_t field_index;
    sxml_byte_view separator;
    bool is_escaped;
    escaping_kind kind;
} joined_field;

static sxml_status append_joined(const sxml_function_call *call, uri_builder *builder,
                                 joined_field joined) {
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(builder->fields->fields[joined.field_index].value);
    sxml_status status = SXML_STATUS_OK;
    bool is_first = true;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK && !is_first) {
            status = append_text(builder, joined.separator);
        }
        if (status == SXML_STATUS_OK) {
            status = joined.is_escaped
                         ? append_escaped(&builder->text,
                                          (escaping_request){.text = datum.datum_details.string.utf8,
                                                             .kind = joined.kind})
                         : append_text(builder, datum.datum_details.string.utf8);
        }
        is_first = false;
    }
    return status;
}

/* The scheme and its delimiter (F&O 4.0, section 7.6.3). */
static sxml_status append_scheme(uri_builder *builder) {
    if (!builder->scheme.is_present) {
        return SXML_STATUS_OK;
    }
    sxml_status status = append_text(builder, builder->scheme.text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (builder->is_non_hierarchical) {
        return append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    if (builder->options.is_unc_path &&
        is_scheme(builder->scheme, SXML_BYTE_VIEW_FROM_LITERAL("file"))) {
        return append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL(":////"));
    }
    return append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("://"));
}

/* userinfo@host:port when any of them is present, else the authority. A
   URI without a scheme gets "//" before an authority, so that it reads
   back as one (F&O 4.0 adds nothing there; D140). */
static sxml_status append_authority(const sxml_function_call *call, uri_builder *builder) {
    uri_part userinfo;
    uri_part host;
    uri_part authority;
    sxml_status status = read_field_text(
        call, (field_datum_request){builder->fields, SXML_URI_FIELD_USERINFO}, &userinfo);
    if (status == SXML_STATUS_OK) {
        status = read_field_text(call, (field_datum_request){builder->fields, SXML_URI_FIELD_HOST},
                                 &host);
    }
    if (status == SXML_STATUS_OK) {
        status = read_field_text(
            call, (field_datum_request){builder->fields, SXML_URI_FIELD_AUTHORITY}, &authority);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (userinfo.is_present && !builder->options.allows_deprecated_features &&
        has_password(userinfo.text)) {
        userinfo.is_present = false;
    }
    bool has_port = builder->fields->fields[SXML_URI_FIELD_PORT].is_present;
    sxml_item port_item = {.representation = 0};
    if (has_port) {
        status = sxml_sequence_get_item(
            builder->fields->fields[SXML_URI_FIELD_PORT].value,
            (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &port_item);
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(port_item, &datum);
        }
        if (status == SXML_STATUS_OK && builder->options.omits_default_ports &&
            datum.datum_kind == SXML_DATUM_KIND_INTEGER && datum.datum_details.integer >= 0 &&
            is_default_port((scheme_port){.scheme = builder->scheme,
                                          .port = (uint64_t)datum.datum_details.integer})) {
            has_port = false;
        }
    }
    const bool has_parts = userinfo.is_present || host.is_present || has_port;
    if (status == SXML_STATUS_OK && !builder->scheme.is_present &&
        (has_parts || authority.is_present)) {
        status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("//"));
    }
    if (status != SXML_STATUS_OK || !has_parts) {
        return status == SXML_STATUS_OK && authority.is_present
                   ? append_text(builder, authority.text)
                   : status;
    }
    if (userinfo.is_present) {
        status = append_text(builder, userinfo.text);
        if (status == SXML_STATUS_OK) {
            status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("@"));
        }
    }
    if (status == SXML_STATUS_OK && host.is_present) {
        status = append_text(builder, host.text);
    }
    if (status == SXML_STATUS_OK && has_port) {
        status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL(":"));
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append_atomic(&builder->text, port_item);
        }
    }
    return status;
}

/* The path from path-segments, escaped unless the URI is
   non-hierarchical, else the path field. */
static sxml_status append_path(const sxml_function_call *call, uri_builder *builder) {
    if (builder->fields->fields[SXML_URI_FIELD_PATH_SEGMENTS].is_present) {
        return append_joined(call, builder, (joined_field){.field_index = SXML_URI_FIELD_PATH_SEGMENTS,
                                                     .separator = SXML_BYTE_VIEW_FROM_LITERAL("/"),
                                                     .is_escaped = !builder->is_non_hierarchical,
                                                     .kind = SXML_ESCAPING_KIND_PATH_SEGMENT});
    }
    uri_part path;
    const sxml_status status =
        read_field_text(call, (field_datum_request){builder->fields, SXML_URI_FIELD_PATH}, &path);
    if (status != SXML_STATUS_OK || !path.is_present) {
        return status;
    }
    return append_text(builder, path.text);
}

/* "?" and the query: key=value (or the value alone for the key "") per
   value of query-parameters, escaped and joined by "&"; else the query
   field. Nothing for an empty query. */
static sxml_status append_query(const sxml_function_call *call, uri_builder *builder) {
    const uri_field *parameters = &builder->fields->fields[SXML_URI_FIELD_QUERY_PARAMETERS];
    if (!parameters->is_present) {
        uri_part query;
        const sxml_status status = read_field_text(
            call, (field_datum_request){builder->fields, SXML_URI_FIELD_QUERY}, &query);
        if (status != SXML_STATUS_OK || !query.is_present || query.text.byte_count == 0) {
            return status;
        }
        sxml_status append_status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("?"));
        return append_status == SXML_STATUS_OK ? append_text(builder, query.text) : append_status;
    }
    sxml_item map = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        parameters->value, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
        &map);
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_size(map, &entry_count);
    }
    bool has_written = false;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        sxml_atomic_datum key;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &key);
        }
        if (status == SXML_STATUS_OK && key.datum_kind != SXML_DATUM_KIND_STRING) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(entry.value);
        while (status == SXML_STATUS_OK) {
            sxml_iteration_step step = {.has_item = false};
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            sxml_atomic_datum value;
            status = sxml_atomic_item_get_datum(step.item, &value);
            if (status == SXML_STATUS_OK && value.datum_kind != SXML_DATUM_KIND_STRING) {
                status = SXML_STATUS_INVALID_ARGUMENT;
            }
            if (status == SXML_STATUS_OK) {
                status = append_text(builder, has_written ? SXML_BYTE_VIEW_FROM_LITERAL("&")
                                                          : SXML_BYTE_VIEW_FROM_LITERAL("?"));
                has_written = true;
            }
            const sxml_byte_view key_text = key.datum_details.string.utf8;
            if (status == SXML_STATUS_OK && key_text.byte_count > 0) {
                status = append_escaped(
                    &builder->text,
                    (escaping_request){.text = key_text, .kind = SXML_ESCAPING_KIND_QUERY_PARAMETER});
                if (status == SXML_STATUS_OK) {
                    status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("="));
                }
            }
            if (status == SXML_STATUS_OK) {
                status = append_escaped(&builder->text,
                                        (escaping_request){.text = value.datum_details.string.utf8,
                                                           .kind = SXML_ESCAPING_KIND_QUERY_PARAMETER});
            }
        }
    }
    return status;
}

/* "#" and the fragment, escaped. */
static sxml_status append_fragment(const sxml_function_call *call, uri_builder *builder) {
    uri_part fragment;
    sxml_status status = read_field_text(
        call, (field_datum_request){builder->fields, SXML_URI_FIELD_FRAGMENT}, &fragment);
    if (status != SXML_STATUS_OK || !fragment.is_present) {
        return status;
    }
    status = append_text(builder, SXML_BYTE_VIEW_FROM_LITERAL("#"));
    if (status == SXML_STATUS_OK) {
        status = append_escaped(
            &builder->text,
            (escaping_request){.text = fragment.text, .kind = SXML_ESCAPING_KIND_FRAGMENT});
    }
    return status;
}

/* fn:build-uri($parts as fn:uri-structure-record, $options as map(*)? :=
   {}) as xs:string (F&O 4.0, section 7.6.3): scheme, authority, path,
   query and fragment from the fields that are present; the uri field is
   ignored. */
static sxml_status call_build_uri(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item parts;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &parts);
    if (status == SXML_STATUS_OK && !parts.has_item) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    uri_fields fields;
    if (status == SXML_STATUS_OK) {
        status = read_uri_fields(call, parts.item, &fields);
    }
    uri_builder builder = {.text = {.item_heap = call->item_heap}, .fields = &fields};
    if (status == SXML_STATUS_OK) {
        status = read_uri_options(call, 1, &builder.options);
    }
    if (status == SXML_STATUS_OK) {
        status = read_field_text(call, (field_datum_request){&fields, SXML_URI_FIELD_SCHEME},
                                 &builder.scheme);
    }
    sxml_atomic_datum hierarchical = {.datum_kind = SXML_DATUM_KIND_BOOLEAN};
    if (status == SXML_STATUS_OK && fields.fields[SXML_URI_FIELD_HIERARCHICAL].is_present) {
        status = read_field_datum(call, (field_datum_request){&fields, SXML_URI_FIELD_HIERARCHICAL},
                                  &hierarchical);
        builder.is_non_hierarchical = hierarchical.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
                                      !hierarchical.datum_details.boolean;
    }
    builder.is_non_hierarchical =
        builder.is_non_hierarchical || is_known_non_hierarchical(builder.scheme);
    if (status == SXML_STATUS_OK) {
        status = append_scheme(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = append_authority(call, &builder);
    }
    if (status == SXML_STATUS_OK) {
        status = append_path(call, &builder);
    }
    if (status == SXML_STATUS_OK) {
        status = append_query(call, &builder);
    }
    if (status == SXML_STATUS_OK) {
        status = append_fragment(call, &builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&builder.text), result);
    }
    sxml_function_library_text_release(&builder.text);
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter escaping_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?")};
static const sxml_function_parameter resolve_uri_parameters[] = {
    SXML_REQUIRED_PARAMETER("href", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("base", "xs:string?"),
};
static const sxml_function_parameter parse_uri_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};
static const sxml_function_parameter build_uri_parameters[] = {
    SXML_REQUIRED_PARAMETER("parts", "fn:uri-structure-record"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
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
    SXML_FUNCTION_ROW("decode-from-uri", escaping_parameters, "xs:string", 0,
                      call_decode_from_uri),
    SXML_FUNCTION_ROW("parse-uri", parse_uri_parameters, "fn:uri-structure-record?", 0,
                      call_parse_uri),
    SXML_FUNCTION_ROW("build-uri", build_uri_parameters, "xs:string", 0, call_build_uri),
};

const sxml_native_function_view sxml_function_library_uri = SXML_LIBRARY_PART(uri_functions);
