/*
 * Node items (include/sxml/xdm.h; D33, D39, D60): XNodes as items, and the
 * node accessors that produce XDM values: dm:string-value, dm:typed-value
 * and dm:base-uri.
 *
 * Golden file: src/xdm_function.c (an XDM module over src/xdm_internal.h,
 * failure paths), after src/memory.c.
 *
 * A node item is inline (src/xdm_internal.h, "Item layout"): the tree in
 * payload.tree, the node index in type_or_node_index and the namespace
 * number in namespace_number. It holds one reference to its tree, which
 * sxml_item_retain and sxml_item_release in src/xdm.c keep.
 *
 * Specification: XDM 4.0, section 7.6.12 (dm:string-value), 7.6.14
 * (dm:typed-value), 7.3 and the base-uri accessors of section 7 (dm:base-uri);
 * XML Base (Second Edition), section 4.2; RFC 3986, sections 5.2 and 5.3
 * (reference resolution).
 */
#include "tree_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Node items */

sxml_item sxml_node_item_make(sxml_node_reference node) {
    sxml_item item = {
        .representation = (uint8_t)SXML_ITEM_REPRESENTATION_NODE,
        .flags = 0,
        .namespace_number = node.namespace_number,
        .type_or_node_index = node.node_index.index,
        .payload = {.bits = 0},
    };
    item.payload.tree = node.tree;
    sxml_tree_retain(node.tree);
    return item;
}

sxml_status sxml_item_get_node(sxml_item item, sxml_node_reference *node) {
    if (sxml_item_get_representation(item) != SXML_ITEM_REPRESENTATION_NODE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *node = (sxml_node_reference){
        .tree = item.payload.tree,
        .node_index = sxml_node_index_from_number(item.type_or_node_index),
        .namespace_number = item.namespace_number,
    };
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- String values */

/* Text to make into an atomic item of a string type. */
typedef struct sxml_string_item_request {
    /* xs:string, xs:untypedAtomic or xs:anyURI. */
    sxml_builtin_type type;
    sxml_byte_view text;
} sxml_string_item_request;

static sxml_status
sxml_string_item_make(sxml_item_heap *item_heap,
                      const sxml_string_item_request *request,
                      sxml_item *item) {
    sxml_string string;
    sxml_status status = sxml_string_from_utf8(request->text, &string);
    if (status != SXML_STATUS_OK) {
        /* Trees hold well-formed UTF-8 (tree.h, get_node_content). */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(request->type),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

/* A string value to make as an atomic item of a string type. */
typedef struct sxml_string_value_request {
    sxml_node_reference node;
    /* xs:string or xs:untypedAtomic. */
    sxml_builtin_type type;
} sxml_string_value_request;

/* What the text descendants of a document or element add up to. */
typedef struct sxml_text_measure {
    size_t text_node_count;
    size_t byte_count;
    /* The content of the first text node, which is the whole string value
       when there is only one. */
    sxml_byte_view first_text;
} sxml_text_measure;

/* A node and its description, as the walks over its descendants use them. */
typedef struct sxml_described_node {
    sxml_node_reference node;
    sxml_node_description description;
} sxml_described_node;

/*
 * Calls visit for each text descendant of a document or element in
 * document order: the nodes after it up to its last descendant (D39),
 * which are its descendants and attributes; attributes are skipped by kind.
 */
typedef struct sxml_text_visitor {
    void *state;
    void (*visit)(void *state, sxml_byte_view text);
} sxml_text_visitor;

static sxml_status
sxml_node_visit_text_descendants(const sxml_described_node *described,
                                 sxml_text_visitor visitor) {
    uint32_t last_number = described->description.last_descendant.index;
    for (uint32_t node_number = described->node.node_index.index + 1;
         node_number <= last_number && node_number != 0; node_number += 1) {
        sxml_node_reference descendant = sxml_node_reference_of_stored_node(
            (sxml_stored_node_request){.tree = described->node.tree,
                                       .node_number = node_number});
        sxml_node_description description;
        sxml_status status = sxml_node_describe(descendant, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind != SXML_NODE_KIND_TEXT) {
            continue;
        }
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(descendant, &text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        visitor.visit(visitor.state, text);
    }
    return SXML_STATUS_OK;
}

static void sxml_text_measure_visit(void *state, sxml_byte_view text) {
    sxml_text_measure *measure = state;
    if (measure->text_node_count == 0) {
        measure->first_text = text;
    }
    measure->text_node_count += 1;
    /* A host tree may hand out one text many times; a sum beyond size_t
       saturates, and allocating it then fails. */
    if (sxml_size_add(measure->byte_count, text.byte_count,
                      &measure->byte_count) != SXML_STATUS_OK) {
        measure->byte_count = SIZE_MAX;
    }
}

/* Where the concatenation of text descendants is copied. */
typedef struct sxml_text_copy {
    sxml_memory_view memory;
    size_t byte_count;
} sxml_text_copy;

static void sxml_text_copy_visit(void *state, sxml_byte_view text) {
    sxml_text_copy *copy = state;
    if (text.byte_count == 0 ||
        text.byte_count > copy->memory.byte_count - copy->byte_count) {
        return;
    }
    memcpy(copy->memory.bytes + copy->byte_count, text.bytes, text.byte_count);
    copy->byte_count += text.byte_count;
}

/* The text descendants of a node to make into an item of type. */
typedef struct sxml_concatenation_request {
    const sxml_described_node *described;
    sxml_builtin_type type;
} sxml_concatenation_request;

/*
 * The string value of a document or element: its text descendants
 * concatenated in document order (XDM 4.0, sections 7.2.12 and 7.3.12,
 * dm:string-value). One text node needs no copy of its own; more are
 * gathered in heap memory released before returning.
 */
static sxml_status
sxml_node_make_concatenated_text(sxml_item_heap *item_heap,
                                 const sxml_concatenation_request *request,
                                 sxml_item *item) {
    const sxml_described_node *described = request->described;
    sxml_text_measure measure = {.text_node_count = 0,
                                 .byte_count = 0,
                                 .first_text = {.bytes = NULL,
                                                .byte_count = 0}};
    sxml_status status = sxml_node_visit_text_descendants(
        described, (sxml_text_visitor){.state = &measure,
                                       .visit = sxml_text_measure_visit});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (measure.text_node_count <= 1) {
        return sxml_string_item_make(
            item_heap,
            &(sxml_string_item_request){.type = request->type,
                                        .text = measure.first_text},
            item);
    }
    void *memory = NULL;
    status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = measure.byte_count,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_text_copy copy = {
        .memory = {.bytes = memory, .byte_count = measure.byte_count},
        .byte_count = 0};
    status = sxml_node_visit_text_descendants(
        described,
        (sxml_text_visitor){.state = &copy, .visit = sxml_text_copy_visit});
    if (status == SXML_STATUS_OK) {
        status = sxml_string_item_make(
            item_heap,
            &(sxml_string_item_request){
                .type = request->type,
                .text = {.bytes = copy.memory.bytes,
                         .byte_count = copy.byte_count}},
            item);
    }
    sxml_item_heap_release_bytes(
        item_heap, (sxml_heap_bytes){.memory = memory,
                                     .byte_count = measure.byte_count});
    return status;
}

/*
 * dm:string-value of request->node as an item of request->type. Documents
 * and elements concatenate their text descendants; every other kind has
 * its content as string value (XDM 4.0, section 7; a namespace node's is
 * its URI).
 */
static sxml_status
sxml_node_make_string_value_as(sxml_item_heap *item_heap,
                               const sxml_string_value_request *request,
                               sxml_item *item) {
    sxml_described_node described = {.node = request->node};
    sxml_status status =
        sxml_node_describe(request->node, &described.description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (described.description.node_kind == SXML_NODE_KIND_DOCUMENT ||
        described.description.node_kind == SXML_NODE_KIND_ELEMENT) {
        return sxml_node_make_concatenated_text(
            item_heap,
            &(sxml_concatenation_request){.described = &described,
                                          .type = request->type},
            item);
    }
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    status = sxml_node_get_content(request->node, &content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_string_item_make(
        item_heap,
        &(sxml_string_item_request){.type = request->type, .text = content},
        item);
}

sxml_status sxml_node_make_string_value(sxml_item_heap *item_heap,
                                        sxml_node_reference node,
                                        sxml_item *item) {
    return sxml_node_make_string_value_as(
        item_heap,
        &(sxml_string_value_request){.node = node,
                                     .type = SXML_TYPE_XS_STRING},
        item);
}

/* -------------------------------------------------------- Typed values */

/*
 * True for the annotations of unvalidated nodes, whose typed value sxml
 * derives from the string value: xs:untyped, xs:untypedAtomic and none.
 */
static bool sxml_type_annotation_is_untyped(sxml_type_identifier annotation) {
    return annotation.index == SXML_TYPE_NONE ||
           annotation.index == SXML_TYPE_XS_UNTYPED ||
           annotation.index == SXML_TYPE_XS_UNTYPED_ATOMIC;
}

/* The tree's typed value of a validated element or attribute. */
static sxml_status
sxml_node_get_validated_typed_value(sxml_item_heap *item_heap,
                                    const sxml_node_value_request *request,
                                    sxml_sequence *typed_value) {
    const sxml_tree *tree = request->node.tree;
    sxml_sequence new_value = sxml_sequence_empty();
    sxml_status status = tree->operations->get_typed_value(
        tree->state,
        &(sxml_typed_value_request){.node_index = request->node.node_index,
                                    .item_heap = item_heap,
                                    .error_record = request->error_record},
        &new_value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *typed_value = new_value;
    return SXML_STATUS_OK;
}

sxml_status sxml_node_make_typed_value(sxml_item_heap *item_heap,
                                       const sxml_node_value_request *request,
                                       sxml_sequence *typed_value) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(request->node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* XDM 4.0, section 7.6.14 and the dm:typed-value accessor of each node
       kind: documents, text and unvalidated elements and attributes give
       xs:untypedAtomic; comments, processing instructions and namespace
       nodes xs:string. */
    sxml_builtin_type type = SXML_TYPE_XS_UNTYPED_ATOMIC;
    switch (description.node_kind) {
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
        if (!sxml_type_annotation_is_untyped(description.type_annotation) &&
            request->node.tree->operations->get_typed_value != NULL) {
            return sxml_node_get_validated_typed_value(item_heap, request,
                                                       typed_value);
        }
        break;
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_TEXT:
        break;
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
    case SXML_NODE_KIND_NAMESPACE:
        type = SXML_TYPE_XS_STRING;
        break;
    case SXML_NODE_KIND_ANY:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    status = sxml_node_make_string_value_as(
        item_heap,
        &(sxml_string_value_request){.node = request->node, .type = type},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *typed_value = sxml_sequence_from_item(item);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------- Reference resolution */

/*
 * A URI reference split into its components (RFC 3986, appendix B). The
 * views point into the text that was split.
 */
typedef struct sxml_uri_reference {
    bool has_scheme;
    sxml_byte_view scheme;
    bool has_authority;
    sxml_byte_view authority;
    sxml_byte_view path;
    bool has_query;
    sxml_byte_view query;
    bool has_fragment;
    sxml_byte_view fragment;
} sxml_uri_reference;

static sxml_byte_view sxml_byte_view_drop_prefix(sxml_byte_view text,
                                                 size_t byte_count) {
    if (byte_count >= text.byte_count) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = text.bytes + byte_count,
                            .byte_count = text.byte_count - byte_count};
}

static sxml_byte_view sxml_byte_view_keep_prefix(sxml_byte_view text,
                                                 size_t byte_count) {
    if (byte_count >= text.byte_count) {
        return text;
    }
    return (sxml_byte_view){.bytes = text.bytes, .byte_count = byte_count};
}

/* The number of bytes of text before the first byte that is one of
   delimiters; text.byte_count when there is none. */
static size_t sxml_byte_view_span_until(sxml_byte_view text,
                                        sxml_byte_view delimiters) {
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        if (memchr(delimiters.bytes, text.bytes[byte_index],
                   delimiters.byte_count) != NULL) {
            return byte_index;
        }
    }
    return text.byte_count;
}

static bool sxml_uri_text_starts_with(sxml_byte_view text,
                                       sxml_byte_view prefix) {
    return text.byte_count >= prefix.byte_count &&
           (prefix.byte_count == 0 ||
            memcmp(text.bytes, prefix.bytes, prefix.byte_count) == 0);
}

/* Splits text as the regular expression of RFC 3986, appendix B does:
   ^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))? */
static sxml_uri_reference sxml_uri_reference_split(sxml_byte_view text) {
    const sxml_byte_view empty = {.bytes = NULL, .byte_count = 0};
    sxml_uri_reference reference = {
        .has_scheme = false, .scheme = empty, .has_authority = false,
        .authority = empty,  .path = empty,   .has_query = false,
        .query = empty,      .has_fragment = false, .fragment = empty};
    sxml_byte_view rest = text;
    size_t scheme_byte_count =
        sxml_byte_view_span_until(rest, SXML_BYTE_VIEW_FROM_LITERAL(":/?#"));
    if (scheme_byte_count > 0 && scheme_byte_count < rest.byte_count &&
        rest.bytes[scheme_byte_count] == ':') {
        reference.has_scheme = true;
        reference.scheme = sxml_byte_view_keep_prefix(rest, scheme_byte_count);
        rest = sxml_byte_view_drop_prefix(rest, scheme_byte_count + 1);
    }
    if (sxml_uri_text_starts_with(rest, SXML_BYTE_VIEW_FROM_LITERAL("//"))) {
        rest = sxml_byte_view_drop_prefix(rest, 2);
        size_t authority_byte_count =
            sxml_byte_view_span_until(rest, SXML_BYTE_VIEW_FROM_LITERAL("/?#"));
        reference.has_authority = true;
        reference.authority =
            sxml_byte_view_keep_prefix(rest, authority_byte_count);
        rest = sxml_byte_view_drop_prefix(rest, authority_byte_count);
    }
    size_t path_byte_count =
        sxml_byte_view_span_until(rest, SXML_BYTE_VIEW_FROM_LITERAL("?#"));
    reference.path = sxml_byte_view_keep_prefix(rest, path_byte_count);
    rest = sxml_byte_view_drop_prefix(rest, path_byte_count);
    if (rest.byte_count > 0 && rest.bytes[0] == '?') {
        rest = sxml_byte_view_drop_prefix(rest, 1);
        size_t query_byte_count =
            sxml_byte_view_span_until(rest, SXML_BYTE_VIEW_FROM_LITERAL("#"));
        reference.has_query = true;
        reference.query = sxml_byte_view_keep_prefix(rest, query_byte_count);
        rest = sxml_byte_view_drop_prefix(rest, query_byte_count);
    }
    if (rest.byte_count > 0 && rest.bytes[0] == '#') {
        reference.has_fragment = true;
        reference.fragment = sxml_byte_view_drop_prefix(rest, 1);
    }
    return reference;
}

/*
 * Text written into memory sized beforehand. An append that would not fit
 * is dropped; the sizes below make that impossible.
 */
typedef struct sxml_uri_output {
    sxml_memory_view memory;
    size_t byte_count;
} sxml_uri_output;

static void sxml_uri_output_append(sxml_uri_output *output,
                                   sxml_byte_view text) {
    if (text.byte_count == 0 ||
        text.byte_count > output->memory.byte_count - output->byte_count) {
        return;
    }
    memcpy(output->memory.bytes + output->byte_count, text.bytes,
           text.byte_count);
    output->byte_count += text.byte_count;
}

static sxml_byte_view sxml_uri_output_view(const sxml_uri_output *output) {
    return (sxml_byte_view){.bytes = output->memory.bytes,
                            .byte_count = output->byte_count};
}

/* A path being written by remove_dot_segments: the output and where the
   path starts in it, which removing a segment never goes before. */
typedef struct sxml_path_output {
    sxml_uri_output *output;
    size_t path_start;
} sxml_path_output;

/* Removes the last segment and its preceding "/", if any, from the output
   (RFC 3986, section 5.2.4, step 2C). */
static void sxml_path_output_remove_last_segment(sxml_path_output path) {
    size_t end = path.output->byte_count;
    while (end > path.path_start && path.output->memory.bytes[end - 1] != '/') {
        end -= 1;
    }
    if (end > path.path_start) {
        end -= 1;
    }
    path.output->byte_count = end;
}

/* Appends input to the output with its dot segments removed (RFC 3986,
   section 5.2.4, remove_dot_segments). */
static void sxml_path_output_append_without_dot_segments(
    sxml_path_output path, sxml_byte_view input) {
    const sxml_byte_view slash = SXML_BYTE_VIEW_FROM_LITERAL("/");
    while (input.byte_count > 0) {
        if (sxml_uri_text_starts_with(input,
                                       SXML_BYTE_VIEW_FROM_LITERAL("../"))) {
            input = sxml_byte_view_drop_prefix(input, 3);
        } else if (sxml_uri_text_starts_with(
                       input, SXML_BYTE_VIEW_FROM_LITERAL("./")) ||
                   sxml_uri_text_starts_with(
                       input, SXML_BYTE_VIEW_FROM_LITERAL("/./"))) {
            /* Steps 2A ("./") and 2B ("/./" becomes "/"). */
            input = sxml_byte_view_drop_prefix(input, 2);
        } else if (sxml_byte_view_equals(input,
                                         SXML_BYTE_VIEW_FROM_LITERAL("/."))) {
            input = slash;
        } else if (sxml_uri_text_starts_with(
                       input, SXML_BYTE_VIEW_FROM_LITERAL("/../"))) {
            input = sxml_byte_view_drop_prefix(input, 3);
            sxml_path_output_remove_last_segment(path);
        } else if (sxml_byte_view_equals(input,
                                         SXML_BYTE_VIEW_FROM_LITERAL("/.."))) {
            input = slash;
            sxml_path_output_remove_last_segment(path);
        } else if (sxml_byte_view_equals(input,
                                         SXML_BYTE_VIEW_FROM_LITERAL(".")) ||
                   sxml_byte_view_equals(input,
                                         SXML_BYTE_VIEW_FROM_LITERAL(".."))) {
            input.byte_count = 0;
        } else {
            /* Step 2E: the first segment, with its leading "/", up to the
               next "/". */
            size_t leading_byte_count = input.bytes[0] == '/' ? 1 : 0;
            size_t segment_byte_count =
                leading_byte_count +
                sxml_byte_view_span_until(
                    sxml_byte_view_drop_prefix(input, leading_byte_count),
                    slash);
            sxml_uri_output_append(
                path.output, sxml_byte_view_keep_prefix(input,
                                                        segment_byte_count));
            input = sxml_byte_view_drop_prefix(input, segment_byte_count);
        }
    }
}

/* A relative reference to resolve against a base URI. */
typedef struct sxml_resolution_request {
    sxml_byte_view base_uri;
    sxml_byte_view reference;
} sxml_resolution_request;

/* The bytes the resolution of request can need, for its output and for the
   merged path: the components come from the base or the reference, plus a
   "/" that merging may add (RFC 3986, section 5.2.3). */
static sxml_status
sxml_resolution_get_byte_count(const sxml_resolution_request *request,
                               size_t *byte_count) {
    size_t sum = 0;
    sxml_status status = sxml_size_add(request->base_uri.byte_count,
                                       request->reference.byte_count, &sum);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sum, 1, &sum);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(sum, 2, &sum);
    }
    if (status == SXML_STATUS_OK) {
        *byte_count = sum;
    }
    return status;
}

/* Writes the path of the target URI for a reference whose path is relative
   and not empty: merge, then remove_dot_segments (RFC 3986, sections
   5.2.2 and 5.2.3). merge_memory holds the merged path. */
typedef struct sxml_path_merge {
    const sxml_uri_reference *base;
    const sxml_uri_reference *reference;
    sxml_memory_view merge_memory;
} sxml_path_merge;

static void sxml_path_output_append_merged(sxml_path_output path,
                                           const sxml_path_merge *merge) {
    sxml_uri_output merged = {.memory = merge->merge_memory, .byte_count = 0};
    if (merge->base->has_authority && merge->base->path.byte_count == 0) {
        sxml_uri_output_append(&merged, SXML_BYTE_VIEW_FROM_LITERAL("/"));
    } else {
        size_t kept_byte_count = merge->base->path.byte_count;
        while (kept_byte_count > 0 &&
               merge->base->path.bytes[kept_byte_count - 1] != '/') {
            kept_byte_count -= 1;
        }
        sxml_uri_output_append(
            &merged,
            sxml_byte_view_keep_prefix(merge->base->path, kept_byte_count));
    }
    sxml_uri_output_append(&merged, merge->reference->path);
    sxml_path_output_append_without_dot_segments(
        path, sxml_uri_output_view(&merged));
}

/*
 * Resolves request->reference against request->base_uri into output,
 * whose memory has sxml_resolution_get_byte_count bytes: the first half
 * receives the target URI, the second the merged path (RFC 3986, section
 * 5.2.2, strict, and the recomposition of section 5.3).
 */
static void sxml_uri_output_resolve(sxml_uri_output *output,
                                    const sxml_resolution_request *request) {
    sxml_uri_reference base = sxml_uri_reference_split(request->base_uri);
    sxml_uri_reference reference =
        sxml_uri_reference_split(request->reference);
    size_t half_byte_count = output->memory.byte_count / 2;
    sxml_memory_view merge_memory = {
        .bytes = output->memory.bytes + half_byte_count,
        .byte_count = half_byte_count};
    output->memory.byte_count = half_byte_count;
    /* The components of the target URI: from the reference down to the
       first component it has, from the base above that. */
    const sxml_uri_reference *scheme_source =
        reference.has_scheme ? &reference : &base;
    const sxml_uri_reference *authority_source =
        reference.has_scheme || reference.has_authority ? &reference : &base;
    bool takes_reference_path = reference.has_scheme ||
                                reference.has_authority ||
                                reference.path.byte_count > 0;
    const sxml_uri_reference *query_source =
        takes_reference_path || reference.has_query ? &reference : &base;
    if (scheme_source->has_scheme) {
        sxml_uri_output_append(output, scheme_source->scheme);
        sxml_uri_output_append(output, SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    if (authority_source->has_authority) {
        sxml_uri_output_append(output, SXML_BYTE_VIEW_FROM_LITERAL("//"));
        sxml_uri_output_append(output, authority_source->authority);
    }
    sxml_path_output path = {.output = output,
                             .path_start = output->byte_count};
    if (!takes_reference_path) {
        sxml_uri_output_append(output, base.path);
    } else if (reference.has_scheme || reference.has_authority ||
               reference.path.bytes[0] == '/') {
        sxml_path_output_append_without_dot_segments(path, reference.path);
    } else {
        sxml_path_output_append_merged(
            path, &(sxml_path_merge){.base = &base,
                                     .reference = &reference,
                                     .merge_memory = merge_memory});
    }
    if (query_source->has_query) {
        sxml_uri_output_append(output, SXML_BYTE_VIEW_FROM_LITERAL("?"));
        sxml_uri_output_append(output, query_source->query);
    }
    if (reference.has_fragment) {
        sxml_uri_output_append(output, SXML_BYTE_VIEW_FROM_LITERAL("#"));
        sxml_uri_output_append(output, reference.fragment);
    }
}

bool sxml_uri_has_scheme(sxml_byte_view text) {
    return sxml_uri_reference_split(text).has_scheme;
}

sxml_status sxml_uri_resolve(const sxml_uri_resolution_request *request,
                             sxml_uri_resolution *resolution) {
    if (request->base_uri.byte_count == 0 ||
        sxml_uri_reference_split(request->reference).has_scheme) {
        *resolution = (sxml_uri_resolution){.uri = request->reference,
                                            .memory = NULL};
        return SXML_STATUS_OK;
    }
    const sxml_resolution_request resolution_request = {
        .base_uri = request->base_uri, .reference = request->reference};
    size_t byte_count = 0;
    sxml_status status =
        sxml_resolution_get_byte_count(&resolution_request, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            request->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_uri_output output = {
        .memory = {.bytes = memory, .byte_count = byte_count},
        .byte_count = 0};
    sxml_uri_output_resolve(&output, &resolution_request);
    *resolution = (sxml_uri_resolution){.uri = sxml_uri_output_view(&output),
                                        .memory = memory};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Base URIs */

/* The xml:base attribute of an element, if it has one. */
typedef struct sxml_base_attribute_lookup {
    bool is_found;
    sxml_byte_view value;
} sxml_base_attribute_lookup;

/* An element and the identifier of xml:base in its tree's pool. */
typedef struct sxml_base_attribute_request {
    sxml_node_reference element;
    const sxml_node_description *description;
    sxml_name_identifier xml_base_name;
} sxml_base_attribute_request;

/* Finds the xml:base attribute among the attributes of an element, which
   are the nodes right after it (D39). */
static sxml_status
sxml_element_find_base_attribute(const sxml_base_attribute_request *request,
                                 sxml_base_attribute_lookup *lookup) {
    uint32_t element_number = request->element.node_index.index;
    for (uint32_t attribute_offset = 1;
         attribute_offset <= request->description->attribute_count;
         attribute_offset += 1) {
        sxml_node_reference attribute = sxml_node_reference_of_stored_node(
            (sxml_stored_node_request){.tree = request->element.tree,
                                       .node_number = element_number +
                                                      attribute_offset});
        sxml_node_description description;
        sxml_status status = sxml_node_describe(attribute, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_name.index == request->xml_base_name.index) {
            sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(attribute, &value);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            *lookup = (sxml_base_attribute_lookup){.is_found = true,
                                                   .value = value};
            return SXML_STATUS_OK;
        }
    }
    *lookup = (sxml_base_attribute_lookup){
        .is_found = false, .value = {.bytes = NULL, .byte_count = 0}};
    return SXML_STATUS_OK;
}

/* The xml:base values that decide an element's base URI, innermost first:
   on the element and its ancestor elements, up to and including the first
   that is an absolute URI (XML Base, section 4.2). */
typedef struct sxml_base_chain {
    sxml_node_reference element;
    sxml_name_identifier xml_base_name;
    /* NULL to count only. */
    sxml_byte_view *values;
    size_t value_count;
} sxml_base_chain;

/* Walks the chain (in/out): fills chain->values when it is not NULL and
   sets value_count. */
static sxml_status sxml_base_chain_walk(sxml_base_chain *chain) {
    sxml_node_reference element = chain->element;
    size_t value_count = 0;
    for (;;) {
        sxml_node_description description;
        sxml_status status = sxml_node_describe(element, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            break;
        }
        sxml_base_attribute_lookup lookup;
        status = sxml_element_find_base_attribute(
            &(sxml_base_attribute_request){.element = element,
                                           .description = &description,
                                           .xml_base_name =
                                               chain->xml_base_name},
            &lookup);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (lookup.is_found) {
            if (chain->values != NULL) {
                chain->values[value_count] = lookup.value;
            }
            value_count += 1;
            if (sxml_uri_reference_split(lookup.value).has_scheme) {
                break;
            }
        }
        if (!description.has_parent) {
            break;
        }
        element.node_index = description.parent;
    }
    chain->value_count = value_count;
    return SXML_STATUS_OK;
}

/* A base URI being resolved: the latest result and the heap memory that
   holds it, if any. */
typedef struct sxml_base_resolution {
    sxml_item_heap *item_heap;
    sxml_byte_view base_uri;
    sxml_heap_bytes memory;
} sxml_base_resolution;

/* Resolves reference against resolution->base_uri (in/out). A relative
   reference with no base to resolve against is kept as written: RFC 3986,
   section 5.2.1, leaves that case to the application. */
static sxml_status
sxml_base_resolution_apply(sxml_base_resolution *resolution,
                           sxml_byte_view reference) {
    if (resolution->base_uri.byte_count == 0 &&
        !sxml_uri_reference_split(reference).has_scheme) {
        resolution->base_uri = reference;
        return SXML_STATUS_OK;
    }
    const sxml_resolution_request request = {
        .base_uri = resolution->base_uri, .reference = reference};
    size_t byte_count = 0;
    sxml_status status = sxml_resolution_get_byte_count(&request, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            resolution->item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_uri_output output = {
        .memory = {.bytes = memory, .byte_count = byte_count},
        .byte_count = 0};
    sxml_uri_output_resolve(&output, &request);
    sxml_item_heap_release_bytes(resolution->item_heap, resolution->memory);
    resolution->memory =
        (sxml_heap_bytes){.memory = memory, .byte_count = byte_count};
    resolution->base_uri = sxml_uri_output_view(&output);
    return SXML_STATUS_OK;
}

/* The base URI of an element, as an owned sequence: the tree's base URI
   with the chain of xml:base values resolved against it from the
   outermost in. */
static sxml_status sxml_element_make_base_uri(sxml_item_heap *item_heap,
                                              sxml_node_reference element,
                                              sxml_sequence *base_uri) {
    sxml_tree_description tree_description;
    sxml_status status =
        sxml_tree_describe(element.tree, &tree_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_base_chain chain = {.element = element,
                             .xml_base_name = {.index = 0},
                             .values = NULL,
                             .value_count = 0};
    bool has_xml_base_name = sxml_name_pool_find_name(
        element.tree->name_pool,
        &(sxml_name_request){.namespace_uri =
                                 sxml_well_known_namespace_identifier(
                                     SXML_NAMESPACE_XML),
                             .local_name =
                                 SXML_BYTE_VIEW_FROM_LITERAL("base")},
        &chain.xml_base_name);
    if (has_xml_base_name) {
        status = sxml_base_chain_walk(&chain);
    }
    size_t values_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(chain.value_count, sizeof(sxml_byte_view),
                                    &values_byte_count);
    }
    void *values_memory = NULL;
    if (status == SXML_STATUS_OK && chain.value_count > 0) {
        status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = values_byte_count,
                                      .alignment = alignof(sxml_byte_view)},
            &values_memory);
        if (status == SXML_STATUS_OK) {
            chain.values = values_memory;
            status = sxml_base_chain_walk(&chain);
        }
    }
    sxml_base_resolution resolution = {
        .item_heap = item_heap,
        .base_uri = tree_description.base_uri,
        .memory = {.memory = NULL, .byte_count = 0}};
    for (size_t value_number = chain.value_count;
         status == SXML_STATUS_OK && value_number > 0; value_number -= 1) {
        status = sxml_base_resolution_apply(&resolution,
                                            chain.values[value_number - 1]);
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK && resolution.base_uri.byte_count > 0) {
        status = sxml_string_item_make(
            item_heap,
            &(sxml_string_item_request){.type = SXML_TYPE_XS_ANY_URI,
                                        .text = resolution.base_uri},
            &item);
    }
    sxml_item_heap_release_bytes(item_heap, resolution.memory);
    sxml_item_heap_release_bytes(
        item_heap, (sxml_heap_bytes){.memory = values_memory,
                                     .byte_count = values_byte_count});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *base_uri = item.representation == SXML_ITEM_REPRESENTATION_NONE
                    ? sxml_sequence_empty()
                    : sxml_sequence_from_item(item);
    return SXML_STATUS_OK;
}

/* The base URI of the tree itself (a document node's, or that of a
   parentless processing instruction), as an owned sequence. */
static sxml_status sxml_tree_make_base_uri(sxml_item_heap *item_heap,
                                           const sxml_tree *tree,
                                           sxml_sequence *base_uri) {
    sxml_tree_description description;
    sxml_status status = sxml_tree_describe(tree, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.base_uri.byte_count == 0) {
        *base_uri = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    status = sxml_string_item_make(
        item_heap,
        &(sxml_string_item_request){.type = SXML_TYPE_XS_ANY_URI,
                                    .text = description.base_uri},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *base_uri = sxml_sequence_from_item(item);
    return SXML_STATUS_OK;
}

sxml_status sxml_node_make_base_uri(sxml_item_heap *item_heap,
                                    const sxml_node_value_request *request,
                                    sxml_sequence *base_uri) {
    /* A loop, not recursion (D71): it goes up to the parent at most once,
       since the parent of a node with a parent is an element or a
       document. */
    sxml_node_reference node = request->node;
    for (;;) {
        sxml_node_description description;
        sxml_status status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* XDM 4.0, section 7: a namespace node has no base URI; attributes,
           text and comments have their parent's, and none without a
           parent; a processing instruction its parent's or, parentless,
           the one it was made with, which is the tree's. */
        switch (description.node_kind) {
        case SXML_NODE_KIND_DOCUMENT:
            return sxml_tree_make_base_uri(item_heap, node.tree, base_uri);
        case SXML_NODE_KIND_ELEMENT:
            return sxml_element_make_base_uri(item_heap, node, base_uri);
        case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
            if (!description.has_parent) {
                return sxml_tree_make_base_uri(item_heap, node.tree,
                                               base_uri);
            }
            break;
        case SXML_NODE_KIND_ATTRIBUTE:
        case SXML_NODE_KIND_TEXT:
        case SXML_NODE_KIND_COMMENT:
            break;
        case SXML_NODE_KIND_NAMESPACE:
        case SXML_NODE_KIND_ANY:
            *base_uri = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        if (!description.has_parent) {
            *base_uri = sxml_sequence_empty();
            return SXML_STATUS_OK;
        }
        node = sxml_node_reference_of_stored_node((sxml_stored_node_request){
            .tree = node.tree, .node_number = description.parent.index});
    }
}
