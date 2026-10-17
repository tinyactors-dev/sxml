#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * The reason of a test outcome or an assertion verdict (glossary:
 * test_outcome, assertion_verdict): one line of text, such as
 * "assert-eq: got 3", built from pieces into a fixed buffer in an arena.
 * Text past the buffer is cut at a UTF-8 character boundary and the
 * reason ends in "...", so a huge result cannot make a huge report.
 *
 * A plain value: start it, append to it, read it. Its text lives as long
 * as the arena's memory.
 */
typedef struct sxml_qt4_reason {
    uint8_t *bytes;
    size_t byte_count;
    size_t capacity;
    bool is_truncated;
} sxml_qt4_reason;

/* The most bytes of a reason, without the final "...". */
enum { SXML_QT4_REASON_MAXIMUM_BYTE_COUNT = 400 };

/*
 * Allocates the buffer of *reason in arena and empties it. Returns the
 * arena's failures, leaving *reason untouched.
 */
SXML_NODISCARD sxml_status sxml_qt4_reason_start(sxml_arena *arena,
                                                 sxml_qt4_reason *reason);

/* Appends text, as much as fits. */
void sxml_qt4_reason_append(sxml_qt4_reason *reason, sxml_byte_view text);

/* Appends a number in decimal. */
void sxml_qt4_reason_append_decimal(sxml_qt4_reason *reason, uint64_t number);

/*
 * Appends what an error record holds: "err:CODE: description" (the
 * prefix, or Q{uri} when there is none), or the status name for a status
 * without an error record.
 */
typedef struct sxml_qt4_raised_error {
    sxml_status status;
    /* May be NULL for a status without an error record. */
    const sxml_error_record *error_record;
} sxml_qt4_raised_error;

void sxml_qt4_reason_append_error(sxml_qt4_reason *reason,
                                  sxml_qt4_raised_error raised_error);

/* A value to show in a reason. */
typedef struct sxml_qt4_shown_value {
    sxml_item_heap *item_heap;
    sxml_sequence value;
} sxml_qt4_shown_value;

/*
 * Appends value by the adaptive output method (Serialization 4.0, section
 * 11), items separated by ", " and in parentheses unless there is exactly
 * one, as much as fits; a value that cannot be serialized appends
 * "(not serializable)". Returns the failures of serializing that are not
 * the value's own (the heap's), with the reason unchanged or partly
 * appended.
 */
SXML_NODISCARD sxml_status sxml_qt4_reason_append_value(
    sxml_qt4_reason *reason, sxml_qt4_shown_value shown_value);

/* The text so far, with "..." at the end when it was cut. Borrowed from
   the reason; valid until the next append or the arena's reset. */
sxml_byte_view sxml_qt4_reason_text(sxml_qt4_reason *reason);
