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
#include "schema_scope_internal.h"
#include "tree_internal.h"
#include "xml_internal.h"
#include "xinclude_internal.h"

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

/* Loads the document uri_reference names as fn:doc does with options:
   *document_node, owned, is the loader's node, its XInclude result, the
   validated copy of that, or the stripped copy of that (validation
   first: strip-space "all" keeps the whitespace of elements of simple
   content, F&O 4.0, 17.2.1). On failure *document_node is released. */
typedef struct document_loading {
    sxml_byte_view uri_reference;
    const sxml_document_options *options;
    /* Where load errors go; NULL leaves them unreported (fn:doc-available
       asks only whether a document comes back). */
    sxml_error_record *error_record;
} document_loading;

static sxml_status load_document_with_options(const sxml_function_call *call,
                                              const document_loading *loading,
                                              sxml_item *document_node) {
    const sxml_document_options *options = loading->options;
    *document_node = (sxml_item){.representation = 0};
    sxml_status status = sxml_dynamic_context_load_document(
        call->dynamic_context,
        &(sxml_document_request){
            .uri_reference = loading->uri_reference,
            .base_uri = sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .error_record = loading->error_record,
            .blocks_external_resources = !options->trusts_external,
            .validates_dtd = options->validates_dtd,
            .skips_preparation = options->keeps_space,
            .preparation_number = sxml_function_library_preparation_number(call)},
        document_node);
    if (status == SXML_STATUS_OK && options->processes_xinclude) {
        /* F&O 4.0, 17.1.1: xinclude (src/xinclude_internal.h). */
        status = sxml_function_library_process_xinclude(
            call,
            &(sxml_xinclude_request){.trusts_external = options->trusts_external,
                                     .error_record = loading->error_record},
            document_node);
    }
    if (status == SXML_STATUS_OK &&
        sxml_sequence_get_representation(options->xsd_validation) !=
            SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        sxml_atomic_datum validation;
        status = sxml_atomic_item_get_datum(options->xsd_validation.content, &validation);
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scope_validate_document(
                call,
                &(sxml_schema_scope_document_validation){
                    .xsd_validation = validation.datum_details.string.utf8,
                    .uses_schema_location_hints = options->uses_schema_location_hints},
                document_node);
        } else {
            sxml_item_release(*document_node);
        }
    }
    if (status == SXML_STATUS_OK && options->strips_space) {
        sxml_sequence stripped = sxml_sequence_empty();
        status = sxml_function_library_strip_document(call, *document_node, &stripped);
        sxml_item_release(*document_node);
        *document_node =
            status == SXML_STATUS_OK ? stripped.content : (sxml_item){.representation = 0};
    }
    return status;
}

/* fn:doc($source, $options := {}) (F&O 4.0, section 17.1.1): through the
   document loader, which gives the same node for the same URI within an
   evaluation; a relative URI is resolved against the static base URI
   (D88). The options are fn:parse-xml's (sxml_function_library_read_
   document_options): strip-space="all" gives a stripped copy, a new tree
   on every call; xsd-validation other than "skip" the validated copy of
   that (F&O 4.0, 17.1.1 and 17.2.1, as fn:parse-xml); the rest the
   loader's one way of parsing honours or rejects. */
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
    sxml_document_options options = {.strips_space = false,
                                     .keeps_space = false,
                                     .validates_dtd = false,
                                     .trusts_external = false,
                                     .xsd_validation = sxml_sequence_empty()};
    status = sxml_function_library_read_document_options(call, 1, &options);
    sxml_string uri_reference;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
    }
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = load_document_with_options(
            call,
            &(document_loading){.uri_reference = uri_reference.utf8,
                                .options = &options,
                                .error_record = call->error_record},
            &document_node);
    }
    sxml_sequence_release(options.xsd_validation);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

/* fn:doc-available($source, $options := {}) (F&O 4.0, section 15.1.2):
   true when fn:doc with the same options would give a document. The
   loader keeps what it loads, so a later fn:doc gives the same node.
   Errors in the options are raised; a document that cannot be loaded,
   is not valid against its DTD (dtd-validation) or the in-scope schemas
   (xsd-validation) gives false. */
static sxml_status call_doc_available(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_optional_item source;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_document_options options = {.strips_space = false,
                                     .keeps_space = false,
                                     .validates_dtd = false,
                                     .trusts_external = false,
                                     .xsd_validation = sxml_sequence_empty()};
    status = sxml_function_library_read_document_options(call, 1, &options);
    bool is_available = false;
    if (status == SXML_STATUS_OK && source.has_item) {
        sxml_string uri_reference;
        status = sxml_function_library_read_string_argument(call, 0, &uri_reference);
        sxml_item document_node = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = load_document_with_options(
                call,
                &(document_loading){.uri_reference = uri_reference.utf8,
                                    .options = &options,
                                    .error_record = NULL},
                &document_node);
        }
        if (status == SXML_STATUS_OK) {
            is_available = true;
            sxml_item_release(document_node);
        } else if (status == SXML_STATUS_DYNAMIC_ERROR) {
            /* What fn:doc would raise (FODC0002, FODC0005, FODC0006,
               FODC0014, ...) is the answer false. */
            sxml_item_heap_clear_raised_error(call->item_heap);
            status = SXML_STATUS_OK;
        }
    }
    sxml_sequence_release(options.xsd_validation);
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

/* The generate-id of a JNode (F&O 4.0, section 14.1.11, PR 2083): "j"
   tree, then "c" child index for the node and each ancestor below the
   root. A JNode is identified by its JTree and its path of child indices
   from the root (XDM 4.0, section 8.5), which the indices read upwards
   determine, so the text is unique per JNode and the same for two cells
   of one JNode. */
static sxml_status generate_json_node_id(const sxml_function_call *call, sxml_item json_node,
                                         sxml_sequence *result) {
    sxml_json_node_description description;
    sxml_status status = sxml_json_node_describe(json_node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("j"));
    if (status == SXML_STATUS_OK) {
        status = append_number(&text, description.json_tree.value);
    }
    while (status == SXML_STATUS_OK && description.has_parent) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("c"));
        if (status == SXML_STATUS_OK) {
            status = append_number(&text, description.child_index);
        }
        if (status == SXML_STATUS_OK) {
            /* The parent is borrowed from its child, which stays alive. */
            status = sxml_json_node_describe(description.parent, &description);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(
            call->item_heap, sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    return status;
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
        return generate_json_node_id(call, argument.item, result);
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
   parentless, an attribute, a namespace node or no child of its parent. */
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
    } else if (sxml_item_get_kind(argument.item) == SXML_ITEM_KIND_JSON_NODE) {
        /* The JNode the item axis selects is not a child of its parent
           (XPath 4.0, 4.7.5.1): its child_index is past the parent's
           children (src/xdm_internal.h). */
        sxml_json_node_description description;
        status = sxml_json_node_describe(argument.item, &description);
        sxml_json_node_description parent_description = {.child_count = 0};
        if (status == SXML_STATUS_OK && description.has_parent) {
            status = sxml_json_node_describe(description.parent, &parent_description);
        }
        is_child = status == SXML_STATUS_OK && description.has_parent &&
                   description.child_index < parent_description.child_count;
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

/* ---------------------------------------------------- Identifying nodes */

static const sxml_static_error not_in_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The node is in a tree whose root is not a document node (F&O 4.0, fn:id, "
        "fn:element-with-id, fn:idref)"),
};

/* Walks the whitespace-separated tokens of a text (an xs:IDREFS value). */
typedef struct token_cursor {
    sxml_byte_view text;
    size_t byte_offset;
} token_cursor;

static bool next_token(token_cursor *cursor, sxml_byte_view *token) {
    const sxml_byte_view text = cursor->text;
    while (cursor->byte_offset < text.byte_count &&
           sxml_xml_byte_is_whitespace(text.bytes[cursor->byte_offset])) {
        cursor->byte_offset += 1;
    }
    const size_t start = cursor->byte_offset;
    while (cursor->byte_offset < text.byte_count &&
           !sxml_xml_byte_is_whitespace(text.bytes[cursor->byte_offset])) {
        cursor->byte_offset += 1;
    }
    if (cursor->byte_offset == start) {
        *token = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return false;
    }
    *token = (sxml_byte_view){.bytes = text.bytes + start, .byte_count = cursor->byte_offset - start};
    return true;
}

/* The string value of a node into *item (owned), and its text. */
typedef struct node_text {
    sxml_item item;
    sxml_byte_view text;
} node_text;

static sxml_status read_node_text(const sxml_function_call *call, sxml_node_reference node,
                                  node_text *text) {
    *text = (node_text){.item = {.representation = 0}, .text = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_node_make_string_value(call->item_heap, node, &text->item);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(text->item, &datum);
    }
    if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING) {
        text->text = datum.datum_details.string.utf8;
    }
    return status;
}

/* The tree the identifying functions search: the document of $node. */
typedef struct target_document {
    sxml_tree *tree;
    uint32_t node_count;
} target_document;

static sxml_status find_target_document(const sxml_function_call *call,
                                        target_document *document) {
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    sxml_node_reference node = {.tree = NULL};
    if (status == SXML_STATUS_OK) {
        status = argument.has_item ? sxml_item_get_node(argument.item, &node)
                                   : SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        /* The root is node 0 of an arena tree's numbering (D39). */
        status = sxml_node_describe(
            (sxml_node_reference){.tree = node.tree, .node_index = {.index = 0}, .namespace_number = 0},
            &description);
    }
    if (status == SXML_STATUS_OK && description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return sxml_function_library_report(call, &not_in_document_error);
    }
    sxml_tree_description tree_description;
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_describe(node.tree, &tree_description);
    }
    if (status == SXML_STATUS_OK) {
        *document = (target_document){.tree = node.tree, .node_count = tree_description.node_count};
    }
    return status;
}

/* Which identifying function runs. */
typedef enum identifying_function {
    /* fn:id: the element with the ID, or with the is-id property. */
    SXML_IDENTIFYING_FUNCTION_ID,
    /* fn:element-with-id: the element with the ID, or the parent of an
       element with the is-id property. */
    SXML_IDENTIFYING_FUNCTION_ELEMENT_WITH_ID,
} identifying_function;

/* The first element in document order whose ID is token (F&O 4.0,
   fn:id), as an owned item in *element when is_found. */
typedef struct id_search {
    identifying_function function;
    target_document document;
    sxml_byte_view token;
} id_search;

typedef struct id_finding {
    bool is_found;
    sxml_node_reference element;
} id_finding;

static sxml_status find_element_with_id(const sxml_function_call *call, const id_search *search,
                                        id_finding *finding) {
    finding->is_found = false;
    sxml_status status = sxml_function_library_charge_steps(call, search->document.node_count);
    for (uint32_t node_index = 0; status == SXML_STATUS_OK && !finding->is_found &&
                                  node_index < search->document.node_count;
         node_index += 1) {
        const sxml_node_reference node = {.tree = search->document.tree,
                                          .node_index = {.index = node_index},
                                          .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || !description.is_id ||
            (description.node_kind != SXML_NODE_KIND_ATTRIBUTE &&
             description.node_kind != SXML_NODE_KIND_ELEMENT)) {
            continue;
        }
        node_text text;
        status = read_node_text(call, node, &text);
        const bool is_match =
            status == SXML_STATUS_OK && sxml_byte_view_equals(sxml_xml_text_trim_whitespace(text.text), search->token);
        sxml_item_release(text.item);
        if (!is_match) {
            continue;
        }
        const bool is_own_id = description.node_kind == SXML_NODE_KIND_ELEMENT &&
                               search->function == SXML_IDENTIFYING_FUNCTION_ID;
        if (is_own_id) {
            *finding = (id_finding){.is_found = true, .element = node};
            continue;
        }
        sxml_node_description parent;
        const sxml_node_reference parent_node = {
            .tree = node.tree, .node_index = description.parent, .namespace_number = 0};
        if (description.has_parent) {
            status = sxml_node_describe(parent_node, &parent);
        }
        if (status == SXML_STATUS_OK && description.has_parent &&
            parent.node_kind == SXML_NODE_KIND_ELEMENT) {
            *finding = (id_finding){.is_found = true, .element = parent_node};
        }
    }
    return status;
}

/* fn:id and fn:element-with-id ($values, $node := .) (F&O 4.0, sections
   12.4.1 and 12.4.2): for each IDREF token of $values, the first element
   in document order with that ID, in document order without duplicates.
   IDs are attributes the parser marks (xml:id, declared ID). */
static sxml_status call_identifying(const sxml_function_call *call,
                                    identifying_function function, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    target_document document = {.tree = NULL, .node_count = 0};
    sxml_status status = find_target_document(call, &document);
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        token_cursor cursor = {.text = datum.datum_details.string.utf8, .byte_offset = 0};
        sxml_byte_view token;
        while (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING &&
               next_token(&cursor, &token)) {
            if (!sxml_xml_text_is_ncname(token)) {
                continue;
            }
            id_finding finding = {.is_found = false};
            status = find_element_with_id(
                call, &(id_search){.function = function, .document = document, .token = token},
                &finding);
            if (status == SXML_STATUS_OK && finding.is_found) {
                const sxml_item element = sxml_node_item_make(finding.element);
                status = sxml_sequence_builder_append_item(builder, element);
                sxml_item_release(element);
            }
        }
    }
    sxml_sequence found = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, &found);
    }
    sxml_sequence_builder_destroy(builder);
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_make_document_order_value(
            call->dynamic_context,
            &(sxml_document_order_request){.input = found, .error_record = call->error_record},
            result);
    }
    sxml_sequence_release(found);
    return status;
}

static sxml_status call_id(const sxml_function_call *call, sxml_sequence *result) {
    return call_identifying(call, SXML_IDENTIFYING_FUNCTION_ID, result);
}

static sxml_status call_element_with_id(const sxml_function_call *call,
                                        sxml_sequence *result) {
    return call_identifying(call, SXML_IDENTIFYING_FUNCTION_ELEMENT_WITH_ID, result);
}

/* True when one of the $values strings, trimmed, is an NCName equal to
   token (F&O 4.0, fn:idref: $values[. castable as xs:NCName]). */
typedef struct candidate_match {
    bool is_match;
} candidate_match;

static sxml_status match_candidate(const sxml_function_call *call, sxml_byte_view token,
                                   candidate_match *match) {
    match->is_match = false;
    sxml_sequence_iterator iterator =
        sxml_sequence_iterator_start(sxml_function_library_get_argument(call, 0));
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && !match->is_match) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK && datum.datum_kind == SXML_DATUM_KIND_STRING) {
            const sxml_byte_view candidate = sxml_xml_text_trim_whitespace(datum.datum_details.string.utf8);
            match->is_match = sxml_xml_text_is_ncname(candidate) && sxml_byte_view_equals(candidate, token);
        }
    }
    return status;
}

/* fn:idref($values, $node := .) (F&O 4.0, section 12.4.3): the elements
   and attributes with the is-idrefs property one of whose tokens is a
   candidate ID of $values, in document order. */
static sxml_status call_idref(const sxml_function_call *call, sxml_sequence *result) {
    target_document document = {.tree = NULL, .node_count = 0};
    sxml_status status = find_target_document(call, &document);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, document.node_count);
    }
    sxml_sequence_builder *builder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(
            sxml_item_heap_as_allocator(call->item_heap),
            &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    }
    for (uint32_t node_index = 0; status == SXML_STATUS_OK && node_index < document.node_count;
         node_index += 1) {
        const sxml_node_reference node = {
            .tree = document.tree, .node_index = {.index = node_index}, .namespace_number = 0};
        sxml_node_description description;
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK || !description.is_idrefs) {
            continue;
        }
        node_text text;
        status = read_node_text(call, node, &text);
        token_cursor cursor = {.text = text.text, .byte_offset = 0};
        sxml_byte_view token;
        candidate_match match = {.is_match = false};
        while (status == SXML_STATUS_OK && !match.is_match && next_token(&cursor, &token)) {
            status = match_candidate(call, token, &match);
        }
        sxml_item_release(text.item);
        if (status == SXML_STATUS_OK && match.is_match) {
            const sxml_item found = sxml_node_item_make(node);
            status = sxml_sequence_builder_append_item(builder, found);
            sxml_item_release(found);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* -------------------------------------------------------------- fn:path */

static const sxml_static_error origin_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOPA0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The origin option of fn:path is not an ancestor of the node (F&O 4.0, fn:path)"),
};

/* F&O 4.0, fn:path: the options and their positions. */
static const sxml_option_definition path_option_definitions[] = {
    SXML_FUNCTION_OPTION("origin", SXML_TYPE_GENERIC_NODE, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("lexical", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("namespaces", SXML_TYPE_MAP, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("indexes", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

enum {
    SXML_PATH_OPTION_ORIGIN,
    SXML_PATH_OPTION_LEXICAL,
    SXML_PATH_OPTION_NAMESPACES,
    SXML_PATH_OPTION_INDEXES,
};

/* The options as fn:path uses them; items borrowed from the values. */
typedef struct path_options {
    bool has_origin;
    sxml_item origin;
    bool is_lexical;
    bool has_namespaces;
    sxml_item namespaces;
    bool has_indexes;
} path_options;

/* The single item of an option, if it has one. */
static bool read_option_item(const sxml_option_values *values, size_t option_index,
                             sxml_item *item) {
    if (!values->is_present[option_index] ||
        sxml_sequence_get_representation(values->values[option_index]) !=
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return false;
    }
    *item = values->values[option_index].content;
    return true;
}

/* A prefix the namespaces option maps to a namespace URI. */
typedef struct prefix_search {
    sxml_item namespaces;
    sxml_byte_view namespace_uri;
    /* A non-empty prefix only, or the empty one only. */
    bool wants_empty_prefix;
} prefix_search;

typedef struct prefix_finding {
    bool is_found;
    sxml_byte_view prefix;
} prefix_finding;

static sxml_status find_prefix(prefix_search search, prefix_finding *finding) {
    *finding = (prefix_finding){.is_found = false, .prefix = {.bytes = NULL, .byte_count = 0}};
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(search.namespaces, &entry_count);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(search.namespaces, entry_index, &entry);
        sxml_atomic_datum key;
        sxml_atomic_datum value;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &key);
        }
        if (status != SXML_STATUS_OK || key.datum_kind != SXML_DATUM_KIND_STRING ||
            sxml_sequence_get_representation(entry.value) !=
                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
            sxml_item_get_kind(entry.value.content) != SXML_ITEM_KIND_ATOMIC) {
            continue;
        }
        status = sxml_atomic_item_get_datum(entry.value.content, &value);
        const sxml_byte_view prefix = key.datum_details.string.utf8;
        if (status == SXML_STATUS_OK && value.datum_kind == SXML_DATUM_KIND_STRING &&
            (prefix.byte_count == 0) == search.wants_empty_prefix &&
            sxml_byte_view_equals(value.datum_details.string.utf8, search.namespace_uri)) {
            *finding = (prefix_finding){.is_found = true, .prefix = prefix};
            return SXML_STATUS_OK;
        }
    }
    return status;
}

/* Whether the namespaces option has the key "". */
typedef struct empty_key_test {
    bool has_empty_key;
} empty_key_test;

static sxml_status test_empty_key(sxml_item namespaces, empty_key_test *test) {
    bool *has_empty_key = &test->has_empty_key;
    *has_empty_key = false;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(namespaces, &entry_count);
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && !*has_empty_key && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(namespaces, entry_index, &entry);
        sxml_atomic_datum key;
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(entry.key, &key);
        }
        *has_empty_key = status == SXML_STATUS_OK && key.datum_kind == SXML_DATUM_KIND_STRING &&
                         key.datum_details.string.utf8.byte_count == 0;
    }
    return status;
}

/* What fn:path writes into: the text and the options. */
typedef struct path_writer {
    const sxml_function_call *call;
    sxml_library_text *text;
    const path_options *options;
} path_writer;

static sxml_status append_literal(const path_writer *writer, sxml_byte_view bytes) {
    return sxml_function_library_text_append(writer->text, bytes);
}

/* "Q{uri}local". */
typedef struct expanded_text {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} expanded_text;

static sxml_status append_braced(const path_writer *writer, expanded_text name) {
    sxml_status status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, name.namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("}"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, name.local_name);
    }
    return status;
}

/* "P:local". */
static sxml_status append_prefixed(const path_writer *writer, sxml_byte_view prefix,
                                   sxml_byte_view local_name) {
    sxml_status status = append_literal(writer, prefix);
    if (status == SXML_STATUS_OK && prefix.byte_count > 0) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, local_name);
    }
    return status;
}

/* A call of a function in the fn namespace ("root", "local-name"), named
   as the rules for the fn:root prefix say. */
static sxml_status append_function_call(const path_writer *writer, sxml_byte_view local_name) {
    const sxml_byte_view functions_uri =
        SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/2005/xpath-functions");
    sxml_status status = SXML_STATUS_OK;
    prefix_finding finding = {.is_found = false};
    if (writer->options->is_lexical) {
        finding = (prefix_finding){.is_found = true, .prefix = SXML_BYTE_VIEW_FROM_LITERAL("fn")};
    } else if (writer->options->has_namespaces) {
        status = find_prefix((prefix_search){.namespaces = writer->options->namespaces,
                                             .namespace_uri = functions_uri,
                                             .wants_empty_prefix = false},
                             &finding);
        if (status == SXML_STATUS_OK && !finding.is_found) {
            status = find_prefix((prefix_search){.namespaces = writer->options->namespaces,
                                                 .namespace_uri = functions_uri,
                                                 .wants_empty_prefix = true},
                                 &finding);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = finding.is_found
                     ? append_prefixed(writer, finding.prefix, local_name)
                     : append_braced(writer, (expanded_text){.namespace_uri = functions_uri,
                                                             .local_name = local_name});
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("()"));
    }
    return status;
}

/* The name of an element or attribute step. */
typedef struct step_name {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    bool is_attribute;
} step_name;

static sxml_status append_step_name(const path_writer *writer, step_name name) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(writer->call->item_heap);
    sxml_name_parts parts;
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_name_pool_get_name_parts(name_pool, name.expanded_name, &parts);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (writer->options->is_lexical) {
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_namespace_prefix(name_pool, name.namespace_prefix, &prefix);
        return status == SXML_STATUS_OK ? append_prefixed(writer, prefix, parts.local_name)
                                        : status;
    }
    if (name.is_attribute && namespace_uri.byte_count == 0) {
        return append_literal(writer, parts.local_name);
    }
    prefix_finding finding = {.is_found = false};
    if (writer->options->has_namespaces) {
        status = find_prefix((prefix_search){.namespaces = writer->options->namespaces,
                                             .namespace_uri = namespace_uri,
                                             .wants_empty_prefix = false},
                             &finding);
        if (status == SXML_STATUS_OK && !finding.is_found && !name.is_attribute) {
            status = find_prefix((prefix_search){.namespaces = writer->options->namespaces,
                                                 .namespace_uri = namespace_uri,
                                                 .wants_empty_prefix = true},
                                 &finding);
        }
        empty_key_test empty_key = {.has_empty_key = true};
        if (status == SXML_STATUS_OK && !finding.is_found && namespace_uri.byte_count == 0) {
            status = test_empty_key(writer->options->namespaces, &empty_key);
        }
        if (status == SXML_STATUS_OK && !empty_key.has_empty_key) {
            finding = (prefix_finding){.is_found = true,
                                       .prefix = {.bytes = NULL, .byte_count = 0}};
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finding.is_found ? append_prefixed(writer, finding.prefix, parts.local_name)
                            : append_braced(writer, (expanded_text){.namespace_uri = namespace_uri,
                                                                    .local_name = parts.local_name});
}

/* "[position]": among the preceding siblings, those of the same kind (and
   name, for elements and processing instructions), plus one. */
static sxml_status append_position(const path_writer *writer, sxml_node_reference node,
                                   const sxml_node_description *description) {
    if (!writer->options->has_indexes) {
        return SXML_STATUS_OK;
    }
    uint64_t position = 1;
    sxml_status status = SXML_STATUS_OK;
    bool has_previous = description->has_previous_sibling;
    sxml_node_index previous = description->previous_sibling;
    while (status == SXML_STATUS_OK && has_previous) {
        sxml_node_description sibling;
        status = sxml_node_describe(
            (sxml_node_reference){.tree = node.tree, .node_index = previous, .namespace_number = 0},
            &sibling);
        if (status == SXML_STATUS_OK && sibling.node_kind == description->node_kind &&
            sibling.node_name.index == description->node_name.index) {
            position += 1;
        }
        has_previous = status == SXML_STATUS_OK && sibling.has_previous_sibling;
        previous = sibling.previous_sibling;
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("["));
    }
    if (status == SXML_STATUS_OK) {
        status = append_number(writer->text, position);
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("]"));
    }
    return status;
}

/* The step for an XNode (F&O 4.0, fn:path, by node kind). */
static sxml_status append_xnode_step(const path_writer *writer, sxml_node_reference node) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const step_name name = {.expanded_name = description.node_name,
                            .namespace_prefix = description.namespace_prefix,
                            .is_attribute = false};
    switch (description.node_kind) {
    case SXML_NODE_KIND_ELEMENT:
        status = append_step_name(writer, name);
        break;
    case SXML_NODE_KIND_ATTRIBUTE:
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("@"));
        if (status == SXML_STATUS_OK) {
            status = append_step_name(writer, (step_name){.expanded_name = name.expanded_name,
                                                          .namespace_prefix = name.namespace_prefix,
                                                          .is_attribute = true});
        }
        return status;
    case SXML_NODE_KIND_TEXT:
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("text()"));
        break;
    case SXML_NODE_KIND_COMMENT:
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("comment()"));
        break;
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION: {
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(
            sxml_item_heap_get_name_pool(writer->call->item_heap), description.node_name, &parts);
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer,
                                    SXML_BYTE_VIEW_FROM_LITERAL("processing-instruction("));
        }
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer, parts.local_name);
        }
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL(")"));
        }
        break;
    }
    case SXML_NODE_KIND_NAMESPACE: {
        if (description.node_name.index == 0) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("namespace::*["));
            if (status == SXML_STATUS_OK) {
                status = append_function_call(writer, SXML_BYTE_VIEW_FROM_LITERAL("local-name"));
            }
            return status == SXML_STATUS_OK
                       ? append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("=\"\"]"))
                       : status;
        }
        sxml_name_parts parts;
        status = sxml_name_pool_get_name_parts(
            sxml_item_heap_get_name_pool(writer->call->item_heap), description.node_name, &parts);
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("namespace::"));
        }
        return status == SXML_STATUS_OK ? append_literal(writer, parts.local_name) : status;
    }
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ANY:
        return SXML_STATUS_OK;
    }
    return status == SXML_STATUS_OK ? append_position(writer, node, &description) : status;
}

/* The canonical text of an atomic item, quoted when asked: each " doubled. */
typedef struct key_text_request {
    sxml_item key;
    bool is_quoted;
} key_text_request;

static sxml_status append_key_text(const path_writer *writer, key_text_request request) {
    sxml_library_text key_text = {.item_heap = writer->call->item_heap};
    sxml_status status = sxml_function_library_text_append_atomic(&key_text, request.key);
    const sxml_byte_view written = sxml_function_library_text_view(&key_text);
    if (status == SXML_STATUS_OK && request.is_quoted) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
        for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < written.byte_count;
             byte_index += 1) {
            status = append_literal(
                writer, written.bytes[byte_index] == '"'
                            ? SXML_BYTE_VIEW_FROM_LITERAL("\"\"")
                            : (sxml_byte_view){.bytes = written.bytes + byte_index, .byte_count = 1});
        }
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("\""));
        }
    } else if (status == SXML_STATUS_OK) {
        status = append_literal(writer, written);
    }
    sxml_function_library_text_release(&key_text);
    return status;
}

/* The step for a JNode, by the type of its ·jkey· (F&O 4.0, fn:path). */
static sxml_status append_jnode_step(const path_writer *writer, sxml_item node) {
    sxml_json_node_description description;
    sxml_status status = sxml_json_node_describe(node, &description);
    sxml_atomic_datum key;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(description.json_key, &key);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(writer->call->item_heap);
    switch (key.datum_kind) {
    case SXML_DATUM_KIND_STRING:
        return append_key_text(writer,
                               (key_text_request){.key = description.json_key,
                                                  .is_quoted = !sxml_xml_text_is_ncname(
                                                      key.datum_details.string.utf8)});
    case SXML_DATUM_KIND_INTEGER:
    case SXML_DATUM_KIND_DECIMAL:
    case SXML_DATUM_KIND_FLOAT:
    case SXML_DATUM_KIND_DOUBLE:
        return append_key_text(writer,
                               (key_text_request){.key = description.json_key, .is_quoted = false});
    case SXML_DATUM_KIND_BOOLEAN:
        return append_literal(writer, key.datum_details.boolean
                                          ? SXML_BYTE_VIEW_FROM_LITERAL("true()")
                                          : SXML_BYTE_VIEW_FROM_LITERAL("false()"));
    case SXML_DATUM_KIND_QNAME: {
        sxml_name_parts parts;
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_name_parts(name_pool, key.datum_details.qname.expanded_name,
                                               &parts);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("#"));
        }
        return status == SXML_STATUS_OK
                   ? append_braced(writer, (expanded_text){.namespace_uri = namespace_uri,
                                                           .local_name = parts.local_name})
                   : status;
    }
    case SXML_DATUM_KIND_DATE_TIME:
    case SXML_DATUM_KIND_DURATION:
    case SXML_DATUM_KIND_BINARY:
        break;
    }
    /* child::{xs:T("S")}, T the key's built-in type annotation. */
    sxml_type_description type;
    status = sxml_type_registry_describe_type(
        sxml_item_heap_get_type_registry(writer->call->item_heap),
        sxml_item_get_type_annotation(description.json_key), &type);
    sxml_name_parts type_parts;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(name_pool, type.type_name, &type_parts);
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("child::{xs:"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, type_parts.local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("("));
    }
    if (status == SXML_STATUS_OK) {
        status = append_key_text(writer,
                                 (key_text_request){.key = description.json_key, .is_quoted = true});
    }
    return status == SXML_STATUS_OK ? append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL(")}"))
                                    : status;
}

/* The ancestor of node at distance steps up (0: the node), owned. */
typedef struct ancestor_request {
    sxml_item node;
    uint64_t distance;
} ancestor_request;

static sxml_status find_ancestor_at(ancestor_request request, sxml_item *ancestor) {
    sxml_item current = request.node;
    sxml_item_retain(current);
    sxml_status status = SXML_STATUS_OK;
    for (uint64_t step_index = 0; status == SXML_STATUS_OK && step_index < request.distance;
         step_index += 1) {
        generic_parent parent;
        status = read_parent(current, &parent);
        sxml_item_release(current);
        current = parent.parent;
        if (status == SXML_STATUS_OK && !parent.has_parent) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(current);
        return status;
    }
    *ancestor = current;
    return SXML_STATUS_OK;
}

/* The number of steps from node up to the origin (exclusive) or to the
   root, and the top node, owned. */
typedef struct path_extent {
    uint64_t step_count;
    sxml_item top;
} path_extent;

static sxml_status measure_path(const path_writer *writer, sxml_item node, path_extent *extent) {
    extent->step_count = 0;
    sxml_item current = node;
    sxml_item_retain(current);
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        if (writer->options->has_origin &&
            sxml_generic_node_is_same_node(
                (sxml_item_pair){.first = writer->options->origin, .second = current})) {
            break;
        }
        generic_parent parent;
        status = read_parent(current, &parent);
        if (status != SXML_STATUS_OK || !parent.has_parent) {
            break;
        }
        sxml_item_release(current);
        current = parent.parent;
        extent->step_count += 1;
    }
    extent->top = current;
    return status;
}

/* Writes the path of node (F&O 4.0, section 12.2.9). */
static sxml_status write_path(const path_writer *writer, sxml_item node) {
    const bool is_json = sxml_item_get_kind(node) == SXML_ITEM_KIND_JSON_NODE;
    path_extent extent = {.step_count = 0, .top = {.representation = 0}};
    sxml_status status = measure_path(writer, node, &extent);
    sxml_node_description top = {.node_kind = SXML_NODE_KIND_DOCUMENT};
    if (status == SXML_STATUS_OK && !is_json) {
        sxml_node_reference reference;
        status = sxml_item_get_node(extent.top, &reference);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(reference, &top);
        }
    }
    sxml_item_release(extent.top);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!writer->options->has_origin && extent.step_count == 0 &&
        (is_json || top.node_kind == SXML_NODE_KIND_DOCUMENT)) {
        return append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("/"));
    }
    if (!writer->options->has_origin && !is_json && top.node_kind != SXML_NODE_KIND_DOCUMENT) {
        status = append_function_call(writer, SXML_BYTE_VIEW_FROM_LITERAL("root"));
    }
    status = status == SXML_STATUS_OK
                 ? sxml_function_library_charge_steps(writer->call,
                                                      extent.step_count * extent.step_count)
                 : status;
    for (uint64_t step_index = 0; status == SXML_STATUS_OK && step_index < extent.step_count;
         step_index += 1) {
        sxml_item step_node = {.representation = 0};
        status = find_ancestor_at(
            (ancestor_request){.node = node, .distance = extent.step_count - 1 - step_index},
            &step_node);
        if (status == SXML_STATUS_OK && !(writer->options->has_origin && step_index == 0)) {
            status = append_literal(writer, SXML_BYTE_VIEW_FROM_LITERAL("/"));
        }
        if (status == SXML_STATUS_OK && is_json) {
            status = append_jnode_step(writer, step_node);
        } else if (status == SXML_STATUS_OK) {
            sxml_node_reference reference;
            status = sxml_item_get_node(step_node, &reference);
            if (status == SXML_STATUS_OK) {
                status = append_xnode_step(writer, reference);
            }
        }
        sxml_item_release(step_node);
    }
    return status;
}

/* fn:path($node := ., $options := {}) (F&O 4.0, section 12.2.9). */
static sxml_status call_path(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_option_values values = {.is_present = {false}};
    status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {path_option_definitions,
                                               sizeof(path_option_definitions) /
                                                   sizeof(path_option_definitions[0])}},
        &values);
    path_options options = {
        .has_origin = false,
        .origin = {.representation = 0},
        .is_lexical = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &values, .option_index = SXML_PATH_OPTION_LEXICAL}),
        .has_namespaces = false,
        .namespaces = {.representation = 0},
        .has_indexes = !sxml_function_library_option_is_false(
            (sxml_option_lookup){.values = &values, .option_index = SXML_PATH_OPTION_INDEXES})};
    options.has_origin = read_option_item(&values, SXML_PATH_OPTION_ORIGIN, &options.origin);
    options.has_namespaces =
        read_option_item(&values, SXML_PATH_OPTION_NAMESPACES, &options.namespaces);
    ancestry answer = {.is_ancestor = true};
    if (status == SXML_STATUS_OK && options.has_origin) {
        status = test_ancestor((sxml_item_pair){.first = options.origin, .second = argument.item},
                               &answer);
    }
    if (status == SXML_STATUS_OK && !answer.is_ancestor) {
        status = sxml_function_library_report(call, &origin_error);
    }
    sxml_library_text text = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = write_path(&(path_writer){.call = call, .text = &text, .options = &options},
                            argument.item);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap,
                                                   sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    sxml_function_library_release_options(&values);
    return status;
}

/* ---------------------------------------------------------- fn:location */

/* An xs:positiveInteger item of number, or the empty sequence for 0. */
static sxml_status make_positive_integer(const sxml_function_call *call, uint32_t number,
                                         sxml_sequence *result) {
    *result = sxml_sequence_empty();
    if (number == 0) {
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_POSITIVE_INTEGER),
            .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                      .datum_details = {.integer = (int64_t)number}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:location($node := .) (F&O 4.0, section 12.1.3): dm:location as an
   fn:location-record, from the tree's location of the node (the XML
   parser records where each node starts); the empty sequence when the
   tree knows none. The system-id is the document URI, when there is one. */
static sxml_status call_location(const sxml_function_call *call, sxml_sequence *result) {
    optional_node node;
    sxml_status status = read_optional_node(call, &node);
    *result = sxml_sequence_empty();
    if (status != SXML_STATUS_OK || !node.has_node || node.node.tree->operations->get_location == NULL) {
        return status;
    }
    sxml_source_location location = {.line_number = 0};
    status = node.node.tree->operations->get_location(node.node.tree->state, node.node.node_index,
                                                      &location);
    if (status != SXML_STATUS_OK || location.line_number == 0) {
        return status;
    }
    sxml_tree_description tree_description;
    status = sxml_tree_describe(node.node.tree, &tree_description);
    sxml_sequence field_values[4] = {sxml_sequence_empty(), sxml_sequence_empty(),
                                     sxml_sequence_empty(), sxml_sequence_empty()};
    if (status == SXML_STATUS_OK && tree_description.document_uri.byte_count > 0) {
        sxml_item uri = {.representation = 0};
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_URI),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = tree_description.document_uri,
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &uri);
        field_values[0] = status == SXML_STATUS_OK ? sxml_sequence_from_item(uri)
                                                   : sxml_sequence_empty();
    }
    if (status == SXML_STATUS_OK) {
        status = make_positive_integer(call, location.line_number, &field_values[2]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_positive_integer(call, location.column_number, &field_values[3]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("location-record"),
                .field_values = {.sequences = field_values, .sequence_count = 4}},
            result);
    }
    for (size_t field_index = 0; field_index < 4; field_index += 1) {
        sxml_sequence_release(field_values[field_index]);
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
static const sxml_function_parameter identifying_parameters[] = {
    SXML_REQUIRED_PARAMETER("values", "xs:string*"),
    SXML_CONTEXT_PARAMETER("node", "xnode()")};
static const sxml_function_parameter path_parameters[] = {
    SXML_CONTEXT_PARAMETER("node", "node()?"), SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
static const sxml_function_parameter nodes_parameters[] = {
    SXML_REQUIRED_PARAMETER("nodes", "node()*")};
static const sxml_function_parameter doc_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?")};
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
    SXML_FUNCTION_ROW("id", identifying_parameters, "element()*", 0, call_id),
    SXML_FUNCTION_ROW("element-with-id", identifying_parameters, "element()*", 0,
                      call_element_with_id),
    SXML_FUNCTION_ROW("idref", identifying_parameters, "xnode()*", 0, call_idref),
    SXML_FUNCTION_ROW("path", path_parameters, "xs:string?", 0, call_path),
    SXML_FUNCTION_ROW("location", xnode_parameters, "fn:location-record?",
                      SXML_NATIVE_FUNCTION_READS_SOURCE_LOCATIONS, call_location),
};

const sxml_native_function_view sxml_function_library_node =
    SXML_LIBRARY_PART(node_functions);
