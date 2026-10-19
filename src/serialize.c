/*
 * Serialization (include/sxml/serialize.h; Serialization 4.0; D47, D51,
 * D82, D106, D120b): the output stream wrapper, the output buffer, the
 * character pipeline every method writes through (src/serialize_internal.h),
 * sequence normalization, the adaptive output method (Serialization 4.0,
 * section 11 "Adaptive Output Method") and the dispatch by method. The XML
 * and XHTML methods are in src/serialize_xml.c.
 *
 * No recursion (D71): arrays, maps, JNodes and the sequences inside them
 * are written from an explicit stack of serialization frames in heap
 * memory; sequence normalization flattens arrays with a stack of its own;
 * nodes are written by src/serialize_xml.c with a stack of open elements.
 *
 * Golden file: src/memory.c (lifecycle, checked sizes).
 */
#include "sxml/serialize.h"

#include "cast_internal.h"
#include "serialize_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "unicode_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ----------------------------------------------------------- Output stream */

sxml_status sxml_output_stream_write(sxml_output_stream output_stream,
                                     sxml_byte_view bytes) {
    if (output_stream.operations == NULL || output_stream.operations->write == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return output_stream.operations->write(output_stream.state, bytes);
}

/* ----------------------------------------------------------- Output buffer */

struct sxml_output_buffer {
    sxml_allocator allocator;
    /* The allocator of sxml_output_buffer_create; the null allocator after
       initialize. */
    sxml_allocator creating_allocator;
    size_t maximum_byte_count;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
};

static sxml_status sxml_output_buffer_check_configuration(
    const sxml_output_buffer_configuration *configuration) {
    if (configuration == NULL || sxml_allocator_is_null(configuration->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_output_buffer_memory_requirements(
    const sxml_output_buffer_configuration *configuration,
    sxml_memory_requirements *requirements) {
    const sxml_status status = sxml_output_buffer_check_configuration(configuration);
    if (status != SXML_STATUS_OK || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_output_buffer),
                                               .alignment = alignof(sxml_output_buffer)};
    return SXML_STATUS_OK;
}

sxml_status sxml_output_buffer_initialize(
    sxml_memory_view memory, const sxml_output_buffer_configuration *configuration,
    sxml_output_buffer **output_buffer) {
    sxml_memory_requirements requirements;
    sxml_status status = sxml_output_buffer_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && output_buffer == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_output_buffer *buffer = placement;
    *buffer = (sxml_output_buffer){.allocator = configuration->allocator,
                                   .creating_allocator = {.state = NULL, .operations = NULL},
                                   .maximum_byte_count = configuration->maximum_byte_count,
                                   .bytes = NULL,
                                   .byte_count = 0,
                                   .byte_capacity = 0};
    *output_buffer = buffer;
    return SXML_STATUS_OK;
}

void sxml_output_buffer_finalize(sxml_output_buffer *output_buffer) {
    if (output_buffer == NULL) {
        return;
    }
    sxml_allocator_release(output_buffer->allocator, output_buffer->bytes);
    output_buffer->bytes = NULL;
    output_buffer->byte_count = 0;
    output_buffer->byte_capacity = 0;
}

sxml_status sxml_output_buffer_create(sxml_allocator allocator,
                                      const sxml_output_buffer_configuration *configuration,
                                      sxml_output_buffer **output_buffer) {
    sxml_memory_requirements requirements;
    sxml_status status = sxml_output_buffer_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && output_buffer == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_output_buffer *buffer = NULL;
    status = sxml_output_buffer_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &buffer);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    buffer->creating_allocator = allocator;
    *output_buffer = buffer;
    return SXML_STATUS_OK;
}

void sxml_output_buffer_destroy(sxml_output_buffer *output_buffer) {
    if (output_buffer == NULL) {
        return;
    }
    const sxml_allocator creating_allocator = output_buffer->creating_allocator;
    sxml_output_buffer_finalize(output_buffer);
    sxml_allocator_release(creating_allocator, output_buffer);
}

/* Appends bytes, growing the memory geometrically; the buffer is
   unchanged on failure. */
static sxml_status sxml_output_buffer_write(void *state, sxml_byte_view bytes) {
    sxml_output_buffer *buffer = state;
    size_t required = 0;
    sxml_status status = sxml_size_add(buffer->byte_count, bytes.byte_count, &required);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (buffer->maximum_byte_count != 0 && required > buffer->maximum_byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (required > buffer->byte_capacity) {
        size_t capacity = buffer->byte_capacity < 256 ? 256 : buffer->byte_capacity;
        while (status == SXML_STATUS_OK && capacity < required) {
            status = sxml_size_multiply(capacity, 2, &capacity);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                buffer->allocator,
                (sxml_allocation_request){.byte_count = capacity, .alignment = 1}, &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (buffer->byte_count > 0) {
            memcpy(memory, buffer->bytes, buffer->byte_count);
        }
        sxml_allocator_release(buffer->allocator, buffer->bytes);
        buffer->bytes = memory;
        buffer->byte_capacity = capacity;
    }
    memcpy(buffer->bytes + buffer->byte_count, bytes.bytes, bytes.byte_count);
    buffer->byte_count = required;
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations sxml_output_buffer_stream_operations = {
    .write = sxml_output_buffer_write,
};

sxml_output_stream sxml_output_buffer_as_output_stream(sxml_output_buffer *output_buffer) {
    return (sxml_output_stream){.state = output_buffer,
                                .operations = &sxml_output_buffer_stream_operations};
}

sxml_byte_view sxml_output_buffer_get_bytes(const sxml_output_buffer *output_buffer) {
    return (sxml_byte_view){.bytes = output_buffer->bytes,
                            .byte_count = output_buffer->byte_count};
}

void sxml_output_buffer_clear(sxml_output_buffer *output_buffer) {
    output_buffer->byte_count = 0;
}

/* ------------------------------------------------------- The serializer */

sxml_status sxml_serializer_grow(sxml_allocator allocator, const sxml_serializer_growth *growth) {
    if (growth->element_count < *growth->element_capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = *growth->element_capacity < 8 ? 8 : 0;
    sxml_status status = SXML_STATUS_OK;
    if (capacity == 0) {
        status = sxml_size_multiply(*growth->element_capacity, 2, &capacity);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(capacity, growth->element_byte_count, &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = growth->alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (growth->element_count > 0) {
        memcpy(memory, *growth->elements, growth->element_count * growth->element_byte_count);
    }
    sxml_allocator_release(allocator, *growth->elements);
    *growth->elements = memory;
    *growth->element_capacity = capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_serializer_raise_error(sxml_serializer *serializer,
                                        sxml_serialization_error error) {
    if (serializer->error_record != NULL) {
        *serializer->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = error.error_local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = error.description, .location = {.line_number = 0}},
        };
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* Serialization 4.0, 5.1.3: a character the encoding cannot represent
   where no character reference is allowed. */
static sxml_status sxml_serializer_raise_unencodable(sxml_serializer *serializer) {
    return sxml_serializer_raise_error(
        serializer,
        (sxml_serialization_error){
            .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0008"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "a character the output encoding cannot represent is not in text or an "
                "attribute value")});
}

sxml_byte_view sxml_serializer_get_encoding_name(const sxml_serializer *serializer) {
    switch (serializer->character_encoding) {
    case SXML_CHARACTER_ENCODING_UTF_8:
        break;
    case SXML_CHARACTER_ENCODING_UTF_16:
        return SXML_BYTE_VIEW_FROM_LITERAL("UTF-16");
    case SXML_CHARACTER_ENCODING_US_ASCII:
        return SXML_BYTE_VIEW_FROM_LITERAL("US-ASCII");
    case SXML_CHARACTER_ENCODING_ISO_8859_1:
        return SXML_BYTE_VIEW_FROM_LITERAL("ISO-8859-1");
    }
    return SXML_BYTE_VIEW_FROM_LITERAL("UTF-8");
}

/* ------------------------------------------------------------ Encoding */

sxml_status sxml_serializer_flush(sxml_serializer *serializer) {
    const sxml_byte_view staged = {.bytes = serializer->staged_bytes,
                                   .byte_count = serializer->staged_byte_count};
    serializer->staged_byte_count = 0;
    return sxml_output_stream_write(serializer->output_stream, staged);
}

/* Appends encoded bytes to the staging area, flushing it when full. */
static sxml_status sxml_serializer_put_bytes(sxml_serializer *serializer, sxml_byte_view bytes) {
    sxml_status status = SXML_STATUS_OK;
    if (serializer->staged_byte_count + bytes.byte_count > SXML_SERIALIZER_STAGING_BYTE_COUNT) {
        status = sxml_serializer_flush(serializer);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (bytes.byte_count > SXML_SERIALIZER_STAGING_BYTE_COUNT) {
        return sxml_output_stream_write(serializer->output_stream, bytes);
    }
    if (bytes.byte_count > 0) {
        memcpy(serializer->staged_bytes + serializer->staged_byte_count, bytes.bytes,
               bytes.byte_count);
        serializer->staged_byte_count += bytes.byte_count;
    }
    return SXML_STATUS_OK;
}

/* Reads the codepoint at *byte_offset of well-formed UTF-8 and advances
   the offset (in/out); a malformed byte reads as U+FFFD. */
static uint32_t sxml_serializer_read_codepoint(sxml_byte_view text, size_t *byte_offset) {
    const size_t offset = *byte_offset;
    const uint8_t lead = text.bytes[offset];
    size_t length = 1;
    uint32_t codepoint = lead;
    if (lead >= 0xF0U) {
        length = 4;
        codepoint = lead & 0x07U;
    } else if (lead >= 0xE0U) {
        length = 3;
        codepoint = lead & 0x0FU;
    } else if (lead >= 0xC0U) {
        length = 2;
        codepoint = lead & 0x1FU;
    } else if (lead >= 0x80U) {
        *byte_offset = offset + 1;
        return 0xFFFDU;
    }
    if (offset + length > text.byte_count) {
        *byte_offset = text.byte_count;
        return 0xFFFDU;
    }
    for (size_t continuation = 1; continuation < length; continuation += 1) {
        codepoint = (codepoint << 6) | (text.bytes[offset + continuation] & 0x3FU);
    }
    *byte_offset = offset + length;
    return codepoint;
}

/* Whether the output encoding represents codepoint (Serialization 4.0,
   5.1.3). */
static bool sxml_serializer_can_encode(const sxml_serializer *serializer, uint32_t codepoint) {
    switch (serializer->character_encoding) {
    case SXML_CHARACTER_ENCODING_UTF_8:
    case SXML_CHARACTER_ENCODING_UTF_16:
        return true;
    case SXML_CHARACTER_ENCODING_US_ASCII:
        return codepoint < 0x80U;
    case SXML_CHARACTER_ENCODING_ISO_8859_1:
        return codepoint < 0x100U;
    }
    return false;
}

/* Encodes one codepoint the encoding represents. */
static sxml_status sxml_serializer_put_codepoint(sxml_serializer *serializer, uint32_t codepoint) {
    uint8_t encoded[4];
    size_t encoded_count = 0;
    if (serializer->character_encoding == SXML_CHARACTER_ENCODING_UTF_16) {
        /* RFC 2781, section 2.1; big endian (D106). */
        if (codepoint >= 0x10000U) {
            const uint32_t offset = codepoint - 0x10000U;
            const uint32_t high = 0xD800U | (offset >> 10);
            const uint32_t low = 0xDC00U | (offset & 0x3FFU);
            encoded[0] = (uint8_t)(high >> 8);
            encoded[1] = (uint8_t)(high & 0xFFU);
            encoded[2] = (uint8_t)(low >> 8);
            encoded[3] = (uint8_t)(low & 0xFFU);
            encoded_count = 4;
        } else {
            encoded[0] = (uint8_t)(codepoint >> 8);
            encoded[1] = (uint8_t)(codepoint & 0xFFU);
            encoded_count = 2;
        }
    } else if (serializer->character_encoding != SXML_CHARACTER_ENCODING_UTF_8 ||
               codepoint < 0x80U) {
        encoded[0] = (uint8_t)codepoint;
        encoded_count = 1;
    } else if (codepoint < 0x800U) {
        encoded[0] = (uint8_t)(0xC0U | (codepoint >> 6));
        encoded[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_count = 2;
    } else if (codepoint < 0x10000U) {
        encoded[0] = (uint8_t)(0xE0U | (codepoint >> 12));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0U | (codepoint >> 18));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 12) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU));
        encoded[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_count = 4;
    }
    return sxml_serializer_put_bytes(serializer,
                                     (sxml_byte_view){.bytes = encoded, .byte_count = encoded_count});
}

/* Encodes ASCII text (references, the line ending), byte by byte. */
static sxml_status sxml_serializer_put_ascii(sxml_serializer *serializer, sxml_byte_view text) {
    if (serializer->character_encoding != SXML_CHARACTER_ENCODING_UTF_16) {
        return sxml_serializer_put_bytes(serializer, text);
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t offset = 0; status == SXML_STATUS_OK && offset < text.byte_count; offset += 1) {
        status = sxml_serializer_put_codepoint(serializer, text.bytes[offset]);
    }
    return status;
}

/* Encodes a codepoint of the serialized text: U+000A becomes the line
   ending (Serialization 4.0, 5.1.7). */
static sxml_status sxml_serializer_put_text_codepoint(sxml_serializer *serializer,
                                                      uint32_t codepoint) {
    if (codepoint == '\n') {
        return sxml_serializer_put_ascii(serializer, serializer->line_ending_text);
    }
    return sxml_serializer_put_codepoint(serializer, codepoint);
}

/* Writes &#xHEX; (Serialization 4.0, 5.1.3, and the round-trip rules of
   section 5). */
static sxml_status sxml_serializer_put_character_reference(sxml_serializer *serializer,
                                                           uint32_t codepoint) {
    static const char hexadecimal_digits[] = "0123456789ABCDEF";
    uint8_t reference[12] = {'&', '#', 'x'};
    size_t reference_count = 3;
    bool has_digit = false;
    for (int shift = 20; shift >= 0; shift -= 4) {
        const uint32_t digit = (codepoint >> (uint32_t)shift) & 0xFU;
        if (digit != 0 || has_digit || shift == 0) {
            reference[reference_count++] = (uint8_t)hexadecimal_digits[digit];
            has_digit = true;
        }
    }
    reference[reference_count++] = ';';
    return sxml_serializer_put_ascii(
        serializer, (sxml_byte_view){.bytes = reference, .byte_count = reference_count});
}

sxml_status sxml_serializer_write_markup(sxml_serializer *serializer, sxml_byte_view markup) {
    if (serializer->character_encoding == SXML_CHARACTER_ENCODING_UTF_8 &&
        serializer->line_ending_text.byte_count == 1 &&
        serializer->line_ending_text.bytes[0] == '\n') {
        return sxml_serializer_put_bytes(serializer, markup);
    }
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < markup.byte_count) {
        const uint32_t codepoint = sxml_serializer_read_codepoint(markup, &offset);
        status = sxml_serializer_can_encode(serializer, codepoint)
                     ? sxml_serializer_put_text_codepoint(serializer, codepoint)
                     : sxml_serializer_raise_unencodable(serializer);
    }
    return status;
}

/* ------------------------------------------------------------ Escaping */

/* The XML escape of codepoint in text or an attribute value, or an empty
   view when it is written as it is (Serialization 4.0, section 5: the
   characters that must be references to survive a round trip). */
typedef struct sxml_escape_query {
    uint32_t codepoint;
    sxml_character_context character_context;
} sxml_escape_query;

static sxml_byte_view sxml_find_escape(sxml_escape_query query) {
    const bool is_attribute =
        query.character_context == SXML_CHARACTER_CONTEXT_ATTRIBUTE ||
        query.character_context == SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE;
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    switch (query.codepoint) {
    case '&':
        return SXML_BYTE_VIEW_FROM_LITERAL("&amp;");
    case '<':
        return SXML_BYTE_VIEW_FROM_LITERAL("&lt;");
    case '>':
        return is_attribute ? none : SXML_BYTE_VIEW_FROM_LITERAL("&gt;");
    case '"':
        return is_attribute ? SXML_BYTE_VIEW_FROM_LITERAL("&quot;") : none;
    case '\r':
        return SXML_BYTE_VIEW_FROM_LITERAL("&#xD;");
    case '\t':
        return is_attribute ? SXML_BYTE_VIEW_FROM_LITERAL("&#x9;") : none;
    case '\n':
        return is_attribute ? SXML_BYTE_VIEW_FROM_LITERAL("&#xA;") : none;
    default:
        break;
    }
    return none;
}

/* Whether codepoint is written as a character reference in text and
   attribute values whatever the encoding (Serialization 4.0, section 5:
   C0 and C1 controls, NEL, LINE SEPARATOR). */
static bool sxml_codepoint_needs_reference(uint32_t codepoint) {
    return (codepoint >= 0x01U && codepoint <= 0x1FU && codepoint != '\t' && codepoint != '\n' &&
            codepoint != '\r') ||
           (codepoint >= 0x7FU && codepoint <= 0x9FU) || codepoint == 0x2028U;
}

/* Writes \uHHHH (RFC 8259, section 7): upper case hexadecimal digits, or
   lower case for RFC 8785 (canonical JSON). */
static sxml_status sxml_serializer_put_json_unit(sxml_serializer *serializer, uint32_t unit) {
    const char *digits = serializer->is_canonical_json ? "0123456789abcdef" : "0123456789ABCDEF";
    const uint8_t escape[6] = {'\\', 'u', (uint8_t)digits[(unit >> 12) & 0xFU],
                               (uint8_t)digits[(unit >> 8) & 0xFU], (uint8_t)digits[(unit >> 4) & 0xFU],
                               (uint8_t)digits[unit & 0xFU]};
    return sxml_serializer_put_ascii(serializer,
                                     (sxml_byte_view){.bytes = escape, .byte_count = sizeof(escape)});
}

/* Writes one codepoint of a JSON string (Serialization 4.0, 9.1: the JSON
   escaping; RFC 8785, section 3.2.2.2, when canonical). */
static sxml_status sxml_serializer_put_json_codepoint(sxml_serializer *serializer,
                                                      uint32_t codepoint) {
    static const struct {
        uint32_t codepoint;
        sxml_byte_view escape;
    } short_escapes[] = {
        {'"', SXML_BYTE_VIEW_INITIALIZER("\\\"")}, {'\\', SXML_BYTE_VIEW_INITIALIZER("\\\\")},
        {'\b', SXML_BYTE_VIEW_INITIALIZER("\\b")},  {'\f', SXML_BYTE_VIEW_INITIALIZER("\\f")},
        {'\n', SXML_BYTE_VIEW_INITIALIZER("\\n")},  {'\r', SXML_BYTE_VIEW_INITIALIZER("\\r")},
        {'\t', SXML_BYTE_VIEW_INITIALIZER("\\t")},
    };
    for (size_t escape_index = 0; escape_index < sizeof(short_escapes) / sizeof(short_escapes[0]);
         escape_index += 1) {
        if (short_escapes[escape_index].codepoint == codepoint) {
            return sxml_serializer_put_ascii(serializer, short_escapes[escape_index].escape);
        }
    }
    if (codepoint == '/' && serializer->escapes_solidus && !serializer->is_canonical_json) {
        return sxml_serializer_put_ascii(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\\/"));
    }
    const bool is_control =
        codepoint < 0x20U ||
        (!serializer->is_canonical_json && codepoint >= 0x7FU && codepoint <= 0x9FU);
    if (!is_control && sxml_serializer_can_encode(serializer, codepoint)) {
        return sxml_serializer_put_codepoint(serializer, codepoint);
    }
    if (codepoint < 0x10000U) {
        return sxml_serializer_put_json_unit(serializer, codepoint);
    }
    /* RFC 8259, section 7: a surrogate pair. */
    const uint32_t offset = codepoint - 0x10000U;
    const sxml_status status = sxml_serializer_put_json_unit(serializer, 0xD800U | (offset >> 10));
    return status == SXML_STATUS_OK
               ? sxml_serializer_put_json_unit(serializer, 0xDC00U | (offset & 0x3FFU))
               : status;
}

/* Writes one codepoint of a CSV field (Serialization 4.0, 10.2): the quote
   character doubled; a carriage return, alone or before a newline, and a
   newline become the line ending. *was_carriage_return carries a carriage
   return to the next codepoint (in/out). */
typedef struct sxml_csv_state {
    bool was_carriage_return;
} sxml_csv_state;

static sxml_status sxml_serializer_put_csv_codepoint(sxml_serializer *serializer,
                                                     uint32_t codepoint, sxml_csv_state *state) {
    const bool was_carriage_return = state->was_carriage_return;
    state->was_carriage_return = codepoint == '\r';
    if (codepoint == '\r') {
        return sxml_serializer_put_ascii(serializer, serializer->line_ending_text);
    }
    if (codepoint == '\n') {
        return was_carriage_return
                   ? SXML_STATUS_OK
                   : sxml_serializer_put_ascii(serializer, serializer->line_ending_text);
    }
    if (!sxml_serializer_can_encode(serializer, codepoint)) {
        return sxml_serializer_raise_unencodable(serializer);
    }
    sxml_status status = SXML_STATUS_OK;
    if (codepoint == serializer->csv_quote_codepoint) {
        status = sxml_serializer_put_codepoint(serializer, codepoint);
    }
    return status == SXML_STATUS_OK ? sxml_serializer_put_codepoint(serializer, codepoint) : status;
}

/* Serialization 4.0, 5.1.2 and 7.3: characters the XML version or the HTML
   version does not allow even as references. */
static sxml_status sxml_serializer_check_allowed(sxml_serializer *serializer, uint32_t codepoint) {
    const bool is_c0_control = codepoint >= 0x01U && codepoint <= 0x1FU && codepoint != '\t' &&
                               codepoint != '\n' && codepoint != '\r';
    if (serializer->is_xml_version_1_0 && is_c0_control) {
        return sxml_serializer_raise_error(
            serializer,
            (sxml_serialization_error){
                .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0006"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "a control character is not allowed by XML 1.0, even as a reference")});
    }
    if (serializer->is_html_before_5 &&
        (is_c0_control || (codepoint >= 0x7FU && codepoint <= 0x9FU))) {
        return sxml_serializer_raise_error(
            serializer,
            (sxml_serialization_error){
                .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0014"),
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "a control character is not allowed by HTML before HTML5")});
    }
    return SXML_STATUS_OK;
}

/* Writes text escaped for its context and the escape style, with
   references for characters the encoding cannot represent; no character
   maps, no normalization. */
static sxml_status sxml_serializer_write_escaped(sxml_serializer *serializer,
                                                 const sxml_character_data *character_data) {
    const sxml_byte_view text = character_data->text;
    const sxml_escape_style style = serializer->escape_style;
    const bool is_canonical = style == SXML_ESCAPE_STYLE_CANONICAL;
    const bool is_unescaped_text =
        character_data->character_context == SXML_CHARACTER_CONTEXT_UNESCAPED_TEXT;
    const bool is_raw = style == SXML_ESCAPE_STYLE_NONE || is_unescaped_text ||
                        character_data->character_context == SXML_CHARACTER_CONTEXT_RAW_TEXT;
    const bool is_attribute =
        character_data->character_context == SXML_CHARACTER_CONTEXT_ATTRIBUTE ||
        character_data->character_context == SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE;
    const bool is_markup_style = !is_raw && style != SXML_ESCAPE_STYLE_JSON &&
                                 style != SXML_ESCAPE_STYLE_CSV;
    sxml_csv_state csv_state = {.was_carriage_return = false};
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const size_t run_start = offset;
        /* Copy a run of plain ASCII at once when the bytes are the
           output. */
        if (serializer->character_encoding != SXML_CHARACTER_ENCODING_UTF_16) {
            while (offset < text.byte_count) {
                const uint8_t byte = text.bytes[offset];
                if (byte < 0x20U || byte >= 0x7FU || byte == '&' || byte == '<' || byte == '>' ||
                    byte == '"' || byte == '\\' || byte == '/' ||
                    byte == serializer->csv_quote_codepoint) {
                    break;
                }
                offset += 1;
            }
            if (offset > run_start) {
                status = sxml_serializer_put_bytes(
                    serializer, (sxml_byte_view){.bytes = text.bytes + run_start,
                                                 .byte_count = offset - run_start});
                continue;
            }
        }
        const uint32_t codepoint = sxml_serializer_read_codepoint(text, &offset);
        if (style == SXML_ESCAPE_STYLE_JSON && !is_raw) {
            status = sxml_serializer_put_json_codepoint(serializer, codepoint);
            continue;
        }
        if (style == SXML_ESCAPE_STYLE_CSV) {
            status = sxml_serializer_put_csv_codepoint(serializer, codepoint, &csv_state);
            continue;
        }
        if (is_raw) {
            if (sxml_serializer_can_encode(serializer, codepoint)) {
                status = sxml_serializer_put_text_codepoint(serializer, codepoint);
            } else {
                status = is_unescaped_text
                             ? sxml_serializer_put_character_reference(serializer, codepoint)
                             : sxml_serializer_raise_unencodable(serializer);
            }
            continue;
        }
        sxml_byte_view escape = sxml_find_escape(
            (sxml_escape_query){.codepoint = codepoint,
                                .character_context = character_data->character_context});
        /* Serialization 4.0, 7.2: "<", and "&" before "{", stay in HTML
           attribute values. */
        if (style == SXML_ESCAPE_STYLE_HTML && is_attribute &&
            (codepoint == '<' ||
             (codepoint == '&' && offset < text.byte_count && text.bytes[offset] == '{'))) {
            escape = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
        if (is_markup_style && !is_canonical) {
            status = sxml_serializer_check_allowed(serializer, codepoint);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (escape.byte_count > 0) {
            status = sxml_serializer_put_ascii(serializer, escape);
        } else if (!is_canonical && (sxml_codepoint_needs_reference(codepoint) ||
                                     !sxml_serializer_can_encode(serializer, codepoint))) {
            status = sxml_serializer_put_character_reference(serializer, codepoint);
        } else {
            status = sxml_serializer_put_text_codepoint(serializer, codepoint);
        }
    }
    return status;
}

/* Writes the inside of the CDATA sections of one text node (Serialization
   4.0, 5.1.8): "]]>" is split between two sections; a character the
   encoding cannot represent, a carriage return or a control character
   closes the section and is written as a reference. */
static sxml_status sxml_serializer_write_cdata_sections(sxml_serializer *serializer,
                                                        sxml_byte_view text) {
    static const sxml_byte_view open = SXML_BYTE_VIEW_INITIALIZER("<![CDATA[");
    static const sxml_byte_view close = SXML_BYTE_VIEW_INITIALIZER("]]>");
    bool is_open = false;
    size_t bracket_count = 0;
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const uint32_t codepoint = sxml_serializer_read_codepoint(text, &offset);
        if (codepoint == '\r' || sxml_codepoint_needs_reference(codepoint) ||
            !sxml_serializer_can_encode(serializer, codepoint)) {
            if (is_open) {
                status = sxml_serializer_put_ascii(serializer, close);
                is_open = false;
            }
            bracket_count = 0;
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_put_character_reference(serializer, codepoint);
            }
            continue;
        }
        if (!is_open) {
            status = sxml_serializer_put_ascii(serializer, open);
            is_open = true;
        } else if (codepoint == '>' && bracket_count >= 2) {
            status = sxml_serializer_put_ascii(serializer, close);
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_put_ascii(serializer, open);
            }
        }
        bracket_count = codepoint == ']' ? bracket_count + 1 : 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_put_text_codepoint(serializer, codepoint);
        }
    }
    if (status == SXML_STATUS_OK && is_open) {
        status = sxml_serializer_put_ascii(serializer, close);
    }
    return status;
}

/* ------------------------------------------- Mapping and normalization */

/* The replacement of codepoint in the character map, or NULL. */
static const sxml_character_mapping *sxml_serializer_find_mapping(
    const sxml_serializer *serializer, uint32_t codepoint) {
    size_t low = 0;
    size_t high = serializer->mapping_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const uint32_t candidate = serializer->sorted_mappings[middle].codepoint;
        if (candidate == codepoint) {
            return &serializer->sorted_mappings[middle];
        }
        if (candidate < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* A part of a text without mapped characters, and whether it starts the
   text (for the fully-normalized check). */
typedef struct sxml_unmapped_segment {
    sxml_byte_view text;
    bool is_start_of_construct;
} sxml_unmapped_segment;

/* Normalizes and escapes one unmapped segment (Serialization 4.0,
   section 4, rules d and e; 5.1.12). */
static sxml_status sxml_serializer_write_segment(sxml_serializer *serializer,
                                                 sxml_character_context character_context,
                                                 sxml_unmapped_segment segment) {
    sxml_byte_view text = segment.text;
    sxml_unicode_normalized_text normalized = {.allocator = serializer->allocator,
                                               .bytes = NULL,
                                               .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    bool is_ascii = true;
    for (size_t offset = 0; is_ascii && offset < text.byte_count; offset += 1) {
        is_ascii = text.bytes[offset] < 0x80U;
    }
    if (serializer->is_normalizing && !is_ascii) {
        sxml_string input;
        status = sxml_string_from_utf8(text, &input);
        if (status == SXML_STATUS_OK) {
            status = sxml_unicode_normalize(
                &(sxml_unicode_normalization_request){
                    .allocator = serializer->allocator,
                    .normalization_form = serializer->normalization_form,
                    .input = input},
                &normalized);
        }
        if (status == SXML_STATUS_OK) {
            text = sxml_unicode_normalized_text_view(&normalized);
        }
    }
    if (status == SXML_STATUS_OK && serializer->is_fully_normalized &&
        segment.is_start_of_construct && text.byte_count > 0) {
        size_t offset = 0;
        if (sxml_unicode_is_composing_character(sxml_serializer_read_codepoint(text, &offset))) {
            status = sxml_serializer_raise_error(
                serializer,
                (sxml_serialization_error){
                    .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SERE0012"),
                    .description = SXML_BYTE_VIEW_FROM_LITERAL(
                        "fully-normalized output would start a construct with a composing "
                        "character")});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = character_context == SXML_CHARACTER_CONTEXT_CDATA_SECTION
                     ? sxml_serializer_write_cdata_sections(serializer, text)
                     : sxml_serializer_write_escaped(
                           serializer, &(sxml_character_data){.text = text,
                                                              .character_context =
                                                                  character_context});
    }
    sxml_unicode_normalized_text_release(&normalized);
    return status;
}

sxml_status sxml_serializer_write_character_data(sxml_serializer *serializer,
                                                 const sxml_character_data *character_data) {
    const sxml_byte_view text = character_data->text;
    const sxml_character_context character_context = character_data->character_context;
    switch (character_context) {
    case SXML_CHARACTER_CONTEXT_MARKUP:
        return sxml_serializer_write_markup(serializer, text);
    case SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE:
        return sxml_serializer_write_escaped(serializer, character_data);
    case SXML_CHARACTER_CONTEXT_CDATA_SECTION:
        /* Character maps do not apply in CDATA sections (section 12). */
        return sxml_serializer_write_segment(
            serializer, character_context,
            (sxml_unmapped_segment){.text = text, .is_start_of_construct = true});
    case SXML_CHARACTER_CONTEXT_TEXT:
    case SXML_CHARACTER_CONTEXT_ATTRIBUTE:
    case SXML_CHARACTER_CONTEXT_RAW_TEXT:
    case SXML_CHARACTER_CONTEXT_UNESCAPED_TEXT:
        break;
    }
    if (!serializer->applies_character_maps || serializer->mapping_count == 0) {
        return sxml_serializer_write_segment(
            serializer, character_context,
            (sxml_unmapped_segment){.text = text, .is_start_of_construct = true});
    }
    /* Section 12: a mapped character is replaced before anything else and
       its replacement is written as it is; the rest is normalized and
       escaped. */
    sxml_status status = SXML_STATUS_OK;
    size_t segment_start = 0;
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const size_t codepoint_start = offset;
        const sxml_character_mapping *mapping =
            sxml_serializer_find_mapping(serializer, sxml_serializer_read_codepoint(text, &offset));
        if (mapping == NULL) {
            continue;
        }
        if (codepoint_start > segment_start) {
            status = sxml_serializer_write_segment(
                serializer, character_context,
                (sxml_unmapped_segment){
                    .text = {.bytes = text.bytes + segment_start,
                             .byte_count = codepoint_start - segment_start},
                    .is_start_of_construct = segment_start == 0});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write_markup(serializer, mapping->replacement_text);
        }
        segment_start = offset;
    }
    if (status == SXML_STATUS_OK && segment_start < text.byte_count) {
        status = sxml_serializer_write_segment(
            serializer, character_context,
            (sxml_unmapped_segment){.text = {.bytes = text.bytes + segment_start,
                                             .byte_count = text.byte_count - segment_start},
                                    .is_start_of_construct = segment_start == 0});
    }
    return status;
}

/* ------------------------------------------------------ Parameter checks */

/* Whether view can be read: bytes present when byte_count is not 0, and
   well-formed UTF-8. */
static bool sxml_text_is_valid(sxml_byte_view text) {
    if (text.byte_count == 0) {
        return true;
    }
    sxml_utf8_validation validation;
    return text.bytes != NULL && sxml_utf8_validate(text, &validation) == SXML_STATUS_OK;
}

static bool sxml_flag_is_valid(sxml_serialization_flag flag) {
    return flag == SXML_SERIALIZATION_FLAG_DEFAULT || flag == SXML_SERIALIZATION_FLAG_NO ||
           flag == SXML_SERIALIZATION_FLAG_YES;
}

static bool sxml_element_names_are_valid(sxml_element_name_view names) {
    if (names.element_name_count > 0 && names.element_names == NULL) {
        return false;
    }
    for (size_t name_index = 0; name_index < names.element_name_count; name_index += 1) {
        if (names.element_names[name_index].index == 0) {
            return false;
        }
    }
    return true;
}

/* XML 1.1, production Char: the repertoire the serializer writes (D120b,
   D128; QT4 serialize-xml-167 maps U+0001). */
static bool sxml_codepoint_is_xml_character(uint32_t codepoint) {
    return (codepoint >= 0x1U && codepoint <= 0xD7FFU) ||
           (codepoint >= 0xE000U && codepoint <= 0xFFFDU) ||
           (codepoint >= 0x10000U && codepoint <= 0x10FFFFU);
}

/* What C can express but XDM cannot (include/sxml/serialize.h): an
   invalid argument. */
static bool sxml_parameters_are_expressible(const sxml_serialization_parameters *parameters) {
    const sxml_serialization_flag flags[] = {
        parameters->allow_duplicate_names, parameters->byte_order_mark,
        parameters->canonical,             parameters->csv_header,
        parameters->escape_solidus,        parameters->escape_uri_attributes,
        parameters->include_content_type,  parameters->indent,
        parameters->indent_attributes,     parameters->json_lines,
        parameters->omit_xml_declaration,  parameters->undeclare_prefixes,
    };
    bool is_valid = parameters->serialization_method <= SXML_SERIALIZATION_METHOD_CSV &&
                    parameters->character_encoding <= SXML_CHARACTER_ENCODING_ISO_8859_1 &&
                    parameters->json_node_output_method <= SXML_JSON_NODE_OUTPUT_METHOD_TEXT &&
                    parameters->line_ending <= SXML_LINE_ENDING_CARRIAGE_RETURN &&
                    parameters->normalization_form <= SXML_NORMALIZATION_FORM_FULLY_NORMALIZED &&
                    parameters->standalone <= SXML_STANDALONE_NO;
    for (size_t flag_index = 0; is_valid && flag_index < sizeof(flags) / sizeof(flags[0]);
         flag_index += 1) {
        is_valid = sxml_flag_is_valid(flags[flag_index]);
    }
    const sxml_byte_view texts[] = {
        parameters->doctype_public, parameters->doctype_system, parameters->html_version,
        parameters->indent_unit,    parameters->item_separator, parameters->media_type,
        parameters->version,
    };
    const bool presences[] = {
        parameters->has_doctype_public, parameters->has_doctype_system,
        parameters->has_html_version,   parameters->has_indent_unit,
        parameters->has_item_separator, parameters->has_media_type,
        parameters->has_version,
    };
    for (size_t text_index = 0; is_valid && text_index < sizeof(texts) / sizeof(texts[0]);
         text_index += 1) {
        is_valid = !presences[text_index] || sxml_text_is_valid(texts[text_index]);
    }
    is_valid = is_valid && sxml_element_names_are_valid(parameters->cdata_section_elements) &&
               sxml_element_names_are_valid(parameters->suppress_indentation);
    const sxml_character_map_view maps = parameters->use_character_maps;
    if (is_valid && maps.character_mapping_count > 0 && maps.character_mappings == NULL) {
        is_valid = false;
    }
    for (size_t mapping_index = 0; is_valid && mapping_index < maps.character_mapping_count;
         mapping_index += 1) {
        const sxml_character_mapping mapping = maps.character_mappings[mapping_index];
        is_valid = sxml_codepoint_is_xml_character(mapping.codepoint) &&
                   sxml_text_is_valid(mapping.replacement_text);
    }
    return is_valid;
}

/* XML 1.0, production PubidChar. */
static bool sxml_byte_is_public_identifier_character(uint8_t byte) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
        return true;
    }
    static const char others[] = " \r\n-'()+,./:=?;!*#@$_%";
    for (size_t other_index = 0; other_index + 1 < sizeof(others); other_index += 1) {
        if ((uint8_t)others[other_index] == byte) {
            return true;
        }
    }
    return false;
}

/* The lexical form of an xs:decimal (XSD 1.1, 3.3.3): an optional sign,
   digits with at most one point, at least one digit. */
static bool sxml_text_is_decimal(sxml_byte_view text) {
    size_t offset = 0;
    if (offset < text.byte_count && (text.bytes[offset] == '+' || text.bytes[offset] == '-')) {
        offset += 1;
    }
    size_t digit_count = 0;
    bool has_point = false;
    for (; offset < text.byte_count; offset += 1) {
        const uint8_t byte = text.bytes[offset];
        if (byte >= '0' && byte <= '9') {
            digit_count += 1;
        } else if (byte == '.' && !has_point) {
            has_point = true;
        } else {
            return false;
        }
    }
    return digit_count > 0;
}

/* The permitted values of Serialization 4.0, section 3, that apply
   whatever the method: SEPM0016 otherwise. */
static sxml_status sxml_serializer_check_permitted_values(sxml_serializer *serializer) {
    const sxml_serialization_parameters *parameters = serializer->parameters;
    const char *problem = NULL;
    if (parameters->has_indent_unit) {
        /* (\t|\\t)*| * after the reader's turning "\t" into a tab. */
        const sxml_byte_view unit = parameters->indent_unit;
        for (size_t offset = 0; problem == NULL && offset < unit.byte_count; offset += 1) {
            if ((unit.bytes[offset] != ' ' && unit.bytes[offset] != '\t') ||
                unit.bytes[offset] != unit.bytes[0]) {
                problem = "indent-unit must be spaces only or tabs only";
            }
        }
    }
    if (problem == NULL && parameters->has_doctype_system) {
        bool has_quotation_mark = false;
        bool has_apostrophe = false;
        for (size_t offset = 0; offset < parameters->doctype_system.byte_count; offset += 1) {
            has_quotation_mark = has_quotation_mark || parameters->doctype_system.bytes[offset] == '"';
            has_apostrophe = has_apostrophe || parameters->doctype_system.bytes[offset] == '\'';
        }
        if (has_quotation_mark && has_apostrophe) {
            problem = "doctype-system must not hold both quotation marks and apostrophes";
        }
    }
    if (problem == NULL && parameters->has_doctype_public) {
        for (size_t offset = 0; problem == NULL && offset < parameters->doctype_public.byte_count;
             offset += 1) {
            if (!sxml_byte_is_public_identifier_character(parameters->doctype_public.bytes[offset])) {
                problem = "doctype-public must consist of PubidChar characters";
            }
        }
    }
    if (problem == NULL && parameters->has_html_version &&
        !sxml_text_is_decimal(parameters->html_version)) {
        problem = "html-version must be a decimal";
    }
    /* Serialization 4.0, 10.3.1 and 10.3.2. */
    const uint32_t separator = parameters->csv_separator_codepoint == 0
                                   ? (uint32_t)','
                                   : parameters->csv_separator_codepoint;
    const uint32_t quote = parameters->csv_quote_character_codepoint == 0
                               ? (uint32_t)'"'
                               : parameters->csv_quote_character_codepoint;
    if (problem == NULL && (separator == quote || separator == '\n' || separator == '\r' ||
                            quote == '\n' || quote == '\r')) {
        problem = "csv-separator and csv-quote-character must differ and must not be line ends";
    }
    if (problem == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_serializer_raise_error(
        serializer, (sxml_serialization_error){
                        .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SEPM0016"),
                        .description = sxml_byte_view_from_c_string(problem)});
}

/* Sorts the character map by codepoint into heap memory (heapsort, so
   the time stays O(n log n) without recursion) and refuses a codepoint
   given twice. */
static sxml_status sxml_serializer_sort_character_map(sxml_serializer *serializer) {
    const sxml_character_map_view maps = serializer->parameters->use_character_maps;
    if (maps.character_mapping_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(maps.character_mapping_count,
                                            sizeof(sxml_character_mapping), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            serializer->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_character_mapping)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_character_mapping *mappings = memory;
    memcpy(mappings, maps.character_mappings, byte_count);
    const size_t count = maps.character_mapping_count;
    serializer->sorted_mappings = mappings;
    serializer->mapping_count = count;
    /* Build a max-heap, then move the largest to the end, count times. */
    for (size_t sort_pass = 0; sort_pass < 2 * count; sort_pass += 1) {
        const bool is_building = sort_pass < count;
        const size_t heap_size = is_building ? count : 2 * count - sort_pass;
        size_t root = 0;
        if (is_building) {
            root = count - 1 - sort_pass;
        } else {
            const sxml_character_mapping largest = mappings[0];
            mappings[0] = mappings[heap_size - 1];
            mappings[heap_size - 1] = largest;
        }
        const size_t sift_size = is_building ? heap_size : heap_size - 1;
        for (;;) {
            size_t largest_index = root;
            const size_t left = 2 * root + 1;
            const size_t right = left + 1;
            if (left < sift_size && mappings[left].codepoint > mappings[largest_index].codepoint) {
                largest_index = left;
            }
            if (right < sift_size &&
                mappings[right].codepoint > mappings[largest_index].codepoint) {
                largest_index = right;
            }
            if (largest_index == root) {
                break;
            }
            const sxml_character_mapping swapped = mappings[root];
            mappings[root] = mappings[largest_index];
            mappings[largest_index] = swapped;
            root = largest_index;
        }
    }
    for (size_t mapping_index = 1; mapping_index < count; mapping_index += 1) {
        if (mappings[mapping_index].codepoint == mappings[mapping_index - 1].codepoint) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------ Sequence normalization */

typedef enum sxml_normalizer_frame_kind {
    /* Items of a sequence: the input, a JNode's value, an array member. */
    SXML_NORMALIZER_FRAME_KIND_SEQUENCE,
    /* Members of an array, flattened (array:flatten). */
    SXML_NORMALIZER_FRAME_KIND_ARRAY,
} sxml_normalizer_frame_kind;

struct sxml_normalizer_frame {
    sxml_normalizer_frame_kind frame_kind;
    sxml_sequence sequence;
    sxml_item array;
    uint64_t next_index;
    uint64_t count;
};

void sxml_sequence_normalizer_start(sxml_serializer *serializer, sxml_sequence value,
                                    sxml_sequence_normalizer *normalizer) {
    /* The first frame is pushed by the first call to next, which can
       fail; leaf_count UINT64_MAX marks that it has not been. */
    *normalizer = (sxml_sequence_normalizer){
        .serializer = serializer,
        .value = value,
        .frames = NULL,
        .frame_count = 0,
        .frame_capacity = 0,
        .leaf_count = UINT64_MAX,
        .was_previous_leaf_atomic = false,
        .is_in_document = false,
        .document_tree = NULL,
        .has_next_child = false,
        .next_child = {.index = 0},
        .has_pending_node = false,
        .pending_node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0},
        .text_bytes = NULL,
        .text_byte_count = 0,
        .text_byte_capacity = 0,
        .has_given_text = false,
    };
}

void sxml_sequence_normalizer_finish(sxml_sequence_normalizer *normalizer) {
    sxml_allocator_release(normalizer->serializer->allocator, normalizer->frames);
    sxml_allocator_release(normalizer->serializer->allocator, normalizer->text_bytes);
    normalizer->frames = NULL;
    normalizer->text_bytes = NULL;
}

static sxml_status sxml_sequence_normalizer_push(sxml_sequence_normalizer *normalizer,
                                                 const sxml_normalizer_frame *frame) {
    const sxml_status status = sxml_serializer_grow(
        normalizer->serializer->allocator,
        &(sxml_serializer_growth){.elements = (void **)&normalizer->frames,
                                  .element_capacity = &normalizer->frame_capacity,
                                  .element_count = normalizer->frame_count,
                                  .element_byte_count = sizeof(sxml_normalizer_frame),
                                  .alignment = alignof(sxml_normalizer_frame)});
    if (status == SXML_STATUS_OK) {
        normalizer->frames[normalizer->frame_count] = *frame;
        normalizer->frame_count += 1;
    }
    return status;
}

static sxml_status sxml_sequence_normalizer_push_sequence(sxml_sequence_normalizer *normalizer,
                                                          sxml_sequence sequence) {
    uint64_t item_count = 0;
    const sxml_status status =
        sxml_sequence_count(sequence, normalizer->serializer->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_sequence_normalizer_push(
        normalizer, &(sxml_normalizer_frame){.frame_kind = SXML_NORMALIZER_FRAME_KIND_SEQUENCE,
                                             .sequence = sequence,
                                             .array = {.representation = 0},
                                             .next_index = 0,
                                             .count = item_count});
}

/* Appends text to the text being merged. */
static sxml_status sxml_sequence_normalizer_append_text(sxml_sequence_normalizer *normalizer,
                                                        sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t required = 0;
    sxml_status status = sxml_size_add(normalizer->text_byte_count, text.byte_count, &required);
    if (status == SXML_STATUS_OK && required > normalizer->text_byte_capacity) {
        size_t capacity = normalizer->text_byte_capacity < 64 ? 64 : normalizer->text_byte_capacity;
        while (status == SXML_STATUS_OK && capacity < required) {
            status = sxml_size_multiply(capacity, 2, &capacity);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                normalizer->serializer->allocator,
                (sxml_allocation_request){.byte_count = capacity, .alignment = 1}, &memory);
        }
        if (status == SXML_STATUS_OK) {
            if (normalizer->text_byte_count > 0) {
                memcpy(memory, normalizer->text_bytes, normalizer->text_byte_count);
            }
            sxml_allocator_release(normalizer->serializer->allocator, normalizer->text_bytes);
            normalizer->text_bytes = memory;
            normalizer->text_byte_capacity = capacity;
        }
    }
    if (status == SXML_STATUS_OK) {
        memcpy(normalizer->text_bytes + normalizer->text_byte_count, text.bytes, text.byte_count);
        normalizer->text_byte_count = required;
    }
    return status;
}

static sxml_status sxml_sequence_normalizer_raise_unserializable(
    sxml_sequence_normalizer *normalizer) {
    return sxml_serializer_raise_error(
        normalizer->serializer,
        (sxml_serialization_error){
            .error_local_name = SXML_BYTE_VIEW_FROM_LITERAL("SENR0001"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "an attribute node, a namespace node or a function item cannot be serialized "
                "by this method")});
}

sxml_status sxml_serializer_get_atomic_text(sxml_serializer *serializer, sxml_item item,
                                            sxml_atomic_text *atomic_text) {
    _Static_assert(SXML_ATOMIC_TEXT_INLINE_CAPACITY == SXML_CAST_CANONICAL_TEXT_CAPACITY,
                   "the inline text holds a canonical text");
    atomic_text->text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    atomic_text->memory = NULL;
    sxml_canonical_text canonical;
    sxml_status status = sxml_cast_write_canonical_text(
        serializer->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = atomic_text->inline_bytes,
                       .byte_count = sizeof(atomic_text->inline_bytes)}},
        &canonical);
    if (status == SXML_STATUS_CAPACITY_EXCEEDED) {
        const size_t byte_count = canonical.required_byte_count;
        status = sxml_allocator_allocate(
            serializer->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
            &atomic_text->memory);
        if (status == SXML_STATUS_OK) {
            status = sxml_cast_write_canonical_text(
                serializer->item_heap,
                &(sxml_canonical_text_request){
                    .item = item, .output = {.bytes = atomic_text->memory, .byte_count = byte_count}},
                &canonical);
        }
    }
    if (status == SXML_STATUS_OK) {
        atomic_text->text = canonical.text;
    }
    return status;
}

void sxml_serializer_release_atomic_text(sxml_serializer *serializer,
                                         sxml_atomic_text *atomic_text) {
    sxml_allocator_release(serializer->allocator, atomic_text->memory);
    atomic_text->memory = NULL;
}

/* Appends the lexical form of an atomic item (its cast to xs:string). */
static sxml_status sxml_sequence_normalizer_append_atomic(sxml_sequence_normalizer *normalizer,
                                                          sxml_item item) {
    sxml_atomic_text atomic_text;
    sxml_status status =
        sxml_serializer_get_atomic_text(normalizer->serializer, item, &atomic_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_normalizer_append_text(normalizer, atomic_text.text);
    }
    sxml_serializer_release_atomic_text(normalizer->serializer, &atomic_text);
    return status;
}

/* Takes one item of S3 (the input with JNodes and arrays expanded) into
   the result tree: sets the pending node, enters a document, or appends
   text. */
static sxml_status sxml_sequence_normalizer_take_leaf(sxml_sequence_normalizer *normalizer,
                                                      sxml_item item) {
    const sxml_serialization_parameters *parameters = normalizer->serializer->parameters;
    const bool is_atomic = sxml_item_get_kind(item) == SXML_ITEM_KIND_ATOMIC;
    const bool is_first = normalizer->leaf_count == 0;
    normalizer->leaf_count += 1;
    sxml_status status = SXML_STATUS_OK;
    /* Step S4: the item separator between every two items, or a space
       between adjacent strings. */
    if (!is_first && parameters->has_item_separator) {
        status = sxml_sequence_normalizer_append_text(normalizer, parameters->item_separator);
    } else if (!is_first && is_atomic && normalizer->was_previous_leaf_atomic) {
        status = sxml_sequence_normalizer_append_text(normalizer, SXML_BYTE_VIEW_FROM_LITERAL(" "));
    }
    normalizer->was_previous_leaf_atomic = is_atomic;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_atomic) {
        return sxml_sequence_normalizer_append_atomic(normalizer, item);
    }
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_NODE) {
        return sxml_sequence_normalizer_raise_unserializable(normalizer);
    }
    sxml_node_reference node;
    sxml_node_description description;
    status = sxml_item_get_node(item, &node);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (description.node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
        /* Step S6: a document is replaced by its children. */
        normalizer->is_in_document = description.has_children;
        normalizer->document_tree = node.tree;
        normalizer->has_next_child = description.has_children;
        normalizer->next_child = description.first_child;
        return SXML_STATUS_OK;
    case SXML_NODE_KIND_TEXT: {
        if (description.disables_output_escaping) {
            /* Not merged: the method writes it unescaped (D188). */
            normalizer->has_pending_node = true;
            normalizer->pending_node = node;
            return SXML_STATUS_OK;
        }
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(node, &content);
        return status == SXML_STATUS_OK ? sxml_sequence_normalizer_append_text(normalizer, content)
                                        : status;
    }
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        normalizer->has_pending_node = true;
        normalizer->pending_node = node;
        return SXML_STATUS_OK;
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        break;
    }
    return sxml_sequence_normalizer_raise_unserializable(normalizer);
}

/* Takes the next child of the document being unwrapped. */
static sxml_status sxml_sequence_normalizer_take_child(sxml_sequence_normalizer *normalizer) {
    const sxml_node_reference child = {.tree = normalizer->document_tree,
                                       .node_index = normalizer->next_child,
                                       .namespace_number = 0};
    sxml_node_description description;
    sxml_status status = sxml_node_describe(child, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    normalizer->has_next_child = description.has_next_sibling;
    normalizer->next_child = description.next_sibling;
    normalizer->is_in_document = normalizer->has_next_child;
    if (description.node_kind == SXML_NODE_KIND_TEXT && !description.disables_output_escaping) {
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(child, &content);
        return status == SXML_STATUS_OK ? sxml_sequence_normalizer_append_text(normalizer, content)
                                        : status;
    }
    normalizer->has_pending_node = true;
    normalizer->pending_node = child;
    return SXML_STATUS_OK;
}

/* Advances the frames by one item. Sets *is_at_end when none is left. */
static sxml_status sxml_sequence_normalizer_advance(sxml_sequence_normalizer *normalizer,
                                                    sxml_answer *is_at_end_answer) {
    sxml_normalizer_frame *frame = &normalizer->frames[normalizer->frame_count - 1];
    if (frame->next_index == frame->count) {
        normalizer->frame_count -= 1;
        is_at_end_answer->is_true = normalizer->frame_count == 0;
        return SXML_STATUS_OK;
    }
    const uint64_t index = frame->next_index;
    frame->next_index += 1;
    sxml_status status = SXML_STATUS_OK;
    if (frame->frame_kind == SXML_NORMALIZER_FRAME_KIND_ARRAY) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(frame->array, (size_t)index, &member);
        return status == SXML_STATUS_OK ? sxml_sequence_normalizer_push_sequence(normalizer, member)
                                        : status;
    }
    sxml_item item = {.representation = 0};
    status = sxml_sequence_get_item(
        frame->sequence,
        (sxml_item_request){.item_index = index,
                            .error_record = normalizer->serializer->error_record},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ARRAY: {
        /* Step S2: arrays are flattened. */
        size_t member_count = 0;
        status = sxml_array_get_size(item, &member_count);
        return status == SXML_STATUS_OK
                   ? sxml_sequence_normalizer_push(
                         normalizer,
                         &(sxml_normalizer_frame){.frame_kind = SXML_NORMALIZER_FRAME_KIND_ARRAY,
                                                  .sequence = sxml_sequence_empty(),
                                                  .array = item,
                                                  .next_index = 0,
                                                  .count = member_count})
                   : status;
    }
    case SXML_ITEM_KIND_JSON_NODE: {
        /* Step S1: a JNode is replaced by its value. */
        sxml_json_node_description description;
        status = sxml_json_node_describe(item, &description);
        return status == SXML_STATUS_OK
                   ? sxml_sequence_normalizer_push_sequence(normalizer, description.json_value)
                   : status;
    }
    case SXML_ITEM_KIND_ATOMIC:
    case SXML_ITEM_KIND_NODE:
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_MAP:
        break;
    }
    return sxml_sequence_normalizer_take_leaf(normalizer, item);
}

sxml_status sxml_sequence_normalizer_next(sxml_sequence_normalizer *normalizer,
                                          sxml_normalized_event *event) {
    sxml_status status = SXML_STATUS_OK;
    if (normalizer->has_given_text) {
        normalizer->text_byte_count = 0;
        normalizer->has_given_text = false;
    }
    if (normalizer->leaf_count == UINT64_MAX) {
        normalizer->leaf_count = 0;
        status = sxml_sequence_normalizer_push_sequence(normalizer, normalizer->value);
    }
    sxml_answer is_at_end = {.is_true = normalizer->frame_count == 0};
    while (status == SXML_STATUS_OK && !normalizer->has_pending_node && !is_at_end.is_true) {
        status = normalizer->is_in_document
                     ? sxml_sequence_normalizer_take_child(normalizer)
                     : sxml_sequence_normalizer_advance(normalizer, &is_at_end);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Step S7: merged text comes before the node that ended it. */
    if (normalizer->text_byte_count > 0) {
        normalizer->has_given_text = true;
        *event = (sxml_normalized_event){
            .event_kind = SXML_NORMALIZED_EVENT_KIND_TEXT,
            .text = {.bytes = normalizer->text_bytes, .byte_count = normalizer->text_byte_count},
            .node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0}};
        return SXML_STATUS_OK;
    }
    if (normalizer->has_pending_node) {
        normalizer->has_pending_node = false;
        *event = (sxml_normalized_event){.event_kind = SXML_NORMALIZED_EVENT_KIND_NODE,
                                         .text = {.bytes = NULL, .byte_count = 0},
                                         .node = normalizer->pending_node};
        return SXML_STATUS_OK;
    }
    *event = (sxml_normalized_event){.event_kind = SXML_NORMALIZED_EVENT_KIND_END,
                                     .text = {.bytes = NULL, .byte_count = 0},
                                     .node = {.tree = NULL, .node_index = {.index = 0},
                                              .namespace_number = 0}};
    return SXML_STATUS_OK;
}

/* --------------------------------------------------- The adaptive method */

typedef enum sxml_serialization_frame_kind {
    /* The sequence given to the serializer: items with the item separator
       between them. */
    SXML_SERIALIZATION_FRAME_KIND_TOP_SEQUENCE,
    /* A member of an array, the value of a map entry or a JNode's value:
       items separated by commas, in parentheses unless there is one. */
    SXML_SERIALIZATION_FRAME_KIND_MEMBER_SEQUENCE,
    SXML_SERIALIZATION_FRAME_KIND_ARRAY,
    SXML_SERIALIZATION_FRAME_KIND_MAP,
    /* Writes the ")" after a JNode's value. */
    SXML_SERIALIZATION_FRAME_KIND_JSON_NODE_END,
} sxml_serialization_frame_kind;

/* A value being written (glossary `serialization_frame`). Values are
   borrowed from the frame below, and the bottom one from the request. */
struct sxml_serialization_frame {
    sxml_serialization_frame_kind frame_kind;
    sxml_sequence sequence;
    sxml_item container;
    uint64_t next_index;
    uint64_t count;
    bool is_parenthesized;
};

/* The adaptive method's text goes through the pipeline as markup: encoded,
   with line endings, never escaped (Serialization 4.0, 11.1). */
static sxml_status sxml_serializer_write(sxml_serializer *serializer, sxml_byte_view bytes) {
    return sxml_serializer_write_markup(serializer, bytes);
}

/* Writes text with every byte equal to quote written twice (the
   doubling of quotes in a string literal). */
static sxml_status sxml_serializer_write_doubling(sxml_serializer *serializer,
                                                  sxml_byte_view text, uint8_t quote) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t run_start = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t offset = 0; status == SXML_STATUS_OK && offset < text.byte_count;
         offset += 1) {
        if (text.bytes[offset] == quote) {
            status = sxml_serializer_write(
                serializer, (sxml_byte_view){.bytes = text.bytes + run_start,
                                             .byte_count = offset + 1 - run_start});
            run_start = offset;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(
            serializer, (sxml_byte_view){.bytes = text.bytes + run_start,
                                         .byte_count = text.byte_count - run_start});
    }
    return status;
}


/* --------------------------------------------------------- Atomic items */

/* An expanded name to write. */
typedef struct sxml_written_name {
    sxml_name_identifier name;
    /* For xs:QName and function names: use the conventional prefixes of
       Serialization 4.0, section 11 (array, fn, map, math, xs), else
       Q{uri}local; no namespace gives the local name. For xs:NOTATION:
       always Q{uri}local. */
    bool uses_conventional_prefix;
} sxml_written_name;

static sxml_status sxml_serializer_write_name(sxml_serializer *serializer,
                                              sxml_written_name written_name) {
    sxml_name_parts parts;
    sxml_status status =
        sxml_name_pool_get_name_parts(serializer->name_pool, written_name.name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    static const struct {
        uint32_t namespace_index;
        sxml_byte_view prefix;
    } conventional_prefixes[] = {
        {SXML_NAMESPACE_ARRAY_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("array:")},
        {SXML_NAMESPACE_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("fn:")},
        {SXML_NAMESPACE_MAP_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("map:")},
        {SXML_NAMESPACE_MATH_FUNCTIONS, SXML_BYTE_VIEW_INITIALIZER("math:")},
        {SXML_NAMESPACE_XML_SCHEMA, SXML_BYTE_VIEW_INITIALIZER("xs:")},
    };
    if (written_name.uses_conventional_prefix) {
        if (parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
            return sxml_serializer_write(serializer, parts.local_name);
        }
        for (size_t prefix_index = 0;
             prefix_index < sizeof(conventional_prefixes) / sizeof(conventional_prefixes[0]);
             prefix_index += 1) {
            if (conventional_prefixes[prefix_index].namespace_index ==
                parts.namespace_uri.index) {
                status = sxml_serializer_write(serializer,
                                               conventional_prefixes[prefix_index].prefix);
                return status == SXML_STATUS_OK
                           ? sxml_serializer_write(serializer, parts.local_name)
                           : status;
            }
        }
    }
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    status = sxml_name_pool_get_namespace_uri(serializer->name_pool, parts.namespace_uri,
                                              &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("}"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, parts.local_name);
    }
    return status;
}

/*
 * Writes the canonical text of an xs:double or xs:float as a mantissa
 * with one digit before the point and an exponent ("1.0e0", "1.2345e-7"),
 * so the type stays visible (Serialization 4.0, section 11, allows an
 * implementation-defined representation).
 */
static sxml_status sxml_serializer_write_exponent_form(sxml_serializer *serializer,
                                                       sxml_byte_view canonical) {
    uint8_t digits[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    size_t digit_count = 0;
    int64_t point_position = 0;
    bool has_point = false;
    bool is_negative = false;
    int64_t exponent = 0;
    bool is_exponent_negative = false;
    size_t offset = 0;
    if (offset < canonical.byte_count && canonical.bytes[offset] == '-') {
        is_negative = true;
        offset += 1;
    }
    for (; offset < canonical.byte_count; offset += 1) {
        const uint8_t byte = canonical.bytes[offset];
        if (byte == '.') {
            has_point = true;
        } else if (byte == 'E' || byte == 'e') {
            offset += 1;
            break;
        } else if (digit_count < sizeof(digits)) {
            digits[digit_count] = byte;
            digit_count += 1;
            point_position += has_point ? 0 : 1;
        }
    }
    if (offset < canonical.byte_count && canonical.bytes[offset] == '-') {
        is_exponent_negative = true;
        offset += 1;
    }
    for (; offset < canonical.byte_count; offset += 1) {
        exponent = exponent * 10 + (canonical.bytes[offset] - '0');
    }
    point_position += is_exponent_negative ? -exponent : exponent;
    size_t first_digit = 0;
    while (first_digit < digit_count && digits[first_digit] == '0') {
        first_digit += 1;
        point_position -= 1;
    }
    while (digit_count > first_digit && digits[digit_count - 1] == '0') {
        digit_count -= 1;
    }
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY + 32];
    size_t output_count = 0;
    if (is_negative) {
        output[output_count++] = '-';
    }
    if (first_digit == digit_count) {
        static const uint8_t zero_form[] = {'0', '.', '0', 'e', '0'};
        memcpy(output + output_count, zero_form, sizeof(zero_form));
        output_count += sizeof(zero_form);
        return sxml_serializer_write(serializer,
                                     (sxml_byte_view){.bytes = output, .byte_count = output_count});
    }
    output[output_count++] = digits[first_digit];
    output[output_count++] = '.';
    if (digit_count - first_digit == 1) {
        output[output_count++] = '0';
    } else {
        memcpy(output + output_count, digits + first_digit + 1, digit_count - first_digit - 1);
        output_count += digit_count - first_digit - 1;
    }
    output[output_count++] = 'e';
    int64_t written_exponent = point_position - 1;
    if (written_exponent < 0) {
        output[output_count++] = '-';
        written_exponent = -written_exponent;
    }
    uint8_t reversed[24];
    size_t reversed_count = 0;
    do {
        reversed[reversed_count++] = (uint8_t)('0' + written_exponent % 10);
        written_exponent /= 10;
    } while (written_exponent != 0);
    while (reversed_count > 0) {
        output[output_count++] = reversed[--reversed_count];
    }
    return sxml_serializer_write(serializer,
                                 (sxml_byte_view){.bytes = output, .byte_count = output_count});
}

/* Writes an atomic item (Serialization 4.0, section 11). */
static sxml_status sxml_serializer_write_atomic(sxml_serializer *serializer, sxml_item item) {
    const sxml_type_identifier primitive_type = sxml_atomic_item_get_primitive_type(item);
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (primitive_type.index) {
    case SXML_TYPE_XS_BOOLEAN:
        return sxml_serializer_write(serializer, datum.datum_details.boolean ? SXML_BYTE_VIEW_FROM_LITERAL("true()") : SXML_BYTE_VIEW_FROM_LITERAL("false()"));
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
    case SXML_TYPE_XS_ANY_URI:
        /* A quotation mark is the quote character; quotes in the value are
           doubled. */
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write_doubling(serializer, datum.datum_details.string.utf8,
                                                    '"');
        }
        return status == SXML_STATUS_OK ? sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\""))
                                        : status;
    case SXML_TYPE_XS_QNAME:
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("#"));
        return status == SXML_STATUS_OK
                   ? sxml_serializer_write_name(
                         serializer,
                         (sxml_written_name){.name = datum.datum_details.qname.expanded_name,
                                             .uses_conventional_prefix = true})
                   : status;
    case SXML_TYPE_XS_NOTATION:
        return sxml_serializer_write_name(
            serializer, (sxml_written_name){.name = datum.datum_details.qname.expanded_name,
                                            .uses_conventional_prefix = false});
    default:
        break;
    }
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text canonical;
    status = sxml_cast_write_canonical_text(
        serializer->item_heap,
        &(sxml_canonical_text_request){.item = item,
                                       .output = {.bytes = output, .byte_count = sizeof(output)}},
        &canonical);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_finite_double =
        primitive_type.index == SXML_TYPE_XS_DOUBLE && canonical.text.byte_count > 0 &&
        canonical.text.bytes[canonical.text.byte_count - 1] >= '0' &&
        canonical.text.bytes[canonical.text.byte_count - 1] <= '9';
    if (primitive_type.index == SXML_TYPE_XS_DECIMAL) {
        return sxml_serializer_write(serializer, canonical.text);
    }
    if (is_finite_double) {
        return sxml_serializer_write_exponent_form(serializer, canonical.text);
    }
    /* Every other type, xs:float and the non-finite doubles: the syntax of
       a constructor function, xs:TYPE("VALUE"), where TYPE is the name of
       the item's type annotation, so xs:dateTimeStamp and
       xs:dayTimeDuration keep their names (Serialization 4.0, section 11,
       as revised by qtspecs PR 2981; QT4 Serialization-adaptive-37a). The
       rules above still go by the primitive type ("an instance of"), so
       derived strings stay quoted and derived integers stay numbers. */
    sxml_type_description description;
    status = sxml_type_registry_describe_type(serializer->registry,
                                              sxml_item_get_type_annotation(item), &description);
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_name(
            serializer, (sxml_written_name){.name = description.type_name,
                                            .uses_conventional_prefix = true});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("(\""));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_doubling(serializer, canonical.text, '"');
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\")"));
    }
    return status;
}

/* ------------------------------------------------------ Function items */

/* name#arity, or (anonymous-function)#arity: the serialization of an
   anonymous function item is implementation-defined (Serialization 4.0,
   section 11). */
static sxml_status sxml_serializer_write_function(sxml_serializer *serializer, sxml_item item) {
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.function_name.index == 0) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("(anonymous-function)"));
    } else {
        status = sxml_serializer_write_name(
            serializer, (sxml_written_name){.name = description.function_name,
                                            .uses_conventional_prefix = true});
    }
    char arity_text[24];
    size_t arity_count = 0;
    size_t arity = description.arity;
    char reversed[24];
    do {
        reversed[arity_count++] = (char)('0' + arity % 10U);
        arity /= 10U;
    } while (arity != 0);
    arity_text[0] = '#';
    for (size_t digit_index = 0; digit_index < arity_count; digit_index += 1) {
        arity_text[digit_index + 1] = reversed[arity_count - 1 - digit_index];
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write(
            serializer,
            (sxml_byte_view){.bytes = (const uint8_t *)arity_text, .byte_count = arity_count + 1});
    }
    return status;
}

/* -------------------------------------------------------- The frames */

static sxml_status sxml_serializer_push(sxml_serializer *serializer,
                                        const sxml_serialization_frame *frame) {
    const sxml_status status = sxml_serializer_grow(
        serializer->allocator,
        &(sxml_serializer_growth){.elements = (void **)&serializer->frames,
                                  .element_capacity = &serializer->frame_capacity,
                                  .element_count = serializer->frame_count,
                                  .element_byte_count = sizeof(sxml_serialization_frame),
                                  .alignment = alignof(sxml_serialization_frame)});
    if (status == SXML_STATUS_OK) {
        serializer->frames[serializer->frame_count] = *frame;
        serializer->frame_count += 1;
    }
    return status;
}

/* Pushes the frame of a member sequence, writing "(" unless it has one
   item. */
static sxml_status sxml_serializer_push_member(sxml_serializer *serializer,
                                               sxml_sequence sequence) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(sequence, serializer->error_record, &item_count);
    if (status == SXML_STATUS_OK && item_count != 1) {
        status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("("));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_push(
            serializer,
            &(sxml_serialization_frame){.frame_kind = SXML_SERIALIZATION_FRAME_KIND_MEMBER_SEQUENCE,
                                        .sequence = sequence,
                                        .container = {.representation = 0},
                                        .next_index = 0,
                                        .count = item_count,
                                        .is_parenthesized = item_count != 1});
    }
    return status;
}

/* Writes one item: atomic items, nodes and functions completely; arrays,
   maps and JNodes up to their opening, pushing a frame for the rest. */
static sxml_status sxml_serializer_write_item(sxml_serializer *serializer, sxml_item item) {
    sxml_status status = SXML_STATUS_OK;
    size_t size = 0;
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ATOMIC:
        return sxml_serializer_write_atomic(serializer, item);
    case SXML_ITEM_KIND_NODE:
        {
        sxml_node_reference node;
        status = sxml_item_get_node(item, &node);
        return status == SXML_STATUS_OK ? sxml_serializer_write_adaptive_node(serializer, node)
                                        : status;
    }
    case SXML_ITEM_KIND_FUNCTION:
        return sxml_serializer_write_function(serializer, item);
    case SXML_ITEM_KIND_ARRAY:
    case SXML_ITEM_KIND_MAP: {
        const bool is_map = sxml_item_get_kind(item) == SXML_ITEM_KIND_MAP;
        status = is_map ? sxml_map_get_size(item, &size) : sxml_array_get_size(item, &size);
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write(serializer, is_map ? SXML_BYTE_VIEW_FROM_LITERAL("{") : SXML_BYTE_VIEW_FROM_LITERAL("["));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_push(
                serializer,
                &(sxml_serialization_frame){.frame_kind = is_map
                                                              ? SXML_SERIALIZATION_FRAME_KIND_MAP
                                                              : SXML_SERIALIZATION_FRAME_KIND_ARRAY,
                                            .sequence = sxml_sequence_empty(),
                                            .container = item,
                                            .next_index = 0,
                                            .count = size,
                                            .is_parenthesized = false});
        }
        return status;
    }
    case SXML_ITEM_KIND_JSON_NODE: {
        /* Serialization 4.0, section 11: jtree(V) for a root, jnode(K:V)
           otherwise. */
        sxml_json_node_description description;
        status = sxml_json_node_describe(item, &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write(serializer, description.has_parent ? SXML_BYTE_VIEW_FROM_LITERAL("jnode(") : SXML_BYTE_VIEW_FROM_LITERAL("jtree("));
        }
        if (status == SXML_STATUS_OK && description.has_parent) {
            status = sxml_serializer_write_atomic(serializer, description.json_key);
            if (status == SXML_STATUS_OK) {
                status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(":"));
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_push(
                serializer,
                &(sxml_serialization_frame){.frame_kind = SXML_SERIALIZATION_FRAME_KIND_JSON_NODE_END,
                                            .sequence = sxml_sequence_empty(),
                                            .container = item,
                                            .next_index = 0,
                                            .count = 0,
                                            .is_parenthesized = false});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_push_member(serializer, description.json_value);
        }
        return status;
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Runs the top frame one step: writes its next item, member or entry, or
   ends it. */
static sxml_status sxml_serializer_step(sxml_serializer *serializer) {
    sxml_serialization_frame *frame = &serializer->frames[serializer->frame_count - 1];
    sxml_status status = SXML_STATUS_OK;
    if (frame->next_index == frame->count) {
        const sxml_serialization_frame_kind kind = frame->frame_kind;
        const bool is_parenthesized = frame->is_parenthesized;
        serializer->frame_count -= 1;
        switch (kind) {
        case SXML_SERIALIZATION_FRAME_KIND_TOP_SEQUENCE:
            return SXML_STATUS_OK;
        case SXML_SERIALIZATION_FRAME_KIND_MEMBER_SEQUENCE:
            return is_parenthesized ? sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(")")) : SXML_STATUS_OK;
        case SXML_SERIALIZATION_FRAME_KIND_ARRAY:
            return sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("]"));
        case SXML_SERIALIZATION_FRAME_KIND_MAP:
            return sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        case SXML_SERIALIZATION_FRAME_KIND_JSON_NODE_END:
            return sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(")"));
        }
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const uint64_t index = frame->next_index;
    frame->next_index += 1;
    switch (frame->frame_kind) {
    case SXML_SERIALIZATION_FRAME_KIND_TOP_SEQUENCE:
    case SXML_SERIALIZATION_FRAME_KIND_MEMBER_SEQUENCE: {
        const sxml_sequence sequence = frame->sequence;
        if (index > 0) {
            status = frame->frame_kind == SXML_SERIALIZATION_FRAME_KIND_TOP_SEQUENCE
                         ? sxml_serializer_write(serializer, serializer->item_separator)
                         : sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(","));
        }
        sxml_item item = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                sequence,
                (sxml_item_request){.item_index = index, .error_record = serializer->error_record},
                &item);
        }
        return status == SXML_STATUS_OK ? sxml_serializer_write_item(serializer, item) : status;
    }
    case SXML_SERIALIZATION_FRAME_KIND_ARRAY: {
        const sxml_item array = frame->container;
        sxml_sequence member = sxml_sequence_empty();
        if (index > 0) {
            status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(","));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_array_get_member(array, (size_t)index, &member);
        }
        return status == SXML_STATUS_OK ? sxml_serializer_push_member(serializer, member)
                                        : status;
    }
    case SXML_SERIALIZATION_FRAME_KIND_MAP: {
        const sxml_item map = frame->container;
        sxml_map_entry entry;
        if (index > 0) {
            status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(","));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_map_get_entry(map, (size_t)index, &entry);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write_atomic(serializer, entry.key);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_serializer_write(serializer, SXML_BYTE_VIEW_FROM_LITERAL(":"));
        }
        return status == SXML_STATUS_OK ? sxml_serializer_push_member(serializer, entry.value)
                                        : status;
    }
    case SXML_SERIALIZATION_FRAME_KIND_JSON_NODE_END:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}


/* Runs the adaptive method (Serialization 4.0, section 11) over value. */
static sxml_status sxml_serialize_adaptive_method(sxml_serializer *serializer,
                                                  sxml_sequence value) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, serializer->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_push(
            serializer,
            &(sxml_serialization_frame){.frame_kind = SXML_SERIALIZATION_FRAME_KIND_TOP_SEQUENCE,
                                        .sequence = value,
                                        .container = {.representation = 0},
                                        .next_index = 0,
                                        .count = item_count,
                                        .is_parenthesized = false});
    }
    while (status == SXML_STATUS_OK && serializer->frame_count > 0) {
        status = sxml_serializer_step(serializer);
    }
    return status;
}

/* ------------------------------------------------------------ Dispatch */

/* Sets up the character pipeline from the parameters (Serialization 4.0,
   section 4; canonical XML is UTF-8 with LF and no character maps,
   Canonical XML 1.1, section 2; canonical JSON is UTF-8, RFC 8785). */
static void sxml_serializer_resolve_pipeline(sxml_serializer *serializer) {
    const sxml_serialization_parameters *parameters = serializer->parameters;
    const sxml_serialization_method method = parameters->serialization_method;
    const bool is_xml_like =
        method == SXML_SERIALIZATION_METHOD_XML || method == SXML_SERIALIZATION_METHOD_XHTML;
    const bool is_canonical_requested = parameters->canonical == SXML_SERIALIZATION_FLAG_YES;
    const bool is_canonical = is_xml_like && is_canonical_requested;
    serializer->is_canonical_json = method == SXML_SERIALIZATION_METHOD_JSON && is_canonical_requested;
    serializer->character_encoding = is_canonical || serializer->is_canonical_json
                                         ? SXML_CHARACTER_ENCODING_UTF_8
                                         : parameters->character_encoding;
    switch (method) {
    case SXML_SERIALIZATION_METHOD_ADAPTIVE:
    case SXML_SERIALIZATION_METHOD_XML:
    case SXML_SERIALIZATION_METHOD_XHTML:
        serializer->escape_style = is_canonical ? SXML_ESCAPE_STYLE_CANONICAL : SXML_ESCAPE_STYLE_XML;
        break;
    case SXML_SERIALIZATION_METHOD_HTML:
        serializer->escape_style = SXML_ESCAPE_STYLE_HTML;
        break;
    case SXML_SERIALIZATION_METHOD_TEXT:
        serializer->escape_style = SXML_ESCAPE_STYLE_NONE;
        break;
    case SXML_SERIALIZATION_METHOD_JSON:
        serializer->escape_style = SXML_ESCAPE_STYLE_JSON;
        break;
    case SXML_SERIALIZATION_METHOD_CSV:
        serializer->escape_style = SXML_ESCAPE_STYLE_CSV;
        break;
    }
    /* Serialization 4.0, 5.1.2: version 1.0 (the default) has no C0
       controls but tab, newline and carriage return. */
    serializer->is_xml_version_1_0 =
        is_xml_like && !is_canonical &&
        (!parameters->has_version ||
         sxml_byte_view_equals(parameters->version, SXML_BYTE_VIEW_FROM_LITERAL("1.0")));
    serializer->is_html_before_5 =
        method == SXML_SERIALIZATION_METHOD_HTML && !sxml_serializer_requests_html5(parameters);
    serializer->escapes_solidus = parameters->escape_solidus != SXML_SERIALIZATION_FLAG_NO;
    serializer->csv_quote_codepoint =
        method != SXML_SERIALIZATION_METHOD_CSV         ? 0U
        : parameters->csv_quote_character_codepoint == 0 ? (uint32_t)'"'
                                                         : parameters->csv_quote_character_codepoint;
    const sxml_line_ending line_ending =
        is_canonical ? SXML_LINE_ENDING_LINE_FEED : parameters->line_ending;
    switch (line_ending) {
    case SXML_LINE_ENDING_LINE_FEED:
        serializer->line_ending_text = SXML_BYTE_VIEW_FROM_LITERAL("\n");
        break;
    case SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED:
        serializer->line_ending_text = SXML_BYTE_VIEW_FROM_LITERAL("\r\n");
        break;
    case SXML_LINE_ENDING_CARRIAGE_RETURN:
        serializer->line_ending_text = SXML_BYTE_VIEW_FROM_LITERAL("\r");
        break;
    }
    /* normalization-form does not apply to the adaptive method (11.1). */
    serializer->is_normalizing = method != SXML_SERIALIZATION_METHOD_ADAPTIVE &&
                                 parameters->normalization_form != SXML_NORMALIZATION_FORM_NONE;
    serializer->is_fully_normalized =
        serializer->is_normalizing &&
        parameters->normalization_form == SXML_NORMALIZATION_FORM_FULLY_NORMALIZED;
    /* D107: serialize.h's forms onto the Unicode module's; fully-normalized
       is NFC here, with the check of SERE0012 instead of a leading space
       (Serialization 4.0, 5.1.12). */
    switch (parameters->normalization_form) {
    case SXML_NORMALIZATION_FORM_NONE:
    case SXML_NORMALIZATION_FORM_NFC:
    case SXML_NORMALIZATION_FORM_FULLY_NORMALIZED:
        serializer->normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC;
        break;
    case SXML_NORMALIZATION_FORM_NFD:
        serializer->normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFD;
        break;
    case SXML_NORMALIZATION_FORM_NFKC:
        serializer->normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFKC;
        break;
    case SXML_NORMALIZATION_FORM_NFKD:
        serializer->normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFKD;
        break;
    }
    /* Section 12: character maps apply to every method but adaptive, and
       not to the canonical forms. */
    serializer->applies_character_maps = method != SXML_SERIALIZATION_METHOD_ADAPTIVE &&
                                         !is_canonical && !serializer->is_canonical_json;
}

/* Writes the byte order mark when byte-order-mark is yes (Serialization
   4.0, section 3; the Unicode Standard 16.0, section 23.8). */
static sxml_status sxml_serializer_write_byte_order_mark(sxml_serializer *serializer) {
    if (serializer->parameters->byte_order_mark != SXML_SERIALIZATION_FLAG_YES ||
        serializer->escape_style == SXML_ESCAPE_STYLE_CANONICAL || serializer->is_canonical_json) {
        return SXML_STATUS_OK;
    }
    if (serializer->character_encoding == SXML_CHARACTER_ENCODING_UTF_8) {
        return sxml_serializer_put_bytes(serializer, SXML_BYTE_VIEW_FROM_LITERAL("\xEF\xBB\xBF"));
    }
    if (serializer->character_encoding == SXML_CHARACTER_ENCODING_UTF_16) {
        return sxml_serializer_put_codepoint(serializer, 0xFEFFU);
    }
    return SXML_STATUS_OK;
}

/* Places a serializer for parameters over output_stream in *serializer,
   checks the parameters every method shares and writes the byte order
   mark. The caller releases it with sxml_serializer_release, also on
   failure. */
typedef struct sxml_serializer_setup {
    sxml_item_heap *item_heap;
    const sxml_serialization_parameters *parameters;
    sxml_output_stream output_stream;
    sxml_error_record *error_record;
} sxml_serializer_setup;

static sxml_status sxml_serializer_start(const sxml_serializer_setup *setup,
                                         sxml_serializer *serializer) {
    const sxml_serialization_parameters *parameters = setup->parameters;
    *serializer = (sxml_serializer){
        .item_heap = setup->item_heap,
        .name_pool = sxml_item_heap_get_name_pool(setup->item_heap),
        .registry = sxml_item_heap_get_type_registry(setup->item_heap),
        .allocator = sxml_item_heap_as_allocator(setup->item_heap),
        .error_record = setup->error_record,
        .parameters = parameters,
        .output_stream = setup->output_stream,
        .character_encoding = SXML_CHARACTER_ENCODING_UTF_8,
        .escape_style = SXML_ESCAPE_STYLE_XML,
        .is_xml_version_1_0 = false,
        .is_html_before_5 = false,
        .escapes_solidus = true,
        .is_canonical_json = false,
        .csv_quote_codepoint = 0,
        .line_ending_text = SXML_BYTE_VIEW_FROM_LITERAL("\n"),
        .is_normalizing = false,
        .is_fully_normalized = false,
        .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFC,
        .applies_character_maps = false,
        .sorted_mappings = NULL,
        .mapping_count = 0,
        .staged_byte_count = 0,
        .item_separator = parameters->has_item_separator ? parameters->item_separator
                                                         : SXML_BYTE_VIEW_FROM_LITERAL("\n"),
        .frames = NULL,
        .frame_count = 0,
        .frame_capacity = 0,
    };
    sxml_serializer_resolve_pipeline(serializer);
    sxml_status status = sxml_serializer_check_permitted_values(serializer);
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_sort_character_map(serializer);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_byte_order_mark(serializer);
    }
    return status;
}

static void sxml_serializer_release(sxml_serializer *serializer) {
    sxml_allocator_release(serializer->allocator, serializer->frames);
    sxml_allocator_release(serializer->allocator, serializer->sorted_mappings);
    serializer->frames = NULL;
    serializer->sorted_mappings = NULL;
}

/* -------------------------------------------------- Sub-serialization */

sxml_status sxml_serializer_write_node_as_string(sxml_serializer *serializer,
                                                 sxml_node_reference node) {
    const sxml_serialization_parameters *outer = serializer->parameters;
    /* Serialization 4.0, 9.1 and 9.3.12: omit-xml-declaration yes and no
       other parameter, unless canonical, which passes them all on. */
    sxml_serialization_parameters parameters = {.serialization_method =
                                                    SXML_SERIALIZATION_METHOD_XML};
    if (outer->canonical == SXML_SERIALIZATION_FLAG_YES) {
        parameters = *outer;
    }
    parameters.omit_xml_declaration = SXML_SERIALIZATION_FLAG_YES;
    switch (outer->json_node_output_method) {
    case SXML_JSON_NODE_OUTPUT_METHOD_XML:
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
        break;
    case SXML_JSON_NODE_OUTPUT_METHOD_XHTML:
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_XHTML;
        break;
    case SXML_JSON_NODE_OUTPUT_METHOD_HTML:
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_HTML;
        break;
    case SXML_JSON_NODE_OUTPUT_METHOD_TEXT:
        parameters.serialization_method = SXML_SERIALIZATION_METHOD_TEXT;
        break;
    }
    /* The string is made in UTF-8 and re-encoded by the outer pipeline. */
    parameters.character_encoding = SXML_CHARACTER_ENCODING_UTF_8;
    parameters.byte_order_mark = SXML_SERIALIZATION_FLAG_NO;
    sxml_output_buffer *output = NULL;
    sxml_status status = sxml_output_buffer_create(
        serializer->allocator,
        &(sxml_output_buffer_configuration){.allocator = serializer->allocator,
                                            .maximum_byte_count = 0},
        &output);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_item item = sxml_node_item_make(node);
    sxml_serializer inner;
    status = sxml_serializer_start(
        &(sxml_serializer_setup){.item_heap = serializer->item_heap,
                                 .parameters = &parameters,
                                 .output_stream = sxml_output_buffer_as_output_stream(output),
                                 .error_record = serializer->error_record},
        &inner);
    if (status == SXML_STATUS_OK) {
        status = parameters.serialization_method == SXML_SERIALIZATION_METHOD_TEXT
                     ? sxml_serialize_text_method(&inner, sxml_sequence_from_item(item))
                     : sxml_serialize_xml_method(&inner, sxml_sequence_from_item(item));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_flush(&inner);
    }
    sxml_serializer_release(&inner);
    sxml_item_release(item);
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_write_character_data(
            serializer, &(sxml_character_data){.text = sxml_output_buffer_get_bytes(output),
                                               .character_context = SXML_CHARACTER_CONTEXT_TEXT});
    }
    sxml_output_buffer_destroy(output);
    return status;
}

sxml_status sxml_serialize_sequence(sxml_item_heap *item_heap,
                                    const sxml_serialization_request *request,
                                    sxml_output_stream output_stream) {
    if (item_heap == NULL || request == NULL || output_stream.operations == NULL ||
        output_stream.operations->write == NULL ||
        !sxml_parameters_are_expressible(&request->serialization_parameters)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_serializer serializer;
    sxml_status status = sxml_serializer_start(
        &(sxml_serializer_setup){.item_heap = item_heap,
                                 .parameters = &request->serialization_parameters,
                                 .output_stream = output_stream,
                                 .error_record = request->error_record},
        &serializer);
    if (status == SXML_STATUS_OK) {
        switch (request->serialization_parameters.serialization_method) {
        case SXML_SERIALIZATION_METHOD_ADAPTIVE:
            status = sxml_serialize_adaptive_method(&serializer, request->value);
            break;
        case SXML_SERIALIZATION_METHOD_XML:
        case SXML_SERIALIZATION_METHOD_XHTML:
        case SXML_SERIALIZATION_METHOD_HTML:
            status = sxml_serialize_xml_method(&serializer, request->value);
            break;
        case SXML_SERIALIZATION_METHOD_TEXT:
            status = sxml_serialize_text_method(&serializer, request->value);
            break;
        case SXML_SERIALIZATION_METHOD_JSON:
            status = sxml_serialize_json_method(&serializer, request->value);
            break;
        case SXML_SERIALIZATION_METHOD_CSV:
            status = sxml_serialize_csv_method(&serializer, request->value);
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serializer_flush(&serializer);
    }
    sxml_serializer_release(&serializer);
    return status;
}
