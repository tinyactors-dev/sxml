/*
 * Suite paths (suite_path.h). Imitates src/string.c: plain functions over
 * byte views, memory from the caller's arena.
 */
#include "suite_path.h"

#include <stdalign.h>
#include <string.h>

sxml_status sxml_qt4_concatenate(sxml_arena *arena,
                                 sxml_qt4_concatenation concatenation,
                                 sxml_byte_view *result) {
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_add(concatenation.first.byte_count,
                      concatenation.second.byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (byte_count == 0) {
        *result = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    status = sxml_arena_allocate(
        arena,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    if (concatenation.first.byte_count != 0) {
        memcpy(bytes, concatenation.first.bytes,
               concatenation.first.byte_count);
    }
    if (concatenation.second.byte_count != 0) {
        memcpy(bytes + concatenation.first.byte_count,
               concatenation.second.bytes, concatenation.second.byte_count);
    }
    *result = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

/* The directory part of path, its final "/" included; empty for none. */
static sxml_byte_view sxml_qt4_directory_of(sxml_byte_view path) {
    size_t byte_count = path.byte_count;
    while (byte_count > 0 && path.bytes[byte_count - 1] != '/') {
        byte_count -= 1;
    }
    return (sxml_byte_view){.bytes = path.bytes, .byte_count = byte_count};
}

static bool sxml_qt4_segment_is(sxml_byte_view segment, sxml_byte_view text) {
    return sxml_byte_view_equals(segment, text);
}

sxml_status
sxml_qt4_suite_path_resolve(const sxml_qt4_suite_path_request *request,
                            sxml_byte_view *suite_path) {
    const bool is_absolute =
        request->reference.byte_count > 0 && request->reference.bytes[0] == '/';
    sxml_byte_view combined = request->reference;
    sxml_status status = SXML_STATUS_OK;
    if (!is_absolute) {
        status = sxml_qt4_concatenate(
            request->arena,
            (sxml_qt4_concatenation){
                .first = sxml_qt4_directory_of(request->containing_file),
                .second = request->reference},
            &combined);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (combined.byte_count == 0) {
        *suite_path = combined;
        return SXML_STATUS_OK;
    }
    /* At most one segment per "/" plus one: the stack of kept segments
       (RFC 3986, section 5.2.4, "remove_dot_segments"). */
    size_t segment_capacity = 1;
    for (size_t byte_index = 0; byte_index < combined.byte_count;
         byte_index += 1) {
        if (combined.bytes[byte_index] == '/') {
            segment_capacity += 1;
        }
    }
    size_t stack_byte_count = 0;
    status = sxml_size_multiply(segment_capacity, sizeof(sxml_byte_view),
                                &stack_byte_count);
    void *stack_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_allocate(
            request->arena,
            (sxml_allocation_request){.byte_count = stack_byte_count,
                                      .alignment = alignof(sxml_byte_view)},
            &stack_memory);
    }
    void *result_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_allocate(
            request->arena,
            (sxml_allocation_request){.byte_count = combined.byte_count,
                                      .alignment = 1},
            &result_memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view *segments = stack_memory;
    size_t segment_count = 0;
    size_t segment_start = 0;
    for (size_t byte_index = 0; byte_index <= combined.byte_count;
         byte_index += 1) {
        if (byte_index < combined.byte_count &&
            combined.bytes[byte_index] != '/') {
            continue;
        }
        const sxml_byte_view segment = {.bytes = combined.bytes + segment_start,
                                        .byte_count =
                                            byte_index - segment_start};
        segment_start = byte_index + 1;
        if (segment.byte_count == 0 ||
            sxml_qt4_segment_is(segment, SXML_BYTE_VIEW_FROM_LITERAL("."))) {
            continue;
        }
        if (sxml_qt4_segment_is(segment, SXML_BYTE_VIEW_FROM_LITERAL(".."))) {
            if (segment_count > 0 &&
                !sxml_qt4_segment_is(segments[segment_count - 1],
                                     SXML_BYTE_VIEW_FROM_LITERAL(".."))) {
                segment_count -= 1;
                continue;
            }
            if (is_absolute) {
                /* Above the root there is nothing to go to. */
                continue;
            }
        }
        segments[segment_count] = segment;
        segment_count += 1;
    }
    uint8_t *result = result_memory;
    size_t result_byte_count = 0;
    if (is_absolute) {
        result[0] = '/';
        result_byte_count = 1;
    }
    for (size_t segment_index = 0; segment_index < segment_count;
         segment_index += 1) {
        if (segment_index > 0) {
            result[result_byte_count] = '/';
            result_byte_count += 1;
        }
        memcpy(result + result_byte_count, segments[segment_index].bytes,
               segments[segment_index].byte_count);
        result_byte_count += segments[segment_index].byte_count;
    }
    *suite_path =
        (sxml_byte_view){.bytes = result, .byte_count = result_byte_count};
    return SXML_STATUS_OK;
}
