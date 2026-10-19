/*
 * The EXPath Binary feature (packet 7.8, D198): the bin: functions of the
 * EXPath Binary Module 4.0 (namespace http://expath.org/ns/binary) over
 * xs:base64Binary and xs:hexBinary values. An optional feature (glossary
 * `optional_feature`, D189): its rows are not a registered library part
 * (the view below is not named sxml_function_library_<part>, so CMake does
 * not register it); a processor serves them only when its configuration
 * names sxml_expath_binary_feature() (D191), so without it the functions
 * are absent (XPST0017, their D162 place) and this file is not linked.
 *
 * Golden file: src/function_library_hash.c (a leaf part: rows, argument
 * reading, errors, a binary result).
 *
 * Binary arguments accept both binary types; every binary result is an
 * xs:base64Binary (EXPath Binary 4.0, section 1.2). Octets are counted
 * from 0, bits from the most significant bit of the first octet (8.7).
 * Text decoding is the one of fn:unparsed-text (src/text_decoding_
 * internal.h); integers beyond int64_t are xs:integer items made from
 * their decimal text, converted here by schoolbook arithmetic on base 10^9
 * limbs, which is all packing needs. Every function charges one step per
 * 64 octets it reads or writes.
 */
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "optional_feature_internal.h"
#include "sxml/processor.h"
#include "text_decoding_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <math.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

/* An error of EXPath Binary 4.0, appendix B, in the bin namespace. */
typedef struct binary_error {
    sxml_byte_view local_name;
    sxml_byte_view description;
} binary_error;

static const binary_error conversion_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("conversion-error"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value does not convert to or from a string in this encoding (EXPath Binary 4.0)")};

static const binary_error differing_length_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("differing-length-arguments"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The two arguments of a bitwise operation have different lengths (EXPath Binary 4.0, 8)")};

static const binary_error index_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("index-out-of-range"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The offset or index is outside the binary value (EXPath Binary 4.0)")};

static const binary_error integer_too_large_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("integer-too-large"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "sxml unpacks integers of at most 4096 octets (EXPath Binary 4.0, 7.1.2)")};

static const binary_error negative_size_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("negative-size"),
    .description =
        SXML_BYTE_VIEW_INITIALIZER("The size or count is negative (EXPath Binary 4.0)")};

static const binary_error non_numeric_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("non-numeric-character"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The string has a character that is not a digit of its radix (EXPath Binary 4.0, 4)")};

static const binary_error unknown_encoding_error = {
    .local_name = SXML_BYTE_VIEW_INITIALIZER("unknown-encoding"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The encoding is not one sxml supports: UTF-8, UTF-16, UTF-16BE, UTF-16LE, "
        "ISO-8859-1 or US-ASCII (EXPath Binary 4.0, 6)")};

/* Fills call->error_record with error, in the bin namespace, and returns
   SXML_STATUS_DYNAMIC_ERROR. */
static sxml_status report(const sxml_function_call *call, const binary_error *error) {
    if (call->error_record != NULL) {
        *call->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL("http://expath.org/ns/binary"),
                     .local_name = error->local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("bin")},
            .value = NULL,
            .diagnostic = {.description = error->description,
                           .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}}};
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* -------------------------------------------------------------- Arguments */

/* A binary argument: its octets, borrowed from the item. */
typedef struct binary_argument {
    bool has_value;
    sxml_byte_view octets;
} binary_argument;

/* Reads a "(xs:hexBinary | xs:base64Binary)?" argument. */
static sxml_status read_binary(const sxml_function_call *call, size_t argument_index,
                               binary_argument *argument) {
    *argument = (binary_argument){.has_value = false, .octets = {.bytes = NULL, .byte_count = 0}};
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    if (status != SXML_STATUS_OK || !optional.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(optional.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
        /* Coercion to the parameter type allows nothing else. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *argument = (binary_argument){.has_value = true, .octets = datum.datum_details.binary};
    }
    return status;
}

/* An xs:integer argument: inline when it fits in int64_t, else its sign
   (offsets and sizes that large are out of range anyway) and its
   canonical decimal text, borrowed from the item. */
typedef struct integer_argument {
    bool has_value;
    bool is_inline;
    int64_t value;
    bool is_negative;
    sxml_byte_view decimal_text;
} integer_argument;

static sxml_status read_integer(const sxml_function_call *call, size_t argument_index,
                                integer_argument *argument) {
    *argument = (integer_argument){.has_value = false,
                                   .is_inline = true,
                                   .value = 0,
                                   .is_negative = false,
                                   .decimal_text = {.bytes = NULL, .byte_count = 0}};
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    if (status != SXML_STATUS_OK || !optional.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(optional.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    argument->has_value = true;
    if (datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        argument->value = datum.datum_details.integer;
        argument->is_negative = argument->value < 0;
        return SXML_STATUS_OK;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_DECIMAL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    argument->is_inline = false;
    argument->decimal_text = datum.datum_details.decimal_text;
    argument->is_negative =
        argument->decimal_text.byte_count > 0 && argument->decimal_text.bytes[0] == '-';
    return SXML_STATUS_OK;
}

/* Reads an "xs:string?" argument; has_value is false for (). */
typedef struct string_argument {
    bool has_value;
    sxml_string string;
} string_argument;

static sxml_status read_string(const sxml_function_call *call, size_t argument_index,
                               string_argument *argument) {
    argument->has_value = false;
    argument->string = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                                     .codepoint_count = 0,
                                     .is_ascii = true};
    sxml_optional_item optional;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &optional);
    if (status != SXML_STATUS_OK || !optional.has_item) {
        return status;
    }
    argument->has_value = true;
    return sxml_function_library_read_string_argument(call, argument_index, &argument->string);
}

/* The span an offset and an optional size select in a value of
   value_byte_count octets (bin:part, EXPath Binary 4.0, 5.2). */
typedef struct octet_span {
    size_t offset;
    size_t byte_count;
} octet_span;

typedef struct span_request {
    integer_argument offset;
    /* has_value false: up to the end. */
    integer_argument size;
    size_t value_byte_count;
} span_request;

/* Checks a span: bin:index-out-of-range for a negative offset or one past
   the end, bin:negative-size for a negative size. */
static sxml_status check_span(const sxml_function_call *call, const span_request *request,
                              octet_span *span) {
    const size_t length = request->value_byte_count;
    if (request->offset.is_negative || !request->offset.is_inline ||
        (uint64_t)request->offset.value > (uint64_t)length) {
        return report(call, &index_error);
    }
    span->offset = (size_t)request->offset.value;
    span->byte_count = length - span->offset;
    if (!request->size.has_value) {
        return SXML_STATUS_OK;
    }
    if (request->size.is_negative) {
        return report(call, &negative_size_error);
    }
    if (!request->size.is_inline || (uint64_t)request->size.value > (uint64_t)span->byte_count) {
        return report(call, &index_error);
    }
    span->byte_count = (size_t)request->size.value;
    return SXML_STATUS_OK;
}

/* The octets of a span. */
static sxml_byte_view span_octets(sxml_byte_view octets, octet_span span) {
    if (octets.bytes == NULL) {
        return octets;
    }
    return (sxml_byte_view){.bytes = octets.bytes + span.offset, .byte_count = span.byte_count};
}

/* Charges the steps for work over byte_count octets. */
static sxml_status charge_octets(const sxml_function_call *call, size_t byte_count) {
    return sxml_function_library_charge_steps(call, (uint64_t)byte_count / 64U + 1U);
}

/* -------------------------------------------------------------- Results */

/* Octets being built in item heap memory; release on every path. */
typedef struct octet_buffer {
    sxml_item_heap *item_heap;
    uint8_t *bytes;
    size_t byte_count;
} octet_buffer;

/* Allocates byte_count zeroed octets. */
static sxml_status octet_buffer_allocate(const sxml_function_call *call, size_t byte_count,
                                         octet_buffer *buffer) {
    *buffer = (octet_buffer){.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = charge_octets(call, byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            call->item_heap, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(memory, 0, byte_count);
        buffer->bytes = memory;
        buffer->byte_count = byte_count;
    }
    return status;
}

static void octet_buffer_release(octet_buffer *buffer) {
    sxml_item_heap_release_bytes(buffer->item_heap,
                                 (sxml_heap_bytes){.memory = buffer->bytes,
                                                   .byte_count = buffer->byte_count});
    buffer->bytes = NULL;
    buffer->byte_count = 0;
}

static sxml_byte_view octet_buffer_view(const octet_buffer *buffer) {
    return (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
}

/* Copies source to target; nothing for no octets (memcpy's arguments
   must not be NULL, C17 7.24.1). */
static void copy_octets(uint8_t *target, sxml_byte_view source) {
    if (source.byte_count > 0 && target != NULL && source.bytes != NULL) {
        memcpy(target, source.bytes, source.byte_count);
    }
}

/* Fills byte_count octets at target with octet. */
typedef struct octet_fill {
    size_t byte_count;
    uint8_t octet;
} octet_fill;

static void fill_octets(uint8_t *target, octet_fill fill) {
    if (fill.byte_count > 0 && target != NULL) {
        memset(target, fill.octet, fill.byte_count);
    }
}

/* Makes an xs:base64Binary item of octets (copied) as *result. */
static sxml_status make_binary(const sxml_function_call *call, sxml_byte_view octets,
                               sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
            .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                      .datum_details = {.binary = octets}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* Makes the binary result of a buffer and releases the buffer. */
static sxml_status finish_buffer(const sxml_function_call *call, octet_buffer *buffer,
                                 sxml_sequence *result) {
    const sxml_status status = make_binary(call, octet_buffer_view(buffer), result);
    octet_buffer_release(buffer);
    return status;
}

/* ------------------------------------------- Constants and conversions */

static bool is_regex_space(uint32_t codepoint) {
    return codepoint == 0x20 || codepoint == 0x09 || codepoint == 0x0A || codepoint == 0x0D;
}

/* The value of a digit in radix (2, 8 or 16), or -1. */
static int digit_value(uint32_t codepoint, unsigned radix) {
    int value = -1;
    if (codepoint >= '0' && codepoint <= '9') {
        value = (int)(codepoint - '0');
    } else if (codepoint >= 'a' && codepoint <= 'f') {
        value = (int)(codepoint - 'a') + 10;
    } else if (codepoint >= 'A' && codepoint <= 'F') {
        value = (int)(codepoint - 'A') + 10;
    }
    return value >= 0 && (unsigned)value < radix ? value : -1;
}

/* How a digit string becomes octets: bits per digit and how many leading
   zero bits may be dropped before padding to whole octets. */
typedef struct radix_rules {
    unsigned radix;
    unsigned bits_per_digit;
    unsigned droppable_zero_bits;
} radix_rules;

/*
 * bin:hex, bin:bin and bin:octal (EXPath Binary 4.0, 4.1 to 4.3): the
 * digits, without whitespace and underscores, written as bits; for octal
 * at most two leading zero bits dropped; then right-aligned in the fewest
 * whole octets (hex's prepended "0" and bin's leading zeros are exactly
 * that alignment). bin:non-numeric-character for any other character.
 */
static sxml_status call_from_digits(const sxml_function_call *call, radix_rules rules,
                                    sxml_sequence *result) {
    *result = sxml_sequence_empty();
    string_argument value;
    sxml_status status = read_string(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    size_t digit_count = 0;
    size_t leading_zero_bits = 0;
    bool is_leading = true;
    sxml_codepoint_iterator iterator = {.string = value.string, .byte_offset = 0};
    uint32_t codepoint = 0;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (is_regex_space(codepoint) || codepoint == '_') {
            continue;
        }
        const int digit = digit_value(codepoint, rules.radix);
        if (digit < 0) {
            return report(call, &non_numeric_error);
        }
        if (is_leading) {
            for (unsigned bit_number = rules.bits_per_digit; bit_number > 0; bit_number -= 1) {
                if (((unsigned)digit >> (bit_number - 1U)) & 1U) {
                    is_leading = false;
                    break;
                }
                leading_zero_bits += 1;
            }
        }
        digit_count += 1;
    }
    const size_t total_bits = digit_count * rules.bits_per_digit;
    const size_t dropped =
        leading_zero_bits < rules.droppable_zero_bits ? leading_zero_bits : rules.droppable_zero_bits;
    const size_t kept_bits = total_bits - dropped;
    octet_buffer buffer;
    status = octet_buffer_allocate(call, (kept_bits + 7U) / 8U, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Bit i of the digits, from the last, goes to bit i of the octets
       from the last; the bits that do not fit are dropped zeros. */
    size_t bit_from_end = total_bits;
    iterator = (sxml_codepoint_iterator){.string = value.string, .byte_offset = 0};
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (is_regex_space(codepoint) || codepoint == '_') {
            continue;
        }
        const unsigned digit = (unsigned)digit_value(codepoint, rules.radix);
        for (unsigned bit_number = rules.bits_per_digit; bit_number > 0; bit_number -= 1) {
            bit_from_end -= 1;
            if (((digit >> (bit_number - 1U)) & 1U) != 0 && bit_from_end < buffer.byte_count * 8U) {
                buffer.bytes[buffer.byte_count - 1U - bit_from_end / 8U] |=
                    (uint8_t)(1U << (bit_from_end % 8U));
            }
        }
    }
    return finish_buffer(call, &buffer, result);
}

static sxml_status call_hex(const sxml_function_call *call, sxml_sequence *result) {
    return call_from_digits(
        call, (radix_rules){.radix = 16, .bits_per_digit = 4, .droppable_zero_bits = 0}, result);
}

static sxml_status call_bin(const sxml_function_call *call, sxml_sequence *result) {
    return call_from_digits(
        call, (radix_rules){.radix = 2, .bits_per_digit = 1, .droppable_zero_bits = 0}, result);
}

static sxml_status call_octal(const sxml_function_call *call, sxml_sequence *result) {
    return call_from_digits(
        call, (radix_rules){.radix = 8, .bits_per_digit = 3, .droppable_zero_bits = 2}, result);
}

/* bin:to-octets($value) as xs:unsignedByte* (4.4). */
static sxml_status call_to_octets(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = charge_octets(call, value.octets.byte_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (size_t octet_index = 0; status == SXML_STATUS_OK && octet_index < value.octets.byte_count;
         octet_index += 1) {
        sxml_item octet = {.representation = 0};
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_UNSIGNED_BYTE),
                .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                          .datum_details = {.integer = value.octets.bytes[octet_index]}}},
            &octet);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, octet);
            sxml_item_release(octet);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* bin:from-octets($values as xs:unsignedByte*) as xs:base64Binary (4.5). */
static sxml_status call_from_octets(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence values = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(values, call->error_record, &item_count);
    if (status == SXML_STATUS_OK && item_count > (uint64_t)SIZE_MAX) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    octet_buffer buffer = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = octet_buffer_allocate(call, (size_t)item_count, &buffer);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(values);
    for (size_t octet_index = 0; status == SXML_STATUS_OK && octet_index < buffer.byte_count;
         octet_index += 1) {
        sxml_iteration_step step = {.has_item = false, .item = {.representation = 0}};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK && step.has_item) {
            status = sxml_atomic_item_get_datum(step.item, &datum);
        }
        if (status == SXML_STATUS_OK && step.has_item && datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
            buffer.bytes[octet_index] = (uint8_t)datum.datum_details.integer;
        } else if (status == SXML_STATUS_OK) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status != SXML_STATUS_OK) {
        octet_buffer_release(&buffer);
        return status;
    }
    return finish_buffer(call, &buffer, result);
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* bin:to-base64url($value) as xs:string? (4.6): Base64 with the URL and
   filename safe alphabet (RFC 4648, section 5), without padding. */
static sxml_status call_to_base64url(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    sxml_status status = read_binary(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    const size_t length = value.octets.byte_count;
    status = charge_octets(call, length);
    sxml_library_text text = {
        .item_heap = call->item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    for (size_t octet_index = 0; status == SXML_STATUS_OK && octet_index < length;
         octet_index += 3) {
        const size_t group_count = length - octet_index < 3 ? length - octet_index : 3;
        uint32_t group = 0;
        for (size_t member = 0; member < 3; member += 1) {
            group = (group << 8U) |
                    (member < group_count ? value.octets.bytes[octet_index + member] : 0U);
        }
        for (size_t sextet = 0; status == SXML_STATUS_OK && sextet <= group_count;
             sextet += 1) {
            status = sxml_function_library_text_append_codepoint(
                &text, (uint32_t)(unsigned char)base64_alphabet[(group >> (18U - 6U * sextet)) &
                                                                0x3FU]);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap,
                                                   sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* The sextet of a Base64url character, or -1. */
static int base64url_value(uint32_t codepoint) {
    for (int value = 0; value < 64; value += 1) {
        if ((uint32_t)(unsigned char)base64_alphabet[value] == codepoint) {
            return value;
        }
    }
    return -1;
}

/* bin:from-base64url($value as xs:string?) as xs:base64Binary? (4.7):
   whitespace and trailing "=" removed, then the URL-safe alphabet only,
   and a length and last character that are canonical Base64
   (bin:conversion-error otherwise). */
static sxml_status call_from_base64url(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    string_argument value;
    sxml_status status = read_string(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    /* The characters before the trailing padding. */
    const sxml_byte_view text = value.string.utf8;
    size_t end = text.byte_count;
    while (end > 0 && (text.bytes[end - 1] == '=' || is_regex_space(text.bytes[end - 1]))) {
        end -= 1;
    }
    size_t sextet_count = 0;
    for (size_t byte_index = 0; byte_index < end; byte_index += 1) {
        if (is_regex_space(text.bytes[byte_index])) {
            continue;
        }
        if (base64url_value(text.bytes[byte_index]) < 0) {
            return report(call, &conversion_error);
        }
        sextet_count += 1;
    }
    if (sextet_count % 4U == 1U) {
        return report(call, &conversion_error);
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, sextet_count * 3U / 4U, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t bits = 0;
    unsigned bit_count = 0;
    size_t written = 0;
    for (size_t byte_index = 0; byte_index < end; byte_index += 1) {
        if (is_regex_space(text.bytes[byte_index])) {
            continue;
        }
        bits = (bits << 6U) | (uint32_t)base64url_value(text.bytes[byte_index]);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            buffer.bytes[written] = (uint8_t)(bits >> bit_count);
            written += 1;
            bits &= (1U << bit_count) - 1U;
        }
    }
    if (bits != 0) {
        /* The bits after the last octet must be zero (XSD 1.1 Part 2,
           3.3.17: the canonical last characters of base64Binary). */
        octet_buffer_release(&buffer);
        return report(call, &conversion_error);
    }
    return finish_buffer(call, &buffer, result);
}

/* ------------------------------------------------------ Basic operations */

/* bin:length($value) as xs:integer (5.1). */
static sxml_status call_length(const sxml_function_call *call, sxml_sequence *result) {
    binary_argument value;
    const sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(
            sxml_atomic_item_from_integer((int64_t)value.octets.byte_count));
    }
    return status;
}

/* bin:part($value, $offset, $size := ()) (5.2). */
static sxml_status call_part(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    span_request request = {.value_byte_count = 0};
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &request.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &request.size);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    request.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &request, &span);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return make_binary(call, span_octets(value.octets, span), result);
}

/* bin:join($values) as xs:base64Binary (5.3). */
static sxml_status call_join(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence values = sxml_function_library_get_argument(call, 0);
    size_t total = 0;
    sxml_status status = SXML_STATUS_OK;
    /* Two passes: the length, then the octets. */
    octet_buffer buffer = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    for (int pass_number = 0; pass_number < 2 && status == SXML_STATUS_OK; pass_number += 1) {
        if (pass_number == 1) {
            status = octet_buffer_allocate(call, total, &buffer);
        }
        size_t written = 0;
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(values);
        sxml_iteration_step step = {.has_item = true, .item = {.representation = 0}};
        while (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            sxml_atomic_datum datum;
            status = sxml_atomic_item_get_datum(step.item, &datum);
            if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
                status = SXML_STATUS_INVALID_ARGUMENT;
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            const sxml_byte_view octets = datum.datum_details.binary;
            if (pass_number == 0) {
                status = sxml_size_add(total, octets.byte_count, &total);
            } else if (octets.byte_count > 0) {
                copy_octets(buffer.bytes + written, octets);
                written += octets.byte_count;
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        octet_buffer_release(&buffer);
        return status;
    }
    return finish_buffer(call, &buffer, result);
}

/* Copies first, then second, into a new buffer and makes the result. */
typedef struct octet_pieces {
    sxml_byte_view pieces[3];
} octet_pieces;

static sxml_status make_concatenation(const sxml_function_call *call, const octet_pieces *pieces,
                                      sxml_sequence *result) {
    size_t total = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t piece_index = 0; status == SXML_STATUS_OK && piece_index < 3; piece_index += 1) {
        status = sxml_size_add(total, pieces->pieces[piece_index].byte_count, &total);
    }
    octet_buffer buffer = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = octet_buffer_allocate(call, total, &buffer);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t written = 0;
    for (size_t piece_index = 0; piece_index < 3; piece_index += 1) {
        const sxml_byte_view piece = pieces->pieces[piece_index];
        if (piece.byte_count > 0) {
            copy_octets(buffer.bytes + written, piece);
            written += piece.byte_count;
        }
    }
    return finish_buffer(call, &buffer, result);
}

/* bin:insert-before($value, $offset, $extra) (5.4). */
static sxml_status call_insert_before(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    binary_argument extra;
    span_request request = {.value_byte_count = 0};
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &request.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_binary(call, 2, &extra);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    request.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &request, &span);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const octet_pieces pieces = {
        .pieces = {{.bytes = value.octets.bytes, .byte_count = span.offset},
                   extra.octets,
                   span_octets(value.octets, span)}};
    return make_concatenation(call, &pieces, result);
}

/* bin:pad-left and bin:pad-right ($value, $count, $octet := 0) (5.5,
   5.6). */
static sxml_status call_pad(const sxml_function_call *call, bool pads_left,
                            sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    integer_argument count;
    integer_argument octet;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &count);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &octet);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    if (count.is_negative) {
        return report(call, &negative_size_error);
    }
    if (!count.is_inline || (uint64_t)count.value > (uint64_t)SIZE_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    size_t total = 0;
    status = sxml_size_add(value.octets.byte_count, (size_t)count.value, &total);
    octet_buffer buffer = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = octet_buffer_allocate(call, total, &buffer);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t padding = (size_t)count.value;
    const size_t value_start = pads_left ? padding : 0;
    const size_t padding_start = pads_left ? 0 : value.octets.byte_count;
    if (value.octets.byte_count > 0) {
        copy_octets(buffer.bytes + value_start, value.octets);
    }
    if (padding > 0) {
        fill_octets(buffer.bytes + padding_start,
                    (octet_fill){.byte_count = padding,
                                 .octet = (uint8_t)(octet.has_value ? octet.value : 0)});
    }
    return finish_buffer(call, &buffer, result);
}

static sxml_status call_pad_left(const sxml_function_call *call, sxml_sequence *result) {
    return call_pad(call, true, result);
}

static sxml_status call_pad_right(const sxml_function_call *call, sxml_sequence *result) {
    return call_pad(call, false, result);
}

/* bin:find($value, $offset, $search) as xs:integer? (5.7). */
static sxml_status call_find(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    binary_argument search;
    span_request request = {.value_byte_count = 0};
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &request.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_binary(call, 2, &search);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    request.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &request, &span);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t length = value.octets.byte_count;
    const size_t search_length = search.octets.byte_count;
    for (size_t position = span.offset;
         status == SXML_STATUS_OK && search_length <= length && position <= length - search_length;
         position += 1) {
        if (search_length == 0 ||
            memcmp(value.octets.bytes + position, search.octets.bytes, search_length) == 0) {
            *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)position));
            return SXML_STATUS_OK;
        }
        status = charge_octets(call, search_length);
    }
    return status;
}

/* --------------------------------------------- Text decoding and encoding */

/* The effective encoding of $encoding for value (bin:infer-encoding, EXPath
   Binary 4.0, 6.1), and the name the function reports for it. */
typedef struct inferred_encoding {
    sxml_text_decoding decoding;
    sxml_byte_view encoding_name;
} inferred_encoding;

static sxml_byte_view canonical_encoding_name(sxml_text_encoding encoding) {
    switch (encoding) {
    case SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN:
        return SXML_BYTE_VIEW_FROM_LITERAL("UTF-16BE");
    case SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN:
        return SXML_BYTE_VIEW_FROM_LITERAL("UTF-16LE");
    case SXML_TEXT_ENCODING_UTF8:
    case SXML_TEXT_ENCODING_ISO_8859_1:
    case SXML_TEXT_ENCODING_US_ASCII:
        break;
    }
    return SXML_BYTE_VIEW_FROM_LITERAL("UTF-8");
}

/*
 * Infers the encoding: a byte order mark the rules evaluate, or UTF-16
 * without one, gives the canonical name (UTF-8, UTF-16BE, UTF-16LE); any
 * other supplied $encoding is reported as given; none gives UTF-8.
 * bin:unknown-encoding for a name sxml does not support.
 */
typedef struct inference_request {
    sxml_byte_view octets;
    /* $encoding; empty when absent. */
    sxml_byte_view candidate;
} inference_request;

static sxml_status infer_encoding(const sxml_function_call *call, inference_request request,
                                  inferred_encoding *inferred) {
    const sxml_byte_view candidate = request.candidate;
    if (!sxml_text_decoding_choose(
            (sxml_text_decoding_choice){.content = request.octets, .candidate = candidate},
            &inferred->decoding)) {
        return report(call, &unknown_encoding_error);
    }
    const bool is_utf16_family = candidate.byte_count > 0 && sxml_xml_classify_encoding_name(
                                                                 candidate) ==
                                                                 SXML_XML_ENCODING_FAMILY_UTF16;
    if (candidate.byte_count == 0 || inferred->decoding.start_offset > 0 || is_utf16_family) {
        inferred->encoding_name = canonical_encoding_name(inferred->decoding.encoding);
    } else {
        inferred->encoding_name = candidate;
    }
    return SXML_STATUS_OK;
}

/* bin:infer-encoding($value, $encoding := ()) as record(encoding as
   xs:string, offset as xs:integer) (6.1). */
static sxml_status call_infer_encoding(const sxml_function_call *call, sxml_sequence *result) {
    binary_argument value;
    string_argument encoding;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_string(call, 1, &encoding);
    }
    inferred_encoding inferred;
    if (status == SXML_STATUS_OK) {
        status = infer_encoding(
            call, (inference_request){.octets = value.octets, .candidate = encoding.string.utf8},
            &inferred);
    }
    sxml_item keys[2] = {{.representation = 0}, {.representation = 0}};
    sxml_sequence name = sxml_sequence_empty();
    static const sxml_byte_view key_texts[2] = {SXML_BYTE_VIEW_INITIALIZER("encoding"),
                                                SXML_BYTE_VIEW_INITIALIZER("offset")};
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < 2; key_index += 1) {
        sxml_sequence key = sxml_sequence_empty();
        status = sxml_function_library_make_string(call->item_heap, key_texts[key_index], &key);
        keys[key_index] = key.content;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap, inferred.encoding_name, &name);
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entries[2] = {
            {.key = keys[0], .value = name},
            {.key = keys[1],
             .value = sxml_sequence_from_item(
                 sxml_atomic_item_from_integer((int64_t)inferred.decoding.start_offset))}};
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = entries, .map_entry_count = 2},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = call->error_record},
            &map);
    }
    sxml_sequence_release(name);
    sxml_item_release(keys[0]);
    sxml_item_release(keys[1]);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

/*
 * bin:decode-string($value, $encoding := (), $offset := (), $size := ())
 * as xs:string? (6.2): the part $offset and $size select (when either is
 * given), decoded by the encoding bin:infer-encoding infers for it, from
 * its effective start; bin:conversion-error for octets that do not decode
 * and for characters XML does not allow.
 */
static sxml_status call_decode_string(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    string_argument encoding;
    span_request request = {.value_byte_count = 0};
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_string(call, 1, &encoding);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &request.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 3, &request.size);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    if (!request.offset.has_value) {
        request.offset = (integer_argument){.has_value = true, .is_inline = true, .value = 0};
    }
    request.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &request, &span);
    inferred_encoding inferred;
    const sxml_byte_view octets = span_octets(value.octets, span);
    if (status == SXML_STATUS_OK) {
        status = infer_encoding(
            call, (inference_request){.octets = octets, .candidate = encoding.string.utf8},
            &inferred);
    }
    if (status == SXML_STATUS_OK) {
        status = charge_octets(call, octets.byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {
        .item_heap = call->item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    sxml_text_decoding_failure failure = SXML_TEXT_DECODING_FAILURE_NONE;
    status = sxml_text_decode(&(sxml_text_decoding_request){.content = octets,
                                                            .decoding = inferred.decoding,
                                                            .normalizes_newlines = false,
                                                            .has_fallback = false,
                                                            .permits_any_character = false},
                              &text, &failure);
    if (status == SXML_STATUS_OK && failure != SXML_TEXT_DECODING_FAILURE_NONE) {
        status = report(call, &conversion_error);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap,
                                                   sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* The octets of one codepoint in encoding: up to 4; byte_count 0 when the
   encoding cannot represent it. */
typedef struct encoded_codepoint {
    uint8_t bytes[4];
    size_t byte_count;
} encoded_codepoint;

static void put_utf16_unit(encoded_codepoint *encoded, uint32_t unit, bool is_big_endian) {
    encoded->bytes[encoded->byte_count] = (uint8_t)(is_big_endian ? unit >> 8U : unit & 0xFFU);
    encoded->bytes[encoded->byte_count + 1] = (uint8_t)(is_big_endian ? unit & 0xFFU : unit >> 8U);
    encoded->byte_count += 2;
}

static encoded_codepoint encode_codepoint(sxml_xml_encoding_family family, uint32_t codepoint) {
    encoded_codepoint encoded = {.bytes = {0}, .byte_count = 0};
    switch (family) {
    case SXML_XML_ENCODING_FAMILY_UTF8:
        if (codepoint < 0x80) {
            encoded.bytes[0] = (uint8_t)codepoint;
            encoded.byte_count = 1;
        } else if (codepoint < 0x800) {
            encoded.bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
            encoded.bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 2;
        } else if (codepoint < 0x10000) {
            encoded.bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
            encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded.bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 3;
        } else {
            encoded.bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
            encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
            encoded.bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded.bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 4;
        }
        break;
    case SXML_XML_ENCODING_FAMILY_UTF16:
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN: {
        const bool is_big_endian = family != SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN;
        if (codepoint < 0x10000) {
            put_utf16_unit(&encoded, codepoint, is_big_endian);
        } else {
            put_utf16_unit(&encoded, 0xD800U + ((codepoint - 0x10000U) >> 10U), is_big_endian);
            put_utf16_unit(&encoded, 0xDC00U + ((codepoint - 0x10000U) & 0x3FFU), is_big_endian);
        }
        break;
    }
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        if (codepoint < (family == SXML_XML_ENCODING_FAMILY_US_ASCII ? 0x80U : 0x100U)) {
            encoded.bytes[0] = (uint8_t)codepoint;
            encoded.byte_count = 1;
        }
        break;
    case SXML_XML_ENCODING_FAMILY_UNSUPPORTED:
        break;
    }
    return encoded;
}

/* bin:encode-string($value, $encoding := 'UTF-8') as xs:base64Binary?
   (6.3): UTF-16 is big-endian; no byte order mark is added;
   bin:conversion-error for a character the encoding cannot represent. */
static sxml_status call_encode_string(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    string_argument value;
    string_argument encoding;
    sxml_status status = read_string(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_string(call, 1, &encoding);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xml_encoding_family family =
        encoding.string.utf8.byte_count == 0
            ? SXML_XML_ENCODING_FAMILY_UTF8
            : sxml_xml_classify_encoding_name(encoding.string.utf8);
    if (family == SXML_XML_ENCODING_FAMILY_UNSUPPORTED) {
        return report(call, &unknown_encoding_error);
    }
    if (!value.has_value) {
        return SXML_STATUS_OK;
    }
    /* Two passes: the length, then the octets. */
    size_t total = 0;
    octet_buffer buffer = {.item_heap = call->item_heap, .bytes = NULL, .byte_count = 0};
    for (int pass_number = 0; pass_number < 2 && status == SXML_STATUS_OK; pass_number += 1) {
        if (pass_number == 1) {
            status = octet_buffer_allocate(call, total, &buffer);
        }
        size_t written = 0;
        sxml_codepoint_iterator iterator = {.string = value.string, .byte_offset = 0};
        uint32_t codepoint = 0;
        while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            const encoded_codepoint encoded = encode_codepoint(family, codepoint);
            if (encoded.byte_count == 0) {
                status = report(call, &conversion_error);
            } else if (pass_number == 0) {
                total += encoded.byte_count;
            } else {
                copy_octets(buffer.bytes + written,
                            (sxml_byte_view){.bytes = encoded.bytes, .byte_count = encoded.byte_count});
                written += encoded.byte_count;
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        octet_buffer_release(&buffer);
        return status;
    }
    return finish_buffer(call, &buffer, result);
}

/* ------------------------------------------------- Packing and unpacking */

/* Whether $order (an enum, default-on-empty) is least-significant-first
   (7.1.1); coercion to the enum type admits only its six values. */
static sxml_status read_is_little_endian(const sxml_function_call *call, size_t argument_index,
                                         bool *is_little_endian) {
    string_argument order;
    const sxml_status status = read_string(call, argument_index, &order);
    const sxml_byte_view text = order.string.utf8;
    *is_little_endian = status == SXML_STATUS_OK && order.has_value &&
                        (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("LE")) ||
                         sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("little-endian")) ||
                         sxml_byte_view_equals(
                             text, SXML_BYTE_VIEW_FROM_LITERAL("least-significant-first")));
    return status;
}

/* Reverses octets in place. */
static void reverse_octets(uint8_t *bytes, size_t byte_count) {
    for (size_t low_index = 0; byte_count > 1 && low_index < byte_count / 2; low_index += 1) {
        const uint8_t kept = bytes[low_index];
        bytes[low_index] = bytes[byte_count - 1 - low_index];
        bytes[byte_count - 1 - low_index] = kept;
    }
}

/* Makes the packed octets of bits (most significant first, byte_count of
   them, at most 8) in the requested order. */
typedef struct packed_bits {
    uint64_t bits;
    size_t byte_count;
    bool is_little_endian;
} packed_bits;

static sxml_status make_packed(const sxml_function_call *call, packed_bits packed,
                               sxml_sequence *result) {
    uint8_t octets[8];
    for (size_t octet_index = 0; octet_index < packed.byte_count; octet_index += 1) {
        octets[octet_index] =
            (uint8_t)(packed.bits >> (8U * (packed.byte_count - 1U - octet_index)));
    }
    if (packed.is_little_endian) {
        reverse_octets(octets, packed.byte_count);
    }
    return make_binary(call, (sxml_byte_view){.bytes = octets, .byte_count = packed.byte_count},
                       result);
}

/* bin:pack-double($value, $order) (7.2): IEEE 754 binary64; NaN packs as
   the quiet NaN 0x7FF8000000000000 (7.1.3). */
static sxml_status call_pack_double(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    bool is_little_endian = false;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(value.item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        status = read_is_little_endian(call, 1, &is_little_endian);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const double number = datum.datum_details.double_number;
    uint64_t bits = UINT64_C(0x7FF8000000000000);
    if (!isnan(number)) {
        memcpy(&bits, &number, sizeof(bits));
    }
    return make_packed(
        call, (packed_bits){.bits = bits, .byte_count = 8, .is_little_endian = is_little_endian},
        result);
}

/* bin:pack-float($value, $order) (7.3): IEEE 754 binary32; NaN packs as
   0x7FC00000. */
static sxml_status call_pack_float(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item value;
    bool is_little_endian = false;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(value.item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        status = read_is_little_endian(call, 1, &is_little_endian);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const float number = datum.datum_details.float_number;
    uint32_t bits = UINT32_C(0x7FC00000);
    if (!isnan(number)) {
        memcpy(&bits, &number, sizeof(bits));
    }
    return make_packed(
        call, (packed_bits){.bits = bits, .byte_count = 4, .is_little_endian = is_little_endian},
        result);
}

/* Base 10^9 limbs of a magnitude, least significant first, in item heap
   memory. */
#define SXML_BINARY_LIMB_BASE UINT32_C(1000000000)

typedef struct limb_number {
    sxml_item_heap *item_heap;
    uint32_t *limbs;
    size_t limb_count;
    size_t limb_capacity;
} limb_number;

static sxml_status limb_number_allocate(const sxml_function_call *call, size_t limb_capacity,
                                        limb_number *number) {
    *number = (limb_number){
        .item_heap = call->item_heap, .limbs = NULL, .limb_count = 0, .limb_capacity = 0};
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(limb_capacity, sizeof(uint32_t), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            call->item_heap,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(uint32_t)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        number->limbs = memory;
        number->limb_capacity = limb_capacity;
    }
    return status;
}

static void limb_number_release(limb_number *number) {
    sxml_item_heap_release_bytes(
        number->item_heap, (sxml_heap_bytes){.memory = number->limbs,
                                             .byte_count = number->limb_capacity * sizeof(uint32_t)});
    number->limbs = NULL;
    number->limb_capacity = 0;
    number->limb_count = 0;
}

/* Writes the two's complement of the magnitude of value (canonical
   decimal text, beyond int64_t) modulo 256^buffer.byte_count, most
   significant octet first (bin:pack-integer, 7.4). */
static sxml_status pack_decimal(const sxml_function_call *call, sxml_byte_view decimal_text,
                                octet_buffer *buffer) {
    const bool is_negative = decimal_text.byte_count > 0 && decimal_text.bytes[0] == '-';
    const size_t first_digit = is_negative ? 1 : 0;
    const size_t digit_count = decimal_text.byte_count - first_digit;
    limb_number number;
    sxml_status status = limb_number_allocate(call, digit_count / 9U + 1U, &number);
    if (status == SXML_STATUS_OK) {
        status = charge_octets(call, digit_count * buffer->byte_count / 64U);
    }
    if (status != SXML_STATUS_OK) {
        limb_number_release(&number);
        return status;
    }
    /* Limbs from the last nine digits backwards. */
    size_t end = decimal_text.byte_count;
    while (end > first_digit) {
        const size_t start = end - first_digit >= 9 ? end - 9 : first_digit;
        uint32_t limb = 0;
        for (size_t digit_index = start; digit_index < end; digit_index += 1) {
            limb = limb * 10U + (uint32_t)(decimal_text.bytes[digit_index] - '0');
        }
        number.limbs[number.limb_count] = limb;
        number.limb_count += 1;
        end = start;
    }
    /* Divide by 256 for each octet, least significant first. */
    for (size_t octet_index = 0; octet_index < buffer->byte_count && number.limb_count > 0;
         octet_index += 1) {
        uint64_t remainder = 0;
        for (size_t limb_index = number.limb_count; limb_index > 0; limb_index -= 1) {
            const uint64_t current =
                remainder * SXML_BINARY_LIMB_BASE + number.limbs[limb_index - 1];
            number.limbs[limb_index - 1] = (uint32_t)(current / 256U);
            remainder = current % 256U;
        }
        while (number.limb_count > 0 && number.limbs[number.limb_count - 1] == 0) {
            number.limb_count -= 1;
        }
        buffer->bytes[buffer->byte_count - 1 - octet_index] = (uint8_t)remainder;
    }
    limb_number_release(&number);
    if (is_negative) {
        /* -m modulo 256^n: invert and add one. */
        unsigned carry = 1;
        for (size_t octet_index = buffer->byte_count; octet_index > 0; octet_index -= 1) {
            const unsigned inverted = (unsigned)(uint8_t)~buffer->bytes[octet_index - 1] + carry;
            buffer->bytes[octet_index - 1] = (uint8_t)inverted;
            carry = inverted >> 8U;
        }
    }
    return SXML_STATUS_OK;
}

/* bin:pack-integer($value, $size, $order) (7.4): $value modulo
   256^$size in two's complement; bin:negative-size for a negative size. */
static sxml_status call_pack_integer(const sxml_function_call *call, sxml_sequence *result) {
    integer_argument value;
    integer_argument size;
    bool is_little_endian = false;
    sxml_status status = read_integer(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &size);
    }
    if (status == SXML_STATUS_OK) {
        status = read_is_little_endian(call, 2, &is_little_endian);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (size.is_negative) {
        return report(call, &negative_size_error);
    }
    if (!size.is_inline || (uint64_t)size.value > (uint64_t)SIZE_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, (size_t)size.value, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (value.is_inline) {
        const uint64_t bits = (uint64_t)value.value;
        for (size_t octet_index = 0; octet_index < buffer.byte_count; octet_index += 1) {
            buffer.bytes[buffer.byte_count - 1 - octet_index] =
                octet_index < 8 ? (uint8_t)(bits >> (8U * octet_index))
                                : (uint8_t)(value.value < 0 ? 0xFF : 0x00);
        }
    } else {
        status = pack_decimal(call, value.decimal_text, &buffer);
    }
    if (status == SXML_STATUS_OK && is_little_endian) {
        reverse_octets(buffer.bytes, buffer.byte_count);
    }
    if (status != SXML_STATUS_OK) {
        octet_buffer_release(&buffer);
        return status;
    }
    return finish_buffer(call, &buffer, result);
}

/* Reads $value, $offset and $order of an unpacking function and selects
   size octets: bin:index-out-of-range when they are not all inside. */
typedef struct unpacking {
    /* Copied, most significant first; size at most 8 here. */
    uint8_t octets[8];
    size_t byte_count;
} unpacking;

typedef struct unpacking_request {
    size_t byte_count;
    size_t order_argument;
} unpacking_request;

static sxml_status read_unpacking(const sxml_function_call *call, unpacking_request request,
                                  unpacking *selected) {
    binary_argument value;
    span_request span_arguments = {
        .size = {.has_value = true, .is_inline = true, .value = (int64_t)request.byte_count}};
    bool is_little_endian = false;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &span_arguments.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_is_little_endian(call, request.order_argument, &is_little_endian);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    span_arguments.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &span_arguments, &span);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(selected->octets, 0, sizeof(selected->octets));
    selected->byte_count = span.byte_count;
    copy_octets(selected->octets, span_octets(value.octets, span));
    if (is_little_endian) {
        reverse_octets(selected->octets, selected->byte_count);
    }
    return SXML_STATUS_OK;
}

static uint64_t unpacked_bits(const unpacking *selected) {
    uint64_t bits = 0;
    for (size_t octet_index = 0; octet_index < selected->byte_count; octet_index += 1) {
        bits = (bits << 8U) | selected->octets[octet_index];
    }
    return bits;
}

/* bin:unpack-double($value, $offset, $order) as xs:double (7.5). */
static sxml_status call_unpack_double(const sxml_function_call *call, sxml_sequence *result) {
    unpacking selected;
    const sxml_status status = read_unpacking(
        call, (unpacking_request){.byte_count = 8, .order_argument = 2}, &selected);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t bits = unpacked_bits(&selected);
    double number = 0;
    memcpy(&number, &bits, sizeof(number));
    /* Signalling NaNs and payloads become the quiet NaN (7.1.3). */
    *result = sxml_sequence_from_item(sxml_atomic_item_from_double(isnan(number) ? NAN : number));
    return SXML_STATUS_OK;
}

/* bin:unpack-float($value, $offset, $order) as xs:float (7.6). */
static sxml_status call_unpack_float(const sxml_function_call *call, sxml_sequence *result) {
    unpacking selected;
    sxml_status status = read_unpacking(
        call, (unpacking_request){.byte_count = 4, .order_argument = 2}, &selected);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t bits = (uint32_t)unpacked_bits(&selected);
    float number = 0;
    memcpy(&number, &bits, sizeof(number));
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_FLOAT),
            .datum = {.datum_kind = SXML_DATUM_KIND_FLOAT,
                      .datum_details = {.float_number = isnan(number) ? (float)NAN : number}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The most octets an unpacked integer may have: 4096 octets have at most
   9,865 decimal digits, within the item heap's decimal limit. */
#define SXML_BINARY_MAXIMUM_INTEGER_OCTETS ((size_t)4096)

/* Makes the xs:integer of octets (most significant first, a magnitude)
   with a sign, through decimal text, as *result. */
typedef struct magnitude {
    sxml_byte_view octets;
    bool is_negative;
} magnitude;

static sxml_status make_large_integer(const sxml_function_call *call, magnitude value,
                                      sxml_sequence *result) {
    limb_number number;
    sxml_status status =
        limb_number_allocate(call, value.octets.byte_count * 241U / 900U + 2U, &number);
    if (status == SXML_STATUS_OK) {
        status = charge_octets(call, value.octets.byte_count * value.octets.byte_count / 64U);
    }
    if (status != SXML_STATUS_OK) {
        limb_number_release(&number);
        return status;
    }
    for (size_t octet_index = 0; octet_index < value.octets.byte_count; octet_index += 1) {
        uint64_t carry = value.octets.bytes[octet_index];
        for (size_t limb_index = 0; limb_index < number.limb_count; limb_index += 1) {
            const uint64_t current = (uint64_t)number.limbs[limb_index] * 256U + carry;
            number.limbs[limb_index] = (uint32_t)(current % SXML_BINARY_LIMB_BASE);
            carry = current / SXML_BINARY_LIMB_BASE;
        }
        while (carry > 0) {
            number.limbs[number.limb_count] = (uint32_t)(carry % SXML_BINARY_LIMB_BASE);
            number.limb_count += 1;
            carry /= SXML_BINARY_LIMB_BASE;
        }
    }
    octet_buffer text;
    status = octet_buffer_allocate(call, number.limb_count * 9U + 2U, &text);
    if (status != SXML_STATUS_OK) {
        limb_number_release(&number);
        return status;
    }
    size_t written = 0;
    if (value.is_negative) {
        text.bytes[written] = '-';
        written += 1;
    }
    for (size_t limb_index = number.limb_count; limb_index > 0; limb_index -= 1) {
        uint32_t limb = number.limbs[limb_index - 1];
        char digits[9];
        for (size_t digit_index = 9; digit_index > 0; digit_index -= 1) {
            digits[digit_index - 1] = (char)('0' + limb % 10U);
            limb /= 10U;
        }
        size_t first = 0;
        while (limb_index == number.limb_count && first < 8 && digits[first] == '0') {
            first += 1;
        }
        memcpy(text.bytes + written, digits + first, 9 - first);
        written += 9 - first;
    }
    if (number.limb_count == 0) {
        text.bytes[written] = '0';
        written += 1;
    }
    limb_number_release(&number);
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
            .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                      .datum_details = {.decimal_text = {.bytes = text.bytes,
                                                         .byte_count = written}}}},
        &item);
    octet_buffer_release(&text);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* bin:unpack-integer and bin:unpack-unsigned-integer ($value, $offset,
   $size, $order) as xs:integer (7.7, 7.8). */
static sxml_status call_unpack(const sxml_function_call *call, bool is_signed,
                               sxml_sequence *result) {
    binary_argument value;
    span_request request = {.value_byte_count = 0};
    bool is_little_endian = false;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &request.offset);
    }
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 2, &request.size);
    }
    if (status == SXML_STATUS_OK) {
        status = read_is_little_endian(call, 3, &is_little_endian);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    request.value_byte_count = value.octets.byte_count;
    octet_span span;
    status = check_span(call, &request, &span);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (span.byte_count > SXML_BINARY_MAXIMUM_INTEGER_OCTETS) {
        return report(call, &integer_too_large_error);
    }
    octet_buffer octets;
    status = octet_buffer_allocate(call, span.byte_count, &octets);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (span.byte_count > 0) {
        copy_octets(octets.bytes, span_octets(value.octets, span));
    }
    if (is_little_endian) {
        reverse_octets(octets.bytes, octets.byte_count);
    }
    const bool is_negative = is_signed && octets.byte_count > 0 && (octets.bytes[0] & 0x80U) != 0;
    if (is_negative) {
        /* The magnitude of a negative value: invert and add one. */
        unsigned carry = 1;
        for (size_t octet_index = octets.byte_count; octet_index > 0; octet_index -= 1) {
            const unsigned inverted = (unsigned)(uint8_t)~octets.bytes[octet_index - 1] + carry;
            octets.bytes[octet_index - 1] = (uint8_t)inverted;
            carry = inverted >> 8U;
        }
    }
    /* Leading zero octets of the magnitude do not count. */
    size_t first = 0;
    while (first < octets.byte_count && octets.bytes[first] == 0) {
        first += 1;
    }
    const sxml_byte_view digits = {
        .bytes = octets.bytes == NULL ? NULL : octets.bytes + first,
        .byte_count = octets.byte_count - first};
    if (digits.byte_count < 8 || (digits.byte_count == 8 && (digits.bytes[0] & 0x80U) == 0)) {
        uint64_t bits = 0;
        for (size_t octet_index = 0; octet_index < digits.byte_count; octet_index += 1) {
            bits = (bits << 8U) | digits.bytes[octet_index];
        }
        const int64_t integer = is_negative ? -(int64_t)bits : (int64_t)bits;
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(integer));
    } else if (is_negative && digits.byte_count == 8 && digits.bytes[0] == 0x80 &&
               memcmp(digits.bytes + 1, "\0\0\0\0\0\0\0", 7) == 0) {
        /* -2^63, whose magnitude is just outside int64_t. */
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(INT64_MIN));
    } else {
        status = make_large_integer(call, (magnitude){.octets = digits, .is_negative = is_negative},
                                    result);
    }
    octet_buffer_release(&octets);
    return status;
}

static sxml_status call_unpack_integer(const sxml_function_call *call, sxml_sequence *result) {
    return call_unpack(call, true, result);
}

static sxml_status call_unpack_unsigned_integer(const sxml_function_call *call,
                                                sxml_sequence *result) {
    return call_unpack(call, false, result);
}

/* ---------------------------------------------------- Bitwise operations */

typedef enum bitwise_operation {
    SXML_BITWISE_OR,
    SXML_BITWISE_XOR,
    SXML_BITWISE_AND,
} bitwise_operation;

/* bin:or, bin:xor and bin:and ($value1, $value2) (8.1 to 8.3). */
static sxml_status call_bitwise(const sxml_function_call *call, bitwise_operation operation,
                                sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument first;
    binary_argument second;
    sxml_status status = read_binary(call, 0, &first);
    if (status == SXML_STATUS_OK) {
        status = read_binary(call, 1, &second);
    }
    if (status != SXML_STATUS_OK || !first.has_value || !second.has_value) {
        return status;
    }
    if (first.octets.byte_count != second.octets.byte_count) {
        return report(call, &differing_length_error);
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, first.octets.byte_count, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t octet_index = 0; octet_index < buffer.byte_count; octet_index += 1) {
        const uint8_t left = first.octets.bytes[octet_index];
        const uint8_t right = second.octets.bytes[octet_index];
        buffer.bytes[octet_index] = operation == SXML_BITWISE_OR    ? (uint8_t)(left | right)
                                    : operation == SXML_BITWISE_XOR ? (uint8_t)(left ^ right)
                                                                    : (uint8_t)(left & right);
    }
    return finish_buffer(call, &buffer, result);
}

static sxml_status call_or(const sxml_function_call *call, sxml_sequence *result) {
    return call_bitwise(call, SXML_BITWISE_OR, result);
}

static sxml_status call_xor(const sxml_function_call *call, sxml_sequence *result) {
    return call_bitwise(call, SXML_BITWISE_XOR, result);
}

static sxml_status call_and(const sxml_function_call *call, sxml_sequence *result) {
    return call_bitwise(call, SXML_BITWISE_AND, result);
}

/* bin:not($value) (8.4). */
static sxml_status call_not(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    sxml_status status = read_binary(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, value.octets.byte_count, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t octet_index = 0; octet_index < buffer.byte_count; octet_index += 1) {
        buffer.bytes[octet_index] = (uint8_t)~value.octets.bytes[octet_index];
    }
    return finish_buffer(call, &buffer, result);
}

/* The octet at index of octets, wrapping when it is cyclic, else 0
   outside. */
typedef struct octet_source {
    sxml_byte_view octets;
    bool is_cyclic;
} octet_source;

static uint8_t octet_at(octet_source source, uint64_t index) {
    const uint64_t length = source.octets.byte_count;
    if (source.is_cyclic) {
        return source.octets.bytes[index % length];
    }
    return index < length ? source.octets.bytes[index] : 0;
}

/* Writes the value shifted left by bit_count bits (bits that leave are
   dropped, or re-enter when cyclic) into buffer. For a right shift of a
   non-cyclic value the caller passes bit_count as the left shift of the
   value padded on the left, see call_shift. */
typedef struct left_shift {
    octet_source source;
    /* Octets and bits the result starts from, into the (padded) value. */
    uint64_t octet_offset;
    unsigned bit_offset;
} left_shift;

static void shift_into(left_shift shift, octet_buffer *buffer) {
    for (size_t octet_index = 0; octet_index < buffer->byte_count; octet_index += 1) {
        const uint64_t index = shift.octet_offset + octet_index;
        const unsigned high = octet_at(shift.source, index);
        const unsigned low = shift.bit_offset == 0 ? 0U : octet_at(shift.source, index + 1U);
        buffer->bytes[octet_index] =
            (uint8_t)((high << shift.bit_offset) | (low >> (8U - shift.bit_offset)));
    }
}

/* Reads $value and $by of bin:shift and bin:rotate. */
typedef struct shift_arguments {
    binary_argument value;
    integer_argument by;
} shift_arguments;

static sxml_status read_shift(const sxml_function_call *call, shift_arguments *arguments) {
    sxml_status status = read_binary(call, 0, &arguments->value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &arguments->by);
    }
    return status;
}

/* bin:shift($value, $by) (8.5): left for a positive $by, right for a
   negative one, zero bits injected; all zeros past the bit length. */
static sxml_status call_shift(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    shift_arguments arguments;
    sxml_status status = read_shift(call, &arguments);
    if (status != SXML_STATUS_OK || !arguments.value.has_value) {
        return status;
    }
    const sxml_byte_view octets = arguments.value.octets;
    octet_buffer buffer;
    status = octet_buffer_allocate(call, octets.byte_count, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t bit_length = (uint64_t)octets.byte_count * 8U;
    const uint64_t distance =
        !arguments.by.is_inline ? UINT64_MAX
        : arguments.by.value < 0 ? (uint64_t)0 - (uint64_t)arguments.by.value
                                 : (uint64_t)arguments.by.value;
    if (distance < bit_length && distance > 0) {
        if (!arguments.by.is_negative) {
            shift_into((left_shift){.source = {.octets = octets, .is_cyclic = false},
                                    .octet_offset = distance / 8U,
                                    .bit_offset = (unsigned)(distance % 8U)},
                       &buffer);
        } else {
            /* A right shift by d is a left shift by 8k - d of the value
               after k zero octets, k = ceil(d / 8). */
            const uint64_t padding_octets = (distance + 7U) / 8U;
            const uint64_t left = padding_octets * 8U - distance;
            for (size_t octet_index = 0; octet_index < buffer.byte_count; octet_index += 1) {
                const uint64_t index = octet_index + left / 8U;
                const unsigned bit_offset = (unsigned)(left % 8U);
                const unsigned high = index < padding_octets
                                          ? 0U
                                          : octet_at((octet_source){.octets = octets,
                                                                    .is_cyclic = false},
                                                     index - padding_octets);
                const unsigned low =
                    bit_offset == 0 || index + 1U < padding_octets
                        ? 0U
                        : octet_at((octet_source){.octets = octets, .is_cyclic = false},
                                   index + 1U - padding_octets);
                buffer.bytes[octet_index] =
                    (uint8_t)((high << bit_offset) | (low >> (8U - bit_offset)));
            }
        }
    } else if (distance == 0 && octets.byte_count > 0) {
        copy_octets(buffer.bytes, octets);
    }
    return finish_buffer(call, &buffer, result);
}

/* $by modulo modulus for an integer beyond int64_t, from its decimal
   text, as a value from 0 to modulus - 1. */
static uint64_t decimal_modulo(sxml_byte_view decimal_text, uint64_t modulus) {
    uint64_t remainder = 0;
    for (size_t digit_index = 0; digit_index < decimal_text.byte_count; digit_index += 1) {
        const uint8_t digit = decimal_text.bytes[digit_index];
        if (digit >= '0' && digit <= '9') {
            remainder = (remainder * 10U + (uint64_t)(digit - '0')) % modulus;
        }
    }
    const bool is_negative = decimal_text.byte_count > 0 && decimal_text.bytes[0] == '-';
    return is_negative && remainder != 0 ? modulus - remainder : remainder;
}

/* bin:rotate($value, $by) (8.6): cyclic modulo the bit length. */
static sxml_status call_rotate(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    shift_arguments arguments;
    sxml_status status = read_shift(call, &arguments);
    if (status != SXML_STATUS_OK || !arguments.value.has_value) {
        return status;
    }
    const sxml_byte_view octets = arguments.value.octets;
    if (octets.byte_count == 0) {
        return make_binary(call, octets, result);
    }
    const uint64_t bit_length = (uint64_t)octets.byte_count * 8U;
    uint64_t left = 0;
    if (!arguments.by.is_inline) {
        left = decimal_modulo(arguments.by.decimal_text, bit_length);
    } else if (arguments.by.value >= 0) {
        left = (uint64_t)arguments.by.value % bit_length;
    } else {
        const uint64_t right = ((uint64_t)0 - (uint64_t)arguments.by.value) % bit_length;
        left = right == 0 ? 0 : bit_length - right;
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, octets.byte_count, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    shift_into((left_shift){.source = {.octets = octets, .is_cyclic = true},
                            .octet_offset = left / 8U,
                            .bit_offset = (unsigned)(left % 8U)},
               &buffer);
    return finish_buffer(call, &buffer, result);
}

/* Checks a bit index of a value of byte_count octets. */
static bool bit_index_is_valid(integer_argument index, size_t byte_count) {
    return index.is_inline && !index.is_negative &&
           (uint64_t)index.value / 8U < (uint64_t)byte_count;
}

/* bin:is-bit-set($value, $index) as xs:boolean? (8.7). */
static sxml_status call_is_bit_set(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    integer_argument index;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = read_integer(call, 1, &index);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    if (!bit_index_is_valid(index, value.octets.byte_count)) {
        return report(call, &index_error);
    }
    const uint64_t bit = (uint64_t)index.value;
    *result = sxml_function_library_make_boolean((sxml_boolean_result){
        .is_true = ((value.octets.bytes[bit / 8U] >> (7U - bit % 8U)) & 1U) != 0});
    return SXML_STATUS_OK;
}

/* bin:set-bits($value, $indices, $set) (8.8). */
static sxml_status call_set_bits(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    sxml_optional_item set;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &set);
    }
    sxml_atomic_datum set_datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(set.item, &set_datum);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    octet_buffer buffer;
    status = octet_buffer_allocate(call, value.octets.byte_count, &buffer);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (buffer.byte_count > 0) {
        memcpy(buffer.bytes, value.octets.bytes, buffer.byte_count);
    }
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 1));
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false, .item = {.representation = 0}};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        integer_argument index = {.has_value = true, .is_inline = false, .is_negative = false};
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
            index = (integer_argument){.has_value = true,
                                       .is_inline = true,
                                       .value = datum.datum_details.integer,
                                       .is_negative = datum.datum_details.integer < 0};
        }
        if (status == SXML_STATUS_OK && !bit_index_is_valid(index, buffer.byte_count)) {
            status = report(call, &index_error);
        }
        if (status == SXML_STATUS_OK) {
            const uint64_t bit = (uint64_t)index.value;
            const uint8_t mask = (uint8_t)(1U << (7U - bit % 8U));
            if (set_datum.datum_details.boolean) {
                buffer.bytes[bit / 8U] |= mask;
            } else {
                buffer.bytes[bit / 8U] &= (uint8_t)~mask;
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        octet_buffer_release(&buffer);
        return status;
    }
    return finish_buffer(call, &buffer, result);
}

/* bin:count-bits-set($value) as xs:integer? (8.9). */
static sxml_status call_count_bits_set(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    binary_argument value;
    sxml_status status = read_binary(call, 0, &value);
    if (status == SXML_STATUS_OK && value.has_value) {
        status = charge_octets(call, value.octets.byte_count);
    }
    if (status != SXML_STATUS_OK || !value.has_value) {
        return status;
    }
    int64_t count = 0;
    for (size_t octet_index = 0; octet_index < value.octets.byte_count; octet_index += 1) {
        for (unsigned octet = value.octets.bytes[octet_index]; octet != 0; octet &= octet - 1U) {
            count += 1;
        }
    }
    *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(count));
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------------ Rows */

/* A row in the bin namespace (EXPath Binary 4.0, section 1.5). */
#define SXML_BINARY_ROW(name_text, parameter_array, result_text, body)                   \
    {.function_namespace = SXML_NAMESPACE_EXPATH_BINARY,                                  \
     .properties = 0,                                                                     \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                 \
     .parameters = {(parameter_array), sizeof(parameter_array) / sizeof((parameter_array)[0])}, \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                              \
     .call = (body),                                                                      \
     .resumable_body = NULL}

#define SXML_BINARY_TYPE "(xs:hexBinary | xs:base64Binary)"
#define SXML_OCTET_ORDER_TYPE                                                            \
    "enum('least-significant-first', 'little-endian', 'LE', 'most-significant-first', " \
    "'big-endian', 'BE')?"

static const sxml_function_parameter string_value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
};
static const sxml_function_parameter binary_value_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE),
};
static const sxml_function_parameter optional_binary_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
};
static const sxml_function_parameter from_octets_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:unsignedByte*"),
};
static const sxml_function_parameter part_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("offset", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("size", "xs:integer?"),
};
static const sxml_function_parameter join_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", SXML_BINARY_TYPE "*"),
};
static const sxml_function_parameter insert_before_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("offset", "xs:integer"),
    SXML_REQUIRED_PARAMETER("extra", SXML_BINARY_TYPE "?"),
};
static const sxml_function_parameter pad_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("count", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("octet", "xs:unsignedByte?"),
};
static const sxml_function_parameter find_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("offset", "xs:integer"),
    SXML_REQUIRED_PARAMETER("search", SXML_BINARY_TYPE),
};
static const sxml_function_parameter infer_encoding_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE),
    SXML_OPTIONAL_PARAMETER("encoding", "xs:string?"),
};
static const sxml_function_parameter decode_string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_OPTIONAL_PARAMETER("encoding", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("offset", "xs:integer?"),
    SXML_OPTIONAL_PARAMETER("size", "xs:integer?"),
};
static const sxml_function_parameter encode_string_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("encoding", "xs:string?"),
};
static const sxml_function_parameter pack_double_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:double"),
    SXML_OPTIONAL_PARAMETER("order", SXML_OCTET_ORDER_TYPE),
};
static const sxml_function_parameter pack_float_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:float"),
    SXML_OPTIONAL_PARAMETER("order", SXML_OCTET_ORDER_TYPE),
};
static const sxml_function_parameter pack_integer_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:integer"),
    SXML_REQUIRED_PARAMETER("size", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("order", SXML_OCTET_ORDER_TYPE),
};
static const sxml_function_parameter unpack_number_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE),
    SXML_REQUIRED_PARAMETER("offset", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("order", SXML_OCTET_ORDER_TYPE),
};
static const sxml_function_parameter unpack_integer_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE),
    SXML_REQUIRED_PARAMETER("offset", "xs:integer"),
    SXML_REQUIRED_PARAMETER("size", "xs:integer"),
    SXML_OPTIONAL_PARAMETER("order", SXML_OCTET_ORDER_TYPE),
};
static const sxml_function_parameter bitwise_parameters[] = {
    SXML_REQUIRED_PARAMETER("value1", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("value2", SXML_BINARY_TYPE "?"),
};
static const sxml_function_parameter shift_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("by", "xs:integer"),
};
static const sxml_function_parameter is_bit_set_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("index", "xs:integer"),
};
static const sxml_function_parameter set_bits_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", SXML_BINARY_TYPE "?"),
    SXML_REQUIRED_PARAMETER("indices", "xs:integer*"),
    SXML_REQUIRED_PARAMETER("set", "xs:boolean"),
};

static const sxml_native_function binary_functions[] = {
    SXML_BINARY_ROW("hex", string_value_parameters, "xs:base64Binary?", call_hex),
    SXML_BINARY_ROW("bin", string_value_parameters, "xs:base64Binary?", call_bin),
    SXML_BINARY_ROW("octal", string_value_parameters, "xs:base64Binary?", call_octal),
    SXML_BINARY_ROW("to-octets", binary_value_parameters, "xs:unsignedByte*", call_to_octets),
    SXML_BINARY_ROW("from-octets", from_octets_parameters, "xs:base64Binary", call_from_octets),
    SXML_BINARY_ROW("to-base64url", optional_binary_parameters, "xs:string?", call_to_base64url),
    SXML_BINARY_ROW("from-base64url", string_value_parameters, "xs:base64Binary?",
                    call_from_base64url),
    SXML_BINARY_ROW("length", binary_value_parameters, "xs:integer", call_length),
    SXML_BINARY_ROW("part", part_parameters, "xs:base64Binary?", call_part),
    SXML_BINARY_ROW("join", join_parameters, "xs:base64Binary", call_join),
    SXML_BINARY_ROW("insert-before", insert_before_parameters, "xs:base64Binary?",
                    call_insert_before),
    SXML_BINARY_ROW("pad-left", pad_parameters, "xs:base64Binary?", call_pad_left),
    SXML_BINARY_ROW("pad-right", pad_parameters, "xs:base64Binary?", call_pad_right),
    SXML_BINARY_ROW("find", find_parameters, "xs:integer?", call_find),
    SXML_BINARY_ROW("infer-encoding", infer_encoding_parameters,
                    "record(encoding as xs:string, offset as xs:integer)", call_infer_encoding),
    SXML_BINARY_ROW("decode-string", decode_string_parameters, "xs:string?", call_decode_string),
    SXML_BINARY_ROW("encode-string", encode_string_parameters, "xs:base64Binary?",
                    call_encode_string),
    SXML_BINARY_ROW("pack-double", pack_double_parameters, "xs:base64Binary", call_pack_double),
    SXML_BINARY_ROW("pack-float", pack_float_parameters, "xs:base64Binary", call_pack_float),
    SXML_BINARY_ROW("pack-integer", pack_integer_parameters, "xs:base64Binary",
                    call_pack_integer),
    SXML_BINARY_ROW("unpack-double", unpack_number_parameters, "xs:double", call_unpack_double),
    SXML_BINARY_ROW("unpack-float", unpack_number_parameters, "xs:float", call_unpack_float),
    SXML_BINARY_ROW("unpack-integer", unpack_integer_parameters, "xs:integer",
                    call_unpack_integer),
    SXML_BINARY_ROW("unpack-unsigned-integer", unpack_integer_parameters, "xs:integer",
                    call_unpack_unsigned_integer),
    SXML_BINARY_ROW("or", bitwise_parameters, "xs:base64Binary?", call_or),
    SXML_BINARY_ROW("xor", bitwise_parameters, "xs:base64Binary?", call_xor),
    SXML_BINARY_ROW("and", bitwise_parameters, "xs:base64Binary?", call_and),
    SXML_BINARY_ROW("not", optional_binary_parameters, "xs:base64Binary?", call_not),
    SXML_BINARY_ROW("shift", shift_parameters, "xs:base64Binary?", call_shift),
    SXML_BINARY_ROW("rotate", shift_parameters, "xs:base64Binary?", call_rotate),
    SXML_BINARY_ROW("is-bit-set", is_bit_set_parameters, "xs:boolean?", call_is_bit_set),
    SXML_BINARY_ROW("set-bits", set_bits_parameters, "xs:base64Binary?", call_set_bits),
    SXML_BINARY_ROW("count-bits-set", optional_binary_parameters, "xs:integer?",
                    call_count_bits_set),
};

/* Not named sxml_function_library_<part>: not a registered part (D191). */
static const sxml_native_function_view binary_library_functions = SXML_LIBRARY_PART(binary_functions);

/* --------------------------------------------------------------- Feature */

const sxml_optional_feature_definition sxml_expath_binary_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_EXPATH_BINARY,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("expath-binary"),
    .operations = &binary_library_functions,
    .finalize_state = NULL,
    .library_functions = &binary_library_functions,
};

sxml_optional_feature sxml_expath_binary_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_expath_binary_feature_definition};
}
