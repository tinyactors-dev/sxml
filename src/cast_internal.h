#pragma once

/*
 * Casting (F&O 4.0, section 19 "Casting"; XPath 4.0, section 4.17 "Cast
 * expressions"): converting an atomic item to a target type, the lexical
 * forms casting from xs:string parses, and the canonical text casting to
 * xs:string writes (docs/DECISIONS.md, D66).
 *
 * Users: the evaluator (cast as, castable as, constructor functions, the
 * untypedAtomic casting and the numeric and URI promotion of coercion), the
 * operators (untypedAtomic operands), the function library (fn:string,
 * fn:number, fn:concat, ...) and serialization (canonical text).
 *
 * Files
 * -----
 * - src/cast.c: the casting table between primitive types, target types
 *   that are atomic, list (xs:NMTOKENS), union (xs:numeric), enumeration or
 *   choice types, whitespace processing, cast and castable.
 * - src/cast_lexical.c: lexical forms of every primitive type to atomic
 *   items, and atomic items to their canonical text.
 *
 * Errors are reported through the request's error record (which may be
 * NULL) with the codes of F&O 4.0: XPTY0004 (a source type that cannot be
 * cast to the target, or a value of more than one item), FORG0001 (a
 * lexical form or value outside the target type), FOCA0002 (NaN or an
 * infinity to xs:decimal or xs:integer), FOCA0001 and FOCA0003 (a decimal
 * or integer beyond the heap's digit limit), FODT0001 and FODT0002 (a year
 * or duration beyond the int64_t components), FONS0004 (an unbound prefix)
 * and the static XPST0080 (a target type that cannot be cast to:
 * xs:NOTATION, xs:anyAtomicType, xs:anySimpleType, non-atomic item types).
 * Outputs are untouched on failure (D20).
 *
 * Thread safety: one thread per item heap (D35).
 */
#include "sxml/tree.h"
#include "xdm_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------- Casts */

/*
 * A value to cast, as "value cast as target_type" (with "?" when
 * allows_empty) or the constructor function of target_type (which allows
 * the empty sequence).
 */
typedef struct sxml_cast_request {
    /* The atomized operand, borrowed: atomic items only (atomizing nodes
       and arrays is the evaluator's, XPath 4.0, section 2.5.3). */
    sxml_sequence value;
    /* An atomic, list, union, enumeration or choice type issued by the
       heap's registry. */
    sxml_type_identifier target_type;
    /* The empty sequence casts to the empty sequence; otherwise it is
       XPTY0004. */
    bool allows_empty;
    /* The statically known namespaces, for a cast from xs:string or
       xs:untypedAtomic to xs:QName (F&O 4.0, section 19.1.9 "Casting to
       xs:QName and xs:NOTATION"): a binding with prefix 0 gives the default
       namespace for elements and types. Prefixes not bound here fall back
       to the predeclared ones (xml, xs, xsi, fn, map, array, math, err).
       Borrowed for the call. */
    sxml_namespace_binding_view namespace_bindings;
    /* Receives the error; may be NULL. */
    sxml_error_record *error_record;
} sxml_cast_request;

/*
 * Casts request->value to request->target_type and writes the result,
 * owned, to *result: one atomic item annotated with the target type (an
 * item whose annotation is already the target type is returned as it is),
 * a sequence of items for a list type, the empty sequence for an empty
 * value that is allowed.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR or SXML_STATUS_TYPE_ERROR with the
 * error record filled (see above), SXML_STATUS_STATIC_ERROR (XPST0080) for
 * a target type that cannot be cast to, and the heap's failures.
 */
SXML_NODISCARD sxml_status
sxml_cast_make_value(sxml_item_heap *item_heap,
                     const sxml_cast_request *request, sxml_sequence *result);

/* The outcome of sxml_cast_test_castability. */
typedef struct sxml_castability {
    bool is_castable;
} sxml_castability;

/*
 * Writes to *castability whether sxml_cast_make_value would succeed
 * ("castable as", XPath 4.0, section 4.17.3): a dynamic or type error of
 * the cast gives false and leaves request->error_record untouched.
 *
 * Returns SXML_STATUS_STATIC_ERROR (XPST0080) as sxml_cast_make_value
 * does, and the heap's failures (memory, limits), which are not answers.
 */
SXML_NODISCARD sxml_status sxml_cast_test_castability(
    sxml_item_heap *item_heap, const sxml_cast_request *request,
    sxml_castability *castability);

/* Text to cast as if it were an xs:string item. */
typedef struct sxml_text_cast_request {
    /* Well-formed UTF-8, borrowed for the call. */
    sxml_byte_view text;
    sxml_type_identifier target_type;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_text_cast_request;

/*
 * Casts text as an xs:string with that text would be cast (F&O 4.0,
 * section 19.2 "Casting from xs:string and xs:untypedAtomic"), without
 * making the string item, and writes the result, owned, to *result.
 * Returns SXML_STATUS_INVALID_ARGUMENT for ill-formed UTF-8 and the
 * failures of sxml_cast_make_value.
 */
SXML_NODISCARD sxml_status sxml_cast_make_value_from_text(
    sxml_item_heap *item_heap, const sxml_text_cast_request *request,
    sxml_sequence *result);

/* ------------------------------------------------------ Canonical text */

/*
 * Bytes of output that always suffice for the canonical text of a boolean,
 * a number held inline (integer, float, double), a date/time value and a
 * duration. Strings and decimals need no output (their text is borrowed);
 * QNames and binary values may need more.
 */
#define SXML_CAST_CANONICAL_TEXT_CAPACITY 96U

/* An atomic item whose canonical text is wanted. */
typedef struct sxml_canonical_text_request {
    sxml_item item;
    /* Where text that the item does not hold is written. */
    sxml_memory_view output;
} sxml_canonical_text_request;

/* Where the canonical text ended up. */
typedef struct sxml_canonical_text {
    /* UTF-8; borrowed from the item (strings, xs:anyURI, xs:untypedAtomic,
       decimals) or from the request's output. */
    sxml_byte_view text;
    /* The byte count of the text, also written when the output is too
       small. */
    size_t required_byte_count;
} sxml_canonical_text;

/*
 * Writes the canonical text of an atomic item: what casting it to
 * xs:string gives (F&O 4.0, section 19.1.2 "Casting to xs:string and
 * xs:untypedAtomic"). Numbers follow the XPath rules: integers and decimals
 * without exponent, xs:double and xs:float as decimals when their absolute
 * value is from 1.0E-6 up to but not including 1.0E6, otherwise with one
 * digit before the point, at least one after it and an exponent ("1.0E6"),
 * always with the fewest digits that read back as the same value; "NaN",
 * "INF", "-INF", "0" and "-0". Date/time values with "Z" for a zero
 * timezone and fraction digits without trailing zeros; durations as
 * PnYnMnDTnHnMnS without zero components ("PT0S", "P0M" when zero);
 * xs:hexBinary in upper case; xs:QName as prefix:local.
 *
 * Returns SXML_STATUS_CAPACITY_EXCEEDED when request->output is too small,
 * with text empty and required_byte_count set (the one output written on
 * failure); SXML_STATUS_INVALID_ARGUMENT when the item is not atomic; and
 * the heap's failures (the shortest digits of a double are computed with
 * the heap's decimal configuration).
 */
SXML_NODISCARD sxml_status sxml_cast_write_canonical_text(
    sxml_item_heap *item_heap, const sxml_canonical_text_request *request,
    sxml_canonical_text *canonical_text);

/* ------------------------------------------- Between src/cast*.c files */

/* An atomic target type, resolved once. */
typedef struct sxml_atomic_target {
    /* The annotation of the result. */
    sxml_type_identifier type;
    /* type, or the nearest built-in type it is derived from. */
    sxml_builtin_type builtin_type;
    sxml_builtin_type primitive_type;
} sxml_atomic_target;

/* A lexical form to turn into an item of an atomic type. */
typedef struct sxml_lexical_form_request {
    /* Well-formed UTF-8 after the whitespace processing of the target's
       whitespace facet. */
    sxml_byte_view lexical_form;
    sxml_atomic_target target;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_lexical_form_request;

/*
 * Parses request->lexical_form in the lexical space of the target (XSD 1.1
 * Part 2, section 3; F&O 4.0, section 19.2) and makes the item, owned, in
 * *item. Returns SXML_STATUS_DYNAMIC_ERROR (FORG0001, FODT0001, FODT0002,
 * FOCA0001, FOCA0003, FONS0004) with the error record filled, and the
 * heap's failures.
 */
SXML_NODISCARD sxml_status sxml_cast_make_item_from_lexical_form(
    sxml_item_heap *item_heap, const sxml_lexical_form_request *request,
    sxml_item *item);

/* ------------------------------------------------------------ Calendar */

/*
 * The calendar of the date and time types: proleptic Gregorian, with
 * year 0 for 1 BCE, a leap year (XSD 1.1 Part 2, section D.2.1; F&O 4.0,
 * fn:days-in-month). One definition for casting, arithmetic, the
 * temporal functions and date formatting.
 */
bool sxml_year_is_leap(int64_t year);

/* The days of month (1 to 12) in year: 28 to 31. */
uint8_t sxml_days_in_month(int64_t year, uint8_t month);
