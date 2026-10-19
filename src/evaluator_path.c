/*
 * Paths for the evaluator (src/evaluator_internal.h; D46, D55, D63, D75):
 * axis steps over XNodes and JNodes, node tests, document order and the
 * node set operators, and the merge sort they and SORT share.
 *
 * Golden file: src/tree_axis.c (iteration over the tree interface, no
 * recursion), after src/memory.c.
 *
 * Specification: XPath 4.0, section 4.6.3 "Path operator" (document order
 * without duplicates, XPTY0018), section 4.6.4 "Axes" (axis order, reverse
 * axes nearest first), section 4.6.5 "Node Tests" (name tests on JNodes
 * select by key), section 4.8.4 "Combining Node Sequences" (union,
 * intersect, except; XPTY0004), section 3.2.7 "Node Types"; XDM 4.0,
 * sections 6.2 (document order) and 8.5 (JNodes).
 *
 * Nothing here recurses (D71): descendants of a JNode are walked with an
 * explicit stack, sorting is bottom-up.
 */
#include "evaluator_internal.h"
#include "tree_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Errors */

static const sxml_static_error sxml_step_origin_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The context item of an axis step is not a node (XPath 4.0, section "
        "4.7.4: XPTY0004, formerly XPTY0020)"),
};

static const sxml_static_error sxml_json_attribute_axis_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The attribute and namespace axes are not defined for JNodes (XPath "
        "4.0, section 4.7.5.1)"),
};

static const sxml_static_error sxml_mixed_path_result_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0018"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The result of a path operator mixes nodes and other items (XPath "
        "4.0, section 4.6.3)"),
};

static const sxml_static_error sxml_node_set_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An operand of union, intersect or except holds an item that is not a "
        "node (XPath 4.0, section 4.8.4)"),
};

/* ------------------------------------------------------------ Sorting */

sxml_status sxml_evaluator_sort_indices(sxml_allocator allocator,
                                        const sxml_index_sort_request *request) {
    const size_t index_count = request->index_count;
    if (index_count < 2) {
        return SXML_STATUS_OK;
    }
    size_t scratch_byte_count = 0;
    sxml_status status =
        sxml_size_multiply(index_count, sizeof(size_t), &scratch_byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = scratch_byte_count,
                                      .alignment = alignof(size_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t *source = request->indices;
    size_t *target = memory;
    /* Runs of width 1, 2, 4, ... merged pairwise; the left element wins
       ties, so the sort is stable. */
    for (size_t width = 1; status == SXML_STATUS_OK && width < index_count;
         width *= 2) {
        for (size_t run_start = 0; run_start < index_count;
             run_start += 2 * width) {
            const size_t middle = run_start + width < index_count
                                      ? run_start + width
                                      : index_count;
            const size_t run_end = middle + width < index_count
                                       ? middle + width
                                       : index_count;
            size_t left = run_start;
            size_t right = middle;
            size_t output = run_start;
            while (status == SXML_STATUS_OK && left < middle && right < run_end) {
                sxml_ordering ordering = SXML_ORDERING_EQUAL;
                status = request->compare(
                    request->compare_state,
                    (sxml_index_pair){.first_index = source[right],
                                      .second_index = source[left]},
                    &ordering);
                if (ordering == SXML_ORDERING_LESS) {
                    target[output] = source[right];
                    right += 1;
                } else {
                    target[output] = source[left];
                    left += 1;
                }
                output += 1;
            }
            while (left < middle) {
                target[output] = source[left];
                left += 1;
                output += 1;
            }
            while (right < run_end) {
                target[output] = source[right];
                right += 1;
                output += 1;
            }
        }
        size_t *swap = source;
        source = target;
        target = swap;
    }
    if (status == SXML_STATUS_OK && source != request->indices) {
        memcpy(request->indices, source, scratch_byte_count);
    }
    sxml_allocator_release(allocator, memory);
    return status;
}

/* ----------------------------------------------------------- Item lists */

/* Items collected in allocator memory, each owned. */
typedef struct sxml_item_list {
    sxml_allocator allocator;
    sxml_item *items;
    size_t item_count;
    size_t item_capacity;
} sxml_item_list;

static sxml_item_list sxml_item_list_start(sxml_allocator allocator) {
    return (sxml_item_list){.allocator = allocator,
                            .items = NULL,
                            .item_count = 0,
                            .item_capacity = 0};
}

/* Appends item, taking over the caller's reference; on failure the item
   is released. */
static sxml_status sxml_item_list_take(sxml_item_list *list, sxml_item item) {
    if (list->item_count == list->item_capacity) {
        void *elements = list->items;
        sxml_status status = sxml_evaluator_grow(
            list->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &list->item_capacity,
                                     .element_count = list->item_count,
                                     .required_count = list->item_count + 1,
                                     .element_byte_count = sizeof(sxml_item),
                                     .alignment = alignof(sxml_item)});
        if (status != SXML_STATUS_OK) {
            sxml_item_release(item);
            return status;
        }
        list->items = elements;
    }
    list->items[list->item_count] = item;
    list->item_count += 1;
    return SXML_STATUS_OK;
}

/* Appends a retained copy of item. */
static sxml_status sxml_item_list_append(sxml_item_list *list,
                                         sxml_item item) {
    sxml_item_retain(item);
    return sxml_item_list_take(list, item);
}

static void sxml_item_list_finalize(sxml_item_list *list) {
    for (size_t item_index = 0; item_index < list->item_count;
         item_index += 1) {
        sxml_item_release(list->items[item_index]);
    }
    if (list->items != NULL) {
        sxml_allocator_release(list->allocator, list->items);
    }
    *list = sxml_item_list_start(list->allocator);
}

/* Makes the vector of the list's items, owned, and empties the list. */
static sxml_status sxml_item_list_finish(sxml_item_list *list,
                                         sxml_item_heap *item_heap,
                                         sxml_sequence *result) {
    sxml_status status = sxml_sequence_make_vector(
        item_heap,
        (sxml_item_view){.items = list->items, .item_count = list->item_count},
        result);
    sxml_item_list_finalize(list);
    return status;
}

/* ------------------------------------------------------------ Node tests */

/* A name and the name test it should match. */
typedef struct sxml_name_question {
    sxml_name_identifier name;
    const sxml_name_test *name_test;
} sxml_name_question;

/* Whether name matches name_test (XPath 4.0, section 4.6.5.1). */
static sxml_status sxml_name_matches_test(const sxml_item_heap *item_heap,
                                          sxml_name_question question,
                                          sxml_type_match *match);

/* A node with its description, and the node test to test it with. */
typedef struct sxml_described_node_test {
    sxml_node_reference node;
    const sxml_node_description *description;
    const sxml_node_test *node_test;
} sxml_described_node_test;

/* sxml_evaluator_test_node for a node already described. */
static sxml_status sxml_test_described_node(const sxml_item_heap *item_heap,
                                            const sxml_described_node_test *request,
                                            sxml_type_match *match);

sxml_status sxml_evaluator_test_node(const sxml_item_heap *item_heap,
                                     const sxml_node_test_request *request,
                                     sxml_type_match *match) {
    sxml_node_description description;
    const sxml_status status = sxml_node_describe(request->node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_test_described_node(
        item_heap,
        &(sxml_described_node_test){.node = request->node,
                                    .description = &description,
                                    .node_test = request->node_test},
        match);
}

static sxml_status sxml_test_described_node(const sxml_item_heap *item_heap,
                                            const sxml_described_node_test *request,
                                            sxml_type_match *match) {
    const sxml_node_test *node_test = request->node_test;
    const sxml_node_description *description = request->description;
    sxml_status status = SXML_STATUS_OK;
    bool is_match = node_test->node_kind == SXML_NODE_KIND_ANY ||
                    node_test->node_kind == description->node_kind;
    if (is_match && node_test->node_kind != SXML_NODE_KIND_ANY) {
        sxml_type_match name_match = {.is_match = false};
        status = sxml_name_matches_test(
            item_heap,
            (sxml_name_question){.name = description->node_name,
                                 .name_test = &node_test->name_test},
            &name_match);
        is_match = name_match.is_match;
    }
    if (status == SXML_STATUS_OK && is_match &&
        node_test->type_annotation.index != SXML_TYPE_NONE) {
        /* element(N, T) and attribute(N, T): the annotation is T or derived
           from it; nilled elements only with "?" (section 3.2.7.2). */
        is_match = sxml_type_registry_is_derived_from(
                       sxml_item_heap_get_type_registry(item_heap),
                       (sxml_type_pair){
                           .derived_type = description->type_annotation,
                           .base_type = node_test->type_annotation}) &&
                   (!description->is_nilled || node_test->allows_nilled);
    }
    if (status == SXML_STATUS_OK && is_match &&
        description->node_kind == SXML_NODE_KIND_DOCUMENT &&
        node_test->document_element_test.index != SXML_TYPE_NONE) {
        /* document-node(E): exactly one element child, matching E, and no
           text children (section 3.2.7.1). */
        sxml_type_description element_test;
        status = sxml_type_registry_describe_type(
            sxml_item_heap_get_type_registry(item_heap),
            node_test->document_element_test, &element_test);
        size_t element_count = 0;
        bool has_text = false;
        sxml_node_reference element = request->node;
        sxml_axis_iterator iterator;
        if (status == SXML_STATUS_OK) {
            status = sxml_axis_iterator_start(
                (sxml_axis_request){.origin = request->node,
                                    .axis = SXML_AXIS_CHILD},
                &iterator);
        }
        while (status == SXML_STATUS_OK) {
            sxml_node_iteration_step step;
            status = sxml_axis_iterator_next(&iterator, &step);
            if (status != SXML_STATUS_OK || !step.has_node) {
                break;
            }
            sxml_node_description child;
            status = sxml_node_describe(step.node, &child);
            if (status == SXML_STATUS_OK &&
                child.node_kind == SXML_NODE_KIND_ELEMENT) {
                element_count += 1;
                element = step.node;
            } else if (status == SXML_STATUS_OK &&
                       child.node_kind == SXML_NODE_KIND_TEXT) {
                has_text = true;
            }
        }
        is_match = element_count == 1 && !has_text &&
                   element_test.variety == SXML_TYPE_VARIETY_NODE_TEST;
        if (status == SXML_STATUS_OK && is_match) {
            /* The element test is one level: element(N, T) itself has no
               nested node test. */
            const sxml_node_test *inner = &element_test.variety_details.node_test;
            sxml_node_description element_description;
            status = sxml_node_describe(element, &element_description);
            if (status == SXML_STATUS_OK) {
                is_match = inner->node_kind == SXML_NODE_KIND_ANY ||
                           inner->node_kind == SXML_NODE_KIND_ELEMENT;
            }
            if (status == SXML_STATUS_OK && is_match) {
                sxml_type_match name_match = {.is_match = false};
                status = sxml_name_matches_test(
                    item_heap,
                    (sxml_name_question){.name = element_description.node_name,
                                         .name_test = &inner->name_test},
                    &name_match);
                is_match = name_match.is_match;
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        match->is_match = is_match;
    }
    return status;
}

static sxml_status sxml_name_matches_test(const sxml_item_heap *item_heap,
                                          sxml_name_question question,
                                          sxml_type_match *match) {
    const sxml_name_identifier name = question.name;
    const sxml_name_test *name_test = question.name_test;
    bool *is_match = &match->is_match;
    switch (name_test->name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        *is_match = true;
        return SXML_STATUS_OK;
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        *is_match = name.index != 0 && name.index == name_test->expanded_name.index;
        return SXML_STATUS_OK;
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        break;
    }
    if (name.index == 0) {
        *is_match = false;
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(
        sxml_item_heap_get_name_pool(item_heap), name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *is_match =
        name_test->name_test_kind == SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD
            ? parts.namespace_uri.index == name_test->namespace_uri.index
            : sxml_byte_view_equals(parts.local_name, name_test->local_name);
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------------- Steps */

/* What a step's node test is, resolved once per step. */
typedef enum sxml_step_test_kind {
    /* node(), or jnode() without constraints: every node. */
    SXML_STEP_TEST_KIND_ANY_GENERIC_NODE,
    /* A node test of XNodes; JNodes match an element test by key. */
    SXML_STEP_TEST_KIND_NODE_TEST,
    /* Anything else (jnode(K, T), choices): the full type match. */
    SXML_STEP_TEST_KIND_TYPE_MATCH,
} sxml_step_test_kind;

typedef struct sxml_step_test {
    sxml_step_test_kind test_kind;
    /* SXML_TYPE_NODE (xnode()) matches no JNode. */
    bool matches_json_nodes;
    sxml_node_test node_test;
    sxml_type_identifier type;
} sxml_step_test;

static sxml_status sxml_step_test_resolve(const sxml_item_heap *item_heap,
                                          sxml_type_identifier type,
                                          sxml_step_test *test) {
    *test = (sxml_step_test){.test_kind = SXML_STEP_TEST_KIND_TYPE_MATCH,
                             .matches_json_nodes = true,
                             .type = type};
    if (type.index == SXML_TYPE_GENERIC_NODE || type.index == SXML_TYPE_ITEM) {
        test->test_kind = SXML_STEP_TEST_KIND_ANY_GENERIC_NODE;
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(
        sxml_item_heap_get_type_registry(item_heap), type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.variety == SXML_TYPE_VARIETY_NODE_TEST) {
        test->test_kind = SXML_STEP_TEST_KIND_NODE_TEST;
        test->node_test = description.variety_details.node_test;
        test->matches_json_nodes = type.index != SXML_TYPE_NODE;
    } else if (description.variety == SXML_TYPE_VARIETY_JSON_NODE_TEST &&
               description.variety_details.json_node_test.key_constraint ==
                   SXML_JSON_KEY_CONSTRAINT_ANY &&
               !description.variety_details.json_node_test.has_value_type) {
        test->test_kind = SXML_STEP_TEST_KIND_ANY_GENERIC_NODE;
    }
    return SXML_STATUS_OK;
}

/* Where nodes of a step go: the dynamic context, the test and the list. */
typedef struct sxml_step_output {
    sxml_dynamic_context *dynamic_context;
    const sxml_step_test *test;
    sxml_item_list *list;
    sxml_error_record *error_record;
} sxml_step_output;

/* Appends the XNode if it matches; the item made is owned by the list.
   The node's description saves describing it again for a node test, and
   a node that does not match makes no item. */
static sxml_status sxml_step_output_add_node(const sxml_step_output *output,
                                             const sxml_described_node *node) {
    const sxml_step_test *test = output->test;
    sxml_type_match match = {.is_match = true};
    sxml_status status = SXML_STATUS_OK;
    if (test->test_kind == SXML_STEP_TEST_KIND_NODE_TEST) {
        status = sxml_test_described_node(
            output->dynamic_context->item_heap,
            &(sxml_described_node_test){.node = node->node,
                                        .description = &node->description,
                                        .node_test = &test->node_test},
            &match);
        if (status != SXML_STATUS_OK || !match.is_match) {
            return status;
        }
    }
    sxml_item item = sxml_node_item_make(node->node);
    if (test->test_kind == SXML_STEP_TEST_KIND_TYPE_MATCH) {
        status = sxml_evaluator_match_sequence_type(
            output->dynamic_context,
            &(sxml_type_match_request){
                .value = sxml_sequence_from_item(item),
                .sequence_type = {.item_type = test->type,
                                  .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                .error_record = output->error_record},
            &match);
    }
    if (status != SXML_STATUS_OK || !match.is_match) {
        sxml_item_release(item);
        return status;
    }
    return sxml_item_list_take(output->list, item);
}

/* True when the JNode's key is the string text (a name test, 4.6.5). */
static sxml_status sxml_json_key_equals_text(sxml_item json_key,
                                             sxml_byte_view text,
                                             sxml_type_match *match) {
    bool *is_match = &match->is_match;
    *is_match = false;
    if (!sxml_item_is_atomic(json_key)) {
        return SXML_STATUS_OK;
    }
    const sxml_type_identifier primitive =
        sxml_atomic_item_get_primitive_type(json_key);
    if (primitive.index != SXML_TYPE_XS_STRING &&
        primitive.index != SXML_TYPE_XS_UNTYPED_ATOMIC &&
        primitive.index != SXML_TYPE_XS_ANY_URI) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(json_key, &datum);
    if (status == SXML_STATUS_OK) {
        *is_match = sxml_byte_view_equals(datum.datum_details.string.utf8, text);
    }
    return status;
}

/* A JNode and the name test of a step's element test. */
typedef struct sxml_json_name_question {
    sxml_item json_node;
    const sxml_name_test *name_test;
} sxml_json_name_question;

/*
 * Whether a JNode's ·jkey· matches a name test (XPath 4.0, section
 * 4.7.5.4): * matches every JNode; an NCName (a name in no namespace) a
 * string, xs:anyURI or xs:untypedAtomic key equal to it; any other name or
 * wildcard an xs:QName key, by the rules of XNode name tests. The IR does
 * not keep the difference between NCName a and EQName Q{}a, so Q{}a is
 * read as the NCName.
 */
static sxml_status sxml_json_key_matches_name_test(const sxml_item_heap *item_heap,
                                                   sxml_json_name_question question,
                                                   sxml_type_match *match) {
    const sxml_name_test *name_test = question.name_test;
    match->is_match = false;
    if (name_test->name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME) {
        match->is_match = true;
        return SXML_STATUS_OK;
    }
    sxml_json_node_description description;
    sxml_status status = sxml_json_node_describe(question.json_node, &description);
    if (status != SXML_STATUS_OK || !description.has_parent ||
        !sxml_item_is_atomic(description.json_key)) {
        return status;
    }
    if (name_test->name_test_kind == SXML_NAME_TEST_KIND_EXPANDED_NAME) {
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(item_heap),
                                               name_test->expanded_name, &parts);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
            return sxml_json_key_equals_text(description.json_key, parts.local_name, match);
        }
    }
    if (sxml_atomic_item_get_primitive_type(description.json_key).index != SXML_TYPE_XS_QNAME) {
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(description.json_key, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_matches_test(
        item_heap,
        (sxml_name_question){.name = datum.datum_details.qname.expanded_name,
                             .name_test = name_test},
        match);
}

/* Appends the JNode, taking over the caller's reference, if it matches. */
static sxml_status sxml_step_output_take_json_node(
    const sxml_step_output *output, sxml_item json_node) {
    const sxml_step_test *test = output->test;
    sxml_type_match match = {.is_match = test->matches_json_nodes};
    sxml_status status = SXML_STATUS_OK;
    if (match.is_match && test->test_kind == SXML_STEP_TEST_KIND_NODE_TEST) {
        /* A JNode matches node(), and an element test (the principal node
           kind of its axis) by its key: * any key, N the key "N" (XPath
           4.0, section 4.6.5). */
        const sxml_node_test *node_test = &test->node_test;
        if (node_test->node_kind == SXML_NODE_KIND_ELEMENT) {
            status = sxml_json_key_matches_name_test(
                output->dynamic_context->item_heap,
                (sxml_json_name_question){.json_node = json_node,
                                          .name_test = &node_test->name_test},
                &match);
        } else {
            match.is_match = node_test->node_kind == SXML_NODE_KIND_ANY;
        }
    } else if (match.is_match &&
               test->test_kind == SXML_STEP_TEST_KIND_TYPE_MATCH) {
        status = sxml_evaluator_match_sequence_type(
            output->dynamic_context,
            &(sxml_type_match_request){
                .value = sxml_sequence_from_item(json_node),
                .sequence_type = {.item_type = test->type,
                                  .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                .error_record = output->error_record},
            &match);
    }
    if (status != SXML_STATUS_OK || !match.is_match) {
        sxml_item_release(json_node);
        return status;
    }
    return sxml_item_list_take(output->list, json_node);
}

/* One XNode axis from origin. */
static sxml_status sxml_step_xml_node(const sxml_step_output *output,
                                      sxml_node_reference origin,
                                      sxml_axis axis) {
    sxml_axis_walk walk;
    sxml_status status =
        sxml_axis_walk_start((sxml_axis_request){.origin = origin, .axis = axis}, &walk);
    while (status == SXML_STATUS_OK) {
        sxml_described_node node;
        status = sxml_axis_walk_next(&walk, &node);
        if (status != SXML_STATUS_OK || !node.has_node) {
            break;
        }
        status = sxml_step_output_add_node(output, &node);
    }
    return status;
}

/* A JNode whose children are being walked, with the next child. */
typedef struct sxml_json_walk_frame {
    /* Owned. */
    sxml_item json_node;
    uint64_t next_child_index;
    uint64_t child_count;
} sxml_json_walk_frame;

/* A preorder walk of a JTree on an explicit stack. */
typedef struct sxml_json_walk {
    sxml_allocator allocator;
    sxml_item_heap *item_heap;
    sxml_json_walk_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_json_walk;

/* Pushes json_node (retained) to have its children walked. */
static sxml_status sxml_json_walk_push(sxml_json_walk *walk,
                                       sxml_item json_node) {
    sxml_json_node_description description;
    sxml_status status = sxml_json_node_describe(json_node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (walk->frame_count == walk->frame_capacity) {
        void *elements = walk->frames;
        status = sxml_evaluator_grow(
            walk->allocator,
            &(sxml_evaluator_growth){
                .elements = &elements,
                .element_capacity = &walk->frame_capacity,
                .element_count = walk->frame_count,
                .required_count = walk->frame_count + 1,
                .element_byte_count = sizeof(sxml_json_walk_frame),
                .alignment = alignof(sxml_json_walk_frame)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        walk->frames = elements;
    }
    sxml_item_retain(json_node);
    walk->frames[walk->frame_count] = (sxml_json_walk_frame){
        .json_node = json_node,
        .next_child_index = 0,
        .child_count = description.child_count};
    walk->frame_count += 1;
    return SXML_STATUS_OK;
}

static void sxml_json_walk_finalize(sxml_json_walk *walk) {
    for (size_t frame_index = 0; frame_index < walk->frame_count;
         frame_index += 1) {
        sxml_item_release(walk->frames[frame_index].json_node);
    }
    if (walk->frames != NULL) {
        sxml_allocator_release(walk->allocator, walk->frames);
    }
    walk->frames = NULL;
    walk->frame_count = 0;
    walk->frame_capacity = 0;
}

/* Writes the next JNode of the preorder walk, owned, to step->item, or
   sets step->has_item false at the end. */
static sxml_status sxml_json_walk_next(sxml_json_walk *walk,
                                       sxml_iteration_step *step) {
    while (walk->frame_count > 0) {
        sxml_json_walk_frame *frame = &walk->frames[walk->frame_count - 1];
        if (frame->next_child_index == frame->child_count) {
            sxml_item_release(frame->json_node);
            walk->frame_count -= 1;
            continue;
        }
        sxml_item child = {.representation = 0};
        sxml_status status = sxml_json_node_make_child(
            walk->item_heap,
            (sxml_json_child_request){.json_node = frame->json_node,
                                      .child_index = frame->next_child_index},
            &child);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->next_child_index += 1;
        status = sxml_json_walk_push(walk, child);
        if (status != SXML_STATUS_OK) {
            sxml_item_release(child);
            return status;
        }
        step->item = child;
        step->has_item = true;
        return SXML_STATUS_OK;
    }
    step->has_item = false;
    return SXML_STATUS_OK;
}

/* Sends origin's descendants (preorder: document order) to output. */
static sxml_status sxml_step_json_descendants(const sxml_step_output *output,
                                              sxml_item origin) {
    sxml_json_walk walk = {.allocator = output->dynamic_context->allocator,
                           .item_heap = output->dynamic_context->item_heap,
                           .frames = NULL,
                           .frame_count = 0,
                           .frame_capacity = 0};
    sxml_status status = sxml_json_walk_push(&walk, origin);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step walk_step = {.has_item = false,
                                         .item = {.representation = 0}};
        status = sxml_json_walk_next(&walk, &walk_step);
        if (status != SXML_STATUS_OK || !walk_step.has_item) {
            break;
        }
        const sxml_item json_node = walk_step.item;
        status = sxml_step_output_take_json_node(output, json_node);
    }
    sxml_json_walk_finalize(&walk);
    return status;
}

/* The JNodes of origin's JTree from the root in preorder up to origin
   (not included), without origin's ancestors: the preceding axis in
   document order. */
static sxml_status sxml_step_json_preceding(const sxml_step_output *output,
                                            sxml_item origin) {
    sxml_item root = origin;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_json_node_description description;
        status = sxml_json_node_describe(root, &description);
        if (status != SXML_STATUS_OK || !description.has_parent) {
            break;
        }
        root = description.parent;
    }
    /* The walk yields the root's descendants: nothing precedes the root,
       which must not see them all. */
    if (status != SXML_STATUS_OK ||
        sxml_generic_node_is_same_node((sxml_item_pair){.first = root, .second = origin})) {
        return status;
    }
    sxml_json_walk walk = {.allocator = output->dynamic_context->allocator,
                           .item_heap = output->dynamic_context->item_heap,
                           .frames = NULL,
                           .frame_count = 0,
                           .frame_capacity = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_json_walk_push(&walk, root);
    }
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step walk_step = {.has_item = false,
                                         .item = {.representation = 0}};
        status = sxml_json_walk_next(&walk, &walk_step);
        if (status != SXML_STATUS_OK || !walk_step.has_item) {
            break;
        }
        const sxml_item json_node = walk_step.item;
        if (sxml_generic_node_is_same_node(
                (sxml_item_pair){.first = json_node, .second = origin})) {
            sxml_item_release(json_node);
            break;
        }
        /* Origin's ancestors come before it in document order but are not
           on the preceding axis. */
        bool is_ancestor = false;
        sxml_item probe = origin;
        while (status == SXML_STATUS_OK) {
            sxml_json_node_description description;
            status = sxml_json_node_describe(probe, &description);
            if (status != SXML_STATUS_OK || !description.has_parent) {
                break;
            }
            probe = description.parent;
            if (sxml_generic_node_is_same_node(
                    (sxml_item_pair){.first = probe, .second = json_node})) {
                is_ancestor = true;
                break;
            }
        }
        if (status != SXML_STATUS_OK || is_ancestor) {
            sxml_item_release(json_node);
            continue;
        }
        status = sxml_step_output_take_json_node(output, json_node);
    }
    sxml_json_walk_finalize(&walk);
    return status;
}

/* Reverses the items the list holds from first_index on. */
static void sxml_item_list_reverse_from(sxml_item_list *list,
                                        size_t first_index) {
    size_t low = first_index;
    size_t high = list->item_count;
    while (high > low + 1) {
        high -= 1;
        const sxml_item swap = list->items[low];
        list->items[low] = list->items[high];
        list->items[high] = swap;
        low += 1;
    }
}

/* Sends the children of parent from child index first to last (both
   included; last below first gives none) to output, in order. */
typedef struct sxml_json_child_range {
    sxml_item parent;
    uint64_t first_child_index;
    uint64_t end_child_index;
} sxml_json_child_range;

static sxml_status sxml_step_json_children(const sxml_step_output *output,
                                           sxml_json_child_range range) {
    sxml_status status = SXML_STATUS_OK;
    for (uint64_t child_index = range.first_child_index;
         status == SXML_STATUS_OK && child_index < range.end_child_index;
         child_index += 1) {
        sxml_item child = {.representation = 0};
        status = sxml_json_node_make_child(
            output->dynamic_context->item_heap,
            (sxml_json_child_request){.json_node = range.parent,
                                      .child_index = child_index},
            &child);
        if (status == SXML_STATUS_OK) {
            status = sxml_step_output_take_json_node(output, child);
        }
    }
    return status;
}

/* One axis from a JNode over its JTree (XPath 4.0, section 4.7.5.1; XDM
   4.0, section 8.5): the attribute and namespace axes raise XPTY0004. */
static sxml_status sxml_step_json_node(const sxml_step_output *output,
                                       sxml_item origin, sxml_axis axis) {
    sxml_json_node_description description;
    sxml_status status = sxml_json_node_describe(origin, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t first_index = output->list->item_count;
    switch (axis) {
    case SXML_AXIS_SELF:
        sxml_item_retain(origin);
        return sxml_step_output_take_json_node(output, origin);
    case SXML_AXIS_CHILD:
        return sxml_step_json_children(
            output, (sxml_json_child_range){.parent = origin,
                                            .first_child_index = 0,
                                            .end_child_index =
                                                description.child_count});
    case SXML_AXIS_DESCENDANT_OR_SELF:
        sxml_item_retain(origin);
        status = sxml_step_output_take_json_node(output, origin);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_step_json_descendants(output, origin);
    case SXML_AXIS_DESCENDANT:
        return sxml_step_json_descendants(output, origin);
    case SXML_AXIS_ATTRIBUTE:
    case SXML_AXIS_NAMESPACE:
        return sxml_error_record_report(output->error_record,
                                        &sxml_json_attribute_axis_error);
    case SXML_AXIS_PARENT:
        if (!description.has_parent) {
            return SXML_STATUS_OK;
        }
        sxml_item_retain(description.parent);
        return sxml_step_output_take_json_node(output, description.parent);
    case SXML_AXIS_ANCESTOR:
    case SXML_AXIS_ANCESTOR_OR_SELF: {
        sxml_item ancestor = origin;
        bool includes = axis == SXML_AXIS_ANCESTOR_OR_SELF;
        for (;;) {
            if (includes) {
                sxml_item_retain(ancestor);
                status = sxml_step_output_take_json_node(output, ancestor);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
            sxml_json_node_description ancestor_description;
            status = sxml_json_node_describe(ancestor, &ancestor_description);
            if (status != SXML_STATUS_OK || !ancestor_description.has_parent) {
                return status;
            }
            ancestor = ancestor_description.parent;
            includes = true;
        }
    }
    case SXML_AXIS_FOLLOWING_SIBLING:
    case SXML_AXIS_FOLLOWING_SIBLING_OR_SELF:
    case SXML_AXIS_PRECEDING_SIBLING:
    case SXML_AXIS_PRECEDING_SIBLING_OR_SELF: {
        if (!description.has_parent) {
            if (axis == SXML_AXIS_FOLLOWING_SIBLING_OR_SELF ||
                axis == SXML_AXIS_PRECEDING_SIBLING_OR_SELF) {
                sxml_item_retain(origin);
                return sxml_step_output_take_json_node(output, origin);
            }
            return SXML_STATUS_OK;
        }
        sxml_json_node_description parent_description;
        status = sxml_json_node_describe(description.parent, &parent_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const bool includes_self = axis == SXML_AXIS_FOLLOWING_SIBLING_OR_SELF ||
                                   axis == SXML_AXIS_PRECEDING_SIBLING_OR_SELF;
        const bool is_following = axis == SXML_AXIS_FOLLOWING_SIBLING ||
                                  axis == SXML_AXIS_FOLLOWING_SIBLING_OR_SELF;
        const uint64_t self_index = description.child_index;
        sxml_json_child_range range = {.parent = description.parent,
                                       .first_child_index = 0,
                                       .end_child_index = 0};
        if (is_following) {
            range.first_child_index = includes_self ? self_index : self_index + 1;
            range.end_child_index = parent_description.child_count;
        } else {
            range.end_child_index = includes_self ? self_index + 1 : self_index;
        }
        status = sxml_step_json_children(output, range);
        if (status == SXML_STATUS_OK && !is_following) {
            /* Reverse axes yield nearest first. */
            sxml_item_list_reverse_from(output->list, first_index);
        }
        return status;
    }
    case SXML_AXIS_FOLLOWING:
    case SXML_AXIS_FOLLOWING_OR_SELF: {
        if (axis == SXML_AXIS_FOLLOWING_OR_SELF) {
            sxml_item_retain(origin);
            status = sxml_step_output_take_json_node(output, origin);
        }
        /* The following siblings of origin and of each ancestor, each with
           its descendants, nearest level first: document order. */
        sxml_item level = origin;
        while (status == SXML_STATUS_OK) {
            sxml_json_node_description level_description;
            status = sxml_json_node_describe(level, &level_description);
            if (status != SXML_STATUS_OK || !level_description.has_parent) {
                break;
            }
            sxml_json_node_description parent_description;
            status = sxml_json_node_describe(level_description.parent,
                                             &parent_description);
            for (uint64_t child_index = level_description.child_index + 1;
                 status == SXML_STATUS_OK &&
                 child_index < parent_description.child_count;
                 child_index += 1) {
                sxml_item sibling = {.representation = 0};
                status = sxml_json_node_make_child(
                    output->dynamic_context->item_heap,
                    (sxml_json_child_request){.json_node =
                                                  level_description.parent,
                                              .child_index = child_index},
                    &sibling);
                if (status == SXML_STATUS_OK) {
                    /* The sibling, then its descendants. */
                    sxml_item_retain(sibling);
                    status = sxml_step_output_take_json_node(output, sibling);
                    if (status == SXML_STATUS_OK) {
                        status = sxml_step_json_descendants(output, sibling);
                    }
                    sxml_item_release(sibling);
                }
            }
            level = level_description.parent;
        }
        return status;
    }
    case SXML_AXIS_PRECEDING:
    case SXML_AXIS_PRECEDING_OR_SELF:
        status = sxml_step_json_preceding(output, origin);
        if (status == SXML_STATUS_OK && axis == SXML_AXIS_PRECEDING_OR_SELF) {
            sxml_item_retain(origin);
            status = sxml_step_output_take_json_node(output, origin);
        }
        if (status == SXML_STATUS_OK) {
            sxml_item_list_reverse_from(output->list, first_index);
        }
        return status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* True for a step whose results from several origins need sorting: every
   axis but self, child and attribute can overlap or run backwards. */
static bool sxml_axis_keeps_document_order(sxml_axis axis) {
    return axis == SXML_AXIS_SELF || axis == SXML_AXIS_CHILD ||
           axis == SXML_AXIS_ATTRIBUTE || axis == SXML_AXIS_NAMESPACE;
}

sxml_status sxml_evaluator_make_step_value(sxml_dynamic_context *dynamic_context,
                                           const sxml_step_request *request,
                                           sxml_sequence *result) {
    sxml_step_test test;
    sxml_status status =
        sxml_step_test_resolve(dynamic_context->item_heap, request->node_test, &test);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_list list = sxml_item_list_start(dynamic_context->allocator);
    const sxml_step_output output = {.dynamic_context = dynamic_context,
                                     .test = &test,
                                     .list = &list,
                                     .error_record = request->error_record};
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(request->origin);
    uint64_t origin_count = 0;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, request->error_record,
                                             &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        origin_count += 1;
        switch (sxml_item_get_kind(step.item)) {
        case SXML_ITEM_KIND_NODE: {
            sxml_node_reference node;
            status = sxml_item_get_node(step.item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_step_xml_node(&output, node, request->axis);
            }
            break;
        }
        case SXML_ITEM_KIND_JSON_NODE:
            status = sxml_step_json_node(&output, step.item, request->axis);
            break;
        case SXML_ITEM_KIND_MAP:
        case SXML_ITEM_KIND_ARRAY: {
            /* XPath 4.0 (editor's draft of 13 October 2026), section
               4.7.5: a map or array origin is converted to a JNode as if
               by fn:jtree. */
            sxml_item root = {.representation = 0};
            status = sxml_json_node_make_root(dynamic_context->item_heap,
                                              sxml_sequence_from_item(step.item), &root);
            if (status == SXML_STATUS_OK) {
                status = sxml_step_json_node(&output, root, request->axis);
                sxml_item_release(root);
            }
            break;
        }
        case SXML_ITEM_KIND_ATOMIC:
        case SXML_ITEM_KIND_FUNCTION:
            status = sxml_error_record_report(request->error_record,
                                              &sxml_step_origin_error);
            break;
        }
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_item_list_finish(&list, dynamic_context->item_heap, &value);
    } else {
        sxml_item_list_finalize(&list);
    }
    if (status == SXML_STATUS_OK && origin_count > 1 &&
        !sxml_axis_keeps_document_order(request->axis)) {
        /* Several origins (a context value of several nodes): the union of
           their steps, in document order. */
        sxml_sequence ordered = sxml_sequence_empty();
        status = sxml_evaluator_make_document_order_value(
            dynamic_context,
            &(sxml_document_order_request){.input = value,
                                           .error_record = request->error_record},
            &ordered);
        sxml_sequence_release(value);
        value = ordered;
    }
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}

/* ------------------------------------------------------- Document order */

/* The items of a sequence, borrowed, in an array in allocator memory. */
typedef struct sxml_item_array {
    sxml_allocator allocator;
    sxml_item *items;
    size_t item_count;
} sxml_item_array;

/* Reads every item of sequence (borrowed) into a new array. */
typedef struct sxml_item_array_request {
    sxml_sequence sequence;
    sxml_error_record *error_record;
} sxml_item_array_request;

static sxml_status sxml_item_array_read(sxml_allocator allocator,
                                        sxml_item_array_request request,
                                        sxml_item_array *array) {
    const sxml_sequence sequence = request.sequence;
    sxml_error_record *error_record = request.error_record;
    *array = (sxml_item_array){.allocator = allocator, .items = NULL, .item_count = 0};
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(sequence, error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count > SIZE_MAX / sizeof(sxml_item)) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    if (item_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = (size_t)item_count * sizeof(sxml_item),
                                  .alignment = alignof(sxml_item)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    array->items = memory;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    while (array->item_count < item_count) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        array->items[array->item_count] = step.item;
        array->item_count += 1;
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        *array = (sxml_item_array){.allocator = allocator, .items = NULL, .item_count = 0};
    }
    return status;
}

static void sxml_item_array_finalize(sxml_item_array *array) {
    if (array->items != NULL) {
        sxml_allocator_release(array->allocator, array->items);
    }
    array->items = NULL;
    array->item_count = 0;
}

/* Orders two generic nodes of an item array. */
static sxml_status sxml_compare_nodes_at(void *state, sxml_index_pair pair,
                                         sxml_ordering *ordering) {
    const sxml_item_array *array = state;
    return sxml_generic_node_compare_document_order(
        (sxml_item_pair){.first = array->items[pair.first_index],
                         .second = array->items[pair.second_index]},
        ordering);
}

/* Sorts the generic nodes of array into document order, drops duplicates
   and writes the vector, owned, to *result. */
static sxml_status sxml_make_sorted_nodes(sxml_dynamic_context *dynamic_context,
                                          sxml_item_array *array,
                                          sxml_sequence *result) {
    sxml_allocator allocator = dynamic_context->allocator;
    const size_t item_count = array->item_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = item_count * sizeof(size_t),
                                  .alignment = alignof(size_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t *indices = memory;
    for (size_t item_index = 0; item_index < item_count; item_index += 1) {
        indices[item_index] = item_index;
    }
    status = sxml_evaluator_sort_indices(
        allocator, &(sxml_index_sort_request){.indices = indices,
                                              .index_count = item_count,
                                              .compare = sxml_compare_nodes_at,
                                              .compare_state = array});
    sxml_item_list list = sxml_item_list_start(allocator);
    for (size_t position = 0; status == SXML_STATUS_OK && position < item_count;
         position += 1) {
        const sxml_item item = array->items[indices[position]];
        if (list.item_count > 0 &&
            sxml_generic_node_is_same_node(
                (sxml_item_pair){.first = list.items[list.item_count - 1],
                                 .second = item})) {
            continue;
        }
        status = sxml_item_list_append(&list, item);
    }
    sxml_allocator_release(allocator, memory);
    if (status != SXML_STATUS_OK) {
        sxml_item_list_finalize(&list);
        return status;
    }
    return sxml_item_list_finish(&list, dynamic_context->item_heap, result);
}

sxml_status sxml_evaluator_make_document_order_value(
    sxml_dynamic_context *dynamic_context,
    const sxml_document_order_request *request, sxml_sequence *result) {
    const sxml_sequence_representation representation =
        sxml_sequence_get_representation(request->input);
    if (representation == SXML_SEQUENCE_REPRESENTATION_EMPTY ||
        representation == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        representation == SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE) {
        sxml_sequence_retain(request->input);
        *result = request->input;
        return SXML_STATUS_OK;
    }
    sxml_item_array array;
    sxml_status status = sxml_item_array_read(dynamic_context->allocator, (sxml_item_array_request){.sequence = request->input, .error_record = request->error_record}, &array);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t node_count = 0;
    bool is_sorted = true;
    for (size_t item_index = 0;
         status == SXML_STATUS_OK && item_index < array.item_count;
         item_index += 1) {
        if (!sxml_item_is_generic_node(array.items[item_index])) {
            continue;
        }
        node_count += 1;
        if (item_index > 0 && is_sorted &&
            sxml_item_is_generic_node(array.items[item_index - 1])) {
            sxml_ordering ordering = SXML_ORDERING_EQUAL;
            status = sxml_generic_node_compare_document_order(
                (sxml_item_pair){.first = array.items[item_index - 1],
                                 .second = array.items[item_index]},
                &ordering);
            /* LESS: in order, and not the same node. */
            is_sorted = ordering == SXML_ORDERING_LESS;
        }
    }
    if (status == SXML_STATUS_OK && node_count != 0 &&
        node_count != array.item_count) {
        status = sxml_error_record_report(request->error_record,
                                          &sxml_mixed_path_result_error);
    }
    if (status == SXML_STATUS_OK && (node_count == 0 || is_sorted)) {
        /* No nodes, or already in order (the common case of a child step):
           the input as it is. */
        sxml_item_array_finalize(&array);
        sxml_sequence_retain(request->input);
        *result = request->input;
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_make_sorted_nodes(dynamic_context, &array, result);
    }
    sxml_item_array_finalize(&array);
    return status;
}

/* ------------------------------------------------------------ Node sets */

/* Reads one operand of a node set operator, sorted and without
   duplicates, as an owned sequence and its items. */
typedef struct sxml_node_set_operand {
    sxml_sequence sequence;
    sxml_item_array array;
} sxml_node_set_operand;

static sxml_status sxml_node_set_operand_read(
    sxml_dynamic_context *dynamic_context, sxml_document_order_request operand_request,
    sxml_node_set_operand *read) {
    const sxml_document_order_request *request = &operand_request;
    const sxml_sequence operand = operand_request.input;
    *read = (sxml_node_set_operand){
        .sequence = sxml_sequence_empty(),
        .array = {.allocator = dynamic_context->allocator, .items = NULL, .item_count = 0}};
    sxml_item_array raw;
    sxml_status status = sxml_item_array_read(dynamic_context->allocator, (sxml_item_array_request){.sequence = operand, .error_record = request->error_record}, &raw);
    for (size_t item_index = 0; status == SXML_STATUS_OK && item_index < raw.item_count;
         item_index += 1) {
        if (!sxml_item_is_generic_node(raw.items[item_index])) {
            status = sxml_error_record_report(request->error_record,
                                              &sxml_node_set_operand_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_make_document_order_value(
            dynamic_context,
            &(sxml_document_order_request){.input = operand,
                                           .error_record = request->error_record},
            &read->sequence);
    }
    sxml_item_array_finalize(&raw);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_array_read(dynamic_context->allocator, (sxml_item_array_request){.sequence = read->sequence, .error_record = request->error_record}, &read->array);
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(read->sequence);
            read->sequence = sxml_sequence_empty();
        }
    }
    return status;
}

static void sxml_node_set_operand_finalize(sxml_node_set_operand *read) {
    sxml_item_array_finalize(&read->array);
    sxml_sequence_release(read->sequence);
    read->sequence = sxml_sequence_empty();
}

sxml_status sxml_evaluator_make_node_set_value(
    sxml_dynamic_context *dynamic_context, const sxml_node_set_request *request,
    sxml_sequence *result) {
    sxml_node_set_operand left;
    sxml_status status = sxml_node_set_operand_read(
        dynamic_context,
        (sxml_document_order_request){.input = request->left_operand,
                                      .error_record = request->error_record},
        &left);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_set_operand right;
    status = sxml_node_set_operand_read(
        dynamic_context,
        (sxml_document_order_request){.input = request->right_operand,
                                      .error_record = request->error_record},
        &right);
    if (status != SXML_STATUS_OK) {
        sxml_node_set_operand_finalize(&left);
        return status;
    }
    /* Both operands in document order: one merge walk. */
    sxml_item_list list = sxml_item_list_start(dynamic_context->allocator);
    size_t left_index = 0;
    size_t right_index = 0;
    const sxml_node_set_operator node_set_operator = request->node_set_operator;
    while (status == SXML_STATUS_OK &&
           (left_index < left.array.item_count ||
            right_index < right.array.item_count)) {
        sxml_ordering ordering = SXML_ORDERING_LESS;
        if (left_index == left.array.item_count) {
            ordering = SXML_ORDERING_GREATER;
        } else if (right_index < right.array.item_count) {
            const sxml_item_pair pair = {.first = left.array.items[left_index],
                                         .second = right.array.items[right_index]};
            if (sxml_generic_node_is_same_node(pair)) {
                ordering = SXML_ORDERING_EQUAL;
            } else {
                status = sxml_generic_node_compare_document_order(pair, &ordering);
            }
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (ordering == SXML_ORDERING_EQUAL) {
            if (node_set_operator != SXML_NODE_SET_OPERATOR_EXCEPT) {
                status = sxml_item_list_append(&list, left.array.items[left_index]);
            }
            left_index += 1;
            right_index += 1;
        } else if (ordering == SXML_ORDERING_LESS) {
            if (node_set_operator != SXML_NODE_SET_OPERATOR_INTERSECT) {
                status = sxml_item_list_append(&list, left.array.items[left_index]);
            }
            left_index += 1;
        } else {
            if (node_set_operator == SXML_NODE_SET_OPERATOR_UNION) {
                status =
                    sxml_item_list_append(&list, right.array.items[right_index]);
            }
            right_index += 1;
        }
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_item_list_finish(&list, dynamic_context->item_heap, &value);
    } else {
        sxml_item_list_finalize(&list);
    }
    sxml_node_set_operand_finalize(&left);
    sxml_node_set_operand_finalize(&right);
    if (status == SXML_STATUS_OK) {
        *result = value;
    }
    return status;
}
