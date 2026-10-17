/*
 * The resource part of the function library (packet 2.12, D131): the
 * functions that read the evaluation environment (src/processor_internal.h)
 * rather than their arguments. F&O 4.0, section 17.1 "Functions giving
 * access to external information": fn:collection, fn:uri-collection,
 * fn:unparsed-text, fn:unparsed-text-lines, fn:unparsed-text-available,
 * fn:unparsed-binary, fn:environment-variable,
 * fn:available-environment-variables; section 17.4.5 fn:json-doc; section
 * 20.1.3 fn:default-language. fn:trace and fn:message, which hand their
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
#include "function_library_parts_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/string.h"
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

static const sxml_static_error option_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An option has a value of the wrong type (F&O 4.0, section 1.7, option "
        "parameter conventions)"),
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
    /* The coerced encoding value the name borrows, owned. */
    sxml_sequence encoding_value;
} text_options;

/* Makes an xs:string key item, owned. */
static sxml_status make_key(sxml_item_heap *item_heap, sxml_byte_view key_text,
                            sxml_item *key) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = key_text}}}},
        key);
}

/* One option to read from a map: its key and the type of its value. */
typedef struct option_query {
    sxml_item options_map;
    sxml_byte_view key_text;
    sxml_sequence_type value_type;
} option_query;

/* Writes the coerced value of the option, owned, to *value, and whether
   the map has it to *is_present (the empty sequence when it has not). A
   value of the wrong type is XPTY0004. */
static sxml_status read_option(const sxml_function_call *call, const option_query *query,
                               sxml_sequence *value) {
    *value = sxml_sequence_empty();
    sxml_item key = {.representation = 0};
    sxml_status status = make_key(call->item_heap, query->key_text, &key);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(query->options_map, key, &lookup);
        sxml_item_release(key);
    }
    if (status != SXML_STATUS_OK || !lookup.is_found) {
        return status;
    }
    status = sxml_evaluator_coerce(
        call->dynamic_context,
        &(sxml_coercion_request){.value = lookup.entry.value,
                                 .sequence_type = query->value_type,
                                 .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                                 .error_record = NULL},
        value);
    if (status == SXML_STATUS_TYPE_ERROR) {
        return sxml_function_library_report(call, &option_type_error);
    }
    return status;
}

/* The value of an xs:boolean option already coerced; false when absent. */
static bool option_is_true(sxml_sequence value) {
    sxml_atomic_datum datum;
    return sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
           sxml_atomic_item_get_datum(value.content, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && datum.datum_details.boolean;
}

/* The text of a coerced xs:string? value; empty for the empty sequence. */
static sxml_byte_view option_text(sxml_sequence value) {
    sxml_atomic_datum datum;
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_get_datum(value.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING) {
        return datum.datum_details.string.utf8;
    }
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* An option key of an options map. */
typedef struct option_key {
    sxml_item options_map;
    sxml_byte_view key_text;
} option_key;

/* A boolean option's value: false when absent. */
typedef struct boolean_option {
    bool is_true;
} boolean_option;

/* Reads a boolean option (normalize-newlines, fallback) into *option. */
static sxml_status read_boolean_option(const sxml_function_call *call, option_key key,
                                       boolean_option *option) {
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = read_option(
        call,
        &(option_query){.options_map = key.options_map,
                        .key_text = key.key_text,
                        .value_type = {.item_type =
                                           sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
                                       .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}},
        &value);
    option->is_true = option_is_true(value);
    sxml_sequence_release(value);
    return status;
}

/* Which function reads a text resource: its options and checks differ. */
typedef enum text_function {
    SXML_TEXT_FUNCTION_UNPARSED_TEXT,
    SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES,
    SXML_TEXT_FUNCTION_JSON_DOC,
} text_function;

/* Whether the options map has the key, in option->is_true. */
static sxml_status find_option_key(const sxml_function_call *call, option_key key,
                                   boolean_option *option) {
    sxml_item key_item = {.representation = 0};
    sxml_status status = make_key(call->item_heap, key.key_text, &key_item);
    sxml_map_lookup lookup = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_find(key.options_map, key_item, &lookup);
        sxml_item_release(key_item);
    }
    option->is_true = lookup.is_found;
    return status;
}

/*
 * Reads $options (argument 1) of fn:unparsed-text and fn:unparsed-text-
 * lines: a string $S is the map {"encoding": $S}; a map has the keys
 * encoding, normalize-newlines (not for -lines, which always normalizes:
 * XPTY0004, as QT4 fn-unparsed-text-lines-40-014 expects) and fallback
 * (F&O 4.0, sections 17.1.5 and 17.1.6). fn:json-doc's options are
 * fn:parse-json's. Release options->encoding_value on every path.
 */
static sxml_status read_text_options(const sxml_function_call *call, text_function function,
                                     text_options *options) {
    *options = (text_options){.encoding_name = {.bytes = NULL, .byte_count = 0},
                              .normalizes_newlines = function == SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES,
                              .has_fallback = false,
                              .permits_any_character = function == SXML_TEXT_FUNCTION_JSON_DOC,
                              .encoding_value = sxml_sequence_empty()};
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
    status = read_option(
        call,
        &(option_query){.options_map = argument.item,
                        .key_text = SXML_BYTE_VIEW_FROM_LITERAL("encoding"),
                        .value_type = {.item_type =
                                           sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                       .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}},
        &options->encoding_value);
    options->encoding_name = option_text(options->encoding_value);
    const sxml_byte_view newlines_key = SXML_BYTE_VIEW_FROM_LITERAL("normalize-newlines");
    boolean_option option = {.is_true = false};
    if (status == SXML_STATUS_OK && function == SXML_TEXT_FUNCTION_UNPARSED_TEXT_LINES) {
        status = find_option_key(
            call, (option_key){.options_map = argument.item, .key_text = newlines_key}, &option);
        if (status == SXML_STATUS_OK && option.is_true) {
            status = sxml_function_library_report(call, &option_type_error);
        }
    } else if (status == SXML_STATUS_OK) {
        status = read_boolean_option(
            call, (option_key){.options_map = argument.item, .key_text = newlines_key}, &option);
        options->normalizes_newlines = option.is_true;
    }
    if (status == SXML_STATUS_OK) {
        status = read_boolean_option(
            call,
            (option_key){.options_map = argument.item,
                         .key_text = SXML_BYTE_VIEW_FROM_LITERAL("fallback")},
            &option);
        options->has_fallback = option.is_true;
    }
    return status;
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

/* The encodings fn:unparsed-text decodes (the XML parser's, D51). */
typedef enum text_encoding {
    SXML_TEXT_ENCODING_UTF8,
    SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN,
    SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN,
    SXML_TEXT_ENCODING_ISO_8859_1,
    SXML_TEXT_ENCODING_US_ASCII,
} text_encoding;

/* How to decode: the encoding and where the characters start. */
typedef struct text_decoding {
    text_encoding encoding;
    size_t start_offset;
    /* True when the encoding came from a candidate (FOUT1190 on bad
       bytes), false when inferred (FOUT1200). */
    bool has_candidate;
} text_decoding;

/* The byte order mark that starts content, if any. */
typedef enum byte_order_mark {
    SXML_BYTE_ORDER_MARK_NONE,
    SXML_BYTE_ORDER_MARK_UTF8,
    SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN,
    SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN,
} byte_order_mark;

static byte_order_mark find_byte_order_mark(sxml_byte_view content) {
    const uint8_t *bytes = content.bytes;
    if (content.byte_count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return SXML_BYTE_ORDER_MARK_UTF8;
    }
    if (content.byte_count >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN;
    }
    if (content.byte_count >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN;
    }
    return SXML_BYTE_ORDER_MARK_NONE;
}

/*
 * Chooses the effective encoding and start position (F&O 4.0, section
 * 17.1.5, after bin:infer-encoding): a byte order mark decides when there
 * is no candidate and is skipped when it agrees with the candidate; a
 * candidate "UTF-16" without one is big-endian; no candidate and no mark
 * is UTF-8. A mark the candidate disagrees with is decoded as characters
 * of the candidate (so it fails, or is U+FEFF's bytes in ISO-8859-1).
 * False for an encoding name sxml does not support (FOUT1190).
 */
static bool choose_decoding(sxml_byte_view content, sxml_byte_view candidate,
                            text_decoding *decoding) {
    const byte_order_mark mark = find_byte_order_mark(content);
    *decoding = (text_decoding){.encoding = SXML_TEXT_ENCODING_UTF8,
                                .start_offset = 0,
                                .has_candidate = candidate.byte_count > 0};
    if (candidate.byte_count == 0) {
        switch (mark) {
        case SXML_BYTE_ORDER_MARK_UTF8:
            decoding->start_offset = 3;
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN:
            *decoding = (text_decoding){.encoding = SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN,
                                        .start_offset = 2,
                                        .has_candidate = false};
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN:
            *decoding = (text_decoding){.encoding = SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN,
                                        .start_offset = 2,
                                        .has_candidate = false};
            break;
        case SXML_BYTE_ORDER_MARK_NONE:
            break;
        }
        return true;
    }
    switch (sxml_xml_classify_encoding_name(candidate)) {
    case SXML_XML_ENCODING_FAMILY_UNSUPPORTED:
        return false;
    case SXML_XML_ENCODING_FAMILY_UTF8:
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF8 ? 3 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16:
        decoding->encoding = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN
                                 ? SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN
                                 : SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ||
                                         mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN
                                     ? 2
                                     : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
        decoding->encoding = SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN:
        decoding->encoding = SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
        decoding->encoding = SXML_TEXT_ENCODING_ISO_8859_1;
        return true;
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        decoding->encoding = SXML_TEXT_ENCODING_US_ASCII;
        return true;
    }
    return false;
}

/* One step of decoding: a codepoint, or a malformed unit to skip. */
typedef struct decoded_unit {
    bool is_well_formed;
    uint32_t codepoint;
    size_t byte_count;
} decoded_unit;

/* The continuation bytes of a UTF-8 sequence, for a lead byte; 0 for an
   invalid lead byte (RFC 3629, section 4). */
static size_t utf8_continuation_count(uint8_t lead_byte) {
    if (lead_byte >= 0xC2 && lead_byte <= 0xDF) {
        return 1;
    }
    if (lead_byte >= 0xE0 && lead_byte <= 0xEF) {
        return 2;
    }
    if (lead_byte >= 0xF0 && lead_byte <= 0xF4) {
        return 3;
    }
    return 0;
}

/* Decodes the UTF-8 sequence at offset (RFC 3629, table 3-7 of Unicode:
   no overlong forms, surrogates or codepoints past U+10FFFF). */
static decoded_unit decode_utf8(sxml_byte_view content, size_t offset) {
    const uint8_t lead_byte = content.bytes[offset];
    if (lead_byte < 0x80) {
        return (decoded_unit){.is_well_formed = true, .codepoint = lead_byte, .byte_count = 1};
    }
    const decoded_unit malformed = {.is_well_formed = false, .codepoint = 0, .byte_count = 1};
    const size_t continuation_count = utf8_continuation_count(lead_byte);
    if (continuation_count == 0 || content.byte_count - offset <= continuation_count) {
        return malformed;
    }
    uint32_t codepoint = lead_byte & (0x7FU >> (continuation_count + 1));
    for (size_t continuation_index = 1; continuation_index <= continuation_count;
         continuation_index += 1) {
        const uint8_t byte = content.bytes[offset + continuation_index];
        if ((byte & 0xC0U) != 0x80U) {
            return malformed;
        }
        codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    static const uint32_t smallest_codepoints[4] = {0, 0x80, 0x800, 0x10000};
    if (codepoint < smallest_codepoints[continuation_count] || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return malformed;
    }
    return (decoded_unit){
        .is_well_formed = true, .codepoint = codepoint, .byte_count = continuation_count + 1};
}

/* The UTF-16 code unit at offset, which has two bytes left. */
static uint32_t utf16_code_unit(sxml_byte_view content, size_t offset, text_encoding encoding) {
    const bool is_big_endian = encoding == SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
    const uint32_t first = content.bytes[offset];
    const uint32_t second = content.bytes[offset + 1];
    return is_big_endian ? (first << 8U) | second : (second << 8U) | first;
}

/* Decodes the UTF-16 code units at offset: a pair for a supplementary
   codepoint; a lone surrogate or an odd last byte is malformed. */
static decoded_unit decode_utf16(sxml_byte_view content, size_t offset, text_encoding encoding) {
    if (content.byte_count - offset < 2) {
        return (decoded_unit){.is_well_formed = false, .codepoint = 0, .byte_count = 1};
    }
    const uint32_t unit = utf16_code_unit(content, offset, encoding);
    const decoded_unit lone = {.is_well_formed = false, .codepoint = 0, .byte_count = 2};
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        return lone;
    }
    if (unit < 0xD800 || unit > 0xDBFF) {
        return (decoded_unit){.is_well_formed = true, .codepoint = unit, .byte_count = 2};
    }
    if (content.byte_count - offset < 4) {
        return lone;
    }
    const uint32_t low_unit = utf16_code_unit(content, offset + 2, encoding);
    if (low_unit < 0xDC00 || low_unit > 0xDFFF) {
        return lone;
    }
    return (decoded_unit){.is_well_formed = true,
                          .codepoint = 0x10000U + ((unit - 0xD800U) << 10U) + (low_unit - 0xDC00U),
                          .byte_count = 4};
}

static decoded_unit decode_unit(sxml_byte_view content, size_t offset, text_encoding encoding) {
    const uint8_t byte = content.bytes[offset];
    switch (encoding) {
    case SXML_TEXT_ENCODING_UTF8:
        return decode_utf8(content, offset);
    case SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN:
    case SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN:
        return decode_utf16(content, offset, encoding);
    case SXML_TEXT_ENCODING_ISO_8859_1:
        return (decoded_unit){.is_well_formed = true, .codepoint = byte, .byte_count = 1};
    case SXML_TEXT_ENCODING_US_ASCII:
        break;
    }
    return (decoded_unit){.is_well_formed = byte < 0x80, .codepoint = byte, .byte_count = 1};
}

/* What to decode and how. */
typedef struct text_decoding_request {
    sxml_byte_view content;
    text_decoding decoding;
    const text_options *options;
} text_decoding_request;

/* Appends codepoint, or a line feed for a line end when newlines are
   normalized: CR LF and a lone CR become LF (F&O 4.0, section 17.1.5,
   normalize-newlines). */
typedef struct newline_state {
    bool follows_carriage_return;
} newline_state;

static sxml_status append_character(sxml_library_text *text, newline_state *state,
                                    uint32_t codepoint) {
    const bool was_carriage_return = state->follows_carriage_return;
    state->follows_carriage_return = codepoint == 0x0D;
    if (codepoint == 0x0D) {
        return sxml_function_library_text_append_codepoint(text, 0x0A);
    }
    if (codepoint == 0x0A && was_carriage_return) {
        return SXML_STATUS_OK;
    }
    return sxml_function_library_text_append_codepoint(text, codepoint);
}

/*
 * Decodes request->content into *text: each well-formed character that
 * XML permits ([2] Char), and U+FFFD for what does not decode or is not
 * permitted when the fallback option is true; otherwise *failure says
 * which error to raise.
 */
static sxml_status decode_text(const text_decoding_request *request, sxml_library_text *text,
                               text_failure *failure) {
    *failure = SXML_TEXT_FAILURE_NONE;
    const sxml_byte_view content = request->content;
    newline_state state = {.follows_carriage_return = false};
    size_t offset = request->decoding.start_offset;
    while (offset < content.byte_count) {
        const decoded_unit unit = decode_unit(content, offset, request->decoding.encoding);
        uint32_t codepoint = unit.codepoint;
        if (!unit.is_well_formed ||
            (!request->options->permits_any_character && !sxml_xml_codepoint_is_char(codepoint))) {
            if (!request->options->has_fallback) {
                *failure = unit.is_well_formed || request->decoding.has_candidate
                               ? SXML_TEXT_FAILURE_ENCODING
                               : SXML_TEXT_FAILURE_UNDECODABLE;
                return SXML_STATUS_OK;
            }
            codepoint = 0xFFFD;
        }
        sxml_status status = SXML_STATUS_OK;
        if (request->options->normalizes_newlines) {
            status = append_character(text, &state, codepoint);
        } else {
            status = sxml_function_library_text_append_codepoint(text, codepoint);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        offset += unit.byte_count;
    }
    return SXML_STATUS_OK;
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
    text_decoding decoding;
    if (!choose_decoding(retrieved->content, candidate, &decoding)) {
        outcome->failure = SXML_TEXT_FAILURE_ENCODING;
        return SXML_STATUS_OK;
    }
    return decode_text(&(text_decoding_request){.content = retrieved->content,
                                                .decoding = decoding,
                                                .options = options},
                       &outcome->text, &outcome->failure);
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
    sxml_sequence_release(reading->options.encoding_value);
    sxml_function_library_text_release(&reading->outcome.text);
}

/*
 * The shared start of fn:unparsed-text, -lines and -available: reads the
 * arguments and the resource. A retrieval failure is raised as FOUT1170
 * with the resolver's description (the status says so); a decoding
 * failure is left in reading->outcome.failure.
 */
static sxml_status start_text_reading(const sxml_function_call *call, text_function function,
                                      text_reading *reading) {
    memset(reading, 0, sizeof(*reading));
    reading->options.encoding_value = sxml_sequence_empty();
    reading->outcome.text.item_heap = call->item_heap;
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
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
        /* FOUT1170 was raised for a resource not retrieved. */
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
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
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

/* The fn:parse-json row of the json part, found by name among the parts
   that serve XPath; NULL when there is none. */
static const sxml_native_function *find_parse_json(void) {
    const sxml_native_function_view_list parts =
        sxml_function_library_parts_for_language(SXML_LANGUAGE_XPATH);
    for (size_t part_index = 0; part_index < parts.native_function_view_count;
         part_index += 1) {
        const sxml_native_function_view *part = parts.native_function_views[part_index];
        for (size_t row_index = 0; row_index < part->native_function_count; row_index += 1) {
            const sxml_native_function *row = &part->native_functions[row_index];
            if (row->function_namespace == SXML_NAMESPACE_FUNCTIONS && row->call != NULL &&
                sxml_byte_view_equals(row->local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("parse-json"))) {
                return row;
            }
        }
    }
    return NULL;
}

/* fn:json-doc($source as xs:string?, $options as map(*)? := {}) as
   item()? (F&O 4.0, section 17.4.5): fn:unparsed-text($source) =>
   fn:parse-json($options); the errors of both. */
static sxml_status call_json_doc(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_native_function *parse_json = find_parse_json();
    if (parse_json == NULL) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    text_reading reading;
    sxml_status status = start_text_reading(call, SXML_TEXT_FUNCTION_JSON_DOC, &reading);
    if (status == SXML_STATUS_OK && !reading.has_source) {
        *result = sxml_sequence_empty();
    } else if (status == SXML_STATUS_OK && reading.outcome.failure != SXML_TEXT_FAILURE_NONE) {
        status = report_text_failure(call, &reading.outcome);
    } else if (status == SXML_STATUS_OK) {
        sxml_sequence text = sxml_sequence_empty();
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&reading.outcome.text), &text);
        if (status == SXML_STATUS_OK) {
            const sxml_sequence arguments[2] = {text, sxml_function_library_get_argument(call, 1)};
            status = parse_json->call(
                &(sxml_function_call){.function_item = call->function_item,
                                      .arguments = {.sequences = arguments,
                                                    .sequence_count = 2},
                                      .dynamic_context = call->dynamic_context,
                                      .item_heap = call->item_heap,
                                      .error_record = call->error_record},
                result);
            sxml_sequence_release(text);
        }
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
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
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
static const sxml_function_parameter json_doc_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
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
    SXML_FUNCTION_ROW("json-doc", json_doc_parameters, "item()?", SXML_RESOURCE_PROPERTIES,
                      call_json_doc),
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
