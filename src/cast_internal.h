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

/*
 * Resolves an atomic type to its nearest built-in ancestor and primitive
 * type. Returns SXML_STATUS_INVALID_ARGUMENT when type is not atomic.
 */
SXML_NODISCARD sxml_status sxml_cast_resolve_atomic_target(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_atomic_target *target);

/* The whiteSpace facet (XSD 1.1 Part 2, section 4.3.6). */
typedef enum sxml_whitespace_facet {
    SXML_WHITESPACE_FACET_PRESERVE,
    SXML_WHITESPACE_FACET_REPLACE,
    SXML_WHITESPACE_FACET_COLLAPSE,
} sxml_whitespace_facet;

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

/* -------------------------------------------- Facets: src/cast_facet.c */

/*
 * The constraining facets of the types a schema registers (XSD 1.1 Part
 * 2, 4.3; include/sxml/types.h, sxml_facet; D165). Casting to such a type
 * is the whiteSpace facet, the lexical mapping of the built-in type it is
 * derived from (src/cast_lexical.c), then every facet of the type and of
 * its registered base types (src/cast_facet.c): one lexical-space engine
 * for XSD validation and XPath casting alike.
 */

/*
 * The whiteSpace facet of type, a registered atomic type: the nearest one
 * on its chain of registered base types, else builtin_facet, that of the
 * built-in type it is derived from (Part 2, 4.3.6).
 */
sxml_whitespace_facet sxml_cast_find_whitespace_facet(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_whitespace_facet builtin_facet);

/*
 * True when a value of the registered list type may have no items: no
 * built-in list type (xs:NMTOKENS, xs:IDREFS, xs:ENTITIES, minLength 1)
 * is on its chain of base types.
 */
bool sxml_cast_list_allows_no_items(const sxml_type_registry *registry,
                                    sxml_type_identifier list_type);

/* The order relation of XSD 1.1 Part 2, 2.2.3: partial for dates and
   times (3.3.6.2: a value without a timezone is compared at -14:00 and
   +14:00, and is less only when it is at both) and for durations (3.3.7.3:
   at the four reference dateTimes of E.3.3), NaN incomparable (3.3.4). */
typedef enum sxml_value_order {
    SXML_VALUE_ORDER_LESS,
    SXML_VALUE_ORDER_EQUAL,
    SXML_VALUE_ORDER_GREATER,
    SXML_VALUE_ORDER_INCOMPARABLE,
} sxml_value_order;

/*
 * Orders two atomic items of one primitive type in XSD's order and writes
 * the order of pair.first to pair.second to *order; items of different
 * type families are incomparable. Used for the bound facets here and for
 * their consistency in schema compilation (src/schema_simple_type.c).
 * Returns the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_cast_order_values(sxml_item_heap *item_heap,
                                                  sxml_item_pair pair,
                                                  sxml_value_order *order);

/* A value whose facets to check. */
typedef struct sxml_facet_check_request {
    /* A registered atomic, list or union type (from
       SXML_TYPE_FIRST_REGISTERED); built-in types have no facets here. */
    sxml_type_identifier type;
    /* The value's lexical form after whitespace processing, what pattern
       facets match (Part 2, 4.3.4.4): the text cast from, or the
       canonical text of a value cast from another type. Borrowed. */
    sxml_byte_view lexical_form;
    /* The value: one atomic item, or a list's items. Borrowed. */
    sxml_item_view items;
    sxml_error_record *error_record;
} sxml_facet_check_request;

/*
 * Checks the facets of request->type and of each registered base type
 * (facets of steps combine by conjunction, the pattern facets of one step
 * by disjunction; Part 2, 4.3.4.3). A value that fails one is FORG0001
 * (F&O 4.0, section 19.3.1), with the constraint name of the facet first
 * in the description: cvc-length-valid, cvc-minLength-valid,
 * cvc-maxLength-valid, cvc-pattern-valid, cvc-enumeration-valid,
 * cvc-maxInclusive-valid, cvc-maxExclusive-valid, cvc-minInclusive-valid,
 * cvc-minExclusive-valid, cvc-totalDigits-valid, cvc-fractionDigits-valid
 * or cvc-explicitTimezone-valid (Part 2, appendix C.1).
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR with the error record filled,
 * SXML_STATUS_INVALID_ARGUMENT for a facet value the type cannot read
 * (schema compilation checks them before it registers a type), and the
 * heap's and the regex engine's failures (limits included).
 */
SXML_NODISCARD sxml_status sxml_cast_check_facets(
    sxml_item_heap *item_heap, const sxml_facet_check_request *request);

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
