/*
 * The node part of the function library (packet 1.20, D78): names of nodes
 * (fn:name, fn:local-name, fn:namespace-uri, fn:node-name, F&O 4.0,
 * section 2.1 and 14.1), fn:root, fn:distinct-ordered-nodes, fn:doc and
 * fn:QName (F&O 4.0, sections 14.1, 14.5, 15.1 and 10.1).
 *
 * Golden file: src/memory.c (layout and comments); rows as in
 * src/function_library_internal.h.
 *
 * "node()" in F&O 4.0 is the generic node: an XNode or a JNode. A JNode has
 * no name, so the name functions give the empty string or sequence for it.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

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
static const sxml_function_parameter nodes_parameters[] = {
    SXML_REQUIRED_PARAMETER("nodes", "node()*")};
static const sxml_function_parameter doc_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?")};
static const sxml_function_parameter qname_parameters[] = {
    SXML_REQUIRED_PARAMETER("uri", "xs:string?"),
    SXML_REQUIRED_PARAMETER("qname", "xs:string")};

static const sxml_native_function node_functions[] = {
    SXML_FUNCTION_ROW("local-name", node_parameters, "xs:string", 0, call_local_name),
    SXML_FUNCTION_ROW("name", node_parameters, "xs:string", 0, call_name),
    SXML_FUNCTION_ROW("namespace-uri", node_parameters, "xs:anyURI", 0,
                      call_namespace_uri),
    SXML_FUNCTION_ROW("node-name", node_parameters, "xs:QName?", 0, call_node_name),
    SXML_FUNCTION_ROW("root", node_parameters, "node()?", 0, call_root),
    SXML_FUNCTION_ROW("distinct-ordered-nodes", nodes_parameters, "node()*", 0,
                      call_distinct_ordered_nodes),
    SXML_FUNCTION_ROW("doc", doc_parameters, "document-node()?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_doc),
    SXML_FUNCTION_ROW("QName", qname_parameters, "xs:QName", 0, call_qname),
};

const sxml_native_function_view sxml_function_library_node =
    SXML_LIBRARY_PART(node_functions);
