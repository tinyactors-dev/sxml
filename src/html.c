/*
 * The HTML parser feature (src/html_internal.h; F&O 4.0, 17.3 "Functions
 * on HTML Data"; HTML Living Standard, 13.2): parses an HTML document
 * into a working tree and sends that tree to an arena tree as tree
 * builder events, mapping it to XDM as F&O 4.0, 17.3.1 describes, with
 * the coercions of HTML, 13.2.9 "Coercing an HTML DOM into an infoset"
 * for names and characters XML does not allow.
 *
 * Golden file: src/processor.c (sxml_processor_parse_document: a parser
 * into an arena tree that destroys itself with its last reference).
 */
#include "html_internal.h"

#include "function_library_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"

#include <string.h>

/* ------------------------------------------------------------ Mapping */

/* What the walk needs to send events. */
typedef struct sxml_html_emitter {
    const sxml_html_working_tree *tree;
    sxml_tree_builder builder;
    sxml_name_pool *name_pool;
    /* By sxml_html_namespace. */
    sxml_namespace_uri_identifier element_namespaces[3];
    sxml_namespace_uri_identifier xlink_namespace;
    sxml_namespace_prefix_identifier xlink_prefix;
    sxml_namespace_prefix_identifier xml_prefix;
    /* Escaped names and coerced characters. */
    sxml_xml_buffer scratch;
} sxml_html_emitter;

static sxml_byte_view working_text(const sxml_html_emitter *emitter, sxml_byte_range range) {
    return sxml_xml_buffer_view(&emitter->tree->text, range);
}

/* 13.2.9: a local name with characters an NCName does not allow gets
   each of them as "U" and six uppercase hexadecimal digits (F&O 4.0,
   17.3.1.10: the result must be a valid NCName). The name is in
   emitter->scratch or, when it needs no change, name itself. */
static sxml_status escape_local_name(sxml_html_emitter *emitter, sxml_byte_view name,
                                     sxml_byte_view *escaped) {
    if (sxml_xml_text_is_ncname(name)) {
        *escaped = name;
        return SXML_STATUS_OK;
    }
    emitter->scratch.byte_count = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < name.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(name, offset);
        const uint32_t codepoint = decoded.codepoint;
        const bool is_allowed = codepoint != ':' && (offset == 0
                                                         ? sxml_xml_codepoint_is_name_start(codepoint)
                                                         : sxml_xml_codepoint_is_name(codepoint));
        if (is_allowed) {
            status = sxml_xml_buffer_append(
                &emitter->scratch,
                (sxml_byte_view){.bytes = name.bytes + offset,
                                 .byte_count = decoded.encoded_byte_count});
        } else {
            static const char digits[] = "0123456789ABCDEF";
            uint8_t replacement[7] = {'U', 0, 0, 0, 0, 0, 0};
            for (size_t digit_index = 0; digit_index < 6; digit_index += 1) {
                replacement[6 - digit_index] =
                    (uint8_t)digits[(codepoint >> (4U * digit_index)) & 0xFU];
            }
            status = sxml_xml_buffer_append(
                &emitter->scratch, (sxml_byte_view){.bytes = replacement, .byte_count = 7});
        }
        offset += decoded.encoded_byte_count;
    }
    *escaped = (sxml_byte_view){.bytes = emitter->scratch.bytes,
                                .byte_count = emitter->scratch.byte_count};
    return status;
}

/* True when text has a character XML 1.0 does not allow (or U+000C). */
static bool needs_character_coercion(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') {
            return true;
        }
        if (byte == 0xEF && byte_index + 2 < text.byte_count && text.bytes[byte_index + 1] == 0xBF &&
            (text.bytes[byte_index + 2] == 0xBE || text.bytes[byte_index + 2] == 0xBF)) {
            return true;
        }
    }
    return false;
}

/* What coercion is applied to text. */
typedef struct sxml_html_coercion_request {
    sxml_byte_view text;
    /* Comment data: no "--" and no "-" at the end (13.2.9). */
    bool is_comment;
} sxml_html_coercion_request;

/* 13.2.9: U+000C becomes a space, any other character XML does not allow
   U+FFFD; in comments a space separates "--" and follows a final "-".
   The result is in emitter->scratch or, without change, the text. */
static sxml_status coerce_characters(sxml_html_emitter *emitter,
                                     const sxml_html_coercion_request *request,
                                     sxml_byte_view *coerced) {
    const sxml_byte_view text = request->text;
    bool needs_change = needs_character_coercion(text);
    if (request->is_comment && !needs_change) {
        for (size_t byte_index = 0; byte_index + 1 < text.byte_count && !needs_change;
             byte_index += 1) {
            needs_change = text.bytes[byte_index] == '-' && text.bytes[byte_index + 1] == '-';
        }
        needs_change = needs_change || (text.byte_count > 0 && text.bytes[text.byte_count - 1] == '-');
    }
    if (!needs_change) {
        *coerced = text;
        return SXML_STATUS_OK;
    }
    emitter->scratch.byte_count = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, offset);
        uint32_t codepoint = decoded.codepoint;
        if (codepoint == 0x0C) {
            codepoint = ' ';
        } else if (!sxml_xml_codepoint_is_char(codepoint)) {
            codepoint = 0xFFFD;
        }
        if (request->is_comment && codepoint == '-' && emitter->scratch.byte_count > 0 &&
            emitter->scratch.bytes[emitter->scratch.byte_count - 1] == '-') {
            status = sxml_xml_buffer_append_byte(&emitter->scratch, ' ');
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_codepoint(&emitter->scratch, codepoint);
        }
        offset += decoded.encoded_byte_count;
    }
    if (status == SXML_STATUS_OK && request->is_comment && emitter->scratch.byte_count > 0 &&
        emitter->scratch.bytes[emitter->scratch.byte_count - 1] == '-') {
        status = sxml_xml_buffer_append_byte(&emitter->scratch, ' ');
    }
    *coerced = (sxml_byte_view){.bytes = emitter->scratch.bytes,
                                .byte_count = emitter->scratch.byte_count};
    return status;
}

/* F&O 4.0, 17.3.1.1: attributes that are namespace declarations are not
   attributes of the XDM element. */
static bool is_namespace_declaration(const sxml_html_emitter *emitter,
                                     const sxml_html_attribute *attribute) {
    if (attribute->attribute_namespace == SXML_HTML_ATTRIBUTE_NAMESPACE_XMLNS) {
        return true;
    }
    if (attribute->attribute_namespace != SXML_HTML_ATTRIBUTE_NAMESPACE_NONE) {
        return false;
    }
    const sxml_byte_view name = working_text(emitter, attribute->attribute_name);
    return sxml_byte_view_equals(name, SXML_BYTE_VIEW_FROM_LITERAL("xmlns")) ||
           (name.byte_count >= 6 && memcmp(name.bytes, "xmlns:", 6) == 0);
}

static sxml_status emit_attribute(sxml_html_emitter *emitter,
                                  const sxml_html_attribute *attribute) {
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    if (attribute->attribute_namespace == SXML_HTML_ATTRIBUTE_NAMESPACE_XLINK) {
        namespace_uri = emitter->xlink_namespace;
        prefix = emitter->xlink_prefix;
    } else if (attribute->attribute_namespace == SXML_HTML_ATTRIBUTE_NAMESPACE_XML) {
        namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_XML};
        prefix = emitter->xml_prefix;
    }
    const sxml_byte_view raw_name = working_text(emitter, attribute->attribute_name);
    sxml_byte_view local_name = {.bytes = NULL, .byte_count = 0};
    sxml_status status = escape_local_name(emitter, raw_name, &local_name);
    sxml_name_identifier attribute_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            emitter->name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
            &attribute_name);
    }
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = coerce_characters(
            emitter,
            &(sxml_html_coercion_request){.text = working_text(emitter, attribute->attribute_value),
                                          .is_comment = false},
            &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* F&O 4.0, 17.3.1.5: an id attribute is an ID when its value is an
       NCName. */
    const bool is_id = attribute->attribute_namespace == SXML_HTML_ATTRIBUTE_NAMESPACE_NONE &&
                       sxml_byte_view_equals(raw_name, SXML_BYTE_VIEW_FROM_LITERAL("id")) &&
                       sxml_xml_text_is_ncname(value);
    return emitter->builder.operations->add_attribute(
        emitter->builder.state,
        &(sxml_attribute_event){.attribute_name = attribute_name,
                                .namespace_prefix = prefix,
                                .attribute_value = value,
                                .type_annotation = {.index = SXML_TYPE_NONE},
                                .is_id = is_id,
                                .is_idrefs = false,
                                .location = {.line_number = 0}});
}

static sxml_status emit_element_start(sxml_html_emitter *emitter, const sxml_html_node *node) {
    sxml_byte_view local_name = {.bytes = NULL, .byte_count = 0};
    sxml_status status = escape_local_name(emitter, working_text(emitter, node->name), &local_name);
    sxml_name_identifier element_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            emitter->name_pool,
            &(sxml_name_request){.namespace_uri =
                                     emitter->element_namespaces[node->element_namespace],
                                 .local_name = local_name},
            &element_name);
    }
    if (status == SXML_STATUS_OK) {
        status = emitter->builder.operations->start_element(
            emitter->builder.state,
            &(sxml_element_start_event){.element_name = element_name,
                                        .namespace_prefix = {.index = 0},
                                        .type_annotation = {.index = SXML_TYPE_NONE},
                                        .is_id = false,
                                        .is_idrefs = false,
                                        .is_nilled = false,
                                        .ignores_parent_namespaces = false,
                                        .location = {.line_number = 0}});
    }
    for (uint32_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < node->attribute_count;
         attribute_index += 1) {
        const sxml_html_attribute *attribute = sxml_xml_array_at(
            &emitter->tree->attributes, (size_t)node->first_attribute + attribute_index);
        if (!is_namespace_declaration(emitter, attribute)) {
            status = emit_attribute(emitter, attribute);
        }
    }
    return status;
}

static sxml_status emit_text(sxml_html_emitter *emitter, const sxml_html_node *node) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t segment_number = node->first_segment;
         status == SXML_STATUS_OK && segment_number != SXML_HTML_NO_NODE;) {
        const sxml_html_text_segment *segment =
            sxml_xml_array_at(&emitter->tree->segments, segment_number);
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        status = coerce_characters(
            emitter,
            &(sxml_html_coercion_request){.text = working_text(emitter, segment->text_range),
                                          .is_comment = false},
            &text);
        if (status == SXML_STATUS_OK && text.byte_count > 0) {
            status = emitter->builder.operations->add_text(
                emitter->builder.state,
                &(sxml_text_event){.text = text, .location = {.line_number = 0},
                                   .is_cdata_section = false});
        }
        segment_number = segment->next_segment;
    }
    return status;
}

/* Sends a node that has no children: text, comment, processing
   instruction. */
static sxml_status emit_leaf(sxml_html_emitter *emitter, const sxml_html_node *node) {
    if (node->node_kind == SXML_HTML_NODE_KIND_TEXT) {
        return emit_text(emitter, node);
    }
    sxml_byte_view data = {.bytes = NULL, .byte_count = 0};
    const bool is_comment = node->node_kind == SXML_HTML_NODE_KIND_COMMENT;
    sxml_status status = coerce_characters(
        emitter,
        &(sxml_html_coercion_request){.text = working_text(emitter, node->data),
                                      .is_comment = is_comment},
        &data);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_comment) {
        return emitter->builder.operations->add_comment(
            emitter->builder.state,
            &(sxml_comment_event){.comment = data, .location = {.line_number = 0}});
    }
    return emitter->builder.operations->add_processing_instruction(
        emitter->builder.state,
        &(sxml_processing_instruction_event){.target = working_text(emitter, node->name),
                                             .data = data,
                                             .location = {.line_number = 0}});
}

/* Walks the working tree in document order by its links, sending the
   events of every node below the document node (no recursion, D71). */
static sxml_status emit_children_of_document(sxml_html_emitter *emitter) {
    const sxml_html_working_tree *tree = emitter->tree;
    uint32_t node_number = sxml_html_working_tree_node(tree, 0)->first_child;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && node_number != SXML_HTML_NO_NODE) {
        const sxml_html_node *node = sxml_html_working_tree_node(tree, node_number);
        if (node->node_kind == SXML_HTML_NODE_KIND_ELEMENT) {
            status = emit_element_start(emitter, node);
            if (status == SXML_STATUS_OK && node->first_child != SXML_HTML_NO_NODE) {
                node_number = node->first_child;
                continue;
            }
            if (status == SXML_STATUS_OK) {
                status = emitter->builder.operations->end_element(emitter->builder.state);
            }
        } else {
            status = emit_leaf(emitter, node);
        }
        /* Up to the next sibling, ending the elements left on the way. */
        while (status == SXML_STATUS_OK && node_number != SXML_HTML_NO_NODE) {
            const sxml_html_node *current = sxml_html_working_tree_node(tree, node_number);
            if (current->next_sibling != SXML_HTML_NO_NODE) {
                node_number = current->next_sibling;
                break;
            }
            node_number = current->parent;
            if (node_number == 0) {
                node_number = SXML_HTML_NO_NODE;
                break;
            }
            status = emitter->builder.operations->end_element(emitter->builder.state);
        }
    }
    return status;
}

/* Interns the names the mapping uses. */
static sxml_status prepare_emitter(sxml_html_emitter *emitter) {
    static const sxml_byte_view element_namespace_uris[3] = {
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/1999/xhtml"),
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/1998/Math/MathML"),
        SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2000/svg"),
    };
    sxml_status status = SXML_STATUS_OK;
    for (size_t namespace_index = 0; status == SXML_STATUS_OK && namespace_index < 3;
         namespace_index += 1) {
        status = sxml_name_pool_intern_namespace_uri(emitter->name_pool,
                                                     element_namespace_uris[namespace_index],
                                                     &emitter->element_namespaces[namespace_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            emitter->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("http://www.w3.org/1999/xlink"),
            &emitter->xlink_namespace);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            emitter->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xlink"), &emitter->xlink_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            emitter->name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &emitter->xml_prefix);
    }
    return status;
}

/* Sends the working tree as one document to builder. */
typedef struct sxml_html_emission_request {
    const sxml_html_working_tree *tree;
    sxml_tree_builder builder;
    sxml_name_pool *name_pool;
    sxml_allocator allocator;
    sxml_byte_view base_uri;
    sxml_byte_view document_uri;
} sxml_html_emission_request;

static sxml_status emit_document(const sxml_html_emission_request *request) {
    sxml_html_emitter emitter;
    memset(&emitter, 0, sizeof(emitter));
    emitter.tree = request->tree;
    emitter.builder = request->builder;
    emitter.name_pool = request->name_pool;
    emitter.scratch = sxml_xml_buffer_empty(request->allocator);
    sxml_status status = prepare_emitter(&emitter);
    if (status == SXML_STATUS_OK) {
        status = emitter.builder.operations->start_document(
            emitter.builder.state,
            &(sxml_document_start_event){.document_uri = request->document_uri,
                                         .base_uri = request->base_uri,
                                         .location = {.line_number = 0}});
    }
    if (status == SXML_STATUS_OK) {
        status = emit_children_of_document(&emitter);
    }
    if (status == SXML_STATUS_OK) {
        status = emitter.builder.operations->end_document(emitter.builder.state);
    }
    sxml_xml_buffer_release(&emitter.scratch);
    return status;
}

/* ------------------------------------------------------------ Parsing */

/* A constructed working tree to build an arena tree of, in processor. */
typedef struct sxml_html_build_request {
    sxml_processor *processor;
    const sxml_html_working_tree *tree;
    const sxml_html_document_request *document;
} sxml_html_build_request;

/* Builds the arena tree of a constructed working tree. */
static sxml_status build_tree(const sxml_html_build_request *build, sxml_item *document_node) {
    sxml_processor *processor = build->processor;
    const sxml_html_document_request *request = build->document;
    const sxml_allocator allocator = processor->allocator;
    sxml_arena_tree *arena_tree = NULL;
    /* The tree destroys itself with its last reference (D39). */
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){
            .allocator = allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = processor->limits.xml_limits.maximum_depth,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = false,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = emit_document(&(sxml_html_emission_request){
            .tree = build->tree,
            .builder = sxml_arena_tree_as_tree_builder(arena_tree),
            .name_pool = processor->name_pool,
            .allocator = allocator,
            .base_uri = request->base_uri,
            .document_uri = request->document_uri});
    }
    sxml_tree *finished = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &finished);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make((sxml_node_reference){
        .tree = finished, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(finished);
    return SXML_STATUS_OK;
}

sxml_status sxml_html_parse_document(sxml_processor *processor,
                                     const sxml_html_document_request *request,
                                     sxml_html_document_outcome *outcome) {
    outcome->failure = SXML_HTML_DOCUMENT_FAILURE_NONE;
    outcome->document_node = (sxml_item){.representation = 0};
    const sxml_allocator allocator = processor->allocator;
    sxml_xml_buffer stream = sxml_xml_buffer_empty(allocator);
    sxml_html_input_outcome input_outcome;
    sxml_status status = sxml_html_prepare_input(&request->input, &stream, &input_outcome);
    if (status == SXML_STATUS_OK &&
        input_outcome.failure == SXML_HTML_INPUT_FAILURE_UNKNOWN_ENCODING) {
        outcome->failure = SXML_HTML_DOCUMENT_FAILURE_UNKNOWN_ENCODING;
    } else if (status == SXML_STATUS_OK && input_outcome.has_parse_error &&
               request->fails_on_parse_error) {
        outcome->failure = SXML_HTML_DOCUMENT_FAILURE_PARSE_ERROR;
    }
    if (status != SXML_STATUS_OK || outcome->failure != SXML_HTML_DOCUMENT_FAILURE_NONE) {
        sxml_xml_buffer_release(&stream);
        return status;
    }
    sxml_html_working_tree tree;
    status = sxml_html_working_tree_initialize(&tree, allocator);
    sxml_html_parse_outcome parse_outcome = {.has_parse_error = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_html_construct_tree(
            &(sxml_html_parse_request){
                .input = {.bytes = stream.bytes, .byte_count = stream.byte_count},
                .stops_at_parse_error = request->fails_on_parse_error},
            &tree, &parse_outcome);
    }
    if (status == SXML_STATUS_OK && parse_outcome.has_parse_error && request->fails_on_parse_error) {
        outcome->failure = SXML_HTML_DOCUMENT_FAILURE_PARSE_ERROR;
    } else if (status == SXML_STATUS_OK) {
        status = build_tree(
            &(sxml_html_build_request){.processor = processor, .tree = &tree, .document = request},
            &outcome->document_node);
    }
    sxml_html_working_tree_finalize(&tree);
    sxml_xml_buffer_release(&stream);
    return status;
}

/* --------------------------------------------------------- The feature */

static const sxml_html_feature_operations sxml_html_feature_operations_table = {
    .parse_document = sxml_html_parse_document,
};

const sxml_optional_feature_definition sxml_html_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_HTML,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("html"),
    .operations = &sxml_html_feature_operations_table,
    .finalize_state = NULL,
    .library_functions = &sxml_html_library_functions,
};

sxml_optional_feature sxml_html_parser_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_html_feature_definition};
}
