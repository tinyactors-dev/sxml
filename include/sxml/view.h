#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Byte views and memory views: a pointer that always travels with its length
 * (docs/C_GUIDELINES.md, "Memory and buffers").
 *
 * The view types live in their own header because both include/sxml/status.h
 * and include/sxml/memory.h need them, and memory.h needs status.h
 * (docs/DECISIONS.md, D19). Include sxml/memory.h for the functions that work
 * on views.
 *
 * Views never own what they point to. A view is valid while the memory it
 * points into is; whoever produced the view states that lifetime. Views are
 * plain values: copy them freely and pass them by value.
 */

/*
 * Read-only bytes: bytes[0] to bytes[byte_count - 1].
 *
 * bytes may be NULL only when byte_count is 0. Text in sxml is UTF-8 in byte
 * views, never NUL-terminated, except at external boundaries.
 */
typedef struct sxml_byte_view {
    const uint8_t *bytes;
    size_t byte_count;
} sxml_byte_view;

/*
 * Writable bytes: bytes[0] to bytes[byte_count - 1].
 *
 * bytes may be NULL only when byte_count is 0. Used for caller-provided memory
 * that objects are placed in, and for output buffers.
 */
typedef struct sxml_memory_view {
    uint8_t *bytes;
    size_t byte_count;
} sxml_memory_view;

/*
 * Brace initializer for a byte view over a string literal, without its
 * terminating NUL. Usable in static tables:
 *
 *     static const sxml_byte_view name = SXML_BYTE_VIEW_INITIALIZER("FOAR0001");
 *
 * A macro because a function cannot take the size of a literal. literal must
 * be a string literal: with a pointer, sizeof gives the pointer size.
 */
#define SXML_BYTE_VIEW_INITIALIZER(literal)                                   \
    {.bytes = (const uint8_t *)(literal), .byte_count = sizeof(literal) - 1}

/* A byte view over a string literal, as an expression (a compound literal). */
#define SXML_BYTE_VIEW_FROM_LITERAL(literal)                                  \
    ((sxml_byte_view)SXML_BYTE_VIEW_INITIALIZER(literal))
