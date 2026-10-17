/*
 * Reasons of test outcomes and assertion verdicts (reason.h). Imitates
 * tests/qt4/driver/suite_path.c: plain values over arena memory.
 */
#include "reason.h"

#include "sxml/serialize.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Room for the final "...". */
enum { SXML_QT4_REASON_ELLIPSIS_BYTE_COUNT = 3 };

sxml_status sxml_qt4_reason_start(sxml_arena *arena, sxml_qt4_reason *reason) {
    void *memory = NULL;
    const sxml_status status = sxml_arena_allocate(
        arena,
        (sxml_allocation_request){
            .byte_count = SXML_QT4_REASON_MAXIMUM_BYTE_COUNT +
                          SXML_QT4_REASON_ELLIPSIS_BYTE_COUNT,
            .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *reason = (sxml_qt4_reason){.bytes = memory,
                                .byte_count = 0,
                                .capacity = SXML_QT4_REASON_MAXIMUM_BYTE_COUNT,
                                .is_truncated = false};
    return SXML_STATUS_OK;
}

void sxml_qt4_reason_append(sxml_qt4_reason *reason, sxml_byte_view text) {
    if (reason->is_truncated || text.byte_count == 0) {
        return;
    }
    size_t byte_count = text.byte_count;
    const size_t room = reason->capacity - reason->byte_count;
    if (byte_count > room) {
        /* Cut before a continuation byte so the text stays UTF-8
           (Unicode 16.0, section 3.9, table 3-7). */
        byte_count = room;
        while (byte_count > 0 && (text.bytes[byte_count] & 0xC0U) == 0x80U) {
            byte_count -= 1;
        }
        reason->is_truncated = true;
    }
    if (byte_count != 0) {
        memcpy(reason->bytes + reason->byte_count, text.bytes, byte_count);
        reason->byte_count += byte_count;
    }
}

void sxml_qt4_reason_append_decimal(sxml_qt4_reason *reason, uint64_t number) {
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof digits - 1 - digit_count] = (uint8_t)('0' + number % 10U);
        number /= 10U;
        digit_count += 1;
    } while (number != 0);
    sxml_qt4_reason_append(
        reason, (sxml_byte_view){.bytes = digits + sizeof digits - digit_count,
                                 .byte_count = digit_count});
}

void sxml_qt4_reason_append_error(sxml_qt4_reason *reason,
                                  sxml_qt4_raised_error raised_error) {
    if (!sxml_status_has_error_record(raised_error.status) ||
        raised_error.error_record == NULL) {
        sxml_qt4_reason_append(reason, sxml_status_name(raised_error.status));
        if (raised_error.error_record != NULL &&
            raised_error.error_record->diagnostic.description.byte_count != 0) {
            sxml_qt4_reason_append(reason, SXML_TEXT(": "));
            sxml_qt4_reason_append(
                reason, raised_error.error_record->diagnostic.description);
        }
        return;
    }
    const sxml_error_code *code = &raised_error.error_record->code;
    if (code->namespace_prefix.byte_count != 0) {
        sxml_qt4_reason_append(reason, code->namespace_prefix);
        sxml_qt4_reason_append(reason, SXML_TEXT(":"));
    } else if (code->namespace_uri.byte_count != 0) {
        sxml_qt4_reason_append(reason, SXML_TEXT("Q{"));
        sxml_qt4_reason_append(reason, code->namespace_uri);
        sxml_qt4_reason_append(reason, SXML_TEXT("}"));
    }
    sxml_qt4_reason_append(reason, code->local_name);
    if (raised_error.error_record->diagnostic.description.byte_count != 0) {
        sxml_qt4_reason_append(reason, SXML_TEXT(": "));
        sxml_qt4_reason_append(reason,
                               raised_error.error_record->diagnostic.description);
    }
}

/* The output stream of sxml_qt4_reason_append_value: stops the serializer
   once the reason is full. */
typedef struct sxml_qt4_reason_stream {
    sxml_qt4_reason *reason;
    bool is_full;
} sxml_qt4_reason_stream;

static sxml_status sxml_qt4_reason_stream_write(void *state,
                                                sxml_byte_view bytes) {
    sxml_qt4_reason_stream *stream = state;
    sxml_qt4_reason_append(stream->reason, bytes);
    if (stream->reason->is_truncated) {
        stream->is_full = true;
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return SXML_STATUS_OK;
}

static const sxml_output_stream_operations sxml_qt4_reason_stream_operations = {
    .write = sxml_qt4_reason_stream_write,
};

sxml_status sxml_qt4_reason_append_value(sxml_qt4_reason *reason,
                                         sxml_qt4_shown_value shown_value) {
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    uint64_t item_count = 0;
    sxml_status status =
        sxml_sequence_count(shown_value.value, &error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_reason_append(reason, SXML_TEXT("(not serializable)"));
        return sxml_status_has_error_record(status) ? SXML_STATUS_OK : status;
    }
    if (item_count != 1) {
        sxml_qt4_reason_append(reason, SXML_TEXT("("));
    }
    sxml_qt4_reason_stream stream = {.reason = reason, .is_full = false};
    status = sxml_serialize_sequence(
        shown_value.item_heap,
        &(sxml_serialization_request){
            .value = shown_value.value,
            .serialization_parameters =
                {.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE,
                 .has_item_separator = true,
                 .item_separator = SXML_TEXT(", ")},
            .error_record = &error_record},
        (sxml_output_stream){.state = &stream,
                             .operations = &sxml_qt4_reason_stream_operations});
    if (stream.is_full) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_reason_append(reason, SXML_TEXT(" (not serializable)"));
        return sxml_status_has_error_record(status) ? SXML_STATUS_OK : status;
    }
    if (item_count != 1) {
        sxml_qt4_reason_append(reason, SXML_TEXT(")"));
    }
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_qt4_reason_text(sxml_qt4_reason *reason) {
    size_t byte_count = reason->byte_count;
    if (reason->is_truncated) {
        memcpy(reason->bytes + byte_count, "...",
               SXML_QT4_REASON_ELLIPSIS_BYTE_COUNT);
        byte_count += SXML_QT4_REASON_ELLIPSIS_BYTE_COUNT;
    }
    return (sxml_byte_view){.bytes = reason->bytes, .byte_count = byte_count};
}
