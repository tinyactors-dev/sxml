/*
 * The axis iterator (include/sxml/tree.h; docs/DECISIONS.md D39 and D67).
 *
 * Golden file: src/memory.c (layout, comments).
 *
 * Specification: XPath 4.0, section 4.7.5 "Axes" (the -or-self axes
 * included) and section 4.7.6 (reverse axes give proximity order).
 *
 * The iterator keeps only its origin and the next node; each step derives
 * the following one from the current node's description, so it works over
 * any tree implementation. Because node indices are preorder with
 * attributes right after their element (D39):
 * - the non-attribute node after node N in document order is
 *   N + 1 + attribute_count(N) (no attribute follows a non-element);
 * - the descendants of O are the non-attribute nodes up to
 *   last_descendant(O);
 * - the following nodes of O start after last_descendant(O), or, for an
 *   attribute or namespace node, after its element's attributes (XPath 4.0:
 *   the following axis of an attribute contains its element's
 *   descendants);
 * - the preceding nodes of O are the non-attribute nodes before its
 *   reference node R (O, or its element for an attribute or namespace
 *   node) whose subtree ends before R: nodes before R whose subtree
 *   reaches R are its ancestors, which the preceding axis excludes.
 * Each step costs O(1) descriptions, except preceding, which skips
 * ancestors and attributes; a whole walk of any axis is linear.
 */
#include "tree_internal.h"

/* ------------------------------------------------------------ Helpers */

static sxml_node_iteration_step sxml_axis_no_node(void) {
    return (sxml_node_iteration_step){
        .has_node = false,
        .node = {.tree = NULL, .node_index = {.index = 0},
                 .namespace_number = 0},
    };
}

static sxml_node_iteration_step sxml_axis_node(sxml_node_reference node) {
    return (sxml_node_iteration_step){.has_node = true, .node = node};
}

/* The stored node numbered node_number in the tree of origin. */
static sxml_node_iteration_step sxml_axis_stored_node(
    sxml_node_reference origin, uint64_t node_number) {
    return sxml_axis_node(sxml_node_reference_of_stored_node(
        (sxml_stored_node_request){.tree = origin.tree,
                                   .node_number = (uint32_t)node_number}));
}

static sxml_node_description sxml_axis_empty_description(void) {
    return (sxml_node_description){
        .node_kind = SXML_NODE_KIND_DOCUMENT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .has_parent = false,
        .parent = {.index = 0},
        .last_descendant = {.index = 0},
        .attribute_count = 0,
        .has_children = false,
        .first_child = {.index = 0},
        .last_child = {.index = 0},
        .has_previous_sibling = false,
        .previous_sibling = {.index = 0},
        .has_next_sibling = false,
        .next_sibling = {.index = 0},
        .type_annotation = {.index = 0},
        .is_id = false,
        .is_idrefs = false,
        .is_nilled = false,
        .is_cdata_section = false,
        .disables_output_escaping = false,
    };
}

/* The reference node of the preceding and following axes. */
static sxml_status sxml_axis_get_reference_number(sxml_node_reference origin,
                                                  uint32_t *reference_number) {
    if (origin.namespace_number != 0) {
        /* A namespace node shares its element's index. */
        *reference_number = origin.node_index.index;
        return SXML_STATUS_OK;
    }
    sxml_node_description description = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(origin, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *reference_number = description.node_kind == SXML_NODE_KIND_ATTRIBUTE
                            ? description.parent.index
                            : origin.node_index.index;
    return SXML_STATUS_OK;
}

/*
 * The first node of the following axis of origin: the first non-attribute
 * node after the subtree of origin, or after the attributes of the element
 * of an attribute or namespace node.
 */
static sxml_status sxml_axis_first_following(sxml_node_reference origin,
                                             sxml_node_iteration_step *step) {
    uint32_t reference_number = 0;
    sxml_status status =
        sxml_axis_get_reference_number(origin, &reference_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_description reference = sxml_axis_empty_description();
    status = sxml_node_describe(
        sxml_node_reference_of_stored_node((sxml_stored_node_request){
            .tree = origin.tree, .node_number = reference_number}),
        &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_reference_itself = reference_number == origin.node_index.index &&
                               origin.namespace_number == 0 &&
                               reference.node_kind != SXML_NODE_KIND_ATTRIBUTE;
    uint64_t first_number =
        is_reference_itself
            ? (uint64_t)reference.last_descendant.index + 1
            : (uint64_t)reference_number + 1 + reference.attribute_count;
    *step = first_number < sxml_tree_get_node_count(origin.tree)
                ? sxml_axis_stored_node(origin, first_number)
                : sxml_axis_no_node();
    return SXML_STATUS_OK;
}

/* Where a backward scan for preceding nodes starts. */
typedef struct sxml_preceding_scan {
    sxml_node_reference origin;
    /* The scan looks at the nodes before this one. */
    uint32_t before_number;
} sxml_preceding_scan;

/*
 * The last node before scan.before_number in document order that is on the
 * preceding axis of scan.origin: not an attribute, not an ancestor of the
 * reference node.
 */
static sxml_status sxml_axis_scan_preceding(sxml_preceding_scan scan,
                                            sxml_node_iteration_step *step) {
    uint32_t reference_number = 0;
    sxml_status status =
        sxml_axis_get_reference_number(scan.origin, &reference_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t node_number = scan.before_number;
    while (node_number > 0) {
        node_number -= 1;
        sxml_node_description description = sxml_axis_empty_description();
        status = sxml_node_describe(
            sxml_node_reference_of_stored_node((sxml_stored_node_request){
                .tree = scan.origin.tree, .node_number = node_number}),
            &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind != SXML_NODE_KIND_ATTRIBUTE &&
            description.last_descendant.index < reference_number) {
            *step = sxml_axis_stored_node(scan.origin, node_number);
            return SXML_STATUS_OK;
        }
    }
    *step = sxml_axis_no_node();
    return SXML_STATUS_OK;
}

/* The first preceding node of origin (before its reference node). */
static sxml_status sxml_axis_first_preceding(sxml_node_reference origin,
                                             sxml_node_iteration_step *step) {
    uint32_t reference_number = 0;
    sxml_status status =
        sxml_axis_get_reference_number(origin, &reference_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_axis_scan_preceding(
        (sxml_preceding_scan){.origin = origin,
                              .before_number = reference_number},
        step);
}

/* A node in the walk of an axis: its iterator and the node. */
typedef struct sxml_axis_position {
    const sxml_axis_iterator *iterator;
    sxml_node_reference node;
} sxml_axis_position;

/*
 * The non-attribute node after position.node in document order, if it is
 * not beyond the bound: the origin's last descendant for the descendant
 * axes, the tree's last node for the following axes.
 */
static sxml_status sxml_axis_next_in_order(sxml_axis_position position,
                                           sxml_node_iteration_step *step) {
    const sxml_axis_iterator *iterator = position.iterator;
    sxml_node_description current = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(position.node, &current);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t last_number = (uint64_t)sxml_tree_get_node_count(
                               iterator->origin.tree) - 1;
    if (iterator->axis == SXML_AXIS_DESCENDANT ||
        iterator->axis == SXML_AXIS_DESCENDANT_OR_SELF) {
        sxml_node_description origin = sxml_axis_empty_description();
        status = sxml_node_describe(iterator->origin, &origin);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        last_number = origin.last_descendant.index;
    }
    /* A namespace node has no subtree beyond itself: nothing in document
       order between it and its element's attributes is a descendant. */
    uint64_t next_number = (uint64_t)position.node.node_index.index + 1 +
                           current.attribute_count;
    *step = next_number <= last_number && position.node.namespace_number == 0
                ? sxml_axis_stored_node(iterator->origin, next_number)
                : sxml_axis_no_node();
    return SXML_STATUS_OK;
}

/* Which link of a description a step follows. */
typedef enum sxml_axis_link {
    SXML_AXIS_LINK_PARENT,
    SXML_AXIS_LINK_FIRST_CHILD,
    SXML_AXIS_LINK_NEXT_SIBLING,
    SXML_AXIS_LINK_PREVIOUS_SIBLING,
} sxml_axis_link;

typedef struct sxml_axis_link_request {
    sxml_node_reference node;
    sxml_axis_link link;
} sxml_axis_link_request;

/* Follows one link of request.node's description. */
static sxml_status sxml_axis_follow_link(sxml_axis_link_request request,
                                         sxml_node_iteration_step *step) {
    sxml_node_description description = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(request.node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool has_link = false;
    sxml_node_index target = {.index = 0};
    switch (request.link) {
    case SXML_AXIS_LINK_PARENT:
        has_link = description.has_parent;
        target = description.parent;
        break;
    case SXML_AXIS_LINK_FIRST_CHILD:
        has_link = description.has_children;
        target = description.first_child;
        break;
    case SXML_AXIS_LINK_NEXT_SIBLING:
        has_link = description.has_next_sibling;
        target = description.next_sibling;
        break;
    case SXML_AXIS_LINK_PREVIOUS_SIBLING:
        has_link = description.has_previous_sibling;
        target = description.previous_sibling;
        break;
    }
    *step = has_link ? sxml_axis_stored_node(request.node, target.index)
                     : sxml_axis_no_node();
    return SXML_STATUS_OK;
}

static sxml_status sxml_axis_follow(sxml_node_reference node,
                                    sxml_axis_link link,
                                    sxml_node_iteration_step *step) {
    return sxml_axis_follow_link(
        (sxml_axis_link_request){.node = node, .link = link}, step);
}

/* The first attribute of an element origin. */
static sxml_status sxml_axis_first_attribute(sxml_node_reference origin,
                                             sxml_node_iteration_step *step) {
    sxml_node_description description = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(origin, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = description.node_kind == SXML_NODE_KIND_ELEMENT &&
                    origin.namespace_number == 0 &&
                    description.attribute_count > 0
                ? sxml_axis_stored_node(
                      origin, (uint64_t)origin.node_index.index + 1)
                : sxml_axis_no_node();
    return SXML_STATUS_OK;
}

/*
 * The namespace node numbered namespace_number of element origin, if the
 * element has that many in-scope namespaces (XPath 4.0, section 4.7.5:
 * the namespace axis, one node per in-scope namespace).
 */
static sxml_status sxml_axis_namespace_node(sxml_node_reference origin,
                                            uint32_t namespace_number,
                                            sxml_node_iteration_step *step) {
    sxml_node_description description = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(origin, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind != SXML_NODE_KIND_ELEMENT ||
        origin.namespace_number != 0) {
        *step = sxml_axis_no_node();
        return SXML_STATUS_OK;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    status = sxml_node_get_in_scope_namespaces(origin, &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (namespace_number > bindings.namespace_binding_count ||
        namespace_number > UINT16_MAX) {
        *step = sxml_axis_no_node();
        return SXML_STATUS_OK;
    }
    sxml_node_reference node = origin;
    node.namespace_number = (uint16_t)namespace_number;
    *step = sxml_axis_node(node);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- First node */

/* The first node of request.axis from request.origin. */
static sxml_status sxml_axis_first(sxml_axis_request request,
                                   sxml_node_iteration_step *step) {
    sxml_node_reference origin = request.origin;
    switch (request.axis) {
    case SXML_AXIS_SELF:
    case SXML_AXIS_DESCENDANT_OR_SELF:
    case SXML_AXIS_FOLLOWING_SIBLING_OR_SELF:
    case SXML_AXIS_FOLLOWING_OR_SELF:
    case SXML_AXIS_ANCESTOR_OR_SELF:
    case SXML_AXIS_PRECEDING_SIBLING_OR_SELF:
    case SXML_AXIS_PRECEDING_OR_SELF:
        *step = sxml_axis_node(origin);
        return SXML_STATUS_OK;
    case SXML_AXIS_CHILD:
        return sxml_axis_follow(origin, SXML_AXIS_LINK_FIRST_CHILD, step);
    case SXML_AXIS_DESCENDANT: {
        const sxml_axis_iterator iterator = {
            .origin = origin,
            .axis = request.axis,
            .next_node = origin,
            .has_next_node = true,
        };
        return sxml_axis_next_in_order(
            (sxml_axis_position){.iterator = &iterator, .node = origin}, step);
    }
    case SXML_AXIS_ATTRIBUTE:
        return sxml_axis_first_attribute(origin, step);
    case SXML_AXIS_NAMESPACE:
        return sxml_axis_namespace_node(origin, 1, step);
    case SXML_AXIS_FOLLOWING_SIBLING:
        return sxml_axis_follow(origin, SXML_AXIS_LINK_NEXT_SIBLING, step);
    case SXML_AXIS_FOLLOWING:
        return sxml_axis_first_following(origin, step);
    case SXML_AXIS_PARENT:
    case SXML_AXIS_ANCESTOR:
        return sxml_axis_follow(origin, SXML_AXIS_LINK_PARENT, step);
    case SXML_AXIS_PRECEDING_SIBLING:
        return sxml_axis_follow(origin, SXML_AXIS_LINK_PREVIOUS_SIBLING,
                                step);
    case SXML_AXIS_PRECEDING:
        return sxml_axis_first_preceding(origin, step);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ----------------------------------------------------------- Successor */

/* The node after position.node on the iterator's axis. */
static sxml_status sxml_axis_successor(sxml_axis_position position,
                                       sxml_node_iteration_step *step) {
    const sxml_axis_iterator *iterator = position.iterator;
    sxml_node_reference current = position.node;
    bool is_origin = sxml_node_is_same_node(current, iterator->origin);
    switch (iterator->axis) {
    case SXML_AXIS_SELF:
    case SXML_AXIS_PARENT:
        *step = sxml_axis_no_node();
        return SXML_STATUS_OK;
    case SXML_AXIS_CHILD:
    case SXML_AXIS_FOLLOWING_SIBLING:
    case SXML_AXIS_FOLLOWING_SIBLING_OR_SELF:
        return sxml_axis_follow(current, SXML_AXIS_LINK_NEXT_SIBLING, step);
    case SXML_AXIS_PRECEDING_SIBLING:
    case SXML_AXIS_PRECEDING_SIBLING_OR_SELF:
        return sxml_axis_follow(current, SXML_AXIS_LINK_PREVIOUS_SIBLING,
                                step);
    case SXML_AXIS_ANCESTOR:
    case SXML_AXIS_ANCESTOR_OR_SELF:
        return sxml_axis_follow(current, SXML_AXIS_LINK_PARENT, step);
    case SXML_AXIS_DESCENDANT:
    case SXML_AXIS_DESCENDANT_OR_SELF:
        return sxml_axis_next_in_order(position, step);
    case SXML_AXIS_FOLLOWING:
    case SXML_AXIS_FOLLOWING_OR_SELF:
        /* After the origin itself (following-or-self) come the nodes after
           its subtree, not its descendants. */
        if (is_origin) {
            return sxml_axis_first_following(iterator->origin, step);
        }
        return sxml_axis_next_in_order(position, step);
    case SXML_AXIS_PRECEDING:
    case SXML_AXIS_PRECEDING_OR_SELF:
        if (is_origin) {
            return sxml_axis_first_preceding(iterator->origin, step);
        }
        return sxml_axis_scan_preceding(
            (sxml_preceding_scan){.origin = iterator->origin,
                                  .before_number = current.node_index.index},
            step);
    case SXML_AXIS_ATTRIBUTE: {
        sxml_node_description origin = sxml_axis_empty_description();
        sxml_status status = sxml_node_describe(iterator->origin, &origin);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint64_t next_number = (uint64_t)current.node_index.index + 1;
        uint64_t last_attribute_number =
            (uint64_t)iterator->origin.node_index.index +
            origin.attribute_count;
        *step = next_number <= last_attribute_number
                    ? sxml_axis_stored_node(iterator->origin, next_number)
                    : sxml_axis_no_node();
        return SXML_STATUS_OK;
    }
    case SXML_AXIS_NAMESPACE:
        return sxml_axis_namespace_node(
            iterator->origin, (uint32_t)current.namespace_number + 1, step);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------ Iterator */

static bool sxml_axis_is_valid(sxml_axis axis) {
    return axis >= SXML_AXIS_SELF && axis <= SXML_AXIS_PRECEDING_OR_SELF;
}

sxml_status sxml_axis_iterator_start(sxml_axis_request request,
                                     sxml_axis_iterator *iterator) {
    if (request.origin.tree == NULL || !sxml_axis_is_valid(request.axis)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Checks the origin exists, for every axis (self included). */
    sxml_node_description origin = sxml_axis_empty_description();
    sxml_status status = sxml_node_describe(request.origin, &origin);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_iteration_step first = sxml_axis_no_node();
    status = sxml_axis_first(request, &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *iterator = (sxml_axis_iterator){
        .origin = request.origin,
        .axis = request.axis,
        .next_node = first.node,
        .has_next_node = first.has_node,
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_axis_iterator_next(sxml_axis_iterator *iterator,
                                    sxml_node_iteration_step *step) {
    if (!iterator->has_next_node) {
        *step = sxml_axis_no_node();
        return SXML_STATUS_OK;
    }
    sxml_node_iteration_step successor = sxml_axis_no_node();
    sxml_status status = sxml_axis_successor(
        (sxml_axis_position){.iterator = iterator,
                             .node = iterator->next_node},
        &successor);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *step = sxml_axis_node(iterator->next_node);
    iterator->next_node = successor.node;
    iterator->has_next_node = successor.has_node;
    return SXML_STATUS_OK;
}
