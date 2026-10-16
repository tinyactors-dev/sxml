/*
 * Values and functions for the evaluator (src/evaluator_internal.h; D38,
 * D55, D75): atomization, the effective boolean value, sequence type
 * matching, the coercion rules, and the function items the evaluator makes
 * (inline functions, named function references, coerced functions,
 * constructor functions) with their body operations.
 *
 * Golden file: src/xdm_function.c (bodies, static errors), after
 * src/memory.c.
 *
 * Specification: XPath 4.0, section 2.5.3 "Atomization", section 2.5.4
 * "Effective Boolean Value", section 3.1.2 "Sequence Type Matching",
 * section 3.4 "Coercion Rules" (atomization, untypedAtomic casting,
 * numeric and URI promotion, relabelling of derived atomic types, function
 * coercion), section 4.5.4 "Named Function References", section 4.5.6
 * "Inline Function Expressions"; F&O 4.0, section 19 (casting).
 *
 * Nothing here recurses (D71): atomization and matching walk nested arrays,
 * maps and choice types on explicit stacks in allocator memory; calling a
 * function item from a body goes through the evaluator's machine.
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Errors */

static const sxml_static_error sxml_function_atomization_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOTY0013"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function item or map cannot be atomized (XPath 4.0, section "
        "2.5.3)"),
};

static const sxml_static_error sxml_effective_boolean_value_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The sequence has no effective boolean value (XPath 4.0, section "
        "2.5.4)"),
};

static const sxml_static_error sxml_coercion_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value does not match the required type (XPath 4.0, section "
        "3.4, coercion rules)"),
};

static const sxml_static_error sxml_template_parameter_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE0590"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value does not match the required type of the parameter (XSLT "
        "4.0, section 9.3)"),
};

static const sxml_static_error sxml_namespace_sensitive_coercion_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0117"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An xs:untypedAtomic item cannot be coerced to a namespace-sensitive "
        "type (XPath 4.0, section 3.4.3)"),
};

static const sxml_static_error sxml_not_a_function_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A dynamic function call is applied to an item that is not a "
        "function (XPath 4.0, section 4.5.3)"),
};

static const sxml_static_error sxml_function_arity_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function item cannot be coerced to a function type of lower "
        "arity (XPath 4.0, section 3.4.4)"),
};

/* ---------------------------------------------------------- Atomization */

/* A sequence or an array being atomized: where its next item or member is. */
typedef struct sxml_atomization_frame {
    bool is_array;
    /* !is_array: the sequence, borrowed from the input or from an array or
       JNode in it. */
    sxml_sequence_iterator iterator;
    /* is_array: the array, borrowed, and its next member. */
    sxml_item array;
    size_t next_member_index;
    size_t member_count;
} sxml_atomization_frame;

typedef struct sxml_atomization_stack {
    sxml_allocator allocator;
    sxml_atomization_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_atomization_stack;

static sxml_status sxml_atomization_stack_push(sxml_atomization_stack *stack,
                                               sxml_atomization_frame frame) {
    if (stack->frame_count == stack->frame_capacity) {
        void *elements = stack->frames;
        sxml_status status = sxml_evaluator_grow(
            stack->allocator,
            &(sxml_evaluator_growth){
                .elements = &elements,
                .element_capacity = &stack->frame_capacity,
                .element_count = stack->frame_count,
                .required_count = stack->frame_count + 1,
                .element_byte_count = sizeof(sxml_atomization_frame),
                .alignment = alignof(sxml_atomization_frame)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        stack->frames = elements;
    }
    stack->frames[stack->frame_count] = frame;
    stack->frame_count += 1;
    return SXML_STATUS_OK;
}

static sxml_atomization_frame sxml_atomization_sequence_frame(
    sxml_sequence sequence) {
    return (sxml_atomization_frame){
        .is_array = false,
        .iterator = sxml_sequence_iterator_start(sequence),
        .array = {.representation = 0},
        .next_member_index = 0,
        .member_count = 0};
}

/* Whether a value or type is atomic. */
typedef struct sxml_atomicity {
    bool is_atomic;
} sxml_atomicity;

/* True when every item of sequence is atomic (nothing to do). */
static sxml_status sxml_sequence_is_atomic(sxml_sequence sequence,
                                           sxml_error_record *error_record,
                                           sxml_atomicity *atomicity) {
    bool *is_atomic = &atomicity->is_atomic;
    switch (sxml_sequence_get_representation(sequence)) {
    case SXML_SEQUENCE_REPRESENTATION_EMPTY:
    case SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE:
        *is_atomic = true;
        return SXML_STATUS_OK;
    case SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM:
        *is_atomic = sxml_item_is_atomic(sequence.content);
        return SXML_STATUS_OK;
    case SXML_SEQUENCE_REPRESENTATION_VECTOR:
    case SXML_SEQUENCE_REPRESENTATION_CONCATENATION:
    case SXML_SEQUENCE_REPRESENTATION_LAZY:
        break;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, error_record, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            *is_atomic = true;
            return SXML_STATUS_OK;
        }
        if (!sxml_item_is_atomic(step.item)) {
            *is_atomic = false;
            return SXML_STATUS_OK;
        }
    }
}

/* Adds the atomized value of item to builder, or pushes a frame for its
   parts (arrays, JNode values). */
typedef struct sxml_atomization_target {
    sxml_dynamic_context *dynamic_context;
    sxml_sequence_builder *builder;
    sxml_atomization_stack *stack;
    sxml_error_record *error_record;
} sxml_atomization_target;

static sxml_status sxml_atomize_item(const sxml_atomization_target *target,
                                     sxml_item item) {
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ATOMIC:
        return sxml_sequence_builder_append_item(target->builder, item);
    case SXML_ITEM_KIND_NODE: {
        sxml_node_reference node;
        sxml_status status = sxml_item_get_node(item, &node);
        sxml_sequence typed_value = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = sxml_node_make_typed_value(
                target->dynamic_context->item_heap,
                &(sxml_node_value_request){.node = node,
                                           .error_record = target->error_record},
                &typed_value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(
                target->builder, typed_value, target->error_record);
            sxml_sequence_release(typed_value);
        }
        return status;
    }
    case SXML_ITEM_KIND_JSON_NODE: {
        /* A JNode atomizes as its ·jvalue· (XPath 4.0, section 2.5.3). */
        sxml_json_node_description description;
        sxml_status status = sxml_json_node_describe(item, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_atomization_stack_push(
            target->stack, sxml_atomization_sequence_frame(description.json_value));
    }
    case SXML_ITEM_KIND_ARRAY: {
        size_t member_count = 0;
        sxml_status status = sxml_array_get_size(item, &member_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_atomization_stack_push(
            target->stack,
            (sxml_atomization_frame){.is_array = true,
                                     .iterator = sxml_sequence_iterator_start(
                                         sxml_sequence_empty()),
                                     .array = item,
                                     .next_member_index = 0,
                                     .member_count = member_count});
    }
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_MAP:
        break;
    }
    return sxml_error_record_report(target->error_record,
                                    &sxml_function_atomization_error);
}

sxml_status sxml_evaluator_atomize(sxml_dynamic_context *dynamic_context,
                                   const sxml_atomization_request *request,
                                   sxml_sequence *result) {
    sxml_atomicity atomicity = {.is_atomic = false};
    sxml_status status =
        sxml_sequence_is_atomic(request->value, request->error_record, &atomicity);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (atomicity.is_atomic) {
        sxml_sequence_retain(request->value);
        *result = request->value;
        return SXML_STATUS_OK;
    }
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        dynamic_context->allocator,
        &(sxml_sequence_builder_configuration){.item_heap =
                                                   dynamic_context->item_heap},
        &builder);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomization_stack stack = {.allocator = dynamic_context->allocator,
                                    .frames = NULL,
                                    .frame_count = 0,
                                    .frame_capacity = 0};
    const sxml_atomization_target target = {.dynamic_context = dynamic_context,
                                            .builder = builder,
                                            .stack = &stack,
                                            .error_record = request->error_record};
    status = sxml_atomization_stack_push(
        &stack, sxml_atomization_sequence_frame(request->value));
    while (status == SXML_STATUS_OK && stack.frame_count > 0) {
        sxml_atomization_frame *frame = &stack.frames[stack.frame_count - 1];
        if (frame->is_array) {
            if (frame->next_member_index == frame->member_count) {
                stack.frame_count -= 1;
                continue;
            }
            sxml_sequence member = sxml_sequence_empty();
            status = sxml_array_get_member(frame->array, frame->next_member_index,
                                           &member);
            frame->next_member_index += 1;
            if (status == SXML_STATUS_OK) {
                status = sxml_atomization_stack_push(
                    &stack, sxml_atomization_sequence_frame(member));
            }
            continue;
        }
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&frame->iterator,
                                             request->error_record, &step);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!step.has_item) {
            stack.frame_count -= 1;
            continue;
        }
        status = sxml_atomize_item(&target, step.item);
    }
    if (stack.frames != NULL) {
        sxml_allocator_release(dynamic_context->allocator, stack.frames);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &value);
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}

/* ---------------------------------------------- Effective boolean value */

sxml_status sxml_evaluator_compute_effective_boolean_value(
    sxml_sequence sequence, sxml_error_record *error_record,
    sxml_effective_boolean_value *effective_boolean_value) {
    sxml_item first = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        sequence,
        (sxml_item_request){.item_index = 0, .error_record = error_record},
        &first);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        /* The empty sequence is false (XPath 4.0, section 2.5.4, rule 1). */
        effective_boolean_value->is_true = false;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_item_is_generic_node(first)) {
        /* Rule 2: a sequence whose first item is a node is true. */
        effective_boolean_value->is_true = true;
        return SXML_STATUS_OK;
    }
    sxml_item second = {.representation = 0};
    status = sxml_sequence_get_item(
        sequence,
        (sxml_item_request){.item_index = 1, .error_record = error_record},
        &second);
    if (status == SXML_STATUS_OK || !sxml_item_is_atomic(first)) {
        /* Rule 6: more items, or a function item, map or array. */
        return sxml_error_record_report(error_record,
                                        &sxml_effective_boolean_value_error);
    }
    if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(first, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Rules 3 to 5: booleans, strings and numbers. */
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_BOOLEAN:
        effective_boolean_value->is_true = datum.datum_details.boolean;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_STRING:
        effective_boolean_value->is_true =
            datum.datum_details.string.utf8.byte_count != 0;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_INTEGER:
        effective_boolean_value->is_true = datum.datum_details.integer != 0;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_DECIMAL: {
        /* Canonical text: "0" is the only zero. */
        const sxml_byte_view text = datum.datum_details.decimal_text;
        effective_boolean_value->is_true =
            !(text.byte_count == 1 && text.bytes[0] == '0');
        return SXML_STATUS_OK;
    }
    case SXML_DATUM_KIND_FLOAT: {
        const float number = datum.datum_details.float_number;
        effective_boolean_value->is_true = number == number && number != 0.0F;
        return SXML_STATUS_OK;
    }
    case SXML_DATUM_KIND_DOUBLE: {
        const double number = datum.datum_details.double_number;
        effective_boolean_value->is_true = number == number && number != 0.0;
        return SXML_STATUS_OK;
    }
    case SXML_DATUM_KIND_QNAME:
    case SXML_DATUM_KIND_DATE_TIME:
    case SXML_DATUM_KIND_DURATION:
    case SXML_DATUM_KIND_BINARY:
        break;
    }
    return sxml_error_record_report(error_record,
                                    &sxml_effective_boolean_value_error);
}

/* ---------------------------------------------------- Type matching */

/*
 * The goals of a match (glossary `type_match_frame`). SEQUENCE, MAP_ENTRIES,
 * ARRAY_MEMBERS and RECORD_FIELDS are conjunctions: they push one goal per
 * part and succeed when every part did. CHOICE is a disjunction: a failure
 * above it unwinds to it, and it tries its next alternative. ITEM is one
 * item and one item type: decided at once, or turned into one of the
 * others in place.
 */
typedef enum sxml_type_match_frame_kind {
    SXML_TYPE_MATCH_FRAME_KIND_SEQUENCE,
    SXML_TYPE_MATCH_FRAME_KIND_ITEM,
    SXML_TYPE_MATCH_FRAME_KIND_MAP_ENTRIES,
    SXML_TYPE_MATCH_FRAME_KIND_ARRAY_MEMBERS,
    SXML_TYPE_MATCH_FRAME_KIND_RECORD_FIELDS,
    SXML_TYPE_MATCH_FRAME_KIND_CHOICE,
} sxml_type_match_frame_kind;

typedef struct sxml_type_match_frame {
    sxml_type_match_frame_kind frame_kind;
    /* SEQUENCE: borrowed; its items are matched against
       sequence_type.item_type. */
    sxml_sequence_iterator iterator;
    sxml_sequence_type sequence_type;
    bool has_counted;
    /* ITEM, CHOICE, MAP_ENTRIES, ARRAY_MEMBERS, RECORD_FIELDS: borrowed. */
    sxml_item item;
    /* ITEM, CHOICE: the type. RECORD_FIELDS: the record type. MAP_ENTRIES:
       the key type (sequence_type is the value type). ARRAY_MEMBERS:
       sequence_type is the member type. */
    sxml_type_identifier item_type;
    /* The next part or alternative, and how many there are. */
    size_t next_index;
    size_t part_count;
} sxml_type_match_frame;

typedef struct sxml_type_matcher {
    sxml_dynamic_context *dynamic_context;
    const sxml_type_registry *registry;
    sxml_error_record *error_record;
    sxml_type_match_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_type_matcher;

/* What advancing the top frame did. */
typedef enum sxml_type_match_outcome {
    /* The frame's goal holds: pop it. */
    SXML_TYPE_MATCH_OUTCOME_HOLDS,
    /* The goal fails: unwind to the nearest choice. */
    SXML_TYPE_MATCH_OUTCOME_FAILS,
    /* A frame was pushed or the frame was changed in place: go on. */
    SXML_TYPE_MATCH_OUTCOME_CONTINUES,
} sxml_type_match_outcome;

static sxml_status sxml_type_matcher_push(sxml_type_matcher *matcher,
                                          sxml_type_match_frame frame) {
    if (matcher->frame_count == matcher->frame_capacity) {
        void *elements = matcher->frames;
        sxml_status status = sxml_evaluator_grow(
            matcher->dynamic_context->allocator,
            &(sxml_evaluator_growth){
                .elements = &elements,
                .element_capacity = &matcher->frame_capacity,
                .element_count = matcher->frame_count,
                .required_count = matcher->frame_count + 1,
                .element_byte_count = sizeof(sxml_type_match_frame),
                .alignment = alignof(sxml_type_match_frame)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        matcher->frames = elements;
    }
    matcher->frames[matcher->frame_count] = frame;
    matcher->frame_count += 1;
    return SXML_STATUS_OK;
}

static sxml_type_match_frame sxml_sequence_match_frame(sxml_sequence sequence,
                                                       sxml_sequence_type type) {
    return (sxml_type_match_frame){
        .frame_kind = SXML_TYPE_MATCH_FRAME_KIND_SEQUENCE,
        .iterator = sxml_sequence_iterator_start(sequence),
        .sequence_type = type,
        .has_counted = false,
        .item = {.representation = 0},
        .item_type = {.index = SXML_TYPE_NONE},
        .next_index = 0,
        .part_count = 0};
}

static sxml_type_match_frame sxml_item_match_frame(sxml_item item,
                                                   sxml_type_identifier type) {
    sxml_type_match_frame frame = sxml_sequence_match_frame(
        sxml_sequence_empty(),
        (sxml_sequence_type){.item_type = {.index = SXML_TYPE_NONE},
                             .occurrence = SXML_OCCURRENCE_ZERO});
    frame.frame_kind = SXML_TYPE_MATCH_FRAME_KIND_ITEM;
    frame.item = item;
    frame.item_type = type;
    return frame;
}

/* True when the count of items is allowed by occurrence (bit sets of
   types.h). */
static bool sxml_occurrence_allows(sxml_occurrence occurrence,
                                   uint64_t item_count) {
    const unsigned count_bit =
        item_count == 0 ? 1U : (item_count == 1 ? 2U : 4U);
    return ((unsigned)occurrence & count_bit) != 0;
}

/* xs:string with a value among the enumeration's (XPath 4.0, section
   3.2.6). */
typedef struct sxml_item_against_type {
    sxml_item item;
    const sxml_type_description *description;
} sxml_item_against_type;

static sxml_status sxml_item_matches_enumeration(
    const sxml_type_matcher *matcher, sxml_item_against_type question,
    sxml_type_match *match) {
    const sxml_item item = question.item;
    const sxml_type_description *description = question.description;
    bool *is_match = &match->is_match;
    *is_match = false;
    if (!sxml_item_is_atomic(item) ||
        !sxml_type_registry_is_derived_from(
            matcher->registry,
            (sxml_type_pair){
                .derived_type = sxml_item_get_type_annotation(item),
                .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING)})) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view_list values =
        description->variety_details.enumeration_values;
    for (size_t value_index = 0; value_index < values.byte_view_count;
         value_index += 1) {
        if (sxml_byte_view_equals(values.byte_views[value_index],
                                  datum.datum_details.string.utf8)) {
            *is_match = true;
            break;
        }
    }
    return SXML_STATUS_OK;
}

/* The key constraint of jnode(K, T) (XPath 4.0, section 3.2.12). */
typedef struct sxml_json_key_question {
    const sxml_json_node_description *node;
    const sxml_json_node_test *test;
} sxml_json_key_question;

static sxml_status sxml_json_key_matches(const sxml_type_matcher *matcher,
                                         sxml_json_key_question question,
                                         sxml_type_match *match) {
    const sxml_json_node_description *node = question.node;
    const sxml_json_node_test *test = question.test;
    bool *is_match = &match->is_match;
    switch (test->key_constraint) {
    case SXML_JSON_KEY_CONSTRAINT_ANY:
        *is_match = true;
        return SXML_STATUS_OK;
    case SXML_JSON_KEY_CONSTRAINT_ROOT:
        *is_match = !node->has_parent;
        return SXML_STATUS_OK;
    case SXML_JSON_KEY_CONSTRAINT_CONSTANT:
        break;
    }
    *is_match = false;
    if (!node->has_parent) {
        return SXML_STATUS_OK;
    }
    sxml_sequence constant = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value_from_text(
        matcher->dynamic_context->item_heap,
        &(sxml_text_cast_request){
            .text = test->key_lexical_form,
            .target_type = test->key_type,
            .namespace_bindings = {.namespace_bindings = NULL,
                                   .namespace_binding_count = 0},
            .error_record = NULL},
        &constant);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *is_match = sxml_atomic_item_is_atomic_equal(constant.content, node->json_key);
    sxml_sequence_release(constant);
    return SXML_STATUS_OK;
}

/* A map matching a record type: no keys beyond the field names (records
   are not extensible here), then each field. */
typedef struct sxml_record_question {
    sxml_item map;
    sxml_type_identifier record_type;
} sxml_record_question;

static sxml_status sxml_map_keys_are_fields(const sxml_type_matcher *matcher,
                                            sxml_record_question question,
                                            sxml_type_match *match) {
    const sxml_item map = question.map;
    const sxml_type_identifier record_type = question.record_type;
    bool *is_match = &match->is_match;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    *is_match = true;
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && *is_match && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_type_identifier primitive =
            sxml_atomic_item_get_primitive_type(entry.key);
        if (primitive.index != SXML_TYPE_XS_STRING &&
            primitive.index != SXML_TYPE_XS_UNTYPED_ATOMIC &&
            primitive.index != SXML_TYPE_XS_ANY_URI) {
            *is_match = false;
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(entry.key, &datum);
        size_t field_index = 0;
        if (status == SXML_STATUS_OK) {
            *is_match = sxml_type_registry_find_record_field(
                matcher->registry,
                (sxml_record_field_query){.record_type = record_type,
                                          .field_name =
                                              datum.datum_details.string.utf8},
                &field_index);
        }
    }
    return status;
}

/* Decides an ITEM frame, or turns it into the frame its type needs. */
static sxml_status sxml_type_matcher_advance_item(sxml_type_matcher *matcher,
                                                  sxml_type_match_frame *frame,
                                                  sxml_type_match_outcome *outcome) {
    const sxml_item item = frame->item;
    const sxml_type_identifier type = frame->item_type;
    *outcome = SXML_TYPE_MATCH_OUTCOME_FAILS;
    /* Generic item types by identifier first (XPath 4.0, section 3.2). */
    switch (type.index) {
    case SXML_TYPE_ITEM:
        *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        return SXML_STATUS_OK;
    case SXML_TYPE_GENERIC_NODE:
        if (sxml_item_is_generic_node(item)) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        }
        return SXML_STATUS_OK;
    case SXML_TYPE_NODE:
        if (sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        }
        return SXML_STATUS_OK;
    case SXML_TYPE_FUNCTION:
        if (sxml_item_is_function_item(item)) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        }
        return SXML_STATUS_OK;
    case SXML_TYPE_MAP:
    case SXML_TYPE_RECORD:
        if (sxml_item_get_kind(item) == SXML_ITEM_KIND_MAP) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        }
        return SXML_STATUS_OK;
    case SXML_TYPE_ARRAY:
        if (sxml_item_get_kind(item) == SXML_ITEM_KIND_ARRAY) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
        }
        return SXML_STATUS_OK;
    default:
        break;
    }
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(matcher->registry, type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_match = false;
    sxml_type_match item_match = {.is_match = false};
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_UNION:
        /* By the item's annotation (section 3.2.3). */
        is_match = sxml_item_is_atomic(item) &&
                   sxml_type_registry_is_derived_from(
                       matcher->registry,
                       (sxml_type_pair){
                           .derived_type = sxml_item_get_type_annotation(item),
                           .base_type = type});
        break;
    case SXML_TYPE_VARIETY_ENUMERATION:
        status = sxml_item_matches_enumeration(
            matcher, (sxml_item_against_type){.item = item, .description = &description},
            &item_match);
        is_match = item_match.is_match;
        break;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_LIST:
        /* Schema types that are not item types match nothing. */
        break;
    case SXML_TYPE_VARIETY_ANY_ITEM:
        is_match = true;
        break;
    case SXML_TYPE_VARIETY_NODE_TEST:
        if (sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
            sxml_node_reference node;
            sxml_type_match node_match = {.is_match = false};
            status = sxml_item_get_node(item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_evaluator_test_node(
                    matcher->dynamic_context->item_heap,
                    &(sxml_node_test_request){
                        .node = node,
                        .node_test = &description.variety_details.node_test},
                    &node_match);
            }
            is_match = node_match.is_match;
        }
        break;
    case SXML_TYPE_VARIETY_JSON_NODE_TEST: {
        if (sxml_item_get_kind(item) != SXML_ITEM_KIND_JSON_NODE) {
            break;
        }
        const sxml_json_node_test *test =
            &description.variety_details.json_node_test;
        sxml_json_node_description node;
        status = sxml_json_node_describe(item, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_json_key_matches(
                matcher, (sxml_json_key_question){.node = &node, .test = test},
                &item_match);
            is_match = item_match.is_match;
        }
        if (status == SXML_STATUS_OK && is_match && test->has_value_type) {
            /* The value must match too: the frame becomes that goal. */
            *frame = sxml_sequence_match_frame(node.json_value, test->value_type);
            *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
            return SXML_STATUS_OK;
        }
        break;
    }
    case SXML_TYPE_VARIETY_FUNCTION:
        /* A function item matches by its signature (section 3.2.8.1); a map
           or array by its type annotation as a function. */
        is_match = sxml_item_is_function_item(item) &&
                   (description.variety_details.function_type.is_any_function ||
                    sxml_type_registry_is_subtype(
                        matcher->registry,
                        (sxml_type_pair){
                            .derived_type = sxml_item_get_type_annotation(item),
                            .base_type = type}));
        break;
    case SXML_TYPE_VARIETY_MAP: {
        if (sxml_item_get_kind(item) != SXML_ITEM_KIND_MAP) {
            break;
        }
        const sxml_map_type *map_type = &description.variety_details.map_type;
        if (map_type->is_any_map) {
            is_match = true;
            break;
        }
        size_t entry_count = 0;
        status = sxml_map_get_size(item, &entry_count);
        if (status == SXML_STATUS_OK) {
            frame->frame_kind = SXML_TYPE_MATCH_FRAME_KIND_MAP_ENTRIES;
            frame->item_type = map_type->key_type;
            frame->sequence_type = map_type->value_type;
            frame->next_index = 0;
            frame->part_count = entry_count;
            *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_ARRAY: {
        if (sxml_item_get_kind(item) != SXML_ITEM_KIND_ARRAY) {
            break;
        }
        const sxml_array_type *array_type = &description.variety_details.array_type;
        if (array_type->is_any_array) {
            is_match = true;
            break;
        }
        size_t member_count = 0;
        status = sxml_array_get_size(item, &member_count);
        if (status == SXML_STATUS_OK) {
            frame->frame_kind = SXML_TYPE_MATCH_FRAME_KIND_ARRAY_MEMBERS;
            frame->sequence_type = array_type->member_type;
            frame->next_index = 0;
            frame->part_count = member_count;
            *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_RECORD: {
        if (sxml_item_get_kind(item) != SXML_ITEM_KIND_MAP) {
            break;
        }
        const sxml_record_type *record_type =
            &description.variety_details.record_type;
        if (record_type->is_any_record ||
            sxml_item_get_type_annotation(item).index == type.index) {
            is_match = true;
            break;
        }
        if (record_type->is_nominative) {
            /* Only records annotated with it (glossary
               `nominative_record_type`). */
            break;
        }
        status = sxml_map_keys_are_fields(
            matcher, (sxml_record_question){.map = item, .record_type = type},
            &item_match);
        is_match = item_match.is_match;
        if (status == SXML_STATUS_OK && is_match) {
            frame->frame_kind = SXML_TYPE_MATCH_FRAME_KIND_RECORD_FIELDS;
            frame->next_index = 0;
            frame->part_count = record_type->record_fields.record_field_count;
            *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
            return SXML_STATUS_OK;
        }
        break;
    }
    case SXML_TYPE_VARIETY_CHOICE:
        frame->frame_kind = SXML_TYPE_MATCH_FRAME_KIND_CHOICE;
        frame->next_index = 0;
        frame->part_count = description.variety_details.member_types
                                .type_identifier_count;
        *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK && is_match) {
        *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
    }
    return status;
}

/* Advances the top frame (not an ITEM frame): pushes the goal of its next
   part, or holds when its parts are done. */
static sxml_status sxml_type_matcher_advance(sxml_type_matcher *matcher,
                                             sxml_type_match_outcome *outcome) {
    sxml_type_match_frame *frame = &matcher->frames[matcher->frame_count - 1];
    *outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
    switch (frame->frame_kind) {
    case SXML_TYPE_MATCH_FRAME_KIND_ITEM:
        return sxml_type_matcher_advance_item(matcher, frame, outcome);
    case SXML_TYPE_MATCH_FRAME_KIND_SEQUENCE: {
        if (!frame->has_counted) {
            uint64_t item_count = 0;
            sxml_status status = sxml_sequence_count(
                frame->iterator.sequence, matcher->error_record, &item_count);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            frame->has_counted = true;
            if (!sxml_occurrence_allows(frame->sequence_type.occurrence,
                                        item_count)) {
                *outcome = SXML_TYPE_MATCH_OUTCOME_FAILS;
                return SXML_STATUS_OK;
            }
            if (item_count == 0 ||
                frame->sequence_type.item_type.index == SXML_TYPE_ITEM) {
                *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
                return SXML_STATUS_OK;
            }
        }
        sxml_iteration_step step;
        sxml_status status = sxml_sequence_iterator_next(
            &frame->iterator, matcher->error_record, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
            return SXML_STATUS_OK;
        }
        return sxml_type_matcher_push(
            matcher,
            sxml_item_match_frame(step.item, frame->sequence_type.item_type));
    }
    case SXML_TYPE_MATCH_FRAME_KIND_MAP_ENTRIES: {
        if (frame->next_index == frame->part_count) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
            return SXML_STATUS_OK;
        }
        sxml_map_entry entry;
        sxml_status status =
            sxml_map_get_entry(frame->item, frame->next_index, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->next_index += 1;
        const sxml_sequence_type value_type = frame->sequence_type;
        const sxml_type_identifier key_type = frame->item_type;
        /* The key, then the value (XPath 4.0, section 3.2.9). */
        status = sxml_type_matcher_push(
            matcher, sxml_sequence_match_frame(entry.value, value_type));
        if (status == SXML_STATUS_OK) {
            status = sxml_type_matcher_push(
                matcher, sxml_item_match_frame(entry.key, key_type));
        }
        return status;
    }
    case SXML_TYPE_MATCH_FRAME_KIND_ARRAY_MEMBERS: {
        if (frame->next_index == frame->part_count) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
            return SXML_STATUS_OK;
        }
        sxml_sequence member = sxml_sequence_empty();
        sxml_status status =
            sxml_array_get_member(frame->item, frame->next_index, &member);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->next_index += 1;
        return sxml_type_matcher_push(
            matcher, sxml_sequence_match_frame(member, frame->sequence_type));
    }
    case SXML_TYPE_MATCH_FRAME_KIND_RECORD_FIELDS: {
        if (frame->next_index == frame->part_count) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
            return SXML_STATUS_OK;
        }
        sxml_type_description description;
        sxml_status status = sxml_type_registry_describe_type(
            matcher->registry, frame->item_type, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_record_field *field =
            &description.variety_details.record_type.record_fields
                 .record_fields[frame->next_index];
        frame->next_index += 1;
        /* An absent field is the empty sequence, which its type must
           allow (section 3.2.10). */
        sxml_item key = {.representation = 0};
        status = sxml_atomic_item_make(
            matcher->dynamic_context->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = field->field_name,
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &key);
        sxml_map_lookup lookup = {.is_found = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_map_find(frame->item, key, &lookup);
            sxml_item_release(key);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_sequence value =
            lookup.is_found ? lookup.entry.value : sxml_sequence_empty();
        return sxml_type_matcher_push(
            matcher, sxml_sequence_match_frame(value, field->field_type));
    }
    case SXML_TYPE_MATCH_FRAME_KIND_CHOICE: {
        if (frame->next_index > 0) {
            /* The alternative pushed last held. */
            *outcome = SXML_TYPE_MATCH_OUTCOME_HOLDS;
            return SXML_STATUS_OK;
        }
        if (frame->part_count == 0) {
            *outcome = SXML_TYPE_MATCH_OUTCOME_FAILS;
            return SXML_STATUS_OK;
        }
        sxml_type_description description;
        sxml_status status = sxml_type_registry_describe_type(
            matcher->registry, frame->item_type, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->next_index = 1;
        return sxml_type_matcher_push(
            matcher,
            sxml_item_match_frame(
                frame->item,
                description.variety_details.member_types.type_identifiers[0]));
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Unwinds after a failed goal to the nearest choice with an untried
   alternative and pushes that alternative; *has_failed is set when no
   choice is left (match->is_match false). */
static sxml_status sxml_type_matcher_unwind(sxml_type_matcher *matcher,
                                            sxml_type_match *match) {
    while (matcher->frame_count > 0) {
        sxml_type_match_frame *frame = &matcher->frames[matcher->frame_count - 1];
        if (frame->frame_kind != SXML_TYPE_MATCH_FRAME_KIND_CHOICE ||
            frame->next_index >= frame->part_count) {
            matcher->frame_count -= 1;
            continue;
        }
        sxml_type_description description;
        sxml_status status = sxml_type_registry_describe_type(
            matcher->registry, frame->item_type, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_type_identifier alternative =
            description.variety_details.member_types
                .type_identifiers[frame->next_index];
        frame->next_index += 1;
        const sxml_item item = frame->item;
        return sxml_type_matcher_push(matcher,
                                      sxml_item_match_frame(item, alternative));
    }
    match->is_match = false;
    return SXML_STATUS_OK;
}

sxml_status sxml_evaluator_match_sequence_type(
    sxml_dynamic_context *dynamic_context, const sxml_type_match_request *request,
    sxml_type_match *match) {
    /* The most common case needs no stack: one item against a type it
       matches by annotation. */
    sxml_type_matcher matcher = {
        .dynamic_context = dynamic_context,
        .registry = sxml_item_heap_get_type_registry(dynamic_context->item_heap),
        .error_record = request->error_record,
        .frames = NULL,
        .frame_count = 0,
        .frame_capacity = 0};
    sxml_status status = sxml_type_matcher_push(
        &matcher, sxml_sequence_match_frame(request->value, request->sequence_type));
    sxml_type_match outcome_match = {.is_match = true};
    while (status == SXML_STATUS_OK && outcome_match.is_match &&
           matcher.frame_count > 0) {
        sxml_type_match_outcome outcome = SXML_TYPE_MATCH_OUTCOME_CONTINUES;
        status = sxml_type_matcher_advance(&matcher, &outcome);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (outcome == SXML_TYPE_MATCH_OUTCOME_HOLDS) {
            matcher.frame_count -= 1;
        } else if (outcome == SXML_TYPE_MATCH_OUTCOME_FAILS) {
            status = sxml_type_matcher_unwind(&matcher, &outcome_match);
        }
    }
    if (matcher.frames != NULL) {
        sxml_allocator_release(dynamic_context->allocator, matcher.frames);
    }
    if (status == SXML_STATUS_OK) {
        match->is_match = outcome_match.is_match;
    }
    return status;
}

/* ------------------------------------------------------------- Coercion */

/* What the coercion rules do with the items of an atomized value. */
typedef struct sxml_atomic_coercion {
    sxml_dynamic_context *dynamic_context;
    const sxml_type_registry *registry;
    /* The required item type and the primitive type of an atomic one
       (SXML_TYPE_NONE for unions, enumerations and choices). */
    sxml_type_identifier item_type;
    sxml_type_identifier primitive_type;
    sxml_error_record *error_record;
} sxml_atomic_coercion;

/* True for item types whose instances are atomic: the generalized atomic
   types of XPath 4.0, section 3.2.3, and choices of them. */
static sxml_status sxml_type_is_generalized_atomic(const sxml_type_registry *registry,
                                                   sxml_type_identifier type,
                                                   sxml_atomicity *atomicity) {
    bool *is_atomic = &atomicity->is_atomic;
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(registry, type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *is_atomic = false;
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ENUMERATION:
        *is_atomic = true;
        break;
    case SXML_TYPE_VARIETY_CHOICE: {
        /* One level: a choice of atomic types, such as (xs:string |
           xs:integer). */
        const sxml_type_identifier_view members =
            description.variety_details.member_types;
        *is_atomic = members.type_identifier_count > 0;
        for (size_t member_index = 0;
             *is_atomic && member_index < members.type_identifier_count;
             member_index += 1) {
            sxml_type_description member;
            status = sxml_type_registry_describe_type(
                registry, members.type_identifiers[member_index], &member);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            *is_atomic = member.variety == SXML_TYPE_VARIETY_ATOMIC ||
                         member.variety == SXML_TYPE_VARIETY_UNION ||
                         member.variety == SXML_TYPE_VARIETY_ENUMERATION;
        }
        break;
    }
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        break;
    }
    return SXML_STATUS_OK;
}

static bool sxml_type_is_derived(const sxml_type_registry *registry,
                                 sxml_type_identifier derived_type,
                                 sxml_builtin_type base_type) {
    return sxml_type_registry_is_derived_from(
        registry,
        (sxml_type_pair){.derived_type = derived_type,
                         .base_type = sxml_builtin_type_identifier(base_type)});
}

/* Casts item to the target (allowing nothing empty) and writes the item,
   owned, to *result. */
static sxml_status sxml_coercion_cast(const sxml_atomic_coercion *coercion,
                                      sxml_item item, sxml_sequence *result) {
    const sxml_type_identifier target = coercion->item_type;
    return sxml_cast_make_value(
        coercion->dynamic_context->item_heap,
        &(sxml_cast_request){.value = sxml_sequence_from_item(item),
                             .target_type = target,
                             .allows_empty = false,
                             .namespace_bindings = {.namespace_bindings = NULL,
                                                    .namespace_binding_count = 0},
                             .error_record = coercion->error_record},
        result);
}

/*
 * Coerces one atomic item to the required atomic item type (XPath 4.0,
 * section 3.4.3) and writes it, owned, to *result. *is_unchanged is set
 * when the item already matches, and *result is then not written.
 * *does_not_match is set when no rule applies.
 */
typedef struct sxml_atomic_coercion_outcome {
    bool is_unchanged;
    bool does_not_match;
    sxml_sequence result;
} sxml_atomic_coercion_outcome;

static sxml_status sxml_coerce_atomic_item(const sxml_atomic_coercion *coercion,
                                           sxml_item item,
                                           sxml_atomic_coercion_outcome *outcome) {
    *outcome = (sxml_atomic_coercion_outcome){.is_unchanged = false,
                                              .does_not_match = false,
                                              .result = sxml_sequence_empty()};
    sxml_type_match match = {.is_match = false};
    sxml_status status = sxml_evaluator_match_sequence_type(
        coercion->dynamic_context,
        &(sxml_type_match_request){
            .value = sxml_sequence_from_item(item),
            .sequence_type = {.item_type = coercion->item_type,
                              .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
            .error_record = coercion->error_record},
        &match);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (match.is_match) {
        outcome->is_unchanged = true;
        return SXML_STATUS_OK;
    }
    const sxml_type_identifier annotation = sxml_item_get_type_annotation(item);
    const sxml_type_identifier item_primitive =
        sxml_atomic_item_get_primitive_type(item);
    const sxml_type_identifier target = coercion->item_type;
    if (item_primitive.index == SXML_TYPE_XS_UNTYPED_ATOMIC) {
        /* untypedAtomic is cast to the required type; never to a
           namespace-sensitive one (XPTY0117). */
        if (coercion->primitive_type.index == SXML_TYPE_XS_QNAME ||
            coercion->primitive_type.index == SXML_TYPE_XS_NOTATION) {
            return sxml_error_record_report(
                coercion->error_record, &sxml_namespace_sensitive_coercion_error);
        }
        return sxml_coercion_cast(coercion, item, &outcome->result);
    }
    const sxml_type_registry *registry = coercion->registry;
    const bool is_decimal =
        sxml_type_is_derived(registry, annotation, SXML_TYPE_XS_DECIMAL);
    const bool is_float = item_primitive.index == SXML_TYPE_XS_FLOAT;
    /* Numeric promotion: decimals to xs:float and xs:double, xs:float to
       xs:double (section 3.4.3, B.1). */
    if ((coercion->primitive_type.index == SXML_TYPE_XS_DOUBLE &&
         (is_decimal || is_float)) ||
        (coercion->primitive_type.index == SXML_TYPE_XS_FLOAT && is_decimal)) {
        return sxml_coercion_cast(coercion, item, &outcome->result);
    }
    /* URI promotion: xs:anyURI to xs:string. */
    if (coercion->primitive_type.index == SXML_TYPE_XS_STRING &&
        item_primitive.index == SXML_TYPE_XS_ANY_URI) {
        return sxml_coercion_cast(coercion, item, &outcome->result);
    }
    /* Relabelling (XPath 4.0, section 3.4.3): an item of the target's
       primitive type whose value is in the target's value space becomes an
       instance of the target, as 3 does for xs:positiveInteger; 4.2 is
       not in the value space of xs:integer, so the cast must not change
       the value. */
    if (coercion->primitive_type.index != SXML_TYPE_NONE &&
        coercion->primitive_type.index == item_primitive.index) {
        sxml_sequence cast = sxml_sequence_empty();
        status = sxml_cast_make_value(
            coercion->dynamic_context->item_heap,
            &(sxml_cast_request){.value = sxml_sequence_from_item(item),
                                 .target_type = target,
                                 .allows_empty = false,
                                 .namespace_bindings = {.namespace_bindings = NULL,
                                                        .namespace_binding_count =
                                                            0},
                                 .error_record = NULL},
            &cast);
        if (status == SXML_STATUS_DYNAMIC_ERROR ||
            status == SXML_STATUS_TYPE_ERROR) {
            outcome->does_not_match = true;
            return SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!sxml_atomic_item_is_atomic_equal(cast.content, item)) {
            sxml_sequence_release(cast);
            outcome->does_not_match = true;
            return SXML_STATUS_OK;
        }
        outcome->result = cast;
        return SXML_STATUS_OK;
    }
    outcome->does_not_match = true;
    return SXML_STATUS_OK;
}

/* Reports the coercion error of role. */
static sxml_status sxml_report_coercion_error(sxml_ir_coercion_role role,
                                              sxml_error_record *error_record) {
    switch (role) {
    case SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER:
        return sxml_error_record_report(error_record,
                                        &sxml_template_parameter_error);
    case SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT:
    case SXML_IR_COERCION_ROLE_FUNCTION_RESULT:
    case SXML_IR_COERCION_ROLE_VARIABLE:
        break;
    }
    return sxml_error_record_report(error_record, &sxml_coercion_error);
}

/* The atomic part of coercion: atomizes request->value and coerces each
   item, writing the new value, owned, to *result. */
static sxml_status sxml_coerce_atomic_value(sxml_dynamic_context *dynamic_context,
                                            const sxml_coercion_request *request,
                                            sxml_sequence *result) {
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(dynamic_context->item_heap);
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(
        registry, request->sequence_type.item_type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_atomic_coercion coercion = {
        .dynamic_context = dynamic_context,
        .registry = registry,
        .item_type = request->sequence_type.item_type,
        .primitive_type = description.variety == SXML_TYPE_VARIETY_ATOMIC
                              ? description.primitive_type
                              : sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .error_record = request->error_record};
    sxml_sequence atomized = sxml_sequence_empty();
    status = sxml_evaluator_atomize(
        dynamic_context,
        &(sxml_atomization_request){.value = request->value,
                                    .error_record = request->error_record},
        &atomized);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_builder *builder = NULL;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(atomized);
    uint64_t item_index = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, request->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_coercion_outcome outcome;
        status = sxml_coerce_atomic_item(&coercion, step.item, &outcome);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (outcome.does_not_match) {
            status = sxml_report_coercion_error(request->role, request->error_record);
            break;
        }
        if (!outcome.is_unchanged && builder == NULL) {
            /* The first changed item: copy the items before it. */
            status = sxml_sequence_builder_create(
                dynamic_context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap =
                                                           dynamic_context->item_heap},
                &builder);
            for (uint64_t earlier_index = 0;
                 status == SXML_STATUS_OK && earlier_index < item_index;
                 earlier_index += 1) {
                sxml_item earlier = {.representation = 0};
                status = sxml_sequence_get_item(
                    atomized,
                    (sxml_item_request){.item_index = earlier_index,
                                        .error_record = request->error_record},
                    &earlier);
                if (status == SXML_STATUS_OK) {
                    status = sxml_sequence_builder_append_item(builder, earlier);
                }
            }
        }
        if (status == SXML_STATUS_OK && builder != NULL) {
            if (outcome.is_unchanged) {
                status = sxml_sequence_builder_append_item(builder, step.item);
            } else {
                status = sxml_sequence_builder_append_sequence(
                    builder, outcome.result, request->error_record);
            }
        }
        if (!outcome.is_unchanged) {
            sxml_sequence_release(outcome.result);
        }
        item_index += 1;
    }
    sxml_sequence value = atomized;
    if (status == SXML_STATUS_OK && builder != NULL) {
        status = sxml_sequence_builder_finish(builder, &value);
        sxml_sequence_release(atomized);
    } else if (status != SXML_STATUS_OK) {
        sxml_sequence_release(atomized);
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}

/* Function coercion (XPath 4.0, section 3.4.4): each function item that
   does not match the required function type is wrapped in a coerced
   function of that type. Writes the new value, owned, to *result. */
static sxml_status sxml_coerce_function_value(sxml_dynamic_context *dynamic_context,
                                              const sxml_coercion_request *request,
                                              sxml_sequence *result) {
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(dynamic_context->item_heap);
    const sxml_type_identifier required = request->sequence_type.item_type;
    sxml_type_description required_description;
    sxml_status status =
        sxml_type_registry_describe_type(registry, required, &required_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_function_type *function_type =
        &required_description.variety_details.function_type;
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        dynamic_context->allocator,
        &(sxml_sequence_builder_configuration){.item_heap =
                                                   dynamic_context->item_heap},
        &builder);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(request->value);
    bool has_changed = false;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, request->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (!sxml_item_is_function_item(step.item) ||
            sxml_type_registry_is_subtype(
                registry,
                (sxml_type_pair){.derived_type =
                                     sxml_item_get_type_annotation(step.item),
                                 .base_type = required})) {
            /* Already of the type, or not a function: the final match
               decides. */
            status = sxml_sequence_builder_append_item(builder, step.item);
            continue;
        }
        sxml_function_item_description description;
        status = sxml_function_item_describe(step.item, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        /* XPath 4.0, section 3.4.4: a function of lower arity is accepted
           and called with the leading arguments only; a higher arity is a
           type error. */
        if (description.arity >
            function_type->parameter_types.sequence_type_count) {
            status = sxml_error_record_report(request->error_record,
                                              &sxml_function_arity_error);
            break;
        }
        sxml_item coerced = {.representation = 0};
        status = sxml_function_item_make(
            dynamic_context->item_heap,
            &(sxml_function_item_request){
                .function_name = description.function_name,
                .signature = required,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = &(sxml_sequence){step.item},
                                    .sequence_count = 1},
                .body = {.state = NULL,
                         .operations =
                             &sxml_evaluator_coerced_function_body_operations}},
            &coerced);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, coerced);
            sxml_item_release(coerced);
            has_changed = true;
        }
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        if (has_changed) {
            status = sxml_sequence_builder_finish(builder, &value);
        } else {
            sxml_sequence_retain(request->value);
            value = request->value;
        }
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}

sxml_status sxml_evaluator_coerce(sxml_dynamic_context *dynamic_context,
                                  const sxml_coercion_request *request,
                                  sxml_sequence *result) {
    const sxml_sequence_type sequence_type = request->sequence_type;
    if (sequence_type.item_type.index == SXML_TYPE_ITEM &&
        sequence_type.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE) {
        sxml_sequence_retain(request->value);
        *result = request->value;
        return SXML_STATUS_OK;
    }
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(dynamic_context->item_heap);
    sxml_atomicity atomicity = {.is_atomic = false};
    sxml_status status = SXML_STATUS_OK;
    if (sequence_type.occurrence != SXML_OCCURRENCE_ZERO) {
        status = sxml_type_is_generalized_atomic(registry, sequence_type.item_type,
                                                 &atomicity);
    }
    const bool is_atomic = atomicity.is_atomic;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence value = sxml_sequence_empty();
    if (is_atomic) {
        status = sxml_coerce_atomic_value(dynamic_context, request, &value);
    } else {
        sxml_type_description description;
        status = sxml_type_registry_describe_type(registry, sequence_type.item_type,
                                                  &description);
        if (status == SXML_STATUS_OK &&
            sequence_type.occurrence != SXML_OCCURRENCE_ZERO &&
            description.variety == SXML_TYPE_VARIETY_FUNCTION &&
            !description.variety_details.function_type.is_any_function) {
            status = sxml_coerce_function_value(dynamic_context, request, &value);
        } else if (status == SXML_STATUS_OK) {
            sxml_sequence_retain(request->value);
            value = request->value;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Finally the value must match (section 3.4, last rule). */
    sxml_type_match match = {.is_match = false};
    status = sxml_evaluator_match_sequence_type(
        dynamic_context,
        &(sxml_type_match_request){.value = value,
                                   .sequence_type = sequence_type,
                                   .error_record = request->error_record},
        &match);
    if (status == SXML_STATUS_OK && !match.is_match) {
        status = sxml_report_coercion_error(request->role, request->error_record);
    }
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    *result = value;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Function items */

sxml_status sxml_evaluator_make_function_definition(
    sxml_dynamic_context *dynamic_context,
    const sxml_function_definition_request *request, sxml_item *item) {
    const sxml_ir_function *function = request->function;
    /* XPath 4.0, section 4.5.6: an inline function has no name (and a
       focus function none either); the body is the IR function, its
       closure the captured values (D38). */
    return sxml_function_item_make(
        dynamic_context->item_heap,
        &(sxml_function_item_request){
            .function_name = function->function_name,
            .signature = function->signature,
            .function_annotations = function->function_annotations,
            .captured_values = request->captured_values,
            .body = {.state = (void *)function,
                     .operations = &sxml_evaluator_function_body_operations}},
        item);
}

/* The name of a native row, interned in the heap's pool. */
static sxml_status sxml_native_function_get_name(sxml_item_heap *item_heap,
                                                 const sxml_native_function *row,
                                                 sxml_name_identifier *name) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    const sxml_name_request request = {
        .namespace_uri = sxml_well_known_namespace_identifier(row->function_namespace),
        .local_name = row->local_name};
    if (sxml_name_pool_find_name(name_pool, &request, name)) {
        return SXML_STATUS_OK;
    }
    return sxml_name_pool_intern_name(name_pool, &request, name);
}

sxml_status sxml_evaluator_make_function_reference(
    sxml_dynamic_context *dynamic_context,
    const sxml_function_reference_request *request, sxml_item *item) {
    const sxml_ir_function_reference *reference = request->function_reference;
    const sxml_ir_function_target target = reference->target;
    sxml_function_item_request item_request = {
        .function_name = {.index = 0},
        .signature = reference->signature,
        .function_annotations = {.function_annotations = NULL,
                                 .function_annotation_count = 0},
        .captured_values = {.sequences = NULL, .sequence_count = 0},
        .body = {.state = NULL, .operations = NULL}};
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence constructor_type = sxml_sequence_empty();
    switch (target.target_kind) {
    case SXML_IR_FUNCTION_TARGET_KIND_NATIVE:
        status = sxml_native_function_get_name(
            dynamic_context->item_heap, target.native_function,
            &item_request.function_name);
        item_request.body = (sxml_function_body){
            .state = (void *)target.native_function,
            .operations = &sxml_native_function_body_operations};
        break;
    case SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION: {
        const sxml_ir_function *function =
            sxml_ir_unit_get_function(request->unit, target.function);
        item_request.function_name = function->function_name;
        item_request.function_annotations = function->function_annotations;
        item_request.body = (sxml_function_body){
            .state = (void *)function,
            .operations = &sxml_evaluator_function_body_operations};
        break;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR: {
        /* The name of a constructor function is the type's (XPath 4.0,
           section 4.5.1.3). */
        sxml_type_description description;
        status = sxml_type_registry_describe_type(
            sxml_item_heap_get_type_registry(dynamic_context->item_heap),
            target.constructor_type, &description);
        item_request.function_name = description.type_name;
        constructor_type = sxml_sequence_from_item(
            sxml_atomic_item_from_integer((int64_t)target.constructor_type.index));
        item_request.captured_values =
            (sxml_sequence_view){.sequences = &constructor_type, .sequence_count = 1};
        item_request.body = (sxml_function_body){
            .state = NULL, .operations = &sxml_evaluator_constructor_body_operations};
        break;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_NONE:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_function_item_make(dynamic_context->item_heap, &item_request, item);
}

sxml_status sxml_evaluator_describe_call_signature(
    sxml_dynamic_context *dynamic_context, sxml_item function_item,
    sxml_call_signature *signature) {
    if (!sxml_item_is_function_item(function_item)) {
        return sxml_error_record_report(dynamic_context->error_record,
                                        &sxml_not_a_function_error);
    }
    *signature = (sxml_call_signature){
        .has_function_type = false,
        .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
        .result_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    if (sxml_item_get_kind(function_item) != SXML_ITEM_KIND_FUNCTION) {
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(
        sxml_item_heap_get_type_registry(dynamic_context->item_heap),
        sxml_item_get_type_annotation(function_item), &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.variety == SXML_TYPE_VARIETY_FUNCTION &&
        !description.variety_details.function_type.is_any_function) {
        signature->has_function_type = true;
        signature->parameter_types =
            description.variety_details.function_type.parameter_types;
        signature->result_type = description.variety_details.function_type.result_type;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Bodies */

/* IR function bodies keep their unit alive (D38, D55). */
static void sxml_ir_body_retain(void *state) {
    const sxml_ir_function *function = state;
    sxml_ir_unit_retain(function->unit);
}

static void sxml_ir_body_release(void *state) {
    const sxml_ir_function *function = state;
    sxml_ir_unit_release(function->unit);
}

/* Called from outside an evaluation: the machine runs the call (inside
   one, the machine never calls this; see "Reentry"). */
static sxml_status sxml_machine_body_call(void *state,
                                          const sxml_function_call *call,
                                          sxml_sequence *result) {
    (void)state;
    return sxml_evaluator_call_function_item(call, result);
}

const sxml_function_body_operations sxml_evaluator_function_body_operations = {
    .call = sxml_machine_body_call,
    .retain = sxml_ir_body_retain,
    .release = sxml_ir_body_release,
};

const sxml_function_body_operations
    sxml_evaluator_coerced_function_body_operations = {
        .call = sxml_machine_body_call,
        .retain = NULL,
        .release = NULL,
};

/* A native row's body: leaf rows are called directly, resumable ones run
   on the machine. */
static sxml_status sxml_native_body_call(void *state,
                                         const sxml_function_call *call,
                                         sxml_sequence *result) {
    const sxml_native_function *row = state;
    if (row->call != NULL) {
        return row->call(call, result);
    }
    if (row->resumable_body == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_evaluator_call_function_item(call, result);
}

const sxml_function_body_operations sxml_native_function_body_operations = {
    .call = sxml_native_body_call,
    .retain = NULL,
    .release = NULL,
};

/* xs:T#1: a cast that allows the empty sequence (XPath 4.0, section
   4.5.1.3); the argument was coerced to xs:anyAtomicType? by the call. */
static sxml_status sxml_constructor_body_call(void *state,
                                              const sxml_function_call *call,
                                              sxml_sequence *result) {
    (void)state;
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK || description.captured_values.sequence_count != 1 ||
        call->arguments.sequence_count != 1) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_item type_item = description.captured_values.sequences[0].content;
    const sxml_type_identifier target = {.index = (uint32_t)type_item.payload.integer};
    return sxml_cast_make_value(
        call->item_heap,
        &(sxml_cast_request){.value = call->arguments.sequences[0],
                             .target_type = target,
                             .allows_empty = true,
                             .namespace_bindings = {.namespace_bindings = NULL,
                                                    .namespace_binding_count = 0},
                             .error_record = call->error_record},
        result);
}

const sxml_function_body_operations sxml_evaluator_constructor_body_operations = {
    .call = sxml_constructor_body_call,
    .retain = NULL,
    .release = NULL,
};
