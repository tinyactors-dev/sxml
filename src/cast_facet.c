/*
 * The constraining facets of registered schema types (src/cast_internal.h,
 * "Facets"; docs/DECISIONS.md D165): the second half of casting to a type
 * a schema derived by restriction, after the lexical mapping of its
 * built-in ancestor. Facet values are text in the registry (types.h,
 * sxml_facet), canonical in the base type, so each is read here with the
 * lexical mapping of the built-in type of the value it is compared with,
 * which has no facets of its own: nothing here casts back into src/cast.c
 * (D71).
 *
 * Specification: XSD 1.1 Part 2, 4.3 "Constraining Facets" (the
 * validation rules cvc-*-valid of each facet) and appendix C.1; the order
 * relations of 3.3.6.2 (dates, the partial order with and without a
 * timezone), 3.3.7 (durations, by the four dateTimes of E.3.3), and
 * float and double, where NaN is incomparable (3.3.4.1).
 *
 * Golden file: src/cast.c (a cast step with static errors).
 */
#include "cast_internal.h"

#include "regex_internal.h"
#include "sxml/string.h"

#include <math.h>
#include <string.h>

/* An answer of a facet test: the value satisfies it, equals the facet
   value, or matches the pattern. */
typedef struct sxml_facet_answer {
    bool holds;
} sxml_facet_answer;

/* --------------------------------------------------------------- Errors */

#define SXML_FACET_ERROR(constraint_text)                                     \
    {                                                                          \
        .error_status = SXML_STATUS_DYNAMIC_ERROR,                             \
        .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0001"),             \
        .description = SXML_BYTE_VIEW_INITIALIZER(constraint_text),            \
    }

/* One error per facet kind, in the order of sxml_facet_kind. */
static const sxml_static_error sxml_facet_errors[] = {
    SXML_FACET_ERROR("cvc-length-valid: the value's length is not the length "
                     "facet (XSD 1.1 Part 2, 4.3.1.4)"),
    SXML_FACET_ERROR("cvc-minLength-valid: the value is shorter than the "
                     "minLength facet (XSD 1.1 Part 2, 4.3.2.4)"),
    SXML_FACET_ERROR("cvc-maxLength-valid: the value is longer than the "
                     "maxLength facet (XSD 1.1 Part 2, 4.3.3.4)"),
    SXML_FACET_ERROR("cvc-pattern-valid: the value matches no pattern facet "
                     "(XSD 1.1 Part 2, 4.3.4.4)"),
    SXML_FACET_ERROR("cvc-enumeration-valid: the value is not one of the "
                     "enumeration facets (XSD 1.1 Part 2, 4.3.5.4)"),
    SXML_FACET_ERROR("cvc-whiteSpace-valid (XSD 1.1 Part 2, 4.3.6)"),
    SXML_FACET_ERROR("cvc-maxInclusive-valid: the value is above the "
                     "maxInclusive facet (XSD 1.1 Part 2, 4.3.7.4)"),
    SXML_FACET_ERROR("cvc-maxExclusive-valid: the value is not below the "
                     "maxExclusive facet (XSD 1.1 Part 2, 4.3.8.4)"),
    SXML_FACET_ERROR("cvc-minExclusive-valid: the value is not above the "
                     "minExclusive facet (XSD 1.1 Part 2, 4.3.9.4)"),
    SXML_FACET_ERROR("cvc-minInclusive-valid: the value is below the "
                     "minInclusive facet (XSD 1.1 Part 2, 4.3.10.4)"),
    SXML_FACET_ERROR("cvc-totalDigits-valid: the value has more digits than "
                     "the totalDigits facet (XSD 1.1 Part 2, 4.3.11.4)"),
    SXML_FACET_ERROR("cvc-fractionDigits-valid: the value has more fraction "
                     "digits than the fractionDigits facet (XSD 1.1 Part 2, "
                     "4.3.12.4)"),
    SXML_FACET_ERROR("cvc-explicitTimezone-valid: the value's timezone is not "
                     "what the explicitTimezone facet requires (XSD 1.1 Part "
                     "2, 4.3.14.4)"),
};

static sxml_status sxml_facet_fail(const sxml_facet_check_request *request,
                                   sxml_facet_kind facet_kind) {
    return sxml_error_record_report(request->error_record,
                                    &sxml_facet_errors[facet_kind]);
}

/* More registered base type steps than any schema derives: a bound on a
   broken registry. */
enum { SXML_FACET_MAXIMUM_DERIVATION_STEP_COUNT = 1024 };

/* ----------------------------------------------------------- Whitespace */

static bool sxml_facet_value_is(sxml_byte_view value, sxml_byte_view text) {
    return sxml_byte_view_equals(value, text);
}

sxml_whitespace_facet sxml_cast_find_whitespace_facet(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_whitespace_facet builtin_facet) {
    sxml_type_identifier current = type;
    for (size_t step_index = 0;
         step_index < SXML_FACET_MAXIMUM_DERIVATION_STEP_COUNT &&
         current.index >= SXML_TYPE_FIRST_REGISTERED;
         step_index += 1) {
        sxml_type_description description;
        if (sxml_type_registry_describe_type(registry, current, &description) !=
            SXML_STATUS_OK) {
            break;
        }
        for (size_t facet_index = 0;
             facet_index < description.facets.facet_count; facet_index += 1) {
            const sxml_facet *facet = &description.facets.facets[facet_index];
            if (facet->facet_kind != SXML_FACET_KIND_WHITESPACE) {
                continue;
            }
            if (sxml_facet_value_is(facet->value, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
                return SXML_WHITESPACE_FACET_PRESERVE;
            }
            return sxml_facet_value_is(facet->value, SXML_BYTE_VIEW_FROM_LITERAL("replace"))
                       ? SXML_WHITESPACE_FACET_REPLACE
                       : SXML_WHITESPACE_FACET_COLLAPSE;
        }
        current = description.base_type;
    }
    return builtin_facet;
}

bool sxml_cast_list_allows_no_items(const sxml_type_registry *registry,
                                    sxml_type_identifier list_type) {
    sxml_type_identifier current = list_type;
    for (size_t step_index = 0;
         step_index < SXML_FACET_MAXIMUM_DERIVATION_STEP_COUNT &&
         current.index >= SXML_TYPE_FIRST_REGISTERED;
         step_index += 1) {
        sxml_type_description description;
        if (sxml_type_registry_describe_type(registry, current, &description) !=
            SXML_STATUS_OK) {
            return false;
        }
        current = description.base_type;
    }
    sxml_type_description builtin_description;
    return sxml_type_registry_describe_type(registry, current,
                                            &builtin_description) ==
               SXML_STATUS_OK &&
           builtin_description.variety != SXML_TYPE_VARIETY_LIST;
}

/* --------------------------------------------------------- Facet values */

/* Reads a non-negative integer facet value (length, the digit facets);
   false when it is none. */
static bool sxml_facet_read_count(sxml_byte_view value, uint64_t *count) {
    uint64_t number = 0;
    if (value.byte_count == 0) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < value.byte_count;
         byte_index += 1) {
        uint8_t byte = value.bytes[byte_index];
        if (byte < '0' || byte > '9') {
            return false;
        }
        if (number > (UINT64_MAX - 9U) / 10U) {
            number = UINT64_MAX;
        } else {
            number = number * 10U + (uint64_t)(byte - '0');
        }
    }
    *count = number;
    return true;
}

/*
 * Reads a facet value with the lexical mapping of the built-in type of
 * item (whose annotation may be a registered type), as an item of that
 * built-in type, owned. SXML_STATUS_INVALID_ARGUMENT when it does not
 * read.
 */
typedef struct sxml_facet_value_request {
    sxml_item item;
    sxml_byte_view value;
} sxml_facet_value_request;

static sxml_status sxml_facet_read_value(sxml_item_heap *item_heap,
                                         sxml_facet_value_request request,
                                         sxml_item *value_item) {
    sxml_atomic_target target;
    sxml_status status = sxml_cast_resolve_atomic_target(
        sxml_item_heap_get_type_registry(item_heap),
        sxml_item_get_type_annotation(request.item), &target);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    target.type = sxml_builtin_type_identifier(target.builtin_type);
    sxml_error_record ignored;
    status = sxml_cast_make_item_from_lexical_form(
        item_heap,
        &(sxml_lexical_form_request){.lexical_form = request.value,
                                     .target = target,
                                     .namespace_bindings =
                                         {.namespace_bindings = NULL,
                                          .namespace_binding_count = 0},
                                     .error_record = &ignored},
        value_item);
    return status == SXML_STATUS_DYNAMIC_ERROR ? SXML_STATUS_INVALID_ARGUMENT
                                               : status;
}

/* ----------------------------------------------------------------- Order */

static sxml_value_order sxml_value_order_of(sxml_ordering ordering) {
    switch (ordering) {
    case SXML_ORDERING_LESS:
        return SXML_VALUE_ORDER_LESS;
    case SXML_ORDERING_EQUAL:
        return SXML_VALUE_ORDER_EQUAL;
    case SXML_ORDERING_GREATER:
        return SXML_VALUE_ORDER_GREATER;
    }
    return SXML_VALUE_ORDER_INCOMPARABLE;
}

static bool sxml_item_is_nan(sxml_item item) {
    sxml_item_representation representation = sxml_item_get_representation(item);
    if (representation == SXML_ITEM_REPRESENTATION_FLOAT) {
        return isnan(item.payload.float_number);
    }
    return representation == SXML_ITEM_REPRESENTATION_DOUBLE &&
           isnan(item.payload.double_number);
}

/* Days from 1970-01-01 to year-month-01 of the proleptic Gregorian
   calendar (H. Hinnant's days_from_civil). */
static int64_t sxml_days_to_month_start(int64_t year, int64_t month) {
    int64_t shifted_year = month <= 2 ? year - 1 : year;
    int64_t era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
    int64_t year_of_era = shifted_year - era * 400;
    int64_t shifted_month = month > 2 ? month - 3 : month + 9;
    int64_t day_of_year = (153 * shifted_month + 2) / 5;
    int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/* A point of the time line: seconds and nanoseconds. */
typedef struct sxml_facet_instant {
    int64_t seconds;
    int32_t nanoseconds;
} sxml_facet_instant;

/* The reference dateTime (year, month, the 1st, 00:00:00) plus duration
   (XSD 1.1 Part 2, E.3.3: months first, then seconds). */
typedef struct sxml_facet_reference_date {
    int64_t year;
    int64_t month;
} sxml_facet_reference_date;

static sxml_facet_instant
sxml_facet_add_duration(sxml_facet_reference_date reference,
                        const sxml_duration *duration) {
    int64_t month_count = reference.year * 12 + (reference.month - 1) +
                          duration->months;
    int64_t year = month_count >= 0 ? month_count / 12
                                    : -((11 - month_count) / 12);
    int64_t month = month_count - year * 12 + 1;
    return (sxml_facet_instant){
        .seconds = sxml_days_to_month_start(year, month) * 86400 +
                   duration->seconds,
        .nanoseconds = duration->nanoseconds};
}

static sxml_value_order sxml_facet_instants_order(sxml_facet_instant first,
                                                  sxml_facet_instant second) {
    if (first.seconds != second.seconds) {
        return first.seconds < second.seconds ? SXML_VALUE_ORDER_LESS
                                              : SXML_VALUE_ORDER_GREATER;
    }
    if (first.nanoseconds != second.nanoseconds) {
        return first.nanoseconds < second.nanoseconds ? SXML_VALUE_ORDER_LESS
                                                      : SXML_VALUE_ORDER_GREATER;
    }
    return SXML_VALUE_ORDER_EQUAL;
}

/* Beyond these the reference dates would overflow; such durations order
   by months, then seconds. */
#define SXML_FACET_LARGEST_MONTHS INT64_C(1000000000000)
#define SXML_FACET_LARGEST_SECONDS INT64_C(100000000000000000)

static bool sxml_duration_is_huge(const sxml_duration *duration) {
    return duration->months > SXML_FACET_LARGEST_MONTHS ||
           duration->months < -SXML_FACET_LARGEST_MONTHS ||
           duration->seconds > SXML_FACET_LARGEST_SECONDS ||
           duration->seconds < -SXML_FACET_LARGEST_SECONDS;
}

/* XSD 1.1 Part 2, 3.3.7.3 and E.3.3: one duration is less than another
   when it is at each of the four reference dateTimes. */
static sxml_value_order sxml_durations_order(const sxml_duration *first,
                                             const sxml_duration *second) {
    static const sxml_facet_reference_date references[] = {
        {.year = 1696, .month = 9},
        {.year = 1697, .month = 2},
        {.year = 1903, .month = 3},
        {.year = 1903, .month = 7},
    };
    if (sxml_duration_is_huge(first) || sxml_duration_is_huge(second)) {
        if (first->months != second->months) {
            return first->months < second->months ? SXML_VALUE_ORDER_LESS
                                                  : SXML_VALUE_ORDER_GREATER;
        }
        return sxml_facet_instants_order(
            (sxml_facet_instant){.seconds = first->seconds,
                                 .nanoseconds = first->nanoseconds},
            (sxml_facet_instant){.seconds = second->seconds,
                                 .nanoseconds = second->nanoseconds});
    }
    sxml_value_order order = SXML_VALUE_ORDER_EQUAL;
    for (size_t reference_index = 0;
         reference_index < sizeof(references) / sizeof(references[0]);
         reference_index += 1) {
        sxml_value_order reference_order = sxml_facet_instants_order(
            sxml_facet_add_duration(references[reference_index], first),
            sxml_facet_add_duration(references[reference_index], second));
        if (reference_index != 0 && reference_order != order) {
            return SXML_VALUE_ORDER_INCOMPARABLE;
        }
        order = reference_order;
    }
    return order;
}

sxml_status sxml_cast_order_values(sxml_item_heap *item_heap,
                                   sxml_item_pair pair,
                                   sxml_value_order *order) {
    if (sxml_item_is_nan(pair.first) || sxml_item_is_nan(pair.second)) {
        *order = SXML_VALUE_ORDER_INCOMPARABLE;
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum first;
    sxml_atomic_datum second;
    sxml_status status = sxml_atomic_item_get_datum(pair.first, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(pair.second, &second);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first.datum_kind == SXML_DATUM_KIND_DURATION &&
        second.datum_kind == SXML_DATUM_KIND_DURATION) {
        *order = sxml_durations_order(&first.datum_details.duration,
                                      &second.datum_details.duration);
        return SXML_STATUS_OK;
    }
    /* Dates and times: a value without a timezone is compared at -14:00
       and +14:00; it is less when it is in both (3.3.6.2). */
    int16_t first_offset = 0;
    int16_t second_offset = 0;
    if (first.datum_kind == SXML_DATUM_KIND_DATE_TIME &&
        second.datum_kind == SXML_DATUM_KIND_DATE_TIME &&
        first.datum_details.date_time.has_timezone !=
            second.datum_details.date_time.has_timezone) {
        first_offset = -840;
        second_offset = 840;
    }
    sxml_ordering first_ordering = SXML_ORDERING_EQUAL;
    sxml_ordering second_ordering = SXML_ORDERING_EQUAL;
    sxml_error_record ignored;
    status = sxml_atomic_item_compare_in_heap(
        item_heap,
        &(sxml_atomic_comparison){.first = pair.first,
                                  .second = pair.second,
                                  .implicit_timezone_offset_minutes =
                                      first_offset,
                                  .error_record = &ignored,
                                  .name_pool = NULL},
        &first_ordering);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_compare_in_heap(
            item_heap,
            &(sxml_atomic_comparison){.first = pair.first,
                                      .second = pair.second,
                                      .implicit_timezone_offset_minutes =
                                          second_offset,
                                      .error_record = &ignored,
                                      .name_pool = NULL},
            &second_ordering);
    }
    if (status == SXML_STATUS_TYPE_ERROR) {
        *order = SXML_VALUE_ORDER_INCOMPARABLE;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *order = first_ordering == second_ordering
                 ? sxml_value_order_of(first_ordering)
                 : SXML_VALUE_ORDER_INCOMPARABLE;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Equality */

/*
 * Whether item equals the facet value (Part 2, 4.3.5.4, enumeration):
 * the value read in item's built-in type and compared by atomic-equal,
 * which is XSD's equality within a primitive type (NaN equal to itself);
 * QNames and NOTATIONs by their EQName (types.h writes them so). A value
 * that does not read in the item's type is not equal.
 */
static sxml_status sxml_facet_qname_equals(sxml_item_heap *item_heap,
                                           sxml_facet_value_request request,
                                           sxml_facet_answer *answer) {
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(request.item, &datum);
    sxml_name_parts parts;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(
            name_pool, datum.datum_details.qname.expanded_name, &parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            name_pool, parts.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Q{uri}local */
    sxml_byte_view value = request.value;
    size_t expected_count =
        3 + namespace_uri.byte_count + parts.local_name.byte_count;
    answer->holds =
        value.byte_count == expected_count && value.bytes[0] == 'Q' &&
        value.bytes[1] == '{' &&
        (namespace_uri.byte_count == 0 ||
         memcmp(value.bytes + 2, namespace_uri.bytes,
                namespace_uri.byte_count) == 0) &&
        value.bytes[2 + namespace_uri.byte_count] == '}' &&
        (parts.local_name.byte_count == 0 ||
         memcmp(value.bytes + 3 + namespace_uri.byte_count,
                parts.local_name.bytes, parts.local_name.byte_count) == 0);
    return SXML_STATUS_OK;
}

static sxml_status sxml_facet_value_equals(sxml_item_heap *item_heap,
                                           sxml_facet_value_request request,
                                           sxml_facet_answer *answer) {
    sxml_builtin_type primitive_type =
        (sxml_builtin_type)sxml_atomic_item_get_primitive_type(request.item)
            .index;
    if (primitive_type == SXML_TYPE_XS_QNAME ||
        primitive_type == SXML_TYPE_XS_NOTATION) {
        return sxml_facet_qname_equals(item_heap, request, answer);
    }
    sxml_item value_item = {.representation = 0};
    sxml_status status =
        sxml_facet_read_value(item_heap, request, &value_item);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        answer->holds = false;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    answer->holds = sxml_atomic_item_is_atomic_equal(request.item, value_item);
    sxml_item_release(value_item);
    return SXML_STATUS_OK;
}

/* Whether the items equal the facet value: one item, or a list whose
   tokens are its items' values in order. */
typedef struct sxml_facet_items_request {
    sxml_item_view items;
    sxml_byte_view value;
    bool is_list;
} sxml_facet_items_request;

static sxml_status sxml_facet_items_equal(sxml_item_heap *item_heap,
                                          sxml_facet_items_request request,
                                          sxml_facet_answer *answer) {
    if (!request.is_list) {
        return sxml_facet_value_equals(
            item_heap,
            (sxml_facet_value_request){.item = request.items.items[0],
                                       .value = request.value},
            answer);
    }
    sxml_byte_view text = request.value;
    size_t offset = 0;
    answer->holds = true;
    for (size_t item_index = 0; answer->holds && item_index <= request.items.item_count;
         item_index += 1) {
        while (offset < text.byte_count && text.bytes[offset] == ' ') {
            offset += 1;
        }
        size_t token_end = offset;
        while (token_end < text.byte_count && text.bytes[token_end] != ' ') {
            token_end += 1;
        }
        if (item_index == request.items.item_count) {
            /* No token may be left over. */
            answer->holds = token_end == offset;
            break;
        }
        if (token_end == offset) {
            answer->holds = false;
            break;
        }
        sxml_status status = sxml_facet_value_equals(
            item_heap,
            (sxml_facet_value_request){
                .item = request.items.items[item_index],
                .value = {.bytes = text.bytes + offset,
                          .byte_count = token_end - offset}},
            answer);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        offset = token_end;
    }
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------- One facet */

/* The length of a value in the facets' units (Part 2, 4.3.1.3):
   characters of strings and URIs, octets of binary values, items of a
   list; false for the types whose length facets always hold (QName,
   NOTATION) or that have none. */
typedef struct sxml_facet_subject {
    sxml_item_view items;
    bool is_list;
    sxml_byte_view lexical_form;
} sxml_facet_subject;

static bool sxml_facet_value_length(sxml_facet_subject subject,
                                    uint64_t *length) {
    if (subject.is_list) {
        *length = subject.items.item_count;
        return true;
    }
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(subject.items.items[0], &datum) !=
        SXML_STATUS_OK) {
        return false;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
        *length = datum.datum_details.string.codepoint_count;
        return true;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_BINARY) {
        *length = datum.datum_details.binary.byte_count;
        return true;
    }
    return false;
}

/* The digits of a decimal value (Part 2, 4.3.11.3 and 4.3.12.3): in the
   canonical text, leading zeros of the integer part and trailing zeros
   of the fraction left out. */
typedef struct sxml_facet_digits {
    uint64_t total_digit_count;
    uint64_t fraction_digit_count;
} sxml_facet_digits;

static bool sxml_facet_count_digits(sxml_item_heap *item_heap, sxml_item item,
                                    sxml_facet_digits *digits) {
    uint8_t bytes[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text canonical;
    if (sxml_atomic_item_get_primitive_type(item).index !=
            SXML_TYPE_XS_DECIMAL ||
        sxml_cast_write_canonical_text(
            item_heap,
            &(sxml_canonical_text_request){
                .item = item,
                .output = {.bytes = bytes, .byte_count = sizeof(bytes)}},
            &canonical) != SXML_STATUS_OK) {
        return false;
    }
    sxml_byte_view text = canonical.text;
    size_t start = 0;
    if (start < text.byte_count && text.bytes[start] == '-') {
        start += 1;
    }
    while (start < text.byte_count && text.bytes[start] == '0') {
        start += 1;
    }
    size_t point = start;
    while (point < text.byte_count && text.bytes[point] != '.') {
        point += 1;
    }
    size_t end = text.byte_count;
    if (point < end) {
        while (end > point + 1 && text.bytes[end - 1] == '0') {
            end -= 1;
        }
    }
    uint64_t fraction_count = point < end ? end - point - 1 : 0;
    *digits = (sxml_facet_digits){
        .total_digit_count = (point - start) + fraction_count,
        .fraction_digit_count = fraction_count};
    return true;
}

/* Whether the value matches one of the patterns: a regex of the XSD
   dialect (D53), compiled in heap memory for the check. */
typedef struct sxml_pattern_request {
    sxml_byte_view lexical_form;
    sxml_byte_view pattern;
} sxml_pattern_request;

static sxml_status sxml_facet_match_pattern(sxml_item_heap *item_heap,
                                            sxml_pattern_request request,
                                            sxml_facet_answer *answer) {
    sxml_string pattern_text;
    sxml_string input;
    if (sxml_string_from_utf8(request.pattern, &pattern_text) != SXML_STATUS_OK ||
        sxml_string_from_utf8(request.lexical_form, &input) != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_allocator allocator = sxml_item_heap_as_allocator(item_heap);
    sxml_regex *regex = NULL;
    sxml_regex_matcher *matcher = NULL;
    sxml_error_record ignored;
    sxml_status status = sxml_regex_create(
        allocator,
        &(sxml_regex_configuration){
            .regex_text = pattern_text,
            .flags = {.is_xsd_pattern = true},
            .maximum_instruction_count = 0,
            .maximum_group_depth = 0,
            .stack_allocator = {.state = NULL, .operations = NULL},
            .error_record = &ignored},
        &regex);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_create(
            allocator,
            &(sxml_regex_matcher_configuration){
                .regex = regex,
                .backtrack_allocator = allocator,
                .initial_backtrack_entry_count = 0,
                .maximum_backtrack_byte_count = 0,
                .maximum_step_count = 0},
            &matcher);
    }
    sxml_regex_search_result result = {.is_found = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_regex_matcher_match_at(
            matcher,
            &(sxml_regex_search_request){.regex_input = input,
                                         .start_byte_offset = 0},
            &result);
    }
    sxml_regex_matcher_destroy(matcher);
    sxml_regex_destroy(regex);
    if (status == SXML_STATUS_OK) {
        answer->holds = result.is_found;
    }
    return status;
}

/* Checks one facet other than pattern, enumeration and whiteSpace; true
   in answer->holds when the value satisfies it. */
typedef struct sxml_facet_test {
    sxml_facet_subject subject;
    const sxml_facet *facet;
} sxml_facet_test;

static sxml_status sxml_facet_check_one(sxml_item_heap *item_heap, sxml_facet_test test,
                                        sxml_facet_answer *answer) {
    sxml_facet_subject subject = test.subject;
    const sxml_facet *facet = test.facet;
    answer->holds = true;
    uint64_t count = 0;
    switch (facet->facet_kind) {
    case SXML_FACET_KIND_LENGTH:
    case SXML_FACET_KIND_MIN_LENGTH:
    case SXML_FACET_KIND_MAX_LENGTH: {
        uint64_t length = 0;
        if (!sxml_facet_read_count(facet->value, &count)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (!sxml_facet_value_length(subject, &length)) {
            return SXML_STATUS_OK;
        }
        if (facet->facet_kind == SXML_FACET_KIND_LENGTH) {
            answer->holds = length == count;
        } else if (facet->facet_kind == SXML_FACET_KIND_MIN_LENGTH) {
            answer->holds = length >= count;
        } else {
            answer->holds = length <= count;
        }
        return SXML_STATUS_OK;
    }
    case SXML_FACET_KIND_TOTAL_DIGITS:
    case SXML_FACET_KIND_FRACTION_DIGITS: {
        sxml_facet_digits digits;
        if (!sxml_facet_read_count(facet->value, &count)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (subject.is_list ||
            !sxml_facet_count_digits(item_heap, subject.items.items[0],
                                     &digits)) {
            return SXML_STATUS_OK;
        }
        answer->holds = facet->facet_kind == SXML_FACET_KIND_TOTAL_DIGITS
                     ? digits.total_digit_count <= count
                     : digits.fraction_digit_count <= count;
        return SXML_STATUS_OK;
    }
    case SXML_FACET_KIND_EXPLICIT_TIMEZONE: {
        sxml_atomic_datum datum;
        if (subject.is_list ||
            sxml_atomic_item_get_datum(subject.items.items[0], &datum) !=
                SXML_STATUS_OK ||
            datum.datum_kind != SXML_DATUM_KIND_DATE_TIME) {
            return SXML_STATUS_OK;
        }
        bool has_timezone = datum.datum_details.date_time.has_timezone;
        if (sxml_facet_value_is(facet->value, SXML_BYTE_VIEW_FROM_LITERAL("required"))) {
            answer->holds = has_timezone;
        } else if (sxml_facet_value_is(facet->value, SXML_BYTE_VIEW_FROM_LITERAL("prohibited"))) {
            answer->holds = !has_timezone;
        }
        return SXML_STATUS_OK;
    }
    case SXML_FACET_KIND_MAX_INCLUSIVE:
    case SXML_FACET_KIND_MAX_EXCLUSIVE:
    case SXML_FACET_KIND_MIN_EXCLUSIVE:
    case SXML_FACET_KIND_MIN_INCLUSIVE: {
        if (subject.is_list) {
            return SXML_STATUS_OK;
        }
        sxml_item bound = {.representation = 0};
        sxml_status status = sxml_facet_read_value(
            item_heap,
            (sxml_facet_value_request){.item = subject.items.items[0],
                                       .value = facet->value},
            &bound);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_value_order order = SXML_VALUE_ORDER_INCOMPARABLE;
        status = sxml_cast_order_values(
            item_heap,
            (sxml_item_pair){.first = subject.items.items[0], .second = bound},
            &order);
        sxml_item_release(bound);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        bool is_less = order == SXML_VALUE_ORDER_LESS;
        bool is_equal = order == SXML_VALUE_ORDER_EQUAL;
        bool is_greater = order == SXML_VALUE_ORDER_GREATER;
        if (facet->facet_kind == SXML_FACET_KIND_MAX_INCLUSIVE) {
            answer->holds = is_less || is_equal;
        } else if (facet->facet_kind == SXML_FACET_KIND_MAX_EXCLUSIVE) {
            answer->holds = is_less;
        } else if (facet->facet_kind == SXML_FACET_KIND_MIN_EXCLUSIVE) {
            answer->holds = is_greater;
        } else {
            answer->holds = is_greater || is_equal;
        }
        return SXML_STATUS_OK;
    }
    case SXML_FACET_KIND_PATTERN:
    case SXML_FACET_KIND_ENUMERATION:
    case SXML_FACET_KIND_WHITESPACE:
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* What the pattern and enumeration facets of one step found: each kind
   holds when the step has none of it or one of its values holds. */
typedef struct sxml_facet_step_alternatives {
    bool has_pattern;
    bool is_pattern_matched;
    bool has_enumeration;
    bool is_enumeration_matched;
} sxml_facet_step_alternatives;

/* Checks the facets of one restriction step. */
static sxml_status sxml_facet_check_step(sxml_item_heap *item_heap,
                                         const sxml_facet_check_request *request,
                                         const sxml_type_description *step) {
    sxml_facet_subject subject = {
        .items = request->items,
        .is_list = step->variety == SXML_TYPE_VARIETY_LIST,
        .lexical_form = request->lexical_form};
    sxml_facet_step_alternatives alternatives = {.has_pattern = false};
    for (size_t facet_index = 0; facet_index < step->facets.facet_count;
         facet_index += 1) {
        const sxml_facet *facet = &step->facets.facets[facet_index];
        sxml_status status = SXML_STATUS_OK;
        sxml_facet_answer answer = {.holds = true};
        if (facet->facet_kind == SXML_FACET_KIND_PATTERN) {
            alternatives.has_pattern = true;
            if (!alternatives.is_pattern_matched) {
                status = sxml_facet_match_pattern(
                    item_heap,
                    (sxml_pattern_request){.lexical_form = subject.lexical_form,
                                           .pattern = facet->value},
                    &answer);
                alternatives.is_pattern_matched = status == SXML_STATUS_OK &&
                                                  answer.holds;
            }
        } else if (facet->facet_kind == SXML_FACET_KIND_ENUMERATION) {
            alternatives.has_enumeration = true;
            if (!alternatives.is_enumeration_matched &&
                request->items.item_count != 0) {
                status = sxml_facet_items_equal(
                    item_heap,
                    (sxml_facet_items_request){.items = request->items,
                                               .value = facet->value,
                                               .is_list = subject.is_list},
                    &answer);
                alternatives.is_enumeration_matched =
                    status == SXML_STATUS_OK && answer.holds;
            } else if (request->items.item_count == 0 &&
                       facet->value.byte_count == 0) {
                /* The empty list equals an empty enumeration value. */
                alternatives.is_enumeration_matched = true;
            }
        } else if (subject.is_list || request->items.item_count != 0) {
            status = sxml_facet_check_one(
                item_heap, (sxml_facet_test){.subject = subject, .facet = facet}, &answer);
            if (status == SXML_STATUS_OK && !answer.holds) {
                return sxml_facet_fail(request, facet->facet_kind);
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (alternatives.has_pattern && !alternatives.is_pattern_matched) {
        return sxml_facet_fail(request, SXML_FACET_KIND_PATTERN);
    }
    if (alternatives.has_enumeration && !alternatives.is_enumeration_matched) {
        return sxml_facet_fail(request, SXML_FACET_KIND_ENUMERATION);
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_cast_check_facets(sxml_item_heap *item_heap,
                                   const sxml_facet_check_request *request) {
    if (item_heap == NULL || request == NULL ||
        (request->items.item_count != 0 && request->items.items == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(item_heap);
    sxml_type_identifier current = request->type;
    for (size_t step_index = 0; current.index >= SXML_TYPE_FIRST_REGISTERED;
         step_index += 1) {
        sxml_type_description step;
        if (step_index == SXML_FACET_MAXIMUM_DERIVATION_STEP_COUNT ||
            sxml_type_registry_describe_type(registry, current, &step) !=
                SXML_STATUS_OK) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_status status = sxml_facet_check_step(item_heap, request, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        current = step.base_type;
    }
    return SXML_STATUS_OK;
}
