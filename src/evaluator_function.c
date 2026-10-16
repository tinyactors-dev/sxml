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

/*
 * A range of integers (D62) against a sequence type, in O(1): every item
 * is an xs:integer, so the range matches when xs:integer is a subtype of
 * the item type and the occurrence allows the count. Sets
 * *decision->is_decided only then; other types are matched item by item.
 */
typedef struct sxml_range_decision {
    bool is_decided;
    bool is_match;
} sxml_range_decision;

static sxml_status sxml_decide_range_match(const sxml_type_registry *registry,
                                           const sxml_type_match_request *request,
                                           sxml_range_decision *decision) {
    *decision = (sxml_range_decision){.is_decided = false, .is_match = false};
    if (sxml_sequence_get_representation(request->value) !=
        SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        return SXML_STATUS_OK;
    }
    const sxml_sequence_type type = request->sequence_type;
    const bool holds_integers =
        type.item_type.index == SXML_TYPE_ITEM ||
        sxml_type_registry_is_subtype(
            registry,
            (sxml_type_pair){.derived_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                             .base_type = type.item_type});
    if (!holds_integers && type.occurrence != SXML_OCCURRENCE_ZERO) {
        return SXML_STATUS_OK;
    }
    uint64_t item_count = 0;
    const sxml_status status =
        sxml_sequence_count(request->value, request->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decision->is_decided = true;
    decision->is_match = sxml_occurrence_allows(type.occurrence, item_count) &&
                         (holds_integers || item_count == 0);
    return SXML_STATUS_OK;
}

sxml_status sxml_evaluator_match_sequence_type(
    sxml_dynamic_context *dynamic_context, const sxml_type_match_request *request,
    sxml_type_match *match) {
    sxml_range_decision decision;
    sxml_status range_status = sxml_decide_range_match(
        sxml_item_heap_get_type_registry(dynamic_context->item_heap), request, &decision);
    if (range_status != SXML_STATUS_OK || decision.is_decided) {
        if (range_status == SXML_STATUS_OK) {
            match->is_match = decision.is_match;
        }
        return range_status;
    }
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
    /* True when item_type is an enumeration type. */
    bool is_enumeration;
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
       xs:double; URI promotion: xs:anyURI to xs:string. The required type
       must be the "to" type exactly (XPath 4.0, section 3.4.2, implicit
       casting): an xs:anyURI is not cast to xs:NCName. */
    if ((target.index == SXML_TYPE_XS_DOUBLE && (is_decimal || is_float)) ||
        (target.index == SXML_TYPE_XS_FLOAT && is_decimal) ||
        (target.index == SXML_TYPE_XS_STRING &&
         item_primitive.index == SXML_TYPE_XS_ANY_URI)) {
        return sxml_coercion_cast(coercion, item, &outcome->result);
    }
    /* An enumeration type and an xs:anyURI: cast to xs:string, then the
       value must be one of the enumeration's (section 3.4.2). */
    if (coercion->is_enumeration && item_primitive.index == SXML_TYPE_XS_ANY_URI) {
        sxml_sequence string = sxml_sequence_empty();
        status = sxml_cast_make_value(
            coercion->dynamic_context->item_heap,
            &(sxml_cast_request){.value = sxml_sequence_from_item(item),
                                 .target_type =
                                     sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                 .allows_empty = false,
                                 .namespace_bindings = {.namespace_bindings = NULL,
                                                        .namespace_binding_count = 0},
                                 .error_record = coercion->error_record},
            &string);
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_match_sequence_type(
                coercion->dynamic_context,
                &(sxml_type_match_request){
                    .value = string,
                    .sequence_type = {.item_type = target,
                                      .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                    .error_record = coercion->error_record},
                &match);
        }
        if (status != SXML_STATUS_OK || !match.is_match) {
            sxml_sequence_release(string);
            outcome->does_not_match = status == SXML_STATUS_OK;
            return status;
        }
        outcome->result = string;
        return SXML_STATUS_OK;
    }
    /* The rest of the implicit casting table of XPath 4.0, section 3.4.2,
       where the required type is the "to" type exactly: xs:double and
       xs:float to xs:decimal, xs:double to xs:float, xs:string to
       xs:anyURI, and between xs:hexBinary and xs:base64Binary. */
    const bool is_double = item_primitive.index == SXML_TYPE_XS_DOUBLE;
    if ((target.index == SXML_TYPE_XS_DECIMAL && (is_double || is_float)) ||
        (target.index == SXML_TYPE_XS_FLOAT && is_double) ||
        (target.index == SXML_TYPE_XS_ANY_URI &&
         item_primitive.index == SXML_TYPE_XS_STRING) ||
        (target.index == SXML_TYPE_XS_HEX_BINARY &&
         item_primitive.index == SXML_TYPE_XS_BASE64_BINARY) ||
        (target.index == SXML_TYPE_XS_BASE64_BINARY &&
         item_primitive.index == SXML_TYPE_XS_HEX_BINARY)) {
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

/*
 * Function coercion of one function item (XPath 4.0, section 3.4.4): an
 * item that does not match the typed function type required is wrapped
 * in a coerced function of that type, which coerces its arguments and
 * result on each call. A function of lower arity is accepted and called
 * with the leading arguments only; a higher arity is a type error. Writes
 * the wrapped item, owned, to *wrapped.
 */
typedef struct sxml_function_wrapping {
    sxml_item function_item;
    sxml_type_identifier required_type;
    sxml_error_record *error_record;
} sxml_function_wrapping;

static sxml_status sxml_wrap_function_item(sxml_dynamic_context *dynamic_context,
                                           const sxml_function_wrapping *wrapping,
                                           sxml_item *wrapped) {
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(dynamic_context->item_heap);
    sxml_type_description required_description;
    sxml_status status = sxml_type_registry_describe_type(
        registry, wrapping->required_type, &required_description);
    sxml_function_item_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_item_describe(wrapping->function_item, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.arity > required_description.variety_details.function_type
                                .parameter_types.sequence_type_count) {
        return sxml_error_record_report(wrapping->error_record, &sxml_function_arity_error);
    }
    return sxml_function_item_make(
        dynamic_context->item_heap,
        &(sxml_function_item_request){
            .function_name = description.function_name,
            .signature = wrapping->required_type,
            .function_annotations = {.function_annotations = NULL,
                                     .function_annotation_count = 0},
            .captured_values = {.sequences = &(sxml_sequence){wrapping->function_item},
                                .sequence_count = 1},
            .body = {.state = NULL,
                     .operations = &sxml_evaluator_coerced_function_body_operations}},
        wrapped);
}

/*
 * The coercion rules (XPath 4.0, section 3.4) on an explicit stack (D71):
 * the frames of glossary `coercion_frame`. SEQUENCE coerces each item of a
 * value to an item type, then checks the count; ITEM applies the item
 * coercion rules of section 3.4.2 to one item, deciding at once or turning
 * into another frame in place; CHOICE tries the alternatives of a choice
 * or union type in order (a failure above it unwinds to it); ARRAY_MEMBERS,
 * MAP_ENTRIES and RECORD_FIELDS coerce the parts of an array, a map or a
 * record and build the new item when a part changed. Every frame finishes
 * with a result for the frame below: "unchanged", or a new value.
 */
typedef enum sxml_coercion_frame_kind {
    SXML_COERCION_FRAME_KIND_SEQUENCE,
    SXML_COERCION_FRAME_KIND_ITEM,
    SXML_COERCION_FRAME_KIND_CHOICE,
    SXML_COERCION_FRAME_KIND_ARRAY_MEMBERS,
    SXML_COERCION_FRAME_KIND_MAP_ENTRIES,
    SXML_COERCION_FRAME_KIND_RECORD_FIELDS,
} sxml_coercion_frame_kind;

/* The steps of a CHOICE. */
typedef enum sxml_choice_phase {
    /* Looking for an alternative the item matches. */
    SXML_CHOICE_PHASE_SCANNING,
    /* Trying the alternatives in order, the item matching none. */
    SXML_CHOICE_PHASE_TRYING,
    /* Coercing to the alternative the item matches. */
    SXML_CHOICE_PHASE_CONVERTING,
} sxml_choice_phase;

typedef struct sxml_coercion_frame {
    sxml_coercion_frame_kind frame_kind;
    /* Where this frame's errors go: the request's record, or the scratch
       record for an alternative after the first. */
    sxml_error_record *error_record;
    /* SEQUENCE: the value (owned when owns_input) and its required type;
       the count is checked only when checks_occurrence. is_derived is set
       for a value made from the item being coerced (atomized, or a JNode's
       jvalue), which is never "unchanged". */
    sxml_sequence input;
    bool owns_input;
    bool checks_occurrence;
    bool is_derived;
    sxml_sequence_type sequence_type;
    sxml_sequence_iterator iterator;
    uint64_t item_index;
    sxml_item current_item;
    /* Owned; made at the first changed item. */
    sxml_sequence_builder *builder;
    /* ITEM, CHOICE, ARRAY_MEMBERS, MAP_ENTRIES, RECORD_FIELDS: the item,
       borrowed. ITEM, CHOICE: its required type; MAP_ENTRIES: the key type
       (sequence_type is the value type); RECORD_FIELDS: the record type;
       ARRAY_MEMBERS: sequence_type is the member type. */
    sxml_item item;
    sxml_type_identifier item_type;
    /* CHOICE: the alternatives; RECORD_FIELDS: the fields. Borrowed from
       the registry. */
    sxml_type_identifier_view alternatives;
    const sxml_record_field *record_fields;
    uint32_t phase;
    size_t next_index;
    size_t part_count;
    bool has_changed;
    /* ARRAY_MEMBERS: the members; MAP_ENTRIES: key and value of each
       entry; RECORD_FIELDS: the field values. Owned, stored_count of them
       set. */
    sxml_sequence *parts;
    size_t stored_count;
    /* RECORD_FIELDS, MAP_ENTRIES: the value being coerced, borrowed. */
    sxml_sequence current_value;
    /* CHOICE: how the first alternative failed. */
    sxml_status first_failure;
} sxml_coercion_frame;

typedef struct sxml_coercion_engine {
    sxml_dynamic_context *dynamic_context;
    const sxml_type_registry *registry;
    sxml_ir_coercion_role role;
    sxml_error_record scratch_record;
    sxml_coercion_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
    /* What the frame popped last handed down: a result (owned value unless
       is_unchanged) or, after an unwind to a CHOICE, a failure. */
    bool has_result;
    bool is_unchanged;
    sxml_sequence result;
    bool has_failure;
    sxml_status failure_status;
} sxml_coercion_engine;

static sxml_coercion_frame sxml_coercion_sequence_frame(sxml_sequence value,
                                                        sxml_sequence_type type,
                                                        sxml_error_record *error_record) {
    sxml_coercion_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.frame_kind = SXML_COERCION_FRAME_KIND_SEQUENCE;
    frame.error_record = error_record;
    frame.input = value;
    frame.checks_occurrence = true;
    frame.sequence_type = type;
    frame.iterator = sxml_sequence_iterator_start(value);
    frame.current_value = sxml_sequence_empty();
    return frame;
}

static sxml_coercion_frame sxml_coercion_item_frame(sxml_item item, sxml_type_identifier type,
                                                    sxml_error_record *error_record) {
    sxml_coercion_frame frame =
        sxml_coercion_sequence_frame(sxml_sequence_empty(),
                                     (sxml_sequence_type){.item_type = type,
                                                          .occurrence =
                                                              SXML_OCCURRENCE_EXACTLY_ONE},
                                     error_record);
    frame.frame_kind = SXML_COERCION_FRAME_KIND_ITEM;
    frame.item = item;
    frame.item_type = type;
    return frame;
}

static sxml_status sxml_coercion_push(sxml_coercion_engine *engine, sxml_coercion_frame frame) {
    if (engine->frame_count == engine->frame_capacity) {
        void *elements = engine->frames;
        sxml_status status = sxml_evaluator_grow(
            engine->dynamic_context->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &engine->frame_capacity,
                                     .element_count = engine->frame_count,
                                     .required_count = engine->frame_count + 1,
                                     .element_byte_count = sizeof(sxml_coercion_frame),
                                     .alignment = alignof(sxml_coercion_frame)});
        if (status != SXML_STATUS_OK) {
            if (frame.owns_input) {
                sxml_sequence_release(frame.input);
            }
            return status;
        }
        engine->frames = elements;
    }
    engine->frames[engine->frame_count] = frame;
    engine->frame_count += 1;
    return SXML_STATUS_OK;
}

static void sxml_coercion_release_frame(sxml_coercion_engine *engine,
                                        sxml_coercion_frame *frame) {
    if (frame->owns_input) {
        sxml_sequence_release(frame->input);
        frame->owns_input = false;
    }
    sxml_sequence_builder_destroy(frame->builder);
    frame->builder = NULL;
    if (frame->parts != NULL) {
        for (size_t part_index = 0; part_index < frame->stored_count; part_index += 1) {
            sxml_sequence_release(frame->parts[part_index]);
        }
        sxml_allocator_release(engine->dynamic_context->allocator, frame->parts);
        frame->parts = NULL;
    }
}

/* Pops the top frame, handing down result (owned; ignored when
   is_unchanged). */
typedef struct sxml_coercion_result {
    bool is_unchanged;
    sxml_sequence value;
} sxml_coercion_result;

static sxml_status sxml_coercion_finish(sxml_coercion_engine *engine,
                                        sxml_coercion_result result) {
    sxml_coercion_frame *frame = &engine->frames[engine->frame_count - 1];
    sxml_coercion_release_frame(engine, frame);
    engine->frame_count -= 1;
    engine->has_result = true;
    engine->is_unchanged = result.is_unchanged;
    engine->result = result.is_unchanged ? sxml_sequence_empty() : result.value;
    if (result.is_unchanged) {
        sxml_sequence_release(result.value);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_coercion_finish_unchanged(sxml_coercion_engine *engine) {
    return sxml_coercion_finish(
        engine, (sxml_coercion_result){.is_unchanged = true, .value = sxml_sequence_empty()});
}

/* Takes the result handed down: *changed_value is the new value (owned),
   or empty when unchanged (then *is_unchanged). */
static sxml_coercion_result sxml_coercion_take_result(sxml_coercion_engine *engine) {
    const sxml_coercion_result result = {.is_unchanged = engine->is_unchanged,
                                         .value = engine->result};
    engine->has_result = false;
    engine->is_unchanged = false;
    engine->result = sxml_sequence_empty();
    return result;
}

/* Allocates count empty parts for the top frame. */
static sxml_status sxml_coercion_allocate_parts(sxml_coercion_engine *engine,
                                                sxml_coercion_frame *frame, size_t count) {
    frame->parts = NULL;
    frame->stored_count = 0;
    if (count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(count, sizeof(sxml_sequence), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            engine->dynamic_context->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_sequence)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        frame->parts = memory;
    }
    return status;
}

/* Stores a part: the coerced value, or original (retained) when the
   result is unchanged. */
static void sxml_coercion_store_part(sxml_coercion_frame *frame,
                                     sxml_coercion_result result, sxml_sequence original) {
    if (result.is_unchanged) {
        sxml_sequence_retain(original);
        frame->parts[frame->stored_count] = original;
    } else {
        frame->parts[frame->stored_count] = result.value;
        frame->has_changed = true;
    }
    frame->stored_count += 1;
}

/* SEQUENCE: each item against the item type, then the count (section
   3.4: the cardinality of the coerced value V'). */
static sxml_status sxml_coercion_step_sequence(sxml_coercion_engine *engine,
                                               sxml_coercion_frame *frame) {
    sxml_status status = SXML_STATUS_OK;
    if (engine->has_result) {
        const sxml_coercion_result result = sxml_coercion_take_result(engine);
        if (!result.is_unchanged && frame->builder == NULL) {
            /* The first changed item: copy the items before it. */
            status = sxml_sequence_builder_create(
                engine->dynamic_context->allocator,
                &(sxml_sequence_builder_configuration){
                    .item_heap = engine->dynamic_context->item_heap},
                &frame->builder);
            for (uint64_t earlier_index = 0;
                 status == SXML_STATUS_OK && earlier_index < frame->item_index;
                 earlier_index += 1) {
                sxml_item earlier = {.representation = 0};
                status = sxml_sequence_get_item(
                    frame->input,
                    (sxml_item_request){.item_index = earlier_index,
                                        .error_record = frame->error_record},
                    &earlier);
                if (status == SXML_STATUS_OK) {
                    status = sxml_sequence_builder_append_item(frame->builder, earlier);
                }
            }
        }
        if (status == SXML_STATUS_OK && frame->builder != NULL) {
            status = result.is_unchanged
                         ? sxml_sequence_builder_append_item(frame->builder,
                                                             frame->current_item)
                         : sxml_sequence_builder_append_sequence(
                               frame->builder, result.value, frame->error_record);
        }
        sxml_sequence_release(result.value);
        frame->item_index += 1;
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_iteration_step step = {.has_item = false};
    if (frame->sequence_type.occurrence != SXML_OCCURRENCE_ZERO &&
        frame->sequence_type.item_type.index != SXML_TYPE_ITEM) {
        status = sxml_sequence_iterator_next(&frame->iterator, frame->error_record, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (step.has_item) {
        frame->current_item = step.item;
        return sxml_coercion_push(engine,
                                  sxml_coercion_item_frame(step.item,
                                                           frame->sequence_type.item_type,
                                                           frame->error_record));
    }
    sxml_sequence value = frame->input;
    if (frame->builder != NULL) {
        status = sxml_sequence_builder_finish(frame->builder, &value);
    } else {
        sxml_sequence_retain(value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (frame->checks_occurrence) {
        uint64_t item_count = 0;
        status = sxml_sequence_count(value, frame->error_record, &item_count);
        if (status == SXML_STATUS_OK &&
            !sxml_occurrence_allows(frame->sequence_type.occurrence, item_count)) {
            status = sxml_report_coercion_error(engine->role, frame->error_record);
        }
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(value);
            return status;
        }
    }
    const bool is_unchanged = frame->builder == NULL && !frame->is_derived;
    return sxml_coercion_finish(engine,
                                (sxml_coercion_result){.is_unchanged = is_unchanged,
                                                       .value = value});
}

/* True when an item that matches type may still be changed by coercing
   it to type: typed function types (function coercion), record types
   (entry order and annotation), typed arrays and maps (their parts), and
   choices with such alternatives. */
static sxml_status sxml_type_converts_matching_items(const sxml_type_registry *registry,
                                                     sxml_type_identifier type,
                                                     sxml_atomicity *converts) {
    bool *does_convert = &converts->is_atomic;
    *does_convert = false;
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(registry, type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_FUNCTION:
        *does_convert = !description.variety_details.function_type.is_any_function;
        break;
    case SXML_TYPE_VARIETY_RECORD:
        *does_convert = !description.variety_details.record_type.is_any_record;
        break;
    case SXML_TYPE_VARIETY_ARRAY:
        *does_convert = !description.variety_details.array_type.is_any_array;
        break;
    case SXML_TYPE_VARIETY_MAP:
        *does_convert = !description.variety_details.map_type.is_any_map;
        break;
    case SXML_TYPE_VARIETY_CHOICE: {
        const sxml_type_identifier_view members = description.variety_details.member_types;
        for (size_t member_index = 0;
             !*does_convert && member_index < members.type_identifier_count;
             member_index += 1) {
            sxml_type_description member;
            status = sxml_type_registry_describe_type(
                registry, members.type_identifiers[member_index], &member);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            /* One level is looked at; anything that is not atomic or a
               node test may convert. */
            *does_convert = member.variety == SXML_TYPE_VARIETY_FUNCTION ||
                            member.variety == SXML_TYPE_VARIETY_RECORD ||
                            member.variety == SXML_TYPE_VARIETY_ARRAY ||
                            member.variety == SXML_TYPE_VARIETY_MAP ||
                            member.variety == SXML_TYPE_VARIETY_CHOICE;
        }
        break;
    }
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        break;
    }
    return SXML_STATUS_OK;
}

/* Whether item matches type (one item). */
typedef struct sxml_coercion_question {
    sxml_item item;
    sxml_type_identifier type;
} sxml_coercion_question;

static sxml_status sxml_coercion_item_matches(sxml_coercion_engine *engine,
                                              sxml_coercion_question question,
                                              sxml_type_match *match) {
    return sxml_evaluator_match_sequence_type(
        engine->dynamic_context,
        &(sxml_type_match_request){
            .value = sxml_sequence_from_item(question.item),
            .sequence_type = {.item_type = question.type,
                              .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
            .error_record = NULL},
        match);
}

/* Turns the ITEM frame into a SEQUENCE over a value made from its item. */
/* A value made from an item; is_owned when the frame takes it over. */
typedef struct sxml_derived_value {
    sxml_sequence value;
    bool is_owned;
} sxml_derived_value;

static void sxml_coercion_become_derived_sequence(sxml_coercion_frame *frame,
                                                  sxml_derived_value derived) {
    const sxml_sequence value = derived.value;
    const bool owns_value = derived.is_owned;
    const sxml_type_identifier type = frame->item_type;
    *frame = sxml_coercion_sequence_frame(
        value,
        (sxml_sequence_type){.item_type = type, .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
        frame->error_record);
    frame->owns_input = owns_value;
    frame->checks_occurrence = false;
    frame->is_derived = true;
}

/* ITEM: the item coercion rules of XPath 4.0, section 3.4.2, in order. */
static sxml_status sxml_coercion_step_item(sxml_coercion_engine *engine,
                                           sxml_coercion_frame *frame) {
    const sxml_item item = frame->item;
    const sxml_type_identifier type = frame->item_type;
    if (type.index == SXML_TYPE_ITEM) {
        return sxml_coercion_finish_unchanged(engine);
    }
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(engine->registry, type, &description);
    sxml_atomicity atomicity = {.is_atomic = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_is_generalized_atomic(engine->registry, type, &atomicity);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_atomic_item = sxml_item_is_atomic(item);
    /* A generalized atomic type and an item that is not atomic: atomize,
       then coerce each atomic item. */
    if (atomicity.is_atomic && !is_atomic_item) {
        sxml_sequence atomized = sxml_sequence_empty();
        status = sxml_evaluator_atomize(
            engine->dynamic_context,
            &(sxml_atomization_request){.value = sxml_sequence_from_item(item),
                                        .error_record = frame->error_record},
            &atomized);
        if (status == SXML_STATUS_OK) {
            sxml_coercion_become_derived_sequence(
                frame, (sxml_derived_value){.value = atomized, .is_owned = true});
        }
        return status;
    }
    /* A choice or union type: the first alternative the item matches, or
       each alternative in turn. */
    if (description.variety == SXML_TYPE_VARIETY_CHOICE ||
        description.variety == SXML_TYPE_VARIETY_UNION) {
        frame->frame_kind = SXML_COERCION_FRAME_KIND_CHOICE;
        frame->alternatives = description.variety_details.member_types;
        frame->phase = SXML_CHOICE_PHASE_SCANNING;
        frame->next_index = 0;
        return SXML_STATUS_OK;
    }
    if (is_atomic_item && (description.variety == SXML_TYPE_VARIETY_ATOMIC ||
                           description.variety == SXML_TYPE_VARIETY_ENUMERATION)) {
        const sxml_atomic_coercion coercion = {
            .dynamic_context = engine->dynamic_context,
            .registry = engine->registry,
            .item_type = type,
            .primitive_type = description.variety == SXML_TYPE_VARIETY_ATOMIC
                                  ? description.primitive_type
                                  : sxml_builtin_type_identifier(SXML_TYPE_NONE),
            .is_enumeration = description.variety == SXML_TYPE_VARIETY_ENUMERATION,
            .error_record = frame->error_record};
        sxml_atomic_coercion_outcome outcome;
        status = sxml_coerce_atomic_item(&coercion, item, &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (outcome.does_not_match) {
            return sxml_report_coercion_error(engine->role, frame->error_record);
        }
        return sxml_coercion_finish(engine,
                                    (sxml_coercion_result){.is_unchanged = outcome.is_unchanged,
                                                           .value = outcome.result});
    }
    const sxml_item_kind kind = sxml_item_get_kind(item);
    if (kind == SXML_ITEM_KIND_JSON_NODE) {
        /* A JNode that does not match: each item of its jvalue. */
        sxml_type_match match = {.is_match = false};
        status = sxml_coercion_item_matches(
            engine, (sxml_coercion_question){.item = item, .type = type}, &match);
        if (status != SXML_STATUS_OK || match.is_match) {
            return status != SXML_STATUS_OK ? status : sxml_coercion_finish_unchanged(engine);
        }
        sxml_json_node_description node;
        status = sxml_json_node_describe(item, &node);
        if (status == SXML_STATUS_OK) {
            sxml_coercion_become_derived_sequence(
                frame, (sxml_derived_value){.value = node.json_value, .is_owned = false});
        }
        return status;
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ARRAY: {
        const sxml_array_type *array_type = &description.variety_details.array_type;
        if (kind != SXML_ITEM_KIND_ARRAY || array_type->is_any_array) {
            break;
        }
        size_t member_count = 0;
        status = sxml_array_get_size(item, &member_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_coercion_allocate_parts(engine, frame, member_count);
        }
        if (status == SXML_STATUS_OK) {
            frame->frame_kind = SXML_COERCION_FRAME_KIND_ARRAY_MEMBERS;
            frame->sequence_type = array_type->member_type;
            frame->part_count = member_count;
            frame->next_index = 0;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_MAP: {
        const sxml_map_type *map_type = &description.variety_details.map_type;
        if (kind != SXML_ITEM_KIND_MAP || map_type->is_any_map) {
            break;
        }
        size_t entry_count = 0;
        size_t part_count = 0;
        status = sxml_map_get_size(item, &entry_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_size_multiply(entry_count, 2, &part_count);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_coercion_allocate_parts(engine, frame, part_count);
        }
        if (status == SXML_STATUS_OK) {
            frame->frame_kind = SXML_COERCION_FRAME_KIND_MAP_ENTRIES;
            frame->item_type = map_type->key_type;
            frame->sequence_type = map_type->value_type;
            frame->part_count = entry_count;
            frame->next_index = 0;
            frame->phase = 0;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_RECORD: {
        const sxml_record_type *record_type = &description.variety_details.record_type;
        if (kind != SXML_ITEM_KIND_MAP || record_type->is_any_record ||
            record_type->is_nominative ||
            sxml_item_get_type_annotation(item).index == type.index) {
            break;
        }
        /* An entry whose key names no field is a type error (section
           3.4.2: surplus entries are rejected). */
        const sxml_type_matcher key_checker = {.dynamic_context = engine->dynamic_context,
                                               .registry = engine->registry,
                                               .error_record = NULL,
                                               .frames = NULL,
                                               .frame_count = 0,
                                               .frame_capacity = 0};
        sxml_type_match keys_match = {.is_match = false};
        status = sxml_map_keys_are_fields(
            &key_checker, (sxml_record_question){.map = item, .record_type = type},
            &keys_match);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!keys_match.is_match) {
            return sxml_report_coercion_error(engine->role, frame->error_record);
        }
        const size_t field_count = record_type->record_fields.record_field_count;
        status = sxml_coercion_allocate_parts(engine, frame, field_count);
        if (status == SXML_STATUS_OK) {
            frame->frame_kind = SXML_COERCION_FRAME_KIND_RECORD_FIELDS;
            frame->record_fields = record_type->record_fields.record_fields;
            frame->part_count = field_count;
            frame->next_index = 0;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_FUNCTION: {
        /* Function coercion applies even to an item that is already an
           instance of the type (section 3.4.2, note; 3.4.4): only a
           signature that is the type itself needs no wrapper. */
        if (description.variety_details.function_type.is_any_function ||
            !sxml_item_is_function_item(item) ||
            sxml_item_get_type_annotation(item).index == type.index) {
            break;
        }
        sxml_item wrapped = {.representation = 0};
        status = sxml_wrap_function_item(
            engine->dynamic_context,
            &(sxml_function_wrapping){.function_item = item,
                                      .required_type = type,
                                      .error_record = frame->error_record},
            &wrapped);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_coercion_finish(
            engine, (sxml_coercion_result){.is_unchanged = false,
                                           .value = sxml_sequence_from_item(wrapped)});
    }
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_CHOICE:
        break;
    }
    /* No rule applies: the item stays, and the final match decides
       (section 3.4). */
    return sxml_coercion_finish_unchanged(engine);
}

/* CHOICE (section 3.4.2): an item that matches an alternative is coerced
   to the first one it matches; otherwise each alternative is tried in
   order, and the error is the first attempt's. */
static sxml_status sxml_coercion_step_choice(sxml_coercion_engine *engine,
                                             sxml_coercion_frame *frame) {
    const size_t alternative_count = frame->alternatives.type_identifier_count;
    switch ((sxml_choice_phase)frame->phase) {
    case SXML_CHOICE_PHASE_SCANNING:
        for (size_t alternative_index = 0; alternative_index < alternative_count;
             alternative_index += 1) {
            const sxml_type_identifier alternative =
                frame->alternatives.type_identifiers[alternative_index];
            sxml_type_match match = {.is_match = false};
            sxml_status status =
                sxml_coercion_item_matches(
                engine, (sxml_coercion_question){.item = frame->item, .type = alternative},
                &match);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!match.is_match) {
                continue;
            }
            sxml_atomicity converts = {.is_atomic = false};
            status = sxml_type_converts_matching_items(engine->registry, alternative,
                                                       &converts);
            if (status != SXML_STATUS_OK || !converts.is_atomic) {
                return status != SXML_STATUS_OK ? status
                                                : sxml_coercion_finish_unchanged(engine);
            }
            frame->phase = SXML_CHOICE_PHASE_CONVERTING;
            return sxml_coercion_push(
                engine, sxml_coercion_item_frame(frame->item, alternative, frame->error_record));
        }
        if (alternative_count == 0) {
            return sxml_report_coercion_error(engine->role, frame->error_record);
        }
        frame->phase = SXML_CHOICE_PHASE_TRYING;
        frame->next_index = 1;
        return sxml_coercion_push(
            engine, sxml_coercion_item_frame(frame->item, frame->alternatives.type_identifiers[0],
                                             frame->error_record));
    case SXML_CHOICE_PHASE_TRYING:
        if (engine->has_failure) {
            engine->has_failure = false;
            if (frame->next_index == 1) {
                frame->first_failure = engine->failure_status;
            }
            if (frame->next_index == alternative_count) {
                /* Every alternative failed: the first failure stands, in
                   the frame's record. */
                return frame->first_failure;
            }
            const sxml_type_identifier alternative =
                frame->alternatives.type_identifiers[frame->next_index];
            frame->next_index += 1;
            return sxml_coercion_push(
                engine,
                sxml_coercion_item_frame(frame->item, alternative, &engine->scratch_record));
        }
        return sxml_coercion_finish(engine, sxml_coercion_take_result(engine));
    case SXML_CHOICE_PHASE_CONVERTING:
        return sxml_coercion_finish(engine, sxml_coercion_take_result(engine));
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ARRAY_MEMBERS: each member to the member type (section 3.4.2). */
static sxml_status sxml_coercion_step_array(sxml_coercion_engine *engine,
                                            sxml_coercion_frame *frame) {
    if (engine->has_result) {
        sxml_coercion_store_part(frame, sxml_coercion_take_result(engine),
                                 frame->current_value);
    }
    if (frame->next_index == frame->part_count) {
        if (!frame->has_changed) {
            return sxml_coercion_finish_unchanged(engine);
        }
        sxml_item array = {.representation = 0};
        sxml_status status = sxml_array_make(
            engine->dynamic_context->item_heap,
            (sxml_sequence_view){.sequences = frame->parts, .sequence_count = frame->stored_count},
            &array);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_coercion_finish(engine,
                                    (sxml_coercion_result){.is_unchanged = false,
                                                           .value = sxml_sequence_from_item(array)});
    }
    sxml_sequence member = sxml_sequence_empty();
    sxml_status status = sxml_array_get_member(frame->item, frame->next_index, &member);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame->next_index += 1;
    frame->current_value = member;
    return sxml_coercion_push(
        engine, sxml_coercion_sequence_frame(member, frame->sequence_type, frame->error_record));
}

static const sxml_static_error sxml_duplicate_key_coercion_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Coercing the keys of a map makes two of them equal (XPath 4.0, section "
        "3.4.2)"),
};

/* MAP_ENTRIES: each key to the key type, each value to the value type;
   duplicate keys after that are XPTY0004 (section 3.4.2). Phase 0: the
   next key; 1: its result came; 2: the value's result came. */
static sxml_status sxml_coercion_step_map(sxml_coercion_engine *engine,
                                          sxml_coercion_frame *frame) {
    sxml_status status = SXML_STATUS_OK;
    sxml_map_entry entry;
    if (engine->has_result && frame->phase == 1) {
        status = sxml_map_get_entry(frame->item, frame->next_index, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_coercion_store_part(frame, sxml_coercion_take_result(engine),
                                 sxml_sequence_from_item(entry.key));
        frame->phase = 2;
        frame->current_value = entry.value;
        return sxml_coercion_push(engine, sxml_coercion_sequence_frame(entry.value,
                                                                       frame->sequence_type,
                                                                       frame->error_record));
    }
    if (engine->has_result) {
        sxml_coercion_store_part(frame, sxml_coercion_take_result(engine),
                                 frame->current_value);
        frame->next_index += 1;
        frame->phase = 0;
    }
    if (frame->next_index < frame->part_count) {
        status = sxml_map_get_entry(frame->item, frame->next_index, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->phase = 1;
        return sxml_coercion_push(
            engine, sxml_coercion_item_frame(entry.key, frame->item_type, frame->error_record));
    }
    if (!frame->has_changed) {
        return sxml_coercion_finish_unchanged(engine);
    }
    void *memory = NULL;
    size_t byte_count = 0;
    status = sxml_size_multiply(frame->part_count, sizeof(sxml_map_entry), &byte_count);
    if (status == SXML_STATUS_OK && byte_count > 0) {
        status = sxml_allocator_allocate(
            engine->dynamic_context->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_map_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_entry *entries = memory;
    for (size_t entry_index = 0; entries != NULL && entry_index < frame->part_count;
         entry_index += 1) {
        const sxml_sequence key = frame->parts[2 * entry_index];
        if (sxml_sequence_get_representation(key) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
            !sxml_item_is_atomic(key.content)) {
            status = sxml_report_coercion_error(engine->role, frame->error_record);
            break;
        }
        entries[entry_index] =
            (sxml_map_entry){.key = key.content, .value = frame->parts[2 * entry_index + 1]};
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            engine->dynamic_context->item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = frame->part_count},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = NULL},
            &map);
        if (status == SXML_STATUS_DYNAMIC_ERROR) {
            status = sxml_error_record_report(frame->error_record,
                                              &sxml_duplicate_key_coercion_error);
        }
    }
    if (memory != NULL) {
        sxml_allocator_release(engine->dynamic_context->allocator, memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_coercion_finish(engine,
                                (sxml_coercion_result){.is_unchanged = false,
                                                       .value = sxml_sequence_from_item(map)});
}

/* RECORD_FIELDS: a new record of the type, each field's value (the empty
   sequence when the map has no such entry) coerced to the field's type
   (section 3.4.2). */
static sxml_status sxml_coercion_step_record(sxml_coercion_engine *engine,
                                             sxml_coercion_frame *frame) {
    if (engine->has_result) {
        sxml_coercion_store_part(frame, sxml_coercion_take_result(engine),
                                 frame->current_value);
    }
    if (frame->next_index == frame->part_count) {
        sxml_item record = {.representation = 0};
        sxml_status status = sxml_record_make(
            engine->dynamic_context->item_heap,
            &(sxml_record_request){.record_type = frame->item_type,
                                   .field_values = {.sequences = frame->parts,
                                                    .sequence_count = frame->stored_count}},
            &record);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_coercion_finish(engine,
                                    (sxml_coercion_result){.is_unchanged = false,
                                                           .value = sxml_sequence_from_item(record)});
    }
    const sxml_record_field *field = &frame->record_fields[frame->next_index];
    sxml_item key = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        engine->dynamic_context->item_heap,
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
    frame->next_index += 1;
    frame->current_value = lookup.is_found ? lookup.entry.value : sxml_sequence_empty();
    return sxml_coercion_push(engine, sxml_coercion_sequence_frame(frame->current_value,
                                                                   field->field_type,
                                                                   frame->error_record));
}

static sxml_status sxml_coercion_step(sxml_coercion_engine *engine) {
    sxml_coercion_frame *frame = &engine->frames[engine->frame_count - 1];
    switch (frame->frame_kind) {
    case SXML_COERCION_FRAME_KIND_SEQUENCE:
        return sxml_coercion_step_sequence(engine, frame);
    case SXML_COERCION_FRAME_KIND_ITEM:
        return sxml_coercion_step_item(engine, frame);
    case SXML_COERCION_FRAME_KIND_CHOICE:
        return sxml_coercion_step_choice(engine, frame);
    case SXML_COERCION_FRAME_KIND_ARRAY_MEMBERS:
        return sxml_coercion_step_array(engine, frame);
    case SXML_COERCION_FRAME_KIND_MAP_ENTRIES:
        return sxml_coercion_step_map(engine, frame);
    case SXML_COERCION_FRAME_KIND_RECORD_FIELDS:
        return sxml_coercion_step_record(engine, frame);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* After a failure with an error record: pops frames down to the nearest
   CHOICE still trying alternatives and tells it; *is_caught is false when
   there is none (every frame is gone). */
typedef struct sxml_coercion_failure {
    sxml_status status;
} sxml_coercion_failure;

static void sxml_coercion_unwind(sxml_coercion_engine *engine, sxml_coercion_failure failure,
                                 sxml_emptiness *is_caught) {
    bool *caught = &is_caught->is_empty;
    *caught = false;
    engine->failure_status = failure.status;
    sxml_sequence_release(engine->result);
    engine->result = sxml_sequence_empty();
    engine->has_result = false;
    /* The failing frame itself goes first, even a CHOICE. */
    if (engine->frame_count > 0) {
        sxml_coercion_release_frame(engine, &engine->frames[engine->frame_count - 1]);
        engine->frame_count -= 1;
    }
    while (engine->frame_count > 0) {
        sxml_coercion_frame *frame = &engine->frames[engine->frame_count - 1];
        if (frame->frame_kind == SXML_COERCION_FRAME_KIND_CHOICE &&
            frame->phase == SXML_CHOICE_PHASE_TRYING) {
            engine->has_failure = true;
            *caught = true;
            return;
        }
        sxml_coercion_release_frame(engine, frame);
        engine->frame_count -= 1;
    }
}

/* Runs the coercion rules on request->value; writes the new value, owned,
   to *result. */
static sxml_status sxml_run_coercion(sxml_dynamic_context *dynamic_context,
                                     const sxml_coercion_request *request,
                                     sxml_sequence *result) {
    sxml_coercion_engine engine = {
        .dynamic_context = dynamic_context,
        .registry = sxml_item_heap_get_type_registry(dynamic_context->item_heap),
        .role = request->role,
        .scratch_record = {.value = NULL},
        .frames = NULL,
        .frame_count = 0,
        .frame_capacity = 0,
        .has_result = false,
        .is_unchanged = false,
        .result = sxml_sequence_empty(),
        .has_failure = false,
        .failure_status = SXML_STATUS_OK};
    sxml_status status = sxml_coercion_push(
        &engine, sxml_coercion_sequence_frame(request->value, request->sequence_type,
                                              request->error_record));
    while (status == SXML_STATUS_OK && engine.frame_count > 0) {
        status = sxml_coercion_step(&engine);
        if (sxml_status_has_error_record(status)) {
            sxml_emptiness is_caught = {.is_empty = false};
            sxml_coercion_unwind(&engine, (sxml_coercion_failure){.status = status},
                                 &is_caught);
            if (is_caught.is_empty) {
                status = SXML_STATUS_OK;
            }
        }
    }
    while (engine.frame_count > 0) {
        sxml_coercion_release_frame(&engine, &engine.frames[engine.frame_count - 1]);
        engine.frame_count -= 1;
    }
    if (engine.frames != NULL) {
        sxml_allocator_release(dynamic_context->allocator, engine.frames);
    }
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(engine.result);
        return status;
    }
    const sxml_coercion_result outcome = sxml_coercion_take_result(&engine);
    if (outcome.is_unchanged) {
        sxml_sequence_retain(request->value);
        *result = request->value;
    } else {
        *result = outcome.value;
    }
    return SXML_STATUS_OK;
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
    /* A range that matches is unchanged by coercion: an xs:integer stays
       itself for any supertype (XPath 4.0, section 3.4.2), and the range
       may be far too long to walk (cbcl-codepoints-to-string-021). */
    sxml_range_decision decision;
    sxml_status status = sxml_decide_range_match(
        registry,
        &(sxml_type_match_request){.value = request->value,
                                   .sequence_type = sequence_type,
                                   .error_record = request->error_record},
        &decision);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (decision.is_decided && decision.is_match) {
        sxml_sequence_retain(request->value);
        *result = request->value;
        return SXML_STATUS_OK;
    }
    if (decision.is_decided) {
        /* Its items would stay as they are: the count is wrong. */
        return sxml_report_coercion_error(request->role, request->error_record);
    }
    /* A value that already matches a type whose matching items stay as
       they are needs no coercion: one match instead of the rules. */
    sxml_atomicity converts = {.is_atomic = true};
    if (sequence_type.occurrence != SXML_OCCURRENCE_ZERO) {
        status = sxml_type_converts_matching_items(registry, sequence_type.item_type,
                                                   &converts);
    }
    sxml_type_match match = {.is_match = false};
    if (status == SXML_STATUS_OK && !converts.is_atomic) {
        status = sxml_evaluator_match_sequence_type(
            dynamic_context,
            &(sxml_type_match_request){.value = request->value,
                                       .sequence_type = sequence_type,
                                       .error_record = request->error_record},
            &match);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (match.is_match) {
        sxml_sequence_retain(request->value);
        *result = request->value;
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    status = sxml_run_coercion(dynamic_context, request, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Finally the value must match (section 3.4, last rule). */
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
