/*
 * Evaluator part: node constructors (src/evaluator_machine_internal.h,
 * D101). Evaluates the seven constructor kinds and SHALLOW_COPY of the IR
 * (src/ir_internal.h, "The node constructor kinds"). Each constructed node
 * is the root of a new arena tree (D40, D67), built through the tree
 * builder and destroyed with its last reference; its memory comes from the
 * item heap, so the node outlives the evaluation that made it.
 *
 * - DOCUMENT and ELEMENT (and SHALLOW_COPY of an element or document)
 *   open their node, then read the atoms of the content expression when it
 *   is a SEQUENCE (one atom per enclosed expression of a direct
 *   constructor, D110), or else become a receiving task and evaluate it as
 *   a delivering task (D166, src/evaluator_machine_internal.h,
 *   "Delivering"), so each value is added as it is made. Values become the
 *   new node's content sequence (glossary `content_sequence`; XQuery 4.0,
 *   "Content"): arrays are flattened (a stack of content frames in
 *   allocator memory, D71), each run of adjacent atomic items becomes text
 *   with one space between items, nodes are copied
 *   (sxml_tree_builder_copy_node), a document node by its children. The
 *   builder merges adjacent text, drops empty text, rejects duplicate
 *   attributes (XQDY0025) and does namespace fixup. A constructor in a
 *   delivering task builds its node into the receiver's tree when that
 *   gives what copying a tree of its own would (sxml_construction_open):
 *   nothing is copied, and a document's children are the content. Leaves
 *   are added to the receiver's tree likewise.
 * - ATTRIBUTE, TEXT, COMMENT, PROCESSING_INSTRUCTION and NAMESPACE read
 *   their value atom, atomize it and join the strings with spaces.
 * - Names: an xs:QName is used as it is; an xs:string, xs:untypedAtomic or
 *   xs:anyURI is a lexical QName or an EQName, resolved with the
 *   constructor's name namespaces, or the unit's static namespaces when it
 *   has none (prefix 0 there is the default element namespace, used for
 *   elements only); XQDY0074 when it is neither, XPTY0004 for another type
 *   or not one item.
 * - Modes come from the constructor's construction_flags (D110): 0 is
 *   construction strip and copy-namespaces preserve, inherit; the flags
 *   select no-preserve, no-inherit (for the copies of content nodes),
 *   construction preserve (xs:anyType for a constructed element, copies
 *   keep their annotations) and XSLT's content rules.
 * - SHALLOW_COPY is xsl:copy (XSLT 4.0, section 11.9.1): an element or a
 *   document gets the content with the rules of section 5.7.1 (later
 *   duplicate attributes replace earlier ones, XTDE0410, XTDE0420,
 *   XTDE0450); other nodes are copied whole, other items returned.
 *
 * Every step is leaf work over explicit stacks; nothing here runs the
 * machine (D71). Building charges one evaluation step per content item and
 * per copied node (D89).
 *
 * Golden file: src/evaluator.c (part steps as its expression steps, the
 * text buffer of "||"), src/function_library_json.c (an arena tree that
 * destroys itself, its memory from the heap).
 *
 * Specification: XQuery 4.0, "Node Constructors" (sections 4.12.1 to
 * 4.12.4); XSLT 4.0, "Constructing Complex Content" (section 5.7.1) and
 * "Shallow Copy" (section 11.9.1).
 */
#include "cast_internal.h"
#include "evaluator_machine_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Errors */

static const sxml_static_error sxml_construct_name_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the name of a computed constructor is not one xs:QName or xs:string"),
};

static const sxml_static_error sxml_construct_invalid_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0074"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the name of a computed constructor is not a valid QName with a bound prefix"),
};

static const sxml_static_error sxml_construct_reserved_element_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0096"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an element name uses the xmlns prefix or namespace, or misuses xml"),
};

static const sxml_static_error sxml_construct_reserved_attribute_name_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0044"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an attribute name is xmlns, uses the xmlns prefix or namespace, or misuses xml"),
};

static const sxml_static_error sxml_construct_invalid_target_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0041"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the target of a processing instruction is not an NCName"),
};

static const sxml_static_error sxml_construct_xml_target_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0064"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the target of a processing instruction is XML in some case"),
};

static const sxml_static_error sxml_construct_instruction_end_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0026"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the content of a processing instruction contains \"?>\""),
};

static const sxml_static_error sxml_construct_comment_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0072"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the content of a comment contains \"--\" or ends with \"-\""),
};

static const sxml_static_error sxml_construct_invalid_prefix_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0074"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the prefix of a computed namespace constructor is not an NCName"),
};

static const sxml_static_error sxml_construct_uri_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the namespace URI of a computed namespace constructor is not one string"),
};

static const sxml_static_error sxml_construct_late_attribute_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQTY0024"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an attribute or namespace node follows other content of an element"),
};

static const sxml_static_error sxml_construct_function_content_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQTY0105"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the content of a constructor contains a function item"),
};

static const sxml_static_error sxml_construct_document_attribute_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the content of a document constructor contains an attribute or namespace node"),
};

static const sxml_static_error sxml_construct_xslt_late_attribute_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0410"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an attribute or namespace node follows other content of an element"),
};

static const sxml_static_error sxml_construct_xslt_document_attribute_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0420"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an attribute or namespace node is added to a document node"),
};

static const sxml_static_error sxml_construct_xslt_function_content_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE0450"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the content of an instruction contains a function item"),
};

static const sxml_static_error sxml_construct_copy_cardinality_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE3180"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "xsl:copy selects more than one item"),
};

/* ------------------------------------------------------- Text buffer */

/* Text being joined in allocator memory. */
typedef struct sxml_construct_text_buffer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_construct_text_buffer;

static sxml_construct_text_buffer sxml_construct_text_buffer_start(
    sxml_allocator allocator) {
    return (sxml_construct_text_buffer){
        .allocator = allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
}

static void sxml_construct_text_buffer_release(sxml_construct_text_buffer *buffer) {
    if (buffer->bytes != NULL) {
        sxml_allocator_release(buffer->allocator, buffer->bytes);
    }
    buffer->bytes = NULL;
    buffer->byte_count = 0;
    buffer->byte_capacity = 0;
}

static sxml_byte_view sxml_construct_text_buffer_view(
    const sxml_construct_text_buffer *buffer) {
    return (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
}

static sxml_status sxml_construct_text_buffer_append(sxml_construct_text_buffer *buffer,
                                                     sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t required = 0;
    sxml_status status = sxml_size_add(buffer->byte_count, text.byte_count, &required);
    if (status == SXML_STATUS_OK) {
        void *elements = buffer->bytes;
        status = sxml_evaluator_grow(
            buffer->allocator,
            &(sxml_evaluator_growth){.elements = &elements,
                                     .element_capacity = &buffer->byte_capacity,
                                     .element_count = buffer->byte_count,
                                     .required_count = required,
                                     .element_byte_count = 1,
                                     .alignment = 1});
        buffer->bytes = elements;
    }
    if (status == SXML_STATUS_OK) {
        memcpy(buffer->bytes + buffer->byte_count, text.bytes, text.byte_count);
        buffer->byte_count = required;
    }
    return status;
}

/* Where the canonical text of an atomic item goes. */
typedef struct sxml_construct_text_target {
    sxml_dynamic_context *dynamic_context;
    sxml_construct_text_buffer *buffer;
} sxml_construct_text_target;

/* Appends the canonical text of an atomic item (casting to xs:string, F&O
   4.0, section 19.1.2). */
static sxml_status sxml_construct_append_atomic_text(sxml_construct_text_target target,
                                                     sxml_item item) {
    sxml_dynamic_context *context = target.dynamic_context;
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text text;
    sxml_status status = sxml_cast_write_canonical_text(
        context->item_heap,
        &(sxml_canonical_text_request){
            .item = item, .output = {.bytes = output, .byte_count = sizeof(output)}},
        &text);
    if (status != SXML_STATUS_CAPACITY_EXCEEDED) {
        return status == SXML_STATUS_OK
                   ? sxml_construct_text_buffer_append(target.buffer, text.text)
                   : status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = text.required_byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_cast_write_canonical_text(
        context->item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = memory, .byte_count = text.required_byte_count}},
        &text);
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_text_buffer_append(target.buffer, text.text);
    }
    sxml_allocator_release(context->allocator, memory);
    return status;
}

/* Whether a joined value had any item. */
typedef struct sxml_construct_join_result {
    bool has_items;
} sxml_construct_join_result;

/* A value whose atomized items are joined. */
typedef struct sxml_construct_join_request {
    sxml_sequence value;
    /* Receives the text, one space between items. */
    sxml_construct_text_buffer *buffer;
} sxml_construct_join_request;

/*
 * Atomizes request->value and joins the strings of its items with single
 * spaces (XQuery 4.0, sections 4.12.3.2 and 4.12.3.4 to 4.12.3.6). Writes
 * whether the value had any item to *result.
 */
static sxml_status sxml_construct_join_atomized(sxml_dynamic_context *context,
                                                const sxml_construct_join_request *request,
                                                sxml_construct_join_result *result) {
    sxml_sequence atomized = sxml_sequence_empty();
    sxml_status status = sxml_evaluator_atomize(
        context,
        &(sxml_atomization_request){.value = request->value,
                                    .error_record = context->error_record},
        &atomized);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    result->has_items = false;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(atomized);
    for (;;) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, context->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (result->has_items) {
            status = sxml_construct_text_buffer_append(request->buffer,
                                                       sxml_byte_view_from_c_string(" "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_construct_append_atomic_text(
                (sxml_construct_text_target){.dynamic_context = context,
                                             .buffer = request->buffer},
                step.item);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        result->has_items = true;
    }
    sxml_sequence_release(atomized);
    return status;
}

/* --------------------------------------------------------------- Text */

static sxml_byte_view sxml_construct_trim_leading(sxml_byte_view text) {
    while (text.byte_count > 0 && sxml_xml_byte_is_whitespace(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    return text;
}

/* True when text contains needle. */
static bool sxml_construct_text_contains(sxml_byte_view text, sxml_byte_view needle) {
    if (needle.byte_count == 0 || text.byte_count < needle.byte_count) {
        return needle.byte_count == 0;
    }
    for (size_t byte_offset = 0; byte_offset + needle.byte_count <= text.byte_count;
         byte_offset += 1) {
        if (memcmp(text.bytes + byte_offset, needle.bytes, needle.byte_count) == 0) {
            return true;
        }
    }
    return false;
}

static bool sxml_construct_text_equals(sxml_byte_view text, sxml_byte_view literal) {
    return sxml_byte_view_equals(text, literal);
}

/* "xml" in any combination of upper and lower case (XQuery 4.0, section
   4.12.3.5, XQDY0064). */
static bool sxml_construct_text_is_xml_in_any_case(sxml_byte_view text) {
    static const char lower_case[] = "xml";
    if (text.byte_count != 3) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < 3; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        const uint8_t lowered = (byte >= 'A' && byte <= 'Z') ? (uint8_t)(byte + 32U) : byte;
        if (lowered != (uint8_t)lower_case[byte_index]) {
            return false;
        }
    }
    return true;
}

/* Whether a text is an NCName. */
typedef struct sxml_construct_ncname_test {
    bool is_ncname;
} sxml_construct_ncname_test;

/* Writes whether text is an NCName (Namespaces in XML 1.0, section 4), as
   casting it to xs:NCName decides. Returns the heap's failures. */
static sxml_status sxml_construct_test_ncname(sxml_dynamic_context *context,
                                              sxml_byte_view text,
                                              sxml_construct_ncname_test *test) {
    test->is_ncname = false;
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_sequence result = sxml_sequence_empty();
    const sxml_status status = sxml_cast_make_value_from_text(
        context->item_heap,
        &(sxml_text_cast_request){
            .text = text,
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_NCNAME),
            .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
            .error_record = NULL},
        &result);
    sxml_sequence_release(result);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR ||
        status == SXML_STATUS_INVALID_ARGUMENT) {
        return SXML_STATUS_OK;
    }
    /* The whitespace facet of xs:NCName collapses, so " a" casts; an
       NCName has no whitespace at all. */
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (sxml_xml_byte_is_whitespace(text.bytes[byte_index])) {
            return status;
        }
    }
    test->is_ncname = status == SXML_STATUS_OK;
    return status;
}

/* ------------------------------------------------------------- Values */

/* An atom read and atomized: no item, or its one item. */
typedef struct sxml_construct_single_value {
    /* Owned. */
    sxml_sequence atomized;
    bool has_item;
    /* Borrowed from atomized. */
    sxml_item item;
} sxml_construct_single_value;

/* An atom to read as at most one atomic item. */
typedef struct sxml_construct_single_request {
    sxml_ir_atom atom;
    /* Raised for more than one item. */
    const sxml_static_error *cardinality_error;
} sxml_construct_single_request;

/*
 * Reads request->atom in the top task's frame and atomizes it into
 * *single_value; more than one item raises request->cardinality_error.
 * On success the caller releases single_value->atomized.
 */
static sxml_status sxml_construct_read_single(sxml_dynamic_context *context,
                                              const sxml_construct_single_request *request,
                                              sxml_construct_single_value *single_value) {
    *single_value = (sxml_construct_single_value){
        .atomized = sxml_sequence_empty(), .has_item = false, .item = {.representation = 0}};
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_evaluator_machine_read_atom(context, request->atom, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence atomized = sxml_sequence_empty();
    status = sxml_evaluator_atomize(
        context,
        &(sxml_atomization_request){.value = value, .error_record = context->error_record},
        &atomized);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t item_count = 0;
    status = sxml_sequence_count(atomized, context->error_record, &item_count);
    if (status == SXML_STATUS_OK && item_count > 1) {
        status = sxml_evaluator_machine_raise(context, request->cardinality_error);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            atomized,
            (sxml_item_request){.item_index = 0, .error_record = context->error_record}, &item);
    }
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(atomized);
        return status;
    }
    *single_value = (sxml_construct_single_value){
        .atomized = atomized, .has_item = item_count == 1, .item = item};
    return SXML_STATUS_OK;
}

/* The text of an xs:string, xs:untypedAtomic or xs:anyURI item (or a type
   derived from one), borrowed; false for other items. */
static bool sxml_construct_get_string_text(sxml_item item, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* ------------------------------------------------------------- Names */

/* Which rule expands an unprefixed lexical QName (XQuery 4.0, section
   4.12.3.1 and 4.12.3.2). */
typedef enum sxml_construct_name_rule {
    /* The constructed element namespace rule: the default element
       namespace. */
    SXML_CONSTRUCT_NAME_RULE_ELEMENT,
    /* The no-namespace rule. */
    SXML_CONSTRUCT_NAME_RULE_ATTRIBUTE,
} sxml_construct_name_rule;

/* A name as a node takes it: expanded name and prefix. */
typedef struct sxml_construct_name {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} sxml_construct_name;

/* A name to make from text. */
typedef struct sxml_construct_name_text {
    sxml_byte_view text;
    sxml_construct_name_rule rule;
} sxml_construct_name_text;

static sxml_status sxml_construct_intern_name(sxml_dynamic_context *context,
                                              const sxml_name_parts *parts,
                                              sxml_name_identifier *expanded_name) {
    return sxml_name_pool_intern_name(
        sxml_item_heap_get_name_pool(context->item_heap),
        &(sxml_name_request){.namespace_uri = parts->namespace_uri,
                             .local_name = parts->local_name},
        expanded_name);
}

/* "prefix:local" or "local" whose parts are NCNames, without resolving the
   prefix: writes the parts or leaves *is_valid false. */
typedef struct sxml_construct_lexical_parts {
    bool is_valid;
    sxml_byte_view namespace_prefix;
    sxml_byte_view local_name;
} sxml_construct_lexical_parts;

static sxml_status sxml_construct_split_lexical(sxml_dynamic_context *context,
                                                sxml_byte_view text,
                                                sxml_construct_lexical_parts *parts) {
    *parts = (sxml_construct_lexical_parts){
        .is_valid = false,
        .namespace_prefix = {.bytes = text.bytes, .byte_count = 0},
        .local_name = text};
    const uint8_t *colon =
        text.byte_count == 0 ? NULL : memchr(text.bytes, ':', text.byte_count);
    if (colon != NULL) {
        parts->namespace_prefix.byte_count = (size_t)(colon - text.bytes);
        parts->local_name = (sxml_byte_view){
            .bytes = colon + 1,
            .byte_count = text.byte_count - parts->namespace_prefix.byte_count - 1};
    }
    sxml_construct_ncname_test test = {.is_ncname = false};
    sxml_status status = sxml_construct_test_ncname(context, parts->local_name, &test);
    if (status == SXML_STATUS_OK && test.is_ncname && colon != NULL) {
        status = sxml_construct_test_ncname(context, parts->namespace_prefix, &test);
    }
    parts->is_valid = status == SXML_STATUS_OK && test.is_ncname;
    return status;
}

/* Q{uri}local or Q{uri}prefix:local (XQuery 4.0, sections 4.12.3.1 and
   4.12.3.2). */
static sxml_status sxml_construct_name_from_eqname(sxml_dynamic_context *context,
                                                   sxml_byte_view text,
                                                   sxml_construct_name *name) {
    const sxml_byte_view after_open = {.bytes = text.bytes + 2,
                                       .byte_count = text.byte_count - 2};
    const uint8_t *close =
        after_open.byte_count == 0 ? NULL : memchr(after_open.bytes, '}', after_open.byte_count);
    if (close == NULL) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_invalid_name_error);
    }
    const sxml_byte_view namespace_uri = {
        .bytes = after_open.bytes, .byte_count = (size_t)(close - after_open.bytes)};
    if (memchr(namespace_uri.bytes, '{', namespace_uri.byte_count) != NULL) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_invalid_name_error);
    }
    const sxml_byte_view rest = {.bytes = close + 1,
                                 .byte_count = after_open.byte_count - namespace_uri.byte_count - 1};
    sxml_construct_lexical_parts parts;
    sxml_status status = sxml_construct_split_lexical(context, rest, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!parts.is_valid) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_invalid_name_error);
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->item_heap);
    sxml_name_parts name_parts = {.namespace_uri = {.index = 0},
                                  .local_name = parts.local_name};
    status = sxml_name_pool_intern_namespace_uri(
        name_pool, sxml_xml_text_trim_whitespace(namespace_uri), &name_parts.namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, parts.namespace_prefix,
                                                        &name->namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_intern_name(context, &name_parts, &name->expanded_name);
    }
    return status;
}

/* The namespaces a lexical name of the constructor on top of the task
   stack is resolved with: its name namespaces (the static namespaces
   extended by enclosing direct constructors, D110), or the unit's static
   namespaces when it has none. */
static sxml_namespace_binding_view sxml_construct_name_namespaces(sxml_dynamic_context *context) {
    const sxml_evaluator_task *top = sxml_evaluator_machine_top(context);
    const sxml_ir_unit *unit = top->function->unit;
    const sxml_ir_namespace_binding_list list =
        sxml_ir_unit_get_expression(unit, top->expression)->node_construction.name_namespaces;
    if (list.namespace_binding_count == 0) {
        return sxml_ir_unit_get_static_namespaces(unit);
    }
    return sxml_ir_unit_get_namespace_bindings(unit, list);
}

/* A lexical QName resolved with the constructor's name namespaces, or an
   unprefixed attribute name in no namespace. */
static sxml_status sxml_construct_name_from_lexical(sxml_dynamic_context *context,
                                                    const sxml_construct_name_text *name_text,
                                                    sxml_construct_name *name) {
    sxml_construct_lexical_parts parts;
    sxml_status status = sxml_construct_split_lexical(context, name_text->text, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!parts.is_valid) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_invalid_name_error);
    }
    if (name_text->rule == SXML_CONSTRUCT_NAME_RULE_ATTRIBUTE &&
        parts.namespace_prefix.byte_count == 0) {
        name->namespace_prefix = (sxml_namespace_prefix_identifier){.index = 0};
        return sxml_construct_intern_name(
            context,
            &(sxml_name_parts){.namespace_uri = sxml_well_known_namespace_identifier(
                                   SXML_NAMESPACE_NONE),
                               .local_name = parts.local_name},
            &name->expanded_name);
    }
    /* Casting to xs:QName resolves the prefix, and an unprefixed name with
       the default element namespace (F&O 4.0, section 19.1.9). */
    sxml_sequence qname = sxml_sequence_empty();
    status = sxml_cast_make_value_from_text(
        context->item_heap,
        &(sxml_text_cast_request){
            .text = name_text->text,
            .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .namespace_bindings = sxml_construct_name_namespaces(context),
            .error_record = NULL},
        &qname);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_invalid_name_error);
    }
    sxml_item qname_item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            qname, (sxml_item_request){.item_index = 0, .error_record = NULL}, &qname_item);
    }
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(qname_item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *name = (sxml_construct_name){
            .expanded_name = datum.datum_details.qname.expanded_name,
            .namespace_prefix = datum.datum_details.qname.namespace_prefix};
    }
    sxml_sequence_release(qname);
    return status;
}

/* The name of an element or attribute constructor, from its name atom. */
typedef struct sxml_construct_name_request {
    sxml_ir_atom atom;
    sxml_construct_name_rule rule;
} sxml_construct_name_request;

static sxml_status sxml_construct_read_name(sxml_dynamic_context *context,
                                            const sxml_construct_name_request *request,
                                            sxml_construct_name *name) {
    sxml_construct_single_value single;
    sxml_status status = sxml_construct_read_single(
        context,
        &(sxml_construct_single_request){.atom = request->atom,
                                         .cardinality_error = &sxml_construct_name_type_error},
        &single);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (single.has_item && sxml_atomic_item_get_datum(single.item, &datum) == SXML_STATUS_OK &&
        datum.datum_kind == SXML_DATUM_KIND_QNAME &&
               !sxml_type_identifier_equals(sxml_item_get_type_annotation(single.item),
                                            sxml_builtin_type_identifier(SXML_TYPE_XS_NOTATION))) {
        *name = (sxml_construct_name){.expanded_name = datum.datum_details.qname.expanded_name,
                                      .namespace_prefix =
                                          datum.datum_details.qname.namespace_prefix};
    } else if (single.has_item && sxml_construct_get_string_text(single.item, &text)) {
        text = sxml_xml_text_trim_whitespace(text);
        if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
            status = sxml_construct_name_from_eqname(context, text, name);
        } else {
            status = sxml_construct_name_from_lexical(
                context, &(sxml_construct_name_text){.text = text, .rule = request->rule}, name);
        }
    } else {
        status = sxml_evaluator_machine_raise(context, &sxml_construct_name_type_error);
    }
    sxml_sequence_release(single.atomized);
    return status;
}

/* What a name's prefix and namespace say about xml and xmlns. */
typedef struct sxml_construct_name_facts {
    bool prefix_is_xml;
    bool prefix_is_xmlns;
    bool is_in_xml_namespace;
    bool is_in_xmlns_namespace;
    bool is_in_no_namespace;
    sxml_byte_view local_name;
} sxml_construct_name_facts;

static sxml_status sxml_construct_read_name_facts(sxml_dynamic_context *context,
                                                  const sxml_construct_name *name,
                                                  sxml_construct_name_facts *facts) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->item_heap);
    sxml_name_parts parts;
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_name_pool_get_name_parts(name_pool, name->expanded_name, &parts);
    if (status == SXML_STATUS_OK && name->namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(name_pool, name->namespace_prefix, &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *facts = (sxml_construct_name_facts){
        .prefix_is_xml = sxml_construct_text_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xml")),
        .prefix_is_xmlns = sxml_construct_text_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xmlns")),
        .is_in_xml_namespace = parts.namespace_uri.index == SXML_NAMESPACE_XML,
        .is_in_xmlns_namespace = parts.namespace_uri.index == SXML_NAMESPACE_XMLNS,
        .is_in_no_namespace = parts.namespace_uri.index == SXML_NAMESPACE_NONE,
        .local_name = parts.local_name};
    return SXML_STATUS_OK;
}

/* XQDY0096 (XQuery 4.0, section 4.12.3.1). */
static sxml_status sxml_construct_check_element_name(sxml_dynamic_context *context,
                                                     const sxml_construct_name *name) {
    sxml_construct_name_facts facts;
    const sxml_status status = sxml_construct_read_name_facts(context, name, &facts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (facts.prefix_is_xmlns || facts.is_in_xmlns_namespace ||
        facts.prefix_is_xml != facts.is_in_xml_namespace) {
        return sxml_evaluator_machine_raise(context, &sxml_construct_reserved_element_name_error);
    }
    return SXML_STATUS_OK;
}

/* XQDY0044 (XQuery 4.0, section 4.12.3.2). An attribute in a namespace
   needs a prefix: the xml prefix for the XML namespace, otherwise an
   implementation-dependent one, here "ns0" (fixup renames it when an
   element binds it otherwise). */
static sxml_status sxml_construct_check_attribute_name(sxml_dynamic_context *context,
                                                       sxml_construct_name *name) {
    sxml_construct_name_facts facts;
    sxml_status status = sxml_construct_read_name_facts(context, name, &facts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (name->namespace_prefix.index == 0 && !facts.is_in_no_namespace &&
        !facts.is_in_xmlns_namespace) {
        status = sxml_name_pool_intern_namespace_prefix(
            sxml_item_heap_get_name_pool(context->item_heap),
            sxml_byte_view_from_c_string(facts.is_in_xml_namespace ? "xml" : "ns0"),
            &name->namespace_prefix);
        facts.prefix_is_xml = facts.is_in_xml_namespace;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_unprefixed_xmlns = name->namespace_prefix.index == 0 &&
                                     sxml_construct_text_equals(facts.local_name,
                                                                SXML_BYTE_VIEW_FROM_LITERAL("xmlns"));
    if (facts.prefix_is_xmlns || is_unprefixed_xmlns || facts.is_in_xmlns_namespace ||
        facts.prefix_is_xml != facts.is_in_xml_namespace) {
        return sxml_evaluator_machine_raise(context,
                                            &sxml_construct_reserved_attribute_name_error);
    }
    return SXML_STATUS_OK;
}

/* An NCName given as an xs:string, xs:untypedAtomic or xs:anyURI: the
   target of a processing instruction, the prefix of a namespace node. */
typedef struct sxml_construct_ncname_request {
    sxml_ir_atom atom;
    /* Raised for text that is not an NCName. */
    const sxml_static_error *invalid_error;
    /* The empty sequence and "" give the empty text (a namespace prefix);
       otherwise they are invalid. */
    bool allows_empty;
} sxml_construct_ncname_request;

/* Reads the NCName into buffer (the item it is borrowed from is released
   before returning). */
static sxml_status sxml_construct_read_ncname(sxml_dynamic_context *context,
                                              const sxml_construct_ncname_request *request,
                                              sxml_construct_text_buffer *buffer) {
    sxml_construct_single_value single;
    sxml_status status = sxml_construct_read_single(
        context,
        &(sxml_construct_single_request){.atom = request->atom,
                                         .cardinality_error = &sxml_construct_name_type_error},
        &single);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (single.has_item ? !sxml_construct_get_string_text(single.item, &text)
                        : !request->allows_empty) {
        status = sxml_evaluator_machine_raise(context, &sxml_construct_name_type_error);
    }
    text = sxml_xml_text_trim_whitespace(text);
    if (status == SXML_STATUS_OK && (text.byte_count > 0 || !request->allows_empty)) {
        sxml_construct_ncname_test test = {.is_ncname = false};
        status = sxml_construct_test_ncname(context, text, &test);
        if (status == SXML_STATUS_OK && !test.is_ncname) {
            status = sxml_evaluator_machine_raise(context, request->invalid_error);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_text_buffer_append(buffer, text);
    }
    sxml_sequence_release(single.atomized);
    return status;
}

/* ------------------------------------------------------------ Builds */

/* Which rules the content of a node follows. */
typedef enum sxml_construct_rules {
    /* XQuery 4.0, "Content": duplicate attributes raise XQDY0025. */
    SXML_CONSTRUCT_RULES_XQUERY,
    /* XSLT 4.0, section 5.7.1: a later duplicate attribute replaces an
       earlier one; XTDE codes. */
    SXML_CONSTRUCT_RULES_XSLT,
} sxml_construct_rules;

/* One array whose members are being added (glossary `content_frame`). */
typedef struct sxml_construct_content_frame {
    /* Borrowed from the content value or an enclosing array. */
    sxml_item array;
    size_t member_count;
    size_t next_member_index;
    /* Borrowed from array: the member being read and where in it. */
    sxml_sequence member;
    sxml_sequence_iterator member_iterator;
} sxml_construct_content_frame;

/*
 * A tree being built and the state of the content being added to it. A
 * constructor in a delivering task builds into the construction of its
 * receiving task instead (D166, src/evaluator_machine_internal.h,
 * "Delivering"): its node becomes the next content of the enclosing
 * construction's node, as the copy of a tree of its own would, so it
 * shares the enclosing builder and keeps a content state of its own. The
 * state of a receiving task is always an sxml_construction (only this
 * part receives).
 */
typedef struct sxml_construction {
    sxml_dynamic_context *dynamic_context;
    /* Owned; NULL for a construction built into enclosing. */
    sxml_arena_tree *arena_tree;
    sxml_tree_builder builder;
    /* The construction of the receiving task this one builds into; NULL
       for one with a tree of its own. Borrowed: the receiving task stays
       below this one's task. */
    struct sxml_construction *enclosing;
    sxml_construct_rules rules;
    /* The constructor's construction_flags (sxml_ir_construction_flag). */
    uint32_t construction_flags;
    /* True for the content of a document node. */
    bool is_document;
    /* True once a child node or non-empty text was sent: no attribute or
       namespace node may follow (XQTY0024). */
    bool has_children;
    /* True when the last content item was atomic, so the next atomic item
       is preceded by a space. */
    bool follows_atomic_item;
    sxml_construct_content_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_construction;

/* The construction_flags of the constructor on top of the task stack
   (0 for the kinds that have none). */
static uint32_t sxml_construct_top_flags(sxml_dynamic_context *context) {
    const sxml_evaluator_task *top = sxml_evaluator_machine_top(context);
    return sxml_ir_unit_get_expression(top->function->unit, top->expression)
        ->node_construction.construction_flags;
}

/* How a construction starts. */
typedef struct sxml_construction_request {
    sxml_construct_rules rules;
    /* The base URI of a root that is not a document node: the static base
       URI for elements, empty for the other kinds (XQuery 4.0, sections
       4.12.1.3 and 4.12.3.5; XDM 4.0, section 7: a parentless attribute,
       text or comment has no base URI). */
    sxml_byte_view base_uri;
} sxml_construction_request;

/* The rules of the constructor on top of the task stack: its XSLT_RULES
   flag selects XSLT's, else those of the request. */
static sxml_construct_rules sxml_construct_top_rules(sxml_dynamic_context *context,
                                                     sxml_construct_rules rules) {
    return (sxml_construct_top_flags(context) & (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES) != 0
               ? SXML_CONSTRUCT_RULES_XSLT
               : rules;
}

/* A construction with no tree and no content yet. */
static sxml_construction sxml_construction_empty(sxml_dynamic_context *context,
                                                 sxml_construct_rules rules) {
    return (sxml_construction){.dynamic_context = context,
                               .arena_tree = NULL,
                               .builder = {.state = NULL, .operations = NULL},
                               .enclosing = NULL,
                               .rules = sxml_construct_top_rules(context, rules),
                               .construction_flags = sxml_construct_top_flags(context),
                               .is_document = false,
                               .has_children = false,
                               .follows_atomic_item = false,
                               .frames = NULL,
                               .frame_count = 0,
                               .frame_capacity = 0};
}

/* Starts a construction with a tree of its own for the constructor on top
   of the task stack. */
static sxml_status sxml_construction_start(sxml_dynamic_context *context,
                                           const sxml_construction_request *request,
                                           sxml_construction *construction) {
    *construction = sxml_construction_empty(context, request->rules);
    /* The tree outlives the evaluation, so its memory is the heap's. The
       error record is used only while events are sent, in this step. */
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(context->item_heap);
    const sxml_status status = sxml_arena_tree_create(
        tree_allocator,
        &(sxml_arena_tree_configuration){
            .allocator = tree_allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = context->item_heap,
            .maximum_node_count = 0,
            .maximum_depth = 0,
            .maximum_text_byte_count = 0,
            .keeps_locations = false,
            .duplicate_attribute_policy = construction->rules == SXML_CONSTRUCT_RULES_XSLT
                                              ? SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE
                                              : SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = context->error_record,
            .base_uri = request->base_uri},
        &construction->arena_tree);
    if (status == SXML_STATUS_OK) {
        construction->builder = sxml_arena_tree_as_tree_builder(construction->arena_tree);
    }
    return status;
}

/* The construction the top task delivers into (D166), or NULL. */
static sxml_construction *sxml_construct_find_receiver(sxml_dynamic_context *context) {
    return sxml_evaluator_machine_find_receiver(context);
}

/*
 * Starts a document or element construction for the constructor on top
 * of the task stack in the construction it delivers into (D166), when the
 * node made there is the copy that a tree of its own would give: the same
 * rules and construction flags, so its content is built alike, and copies
 * that keep their namespaces (copy-namespaces preserve), so nothing would
 * be dropped; inherit or no-inherit is the element start's
 * ignores_parent_namespaces, its type annotation is the copy's under
 * either construction mode, and the copy has no base URI of its own. Its
 * own tree otherwise.
 */
static sxml_status sxml_construction_open(sxml_dynamic_context *context,
                                          const sxml_construction_request *request,
                                          sxml_construction *construction) {
    sxml_construction *enclosing = sxml_construct_find_receiver(context);
    const sxml_construction empty = sxml_construction_empty(context, request->rules);
    if (enclosing == NULL || enclosing->rules != empty.rules ||
        enclosing->construction_flags != empty.construction_flags ||
        (empty.construction_flags & (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES) != 0) {
        return sxml_construction_start(context, request, construction);
    }
    *construction = empty;
    construction->builder = enclosing->builder;
    construction->enclosing = enclosing;
    /* A node of the content: the next atomic item starts a new text. */
    enclosing->follows_atomic_item = false;
    return SXML_STATUS_OK;
}

/* Releases what an unfinished construction holds. */
static void sxml_construction_abandon(sxml_construction *construction) {
    sxml_arena_tree_destroy(construction->arena_tree);
    construction->arena_tree = NULL;
    if (construction->frames != NULL) {
        sxml_allocator_release(construction->dynamic_context->allocator, construction->frames);
        construction->frames = NULL;
    }
}

/* Ends a construction with a tree of its own: its root as an owned node
   item in *result. On failure everything is released. */
static sxml_status sxml_construction_finish(sxml_construction *construction,
                                            sxml_sequence *result) {
    sxml_tree *tree = NULL;
    const sxml_status status = sxml_arena_tree_finish(construction->arena_tree, &tree);
    if (status != SXML_STATUS_OK) {
        sxml_construction_abandon(construction);
        return status;
    }
    construction->arena_tree = NULL;
    sxml_construction_abandon(construction);
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* Raises the error of rules: XQuery's or XSLT's. */
typedef struct sxml_construct_rule_errors {
    const sxml_static_error *xquery_error;
    const sxml_static_error *xslt_error;
} sxml_construct_rule_errors;

static sxml_status sxml_construction_raise(const sxml_construction *construction,
                                           sxml_construct_rule_errors errors) {
    return sxml_evaluator_machine_raise(construction->dynamic_context,
                                        construction->rules == SXML_CONSTRUCT_RULES_XSLT
                                            ? errors.xslt_error
                                            : errors.xquery_error);
}

static sxml_status sxml_construction_send_text(sxml_construction *construction,
                                               sxml_byte_view text) {
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    construction->has_children = true;
    return construction->builder.operations->add_text(
        construction->builder.state,
        &(sxml_text_event){.text = text,
                           .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}});
}

/* An atomic item of the content: its text, after a space when it follows
   another atomic item (XQuery 4.0, section 4.12.1.3, step 1e). */
static sxml_status sxml_construction_add_atomic(sxml_construction *construction,
                                                sxml_item item) {
    sxml_construct_text_buffer buffer =
        sxml_construct_text_buffer_start(construction->dynamic_context->allocator);
    sxml_status status = SXML_STATUS_OK;
    if (construction->follows_atomic_item) {
        status = sxml_construct_text_buffer_append(&buffer, sxml_byte_view_from_c_string(" "));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_append_atomic_text(
            (sxml_construct_text_target){.dynamic_context = construction->dynamic_context,
                                         .buffer = &buffer},
            item);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_send_text(construction,
                                             sxml_construct_text_buffer_view(&buffer));
    }
    sxml_construct_text_buffer_release(&buffer);
    construction->follows_atomic_item = true;
    return status;
}

/* Copies node (and its subtree) into the tree being built with the
   construction's copy-namespaces and construction modes (XQuery 4.0,
   4.12.1.3, step 1d: copied as by the copy-namespaces mode; untyped
   unless construction is preserve). */
static sxml_status sxml_construction_copy(sxml_construction *construction,
                                          sxml_node_reference node,
                                          const sxml_node_description *description) {
    uint64_t node_count = 1;
    if (node.namespace_number == 0) {
        node_count += (uint64_t)description->last_descendant.index - node.node_index.index;
    }
    sxml_status status =
        sxml_dynamic_context_charge_steps(construction->dynamic_context, node_count);
    if (status == SXML_STATUS_OK) {
        const uint32_t flags = construction->construction_flags;
        status = sxml_tree_builder_copy_node(
            construction->builder,
            &(sxml_copy_request){
                .node = node,
                .preserves_namespaces =
                    (flags & (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES) == 0,
                .inherits_namespaces =
                    (flags & (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES) == 0,
                .preserves_type_annotations =
                    (flags & (uint32_t)SXML_IR_CONSTRUCTION_PRESERVES_TYPES) != 0});
    }
    return status;
}

/* The children of a document node in the content (XQuery 4.0, section
   4.12.1.3, step 2: a document node is replaced by its children). */
static sxml_status sxml_construction_add_document_children(
    sxml_construction *construction, sxml_node_reference document,
    const sxml_node_description *description) {
    if (!description->has_children) {
        return SXML_STATUS_OK;
    }
    sxml_node_reference child = {.tree = document.tree,
                                 .node_index = description->first_child,
                                 .namespace_number = 0};
    for (;;) {
        sxml_node_description child_description;
        sxml_status status = sxml_node_describe(child, &child_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_construction_copy(construction, child, &child_description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        construction->has_children = true;
        if (!child_description.has_next_sibling) {
            return SXML_STATUS_OK;
        }
        child.node_index = child_description.next_sibling;
    }
}

/* A node of the content. */
static sxml_status sxml_construction_add_node(sxml_construction *construction,
                                              sxml_item item) {
    construction->follows_atomic_item = false;
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(item, &node);
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (description.node_kind) {
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_NAMESPACE:
        if (construction->is_document) {
            return sxml_construction_raise(
                construction,
                (sxml_construct_rule_errors){
                    .xquery_error = &sxml_construct_document_attribute_error,
                    .xslt_error = &sxml_construct_xslt_document_attribute_error});
        }
        if (construction->has_children) {
            return sxml_construction_raise(
                construction,
                (sxml_construct_rule_errors){
                    .xquery_error = &sxml_construct_late_attribute_error,
                    .xslt_error = &sxml_construct_xslt_late_attribute_error});
        }
        return sxml_construction_copy(construction, node, &description);
    case SXML_NODE_KIND_DOCUMENT:
        return sxml_construction_add_document_children(construction, node, &description);
    case SXML_NODE_KIND_TEXT: {
        /* Empty text makes no node, and no content (step 3). */
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(node, &text);
        if (status != SXML_STATUS_OK || text.byte_count == 0) {
            return status;
        }
        construction->has_children = true;
        return sxml_construction_copy(construction, node, &description);
    }
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        construction->has_children = true;
        return sxml_construction_copy(construction, node, &description);
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Pushes a content frame for array, whose members are added next
   (array:flatten, XQuery 4.0, section 4.12.1.3, step 1e). */
static sxml_status sxml_construction_push_array(sxml_construction *construction,
                                                sxml_item array) {
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(array, &member_count);
    if (status != SXML_STATUS_OK || member_count == 0) {
        return status;
    }
    void *elements = construction->frames;
    status = sxml_evaluator_grow(
        construction->dynamic_context->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &construction->frame_capacity,
                                 .element_count = construction->frame_count,
                                 .required_count = construction->frame_count + 1,
                                 .element_byte_count = sizeof(sxml_construct_content_frame),
                                 .alignment = alignof(sxml_construct_content_frame)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    construction->frames = elements;
    construction->frames[construction->frame_count] = (sxml_construct_content_frame){
        .array = array,
        .member_count = member_count,
        .next_member_index = 0,
        .member = sxml_sequence_empty(),
        .member_iterator = sxml_sequence_iterator_start(sxml_sequence_empty())};
    construction->frame_count += 1;
    return SXML_STATUS_OK;
}

/* One item of the content value or of a flattened array. */
static sxml_status sxml_construction_add_item(sxml_construction *construction,
                                              sxml_item item) {
    sxml_status status = sxml_dynamic_context_charge_steps(construction->dynamic_context, 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_NODE:
        return sxml_construction_add_node(construction, item);
    case SXML_ITEM_KIND_ARRAY:
        return sxml_construction_push_array(construction, item);
    case SXML_ITEM_KIND_JSON_NODE: {
        /* A JNode adds its atomized value (XPath 4.0, section 2.5.3). */
        sxml_sequence atomized = sxml_sequence_empty();
        status = sxml_evaluator_atomize(
            construction->dynamic_context,
            &(sxml_atomization_request){
                .value = sxml_sequence_from_item(item),
                .error_record = construction->dynamic_context->error_record},
            &atomized);
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(atomized);
        while (status == SXML_STATUS_OK) {
            sxml_iteration_step step;
            status = sxml_sequence_iterator_next(
                &iterator, construction->dynamic_context->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            status = sxml_construction_add_atomic(construction, step.item);
        }
        sxml_sequence_release(atomized);
        return status;
    }
    case SXML_ITEM_KIND_ATOMIC:
        return sxml_construction_add_atomic(construction, item);
    case SXML_ITEM_KIND_FUNCTION:
    case SXML_ITEM_KIND_MAP:
        break;
    }
    return sxml_construction_raise(
        construction, (sxml_construct_rule_errors){
                          .xquery_error = &sxml_construct_function_content_error,
                          .xslt_error = &sxml_construct_xslt_function_content_error});
}

/* The next item of the innermost content frame; pops finished frames.
   Writes false to step->has_item when the frames are exhausted. */
static sxml_status sxml_construction_next_frame_item(sxml_construction *construction,
                                                     sxml_iteration_step *step) {
    step->has_item = false;
    while (construction->frame_count > 0) {
        sxml_construct_content_frame *frame =
            &construction->frames[construction->frame_count - 1];
        sxml_status status = sxml_sequence_iterator_next(
            &frame->member_iterator, construction->dynamic_context->error_record, step);
        if (status != SXML_STATUS_OK || step->has_item) {
            return status;
        }
        if (frame->next_member_index == frame->member_count) {
            construction->frame_count -= 1;
            continue;
        }
        status = sxml_array_get_member(frame->array, frame->next_member_index, &frame->member);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->next_member_index += 1;
        frame->member_iterator = sxml_sequence_iterator_start(frame->member);
    }
    return SXML_STATUS_OK;
}

/* Adds the items of value, arrays flattened, as content. */
static sxml_status sxml_construction_add_items(sxml_construction *construction,
                                               sxml_sequence value) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    for (;;) {
        sxml_iteration_step step;
        sxml_status status = sxml_construction_next_frame_item(construction, &step);
        if (status == SXML_STATUS_OK && !step.has_item) {
            status = sxml_sequence_iterator_next(
                &iterator, construction->dynamic_context->error_record, &step);
        }
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        status = sxml_construction_add_item(construction, step.item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Adds the items of the value of one enclosed expression as content. */
static sxml_status sxml_construction_add_enclosed(sxml_construction *construction,
                                                  sxml_sequence value) {
    /* Only atomic items of one enclosed expression are joined by spaces
       (XQuery 4.0, section 4.12.1.3: <c>{1}{2}</c> is "12"). */
    construction->follows_atomic_item = false;
    return sxml_construction_add_items(construction, value);
}

/* ------------------------------------------------------ Kind helpers */

static const sxml_ir_expression *sxml_construct_expression(const sxml_evaluator_task *task) {
    return sxml_ir_unit_get_expression(task->function->unit, task->expression);
}

/* True for a content expression whose atoms are the enclosed
   expressions (see the convention in sxml_construction_add_atom_list). */
static bool sxml_construct_content_is_enclosed_list(const sxml_evaluator_task *task,
                                                    sxml_ir_expression_index content) {
    return content.value != 0 &&
           sxml_ir_unit_get_expression(task->function->unit, content)->expression_kind ==
               SXML_IR_EXPRESSION_KIND_SEQUENCE;
}

/*
 * Adds the atoms of an enclosed-list content expression of the
 * constructor on top of the task stack. The IR convention (D110): a
 * content expression of kind SEQUENCE lists the enclosed expressions and
 * literal text of a direct constructor, one atom each, which are not
 * evaluated as one value: atomic items are joined by spaces only within
 * one atom. Any other content expression is one enclosed expression (a
 * computed constructor, xsl:copy, a literal result element whose block
 * has LETs), whose value is delivered (D166).
 */
static sxml_status sxml_construction_add_atom_list(sxml_construction *construction,
                                                   sxml_ir_expression_index content) {
    sxml_dynamic_context *context = construction->dynamic_context;
    const sxml_evaluator_task *task = sxml_evaluator_machine_top(context);
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(
        task->function->unit, sxml_ir_unit_get_expression(task->function->unit, content)->sequence);
    for (size_t atom_index = 0; atom_index < atoms.atom_count; atom_index += 1) {
        sxml_sequence value = sxml_sequence_empty();
        sxml_status status =
            sxml_evaluator_machine_read_atom(context, atoms.atoms[atom_index], &value);
        if (status == SXML_STATUS_OK) {
            status = sxml_construction_add_enclosed(construction, value);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_construction_send_bindings(sxml_construction *construction,
                                                   sxml_namespace_binding_view bindings) {
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_status status = construction->builder.operations->add_namespace_binding(
            construction->builder.state, &bindings.namespace_bindings[binding_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static const sxml_source_location sxml_construct_no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

/* Ends a part task: the constructed value goes down; content is
   released. */
static sxml_status sxml_construct_end(sxml_dynamic_context *context,
                                      sxml_status status, sxml_sequence result) {
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(result);
        return status;
    }
    return sxml_evaluator_machine_finish(context, result);
}

/* ------------------------------------------------- Receiving content */

/* The release function of a construction that is a part task's state. */
static void sxml_construction_release_state(sxml_dynamic_context *dynamic_context, void *state) {
    sxml_construction_abandon(state);
    sxml_allocator_release(dynamic_context->allocator, state);
}

/* The receive function of a receiving task (D166): the delivered value
   is more of one enclosed expression, so atomic items stay joined across
   deliveries (`element a {for $i in 1 to 3 return $i}` is "1 2 3"). */
static sxml_status sxml_construction_receive(sxml_dynamic_context *dynamic_context, void *state,
                                             sxml_sequence value) {
    (void)dynamic_context;
    const sxml_status status = sxml_construction_add_items(state, value);
    sxml_sequence_release(value);
    return status;
}

/* A construction in allocator memory as the state of the part task on
   top, released with it. */
static sxml_status sxml_construct_allocate(sxml_dynamic_context *context,
                                           sxml_construction **construction) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        context->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_construction),
                                  .alignment = alignof(sxml_construction)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *construction = memory;
    **construction = sxml_construction_empty(context, SXML_CONSTRUCT_RULES_XQUERY);
    sxml_evaluator_task *task = sxml_evaluator_machine_top(context);
    task->details.part.state = memory;
    task->details.part.release_state = sxml_construction_release_state;
    return SXML_STATUS_OK;
}

/* Whether the content was pushed: the step returns and goes on in phase
   1 when its value has been delivered. */
typedef struct sxml_construct_content_start {
    bool has_pushed;
} sxml_construct_content_start;

/*
 * Adds the content of the constructor on top of the task stack to its
 * construction, the task's state: none for content 0, the atoms of an
 * enclosed list at once, otherwise by making the task a receiving task
 * and pushing the content expression as a delivering task (D166), so
 * every value is added as it is made.
 */
static sxml_status sxml_construct_start_content(sxml_construction *construction,
                                                sxml_ir_expression_index content,
                                                sxml_construct_content_start *start) {
    sxml_dynamic_context *context = construction->dynamic_context;
    sxml_evaluator_task *task = sxml_evaluator_machine_top(context);
    start->has_pushed = false;
    if (content.value == 0) {
        return SXML_STATUS_OK;
    }
    if (sxml_construct_content_is_enclosed_list(task, content)) {
        return sxml_construction_add_atom_list(construction, content);
    }
    task->phase = 1;
    task->details.part.receive = sxml_construction_receive;
    start->has_pushed = true;
    return sxml_evaluator_machine_push_content(context, content);
}

/* The end of a document or element construction: its end event, then its
   root as an owned node item in *result, or, built into the enclosing
   construction, the empty sequence. Releases what the tree holds. */
static sxml_status sxml_construction_end(sxml_construction *construction,
                                         sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_evaluator_machine_top(construction->dynamic_context)->details.part.receive = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (!construction->is_document) {
        status = construction->builder.operations->end_element(construction->builder.state);
    } else if (construction->enclosing == NULL) {
        status = construction->builder.operations->end_document(construction->builder.state);
    } else if (construction->has_children) {
        /* The children of a document node are the content (XQuery 4.0,
           section 4.12.1.3, step 2). */
        construction->enclosing->has_children = true;
    }
    if (status != SXML_STATUS_OK || construction->enclosing != NULL) {
        sxml_construction_abandon(construction);
        return status;
    }
    return sxml_construction_finish(construction, result);
}

/* Phase 1 of a document or element constructor: its content was
   delivered; ends the node and the task. */
static sxml_status sxml_construct_finish_content(sxml_dynamic_context *context) {
    sxml_sequence_release(sxml_evaluator_machine_take_value(context));
    sxml_construction *construction = sxml_evaluator_machine_top(context)->details.part.state;
    sxml_sequence result = sxml_sequence_empty();
    const sxml_status status = sxml_construction_end(construction, &result);
    return sxml_construct_end(context, status, result);
}

/* After start: ends the node unless its content was pushed. */
static sxml_status sxml_construct_continue(sxml_construction *construction, sxml_status status,
                                           sxml_construct_content_start start) {
    if (status != SXML_STATUS_OK || start.has_pushed) {
        return status;
    }
    return sxml_construct_finish_content(construction->dynamic_context);
}

/* ------------------------------------------- Document and element */

/* Starts a document node: in the enclosing construction its children
   become the enclosing node's content. */
static sxml_status sxml_construction_start_document(sxml_construction *construction) {
    construction->is_document = true;
    if (construction->enclosing != NULL) {
        return SXML_STATUS_OK;
    }
    return construction->builder.operations->start_document(
        construction->builder.state,
        &(sxml_document_start_event){
            .document_uri = {.bytes = NULL, .byte_count = 0},
            .base_uri = sxml_dynamic_context_get_static_base_uri(construction->dynamic_context),
            .location = sxml_construct_no_location});
}

/* DOCUMENT (XQuery 4.0, section 4.12.3.3). */
static sxml_status sxml_construct_step_document(sxml_dynamic_context *dynamic_context,
                                                sxml_evaluator_task *task) {
    if (task->phase != 0) {
        return sxml_construct_finish_content(dynamic_context);
    }
    const sxml_ir_expression_index content =
        sxml_construct_expression(task)->node_construction.content;
    sxml_construction *construction = NULL;
    sxml_status status = sxml_construct_allocate(dynamic_context, &construction);
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_open(
            dynamic_context,
            &(sxml_construction_request){.rules = SXML_CONSTRUCT_RULES_XQUERY,
                                         .base_uri = {.bytes = NULL, .byte_count = 0}},
            construction);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_start_document(construction);
    }
    sxml_construct_content_start start = {.has_pushed = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_start_content(construction, content, &start);
    }
    return sxml_construct_continue(construction, status, start);
}

/* What starts a new element. */
typedef struct sxml_construct_element_start {
    sxml_construct_name name;
    /* Sent after the start, before the content. */
    sxml_namespace_binding_view bindings;
    /* SXML_TYPE_NONE for xs:untyped. */
    sxml_type_identifier type_annotation;
} sxml_construct_element_start;

/* A constructed element's type annotation: xs:untyped under construction
   strip, xs:anyType under preserve (XQuery 4.0, section 4.12.1.3). */
static sxml_type_identifier sxml_construct_element_type(
    const sxml_ir_node_construction *construction) {
    if ((construction->construction_flags & (uint32_t)SXML_IR_CONSTRUCTION_PRESERVES_TYPES) !=
        0) {
        return sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_TYPE);
    }
    return (sxml_type_identifier){.index = SXML_TYPE_NONE};
}

/* Starts an element; in the enclosing construction it is a child of the
   enclosing node, which inherits its namespaces unless the flags say
   no-inherit (as its copy would, D69). */
static sxml_status sxml_construction_start_element(sxml_construction *construction,
                                                   const sxml_construct_element_start *start) {
    const bool is_enclosed = construction->enclosing != NULL;
    if (is_enclosed) {
        construction->enclosing->has_children = true;
    }
    sxml_status status = construction->builder.operations->start_element(
        construction->builder.state,
        &(sxml_element_start_event){
            .element_name = start->name.expanded_name,
            .namespace_prefix = start->name.namespace_prefix,
            .type_annotation = start->type_annotation,
            .is_id = false,
            .is_idrefs = false,
            .is_nilled = false,
            .ignores_parent_namespaces =
                is_enclosed && (construction->construction_flags &
                                (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES) != 0,
            .location = sxml_construct_no_location});
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_send_bindings(construction, start->bindings);
    }
    return status;
}

/* Opens the element of the constructor on top of the task stack, as rules
   say, and starts its content. */
typedef struct sxml_construct_element_request {
    sxml_construct_element_start start;
    sxml_ir_expression_index content;
    sxml_construct_rules rules;
} sxml_construct_element_request;

static sxml_status sxml_construct_open_element(sxml_dynamic_context *context,
                                               const sxml_construct_element_request *request) {
    sxml_construction *construction = NULL;
    sxml_status status = sxml_construct_allocate(context, &construction);
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_open(
            context,
            &(sxml_construction_request){
                .rules = request->rules,
                .base_uri = sxml_dynamic_context_get_static_base_uri(context)},
            construction);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_start_element(construction, &request->start);
    }
    sxml_construct_content_start start = {.has_pushed = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_start_content(construction, request->content, &start);
    }
    if (construction == NULL) {
        return status;
    }
    return sxml_construct_continue(construction, status, start);
}

/* ELEMENT (XQuery 4.0, sections 4.12.1 and 4.12.3.1). */
static sxml_status sxml_construct_step_element(sxml_dynamic_context *dynamic_context,
                                               sxml_evaluator_task *task) {
    if (task->phase != 0) {
        return sxml_construct_finish_content(dynamic_context);
    }
    const sxml_ir_node_construction *construction =
        &sxml_construct_expression(task)->node_construction;
    sxml_construct_element_request request = {
        .start = {.name = {.expanded_name = {.index = 0}, .namespace_prefix = {.index = 0}},
                  .bindings = sxml_ir_unit_get_namespace_bindings(
                      task->function->unit, construction->namespace_bindings),
                  .type_annotation = sxml_construct_element_type(construction)},
        .content = construction->content,
        .rules = SXML_CONSTRUCT_RULES_XQUERY};
    sxml_status status = sxml_construct_read_name(
        dynamic_context,
        &(sxml_construct_name_request){.atom = construction->name,
                                       .rule = SXML_CONSTRUCT_NAME_RULE_ELEMENT},
        &request.start.name);
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_check_element_name(dynamic_context, &request.start.name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_construct_open_element(dynamic_context, &request);
}

/* --------------------------------------------- Leaf kinds and nodes */

/* Builds a parentless leaf: an attribute, text, comment, processing
   instruction or namespace node. */
typedef enum sxml_construct_leaf_kind {
    SXML_CONSTRUCT_LEAF_KIND_ATTRIBUTE,
    SXML_CONSTRUCT_LEAF_KIND_TEXT,
    SXML_CONSTRUCT_LEAF_KIND_COMMENT,
    SXML_CONSTRUCT_LEAF_KIND_PROCESSING_INSTRUCTION,
    SXML_CONSTRUCT_LEAF_KIND_NAMESPACE,
} sxml_construct_leaf_kind;

typedef struct sxml_construct_leaf {
    sxml_construct_leaf_kind leaf_kind;
    /* ATTRIBUTE: the name. */
    sxml_construct_name name;
    /* ATTRIBUTE: true for xml:id. */
    bool is_id;
    /* TEXT: asks for CDATA sections (glossary cdata_section). */
    bool is_cdata_section;
    /* PROCESSING_INSTRUCTION: the target; NAMESPACE: the prefix. */
    sxml_byte_view target;
    /* The value, content or namespace URI. */
    sxml_byte_view text;
} sxml_construct_leaf;

static sxml_status sxml_construct_send_leaf(sxml_dynamic_context *context,
                                            sxml_tree_builder builder,
                                            const sxml_construct_leaf *leaf) {
    switch (leaf->leaf_kind) {
    case SXML_CONSTRUCT_LEAF_KIND_ATTRIBUTE:
        return builder.operations->add_attribute(
            builder.state, &(sxml_attribute_event){
                               .attribute_name = leaf->name.expanded_name,
                               .namespace_prefix = leaf->name.namespace_prefix,
                               .attribute_value = leaf->text,
                               .type_annotation = {.index = SXML_TYPE_NONE},
                               .is_id = leaf->is_id,
                               .is_idrefs = false,
                               .location = sxml_construct_no_location});
    case SXML_CONSTRUCT_LEAF_KIND_TEXT:
        return builder.operations->add_text(
            builder.state,
            &(sxml_text_event){.text = leaf->text,
                               .location = sxml_construct_no_location,
                               .is_cdata_section = leaf->is_cdata_section});
    case SXML_CONSTRUCT_LEAF_KIND_COMMENT:
        return builder.operations->add_comment(
            builder.state,
            &(sxml_comment_event){.comment = leaf->text, .location = sxml_construct_no_location});
    case SXML_CONSTRUCT_LEAF_KIND_PROCESSING_INSTRUCTION:
        return builder.operations->add_processing_instruction(
            builder.state, &(sxml_processing_instruction_event){
                               .target = leaf->target,
                               .data = leaf->text,
                               .location = sxml_construct_no_location});
    case SXML_CONSTRUCT_LEAF_KIND_NAMESPACE: {
        sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->item_heap);
        sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                          .namespace_uri = {.index = 0}};
        sxml_status status = sxml_name_pool_intern_namespace_prefix(name_pool, leaf->target,
                                                                    &binding.namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(name_pool, leaf->text,
                                                         &binding.namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            /* The builder raises XQDY0101 for a reserved binding or an
               empty URI (XQuery 4.0, section 4.12.3.7). */
            status = builder.operations->add_namespace_binding(builder.state, &binding);
        }
        return status;
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Adds leaf to construction as its copy would be added
   (sxml_construction_add_node), without a tree of its own (D166). */
static sxml_status sxml_construction_add_leaf(sxml_construction *construction,
                                              const sxml_construct_leaf *leaf) {
    construction->follows_atomic_item = false;
    switch (leaf->leaf_kind) {
    case SXML_CONSTRUCT_LEAF_KIND_ATTRIBUTE:
        if (construction->is_document) {
            return sxml_construction_raise(
                construction,
                (sxml_construct_rule_errors){
                    .xquery_error = &sxml_construct_document_attribute_error,
                    .xslt_error = &sxml_construct_xslt_document_attribute_error});
        }
        if (construction->has_children) {
            return sxml_construction_raise(
                construction,
                (sxml_construct_rule_errors){
                    .xquery_error = &sxml_construct_late_attribute_error,
                    .xslt_error = &sxml_construct_xslt_late_attribute_error});
        }
        break;
    case SXML_CONSTRUCT_LEAF_KIND_TEXT:
        /* A zero-length text node disappears in content (D163). */
        if (leaf->text.byte_count == 0) {
            return SXML_STATUS_OK;
        }
        construction->has_children = true;
        break;
    case SXML_CONSTRUCT_LEAF_KIND_COMMENT:
    case SXML_CONSTRUCT_LEAF_KIND_PROCESSING_INSTRUCTION:
        construction->has_children = true;
        break;
    case SXML_CONSTRUCT_LEAF_KIND_NAMESPACE:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_construct_send_leaf(construction->dynamic_context, construction->builder, leaf);
}

/* A parentless leaf in a tree of its own, or, in a delivering task, the
   leaf added to the receiver's construction and () (D166). A namespace
   node always gets its tree: building it checks its binding (XQDY0101 for
   an empty URI, 4.12.3.7), which an element's builder would read as an
   undeclaration. */
static sxml_status sxml_construct_build_leaf(sxml_dynamic_context *context,
                                             const sxml_construct_leaf *leaf,
                                             sxml_sequence *result) {
    sxml_construction *enclosing = sxml_construct_find_receiver(context);
    if (enclosing != NULL && leaf->leaf_kind != SXML_CONSTRUCT_LEAF_KIND_NAMESPACE) {
        *result = sxml_sequence_empty();
        return sxml_construction_add_leaf(enclosing, leaf);
    }
    sxml_construction construction;
    sxml_status status = sxml_construction_start(
        context,
        &(sxml_construction_request){.rules = SXML_CONSTRUCT_RULES_XQUERY,
                                     .base_uri = {.bytes = NULL, .byte_count = 0}},
        &construction);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_construct_send_leaf(context, construction.builder, leaf);
    if (status != SXML_STATUS_OK) {
        sxml_construction_abandon(&construction);
        return status;
    }
    return sxml_construction_finish(&construction, result);
}

/* Reads the value atom of the top task's expression and joins its
   atomized items into buffer. */
static sxml_status sxml_construct_read_joined_value(sxml_dynamic_context *context,
                                                    sxml_construct_text_buffer *buffer,
                                                    sxml_construct_join_result *result) {
    const sxml_ir_expression *expression =
        sxml_construct_expression(sxml_evaluator_machine_top(context));
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = sxml_evaluator_machine_read_atom(
        context, expression->node_construction.value, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_construct_join_atomized(
        context, &(sxml_construct_join_request){.value = value, .buffer = buffer}, result);
}

/* xs:ID collapses whitespace (xml:id processing, XML ID 1.0, section 4). */
static sxml_status sxml_construct_collapse_whitespace(sxml_construct_text_buffer *buffer) {
    size_t written = 0;
    bool pending_space = false;
    for (size_t byte_index = 0; byte_index < buffer->byte_count; byte_index += 1) {
        const uint8_t byte = buffer->bytes[byte_index];
        if (sxml_xml_byte_is_whitespace(byte)) {
            pending_space = written > 0;
            continue;
        }
        if (pending_space) {
            buffer->bytes[written] = ' ';
            written += 1;
            pending_space = false;
        }
        buffer->bytes[written] = byte;
        written += 1;
    }
    buffer->byte_count = written;
    return SXML_STATUS_OK;
}

/* Marks an attribute named xml:id as an ID (XQuery 4.0, section
   4.12.3.2). */
static sxml_status sxml_construct_mark_xml_id(sxml_dynamic_context *context,
                                              sxml_construct_leaf *leaf) {
    sxml_name_parts parts;
    const sxml_status status = sxml_name_pool_get_name_parts(
        sxml_item_heap_get_name_pool(context->item_heap), leaf->name.expanded_name, &parts);
    leaf->is_id = status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_XML &&
                  sxml_construct_text_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("id"));
    return status;
}

/* ATTRIBUTE (XQuery 4.0, section 4.12.3.2). */
static sxml_status sxml_construct_step_attribute(sxml_dynamic_context *dynamic_context,
                                                 sxml_evaluator_task *task) {
    const sxml_ir_node_construction *construction =
        &sxml_construct_expression(task)->node_construction;
    sxml_construct_leaf leaf = {.leaf_kind = SXML_CONSTRUCT_LEAF_KIND_ATTRIBUTE,
                                .name = {.expanded_name = {.index = 0},
                                         .namespace_prefix = {.index = 0}},
                                .is_id = false,
                                .target = {.bytes = NULL, .byte_count = 0},
                                .text = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_construct_read_name(
        dynamic_context,
        &(sxml_construct_name_request){.atom = construction->name,
                                       .rule = SXML_CONSTRUCT_NAME_RULE_ATTRIBUTE},
        &leaf.name);
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_check_attribute_name(dynamic_context, &leaf.name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_mark_xml_id(dynamic_context, &leaf);
    }
    sxml_construct_text_buffer buffer =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_construct_join_result joined = {.has_items = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_read_joined_value(dynamic_context, &buffer, &joined);
    }
    if (status == SXML_STATUS_OK && leaf.is_id) {
        status = sxml_construct_collapse_whitespace(&buffer);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        leaf.text = sxml_construct_text_buffer_view(&buffer);
        status = sxml_construct_build_leaf(dynamic_context, &leaf, &result);
    }
    sxml_construct_text_buffer_release(&buffer);
    return sxml_construct_end(dynamic_context, status, result);
}

/* TEXT (XQuery 4.0, section 4.12.3.4; XSLT 4.0, section 11.4.2): no node
   for an empty value; a value whose text is empty makes a zero-length text
   node, which has no parent (XDM 4.0, section 7.5.7.1) and disappears when
   it is added to content (D163). */
static sxml_status sxml_construct_step_text(sxml_dynamic_context *dynamic_context,
                                            sxml_evaluator_task *task) {
    (void)task;
    const uint32_t construction_flags =
        sxml_construct_expression(sxml_evaluator_machine_top(dynamic_context))
            ->node_construction.construction_flags;
    sxml_construct_text_buffer buffer =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_construct_join_result joined = {.has_items = false};
    sxml_status status = sxml_construct_read_joined_value(dynamic_context, &buffer, &joined);
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK && joined.has_items) {
        status = sxml_construct_build_leaf(
            dynamic_context,
            &(sxml_construct_leaf){.leaf_kind = SXML_CONSTRUCT_LEAF_KIND_TEXT,
                                   .name = {.expanded_name = {.index = 0},
                                            .namespace_prefix = {.index = 0}},
                                   .is_id = false,
                                   .is_cdata_section =
                                       (construction_flags &
                                        (uint32_t)SXML_IR_CONSTRUCTION_CDATA_SECTION) != 0,
                                   .target = {.bytes = NULL, .byte_count = 0},
                                   .text = sxml_construct_text_buffer_view(&buffer)},
            &result);
    }
    sxml_construct_text_buffer_release(&buffer);
    return sxml_construct_end(dynamic_context, status, result);
}

/* COMMENT (XQuery 4.0, section 4.12.3.6). */
static sxml_status sxml_construct_step_comment(sxml_dynamic_context *dynamic_context,
                                               sxml_evaluator_task *task) {
    (void)task;
    sxml_construct_text_buffer buffer =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_construct_join_result joined = {.has_items = false};
    sxml_status status = sxml_construct_read_joined_value(dynamic_context, &buffer, &joined);
    const sxml_byte_view text = sxml_construct_text_buffer_view(&buffer);
    if (status == SXML_STATUS_OK &&
        (sxml_construct_text_contains(text, sxml_byte_view_from_c_string("--")) ||
         (text.byte_count > 0 && text.bytes[text.byte_count - 1] == '-'))) {
        status = sxml_evaluator_machine_raise(dynamic_context, &sxml_construct_comment_error);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_build_leaf(
            dynamic_context,
            &(sxml_construct_leaf){.leaf_kind = SXML_CONSTRUCT_LEAF_KIND_COMMENT,
                                   .name = {.expanded_name = {.index = 0},
                                            .namespace_prefix = {.index = 0}},
                                   .is_id = false,
                                   .target = {.bytes = NULL, .byte_count = 0},
                                   .text = text},
            &result);
    }
    sxml_construct_text_buffer_release(&buffer);
    return sxml_construct_end(dynamic_context, status, result);
}

/* PROCESSING_INSTRUCTION (XQuery 4.0, section 4.12.3.5). */
static sxml_status sxml_construct_step_processing_instruction(
    sxml_dynamic_context *dynamic_context, sxml_evaluator_task *task) {
    const sxml_ir_atom name_atom = sxml_construct_expression(task)->node_construction.name;
    sxml_construct_text_buffer target =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_construct_text_buffer buffer =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_status status = sxml_construct_read_ncname(
        dynamic_context,
        &(sxml_construct_ncname_request){.atom = name_atom,
                                         .invalid_error = &sxml_construct_invalid_target_error,
                                         .allows_empty = false},
        &target);
    if (status == SXML_STATUS_OK &&
        sxml_construct_text_is_xml_in_any_case(sxml_construct_text_buffer_view(&target))) {
        status = sxml_evaluator_machine_raise(dynamic_context, &sxml_construct_xml_target_error);
    }
    sxml_construct_join_result joined = {.has_items = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_read_joined_value(dynamic_context, &buffer, &joined);
    }
    if (status == SXML_STATUS_OK &&
        sxml_construct_text_contains(sxml_construct_text_buffer_view(&buffer),
                                     sxml_byte_view_from_c_string("?>"))) {
        status = sxml_evaluator_machine_raise(dynamic_context,
                                              &sxml_construct_instruction_end_error);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_build_leaf(
            dynamic_context,
            &(sxml_construct_leaf){
                .leaf_kind = SXML_CONSTRUCT_LEAF_KIND_PROCESSING_INSTRUCTION,
                .name = {.expanded_name = {.index = 0}, .namespace_prefix = {.index = 0}},
                .is_id = false,
                .target = sxml_construct_text_buffer_view(&target),
                .text = sxml_construct_trim_leading(sxml_construct_text_buffer_view(&buffer))},
            &result);
    }
    sxml_construct_text_buffer_release(&target);
    sxml_construct_text_buffer_release(&buffer);
    return sxml_construct_end(dynamic_context, status, result);
}

/* NAMESPACE (XQuery 4.0, section 4.12.3.7): a parentless namespace node,
   stored as the root of its tree (D98). */
static sxml_status sxml_construct_step_namespace(sxml_dynamic_context *dynamic_context,
                                                 sxml_evaluator_task *task) {
    const sxml_ir_node_construction *construction =
        &sxml_construct_expression(task)->node_construction;
    sxml_construct_text_buffer prefix =
        sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_construct_text_buffer uri = sxml_construct_text_buffer_start(dynamic_context->allocator);
    sxml_status status = sxml_construct_read_ncname(
        dynamic_context,
        &(sxml_construct_ncname_request){.atom = construction->name,
                                         .invalid_error = &sxml_construct_invalid_prefix_error,
                                         .allows_empty = true},
        &prefix);
    sxml_construct_single_value single = {
        .atomized = sxml_sequence_empty(), .has_item = false, .item = {.representation = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_read_single(
            dynamic_context,
            &(sxml_construct_single_request){.atom = construction->value,
                                             .cardinality_error = &sxml_construct_uri_type_error},
            &single);
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK && single.has_item &&
        !sxml_construct_get_string_text(single.item, &text)) {
        status = sxml_evaluator_machine_raise(dynamic_context, &sxml_construct_uri_type_error);
    }
    if (status == SXML_STATUS_OK) {
        /* The whitespace facet of xs:anyURI collapses. */
        status = sxml_construct_text_buffer_append(&uri, text);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_collapse_whitespace(&uri);
    }
    sxml_sequence result = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_build_leaf(
            dynamic_context,
            &(sxml_construct_leaf){.leaf_kind = SXML_CONSTRUCT_LEAF_KIND_NAMESPACE,
                                   .name = {.expanded_name = {.index = 0},
                                            .namespace_prefix = {.index = 0}},
                                   .is_id = false,
                                   .target = sxml_construct_text_buffer_view(&prefix),
                                   .text = sxml_construct_text_buffer_view(&uri)},
            &result);
    }
    sxml_sequence_release(single.atomized);
    sxml_construct_text_buffer_release(&prefix);
    sxml_construct_text_buffer_release(&uri);
    return sxml_construct_end(dynamic_context, status, result);
}

/* ------------------------------------------------------ Shallow copy */

/* The item SHALLOW_COPY copies: none, or one, borrowed from value. */
typedef struct sxml_construct_copied_item {
    /* Owned. */
    sxml_sequence value;
    bool has_item;
    sxml_item item;
    /* For an XNode: the node and its description. */
    bool is_node;
    sxml_node_reference node;
    sxml_node_description description;
} sxml_construct_copied_item;

static sxml_status sxml_construct_read_copied_item(sxml_dynamic_context *context,
                                                   sxml_construct_copied_item *copied) {
    *copied = (sxml_construct_copied_item){.value = sxml_sequence_empty(),
                                           .has_item = false,
                                           .item = {.representation = 0},
                                           .is_node = false};
    const sxml_ir_expression *expression =
        sxml_construct_expression(sxml_evaluator_machine_top(context));
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status =
        sxml_evaluator_machine_read_atom(context, expression->node_construction.value, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t item_count = 0;
    status = sxml_sequence_count(value, context->error_record, &item_count);
    if (status == SXML_STATUS_OK && item_count > 1) {
        status = sxml_evaluator_machine_raise(context, &sxml_construct_copy_cardinality_error);
    }
    if (status != SXML_STATUS_OK || item_count == 0) {
        return status;
    }
    sxml_sequence_retain(value);
    copied->value = value;
    status = sxml_sequence_get_item(
        value, (sxml_item_request){.item_index = 0, .error_record = context->error_record},
        &copied->item);
    if (status == SXML_STATUS_OK) {
        copied->has_item = true;
        copied->is_node = sxml_item_get_kind(copied->item) == SXML_ITEM_KIND_NODE;
    }
    if (status == SXML_STATUS_OK && copied->is_node) {
        status = sxml_item_get_node(copied->item, &copied->node);
    }
    if (status == SXML_STATUS_OK && copied->is_node) {
        status = sxml_node_describe(copied->node, &copied->description);
    }
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(copied->value);
        copied->value = sxml_sequence_empty();
    }
    return status;
}

static bool sxml_construct_copy_takes_content(const sxml_construct_copied_item *copied) {
    return copied->is_node && (copied->description.node_kind == SXML_NODE_KIND_ELEMENT ||
                               copied->description.node_kind == SXML_NODE_KIND_DOCUMENT);
}

/* A node that is not an element or document: copied whole, its content
   not evaluated (XSLT 4.0, section 11.9.1). In a delivering task whose
   receiver has the same rules and flags the node itself is delivered, as
   the receiver copies it in the same way (D166). */
static sxml_status sxml_construct_copy_whole(sxml_dynamic_context *context,
                                             const sxml_construct_copied_item *copied,
                                             sxml_sequence *result) {
    const sxml_construction *enclosing = sxml_construct_find_receiver(context);
    const sxml_construction empty = sxml_construction_empty(context, SXML_CONSTRUCT_RULES_XSLT);
    if (enclosing != NULL && enclosing->rules == empty.rules &&
        enclosing->construction_flags == empty.construction_flags) {
        sxml_item_retain(copied->item);
        *result = sxml_sequence_from_item(copied->item);
        return SXML_STATUS_OK;
    }
    sxml_construction construction;
    sxml_status status = sxml_construction_start(
        context,
        &(sxml_construction_request){.rules = SXML_CONSTRUCT_RULES_XSLT,
                                     .base_uri = {.bytes = NULL, .byte_count = 0}},
        &construction);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_construction_copy(&construction, copied->node, &copied->description);
    if (status != SXML_STATUS_OK) {
        sxml_construction_abandon(&construction);
        return status;
    }
    return sxml_construction_finish(&construction, result);
}

/* An element or document node with new content (XSLT 4.0, section
   11.9.1: the element's namespace nodes are copied unless the
   NO_PRESERVE_NAMESPACES flag says copy-namespaces "no"; its type
   annotation is kept only with PRESERVES_TYPES, validation "preserve"):
   opens it and starts its content. */
static sxml_status sxml_construct_copy_with_content(sxml_dynamic_context *context,
                                                    const sxml_construct_copied_item *copied) {
    const sxml_ir_expression_index content =
        sxml_construct_expression(sxml_evaluator_machine_top(context))->node_construction.content;
    if (copied->description.node_kind == SXML_NODE_KIND_ELEMENT) {
        const uint32_t flags = sxml_construct_top_flags(context);
        sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                                .namespace_binding_count = 0};
        if ((flags & (uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES) == 0) {
            const sxml_status status =
                sxml_node_get_in_scope_namespaces(copied->node, &bindings);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return sxml_construct_open_element(
            context,
            &(sxml_construct_element_request){
                .start = {.name = {.expanded_name = copied->description.node_name,
                                   .namespace_prefix = copied->description.namespace_prefix},
                          .bindings = bindings,
                          .type_annotation =
                              (flags & (uint32_t)SXML_IR_CONSTRUCTION_PRESERVES_TYPES) != 0
                                  ? copied->description.type_annotation
                                  : (sxml_type_identifier){.index = SXML_TYPE_NONE}},
                .content = content,
                .rules = SXML_CONSTRUCT_RULES_XSLT});
    }
    sxml_construction *construction = NULL;
    sxml_status status = sxml_construct_allocate(context, &construction);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_construction_open(
        context,
        &(sxml_construction_request){.rules = SXML_CONSTRUCT_RULES_XSLT,
                                     .base_uri = {.bytes = NULL, .byte_count = 0}},
        construction);
    if (status == SXML_STATUS_OK) {
        status = sxml_construction_start_document(construction);
    }
    sxml_construct_content_start start = {.has_pushed = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_construct_start_content(construction, content, &start);
    }
    return sxml_construct_continue(construction, status, start);
}

/* SHALLOW_COPY (XSLT 4.0, section 11.9.1). */
static sxml_status sxml_construct_step_shallow_copy(sxml_dynamic_context *dynamic_context,
                                                    sxml_evaluator_task *task) {
    if (task->phase != 0) {
        return sxml_construct_finish_content(dynamic_context);
    }
    sxml_construct_copied_item copied;
    sxml_status status = sxml_construct_read_copied_item(dynamic_context, &copied);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_construct_copy_takes_content(&copied)) {
        /* The copied node stays alive in the atom's slot while the content
           is evaluated; the start reads what it needs at once. */
        status = sxml_construct_copy_with_content(dynamic_context, &copied);
        sxml_sequence_release(copied.value);
        return status;
    }
    sxml_sequence result = sxml_sequence_empty();
    if (copied.has_item && !copied.is_node) {
        /* Atomic items, function items and JNodes are returned as they
           are. */
        sxml_item_retain(copied.item);
        result = sxml_sequence_from_item(copied.item);
    } else if (copied.has_item) {
        status = sxml_construct_copy_whole(dynamic_context, &copied, &result);
    }
    sxml_sequence_release(copied.value);
    return sxml_construct_end(dynamic_context, status, result);
}

/* -------------------------------------------------------------- Part */

static const sxml_evaluator_kind_handler sxml_construct_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
     .step = sxml_construct_step_document},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
     .step = sxml_construct_step_element},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
     .step = sxml_construct_step_attribute},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
     .step = sxml_construct_step_namespace},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
     .step = sxml_construct_step_text},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
     .step = sxml_construct_step_comment},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
     .step = sxml_construct_step_processing_instruction},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
     .step = sxml_construct_step_shallow_copy},
};

const sxml_evaluator_part sxml_evaluator_part_construct = {
    .kind_handlers = sxml_construct_kind_handlers,
    .kind_handler_count =
        sizeof(sxml_construct_kind_handlers) / sizeof(sxml_construct_kind_handlers[0]),
};
