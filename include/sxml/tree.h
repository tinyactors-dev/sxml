#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/view.h"

/*
 * Trees of XML nodes (XNodes): the tree interface, node references, document
 * order, axes, the tree builder interface and the built-in arena tree
 * (docs/DECISIONS.md, D39 and D40).
 *
 * - An sxml_tree is one XTree: a document, or a parentless element,
 *   attribute, text, comment or processing-instruction node with its
 *   subtree (XDM 4.0, section 6). Its data lives in an implementation
 *   behind the tree interface, a state pointer plus sxml_tree_operations, so
 *   a host can expose its own document model; sxml_arena_tree is the
 *   built-in implementation that the XML parser, XQuery constructors and
 *   XSLT result trees build.
 * - Nodes are numbered by sxml_node_index in document order: preorder, with
 *   an element's attributes right after the element and before its
 *   children. Every implementation must number this way; it makes document
 *   order within a tree an integer comparison and the descendant axis a
 *   range.
 * - A node is identified by an sxml_node_reference: its tree, its node
 *   index and, for a namespace node, its namespace number.
 * - Document order across trees follows the order in which their tree
 *   identifiers were issued by the item heap, so it is stable and
 *   consistent for the life of the heap (XDM 4.0, section 6.2).
 * - Trees are immutable once built. A tree counts references: node items
 *   retain the tree they point into, and its owner is told when the last
 *   reference goes.
 *
 * Producing XDM values from nodes (dm:string-value, dm:typed-value,
 * dm:base-uri as items) needs the item heap; those functions are in
 * include/sxml/xdm.h.
 *
 * Thread safety: a tree is used by the thread of its item heap (D35).
 */

/* The item heap of include/sxml/xdm.h, which issues tree identifiers. */
typedef struct sxml_item_heap sxml_item_heap;

/* ------------------------------------------------------ Node identifiers */

/*
 * The position of a node in its tree's document order, from 0 (the root).
 * Trees hold fewer than UINT32_MAX nodes.
 */
typedef struct sxml_node_index {
    uint32_t index;
} sxml_node_index;

/*
 * Identifies a tree among all trees and JTrees of one item heap. Issued in
 * increasing order and never reused, so it also orders trees.
 */
typedef struct sxml_tree_identifier {
    uint64_t value;
} sxml_tree_identifier;

typedef struct sxml_tree sxml_tree;

/*
 * A node: its tree, its index and, for a namespace node, which in-scope
 * namespace of its element it is.
 *
 * A namespace node has the node_index of its parent element and a
 * namespace_number from 1 (the position of the binding in the element's
 * in-scope namespaces, sxml_tree_get_in_scope_namespaces); every other node
 * has namespace_number 0. Namespace nodes are not stored: they are made on
 * request and identified by (element, namespace_number) (XDM 4.0, section
 * 7.5.4). Comparing (node_index, namespace_number) lexicographically gives
 * document order: an element, then its namespace nodes, then its
 * attributes and children (XDM 4.0, section 6.2).
 *
 * The one stored namespace node is a parentless one (an XQuery computed
 * namespace constructor, xsl:namespace outside an element): it is the root
 * of a tree of its own, node_index 0 with namespace_number 0, and describes
 * itself as SXML_NODE_KIND_NAMESPACE with its prefix as node name and its
 * namespace URI as content (D98).
 *
 * A plain value. It does not retain tree: it is valid while the tree is.
 */
typedef struct sxml_node_reference {
    sxml_tree *tree;
    sxml_node_index node_index;
    uint16_t namespace_number;
} sxml_node_reference;

/* ------------------------------------------------------- Node descriptions */

/*
 * What a tree reports about one node (not the namespace node of an
 * element, which sxml_node_describe makes). The accessors
 * of XDM 4.0, section 7.6, that need no XDM values.
 */
typedef struct sxml_node_description {
    sxml_node_kind node_kind;
    /* dm:node-name: elements, attributes and processing instructions (the
       target, as a name in no namespace); 0 for other kinds. */
    sxml_name_identifier node_name;
    /* The prefix the name was written with, for output; not part of the
       name (XDM 4.0, section 5.6). */
    sxml_namespace_prefix_identifier namespace_prefix;
    /* dm:parent. An attribute's parent is its element. */
    bool has_parent;
    sxml_node_index parent;
    /* The last node of this node's subtree in document order; node_index
       itself for a node without attributes or children. Descendants (and
       attributes) are exactly the indices after node_index up to this. */
    sxml_node_index last_descendant;
    /* Number of attributes of an element: they are the nodes
       node_index + 1 to node_index + attribute_count. 0 for other kinds. */
    uint32_t attribute_count;
    /* dm:children: present for documents and elements with children. The
       first child follows the attributes. */
    bool has_children;
    sxml_node_index first_child;
    sxml_node_index last_child;
    /* Siblings among the parent's children; attributes have none. */
    bool has_previous_sibling;
    sxml_node_index previous_sibling;
    bool has_next_sibling;
    sxml_node_index next_sibling;
    /* dm:type-name: xs:untyped for an unvalidated element, xs:untypedAtomic
       for an unvalidated attribute, SXML_TYPE_NONE for other kinds. */
    sxml_type_identifier type_annotation;
    /* dm:is-id, dm:is-idrefs, dm:nilled. */
    bool is_id;
    bool is_idrefs;
    bool is_nilled;
    /* A text node to serialize as CDATA sections (glossary cdata_section;
       XSLT 4.0, section 11.4.3); false for other kinds and for trees that
       do not keep it. Not part of XDM. */
    bool is_cdata_section;
    /* A text node whose characters the serializer writes unescaped
       (glossary disable_output_escaping; XSLT 4.0, section 26.1.8); false
       for other kinds and for trees that do not keep it. Not part of XDM.
       Added by D188. */
    bool disables_output_escaping;
} sxml_node_description;

/* A namespace binding: a prefix (0 for the default namespace) bound to a
   namespace URI. In a tree builder event, namespace URI 0 undeclares the
   prefix. */
typedef struct sxml_namespace_binding {
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_namespace_uri_identifier namespace_uri;
} sxml_namespace_binding;

typedef struct sxml_namespace_binding_view {
    const sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
} sxml_namespace_binding_view;

/* An unparsed entity declared in a document's DTD (XDM 4.0, sections
   7.6.15 and 7.6.16). */
typedef struct sxml_unparsed_entity {
    sxml_byte_view entity_name;
    sxml_byte_view system_identifier;
    /* Empty when the entity has no public identifier. */
    sxml_byte_view public_identifier;
} sxml_unparsed_entity;

/* What a tree reports about itself. Views are owned by the tree. */
typedef struct sxml_tree_description {
    /* Number of stored nodes (the namespace nodes of elements are not
       stored). */
    uint32_t node_count;
    /* dm:document-uri of the root, if it is a document node; empty when
       absent. */
    sxml_byte_view document_uri;
    /* The base URI of the root before any xml:base attribute applies
       (XDM 4.0, section 7.3); empty when absent. */
    sxml_byte_view base_uri;
    /* The pool every name and prefix identifier in the tree comes from. */
    sxml_name_pool *name_pool;
} sxml_tree_description;

/* --------------------------------------------------------- Tree interface */

/* A request for the typed value of a node. */
typedef struct sxml_typed_value_request {
    sxml_node_index node_index;
    /* The heap the result is allocated in. Borrowed for the call. */
    sxml_item_heap *item_heap;
    /* Receives the error when the typed value is undefined (FOTY0012 for
       an element with element-only content). May be NULL. */
    sxml_error_record *error_record;
} sxml_typed_value_request;

/* The outcome of finding an element by its ID (fn:element-with-id, fn:id). */
typedef struct sxml_element_by_id_result {
    bool is_found;
    sxml_node_index element;
} sxml_element_by_id_result;

/*
 * The operations of a tree implementation. Tables are static const. Every
 * operation receives the state given in sxml_tree_configuration. Views it
 * returns stay valid as long as the tree.
 *
 * The wrappers below check node indices against node_count before calling,
 * so implementations may trust them; implementations return
 * SXML_STATUS_INVALID_ARGUMENT when a node is of the wrong kind for the
 * operation. Operations marked optional may be NULL.
 *
 * Contract every implementation keeps: node indices are in document order
 * as described at the top of this header, and nothing about a node changes
 * while the tree is referenced.
 */
typedef struct sxml_tree_operations {
    sxml_status (*describe_tree)(void *state,
                                 sxml_tree_description *description);
    sxml_status (*describe_node)(void *state, sxml_node_index node_index,
                                 sxml_node_description *description);
    /* The content of a text, comment, processing-instruction or attribute
       node, or of a stored namespace node: its string value (the namespace
       URI), as well-formed UTF-8. */
    sxml_status (*get_node_content)(void *state, sxml_node_index node_index,
                                    sxml_byte_view *content);
    /* The in-scope namespaces of an element, without duplicates and
       without undeclarations, the xml prefix included (XDM 4.0, section
       7.5.2.2, dm:namespace-nodes). Order is stable: it gives the
       namespace numbers. */
    sxml_status (*get_in_scope_namespaces)(
        void *state, sxml_node_index element,
        sxml_namespace_binding_view *namespace_bindings);
    /* Optional. dm:typed-value of a validated node. NULL means sxml
       derives it from the string value and the type annotation (XDM 4.0,
       section 7.6.14; D165): xs:untypedAtomic for an untyped node, the
       string value cast to a simple type annotation (with the element's
       in-scope namespaces, the parent's for an attribute), and for an
       element of a complex type as its content type variety says
       (include/sxml/types.h); a nilled element has none. Every tree that
       holds only untyped nodes gets what it got before. */
    sxml_status (*get_typed_value)(void *state,
                                   const sxml_typed_value_request *request,
                                   sxml_sequence *typed_value);
    /* Optional. dm:location of a node; NULL or line_number 0 means
       unknown. */
    sxml_status (*get_location)(void *state, sxml_node_index node_index,
                                sxml_source_location *location);
    /* Optional. Finds the unparsed entity named entity->entity_name and
       fills the rest of *entity; SXML_STATUS_INDEX_OUT_OF_RANGE when there
       is none. */
    sxml_status (*find_unparsed_entity)(void *state,
                                        sxml_unparsed_entity *entity);
    /* Optional. Finds the element with an ID equal to identifier; NULL
       makes sxml scan the tree. */
    sxml_status (*find_element_by_id)(void *state, sxml_byte_view identifier,
                                      sxml_element_by_id_result *result);
} sxml_tree_operations;

/*
 * Told when a tree loses its last reference, so whoever owns the tree's
 * memory can free it. Interfaces of this kind are passed by value.
 */
typedef struct sxml_tree_owner_operations {
    /* Called once, from sxml_tree_release, when the reference count
       reaches 0. tree must not be used after release returns, except to
       finalize or destroy it, which release may do. */
    void (*release)(void *state, sxml_tree *tree);
} sxml_tree_owner_operations;

typedef struct sxml_tree_owner {
    void *state;
    const sxml_tree_owner_operations *operations;
} sxml_tree_owner;

typedef struct sxml_tree_configuration {
    /* The implementation: its state is borrowed and must outlive the
       tree; operations is a static const table. */
    void *state;
    const sxml_tree_operations *operations;
    /* Issues the tree identifier. The tree keeps a reference; item_heap
       must outlive it. The tree's name pool must be the heap's. */
    sxml_item_heap *item_heap;
    /* Told when the reference count drops to 0. operations NULL: nobody is
       told; the creator finalizes or destroys the tree itself, after the
       last item referring to it is released. */
    sxml_tree_owner owner;
} sxml_tree_configuration;

/*
 * Writes the memory requirements of a tree (the wrapper around an
 * implementation, not the implementation's data) to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_tree_memory_requirements(
    const sxml_tree_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a tree in memory, takes a new tree identifier from the item heap
 * and writes the tree's address to *tree. The tree starts with one
 * reference, owned by the caller. memory must stay valid, unmoved, until
 * sxml_tree_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration
 * (missing operations or heap, describe_tree failing, a name pool other
 * than the heap's), or SXML_STATUS_INSUFFICIENT_MEMORY or
 * SXML_STATUS_MISALIGNED_MEMORY when memory does not meet the requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_tree_initialize(
    sxml_memory_view memory, const sxml_tree_configuration *configuration,
    sxml_tree **tree);

/*
 * Ends the tree. Items still referring to it must have been released
 * before; sxml cannot check this. Does nothing when tree is NULL.
 */
SXML_EXPORT void sxml_tree_finalize(sxml_tree *tree);

/*
 * Allocates a tree through allocator and initializes it. allocator is
 * stored and must outlive the tree. Returns the failures of
 * sxml_tree_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_tree_create(
    sxml_allocator allocator, const sxml_tree_configuration *configuration,
    sxml_tree **tree);

/*
 * Finalizes a tree made by sxml_tree_create and releases its memory. Does
 * nothing when tree is NULL.
 */
SXML_EXPORT void sxml_tree_destroy(sxml_tree *tree);

/*
 * Adds a reference to tree. A tree whose count would overflow becomes
 * immortal: retain and release no longer change it.
 */
SXML_EXPORT void sxml_tree_retain(sxml_tree *tree);

/*
 * Drops a reference to tree; at 0 the owner's release is called. Does
 * nothing when tree is NULL.
 */
SXML_EXPORT void sxml_tree_release(sxml_tree *tree);

/* The identifier the item heap issued to tree. */
SXML_EXPORT sxml_tree_identifier sxml_tree_get_identifier(
    const sxml_tree *tree);

/*
 * Calls describe_tree. Returns the implementation's failures. The
 * description's views are owned by the tree.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_tree_describe(
    const sxml_tree *tree, sxml_tree_description *description);

/* ------------------------------------------------------- Node accessors */

/*
 * Describes node, a namespace node included: a namespace node has kind
 * SXML_NODE_KIND_NAMESPACE, the prefix as its node name (a name in no
 * namespace; 0 for the default namespace), its element as parent, and no
 * siblings or children (XDM 4.0, section 7.5.4.2).
 *
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE for a node index or namespace
 * number the tree does not have, and the implementation's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_describe(
    sxml_node_reference node, sxml_node_description *description);

/*
 * Writes the content of a text, comment, processing-instruction, attribute
 * or namespace node to *content: its string value (for a namespace node,
 * the namespace URI). The view is owned by the tree. Returns
 * SXML_STATUS_INVALID_ARGUMENT for a document or element node, whose string
 * value is made by sxml_node_make_string_value (include/sxml/xdm.h).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_get_content(
    sxml_node_reference node, sxml_byte_view *content);

/*
 * Writes the in-scope namespaces of element to *namespace_bindings, owned by
 * the tree. Returns SXML_STATUS_INVALID_ARGUMENT when element is not an
 * element node.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_get_in_scope_namespaces(
    sxml_node_reference element,
    sxml_namespace_binding_view *namespace_bindings);

/* True when first and second are the same node (XDM 4.0, section 6.1). */
SXML_EXPORT bool sxml_node_is_same_node(sxml_node_reference first,
                                        sxml_node_reference second);

/* Two nodes to compare. */
typedef struct sxml_node_pair {
    sxml_node_reference first;
    sxml_node_reference second;
} sxml_node_pair;

/*
 * The relative document order of two nodes (XDM 4.0, section 6.2): by tree
 * identifier for nodes of different trees, otherwise by
 * (node_index, namespace_number). O(1).
 */
SXML_EXPORT sxml_ordering sxml_node_compare_document_order(
    sxml_node_pair pair);

/* --------------------------------------------------------------- Axes */

/* The axes over XNodes (XPath 4.0, section 4.7.5). */
typedef enum sxml_axis {
    SXML_AXIS_SELF,
    SXML_AXIS_CHILD,
    SXML_AXIS_DESCENDANT,
    SXML_AXIS_DESCENDANT_OR_SELF,
    SXML_AXIS_ATTRIBUTE,
    SXML_AXIS_NAMESPACE,
    SXML_AXIS_FOLLOWING_SIBLING,
    SXML_AXIS_FOLLOWING_SIBLING_OR_SELF,
    SXML_AXIS_FOLLOWING,
    SXML_AXIS_FOLLOWING_OR_SELF,
    SXML_AXIS_PARENT,
    SXML_AXIS_ANCESTOR,
    SXML_AXIS_ANCESTOR_OR_SELF,
    SXML_AXIS_PRECEDING_SIBLING,
    SXML_AXIS_PRECEDING_SIBLING_OR_SELF,
    SXML_AXIS_PRECEDING,
    SXML_AXIS_PRECEDING_OR_SELF,
} sxml_axis;

/*
 * Walks one axis from an origin node. A plain value the caller keeps, for
 * example on the stack; its fields are private to sxml and its size is part
 * of the ABI. It does not retain the tree, which must outlive it.
 *
 * Forward axes yield nodes in document order, reverse axes (parent,
 * ancestor, preceding...) in reverse document order, so that the position
 * of a node is its proximity position (XPath 4.0, section 4.7.6).
 */
typedef struct sxml_axis_iterator {
    sxml_node_reference origin;
    sxml_axis axis;
    sxml_node_reference next_node;
    bool has_next_node;
} sxml_axis_iterator;

/* The axis to walk and where from. */
typedef struct sxml_axis_request {
    sxml_node_reference origin;
    sxml_axis axis;
} sxml_axis_request;

/*
 * Starts an iterator over request.axis from request.origin. Returns the
 * failures of sxml_node_describe.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_axis_iterator_start(
    sxml_axis_request request, sxml_axis_iterator *iterator);

/* What one call to sxml_axis_iterator_next yields. */
typedef struct sxml_node_iteration_step {
    bool has_node;
    sxml_node_reference node;
} sxml_node_iteration_step;

/*
 * Moves iterator (in/out) to the next node of its axis and writes it to
 * *step, or sets step->has_node false at the end. Returns the failures of
 * the tree implementation; on failure iterator and *step are untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_axis_iterator_next(
    sxml_axis_iterator *iterator, sxml_node_iteration_step *step);

/* ----------------------------------------------------- Tree builder */

/* Starts a document node. Views are borrowed for the call. */
typedef struct sxml_document_start_event {
    sxml_byte_view document_uri;
    sxml_byte_view base_uri;
    sxml_source_location location;
} sxml_document_start_event;

/* Starts an element. */
typedef struct sxml_element_start_event {
    sxml_name_identifier element_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    /* xs:untyped for an unvalidated element. SXML_TYPE_NONE means
       xs:untyped. */
    sxml_type_identifier type_annotation;
    bool is_id;
    bool is_idrefs;
    bool is_nilled;
    /* When true, the element does not inherit the in-scope namespaces of
       the element it is built in: it starts from the xml binding alone,
       plus its own bindings and those namespace fixup makes (XQuery 4.0
       copy-namespaces no-inherit, D69). False, the default, inherits. */
    bool ignores_parent_namespaces;
    /* Where the element starts in its source; line_number 0 when unknown. */
    sxml_source_location location;
} sxml_element_start_event;

/* An attribute of the element just started, or a parentless attribute. */
typedef struct sxml_attribute_event {
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    /* The normalized value, well-formed UTF-8. Borrowed for the call. */
    sxml_byte_view attribute_value;
    /* xs:untypedAtomic for an unvalidated attribute. SXML_TYPE_NONE means
       xs:untypedAtomic. */
    sxml_type_identifier type_annotation;
    bool is_id;
    bool is_idrefs;
    sxml_source_location location;
} sxml_attribute_event;

/* Character data. Borrowed for the call. */
typedef struct sxml_text_event {
    sxml_byte_view text;
    sxml_source_location location;
    /* Asks that the text be serialized as CDATA sections (glossary
       cdata_section; XSLT 4.0, section 11.4.3). A text node made of
       several events keeps the request when any of them has it (D163). */
    bool is_cdata_section;
    /* Asks that the text be serialized unescaped (glossary
       disable_output_escaping; XSLT 4.0, section 26.1.8). Text events that
       differ in it make separate text nodes inside a parent, because the
       property belongs to each character (D188). */
    bool disables_output_escaping;
} sxml_text_event;

typedef struct sxml_comment_event {
    sxml_byte_view comment;
    sxml_source_location location;
} sxml_comment_event;

typedef struct sxml_processing_instruction_event {
    /* An NCName. */
    sxml_byte_view target;
    sxml_byte_view data;
    sxml_source_location location;
} sxml_processing_instruction_event;

/*
 * The operations of a tree builder: the events that construct a tree, in
 * document order. Implemented by the arena tree and, in later phases, by
 * serializers, so an XSLT result can go straight to output.
 *
 * Event grammar: either one document (start_document, its content,
 * end_document) or one parentless node: an element (start_element ...
 * end_element), an attribute, a namespace binding (a parentless namespace
 * node, D98; its namespace URI must not be 0), a text, a comment or a
 * processing instruction. An unparsed entity may follow start_document. Inside an
 * element, namespace bindings and attributes come after start_element and
 * before any content.
 *
 * Builders keep XDM's constraints: adjacent text events form one text node
 * and empty text makes no node (XDM 4.0, section 7.5.7.1), except that a
 * parentless text whose events are all empty is a zero-length text node
 * (text nodes may be empty only if they have no parent; D163), and that
 * inside a parent, adjacent text events that differ in
 * disables_output_escaping form separate text nodes (D188); a second
 * attribute with the same name is handled by the builder's
 * sxml_duplicate_attribute_policy; element and attribute names whose prefix
 * is not bound, or bound to another URI, get a binding through namespace
 * fixup (XSLT 4.0, section 5.7.3). Two namespace bindings of one prefix to
 * different URIs on one element raise XQDY0102.
 *
 * Specification errors carry the XQuery error codes; an XSLT evaluator
 * rewrites them to its own (XQDY0102 to XTDE0430, for example). The event
 * grammar is the caller's contract, not a user error: XQuery and XSLT
 * evaluators check themselves that no attribute follows content (XQTY0024,
 * XTDE0410) before sending events, and the XML parser checks well-formedness
 * (WFC: Unique Att Spec included) before sending its events.
 *
 * Every operation returns SXML_STATUS_INVALID_ARGUMENT for an event out of
 * order, the allocator's failures, SXML_STATUS_LIMIT_EXCEEDED at a
 * configured limit, and SXML_STATUS_DYNAMIC_ERROR with the error record
 * filled for a specification error.
 */
typedef struct sxml_tree_builder_operations {
    sxml_status (*start_document)(void *state,
                                  const sxml_document_start_event *event);
    sxml_status (*end_document)(void *state);
    sxml_status (*start_element)(void *state,
                                 const sxml_element_start_event *event);
    sxml_status (*add_namespace_binding)(
        void *state, const sxml_namespace_binding *namespace_binding);
    sxml_status (*add_attribute)(void *state,
                                 const sxml_attribute_event *event);
    sxml_status (*end_element)(void *state);
    sxml_status (*add_text)(void *state, const sxml_text_event *event);
    sxml_status (*add_comment)(void *state, const sxml_comment_event *event);
    sxml_status (*add_processing_instruction)(
        void *state, const sxml_processing_instruction_event *event);
    sxml_status (*add_unparsed_entity)(void *state,
                                       const sxml_unparsed_entity *entity);
} sxml_tree_builder_operations;

/* The tree builder interface, passed by value. */
typedef struct sxml_tree_builder {
    void *state;
    const sxml_tree_builder_operations *operations;
} sxml_tree_builder;

/* How sxml_tree_builder_copy_node treats namespaces and types (the XQuery
   4.0 copy-namespaces declaration; XSLT 4.0 xsl:copy-of). */
typedef struct sxml_copy_request {
    sxml_node_reference node;
    /* Copy the in-scope namespaces of copied elements (preserve); when
       false, only those the names use (no-preserve). */
    bool preserves_namespaces;
    /* Let copied elements inherit the in-scope namespaces of the element
       they are copied into (inherit); when false, bindings of the new
       parent that the copy does not have are undeclared (no-inherit). */
    bool inherits_namespaces;
    /* Keep type annotations; when false, elements become xs:untyped and
       attributes xs:untypedAtomic, nilled is false, and is-id and
       is-idrefs stay as they are (XSLT 4.0, 4.3 and validation="strip"). */
    bool preserves_type_annotations;
    /* With preserves_type_annotations false: is-id and is-idrefs become
       false too, except that an xml:id attribute stays an ID (XQuery 3.1,
       3.9.1.3, construction mode strip; XQuery Update Facility 3.0, 5.4.5;
       D205). Ignored when types are kept. Added after the freeze (D99):
       false, the zero value, is the behaviour before it. */
    bool strips_identity;
} sxml_copy_request;

/*
 * Sends the events that rebuild request->node and its subtree to builder:
 * the deep copy that node constructors, xsl:copy-of and document copies
 * make. A document node is copied as a document; an attribute or namespace
 * node as an attribute or namespace binding of the element being built.
 *
 * Returns the builder's failures and those of sxml_node_describe; on
 * failure the builder has received part of the events.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_tree_builder_copy_node(
    sxml_tree_builder builder, const sxml_copy_request *request);

/* --------------------------------------------------------- Arena tree */

/* What a tree builder does with a second attribute of the same name on one
   element. */
typedef enum sxml_duplicate_attribute_policy {
    /* Raise XQDY0025 (XQuery 4.0 element constructors). */
    SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
    /* The later attribute replaces the earlier one, keeping its position
       (XSLT 4.0, section 5.7.1 "Constructing Complex Content"). */
    SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE,
} sxml_duplicate_attribute_policy;

/*
 * The built-in tree. Its nodes live in arrays indexed by node index (kind,
 * name, parent, last descendant, ...), and its text in one byte store, all
 * owned by the arena tree; the in-scope namespaces of elements that declare
 * nothing are shared with their parent.
 *
 * Life of an arena tree: initialize or create it; send events through
 * sxml_arena_tree_as_tree_builder; call sxml_arena_tree_finish, which
 * hands out the sxml_tree. The tree is then immutable.
 *
 * Thread safety: one thread at a time, the thread of its item heap.
 */
typedef struct sxml_arena_tree sxml_arena_tree;

typedef struct sxml_arena_tree_configuration {
    /* Provides memory for nodes and text as the tree grows. The null
       allocator is allowed only when initial_memory is large enough;
       growth then fails with SXML_STATUS_CAPACITY_EXCEEDED. Must outlive
       the arena tree. */
    sxml_allocator allocator;
    /* Memory used before any allocation; may be empty. Borrowed: must stay
       valid until the arena tree is finalized. */
    sxml_memory_view initial_memory;
    /* Issues the tree identifier and provides the name pool. Must outlive
       the arena tree. */
    sxml_item_heap *item_heap;
    /* Limits for untrusted input; 0 means no limit. Exceeding one gives
       SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_node_count;
    uint32_t maximum_depth;
    size_t maximum_text_byte_count;
    /* Keep the source location of each node, for dm:location and error
       messages. */
    bool keeps_locations;
    /* REJECT for the XML parser (which has checked already) and XQuery,
       REPLACE for XSLT. */
    sxml_duplicate_attribute_policy duplicate_attribute_policy;
    /* When true, the arena tree, which must have been made by
       sxml_arena_tree_create, destroys itself when its tree loses the last
       reference. Trees built during evaluation use this. When false, the
       creator destroys or finalizes it. */
    bool is_destroyed_when_unreferenced;
    /* Receives specification errors raised by building; may be NULL.
       Borrowed: must outlive the arena tree. */
    sxml_error_record *error_record;
    /* The base URI of a root that is not a document node, before any
       xml:base attribute applies (sxml_tree_description.base_uri): the
       static base URI for an XQuery constructor, the base URI of the
       instruction for XSLT (XQuery 4.0, section 5.7.1 "Direct Element
       Constructors"; XSLT 4.0, section 11.1 "Literal Result Elements").
       Empty when absent. A document node takes its base URI from its
       start event instead. Copied by sxml_arena_tree_finish; must stay
       valid until then (D98). */
    sxml_byte_view base_uri;
} sxml_arena_tree_configuration;

/*
 * Writes the memory requirements of an arena tree, without its nodes and
 * text, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an
 * invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_tree_memory_requirements(
    const sxml_arena_tree_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty arena tree in memory, ready for events, and writes its
 * address to *arena_tree. memory must stay valid, unmoved, until
 * sxml_arena_tree_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration
 * (is_destroyed_when_unreferenced is not allowed here), or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_tree_initialize(
    sxml_memory_view memory,
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree **arena_tree);

/*
 * Releases the nodes and text. Items referring to the tree must have been
 * released before. Does nothing when arena_tree is NULL.
 */
SXML_EXPORT void sxml_arena_tree_finalize(sxml_arena_tree *arena_tree);

/*
 * Allocates an arena tree through allocator and initializes it. allocator
 * is stored and must outlive the arena tree. Returns the failures of
 * sxml_arena_tree_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_tree_create(
    sxml_allocator allocator,
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree **arena_tree);

/*
 * Finalizes an arena tree made by sxml_arena_tree_create and releases its
 * memory. Does nothing when arena_tree is NULL.
 */
SXML_EXPORT void sxml_arena_tree_destroy(sxml_arena_tree *arena_tree);

/*
 * The builder interface that fills arena_tree. Valid until
 * sxml_arena_tree_finish or until arena_tree is finalized.
 */
SXML_EXPORT sxml_tree_builder sxml_arena_tree_as_tree_builder(
    sxml_arena_tree *arena_tree);

/*
 * Ends building and writes the finished tree to *tree. The caller owns one
 * reference to it. With is_destroyed_when_unreferenced, releasing the last
 * reference destroys the arena tree; otherwise the tree lives until the
 * arena tree is finalized or destroyed.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when the events do not form a
 * complete tree (an element not ended, no root), leaving *tree untouched,
 * and the failures of sxml_tree_initialize.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_arena_tree_finish(
    sxml_arena_tree *arena_tree, sxml_tree **tree);
