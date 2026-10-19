/*
 * Evaluator part: streamed evaluation (src/evaluator_machine_internal.h,
 * D101, D166, D219). Evaluates STREAM (src/ir_internal.h): the stream
 * points of xsl:source-document with streamable="yes" and of the bodies it
 * streams (XSLT 4.0, 18.1 and 19).
 *
 * A streamed document (glossary `streamed_document`) is read by an
 * incremental parse (src/xml_internal.h, glossary `incremental_parse`)
 * whose events go to the stream builder below, never into one tree. The
 * builder keeps the open elements with their namespace bindings and
 * attributes (the window of ancestors) and queues the events the STREAM
 * tasks read one at a time; it drops the events of content nobody reads
 * (skipping) and builds the subtree of a node that needs one (capturing).
 *
 * A selected node lives in a stream window (glossary `stream_window`), a
 * small arena tree made for it: the document node, the node's ancestors
 * with their attributes and namespaces, the node, and either the node's
 * subtree (a full window, made while its events pass) or nothing more (a
 * shallow window, for an element whose body reads its content only
 * through STREAMs of its own, or reads none: XSLT 4.0, 19, a motionless
 * body). The element of a shallow window is the open node (glossary
 * `open_node`): a STREAM whose origin it is reads the element's content
 * from the events; whatever the body did not read is skipped when it
 * returns. Windows are released when the body's value no longer holds
 * their nodes, so the memory a streamed document takes is bounded by the
 * depth of the document and the windows the bodies keep, not by its size.
 * A window goes through the document preparation fn:doc applies (XSLT's
 * whitespace stripping, 4.2).
 *
 * A STREAM whose origin is not an open node (a node of a full window, of
 * an ordinary tree, a document the resolver gave as a node) walks the
 * axis of origin's tree instead: the same IR serves streamed and
 * unstreamed nodes, and the results are those of unstreamed evaluation.
 *
 * The parser never calls the evaluator: a STREAM task resumes the parse,
 * which returns after a few events, and pushes the predicate and body
 * tasks itself, so nested STREAMs (a path step by step, xsl:for-each
 * inside xsl:for-each) are tasks of the one machine loop (D71). A STREAM
 * below the top finds the streamed document of its origin on the task
 * stack.
 *
 * Golden file: src/evaluator_template.c (a part task that loops over
 * items and pushes calls), src/xslt_whitespace.c (a tree builder of its
 * own).
 *
 * Specification: XSLT 4.0, 18.1 "xsl:source-document" and 19
 * "Streamability"; XPath 4.0, 4.6 (paths) and 4.4.1 (predicates).
 */
#include "evaluator_machine_internal.h"

#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xml.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Errors */

static const sxml_static_error sxml_stream_not_well_formed_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The document of xsl:source-document is not a well-formed XML document "
        "(XSLT 4.0, 18.1; F&O 4.0, fn:doc)"),
};

static const sxml_static_error sxml_stream_not_a_node_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0020"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An axis step is applied to an item that is not a node (XPath 4.0, 4.6.2)"),
};

static const sxml_static_error sxml_stream_no_document_uri_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0005"),
    .description =
        SXML_BYTE_VIEW_INITIALIZER("The href of xsl:source-document is not a URI (XSLT 4.0, 18.1)"),
};

/* --------------------------------------------------- The open elements */

/* An attribute of an open element; its value is in the document's
   ancestor text. */
typedef struct sxml_stream_attribute {
    sxml_attribute_event event;
    sxml_byte_range value;
} sxml_stream_attribute;

/* An open element of the streamed document: an ancestor of what is read
   next. */
typedef struct sxml_stream_element {
    sxml_element_start_event event;
    sxml_byte_range entity_base_uri;
    size_t first_binding;
    size_t binding_count;
    size_t first_attribute;
    size_t attribute_count;
    /* The ancestor text's size before the element's bytes. */
    size_t text_mark;
} sxml_stream_element;

/* An event queued for the STREAM tasks. */
typedef enum sxml_stream_event_kind {
    SXML_STREAM_EVENT_NONE,
    /* The element on top of the open elements started (its namespaces and
       attributes are there). */
    SXML_STREAM_EVENT_START_ELEMENT,
    /* The element on top ended; it is removed when the event is taken. */
    SXML_STREAM_EVENT_END_ELEMENT,
    SXML_STREAM_EVENT_TEXT,
    SXML_STREAM_EVENT_COMMENT,
    SXML_STREAM_EVENT_PROCESSING_INSTRUCTION,
    SXML_STREAM_EVENT_END_DOCUMENT,
} sxml_stream_event_kind;

typedef struct sxml_stream_event {
    sxml_stream_event_kind event_kind;
    /* In the event text: the text, the comment, the target. */
    sxml_byte_range text;
    /* The data of a processing instruction. */
    sxml_byte_range data;
    bool is_cdata_section;
    sxml_source_location location;
} sxml_stream_event;

/* What the builder does with the events of the parse. */
typedef enum sxml_stream_mode {
    /* Queues them and asks the parse to pause. */
    SXML_STREAM_MODE_NOTIFY,
    /* Drops them until the element at mode_depth ends. */
    SXML_STREAM_MODE_SKIP,
    /* Sends them to the window being captured until the element at
       mode_depth ends. */
    SXML_STREAM_MODE_CAPTURE,
    /* Drops everything to the end of the document. */
    SXML_STREAM_MODE_DRAIN,
} sxml_stream_mode;

/* Events of one parse visit: a start tag and its end at most, after the
   text before them. */
#define STREAM_QUEUE_CAPACITY 4U

/* ---------------------------------------------------------- Documents */

/* A window being built: its arena tree and the next node's index. */
typedef struct sxml_stream_window_build {
    sxml_arena_tree *arena_tree;
    sxml_tree_builder builder;
    uint32_t next_node_index;
    /* The index of the last element replayed. */
    uint32_t last_element_index;
} sxml_stream_window_build;

/* A streamed document (glossary `streamed_document`). */
typedef struct sxml_stream_document {
    sxml_allocator allocator;
    sxml_processor *processor;
    sxml_processor_document_loader *loader;
    sxml_streamed_document source;
    sxml_xml_parser *parser;
    sxml_xml_incremental_parse *parse;
    bool is_finished;
    /* The document node's URIs and unparsed entities, for every window. */
    sxml_xml_buffer document_text;
    sxml_byte_range document_uri;
    sxml_byte_range base_uri;
    sxml_xml_array unparsed_entities;
    /* The open elements, their bindings and attributes, their text. */
    sxml_xml_array elements;
    sxml_xml_array bindings;
    sxml_xml_array attributes;
    sxml_xml_buffer ancestor_text;
    /* Events for the STREAM tasks, and their text. */
    sxml_stream_event queue[STREAM_QUEUE_CAPACITY];
    size_t queue_start;
    size_t queue_count;
    sxml_xml_buffer event_text;
    /* Text events not yet ended by another event: one text node. */
    bool has_pending_text;
    sxml_stream_event pending_text;
    sxml_stream_mode mode;
    size_t mode_depth;
    /* The window captured in CAPTURE mode. */
    sxml_stream_window_build capture;
    bool is_capture_done;
    /* The open node (glossary `open_node`): an element (or the document
       node, depth 0) of a shallow window whose content is read next;
       is_consumed once a STREAM reads it. Owned. */
    bool has_open_node;
    sxml_item open_node;
    size_t open_depth;
    bool is_open_node_consumed;
} sxml_stream_document;

/* Unparsed entities of the document node, kept for every window. */
typedef struct sxml_stream_unparsed_entity {
    sxml_byte_range entity_name;
    sxml_byte_range system_identifier;
    sxml_byte_range public_identifier;
} sxml_stream_unparsed_entity;

static sxml_stream_element *sxml_stream_element_at(const sxml_stream_document *document,
                                                   size_t element_index) {
    return sxml_xml_array_at(&document->elements, element_index);
}

static size_t sxml_stream_depth(const sxml_stream_document *document) {
    return document->elements.element_count;
}

/* Keeps bytes in buffer and writes where they went to *range. */
static sxml_status sxml_stream_keep(sxml_xml_buffer *buffer, sxml_byte_view bytes,
                                    sxml_byte_range *range) {
    range->byte_offset = buffer->byte_count;
    range->byte_count = bytes.byte_count;
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_buffer_append(buffer, bytes);
}

/* ------------------------------------------------------ Window building */

/* Starts a window: the document node and the open elements up to
   element_count, with their bindings and attributes. */
static sxml_status sxml_stream_begin_window(sxml_stream_document *document, size_t element_count,
                                            sxml_stream_window_build *build) {
    sxml_processor *processor = document->processor;
    *build = (sxml_stream_window_build){.arena_tree = NULL,
                                        .builder = {.state = NULL, .operations = NULL},
                                        .next_node_index = 0,
                                        .last_element_index = 0};
    sxml_status status = sxml_arena_tree_create(
        document->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = document->allocator,
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
        &build->arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    build->builder = sxml_arena_tree_as_tree_builder(build->arena_tree);
    const sxml_tree_builder builder = build->builder;
    status = builder.operations->start_document(
        builder.state,
        &(sxml_document_start_event){
            .document_uri = sxml_xml_buffer_view(&document->document_text, document->document_uri),
            .base_uri = sxml_xml_buffer_view(&document->document_text, document->base_uri),
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}});
    for (size_t entity_index = 0;
         status == SXML_STATUS_OK && entity_index < document->unparsed_entities.element_count;
         entity_index += 1) {
        const sxml_stream_unparsed_entity *entity =
            sxml_xml_array_at(&document->unparsed_entities, entity_index);
        status = builder.operations->add_unparsed_entity(
            builder.state,
            &(sxml_unparsed_entity){
                .entity_name = sxml_xml_buffer_view(&document->document_text, entity->entity_name),
                .system_identifier =
                    sxml_xml_buffer_view(&document->document_text, entity->system_identifier),
                .public_identifier =
                    sxml_xml_buffer_view(&document->document_text, entity->public_identifier)});
    }
    build->next_node_index = 1;
    for (size_t element_index = 0; status == SXML_STATUS_OK && element_index < element_count;
         element_index += 1) {
        const sxml_stream_element *element = sxml_stream_element_at(document, element_index);
        sxml_element_start_event start = element->event;
        start.entity_base_uri =
            sxml_xml_buffer_view(&document->ancestor_text, element->entity_base_uri);
        status = builder.operations->start_element(builder.state, &start);
        for (size_t binding_index = 0;
             status == SXML_STATUS_OK && binding_index < element->binding_count;
             binding_index += 1) {
            const sxml_namespace_binding *binding =
                sxml_xml_array_at(&document->bindings, element->first_binding + binding_index);
            status = builder.operations->add_namespace_binding(builder.state, binding);
        }
        for (size_t attribute_index = 0;
             status == SXML_STATUS_OK && attribute_index < element->attribute_count;
             attribute_index += 1) {
            const sxml_stream_attribute *attribute = sxml_xml_array_at(
                &document->attributes, element->first_attribute + attribute_index);
            sxml_attribute_event event = attribute->event;
            event.attribute_value =
                sxml_xml_buffer_view(&document->ancestor_text, attribute->value);
            status = builder.operations->add_attribute(builder.state, &event);
        }
        build->last_element_index = build->next_node_index;
        build->next_node_index += 1U + (uint32_t)element->attribute_count;
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(build->arena_tree);
        build->arena_tree = NULL;
    }
    return status;
}

/* Ends open_count elements and the document, and writes the node of the
   window at node_index, owned, to *node: in the prepared copy when the
   document is prepared, or no item (representation 0) when preparation
   removed it (a whitespace text node). */
typedef struct sxml_stream_window_end {
    sxml_stream_window_build *build;
    size_t open_count;
    uint32_t node_index;
} sxml_stream_window_end;

static sxml_status sxml_stream_end_window(sxml_stream_document *document,
                                          sxml_stream_window_end end, sxml_item *node) {
    sxml_stream_window_build *build = end.build;
    *node = (sxml_item){.representation = 0};
    const sxml_tree_builder builder = build->builder;
    sxml_status status = SXML_STATUS_OK;
    for (size_t open_index = 0; status == SXML_STATUS_OK && open_index < end.open_count;
         open_index += 1) {
        status = builder.operations->end_element(builder.state);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(build->arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(build->arena_tree);
        build->arena_tree = NULL;
        return status;
    }
    build->arena_tree = NULL;
    sxml_item document_node = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    const sxml_document_preparation preparation = document->source.preparation;
    if (preparation.operations != NULL) {
        sxml_item prepared = {.representation = 0};
        status =
            preparation.operations->prepare_document(preparation.state, document_node, &prepared);
        sxml_item_release(document_node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        document_node = prepared;
        sxml_node_reference prepared_node;
        sxml_tree_description after;
        status = sxml_item_get_node(document_node, &prepared_node);
        if (status == SXML_STATUS_OK) {
            status = sxml_tree_describe(prepared_node.tree, &after);
        }
        if (status != SXML_STATUS_OK) {
            sxml_item_release(document_node);
            return status;
        }
        if (after.node_count <= end.node_index) {
            /* The node was a whitespace text node it removed (4.2). */
            sxml_item_release(document_node);
            return SXML_STATUS_OK;
        }
        tree = prepared_node.tree;
    }
    *node = sxml_node_item_make((sxml_node_reference){
        .tree = tree, .node_index = {.index = end.node_index}, .namespace_number = 0});
    sxml_item_release(document_node);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- The builder */

static sxml_stream_document *sxml_stream_document_from_state(void *state) {
    return state;
}

static sxml_status sxml_stream_queue(sxml_stream_document *document, sxml_stream_event event) {
    if (document->queue_count == STREAM_QUEUE_CAPACITY) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    const size_t slot = (document->queue_start + document->queue_count) % STREAM_QUEUE_CAPACITY;
    document->queue[slot] = event;
    document->queue_count += 1;
    sxml_xml_incremental_parse_pause(document->parse);
    return SXML_STATUS_OK;
}

/* Ends the pending text node: queued, or sent to the capture. */
static sxml_status sxml_stream_flush_text(sxml_stream_document *document) {
    if (!document->has_pending_text) {
        return SXML_STATUS_OK;
    }
    document->has_pending_text = false;
    if (document->mode == SXML_STREAM_MODE_NOTIFY) {
        return sxml_stream_queue(document, document->pending_text);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_stream_on_start_document(void *state,
                                                 const sxml_document_start_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status =
        sxml_stream_keep(&document->document_text, event->document_uri, &document->document_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_keep(&document->document_text, event->base_uri, &document->base_uri);
    }
    return status;
}

static sxml_status sxml_stream_on_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&document->unparsed_entities, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_unparsed_entity *kept = element;
    status = sxml_stream_keep(&document->document_text, entity->entity_name, &kept->entity_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_keep(&document->document_text, entity->system_identifier,
                                  &kept->system_identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_keep(&document->document_text, entity->public_identifier,
                                  &kept->public_identifier);
    }
    return status;
}

static sxml_status sxml_stream_on_end_document(void *state) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status = sxml_stream_flush_text(document);
    if (status == SXML_STATUS_OK && document->mode == SXML_STREAM_MODE_NOTIFY) {
        status = sxml_stream_queue(
            document, (sxml_stream_event){.event_kind = SXML_STREAM_EVENT_END_DOCUMENT});
    }
    return status;
}

static sxml_status sxml_stream_on_start_element(void *state,
                                                const sxml_element_start_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status = sxml_stream_flush_text(document);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&document->elements, &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_element *element = memory;
    element->event = *event;
    element->event.entity_base_uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    element->text_mark = document->ancestor_text.byte_count;
    element->first_binding = document->bindings.element_count;
    element->binding_count = 0;
    element->first_attribute = document->attributes.element_count;
    element->attribute_count = 0;
    status = sxml_stream_keep(&document->ancestor_text, event->entity_base_uri,
                              &element->entity_base_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->start_element(document->capture.builder.state,
                                                                   event);
    }
    if (document->mode == SXML_STREAM_MODE_NOTIFY) {
        return sxml_stream_queue(document,
                                 (sxml_stream_event){.event_kind = SXML_STREAM_EVENT_START_ELEMENT,
                                                     .location = event->location});
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_stream_on_namespace_binding(void *state,
                                                    const sxml_namespace_binding *binding) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    void *memory = NULL;
    sxml_status status = sxml_xml_array_push(&document->bindings, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(sxml_namespace_binding *)memory = *binding;
    sxml_stream_element_at(document, sxml_stream_depth(document) - 1)->binding_count += 1;
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->add_namespace_binding(
            document->capture.builder.state, binding);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_stream_on_attribute(void *state, const sxml_attribute_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    void *memory = NULL;
    sxml_status status = sxml_xml_array_push(&document->attributes, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_attribute *attribute = memory;
    attribute->event = *event;
    attribute->event.attribute_value = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    status = sxml_stream_keep(&document->ancestor_text, event->attribute_value, &attribute->value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_element_at(document, sxml_stream_depth(document) - 1)->attribute_count += 1;
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->add_attribute(document->capture.builder.state,
                                                                   event);
    }
    return SXML_STATUS_OK;
}

/* Removes the element on top of the open elements. */
static void sxml_stream_pop_element(sxml_stream_document *document) {
    const sxml_stream_element *element =
        sxml_stream_element_at(document, sxml_stream_depth(document) - 1);
    document->bindings.element_count = element->first_binding;
    document->attributes.element_count = element->first_attribute;
    document->ancestor_text.byte_count = element->text_mark;
    document->elements.element_count -= 1;
}

static sxml_status sxml_stream_on_end_element(void *state) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status = sxml_stream_flush_text(document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (document->mode) {
    case SXML_STREAM_MODE_NOTIFY:
        return sxml_stream_queue(document,
                                 (sxml_stream_event){.event_kind = SXML_STREAM_EVENT_END_ELEMENT});
    case SXML_STREAM_MODE_CAPTURE:
        status = document->capture.builder.operations->end_element(document->capture.builder.state);
        if (status == SXML_STATUS_OK && sxml_stream_depth(document) == document->mode_depth) {
            document->is_capture_done = true;
            document->mode = SXML_STREAM_MODE_NOTIFY;
            sxml_xml_incremental_parse_pause(document->parse);
        }
        sxml_stream_pop_element(document);
        return status;
    case SXML_STREAM_MODE_SKIP:
        if (sxml_stream_depth(document) == document->mode_depth) {
            document->mode = SXML_STREAM_MODE_NOTIFY;
            sxml_xml_incremental_parse_pause(document->parse);
        }
        sxml_stream_pop_element(document);
        return SXML_STATUS_OK;
    case SXML_STREAM_MODE_DRAIN:
        sxml_stream_pop_element(document);
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status sxml_stream_on_text(void *state, const sxml_text_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->add_text(document->capture.builder.state,
                                                              event);
    }
    if (document->mode != SXML_STREAM_MODE_NOTIFY || event->text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (!document->has_pending_text) {
        document->has_pending_text = true;
        document->pending_text = (sxml_stream_event){
            .event_kind = SXML_STREAM_EVENT_TEXT,
            .text = {.byte_offset = document->event_text.byte_count, .byte_count = 0},
            .is_cdata_section = false,
            .location = event->location};
    }
    document->pending_text.is_cdata_section =
        document->pending_text.is_cdata_section || event->is_cdata_section;
    document->pending_text.text.byte_count += event->text.byte_count;
    return sxml_xml_buffer_append(&document->event_text, event->text);
}

static sxml_status sxml_stream_on_comment(void *state, const sxml_comment_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status = sxml_stream_flush_text(document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->add_comment(document->capture.builder.state,
                                                                 event);
    }
    if (document->mode != SXML_STREAM_MODE_NOTIFY) {
        return SXML_STATUS_OK;
    }
    sxml_stream_event queued = {.event_kind = SXML_STREAM_EVENT_COMMENT,
                                .location = event->location};
    status = sxml_stream_keep(&document->event_text, event->comment, &queued.text);
    return status == SXML_STATUS_OK ? sxml_stream_queue(document, queued) : status;
}

static sxml_status
sxml_stream_on_processing_instruction(void *state, const sxml_processing_instruction_event *event) {
    sxml_stream_document *document = sxml_stream_document_from_state(state);
    sxml_status status = sxml_stream_flush_text(document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (document->mode == SXML_STREAM_MODE_CAPTURE) {
        return document->capture.builder.operations->add_processing_instruction(
            document->capture.builder.state, event);
    }
    if (document->mode != SXML_STREAM_MODE_NOTIFY) {
        return SXML_STATUS_OK;
    }
    sxml_stream_event queued = {.event_kind = SXML_STREAM_EVENT_PROCESSING_INSTRUCTION,
                                .location = event->location};
    status = sxml_stream_keep(&document->event_text, event->target, &queued.text);
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_keep(&document->event_text, event->data, &queued.data);
    }
    return status == SXML_STATUS_OK ? sxml_stream_queue(document, queued) : status;
}

static const sxml_tree_builder_operations sxml_stream_builder_operations = {
    .start_document = sxml_stream_on_start_document,
    .end_document = sxml_stream_on_end_document,
    .start_element = sxml_stream_on_start_element,
    .add_namespace_binding = sxml_stream_on_namespace_binding,
    .add_attribute = sxml_stream_on_attribute,
    .end_element = sxml_stream_on_end_element,
    .add_text = sxml_stream_on_text,
    .add_comment = sxml_stream_on_comment,
    .add_processing_instruction = sxml_stream_on_processing_instruction,
    .add_unparsed_entity = sxml_stream_on_unparsed_entity,
};

/* ------------------------------------------------------- Reading events */

/* Resumes the parse once; its failures become FODC0002 when they say the
   document is not well-formed. */
static sxml_status sxml_stream_resume(sxml_dynamic_context *dynamic_context,
                                      sxml_stream_document *document) {
    if (document->is_finished) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status status =
        sxml_xml_incremental_parse_resume(document->parse, &document->is_finished);
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_stream_not_well_formed_error);
    }
    return status;
}

/* Takes the next queued event into *event, resuming the parse as needed;
   an END_ELEMENT removes its element. */
static sxml_status sxml_stream_next_event(sxml_dynamic_context *dynamic_context,
                                          sxml_stream_document *document,
                                          sxml_stream_event *event) {
    while (document->queue_count == 0) {
        if (document->is_finished) {
            *event = (sxml_stream_event){.event_kind = SXML_STREAM_EVENT_END_DOCUMENT};
            return SXML_STATUS_OK;
        }
        document->event_text.byte_count =
            document->has_pending_text ? document->event_text.byte_count : 0;
        const sxml_status status = sxml_stream_resume(dynamic_context, document);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *event = document->queue[document->queue_start];
    document->queue_start = (document->queue_start + 1U) % STREAM_QUEUE_CAPACITY;
    document->queue_count -= 1;
    if (event->event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
        sxml_stream_pop_element(document);
    }
    return SXML_STATUS_OK;
}

/* Reads on until the element count is below depth: the rest of an
   element's content nobody reads (XSLT 4.0, 19: what a body did not
   consume). */
static sxml_status sxml_stream_skip_to(sxml_dynamic_context *dynamic_context,
                                       sxml_stream_document *document, size_t depth) {
    while (document->queue_count > 0 && sxml_stream_depth(document) >= depth) {
        sxml_stream_event event;
        const sxml_status status = sxml_stream_next_event(dynamic_context, document, &event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (sxml_stream_depth(document) < depth) {
        return SXML_STATUS_OK;
    }
    document->mode = SXML_STREAM_MODE_SKIP;
    document->mode_depth = depth;
    while (document->mode == SXML_STREAM_MODE_SKIP) {
        const sxml_status status = sxml_stream_resume(dynamic_context, document);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The window of the element on top of the open elements, whose start was
   just taken: shallow (without its content) or full (its subtree, read
   now). */
typedef struct sxml_stream_element_request {
    sxml_stream_document *document;
    bool is_full;
} sxml_stream_element_request;

static sxml_status sxml_stream_element_window(sxml_dynamic_context *dynamic_context,
                                              sxml_stream_element_request request,
                                              sxml_item *node) {
    sxml_stream_document *document = request.document;
    const bool is_full = request.is_full;
    const size_t depth = sxml_stream_depth(document);
    sxml_stream_window_build build;
    sxml_status status = sxml_stream_begin_window(document, depth, &build);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t node_index = build.last_element_index;
    size_t open_count = depth;
    if (is_full) {
        if (document->queue_count > 0 &&
            document->queue[document->queue_start].event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
            /* An empty element: its end came with its start, and the
               window still has it open. */
            sxml_stream_event event;
            status = sxml_stream_next_event(dynamic_context, document, &event);
            open_count = depth + 1U;
        } else {
            document->capture = build;
            document->is_capture_done = false;
            document->mode = SXML_STREAM_MODE_CAPTURE;
            document->mode_depth = depth;
            while (status == SXML_STATUS_OK && !document->is_capture_done) {
                status = sxml_stream_resume(dynamic_context, document);
            }
            document->mode = SXML_STREAM_MODE_NOTIFY;
            build = document->capture;
            document->capture.arena_tree = NULL;
        }
        if (status != SXML_STATUS_OK) {
            sxml_arena_tree_destroy(build.arena_tree);
            return status;
        }
        /* The element's own end went to the window (or comes now). */
        open_count -= 1U;
    }
    return sxml_stream_end_window(document,
                                  (sxml_stream_window_end){.build = &build,
                                                           .open_count = open_count,
                                                           .node_index = node_index},
                                  node);
}

/* The window of a text, comment or processing instruction just taken. */
static sxml_status sxml_stream_leaf_window(sxml_stream_document *document,
                                           const sxml_stream_event *event, sxml_item *node) {
    const size_t depth = sxml_stream_depth(document);
    sxml_stream_window_build build;
    sxml_status status = sxml_stream_begin_window(document, depth, &build);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t node_index = build.next_node_index;
    const sxml_tree_builder builder = build.builder;
    const sxml_byte_view text = sxml_xml_buffer_view(&document->event_text, event->text);
    if (event->event_kind == SXML_STREAM_EVENT_TEXT) {
        status = builder.operations->add_text(
            builder.state, &(sxml_text_event){.text = text,
                                              .location = event->location,
                                              .is_cdata_section = event->is_cdata_section,
                                              .disables_output_escaping = false});
    } else if (event->event_kind == SXML_STREAM_EVENT_COMMENT) {
        status = builder.operations->add_comment(
            builder.state, &(sxml_comment_event){.comment = text, .location = event->location});
    } else {
        status = builder.operations->add_processing_instruction(
            builder.state, &(sxml_processing_instruction_event){
                               .target = text,
                               .data = sxml_xml_buffer_view(&document->event_text, event->data),
                               .location = event->location});
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(build.arena_tree);
        return status;
    }
    return sxml_stream_end_window(
        document,
        (sxml_stream_window_end){.build = &build, .open_count = depth, .node_index = node_index},
        node);
}

/* ------------------------------------------------- Opening and closing */

static void sxml_stream_document_destroy(sxml_stream_document *document) {
    if (document == NULL) {
        return;
    }
    sxml_xml_incremental_parse_destroy(document->parse);
    sxml_xml_parser_destroy(document->parser);
    if (document->capture.arena_tree != NULL) {
        sxml_arena_tree_destroy(document->capture.arena_tree);
    }
    if (document->loader != NULL) {
        sxml_processor_document_loader_close_streamed(document->loader, &document->source);
    }
    sxml_item_release(document->open_node);
    sxml_xml_buffer_release(&document->document_text);
    sxml_xml_array_release(&document->unparsed_entities);
    sxml_xml_array_release(&document->elements);
    sxml_xml_array_release(&document->bindings);
    sxml_xml_array_release(&document->attributes);
    sxml_xml_buffer_release(&document->ancestor_text);
    sxml_xml_buffer_release(&document->event_text);
    sxml_allocator_release(document->allocator, document);
}

static sxml_xml_array sxml_stream_array(sxml_allocator allocator, size_t element_byte_count) {
    return sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = element_byte_count});
}

/* Opens the document at uri_reference (resolved against the static base
   URI) into a new *document; *document is NULL, with no failure, when the
   evaluation has no processor's loader. *document->source says whether it
   is streamed. */
static sxml_status sxml_stream_document_open(sxml_dynamic_context *dynamic_context,
                                             sxml_byte_view uri_reference,
                                             sxml_stream_document **document) {
    *document = NULL;
    sxml_processor_document_loader *loader =
        sxml_processor_document_loader_from_dynamic_context(dynamic_context);
    if (loader == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_processor *processor = sxml_processor_document_loader_get_environment(loader)->processor;
    const sxml_allocator allocator = processor->allocator;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_stream_document),
                                  .alignment = alignof(sxml_stream_document)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_document *new_document = memory;
    memset(new_document, 0, sizeof(*new_document));
    new_document->allocator = allocator;
    new_document->processor = processor;
    new_document->open_node = (sxml_item){.representation = 0};
    new_document->document_text = sxml_xml_buffer_empty(allocator);
    new_document->ancestor_text = sxml_xml_buffer_empty(allocator);
    new_document->event_text = sxml_xml_buffer_empty(allocator);
    new_document->unparsed_entities =
        sxml_stream_array(allocator, sizeof(sxml_stream_unparsed_entity));
    new_document->elements = sxml_stream_array(allocator, sizeof(sxml_stream_element));
    new_document->bindings = sxml_stream_array(allocator, sizeof(sxml_namespace_binding));
    new_document->attributes = sxml_stream_array(allocator, sizeof(sxml_stream_attribute));
    new_document->mode = SXML_STREAM_MODE_NOTIFY;
    status = sxml_processor_document_loader_open_streamed(
        loader,
        &(sxml_streamed_document_request){
            .uri_reference = uri_reference,
            .base_uri = sxml_dynamic_context_get_static_base_uri(dynamic_context),
            .error_record = dynamic_context->error_record},
        &new_document->source);
    new_document->loader = loader;
    if (status == SXML_STATUS_OK && new_document->source.is_streamed) {
        status = sxml_xml_parser_create(
            allocator,
            &(sxml_xml_parser_configuration){.allocator = allocator,
                                             .name_pool = processor->name_pool,
                                             .limits = processor->limits.xml_limits},
            &new_document->parser);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_incremental_parse_create(
                &(sxml_xml_incremental_parse_request){
                    .allocator = allocator,
                    .parser = new_document->parser,
                    .parse_request = &new_document->source.parse_request,
                    .builder = {.state = new_document,
                                .operations = &sxml_stream_builder_operations}},
                &new_document->parse);
        }
        if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
            status == SXML_STATUS_UNSUPPORTED_FEATURE) {
            status =
                sxml_evaluator_machine_raise(dynamic_context, &sxml_stream_not_well_formed_error);
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_stream_document_destroy(new_document);
        return status;
    }
    *document = new_document;
    return SXML_STATUS_OK;
}

/* Makes item the open node at depth (owned). */
static void sxml_stream_set_open_node(sxml_stream_document *document, sxml_item item,
                                      size_t depth) {
    sxml_item_release(document->open_node);
    document->open_node = item;
    document->has_open_node = true;
    document->open_depth = depth;
    document->is_open_node_consumed = false;
}

/* --------------------------------------------------------- Node tests */

/* The node test of a STREAM, read once. */
typedef struct sxml_stream_test {
    /* False for a test sxml matches through the type registry. */
    bool is_simple;
    sxml_node_test node_test;
    sxml_type_identifier type;
} sxml_stream_test;

static sxml_status sxml_stream_resolve_test(const sxml_dynamic_context *dynamic_context,
                                            sxml_type_identifier type, sxml_stream_test *test) {
    *test = (sxml_stream_test){.is_simple = false, .type = type};
    if (type.index == SXML_TYPE_GENERIC_NODE || type.index == SXML_TYPE_NODE) {
        test->is_simple = true;
        test->node_test.node_kind = SXML_NODE_KIND_ANY;
        test->node_test.name_test.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME;
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    const sxml_status status = sxml_type_registry_describe_type(
        sxml_item_heap_get_type_registry(dynamic_context->item_heap), type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.variety == SXML_TYPE_VARIETY_NODE_TEST) {
        const sxml_node_test *node_test = &description.variety_details.node_test;
        test->node_test = *node_test;
        test->is_simple = node_test->type_annotation.index == SXML_TYPE_NONE &&
                          !node_test->is_schema_declaration &&
                          node_test->document_element_test.index == SXML_TYPE_NONE;
    }
    return SXML_STATUS_OK;
}

/* Whether a node of kind and name matches a simple test (XPath 4.0,
   4.6.5). */
typedef struct sxml_stream_candidate_kind {
    const sxml_stream_test *test;
    sxml_node_kind node_kind;
    sxml_name_identifier node_name;
} sxml_stream_candidate_kind;

static sxml_status sxml_stream_test_matches(const sxml_dynamic_context *dynamic_context,
                                            sxml_stream_candidate_kind candidate, bool *matches) {
    const sxml_stream_test *test = candidate.test;
    const sxml_node_test *node_test = &test->node_test;
    *matches =
        node_test->node_kind == SXML_NODE_KIND_ANY || node_test->node_kind == candidate.node_kind;
    if (!*matches || node_test->node_kind == SXML_NODE_KIND_ANY) {
        return SXML_STATUS_OK;
    }
    const sxml_name_test *name_test = &node_test->name_test;
    if (name_test->name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME) {
        return SXML_STATUS_OK;
    }
    if (name_test->name_test_kind == SXML_NAME_TEST_KIND_EXPANDED_NAME) {
        *matches = name_test->expanded_name.index == candidate.node_name.index;
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts;
    const sxml_status status = sxml_name_pool_get_name_parts(
        sxml_item_heap_get_name_pool(dynamic_context->item_heap), candidate.node_name, &parts);
    if (status != SXML_STATUS_OK) {
        *matches = false;
        return status;
    }
    if (name_test->name_test_kind == SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD) {
        *matches = parts.namespace_uri.index == name_test->namespace_uri.index;
    } else {
        *matches = parts.local_name.byte_count == name_test->local_name.byte_count &&
                   (parts.local_name.byte_count == 0 ||
                    memcmp(parts.local_name.bytes, name_test->local_name.bytes,
                           parts.local_name.byte_count) == 0);
    }
    return SXML_STATUS_OK;
}

/* Whether node (an item, for a test that is not simple) matches. */
typedef struct sxml_stream_node_question {
    const sxml_stream_test *test;
    sxml_item node_item;
} sxml_stream_node_question;

static sxml_status sxml_stream_node_matches(sxml_dynamic_context *dynamic_context,
                                            sxml_stream_node_question question, bool *matches) {
    const sxml_stream_test *test = question.test;
    const sxml_item node_item = question.node_item;
    *matches = false;
    if (!test->is_simple) {
        sxml_type_match match = {.is_match = false};
        const sxml_status status = sxml_evaluator_match_sequence_type(
            dynamic_context,
            &(sxml_type_match_request){.value = sxml_sequence_from_item(node_item),
                                       .sequence_type = {.item_type = test->type,
                                                         .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                                       .error_record = dynamic_context->error_record},
            &match);
        *matches = match.is_match;
        return status;
    }
    sxml_node_reference node;
    sxml_status status = sxml_item_get_node(node_item, &node);
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_stream_test_matches(
        dynamic_context,
        (sxml_stream_candidate_kind){
            .test = test, .node_kind = description.node_kind, .node_name = description.node_name},
        matches);
}

/* ---------------------------------------------------------- The task */

typedef enum sxml_stream_phase {
    SXML_STREAM_PHASE_START = 0,
    /* Looks for the next candidate, or finishes. */
    SXML_STREAM_PHASE_NEXT,
    /* The predicate of the candidate ended. */
    SXML_STREAM_PHASE_PREDICATE,
    /* The body of the selected node ended. */
    SXML_STREAM_PHASE_BODY,
} sxml_stream_phase;

/* Where the candidate comes from. */
typedef enum sxml_stream_candidate_source {
    /* An element of the events, still open: a shallow window. */
    SXML_STREAM_CANDIDATE_OPEN_ELEMENT,
    /* An element of the events whose subtree is in a full window. */
    SXML_STREAM_CANDIDATE_CAPTURED_ELEMENT,
    /* A text, comment or processing instruction of the events, or a node
       of a tree (tree_iterator). */
    SXML_STREAM_CANDIDATE_LEAF,
} sxml_stream_candidate_source;

typedef struct sxml_stream_state {
    const sxml_ir_stream *stream;
    sxml_stream_test test;
    /* The streamed document of an event STREAM, owned when owns_document
       (the OPENS_DOCUMENT task's); NULL when the STREAM walks a tree. */
    sxml_stream_document *document;
    bool owns_document;
    /* The element count while origin is the open element. */
    size_t base_depth;
    /* Owned: the origin, or the document node of an unstreamed
       xsl:source-document. */
    sxml_item origin;
    /* Walking a tree: from origin, or inside the full window of a
       DESCENDANT candidate (then back to the events at its end). */
    bool walks_tree;
    sxml_axis_iterator tree_iterator;
    /* Owned while it is walked: the window the tree iterator is in. */
    sxml_item tree_window;
    /* The candidate, owned, and where it is. */
    sxml_item candidate;
    sxml_stream_candidate_source candidate_source;
    size_t candidate_depth;
    uint64_t step_position;
    uint64_t selected_count;
    /* Owned; made on the first value. */
    sxml_sequence_builder *builder;
} sxml_stream_state;

static void sxml_stream_release_state(sxml_dynamic_context *dynamic_context, void *state) {
    sxml_stream_state *stream_state = state;
    sxml_item_release(stream_state->origin);
    sxml_item_release(stream_state->tree_window);
    sxml_item_release(stream_state->candidate);
    sxml_sequence_builder_destroy(stream_state->builder);
    if (stream_state->owns_document) {
        sxml_stream_document_destroy(stream_state->document);
    }
    sxml_allocator_release(dynamic_context->allocator, stream_state);
}

static sxml_status sxml_stream_step(sxml_dynamic_context *dynamic_context,
                                    sxml_evaluator_task *task);

/* The streamed document whose open node origin is, found on the STREAM
   tasks below the top; NULL when origin is not an open node. */
static sxml_stream_document *sxml_stream_find_document(const sxml_dynamic_context *dynamic_context,
                                                       sxml_item origin) {
    sxml_node_reference origin_node;
    if (sxml_item_get_node(origin, &origin_node) != SXML_STATUS_OK) {
        return NULL;
    }
    const sxml_evaluator_task_stack *tasks = &dynamic_context->task_stack;
    for (size_t task_index = tasks->task_count; task_index > 0; task_index -= 1) {
        const sxml_evaluator_task *below = &tasks->tasks[task_index - 1U];
        if (below->task_kind != SXML_EVALUATOR_TASK_KIND_PART ||
            below->details.part.step != sxml_stream_step || below->details.part.state == NULL) {
            continue;
        }
        const sxml_stream_state *state = below->details.part.state;
        sxml_stream_document *document = state->document;
        sxml_node_reference open_node;
        if (document != NULL && document->has_open_node && !document->is_open_node_consumed &&
            sxml_item_get_node(document->open_node, &open_node) == SXML_STATUS_OK &&
            sxml_node_is_same_node(open_node, origin_node)) {
            return document;
        }
    }
    return NULL;
}

static const sxml_ir_stream *sxml_stream_get_expression(const sxml_evaluator_task *task) {
    return &sxml_ir_unit_get_expression(task->function->unit, task->expression)->stream;
}

/* Appends value (owned) to the result, or delivers it (D166). */
static sxml_status sxml_stream_append(sxml_dynamic_context *dynamic_context,
                                      sxml_stream_state *state, sxml_sequence value) {
    if (sxml_evaluator_machine_top(dynamic_context)->delivers_content) {
        return sxml_evaluator_machine_deliver(dynamic_context, value);
    }
    sxml_status status = SXML_STATUS_OK;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        if (state->builder == NULL) {
            status = sxml_sequence_builder_create(
                dynamic_context->allocator,
                &(sxml_sequence_builder_configuration){.item_heap = dynamic_context->item_heap},
                &state->builder);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(state->builder, value,
                                                           dynamic_context->error_record);
        }
    }
    sxml_sequence_release(value);
    return status;
}

static sxml_status sxml_stream_finish(sxml_dynamic_context *dynamic_context,
                                      sxml_stream_state *state) {
    sxml_sequence result = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    if (state->builder != NULL) {
        status = sxml_sequence_builder_finish(state->builder, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_evaluator_machine_finish(dynamic_context, result);
}

/* Binds the focus to the candidate at position and pushes expression
   (the predicate, or the body, which delivers when the task does). */
typedef struct sxml_stream_push {
    sxml_ir_expression_index expression;
    uint64_t position;
    sxml_stream_phase phase;
} sxml_stream_push;

static sxml_status sxml_stream_push_focused(sxml_dynamic_context *dynamic_context,
                                            sxml_stream_state *state, sxml_stream_push push) {
    const sxml_ir_focus focus = state->stream->focus;
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->phase = (uint32_t)push.phase;
    if (focus.context_value.value != 0) {
        sxml_item_retain(state->candidate);
        sxml_evaluator_machine_bind_slot(dynamic_context, focus.context_value,
                                         sxml_sequence_from_item(state->candidate));
    }
    if (focus.context_position.value != 0) {
        sxml_evaluator_machine_bind_slot(
            dynamic_context, focus.context_position,
            sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)push.position)));
    }
    if (push.phase == SXML_STREAM_PHASE_BODY && task->delivers_content) {
        return sxml_evaluator_machine_push_content(dynamic_context, push.expression);
    }
    return sxml_evaluator_machine_push_expression(dynamic_context, push.expression);
}

/* The position the body sees: the next of the path's counter, or of the
   step's selected nodes. */
static sxml_status sxml_stream_next_position(sxml_dynamic_context *dynamic_context,
                                             sxml_stream_state *state, uint64_t *position) {
    state->selected_count += 1;
    *position = state->selected_count;
    const sxml_ir_slot counter = state->stream->position_counter;
    if (counter.value == 0) {
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = sxml_evaluator_machine_read_atom(
        dynamic_context, (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = counter},
        &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    int64_t count = 0;
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
        sxml_item_get_representation(value.content) == SXML_ITEM_REPRESENTATION_INTEGER) {
        count = value.content.payload.integer;
    }
    count += 1;
    *position = (uint64_t)count;
    sxml_evaluator_machine_bind_slot(dynamic_context, counter,
                                     sxml_sequence_from_item(sxml_atomic_item_from_integer(count)));
    return SXML_STATUS_OK;
}

/* The candidate is selected: its window as the body needs it, then the
   body. */
static sxml_status sxml_stream_select(sxml_dynamic_context *dynamic_context,
                                      sxml_stream_state *state) {
    const bool is_shallow = (state->stream->flags & (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS) != 0;
    if (state->candidate_source == SXML_STREAM_CANDIDATE_OPEN_ELEMENT) {
        if (is_shallow) {
            sxml_item_retain(state->candidate);
            sxml_stream_set_open_node(state->document, state->candidate, state->candidate_depth);
        } else {
            sxml_item_release(state->candidate);
            state->candidate = (sxml_item){.representation = 0};
            const sxml_status status = sxml_stream_element_window(
                dynamic_context,
                (sxml_stream_element_request){.document = state->document, .is_full = true},
                &state->candidate);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            state->candidate_source = SXML_STREAM_CANDIDATE_CAPTURED_ELEMENT;
        }
    }
    uint64_t position = 0;
    const sxml_status status = sxml_stream_next_position(dynamic_context, state, &position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_stream_push_focused(dynamic_context, state,
                                    (sxml_stream_push){.expression = state->stream->body,
                                                       .position = position,
                                                       .phase = SXML_STREAM_PHASE_BODY});
}

/* A candidate was found: its predicate, or it is selected. */
static sxml_status sxml_stream_consider(sxml_dynamic_context *dynamic_context,
                                        sxml_stream_state *state) {
    state->step_position += 1;
    if (state->stream->predicate.value != 0) {
        return sxml_stream_push_focused(dynamic_context, state,
                                        (sxml_stream_push){.expression = state->stream->predicate,
                                                           .position = state->step_position,
                                                           .phase = SXML_STREAM_PHASE_PREDICATE});
    }
    return sxml_stream_select(dynamic_context, state);
}

/* After a candidate element of the events that is not taken (its test or
   predicate failed): a child's content is skipped, a descendant's is read
   on (its descendants are candidates too). */
static sxml_status sxml_stream_pass_element(sxml_dynamic_context *dynamic_context,
                                            sxml_stream_state *state) {
    if (state->stream->axis == SXML_AXIS_DESCENDANT) {
        return SXML_STATUS_OK;
    }
    return sxml_stream_skip_to(dynamic_context, state->document, state->candidate_depth);
}

/* The next node of the tree walk that matches the test; false at its end. */
static sxml_status sxml_stream_next_tree_candidate(sxml_dynamic_context *dynamic_context,
                                                   sxml_stream_state *state, bool *has_candidate) {
    *has_candidate = false;
    for (;;) {
        sxml_node_iteration_step step = {.has_node = false};
        sxml_status status = sxml_axis_iterator_next(&state->tree_iterator, &step);
        if (status != SXML_STATUS_OK || !step.has_node) {
            return status;
        }
        sxml_item item = sxml_node_item_make(step.node);
        bool matches = false;
        status = sxml_stream_node_matches(
            dynamic_context, (sxml_stream_node_question){.test = &state->test, .node_item = item},
            &matches);
        if (status != SXML_STATUS_OK || !matches) {
            sxml_item_release(item);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        state->candidate = item;
        state->candidate_source = SXML_STREAM_CANDIDATE_LEAF;
        *has_candidate = true;
        return SXML_STATUS_OK;
    }
}

/* Reads events to the next candidate of the step, or finishes the task at
   the end of origin's content. */
static sxml_status sxml_stream_next_event_candidate(sxml_dynamic_context *dynamic_context,
                                                    sxml_stream_state *state, bool *has_candidate) {
    *has_candidate = false;
    sxml_stream_document *document = state->document;
    for (;;) {
        sxml_stream_event event;
        sxml_status status = sxml_stream_next_event(dynamic_context, document, &event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const size_t depth = sxml_stream_depth(document);
        switch (event.event_kind) {
        case SXML_STREAM_EVENT_NONE:
            return SXML_STATUS_INVALID_ARGUMENT;
        case SXML_STREAM_EVENT_END_DOCUMENT:
            return SXML_STATUS_OK;
        case SXML_STREAM_EVENT_END_ELEMENT:
            if (depth < state->base_depth) {
                /* Origin's own end. */
                return SXML_STATUS_OK;
            }
            continue;
        case SXML_STREAM_EVENT_START_ELEMENT: {
            if (state->stream->axis == SXML_AXIS_CHILD && depth != state->base_depth + 1U) {
                status = sxml_stream_skip_to(dynamic_context, document, depth);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                continue;
            }
            const sxml_stream_element *element = sxml_stream_element_at(document, depth - 1U);
            bool matches = false;
            state->candidate_depth = depth;
            if (state->test.is_simple) {
                status = sxml_stream_test_matches(
                    dynamic_context,
                    (sxml_stream_candidate_kind){.test = &state->test,
                                                 .node_kind = SXML_NODE_KIND_ELEMENT,
                                                 .node_name = element->event.element_name},
                    &matches);
            } else {
                matches = true;
            }
            if (status == SXML_STATUS_OK && matches) {
                status = sxml_stream_element_window(
                    dynamic_context,
                    (sxml_stream_element_request){.document = document, .is_full = false},
                    &state->candidate);
                if (status == SXML_STATUS_OK && !state->test.is_simple) {
                    status = sxml_stream_node_matches(
                        dynamic_context,
                        (sxml_stream_node_question){.test = &state->test,
                                                    .node_item = state->candidate},
                        &matches);
                }
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!matches) {
                sxml_item_release(state->candidate);
                state->candidate = (sxml_item){.representation = 0};
                status = sxml_stream_pass_element(dynamic_context, state);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                continue;
            }
            state->candidate_source = SXML_STREAM_CANDIDATE_OPEN_ELEMENT;
            *has_candidate = true;
            return SXML_STATUS_OK;
        }
        case SXML_STREAM_EVENT_TEXT:
        case SXML_STREAM_EVENT_COMMENT:
        case SXML_STREAM_EVENT_PROCESSING_INSTRUCTION: {
            if (state->stream->axis == SXML_AXIS_CHILD && depth != state->base_depth) {
                continue;
            }
            sxml_node_kind kind = SXML_NODE_KIND_TEXT;
            if (event.event_kind == SXML_STREAM_EVENT_COMMENT) {
                kind = SXML_NODE_KIND_COMMENT;
            } else if (event.event_kind == SXML_STREAM_EVENT_PROCESSING_INSTRUCTION) {
                kind = SXML_NODE_KIND_PROCESSING_INSTRUCTION;
            }
            bool matches = true;
            if (state->test.is_simple && kind != SXML_NODE_KIND_PROCESSING_INSTRUCTION) {
                status = sxml_stream_test_matches(
                    dynamic_context,
                    (sxml_stream_candidate_kind){
                        .test = &state->test, .node_kind = kind, .node_name = {.index = 0}},
                    &matches);
            }
            if (status == SXML_STATUS_OK && matches) {
                status = sxml_stream_leaf_window(document, &event, &state->candidate);
            }
            if (status == SXML_STATUS_OK && matches && state->candidate.representation != 0 &&
                (!state->test.is_simple || kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION)) {
                status = sxml_stream_node_matches(
                    dynamic_context,
                    (sxml_stream_node_question){.test = &state->test,
                                                .node_item = state->candidate},
                    &matches);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!matches || state->candidate.representation == 0) {
                sxml_item_release(state->candidate);
                state->candidate = (sxml_item){.representation = 0};
                continue;
            }
            state->candidate_source = SXML_STREAM_CANDIDATE_LEAF;
            *has_candidate = true;
            return SXML_STATUS_OK;
        }
        }
    }
}

/* Finds the next candidate and considers it, or finishes. */
static sxml_status sxml_stream_next(sxml_dynamic_context *dynamic_context,
                                    sxml_stream_state *state) {
    bool has_candidate = false;
    sxml_status status = SXML_STATUS_OK;
    if (state->walks_tree) {
        status = sxml_stream_next_tree_candidate(dynamic_context, state, &has_candidate);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!has_candidate && state->document != NULL) {
            /* The end of a full window's descendants: back to the events. */
            state->walks_tree = false;
            sxml_item_release(state->tree_window);
            state->tree_window = (sxml_item){.representation = 0};
        }
    }
    if (!has_candidate && !state->walks_tree && state->document != NULL) {
        status = sxml_stream_next_event_candidate(dynamic_context, state, &has_candidate);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (!has_candidate) {
        return sxml_stream_finish(dynamic_context, state);
    }
    return sxml_stream_consider(dynamic_context, state);
}

/* The predicate ended: selected, or the next candidate. */
static sxml_status sxml_stream_take_predicate(sxml_dynamic_context *dynamic_context,
                                              sxml_stream_state *state) {
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    bool selects = false;
    sxml_status status = sxml_evaluator_machine_test_predicate(
        dynamic_context, &(sxml_predicate_value){.value = value, .position = state->step_position},
        &selects);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (selects) {
        return sxml_stream_select(dynamic_context, state);
    }
    const sxml_stream_candidate_source source = state->candidate_source;
    sxml_item_release(state->candidate);
    state->candidate = (sxml_item){.representation = 0};
    if (source == SXML_STREAM_CANDIDATE_OPEN_ELEMENT) {
        status = sxml_stream_pass_element(dynamic_context, state);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return sxml_stream_next(dynamic_context, state);
}

/* The body ended: what is left of the selected node, then the next. */
static sxml_status sxml_stream_take_body(sxml_dynamic_context *dynamic_context,
                                         sxml_stream_state *state) {
    sxml_status status = sxml_stream_append(dynamic_context, state,
                                            sxml_evaluator_machine_take_value(dynamic_context));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_document *document = state->document;
    if (state->candidate_source == SXML_STREAM_CANDIDATE_OPEN_ELEMENT) {
        const bool was_consumed = document->is_open_node_consumed;
        /* origin is the open node again, read by this task. */
        sxml_item_retain(state->origin);
        sxml_stream_set_open_node(document, state->origin, state->base_depth);
        document->is_open_node_consumed = true;
        if (state->stream->axis != SXML_AXIS_DESCENDANT || was_consumed) {
            status = sxml_stream_skip_to(dynamic_context, document, state->candidate_depth);
        }
    } else if (state->candidate_source == SXML_STREAM_CANDIDATE_CAPTURED_ELEMENT &&
               state->stream->axis == SXML_AXIS_DESCENDANT) {
        /* The candidate's descendants are in its window. */
        sxml_node_reference node;
        status = sxml_item_get_node(state->candidate, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_axis_iterator_start(
                (sxml_axis_request){.origin = node, .axis = SXML_AXIS_DESCENDANT},
                &state->tree_iterator);
        }
        if (status == SXML_STATUS_OK) {
            state->walks_tree = true;
            state->tree_window = state->candidate;
            state->candidate = (sxml_item){.representation = 0};
        }
    }
    sxml_item_release(state->candidate);
    state->candidate = (sxml_item){.representation = 0};
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_stream_next(dynamic_context, state);
}

/* ------------------------------------------------------------- Start */

static sxml_status sxml_stream_allocate_state(sxml_dynamic_context *dynamic_context,
                                              sxml_evaluator_task *task,
                                              sxml_stream_state **state) {
    void *memory = NULL;
    const sxml_status status =
        sxml_allocator_allocate(dynamic_context->allocator,
                                (sxml_allocation_request){.byte_count = sizeof(sxml_stream_state),
                                                          .alignment = alignof(sxml_stream_state)},
                                &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_state *new_state = memory;
    memset(new_state, 0, sizeof(*new_state));
    new_state->stream = sxml_stream_get_expression(task);
    new_state->origin = (sxml_item){.representation = 0};
    new_state->tree_window = (sxml_item){.representation = 0};
    new_state->candidate = (sxml_item){.representation = 0};
    task->details.part.state = new_state;
    task->details.part.release_state = sxml_stream_release_state;
    *state = new_state;
    return SXML_STATUS_OK;
}

/* The single item of value, retained; XPTY0020 for anything but one
   node. */
static sxml_status sxml_stream_read_origin(sxml_dynamic_context *dynamic_context,
                                           sxml_stream_state *state) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status =
        sxml_evaluator_machine_read_atom(dynamic_context, state->stream->origin, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_reference node;
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_node(value.content, &node) != SXML_STATUS_OK) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_stream_not_a_node_error);
    }
    sxml_item_retain(value.content);
    state->origin = value.content;
    return SXML_STATUS_OK;
}

/* OPENS_DOCUMENT: the document, streamed or as fn:doc gives it, as the
   body's focus. */
static sxml_status sxml_stream_start_document(sxml_dynamic_context *dynamic_context,
                                              sxml_stream_state *state) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status =
        sxml_evaluator_machine_read_atom(dynamic_context, state->stream->origin, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_atomic_item_get_datum(value.content, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_stream_no_document_uri_error);
    }
    const sxml_byte_view uri_reference = datum.datum_details.string.utf8;
    status = sxml_stream_document_open(dynamic_context, uri_reference, &state->document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->owns_document = state->document != NULL;
    if (state->document == NULL) {
        /* No processor's loader: fn:doc's document. */
        if (dynamic_context->document_loader.operations == NULL ||
            dynamic_context->document_loader.operations->load_document == NULL) {
            return sxml_evaluator_machine_raise(dynamic_context,
                                                &sxml_stream_no_document_uri_error);
        }
        status = dynamic_context->document_loader.operations->load_document(
            dynamic_context->document_loader.state,
            &(sxml_document_request){.uri_reference = uri_reference,
                                     .base_uri =
                                         sxml_dynamic_context_get_static_base_uri(dynamic_context),
                                     .error_record = dynamic_context->error_record,
                                     .blocks_external_resources = false,
                                     .validates_dtd = false},
            &state->candidate);
    } else if (!state->document->source.is_streamed) {
        state->candidate = state->document->source.document_node;
        sxml_item_retain(state->candidate);
        sxml_stream_document *unstreamed = state->document;
        state->document = NULL;
        state->owns_document = false;
        sxml_stream_document_destroy(unstreamed);
    } else {
        sxml_stream_window_build build;
        status = sxml_stream_begin_window(state->document, 0, &build);
        if (status == SXML_STATUS_OK) {
            status = sxml_stream_end_window(
                state->document,
                (sxml_stream_window_end){.build = &build, .open_count = 0, .node_index = 0},
                &state->candidate);
        }
        if (status == SXML_STATUS_OK) {
            sxml_item_retain(state->candidate);
            sxml_stream_set_open_node(state->document, state->candidate, 0);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->candidate_source = SXML_STREAM_CANDIDATE_LEAF;
    state->selected_count = 1;
    return sxml_stream_push_focused(dynamic_context, state,
                                    (sxml_stream_push){.expression = state->stream->body,
                                                       .position = 1,
                                                       .phase = SXML_STREAM_PHASE_BODY});
}

/* OPENS_DOCUMENT: the body ended; the rest of the document is read (it
   must be well-formed) and the value handed on. */
static sxml_status sxml_stream_finish_document(sxml_dynamic_context *dynamic_context,
                                               sxml_stream_state *state) {
    sxml_status status = sxml_stream_append(dynamic_context, state,
                                            sxml_evaluator_machine_take_value(dynamic_context));
    sxml_stream_document *document = state->document;
    if (status == SXML_STATUS_OK && document != NULL) {
        document->mode = SXML_STREAM_MODE_DRAIN;
        document->queue_count = 0;
        while (status == SXML_STATUS_OK && !document->is_finished) {
            status = sxml_stream_resume(dynamic_context, document);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_stream_finish(dynamic_context, state);
}

static sxml_status sxml_stream_start(sxml_dynamic_context *dynamic_context,
                                     sxml_evaluator_task *task) {
    sxml_stream_state *state = NULL;
    sxml_status status = sxml_stream_allocate_state(dynamic_context, task, &state);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if ((state->stream->flags & (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT) != 0) {
        return sxml_stream_start_document(dynamic_context, state);
    }
    status = sxml_stream_resolve_test(dynamic_context, state->stream->node_test, &state->test);
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_read_origin(dynamic_context, state);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->document = sxml_stream_find_document(dynamic_context, state->origin);
    if (state->document != NULL) {
        /* origin's content is read by this task (it consumes it). */
        state->document->is_open_node_consumed = true;
        state->base_depth = state->document->open_depth;
    } else {
        sxml_node_reference node;
        status = sxml_item_get_node(state->origin, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_axis_iterator_start(
                (sxml_axis_request){.origin = node, .axis = state->stream->axis},
                &state->tree_iterator);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        state->walks_tree = true;
    }
    return sxml_stream_next(dynamic_context, state);
}

/* The step function of STREAM. */
static sxml_status sxml_stream_step(sxml_dynamic_context *dynamic_context,
                                    sxml_evaluator_task *task) {
    sxml_stream_state *state = task->details.part.state;
    switch ((sxml_stream_phase)task->phase) {
    case SXML_STREAM_PHASE_START:
        task->phase = (uint32_t)SXML_STREAM_PHASE_NEXT;
        return sxml_stream_start(dynamic_context, task);
    case SXML_STREAM_PHASE_NEXT:
        return sxml_stream_next(dynamic_context, state);
    case SXML_STREAM_PHASE_PREDICATE:
        return sxml_stream_take_predicate(dynamic_context, state);
    case SXML_STREAM_PHASE_BODY:
        if ((state->stream->flags & (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT) != 0) {
            return sxml_stream_finish_document(dynamic_context, state);
        }
        return sxml_stream_take_body(dynamic_context, state);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_evaluator_kind_handler sxml_stream_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_STREAM, .step = sxml_stream_step},
};

const sxml_evaluator_part sxml_evaluator_part_stream = {
    .kind_handlers = sxml_stream_kind_handlers,
    .kind_handler_count = sizeof(sxml_stream_kind_handlers) / sizeof(sxml_stream_kind_handlers[0]),
};
