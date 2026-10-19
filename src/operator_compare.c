/*
 * Value, general and GNode comparisons (src/operator_internal.h).
 *
 * Golden file: src/memory.c (and src/operator.c).
 *
 * Both come down to fn:compare (sxml_atomic_item_compare) on two atomic
 * items, which already orders every type family as XPath 4.0 requires
 * (PR2256) and compares numbers of different types by exact value
 * (PR2218, D57); NaN is the only number handled here. General comparisons
 * add the casting of xs:untypedAtomic and the search over pairs.
 *
 * Specification: XPath 4.0, section 4.11.1 "Value Comparisons", section
 * 4.11.2 "General Comparisons" and section 4.11.3 "GNode Comparisons";
 * F&O 4.0, fn:compare; XDM 4.0, sections 6.1 and 6.2 (identity, document
 * order).
 */
#include "operator_internal.h"

#include "collation_internal.h"

#include <math.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error sxml_comparison_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the compared items belong to different type families (XPath 4.0, "
        "section 4.11.1; F&O 4.0, fn:compare)"),
};

static const sxml_static_error sxml_node_comparison_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an operand of a GNode comparison must be one GNode or the empty "
        "sequence (XPath 4.0, section 4.11.3)"),
};

/* ------------------------------------------------------ One item pair */

/* True for xs:float and xs:double NaN. */
static bool sxml_item_is_not_a_number(sxml_item item) {
    if (item.representation == SXML_ITEM_REPRESENTATION_DOUBLE) {
        return isnan(item.payload.double_number);
    }
    if (item.representation == SXML_ITEM_REPRESENTATION_FLOAT) {
        return isnan(item.payload.float_number);
    }
    return false;
}

/* Whether an ordering from fn:compare satisfies the operator (the table
   "Result of a Value Comparison", XPath 4.0, section 4.11.1). */
static bool
sxml_ordering_satisfies(sxml_comparison_operator comparison_operator,
                        sxml_ordering ordering) {
    switch (comparison_operator) {
    case SXML_COMPARISON_OPERATOR_EQUAL:
        return ordering == SXML_ORDERING_EQUAL;
    case SXML_COMPARISON_OPERATOR_NOT_EQUAL:
        return ordering != SXML_ORDERING_EQUAL;
    case SXML_COMPARISON_OPERATOR_LESS:
        return ordering == SXML_ORDERING_LESS;
    case SXML_COMPARISON_OPERATOR_LESS_OR_EQUAL:
        return ordering != SXML_ORDERING_GREATER;
    case SXML_COMPARISON_OPERATOR_GREATER:
        return ordering == SXML_ORDERING_GREATER;
    case SXML_COMPARISON_OPERATOR_GREATER_OR_EQUAL:
        return ordering != SXML_ORDERING_LESS;
    }
    return false;
}

/* Two atomic items to compare as a value comparison does. */
typedef struct sxml_item_comparison {
    sxml_comparison_operator comparison_operator;
    sxml_item_pair pair;
    int16_t implicit_timezone_offset_minutes;
    sxml_error_record *error_record;
    /* NULL for the codepoint collation. */
    const sxml_collation *collation;
} sxml_item_comparison;

/* Orders two strings under a collation other than the codepoint one
   (F&O 4.0, fn:compare); *is_compared is false when an item is not a
   string. */
typedef struct sxml_collated_comparison {
    sxml_item_heap *item_heap;
    const sxml_item_comparison *comparison;
} sxml_collated_comparison;

static sxml_status sxml_items_compare_collated(sxml_collated_comparison collated,
                                               bool *is_compared,
                                               sxml_ordering *ordering) {
    *is_compared = false;
    sxml_atomic_datum first;
    sxml_atomic_datum second;
    if (sxml_atomic_item_get_datum(collated.comparison->pair.first, &first) !=
            SXML_STATUS_OK ||
        sxml_atomic_item_get_datum(collated.comparison->pair.second, &second) !=
            SXML_STATUS_OK ||
        first.datum_kind != SXML_DATUM_KIND_STRING ||
        second.datum_kind != SXML_DATUM_KIND_STRING) {
        return SXML_STATUS_OK;
    }
    *is_compared = true;
    return sxml_collation_compare(
        &(sxml_collation_comparison){
            .collation = collated.comparison->collation,
            .allocator = sxml_item_heap_as_allocator(collated.item_heap),
            .first = first.datum_details.string,
            .second = second.datum_details.string},
        ordering);
}

/* The outcome of comparing one pair. */
typedef struct sxml_comparison_outcome {
    bool is_true;
} sxml_comparison_outcome;

static sxml_status sxml_items_compare(sxml_item_heap *item_heap,
                                      const sxml_item_comparison *comparison,
                                      sxml_comparison_outcome *outcome) {
    sxml_item first = comparison->pair.first;
    sxml_item second = comparison->pair.second;
    if (sxml_atomic_item_is_numeric(first) &&
        sxml_atomic_item_is_numeric(second) &&
        (sxml_item_is_not_a_number(first) ||
         sxml_item_is_not_a_number(second))) {
        /* NaN: false, but true for ne (XPath 4.0, section 4.11.1). */
        outcome->is_true = comparison->comparison_operator ==
                           SXML_COMPARISON_OPERATOR_NOT_EQUAL;
        return SXML_STATUS_OK;
    }
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    if (comparison->collation != NULL) {
        bool is_compared = false;
        const sxml_status collated_status = sxml_items_compare_collated(
            (sxml_collated_comparison){.item_heap = item_heap,
                                       .comparison = comparison},
            &is_compared, &ordering);
        if (collated_status != SXML_STATUS_OK) {
            return collated_status;
        }
        if (is_compared) {
            outcome->is_true = sxml_ordering_satisfies(
                comparison->comparison_operator, ordering);
            return SXML_STATUS_OK;
        }
    }
    sxml_status status = sxml_atomic_item_compare(
        &(sxml_atomic_comparison){
            .first = first,
            .second = second,
            .implicit_timezone_offset_minutes =
                comparison->implicit_timezone_offset_minutes,
            .error_record = NULL,
            .name_pool = sxml_item_heap_get_name_pool(item_heap)},
        &ordering);
    if (status == SXML_STATUS_TYPE_ERROR) {
        return sxml_error_record_report(comparison->error_record,
                                        &sxml_comparison_type_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    outcome->is_true =
        sxml_ordering_satisfies(comparison->comparison_operator, ordering);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Value comparison */

sxml_status
sxml_operator_make_value_comparison(sxml_item_heap *item_heap,
                                    const sxml_comparison_request *request,
                                    sxml_sequence *result) {
    sxml_single_operand left_operand = {.has_item = false,
                                        .item = {.representation = 0}};
    sxml_status status = sxml_operator_read_single_operand(
        request->left_operand, request->error_record, &left_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!left_operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_single_operand right_operand = {.has_item = false,
                                         .item = {.representation = 0}};
    status = sxml_operator_read_single_operand(
        request->right_operand, request->error_record, &right_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!right_operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_comparison_outcome outcome = {.is_true = false};
    status = sxml_items_compare(
        item_heap,
        &(sxml_item_comparison){
            .comparison_operator = request->comparison_operator,
            .pair = {.first = left_operand.item, .second = right_operand.item},
            .implicit_timezone_offset_minutes =
                request->implicit_timezone_offset_minutes,
            .error_record = request->error_record,
            .collation = request->collation},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = sxml_sequence_from_item(
        outcome.is_true ? sxml_atomic_item_true() : sxml_atomic_item_false());
    return SXML_STATUS_OK;
}

/* -------------------------------------------------- General comparison */

static bool sxml_item_is_untyped(sxml_item item) {
    return sxml_atomic_item_get_primitive_type(item).index ==
           (uint32_t)SXML_TYPE_XS_UNTYPED_ATOMIC;
}

/* An xs:untypedAtomic item to convert for comparing with another item. */
typedef struct sxml_untyped_conversion {
    sxml_item untyped_item;
    /* The primitive type of the other item: the target. */
    sxml_builtin_type target_type;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_untyped_conversion;

/* A converted item, owned when is_owned. */
typedef struct sxml_converted_item {
    sxml_item item;
    bool is_owned;
} sxml_converted_item;

static void sxml_converted_item_release(sxml_converted_item *converted) {
    if (converted->is_owned) {
        sxml_item_release(converted->item);
        converted->is_owned = false;
    }
}

/*
 * Converts an xs:untypedAtomic item for comparing with an item of
 * conversion->target_type (XPath 4.0, section 4.11.2): against a string or
 * another untyped item it stays as it is (it compares as a string); against
 * a number it is cast to that number's primitive type, or failing that to
 * xs:double; otherwise to the other item's primitive type (FORG0001 when
 * the cast fails).
 */
static sxml_status
sxml_untyped_convert(sxml_item_heap *item_heap,
                     const sxml_untyped_conversion *conversion,
                     sxml_converted_item *converted) {
    sxml_builtin_type target_type = conversion->target_type;
    if (target_type == SXML_TYPE_XS_UNTYPED_ATOMIC ||
        target_type == SXML_TYPE_XS_STRING) {
        *converted = (sxml_converted_item){.item = conversion->untyped_item,
                                           .is_owned = false};
        return SXML_STATUS_OK;
    }
    sxml_operator_cast cast = {
        .item = conversion->untyped_item,
        .target_type = target_type,
        .namespace_bindings = conversion->namespace_bindings,
        .error_record = conversion->error_record,
    };
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_operator_cast_item(item_heap, &cast, &item);
    bool is_numeric = target_type == SXML_TYPE_XS_DECIMAL ||
                      target_type == SXML_TYPE_XS_FLOAT ||
                      target_type == SXML_TYPE_XS_DOUBLE;
    if (is_numeric && target_type != SXML_TYPE_XS_DOUBLE &&
        (status == SXML_STATUS_DYNAMIC_ERROR ||
         status == SXML_STATUS_TYPE_ERROR)) {
        /* "1.2e0" is no xs:decimal but an xs:double. */
        cast.target_type = SXML_TYPE_XS_DOUBLE;
        status = sxml_operator_cast_item(item_heap, &cast, &item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *converted = (sxml_converted_item){.item = item, .is_owned = true};
    return SXML_STATUS_OK;
}

/* One left item's conversion, kept while the right operand is walked. */
typedef struct sxml_conversion_cache {
    bool has_item;
    sxml_builtin_type target_type;
    sxml_converted_item converted;
} sxml_conversion_cache;

/* What comparing one pair of a general comparison needs. */
typedef struct sxml_general_pair {
    const sxml_comparison_request *request;
    sxml_item left_item;
    sxml_item right_item;
    /* The left item's conversion (in/out). */
    sxml_conversion_cache *left_cache;
} sxml_general_pair;

static sxml_builtin_type sxml_primitive_type_of(sxml_item item) {
    return (sxml_builtin_type)sxml_atomic_item_get_primitive_type(item).index;
}

/* Compares one pair after converting untyped items (4.11.2). */
static sxml_status sxml_general_pair_compare(sxml_item_heap *item_heap,
                                             const sxml_general_pair *pair,
                                             sxml_comparison_outcome *outcome) {
    const sxml_comparison_request *request = pair->request;
    bool left_is_untyped = sxml_item_is_untyped(pair->left_item);
    bool right_is_untyped = sxml_item_is_untyped(pair->right_item);
    sxml_item left_item = pair->left_item;
    sxml_converted_item right_converted = {.item = pair->right_item,
                                           .is_owned = false};
    sxml_status status = SXML_STATUS_OK;
    if (left_is_untyped && !right_is_untyped) {
        sxml_builtin_type target_type =
            sxml_primitive_type_of(pair->right_item);
        sxml_conversion_cache *cache = pair->left_cache;
        if (!cache->has_item || cache->target_type != target_type) {
            sxml_converted_item converted = {.item = {.representation = 0},
                                             .is_owned = false};
            status = sxml_untyped_convert(
                item_heap,
                &(sxml_untyped_conversion){
                    .untyped_item = pair->left_item,
                    .target_type = target_type,
                    .namespace_bindings = request->namespace_bindings,
                    .error_record = request->error_record},
                &converted);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            sxml_converted_item_release(&cache->converted);
            *cache = (sxml_conversion_cache){.has_item = true,
                                             .target_type = target_type,
                                             .converted = converted};
        }
        left_item = cache->converted.item;
    } else if (right_is_untyped && !left_is_untyped) {
        status = sxml_untyped_convert(
            item_heap,
            &(sxml_untyped_conversion){
                .untyped_item = pair->right_item,
                .target_type = sxml_primitive_type_of(pair->left_item),
                .namespace_bindings = request->namespace_bindings,
                .error_record = request->error_record},
            &right_converted);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    status = sxml_items_compare(
        item_heap,
        &(sxml_item_comparison){
            .comparison_operator = request->comparison_operator,
            .pair = {.first = left_item, .second = right_converted.item},
            .implicit_timezone_offset_minutes =
                request->implicit_timezone_offset_minutes,
            .error_record = request->error_record,
            .collation = request->collation},
        outcome);
    sxml_converted_item_release(&right_converted);
    return status;
}

/* Compares one left item with every right item until one pair holds. */
typedef struct sxml_general_row {
    const sxml_comparison_request *request;
    sxml_item left_item;
} sxml_general_row;

static sxml_status sxml_general_row_compare(sxml_item_heap *item_heap,
                                            const sxml_general_row *row,
                                            sxml_comparison_outcome *outcome) {
    const sxml_comparison_request *request = row->request;
    if (!sxml_item_is_atomic(row->left_item)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_conversion_cache left_cache = {
        .has_item = false,
        .target_type = SXML_TYPE_NONE,
        .converted = {.item = {.representation = 0}, .is_owned = false}};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(request->right_operand);
    sxml_status status = SXML_STATUS_OK;
    outcome->is_true = false;
    while (!outcome->is_true) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, request->error_record,
                                             &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (!sxml_item_is_atomic(step.item)) {
            status = SXML_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = sxml_general_pair_compare(
            item_heap,
            &(sxml_general_pair){.request = request,
                                 .left_item = row->left_item,
                                 .right_item = step.item,
                                 .left_cache = &left_cache},
            outcome);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_converted_item_release(&left_cache.converted);
    return status;
}

sxml_status
sxml_operator_make_general_comparison(sxml_item_heap *item_heap,
                                      const sxml_comparison_request *request,
                                      sxml_item *result) {
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(request->left_operand);
    sxml_comparison_outcome outcome = {.is_true = false};
    while (!outcome.is_true) {
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(
            &iterator, request->error_record, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            break;
        }
        status = sxml_general_row_compare(
            item_heap,
            &(sxml_general_row){.request = request, .left_item = step.item},
            &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *result =
        outcome.is_true ? sxml_atomic_item_true() : sxml_atomic_item_false();
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- GNode comparison */

/* Reads an operand that must be empty or one GNode. */
static sxml_status sxml_node_operand_read(sxml_sequence operand,
                                          sxml_error_record *error_record,
                                          sxml_single_operand *single_operand) {
    sxml_item first_item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        operand,
        (sxml_item_request){.item_index = 0, .error_record = error_record},
        &first_item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *single_operand = (sxml_single_operand){.has_item = false,
                                                .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item second_item = {.representation = 0};
    status = sxml_sequence_get_item(
        operand,
        (sxml_item_request){.item_index = 1, .error_record = error_record},
        &second_item);
    if (status == SXML_STATUS_OK || !sxml_item_is_generic_node(first_item)) {
        return sxml_error_record_report(error_record,
                                        &sxml_node_comparison_operand_error);
    }
    if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return status;
    }
    *single_operand =
        (sxml_single_operand){.has_item = true, .item = first_item};
    return SXML_STATUS_OK;
}

/* Whether the document order of two GNodes satisfies the operator. */
static bool sxml_node_ordering_satisfies(
    sxml_node_comparison_operator node_comparison_operator,
    sxml_ordering ordering) {
    switch (node_comparison_operator) {
    case SXML_NODE_COMPARISON_OPERATOR_IS:
        return ordering == SXML_ORDERING_EQUAL;
    case SXML_NODE_COMPARISON_OPERATOR_IS_NOT:
        return ordering != SXML_ORDERING_EQUAL;
    case SXML_NODE_COMPARISON_OPERATOR_PRECEDES:
        return ordering == SXML_ORDERING_LESS;
    case SXML_NODE_COMPARISON_OPERATOR_FOLLOWS:
        return ordering == SXML_ORDERING_GREATER;
    case SXML_NODE_COMPARISON_OPERATOR_PRECEDES_OR_IS:
        return ordering != SXML_ORDERING_GREATER;
    case SXML_NODE_COMPARISON_OPERATOR_FOLLOWS_OR_IS:
        return ordering != SXML_ORDERING_LESS;
    }
    return false;
}

sxml_status
sxml_operator_make_node_comparison(const sxml_node_comparison_request *request,
                                   sxml_sequence *result) {
    sxml_single_operand left_operand = {.has_item = false,
                                        .item = {.representation = 0}};
    sxml_status status = sxml_node_operand_read(
        request->left_operand, request->error_record, &left_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!left_operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_single_operand right_operand = {.has_item = false,
                                         .item = {.representation = 0}};
    status = sxml_node_operand_read(request->right_operand,
                                    request->error_record, &right_operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!right_operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item_pair pair = {.first = left_operand.item,
                           .second = right_operand.item};
    /* Document order is total, and equal exactly for the same GNode
       (XDM 4.0, sections 6.1 and 6.2). */
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    status = sxml_generic_node_compare_document_order(pair, &ordering);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result =
        sxml_sequence_from_item(sxml_node_ordering_satisfies(
                                    request->node_comparison_operator, ordering)
                                    ? sxml_atomic_item_true()
                                    : sxml_atomic_item_false());
    return SXML_STATUS_OK;
}
