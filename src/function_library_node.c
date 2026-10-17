/*
 * The node part of the function library (packet 1.20, D78): names of nodes
 * (fn:name, fn:local-name, fn:namespace-uri, fn:node-name, F&O 4.0,
 * section 2.1 and 14.1), fn:root, fn:distinct-ordered-nodes, fn:doc and
 * fn:QName (F&O 4.0, sections 14.1, 14.5, 15.1 and 10.1); fn:doc-available,
 * fn:base-uri, fn:document-uri, fn:has-children and fn:generate-id (packet
 * 1.23b, D91).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * "node()" in F&O 4.0 is the generic node: an XNode or a JNode. A JNode has
 * no name, so the name functions give the empty string or sequence for it.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

#include <string.h>

/* ----------------------------------------------------------------- Names */

/* The name of a node argument, for the name functions. */
typedef struct node_name {
    /* False for the empty sequence, a JNode and nodes without a name. */
    bool has_name;
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} node_name;

static sxml_status read_node_name(const sxml_function_call *call, node_name *name) {
    *name = (node_name){.has_name = false,
                        .expanded_name = {.index = 0},
                        .namespace_prefix = {.index = 0}};
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item ||
        sxml_item_get_kind(argument.item) != SXML_ITEM_KIND_NODE) {
        return status;
    }
    sxml_node_reference node;
    status = sxml_item_get_node(argument.item, &node);
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status == SXML_STATUS_OK && description.node_name.index != 0) {
        *name = (node_name){.has_name = true,
                            .expanded_name = description.node_name,
                            .namespace_prefix = description.namespace_prefix};
    }
    return status;
}

/* fn:local-name($node := .) (F&O 4.0, section 14.1.2). */
static sxml_status call_local_name(const sxml_function_call *call,
                                   sxml_sequence *result) {
    node_name name;
    sxml_status status = read_node_name(call, &name);
    sxml_name_parts parts = {.local_name = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK && name.has_name) {
        status = sxml_name_pool_get_name_parts(
            sxml_item_heap_get_name_pool(call->item_heap), name.expanded_name, &parts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_function_library_make_string(call->item_heap, parts.local_name, result);
}

/* fn:name($node := .) (F&O 4.0, section 14.1.1): prefix:local as written. */
static sxml_status call_name(const sxml_function_call *call, sxml_sequence *result) {
    node_name name;
    sxml_status status = read_node_name(call, &name);
    if (status != SXML_STATUS_OK || !name.has_name) {
        return status != SXML_STATUS_OK
                   ? status
                   : sxml_function_library_make_string(
                         call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0},
                         result);
    }
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_name_parts parts;
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    status = sxml_name_pool_get_name_parts(name_pool, name.expanded_name, &parts);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_prefix(name_pool, name.namespace_prefix,
                                                     &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    status = sxml_function_library_text_append(&text, prefix);
    if (status == SXML_STATUS_OK && prefix.byte_count > 0) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, parts.local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* fn:namespace-uri($node := .) (F&O 4.0, section 14.1.4). */
static sxml_status call_namespace_uri(const sxml_function_call *call,
                                      sxml_sequence *result) {
    node_name name;
    sxml_status status = read_node_name(call, &name);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK && name.has_name) {
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(name_pool, name.expanded_name, &parts);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = namespace_uri,
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:node-name($node := .) (F&O 4.0, section 2.1.3). */
static sxml_status call_node_name(const sxml_function_call *call,
                                  sxml_sequence *result) {
    node_name name;
    sxml_status status = read_node_name(call, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!name.has_name) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name.expanded_name,
                                                  .namespace_prefix = name.namespace_prefix}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ----------------------------------------------------------- Navigation */

/* fn:root($node := .) (F&O 4.0, section 14.1.9): the top of the parent
   chain of an XNode, or the root of a JNode's JTree. */
static sxml_status call_root(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!argument.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item item = argument.item;
    if (sxml_item_get_kind(item) == SXML_ITEM_KIND_JSON_NODE) {
        for (;;) {
            sxml_json_node_description description;
            status = sxml_json_node_describe(item, &description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!description.has_parent) {
                break;
            }
            item = description.parent;
        }
        sxml_item_retain(item);
        *result = sxml_sequence_from_item(item);
        return SXML_STATUS_OK;
    }
    sxml_node_reference node;
    status = sxml_item_get_node(item, &node);
    while (status == SXML_STATUS_OK) {
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || !description.has_parent) {
            break;
        }
        node = (sxml_node_reference){
            .tree = node.tree, .node_index = description.parent, .namespace_number = 0};
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_node_item_make(node));
    }
    return status;
}

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

/* fn:distinct-ordered-nodes($nodes) (F&O 4.0, section 14.5.1): document
   order without duplicates, as the path operator gives. */
static sxml_status call_distinct_ordered_nodes(const sxml_function_call *call,
                                               sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    return sxml_evaluator_make_document_order_value(
        call->dynamic_context,
        &(sxml_document_order_request){.input = sxml_function_library_get_argument(call, 0),
                                       .error_record = call->error_record},
        result);
}

/* fn:doc($source) (F&O 4.0, section 15.1.1): through the document loader,
   which gives the same node for the same URI within an evaluation; a
   relative URI is resolved against the static base URI (D88). */
static sxml_status call_doc(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!source.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_string uri_reference;
    status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_load_document(
            call->dynamic_context,
            &(sxml_document_request){.uri_reference = uri_reference.utf8,
                                     .base_uri = sxml_dynamic_context_get_static_base_uri(
                                         call->dynamic_context),
                                     .error_record = call->error_record},
            &document_node);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

/* fn:doc-available($source, $options := {}) (F&O 4.0, section 15.1.2):
   true when fn:doc would give a document. The loader keeps what it loads,
   so a later fn:doc gives the same node; FODC0002 and FODC0005 mean
   false. The options are accepted and ignored, as fn:doc ignores them. */
static sxml_status call_doc_available(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_available = false;
    if (source.has_item) {
        sxml_string uri_reference;
        status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
        sxml_item document_node = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_dynamic_context_load_document(
                call->dynamic_context,
                &(sxml_document_request){.uri_reference = uri_reference.utf8,
                                         .base_uri = sxml_dynamic_context_get_static_base_uri(
                                             call->dynamic_context),
                                         .error_record = NULL},
                &document_node);
        }
        if (status == SXML_STATUS_OK) {
            is_available = true;
            sxml_item_release(document_node);
        } else if (status == SXML_STATUS_DYNAMIC_ERROR) {
            status = SXML_STATUS_OK;
        }
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = is_available});
    }
    return status;
}

/* ------------------------------------------------------------ Properties */

/* Reads argument 0 as an XNode: has_node is false for the empty sequence
   and for a JNode. */
typedef struct optional_node {
    bool has_node;
    sxml_node_reference node;
} optional_node;

static sxml_status read_optional_node(const sxml_function_call *call, optional_node *node) {
    *node = (optional_node){.has_node = false,
                            .node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0}};
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status == SXML_STATUS_OK && argument.has_item &&
        sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_NODE) {
        status = sxml_item_get_node(argument.item, &node->node);
        node->has_node = status == SXML_STATUS_OK;
    }
    return status;
}

/* fn:base-uri($node := .) (F&O 4.0, section 2.1.5): dm:base-uri, from
   xml:base attributes and the tree's base URI (XDM 4.0, section 7.3). */
static sxml_status call_base_uri(const sxml_function_call *call, sxml_sequence *result) {
    optional_node node;
    const sxml_status status = read_optional_node(call, &node);
    if (status != SXML_STATUS_OK || !node.has_node) {
        *result = sxml_sequence_empty();
        return status;
    }
    return sxml_node_make_base_uri(
        call->item_heap,
        &(sxml_node_value_request){.node = node.node, .error_record = call->error_record},
        result);
}

/* fn:document-uri($node := .) (F&O 4.0, section 2.1.6): dm:document-uri of
   a document node; the empty sequence for other nodes and when absent. */
static sxml_status call_document_uri(const sxml_function_call *call, sxml_sequence *result) {
    optional_node node;
    sxml_status status = read_optional_node(call, &node);
    *result = sxml_sequence_empty();
    sxml_node_description description;
    if (status != SXML_STATUS_OK || !node.has_node) {
        return status;
    }
    status = sxml_node_describe(node.node, &description);
    sxml_tree_description tree_description;
    if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_DOCUMENT) {
        status = sxml_tree_describe(node.node.tree, &tree_description);
        if (status == SXML_STATUS_OK && tree_description.document_uri.byte_count > 0) {
            sxml_item item = {.representation = 0};
            status = sxml_atomic_item_make(
                call->item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = {.utf8 = tree_description.document_uri,
                                                           .codepoint_count = 0,
                                                           .is_ascii = false}}}},
                &item);
            if (status == SXML_STATUS_OK) {
                *result = sxml_sequence_from_item(item);
            }
        }
    }
    return status;
}

/* fn:has-children($node := .) (F&O 4.0, section 14.1.10): whether the node
   has children; for a JNode, whether its value has members or entries. */
static sxml_status call_has_children(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    bool has_children = false;
    if (status == SXML_STATUS_OK && argument.has_item) {
        if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_JSON_NODE) {
            sxml_json_node_description description;
            status = sxml_json_node_describe(argument.item, &description);
            has_children = status == SXML_STATUS_OK && description.child_count > 0;
        } else {
            sxml_node_reference node;
            status = sxml_item_get_node(argument.item, &node);
            sxml_node_description description;
            if (status == SXML_STATUS_OK) {
                status = sxml_node_describe(node, &description);
            }
            has_children = status == SXML_STATUS_OK && description.has_children;
        }
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean(
            (sxml_boolean_result){.is_true = has_children});
    }
    return status;
}

/* Appends the decimal digits of number to text. */
static sxml_status append_number(sxml_library_text *text, uint64_t number) {
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + number % 10U);
        digit_count += 1;
        number /= 10U;
    } while (number != 0);
    return sxml_function_library_text_append(
        text, (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                               .byte_count = digit_count});
}

/* fn:generate-id($node := .) (F&O 4.0, section 14.1.11): "" for the empty
   sequence; otherwise "t" tree "n" index "s" namespace number, which is an
   ASCII name that starts with a letter and is unique per XNode, since
   (tree, index, namespace number) identifies a node (D91). */
static sxml_status call_generate_id(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!argument.has_item) {
        return sxml_function_library_make_string(
            call->item_heap, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, result);
    }
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_JSON_NODE) {
        /* JNodes have no stable identity to write yet. */
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_node_reference node;
    status = sxml_item_get_node(argument.item, &node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("t"));
    if (status == SXML_STATUS_OK) {
        status = append_number(&text, sxml_tree_get_identifier(node.tree).value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("n"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_number(&text, node.node_index.index);
    }
    if (status == SXML_STATUS_OK && node.namespace_number > 0) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("s"));
        if (status == SXML_STATUS_OK) {
            status = append_number(&text, node.namespace_number);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* -------------------------------------------------------- Generic nodes */

/* The parent of a generic node (dm:parent, ·jparent·): owned in *parent
   when has_parent. */
typedef struct generic_parent {
    bool has_parent;
    sxml_item parent;
} generic_parent;

static sxml_status read_parent(sxml_item node, generic_parent *parent) {
    *parent = (generic_parent){.has_parent = false, .parent = {.representation = 0}};
    if (sxml_item_get_kind(node) == SXML_ITEM_KIND_JSON_NODE) {
        sxml_json_node_description description;
        const sxml_status status = sxml_json_node_describe(node, &description);
        if (status == SXML_STATUS_OK && description.has_parent) {
            sxml_item_retain(description.parent);
            *parent = (generic_parent){.has_parent = true, .parent = description.parent};
        }
        return status;
    }
    sxml_node_reference reference;
    sxml_status status = sxml_item_get_node(node, &reference);
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(reference, &description);
    }
    if (status == SXML_STATUS_OK && description.has_parent) {
        *parent = (generic_parent){
            .has_parent = true,
            .parent = sxml_node_item_make((sxml_node_reference){
                .tree = reference.tree, .node_index = description.parent, .namespace_number = 0})};
    }
    return status;
}

/* Whether pair.first is an ancestor of pair.second (XPath 4.0, the
   ancestor axis: the parent, its parent, ...; an attribute's or namespace
   node's element included). */
typedef struct ancestry {
    bool is_ancestor;
} ancestry;

static sxml_status test_ancestor(sxml_item_pair pair, ancestry *answer) {
    answer->is_ancestor = false;
    sxml_item current = pair.second;
    sxml_item_retain(current);
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        generic_parent parent;
        status = read_parent(current, &parent);
        sxml_item_release(current);
        if (status != SXML_STATUS_OK || !parent.has_parent) {
            break;
        }
        if (sxml_generic_node_is_same_node(
                (sxml_item_pair){.first = pair.first, .second = parent.parent})) {
            answer->is_ancestor = true;
            sxml_item_release(parent.parent);
            break;
        }
        current = parent.parent;
    }
    return status;
}

/* One axis step from origin over every generic node (node()). */
typedef struct generic_step {
    sxml_sequence origin;
    sxml_axis axis;
} generic_step;

static sxml_status make_step(const sxml_function_call *call, generic_step step,
                             sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    return sxml_evaluator_make_step_value(
        call->dynamic_context,
        &(sxml_step_request){.origin = step.origin,
                             .axis = step.axis,
                             .node_test = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE),
                             .error_record = call->error_record},
        result);
}

/* fn:siblings($node := .) (F&O 4.0, section 12.2.11): the children of
   the parent, in document order, $node included; $node alone when it is
   parentless, an attribute or a namespace node. */
static sxml_status call_siblings(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    bool is_child = true;
    if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_NODE) {
        sxml_node_reference node;
        status = sxml_item_get_node(argument.item, &node);
        sxml_node_description description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        is_child = status == SXML_STATUS_OK &&
                   description.node_kind != SXML_NODE_KIND_ATTRIBUTE &&
                   description.node_kind != SXML_NODE_KIND_NAMESPACE;
    }
    generic_parent parent = {.has_parent = false};
    if (status == SXML_STATUS_OK && is_child) {
        status = read_parent(argument.item, &parent);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!parent.has_parent) {
        sxml_item_retain(argument.item);
        *result = sxml_sequence_from_item(argument.item);
        return SXML_STATUS_OK;
    }
    status = make_step(call,
                       (generic_step){.origin = sxml_sequence_from_item(parent.parent),
                                      .axis = SXML_AXIS_CHILD},
                       result);
    sxml_item_release(parent.parent);
    return status;
}

/* Which of the two selections over a node sequence. */
typedef struct nesting_selection {
    /* fn:innermost when true, fn:outermost otherwise. */
    bool keeps_innermost;
} nesting_selection;

/*
 * fn:innermost($nodes) and fn:outermost($nodes) (F&O 4.0, sections 12.3.2
 * and 12.3.3), in document order without duplicates. In document order a
 * node's subtree (attributes and namespace nodes included) directly
 * follows it, so a node is an ancestor of some member exactly when it is
 * an ancestor of the next one, and a node has an ancestor among the
 * members exactly when the last kept outermost node is one.
 */
static sxml_status call_nesting(const sxml_function_call *call, nesting_selection selection,
                                sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_sequence ordered = sxml_sequence_empty();
    sxml_status status = sxml_evaluator_make_document_order_value(
        call->dynamic_context,
        &(sxml_document_order_request){.input = sxml_function_library_get_argument(call, 0),
                                       .error_record = call->error_record},
        &ordered);
    uint64_t node_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(ordered, call->error_record, &node_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, node_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    bool has_kept = false;
    sxml_item last_kept = {.representation = 0};
    for (uint64_t node_index = 0; status == SXML_STATUS_OK && node_index < node_count;
         node_index += 1) {
        sxml_item node = {.representation = 0};
        status = sxml_sequence_get_item(
            ordered, (sxml_item_request){.item_index = node_index, .error_record = call->error_record},
            &node);
        ancestry answer = {.is_ancestor = false};
        if (status == SXML_STATUS_OK && selection.keeps_innermost && node_index + 1 < node_count) {
            sxml_item next = {.representation = 0};
            status = sxml_sequence_get_item(
                ordered,
                (sxml_item_request){.item_index = node_index + 1,
                                    .error_record = call->error_record},
                &next);
            if (status == SXML_STATUS_OK) {
                status = test_ancestor((sxml_item_pair){.first = node, .second = next}, &answer);
            }
        } else if (status == SXML_STATUS_OK && !selection.keeps_innermost && has_kept) {
            status = test_ancestor((sxml_item_pair){.first = last_kept, .second = node}, &answer);
        }
        if (status == SXML_STATUS_OK && !answer.is_ancestor) {
            status = sxml_sequence_builder_append_item(builder, node);
            has_kept = true;
            last_kept = node;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    sxml_sequence_release(ordered);
    return status;
}

static sxml_status call_innermost(const sxml_function_call *call, sxml_sequence *result) {
    return call_nesting(call, (nesting_selection){.keeps_innermost = true}, result);
}

static sxml_status call_outermost(const sxml_function_call *call, sxml_sequence *result) {
    return call_nesting(call, (nesting_selection){.keeps_innermost = false}, result);
}

/* fn:nilled($node := .) (F&O 4.0, section 12.1.4): dm:nilled of an
   element; the empty sequence for other nodes. */
static sxml_status call_nilled(const sxml_function_call *call, sxml_sequence *result) {
    optional_node node;
    sxml_status status = read_optional_node(call, &node);
    *result = sxml_sequence_empty();
    sxml_node_description description;
    if (status == SXML_STATUS_OK && node.has_node) {
        status = sxml_node_describe(node.node, &description);
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            *result = sxml_function_library_make_boolean(
                (sxml_boolean_result){.is_true = description.is_nilled});
        }
    }
    return status;
}

/* ------------------------------------------------------------ fn:lang */

/* Appends the full lower-case mapping of text: the caseless default match
   of F&O 4.0, fn:lang, compares these. */
static sxml_status append_lower_case(sxml_library_text *text, sxml_byte_view utf8) {
    sxml_utf8_validation validation = {.codepoint_count = 0};
    sxml_status status = sxml_utf8_validate(utf8, &validation);
    sxml_codepoint_iterator iterator = {
        .string = {.utf8 = utf8, .codepoint_count = validation.codepoint_count, .is_ascii = false},
        .byte_offset = 0};
    uint32_t codepoint = 0;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = codepoint,
                                        .mapping_kind = SXML_CASE_MAPPING_KIND_LOWER},
            &mapping);
        for (size_t mapped_index = 0;
             status == SXML_STATUS_OK && mapped_index < mapping.codepoint_count;
             mapped_index += 1) {
            status = sxml_function_library_text_append_codepoint(text,
                                                                 mapping.codepoints[mapped_index]);
        }
    }
    return status;
}

/* The xml:lang attribute of an element, when it has one. */
typedef struct language_attribute {
    bool is_found;
    sxml_byte_view value;
} language_attribute;

typedef struct language_lookup {
    sxml_node_reference element;
    const sxml_node_description *description;
    sxml_name_identifier language_name;
} language_lookup;

static sxml_status find_language_attribute(language_lookup lookup,
                                           language_attribute *attribute) {
    attribute->is_found = false;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_index = 1;
         status == SXML_STATUS_OK && attribute_index <= lookup.description->attribute_count;
         attribute_index += 1) {
        const sxml_node_reference candidate = {
            .tree = lookup.element.tree,
            .node_index = {.index = lookup.element.node_index.index + attribute_index},
            .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(candidate, &description);
        if (status == SXML_STATUS_OK && description.node_name.index == lookup.language_name.index) {
            attribute->is_found = true;
            return sxml_node_get_content(candidate, &attribute->value);
        }
    }
    return status;
}

/* fn:lang($language, $node := .) (F&O 4.0, section 12.2.4): whether the
   xml:lang of the nearest ancestor-or-self element that has one equals
   $language, or starts with it followed by "-", under a caseless match. */
static sxml_status call_lang(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string language;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &language);
    sxml_optional_item argument = {.has_item = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 1, &argument);
    }
    sxml_node_reference node = {.tree = NULL};
    if (status == SXML_STATUS_OK && argument.has_item) {
        status = sxml_item_get_node(argument.item, &node);
    }
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_name_identifier language_name = {.index = 0};
    bool is_true = false;
    language_attribute attribute = {.is_found = false};
    if (sxml_name_pool_find_name(
            sxml_item_heap_get_name_pool(call->item_heap),
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("lang")},
            &language_name)) {
        while (status == SXML_STATUS_OK && !attribute.is_found) {
            sxml_node_description description;
            status = sxml_node_describe(node, &description);
            if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
                status = find_language_attribute(
                    (language_lookup){.element = node,
                                      .description = &description,
                                      .language_name = language_name},
                    &attribute);
            }
            if (status != SXML_STATUS_OK || attribute.is_found || !description.has_parent) {
                break;
            }
            node = (sxml_node_reference){
                .tree = node.tree, .node_index = description.parent, .namespace_number = 0};
        }
    }
    sxml_library_text wanted = {.item_heap = call->item_heap};
    sxml_library_text found = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK && attribute.is_found) {
        status = append_lower_case(&wanted, language.utf8);
        if (status == SXML_STATUS_OK) {
            status = append_lower_case(&found, attribute.value);
        }
        const sxml_byte_view wanted_text = sxml_function_library_text_view(&wanted);
        const sxml_byte_view found_text = sxml_function_library_text_view(&found);
        is_true = status == SXML_STATUS_OK && found_text.byte_count >= wanted_text.byte_count &&
                  (wanted_text.byte_count == 0 ||
                   memcmp(found_text.bytes, wanted_text.bytes, wanted_text.byte_count) == 0) &&
                  (found_text.byte_count == wanted_text.byte_count ||
                   found_text.bytes[wanted_text.byte_count] == '-');
    }
    sxml_function_library_text_release(&wanted);
    sxml_function_library_text_release(&found);
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = is_true});
    }
    return status;
}

/* ----------------------------------------------------------------- QName */

static const sxml_static_error invalid_qname_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The value is not a lexical QName, or has a prefix but no namespace URI "
        "(F&O 4.0, fn:QName)"),
};

/* Whether a text is an NCName. */
typedef struct name_check {
    bool is_ncname;
} name_check;

/* Checks text by making an xs:NCName item (XML Namespaces, [4] NCName). */
static sxml_status test_ncname(sxml_item_heap *item_heap, sxml_byte_view text,
                               name_check *check) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_NCNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = text,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    check->is_ncname = status == SXML_STATUS_OK;
    sxml_item_release(item);
    return status == SXML_STATUS_INVALID_ARGUMENT ? SXML_STATUS_OK : status;
}

/* fn:QName($uri, $qname) (F&O 4.0, section 10.1.2). */
static sxml_status call_qname(const sxml_function_call *call, sxml_sequence *result) {
    sxml_string uri;
    sxml_string qualified_name;
    sxml_status status = sxml_function_library_read_string_argument(call, 0, &uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &qualified_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view local_name = qualified_name.utf8;
    for (size_t byte_index = 0; byte_index < qualified_name.utf8.byte_count;
         byte_index += 1) {
        if (qualified_name.utf8.bytes[byte_index] == ':') {
            prefix = (sxml_byte_view){.bytes = qualified_name.utf8.bytes,
                                      .byte_count = byte_index};
            local_name = (sxml_byte_view){
                .bytes = qualified_name.utf8.bytes + byte_index + 1,
                .byte_count = qualified_name.utf8.byte_count - byte_index - 1};
            break;
        }
    }
    name_check is_local_name = {.is_ncname = false};
    name_check is_prefix = {.is_ncname = true};
    status = test_ncname(call->item_heap, local_name, &is_local_name);
    if (status == SXML_STATUS_OK && local_name.bytes != qualified_name.utf8.bytes) {
        status = test_ncname(call->item_heap, prefix, &is_prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_local_name.is_ncname || !is_prefix.is_ncname ||
        (prefix.byte_count > 0 && uri.utf8.byte_count == 0)) {
        return sxml_function_library_report(call, &invalid_qname_error);
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_name_identifier expanded_name = {.index = 0};
    sxml_namespace_prefix_identifier namespace_prefix = {.index = 0};
    status = sxml_name_pool_intern_namespace_uri(name_pool, uri.utf8, &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
            &expanded_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, prefix, &namespace_prefix);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = expanded_name,
                                                      .namespace_prefix = namespace_prefix}}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter node_parameters[] = {
    SXML_CONTEXT_PARAMETER("node", "node()?")};
static const sxml_function_parameter xnode_parameters[] = {
    SXML_CONTEXT_PARAMETER("node", "xnode()?")};
static const sxml_function_parameter lang_parameters[] = {
    SXML_REQUIRED_PARAMETER("language", "xs:string?"),
    SXML_CONTEXT_PARAMETER("node", "xnode()")};
static const sxml_function_parameter nodes_parameters[] = {
    SXML_REQUIRED_PARAMETER("nodes", "node()*")};
static const sxml_function_parameter doc_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?")};
static const sxml_function_parameter doc_available_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter qname_parameters[] = {
    SXML_REQUIRED_PARAMETER("uri", "xs:string?"),
    SXML_REQUIRED_PARAMETER("qname", "xs:string")};

static const sxml_native_function node_functions[] = {
    SXML_FUNCTION_ROW("local-name", xnode_parameters, "xs:string", 0, call_local_name),
    SXML_FUNCTION_ROW("name", xnode_parameters, "xs:string", 0, call_name),
    SXML_FUNCTION_ROW("namespace-uri", xnode_parameters, "xs:anyURI", 0,
                      call_namespace_uri),
    SXML_FUNCTION_ROW("node-name", xnode_parameters, "xs:QName?", 0, call_node_name),
    SXML_FUNCTION_ROW("root", node_parameters, "node()?", 0, call_root),
    SXML_FUNCTION_ROW("distinct-ordered-nodes", nodes_parameters, "node()*", 0,
                      call_distinct_ordered_nodes),
    SXML_FUNCTION_ROW("doc", doc_parameters, "document-node()?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_doc),
    SXML_FUNCTION_ROW("QName", qname_parameters, "xs:QName", 0, call_qname),
    SXML_FUNCTION_ROW("doc-available", doc_available_parameters, "xs:boolean",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_doc_available),
    SXML_FUNCTION_ROW("base-uri", xnode_parameters, "xs:anyURI?", 0, call_base_uri),
    SXML_FUNCTION_ROW("document-uri", xnode_parameters, "xs:anyURI?", 0, call_document_uri),
    SXML_FUNCTION_ROW("has-children", node_parameters, "xs:boolean", 0, call_has_children),
    SXML_FUNCTION_ROW("generate-id", node_parameters, "xs:string", 0, call_generate_id),
    SXML_FUNCTION_ROW("siblings", node_parameters, "node()*", 0, call_siblings),
    SXML_FUNCTION_ROW("innermost", nodes_parameters, "node()*", 0, call_innermost),
    SXML_FUNCTION_ROW("outermost", nodes_parameters, "node()*", 0, call_outermost),
    SXML_FUNCTION_ROW("nilled", xnode_parameters, "xs:boolean?", 0, call_nilled),
    SXML_FUNCTION_ROW("lang", lang_parameters, "xs:boolean", 0, call_lang),
};

const sxml_native_function_view sxml_function_library_node =
    SXML_LIBRARY_PART(node_functions);
