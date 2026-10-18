/*
 * Snippet text (src/xslt_snippet_text_internal.h). Golden file: the
 * helpers it replaces in src/xslt_key.c (packet 4.10).
 */
#include "xslt_snippet_text_internal.h"

#include <string.h>

/* The first capacity of a text. */
#define SNIPPET_TEXT_FIRST_CAPACITY 256U

void sxml_xslt_snippet_text_append(sxml_xslt_snippet_text *text, sxml_byte_view bytes) {
    if (text->status != SXML_STATUS_OK || bytes.byte_count == 0) {
        return;
    }
    size_t needed = 0;
    text->status = sxml_size_add(text->byte_count, bytes.byte_count, &needed);
    if (text->status == SXML_STATUS_OK && needed > text->byte_capacity) {
        size_t capacity =
            text->byte_capacity == 0 ? SNIPPET_TEXT_FIRST_CAPACITY : text->byte_capacity;
        while (capacity < needed && text->status == SXML_STATUS_OK) {
            text->status = sxml_size_multiply(capacity, 2, &capacity);
        }
        void *memory = NULL;
        if (text->status == SXML_STATUS_OK) {
            text->status = sxml_allocator_allocate(
                text->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
                &memory);
        }
        if (text->status == SXML_STATUS_OK) {
            if (text->byte_count > 0) {
                memcpy(memory, text->bytes, text->byte_count);
            }
            sxml_allocator_release(text->allocator, text->bytes);
            text->bytes = memory;
            text->byte_capacity = capacity;
        }
    }
    if (text->status == SXML_STATUS_OK && text->bytes != NULL) {
        memcpy(text->bytes + text->byte_count, bytes.bytes, bytes.byte_count);
        text->byte_count = needed;
    }
}

void sxml_xslt_snippet_text_append_number(sxml_xslt_snippet_text *text, size_t number) {
    uint8_t digits[24];
    size_t digit_count = 0;
    size_t rest = number;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + rest % 10U);
        digit_count += 1;
        rest /= 10U;
    } while (rest > 0 && digit_count < sizeof(digits));
    sxml_xslt_snippet_text_append(text,
                                  (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                                   .byte_count = digit_count});
}

void sxml_xslt_snippet_text_append_string_literal(sxml_xslt_snippet_text *text,
                                                  sxml_byte_view value) {
    SXML_XSLT_SNIPPET_APPEND(text, "\"");
    size_t start = 0;
    for (size_t byte_index = 0; byte_index < value.byte_count; byte_index += 1) {
        if (value.bytes[byte_index] == '"') {
            sxml_xslt_snippet_text_append(text,
                                          (sxml_byte_view){.bytes = value.bytes + start,
                                                           .byte_count = byte_index + 1 - start});
            SXML_XSLT_SNIPPET_APPEND(text, "\"");
            start = byte_index + 1;
        }
    }
    if (value.byte_count > start) {
        sxml_xslt_snippet_text_append(
            text,
            (sxml_byte_view){.bytes = value.bytes + start, .byte_count = value.byte_count - start});
    }
    SXML_XSLT_SNIPPET_APPEND(text, "\"");
}

void sxml_xslt_snippet_text_append_qname(sxml_xslt_snippet_text *text,
                                         const sxml_name_pool *name_pool,
                                         sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (text->status == SXML_STATUS_OK) {
        text->status = sxml_name_pool_get_name_parts(name_pool, name, &parts);
    }
    if (text->status == SXML_STATUS_OK && parts.namespace_uri.index != 0) {
        text->status =
            sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri);
    }
    SXML_XSLT_SNIPPET_APPEND(text, "QName(");
    sxml_xslt_snippet_text_append_string_literal(text, namespace_uri);
    SXML_XSLT_SNIPPET_APPEND(text, ", ");
    sxml_xslt_snippet_text_append_string_literal(text, parts.local_name);
    SXML_XSLT_SNIPPET_APPEND(text, ")");
}

sxml_byte_view sxml_xslt_snippet_text_view(const sxml_xslt_snippet_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

void sxml_xslt_snippet_text_release(sxml_xslt_snippet_text *text) {
    sxml_allocator_release(text->allocator, text->bytes);
    text->bytes = NULL;
    text->byte_count = 0;
    text->byte_capacity = 0;
}
