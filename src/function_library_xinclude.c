/*
 * XInclude processing for fn:doc, fn:doc-available, fn:parse-xml and
 * XSLT's fn:document (src/xinclude_internal.h; XML Inclusions (XInclude)
 * 1.0, Second Edition; F&O 4.0, 17.1.1 and 17.2.1, the xinclude option).
 *
 * Golden file: src/function_library_json.c (json_copy_into_document: a
 * new arena tree on the call's heap, built from builder events) and
 * src/tree.c (sxml_tree_builder_copy_node: a copy without recursion).
 *
 * The walk keeps one cursor per open element, included document and
 * fallback on an explicit stack in heap memory (D71): the cursor names the
 * next sibling to visit, and what to do when the siblings end (end the
 * element, release the included document, nothing for a fallback).
 * Leaves and attributes are copied with sxml_tree_builder_copy_node;
 * elements are started here, with exactly their own in-scope namespaces
 * (ignores_parent_namespaces), since an included element keeps the
 * namespaces of its own document (XInclude 1.0, 4.5.1).
 */
#include "xinclude_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/memory.h"
#include "text_decoding_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The XInclude namespace (XInclude 1.0, section 3). */
#define SXML_XINCLUDE_NAMESPACE_URI "http://www.w3.org/2001/XInclude"

/* ---------------------------------------------------------------- Errors */

/* F&O 4.0 names no code for a failed XInclude; fn:doc's for a document
   that cannot be read is FODC0002. */
#define SXML_XINCLUDE_FATAL_ERROR(text)                                       \
    {.error_status = SXML_STATUS_DYNAMIC_ERROR,                               \
     .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),               \
     .description = SXML_BYTE_VIEW_INITIALIZER(text)}

static const sxml_static_error xinclude_misplaced_fallback_error = SXML_XINCLUDE_FATAL_ERROR(
    "An xi:fallback element is not the child of an xi:include element (XInclude 1.0, 3.2)");

static const sxml_static_error xinclude_fallbacks_error = SXML_XINCLUDE_FATAL_ERROR(
    "An xi:include element has more than one xi:fallback child (XInclude 1.0, 3.1)");

static const sxml_static_error xinclude_parse_error = SXML_XINCLUDE_FATAL_ERROR(
    "The parse attribute of an xi:include element is neither \"xml\" nor \"text\" "
    "(XInclude 1.0, 3.1)");

static const sxml_static_error xinclude_fragment_error = SXML_XINCLUDE_FATAL_ERROR(
    "The href attribute of an xi:include element has a fragment identifier (XInclude 1.0, "
    "3.1)");

static const sxml_static_error xinclude_no_href_error = SXML_XINCLUDE_FATAL_ERROR(
    "An xi:include element has neither an href nor an xpointer attribute (XInclude 1.0, "
    "3.1)");

static const sxml_static_error xinclude_loop_error = SXML_XINCLUDE_FATAL_ERROR(
    "An xi:include element includes a document that is already being included (XInclude "
    "1.0, 4.5.1: inclusion loop)");

static const sxml_static_error xinclude_unavailable_error = SXML_XINCLUDE_FATAL_ERROR(
    "The resource an xi:include element names cannot be read, and the element has no "
    "xi:fallback (XInclude 1.0, 4.4; sxml supports no xpointer attribute)");

static const sxml_static_error xinclude_untrusted_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0016"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The document has an xi:include element, and the call does not trust external "
        "resources (F&O 4.0, fn:doc, fn:parse-xml: trust-external)"),
};

/* ----------------------------------------------------------------- Names */

/* What an element is to XInclude (section 3). */
typedef enum xinclude_element_kind {
    SXML_XINCLUDE_ELEMENT_OTHER,
    SXML_XINCLUDE_ELEMENT_INCLUDE,
    SXML_XINCLUDE_ELEMENT_FALLBACK,
} xinclude_element_kind;

/* The name of a stored node, read from its tree's pool. */
typedef struct xinclude_name {
    sxml_name_parts parts;
    sxml_byte_view namespace_uri;
} xinclude_name;

static sxml_status xinclude_read_name(sxml_node_reference node, sxml_name_identifier node_name,
                                      xinclude_name *name) {
    memset(name, 0, sizeof(*name));
    sxml_tree_description tree;
    sxml_status status = sxml_tree_describe(node.tree, &tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(tree.name_pool, node_name, &name->parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(tree.name_pool, name->parts.namespace_uri,
                                                  &name->namespace_uri);
    }
    return status;
}

static sxml_status xinclude_classify(sxml_node_reference element,
                                     const sxml_node_description *description,
                                     xinclude_element_kind *kind) {
    *kind = SXML_XINCLUDE_ELEMENT_OTHER;
    xinclude_name name;
    const sxml_status status = xinclude_read_name(element, description->node_name, &name);
    if (status != SXML_STATUS_OK ||
        !sxml_byte_view_equals(name.namespace_uri,
                               SXML_BYTE_VIEW_FROM_LITERAL(SXML_XINCLUDE_NAMESPACE_URI))) {
        return status;
    }
    if (sxml_byte_view_equals(name.parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("include"))) {
        *kind = SXML_XINCLUDE_ELEMENT_INCLUDE;
    } else if (sxml_byte_view_equals(name.parts.local_name,
                                     SXML_BYTE_VIEW_FROM_LITERAL("fallback"))) {
        *kind = SXML_XINCLUDE_ELEMENT_FALLBACK;
    }
    return SXML_STATUS_OK;
}

/* An attribute of an element to find: no-namespace (namespace_uri
   empty) or in the xml namespace. */
typedef struct xinclude_attribute_query {
    sxml_node_reference element;
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} xinclude_attribute_query;

/* The attribute's value, borrowed from the tree. */
typedef struct xinclude_attribute_value {
    bool is_present;
    sxml_byte_view value;
} xinclude_attribute_value;

static sxml_status xinclude_find_attribute(const xinclude_attribute_query *query,
                                           xinclude_attribute_value *found) {
    *found = (xinclude_attribute_value){.is_present = false,
                                        .value = {.bytes = NULL, .byte_count = 0}};
    sxml_node_description element;
    memset(&element, 0, sizeof(element));
    sxml_status status = sxml_node_describe(query->element, &element);
    for (uint32_t offset = 1; status == SXML_STATUS_OK && offset <= element.attribute_count;
         offset += 1) {
        const sxml_node_reference attribute = {
            .tree = query->element.tree,
            .node_index = {.index = query->element.node_index.index + offset},
            .namespace_number = 0};
        sxml_node_description description;
    memset(&description, 0, sizeof(description));
        status = sxml_node_describe(attribute, &description);
        xinclude_name name;
        memset(&name, 0, sizeof(name));
        if (status == SXML_STATUS_OK) {
            status = xinclude_read_name(attribute, description.node_name, &name);
        }
        if (status == SXML_STATUS_OK &&
            sxml_byte_view_equals(name.namespace_uri, query->namespace_uri) &&
            sxml_byte_view_equals(name.parts.local_name, query->local_name)) {
            found->is_present = true;
            return sxml_node_get_content(attribute, &found->value);
        }
    }
    return status;
}

/* ------------------------------------------------------------ The stack */

/* What a cursor does when its siblings end. */
typedef enum xinclude_cursor_end {
    /* A document's or fallback's children: nothing. */
    SXML_XINCLUDE_CURSOR_END_NONE,
    /* An element's children: end the element in the copy. */
    SXML_XINCLUDE_CURSOR_END_ELEMENT,
    /* An included document's children: release the document. */
    SXML_XINCLUDE_CURSOR_END_INCLUSION,
} xinclude_cursor_end;

/* One level of the walk: the next sibling to visit. */
typedef struct xinclude_cursor {
    bool has_next;
    sxml_node_reference next;
    xinclude_cursor_end end;
    /* END_INCLUSION: the included document node, retained, and the
       xi:include element whose parent its top elements are fixed up
       against (glossary inclusion_fixup). */
    sxml_item included_document;
    sxml_node_reference include_element;
} xinclude_cursor;

typedef struct xinclude_walk {
    const sxml_function_call *call;
    const sxml_xinclude_request *request;
    sxml_tree_builder builder;
    /* The tree of the document being processed: including it is a loop. */
    const sxml_tree *root_tree;
    xinclude_cursor *cursors;
    size_t depth;
    size_t capacity;
} xinclude_walk;

/* The first child of node as a cursor's next sibling. */
static sxml_status xinclude_first_child(sxml_node_reference node, xinclude_cursor *cursor) {
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    const sxml_status status = sxml_node_describe(node, &description);
    cursor->has_next = status == SXML_STATUS_OK && description.has_children;
    cursor->next = (sxml_node_reference){
        .tree = node.tree, .node_index = description.first_child, .namespace_number = 0};
    return status;
}

static sxml_status xinclude_push(xinclude_walk *walk, const xinclude_cursor *cursor) {
    if (walk->depth == walk->capacity) {
        const size_t capacity = walk->capacity == 0 ? 16 : walk->capacity * 2;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(xinclude_cursor), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_item_heap_allocate_bytes(
                walk->call->item_heap,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(xinclude_cursor)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (walk->depth > 0) {
            memcpy(memory, walk->cursors, walk->depth * sizeof(xinclude_cursor));
        }
        sxml_item_heap_release_bytes(
            walk->call->item_heap,
            (sxml_heap_bytes){.memory = walk->cursors,
                              .byte_count = walk->capacity * sizeof(xinclude_cursor)});
        walk->cursors = memory;
        walk->capacity = capacity;
    }
    walk->cursors[walk->depth] = *cursor;
    walk->depth += 1;
    return SXML_STATUS_OK;
}

/* Releases the documents of the cursors still open and the stack. */
static void xinclude_walk_release(xinclude_walk *walk) {
    for (size_t cursor_index = 0; cursor_index < walk->depth; cursor_index += 1) {
        if (walk->cursors[cursor_index].end == SXML_XINCLUDE_CURSOR_END_INCLUSION) {
            sxml_item_release(walk->cursors[cursor_index].included_document);
        }
    }
    sxml_item_heap_release_bytes(
        walk->call->item_heap,
        (sxml_heap_bytes){.memory = walk->cursors,
                          .byte_count = walk->capacity * sizeof(xinclude_cursor)});
    walk->cursors = NULL;
    walk->depth = 0;
    walk->capacity = 0;
}

/* ---------------------------------------------------------------- Fixup */

/* The xml:base and xml:lang attributes a top element of an included
   document gets (XInclude 1.0, 4.5.5 and 4.5.6). */
typedef struct xinclude_fixup {
    /* The base URIs of the element and of the include's parent, owned
       (the caller releases their values): base_uri borrows the first. */
    sxml_sequence base_uris[2];
    bool sets_base;
    sxml_byte_view base_uri;
    bool sets_language;
    sxml_byte_view language;
} xinclude_fixup;

/* dm:base-uri of a node: the owned item and its text, which it holds. */
typedef struct xinclude_base_uri {
    sxml_sequence value;
    sxml_byte_view text;
} xinclude_base_uri;

static sxml_status xinclude_read_base_uri(const xinclude_walk *walk, sxml_node_reference node,
                                          xinclude_base_uri *base_uri) {
    base_uri->text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_node_make_base_uri(
        walk->call->item_heap, &(sxml_node_value_request){.node = node, .error_record = NULL},
        &base_uri->value);
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (status == SXML_STATUS_OK &&
        sxml_sequence_get_representation(base_uri->value) ==
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_atomic_item_get_datum(base_uri->value.content, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_STRING) {
        base_uri->text = datum.datum_details.string.utf8;
    }
    return status;
}

/* The [language] of node: the nearest xml:lang of it or an ancestor
   (XML 1.0, 2.12). */
static sxml_status xinclude_language(sxml_node_reference node, xinclude_attribute_value *language) {
    *language = (xinclude_attribute_value){.is_present = false,
                                           .value = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_node_description description;
    memset(&description, 0, sizeof(description));
        status = sxml_node_describe(node, &description);
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = xinclude_find_attribute(
                &(xinclude_attribute_query){
                    .element = node,
                    .namespace_uri =
                        SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/XML/1998/namespace"),
                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("lang")},
                language);
        }
        if (status != SXML_STATUS_OK || language->is_present || !description.has_parent) {
            return status;
        }
        node.node_index = description.parent;
    }
}

/* The xml:base value of a fixup: the base URI relative to the parent's
   when it is in the parent's directory or below (chapter.xml for
   file:///d/chapter.xml under file:///d/book.xml), else the base URI
   (XInclude 1.0, 4.5.5 allows either). */
static sxml_byte_view xinclude_relative_reference(sxml_byte_view base_uri,
                                                  sxml_byte_view parent_base_uri) {
    size_t directory_length = 0;
    for (size_t byte_index = 0; byte_index < parent_base_uri.byte_count; byte_index += 1) {
        if (parent_base_uri.bytes[byte_index] == '/') {
            directory_length = byte_index + 1;
        }
    }
    if (directory_length == 0 || base_uri.byte_count <= directory_length ||
        memcmp(base_uri.bytes, parent_base_uri.bytes, directory_length) != 0) {
        return base_uri;
    }
    const sxml_byte_view relative = {.bytes = base_uri.bytes + directory_length,
                                     .byte_count = base_uri.byte_count - directory_length};
    /* A first segment with a colon would read as a scheme (RFC 3986, 4.2). */
    for (size_t byte_index = 0; byte_index < relative.byte_count; byte_index += 1) {
        if (relative.bytes[byte_index] == '/') {
            break;
        }
        if (relative.bytes[byte_index] == ':') {
            return base_uri;
        }
    }
    return relative;
}

/* Which fixup attributes the top element of an inclusion gets: those
   whose value differs from the include's parent's. */
typedef struct xinclude_fixup_request {
    sxml_node_reference element;
    sxml_node_reference include_element;
} xinclude_fixup_request;

/* Fills *fixup, whose base_uris the caller releases. */
static sxml_status xinclude_find_fixup(const xinclude_walk *walk,
                                       const xinclude_fixup_request *request,
                                       xinclude_fixup *fixup) {
    sxml_node_description include;
    memset(&include, 0, sizeof(include));
    sxml_status status = sxml_node_describe(request->include_element, &include);
    const sxml_node_reference parent = {.tree = request->include_element.tree,
                                        .node_index = include.parent,
                                        .namespace_number = 0};
    xinclude_base_uri element_base = {.value = sxml_sequence_empty(),
                                      .text = {.bytes = NULL, .byte_count = 0}};
    xinclude_base_uri parent_base = element_base;
    if (status == SXML_STATUS_OK) {
        status = xinclude_read_base_uri(walk, request->element, &element_base);
    }
    fixup->base_uris[0] = element_base.value;
    if (status == SXML_STATUS_OK) {
        status = xinclude_read_base_uri(walk, parent, &parent_base);
    }
    fixup->base_uris[1] = parent_base.value;
    xinclude_attribute_value element_language;
    xinclude_attribute_value parent_language;
    if (status == SXML_STATUS_OK) {
        status = xinclude_language(request->element, &element_language);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_language(parent, &parent_language);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    fixup->sets_base = !sxml_byte_view_equals(element_base.text, parent_base.text);
    fixup->base_uri = xinclude_relative_reference(element_base.text, parent_base.text);
    /* An element without a language under one with a language gets
       xml:lang="" (4.5.6). */
    fixup->sets_language =
        element_language.is_present != parent_language.is_present ||
        !sxml_byte_view_equals(element_language.value, parent_language.value);
    fixup->language = element_language.value;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Copying */

/* Sends an xml:base or xml:lang attribute of the element being built. */
typedef struct xinclude_xml_attribute {
    sxml_byte_view local_name;
    sxml_byte_view value;
} xinclude_xml_attribute;

static sxml_status xinclude_send_xml_attribute(const xinclude_walk *walk,
                                               const xinclude_xml_attribute *attribute) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(walk->call->item_heap);
    sxml_name_identifier name = {.index = 0};
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_status status = sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                             .local_name = attribute->local_name},
        &name);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return walk->builder.operations->add_attribute(
        walk->builder.state,
        &(sxml_attribute_event){.attribute_name = name,
                                .namespace_prefix = prefix,
                                .attribute_value = attribute->value,
                                .type_annotation = sxml_builtin_type_identifier(
                                    SXML_TYPE_XS_UNTYPED_ATOMIC),
                                .is_id = false,
                                .is_idrefs = false,
                                .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                             .line_number = 0,
                                             .column_number = 0,
                                             .byte_offset = 0}});
}

/* Whether an attribute is one a fixup replaces. */
static sxml_status xinclude_is_replaced(sxml_node_reference attribute, const xinclude_fixup *fixup,
                                        bool *is_replaced) {
    *is_replaced = false;
    if (fixup == NULL || (!fixup->sets_base && !fixup->sets_language)) {
        return SXML_STATUS_OK;
    }
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status = sxml_node_describe(attribute, &description);
    xinclude_name name;
    memset(&name, 0, sizeof(name));
    if (status == SXML_STATUS_OK) {
        status = xinclude_read_name(attribute, description.node_name, &name);
    }
    if (status == SXML_STATUS_OK && name.parts.namespace_uri.index == SXML_NAMESPACE_XML) {
        *is_replaced =
            (fixup->sets_base &&
             sxml_byte_view_equals(name.parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("base"))) ||
            (fixup->sets_language &&
             sxml_byte_view_equals(name.parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("lang")));
    }
    return status;
}

/* Starts the copy of an element: its name, its own in-scope namespaces,
   its attributes, and the fixup's (NULL when none). */
static sxml_status xinclude_start_element(const xinclude_walk *walk, sxml_node_reference element,
                                          const xinclude_fixup *fixup) {
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status = sxml_node_describe(element, &description);
    if (status == SXML_STATUS_OK) {
        status = walk->builder.operations->start_element(
            walk->builder.state,
            &(sxml_element_start_event){
                .element_name = description.node_name,
                .namespace_prefix = description.namespace_prefix,
                .type_annotation = description.type_annotation,
                .is_id = description.is_id,
                .is_idrefs = description.is_idrefs,
                .is_nilled = description.is_nilled,
                .ignores_parent_namespaces = true,
                .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                             .line_number = 0,
                             .column_number = 0,
                             .byte_offset = 0},
                .entity_base_uri = {.bytes = NULL, .byte_count = 0}});
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_in_scope_namespaces(element, &bindings);
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        if (bindings.namespace_bindings[binding_index].namespace_uri.index != SXML_NAMESPACE_XML) {
            status = walk->builder.operations->add_namespace_binding(
                walk->builder.state, &bindings.namespace_bindings[binding_index]);
        }
    }
    for (uint32_t offset = 1; status == SXML_STATUS_OK && offset <= description.attribute_count;
         offset += 1) {
        const sxml_node_reference attribute = {
            .tree = element.tree,
            .node_index = {.index = element.node_index.index + offset},
            .namespace_number = 0};
        bool is_replaced = false;
        status = xinclude_is_replaced(attribute, fixup, &is_replaced);
        if (status == SXML_STATUS_OK && !is_replaced) {
            status = sxml_tree_builder_copy_node(
                walk->builder, &(sxml_copy_request){.node = attribute,
                                                    .preserves_namespaces = true,
                                                    .inherits_namespaces = true,
                                                    .preserves_type_annotations = true,
                                                    .strips_identity = false});
        }
    }
    if (status == SXML_STATUS_OK && fixup != NULL && fixup->sets_base) {
        status = xinclude_send_xml_attribute(
            walk, &(xinclude_xml_attribute){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("base"),
                                            .value = fixup->base_uri});
    }
    if (status == SXML_STATUS_OK && fixup != NULL && fixup->sets_language) {
        status = xinclude_send_xml_attribute(
            walk, &(xinclude_xml_attribute){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("lang"),
                                            .value = fixup->language});
    }
    return status;
}

/* ------------------------------------------------------------ Including */

/* The attributes of an xi:include element (section 3.1). */
typedef struct xinclude_attributes {
    xinclude_attribute_value href;
    xinclude_attribute_value parse;
    xinclude_attribute_value xpointer;
    xinclude_attribute_value encoding;
} xinclude_attributes;

static sxml_status xinclude_read_attributes(sxml_node_reference element,
                                            xinclude_attributes *attributes) {
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    sxml_status status = xinclude_find_attribute(
        &(xinclude_attribute_query){.element = element,
                                    .namespace_uri = none,
                                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("href")},
        &attributes->href);
    if (status == SXML_STATUS_OK) {
        status = xinclude_find_attribute(
            &(xinclude_attribute_query){.element = element,
                                        .namespace_uri = none,
                                        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("parse")},
            &attributes->parse);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_find_attribute(
            &(xinclude_attribute_query){.element = element,
                                        .namespace_uri = none,
                                        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("xpointer")},
            &attributes->xpointer);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_find_attribute(
            &(xinclude_attribute_query){.element = element,
                                        .namespace_uri = none,
                                        .local_name = SXML_BYTE_VIEW_FROM_LITERAL("encoding")},
            &attributes->encoding);
    }
    return status;
}

/* The xi:fallback child of an xi:include element, if any; two are a
   fatal error (section 3.1). */
typedef struct xinclude_fallback {
    bool is_present;
    sxml_node_reference element;
} xinclude_fallback;

static sxml_status xinclude_find_fallback(const xinclude_walk *walk, sxml_node_reference element,
                                          xinclude_fallback *fallback) {
    fallback->is_present = false;
    xinclude_cursor children;
    memset(&children, 0, sizeof(children));
    sxml_status status = xinclude_first_child(element, &children);
    while (status == SXML_STATUS_OK && children.has_next) {
        sxml_node_description description;
    memset(&description, 0, sizeof(description));
        status = sxml_node_describe(children.next, &description);
        xinclude_element_kind kind = SXML_XINCLUDE_ELEMENT_OTHER;
        if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = xinclude_classify(children.next, &description, &kind);
        }
        if (status == SXML_STATUS_OK && kind == SXML_XINCLUDE_ELEMENT_FALLBACK) {
            if (fallback->is_present) {
                return sxml_error_record_report(walk->request->error_record,
                                                &xinclude_fallbacks_error);
            }
            *fallback = (xinclude_fallback){.is_present = true, .element = children.next};
        }
        children.has_next = description.has_next_sibling;
        children.next.node_index = description.next_sibling;
    }
    return status;
}

/* What an include reads: its href, against the element's base URI. */
typedef struct xinclude_resource {
    sxml_byte_view href;
    sxml_byte_view base_uri;
    sxml_byte_view encoding;
} xinclude_resource;

/* parse="text" (section 4.3): the resource's characters as a text node,
   decoded by fn:unparsed-text's rules with the encoding attribute as the
   candidate. *is_read is false when the resource cannot be read or
   decoded (a resource error). */
static sxml_status xinclude_include_text(const xinclude_walk *walk,
                                         const xinclude_resource *resource, bool *is_read) {
    *is_read = false;
    sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(walk->call->dynamic_context);
    if (loader == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_retrieved_resource retrieved;
    sxml_status status = sxml_processor_document_loader_retrieve(
        loader,
        &(sxml_resource_retrieval_request){.resource_kind = SXML_RESOURCE_KIND_TEXT,
                                           .uri_reference = resource->href,
                                           .base_uri = resource->base_uri,
                                           .encoding_name = resource->encoding,
                                           .asks_unresolved = false,
                                           .accepts_document_node = false},
        &retrieved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_library_text text = {
        .item_heap = walk->call->item_heap, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    sxml_text_decoding decoding;
    if (retrieved.is_available &&
        sxml_text_decoding_choose(
            (sxml_text_decoding_choice){.content = retrieved.content,
                                        .candidate = retrieved.encoding_name.byte_count > 0
                                                         ? retrieved.encoding_name
                                                         : resource->encoding},
            &decoding)) {
        sxml_text_decoding_failure failure = SXML_TEXT_DECODING_FAILURE_NONE;
        status = sxml_text_decode(&(sxml_text_decoding_request){.content = retrieved.content,
                                                                .decoding = decoding,
                                                                .normalizes_newlines = false,
                                                                .has_fallback = false,
                                                                .permits_any_character = false},
                                  &text, &failure);
        *is_read = status == SXML_STATUS_OK && failure == SXML_TEXT_DECODING_FAILURE_NONE;
    }
    sxml_processor_document_loader_release(loader, &retrieved);
    if (*is_read && text.byte_count > 0) {
        status = walk->builder.operations->add_text(
            walk->builder.state,
            &(sxml_text_event){.text = sxml_function_library_text_view(&text),
                               .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                            .line_number = 0,
                                            .column_number = 0,
                                            .byte_offset = 0},
                               .is_cdata_section = false,
                               .disables_output_escaping = false});
    }
    sxml_function_library_text_release(&text);
    return status;
}

/* Whether tree is the processed document's or an included document's
   on the stack (section 4.5.1: an inclusion loop). */
static bool xinclude_is_open(const xinclude_walk *walk, const sxml_tree *tree) {
    if (tree == walk->root_tree) {
        return true;
    }
    for (size_t cursor_index = 0; cursor_index < walk->depth; cursor_index += 1) {
        const xinclude_cursor *cursor = &walk->cursors[cursor_index];
        sxml_node_reference document;
        if (cursor->end == SXML_XINCLUDE_CURSOR_END_INCLUSION &&
            sxml_item_get_node(cursor->included_document, &document) == SXML_STATUS_OK &&
            document.tree == tree) {
            return true;
        }
    }
    return false;
}

/* parse="xml" (section 4.2): the document fn:doc gives for the href,
   whose children the walk visits next. *is_read is false for a resource
   error (the loader's error is raised then). */
typedef struct xinclude_document_request {
    const xinclude_resource *resource;
    sxml_node_reference include_element;
} xinclude_document_request;

static sxml_status xinclude_include_document(xinclude_walk *walk,
                                             const xinclude_document_request *request,
                                             bool *is_read) {
    *is_read = false;
    sxml_item document_node = {.representation = 0};
    sxml_status status = sxml_dynamic_context_load_document(
        walk->call->dynamic_context,
        &(sxml_document_request){.uri_reference = request->resource->href,
                                 .base_uri = request->resource->base_uri,
                                 .error_record = walk->request->error_record,
                                 .blocks_external_resources = false,
                                 .validates_dtd = false,
                                 .skips_preparation = false},
        &document_node);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return SXML_STATUS_OK;
    }
    sxml_node_reference document;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(document_node, &document);
    }
    if (status == SXML_STATUS_OK && xinclude_is_open(walk, document.tree)) {
        status = sxml_error_record_report(walk->request->error_record, &xinclude_loop_error);
    }
    xinclude_cursor cursor = {.has_next = false,
                              .next = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0},
                              .end = SXML_XINCLUDE_CURSOR_END_INCLUSION,
                              .included_document = document_node,
                              .include_element = request->include_element};
    if (status == SXML_STATUS_OK) {
        status = xinclude_first_child(document, &cursor);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_push(walk, &cursor);
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(document_node);
        return status;
    }
    *is_read = true;
    return SXML_STATUS_OK;
}

/* Replaces an xi:include element (sections 3.1, 4.2 to 4.4): its
   document or text, else its fallback's children, else a fatal error. */
static sxml_status xinclude_include(xinclude_walk *walk, sxml_node_reference element) {
    xinclude_attributes attributes;
    sxml_status status = xinclude_read_attributes(element, &attributes);
    xinclude_fallback fallback = {.is_present = false,
                                  .element = {.tree = NULL, .node_index = {.index = 0},
                                              .namespace_number = 0}};
    if (status == SXML_STATUS_OK) {
        status = xinclude_find_fallback(walk, element, &fallback);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!walk->request->trusts_external) {
        return sxml_error_record_report(walk->request->error_record, &xinclude_untrusted_error);
    }
    const bool is_text =
        attributes.parse.is_present &&
        sxml_byte_view_equals(attributes.parse.value, SXML_BYTE_VIEW_FROM_LITERAL("text"));
    if (attributes.parse.is_present && !is_text &&
        !sxml_byte_view_equals(attributes.parse.value, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
        return sxml_error_record_report(walk->request->error_record, &xinclude_parse_error);
    }
    const sxml_byte_view href = attributes.href.value;
    for (size_t byte_index = 0; byte_index < href.byte_count; byte_index += 1) {
        if (href.bytes[byte_index] == '#') {
            return sxml_error_record_report(walk->request->error_record,
                                            &xinclude_fragment_error);
        }
    }
    if (href.byte_count == 0 && !attributes.xpointer.is_present) {
        return sxml_error_record_report(walk->request->error_record, &xinclude_no_href_error);
    }
    xinclude_base_uri base_uri = {.value = sxml_sequence_empty(),
                                  .text = {.bytes = NULL, .byte_count = 0}};
    status = sxml_dynamic_context_check_trust(walk->call->dynamic_context,
                                              walk->request->error_record);
    if (status == SXML_STATUS_OK) {
        status = xinclude_read_base_uri(walk, element, &base_uri);
    }
    const xinclude_resource resource = {
        .href = href, .base_uri = base_uri.text, .encoding = attributes.encoding.value};
    /* No XPointer scheme is supported: a resource error (section 3.1). */
    bool is_read = false;
    bool has_raised = false;
    if (status == SXML_STATUS_OK && !attributes.xpointer.is_present) {
        if (is_text) {
            status = xinclude_include_text(walk, &resource, &is_read);
        } else {
            status = xinclude_include_document(
                walk,
                &(xinclude_document_request){.resource = &resource, .include_element = element},
                &is_read);
            has_raised = !is_read;
        }
    }
    sxml_sequence_release(base_uri.value);
    if (status != SXML_STATUS_OK || is_read) {
        return status;
    }
    if (!fallback.is_present) {
        return has_raised ? SXML_STATUS_DYNAMIC_ERROR
                          : sxml_error_record_report(walk->request->error_record,
                                                     &xinclude_unavailable_error);
    }
    /* A resource error with a fallback: its children instead (4.4). */
    if (has_raised) {
        sxml_item_heap_clear_raised_error(walk->call->item_heap);
    }
    xinclude_cursor cursor;
    memset(&cursor, 0, sizeof(cursor));
    cursor.end = SXML_XINCLUDE_CURSOR_END_NONE;
    status = xinclude_first_child(fallback.element, &cursor);
    if (status == SXML_STATUS_OK) {
        status = xinclude_push(walk, &cursor);
    }
    return status;
}

/* ------------------------------------------------------------- The walk */

/* Visits one node the top cursor named. */
static sxml_status xinclude_visit(xinclude_walk *walk, const xinclude_cursor *parent,
                                  sxml_node_reference node) {
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return sxml_tree_builder_copy_node(walk->builder,
                                           &(sxml_copy_request){.node = node,
                                                                .preserves_namespaces = true,
                                                                .inherits_namespaces = true,
                                                                .preserves_type_annotations = true,
                                                                .strips_identity = false});
    }
    xinclude_element_kind kind = SXML_XINCLUDE_ELEMENT_OTHER;
    status = xinclude_classify(node, &description, &kind);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (kind == SXML_XINCLUDE_ELEMENT_INCLUDE) {
        return xinclude_include(walk, node);
    }
    if (kind == SXML_XINCLUDE_ELEMENT_FALLBACK) {
        return sxml_error_record_report(walk->request->error_record,
                                        &xinclude_misplaced_fallback_error);
    }
    xinclude_fixup fixup = {.base_uris = {sxml_sequence_empty(), sxml_sequence_empty()},
                            .sets_base = false,
                            .base_uri = {.bytes = NULL, .byte_count = 0},
                            .sets_language = false,
                            .language = {.bytes = NULL, .byte_count = 0}};
    const bool is_top_element = parent->end == SXML_XINCLUDE_CURSOR_END_INCLUSION;
    if (is_top_element) {
        status = xinclude_find_fixup(
            walk,
            &(xinclude_fixup_request){.element = node, .include_element = parent->include_element},
            &fixup);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_start_element(walk, node, is_top_element ? &fixup : NULL);
    }
    sxml_sequence_release(fixup.base_uris[0]);
    sxml_sequence_release(fixup.base_uris[1]);
    xinclude_cursor cursor;
    memset(&cursor, 0, sizeof(cursor));
    cursor.end = SXML_XINCLUDE_CURSOR_END_ELEMENT;
    if (status == SXML_STATUS_OK) {
        status = xinclude_first_child(node, &cursor);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_push(walk, &cursor);
    }
    return status;
}

/* Sends the processed copy of document to the walk's builder. */
static sxml_status xinclude_send_document(xinclude_walk *walk, sxml_node_reference document) {
    sxml_tree_description tree;
    sxml_status status = sxml_tree_describe(document.tree, &tree);
    if (status == SXML_STATUS_OK) {
        status = walk->builder.operations->start_document(
            walk->builder.state,
            &(sxml_document_start_event){.document_uri = tree.document_uri,
                                         .base_uri = tree.base_uri,
                                         .location = {.source_name = {.bytes = NULL,
                                                                      .byte_count = 0},
                                                      .line_number = 0,
                                                      .column_number = 0,
                                                      .byte_offset = 0}});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_unparsed_entities(walk->builder, document.tree);
    }
    xinclude_cursor root;
    memset(&root, 0, sizeof(root));
    root.end = SXML_XINCLUDE_CURSOR_END_NONE;
    if (status == SXML_STATUS_OK) {
        status = xinclude_first_child(document, &root);
    }
    if (status == SXML_STATUS_OK) {
        status = xinclude_push(walk, &root);
    }
    while (status == SXML_STATUS_OK && walk->depth > 0) {
        xinclude_cursor *top = &walk->cursors[walk->depth - 1];
        if (!top->has_next) {
            if (top->end == SXML_XINCLUDE_CURSOR_END_ELEMENT) {
                status = walk->builder.operations->end_element(walk->builder.state);
            } else if (top->end == SXML_XINCLUDE_CURSOR_END_INCLUSION) {
                sxml_item_release(top->included_document);
            }
            walk->depth -= 1;
            continue;
        }
        const sxml_node_reference node = top->next;
        sxml_node_description description;
    memset(&description, 0, sizeof(description));
        status = sxml_node_describe(node, &description);
        top->has_next = description.has_next_sibling;
        top->next.node_index = description.next_sibling;
        /* The visit may push and so move the stack: it gets a copy. */
        const xinclude_cursor parent = *top;
        if (status == SXML_STATUS_OK) {
            status = xinclude_visit(walk, &parent, node);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = walk->builder.operations->end_document(walk->builder.state);
    }
    return status;
}

sxml_status sxml_function_library_process_xinclude(const sxml_function_call *call,
                                                   const sxml_xinclude_request *request,
                                                   sxml_item *document_node) {
    sxml_node_reference document;
    sxml_status status = sxml_item_get_node(*document_node, &document);
    sxml_tree_description tree;
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_describe(document.tree, &tree);
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    if (status != SXML_STATUS_OK ||
        !sxml_name_pool_find_namespace_uri(
            tree.name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_XINCLUDE_NAMESPACE_URI),
            &namespace_uri)) {
        /* No name of the document is in the XInclude namespace. */
        return status;
    }
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    status = sxml_arena_tree_create(
        tree_allocator,
        &(sxml_arena_tree_configuration){
            .allocator = tree_allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = call->item_heap,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = request->error_record},
        &arena_tree);
    xinclude_walk walk = {.call = call,
                          .request = request,
                          .builder = {.state = NULL, .operations = NULL},
                          .root_tree = document.tree,
                          .cursors = NULL,
                          .depth = 0,
                          .capacity = 0};
    if (status == SXML_STATUS_OK) {
        walk.builder = sxml_arena_tree_as_tree_builder(arena_tree);
        status = xinclude_send_document(&walk, document);
    }
    xinclude_walk_release(&walk);
    sxml_tree *result = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &result);
    }
    sxml_item_release(*document_node);
    *document_node = (sxml_item){.representation = 0};
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make(
        (sxml_node_reference){.tree = result, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(result);
    return SXML_STATUS_OK;
}
