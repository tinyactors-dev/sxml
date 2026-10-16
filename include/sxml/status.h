#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/view.h"

/*
 * Status codes, error records and diagnostics (docs/C_GUIDELINES.md, "Errors").
 *
 * Every fallible function returns an sxml_status and delivers its results
 * through output parameters. Unless a declaration says otherwise, a function
 * that fails leaves its outputs untouched (docs/DECISIONS.md, D20).
 *
 * Three status values say "a specification error was raised":
 * SXML_STATUS_STATIC_ERROR, SXML_STATUS_DYNAMIC_ERROR and
 * SXML_STATUS_TYPE_ERROR. The details (the error code such as err:FOAR0001,
 * a description, the error value and where it happened) go into an
 * sxml_error_record supplied by the caller. Every other status is a condition
 * of the C interface itself; its optional details go into an sxml_diagnostic.
 */

/*
 * SXML_NODISCARD marks a function whose result must not be ignored; ignoring
 * an sxml_status is a build error (-Werror). Placed before the declaration:
 *
 *     SXML_NODISCARD SXML_EXPORT sxml_status sxml_size_add(...);
 *
 * C23 [[nodiscard]] where available; otherwise the GCC and Clang attribute,
 * or the MSVC source annotation, which MSVC checks under /analyze.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define SXML_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define SXML_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#include <sal.h>
#define SXML_NODISCARD _Check_return_
#else
#define SXML_NODISCARD
#endif

/*
 * The result of a fallible function.
 *
 * SXML_STATUS_OK is zero; every failure is non-zero. The set is closed: code
 * that switches over it lists every value (docs/C_GUIDELINES.md, "Types and
 * expressions").
 */
typedef enum sxml_status {
    /* The operation succeeded. */
    SXML_STATUS_OK = 0,

    /* An argument breaks the function's stated preconditions, for example
       an alignment that is not a power of two. */
    SXML_STATUS_INVALID_ARGUMENT,
    /* Caller-provided memory is smaller than the memory requirements. */
    SXML_STATUS_INSUFFICIENT_MEMORY,
    /* Caller-provided memory does not have the required alignment. */
    SXML_STATUS_MISALIGNED_MEMORY,
    /* An object without an allocator has used up its fixed capacity. */
    SXML_STATUS_CAPACITY_EXCEEDED,
    /* A limit from a configuration was reached, such as a maximum byte count
       or nesting depth meant to bound work on untrusted input. */
    SXML_STATUS_LIMIT_EXCEEDED,
    /* An allocator could not provide the requested memory. */
    SXML_STATUS_OUT_OF_MEMORY,
    /* A size computation would not fit in size_t. */
    SXML_STATUS_ARITHMETIC_OVERFLOW,
    /* An index is not below the number of elements it indexes. */
    SXML_STATUS_INDEX_OUT_OF_RANGE,

    /* Input ended in the middle of a construct. */
    SXML_STATUS_UNEXPECTED_END_OF_INPUT,
    /* Input does not have the required syntax. */
    SXML_STATUS_MALFORMED_INPUT,
    /* Bytes are not well-formed UTF-8 (Unicode 16.0, section 3.9, D92). */
    SXML_STATUS_INVALID_UTF8,
    /* The input uses a feature this build of sxml does not implement. */
    SXML_STATUS_UNSUPPORTED_FEATURE,

    /* A static error was raised; details are in the error record
       (XPath 4.0, section 2.4.1 "Kinds of Errors"). */
    SXML_STATUS_STATIC_ERROR,
    /* A dynamic error was raised, including errors raised by fn:error and
       by F&O 4.0 functions; details are in the error record. */
    SXML_STATUS_DYNAMIC_ERROR,
    /* A type error was raised; details are in the error record. */
    SXML_STATUS_TYPE_ERROR,
} sxml_status;

/*
 * Returns the name of a status constant, for example "SXML_STATUS_OK", as
 * ASCII text with static storage duration owned by the library. A value
 * outside the enumeration gives "SXML_STATUS_UNKNOWN".
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT sxml_byte_view sxml_status_name(sxml_status status);

/*
 * True when status reports a specification error whose details are in an
 * error record: SXML_STATUS_STATIC_ERROR, SXML_STATUS_DYNAMIC_ERROR or
 * SXML_STATUS_TYPE_ERROR.
 *
 * Thread safety: safe to call from any thread.
 */
SXML_EXPORT bool sxml_status_has_error_record(sxml_status status);

/*
 * The namespace of the error codes defined by the XPath, XQuery, XSLT and
 * F&O specifications, conventionally bound to the prefix "err"
 * (XPath 4.0, section 2.4.2 "Identifying and Reporting Errors").
 */
#define SXML_ERROR_NAMESPACE_URI "http://www.w3.org/2005/xqt-errors"

/*
 * Where something is in a source text: a query, a stylesheet, a document.
 *
 * Numbers count from 1; 0 means unknown. column_number counts codepoints from
 * the start of the line. byte_offset counts bytes from the start of the
 * source text and is meaningful only when line_number is not 0.
 */
typedef struct sxml_source_location {
    /* The module URI, file name or other name the caller gave the source;
       empty when it has none. */
    sxml_byte_view source_name;
    uint32_t line_number;
    uint32_t column_number;
    size_t byte_offset;
} sxml_source_location;

/*
 * Caller-supplied record that receives what went wrong and where.
 *
 * Functions that accept an optional diagnostic (a pointer that may be NULL,
 * normally a field of a request or configuration) fill it when they fail and
 * leave it untouched when they succeed.
 *
 * Lifetime: description and location.source_name point either to text with
 * static storage duration or into memory owned by the object that reported
 * the failure. They stay valid until that object is next used or finalized,
 * whichever comes first; copy them to keep them longer (D21).
 */
typedef struct sxml_diagnostic {
    /* Human-readable UTF-8 text; empty when there is none. */
    sxml_byte_view description;
    sxml_source_location location;
} sxml_diagnostic;

/*
 * The name of a specification error: an expanded QName such as
 * {http://www.w3.org/2005/xqt-errors}FOAR0001. User-defined error codes from
 * fn:error may use any namespace, or none.
 */
typedef struct sxml_error_code {
    /* Empty for a name in no namespace. */
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
    /* The prefix to show the name with, such as "err"; may be empty. Not
       part of the name's identity. */
    sxml_byte_view namespace_prefix;
} sxml_error_code;

/*
 * An XDM sequence. Declared here so an error record can carry an error value;
 * defined by include/sxml/xdm.h.
 */
typedef struct sxml_sequence sxml_sequence;

/*
 * A raised specification error, filled when a function returns a status for
 * which sxml_status_has_error_record is true.
 *
 * The fields hold what an XQuery 4.0 catch clause binds (XQuery 4.0, section
 * "Try/Catch Expressions"): code is $err:code, diagnostic.description is
 * $err:description, value is $err:value, and diagnostic.location gives
 * $err:module, $err:line-number and $err:column-number.
 *
 * Errors raised by sxml itself need no memory: their code and description
 * are static text (D21). An error whose description or value is computed
 * (fn:error, a host function) is raised through sxml_item_heap_raise_error
 * (include/sxml/xdm.h): the item heap keeps the text and the value, and the
 * record points into the heap until the next error raised on it, until
 * sxml_item_heap_clear_raised_error, or until the heap is finalized,
 * whichever comes first (D41). A catch clause that binds $err:value retains
 * the value before evaluating anything else.
 */
typedef struct sxml_error_record {
    sxml_error_code code;
    /* The error value; NULL when the error has none (fn:error called with
       fewer than three arguments). Borrowed, as described above. */
    const sxml_sequence *value;
    sxml_diagnostic diagnostic;
} sxml_error_record;
