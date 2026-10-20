/*
 * Evaluator part: streamed evaluation (src/evaluator_machine_internal.h,
 * D101, D166, D219). Evaluates STREAM (src/ir_internal.h): the stream
 * points of xsl:source-document with streamable="yes" and of the bodies it
 * streams (XSLT 4.0, 18.1 and 19); and MERGE, xsl:merge over streamed
 * documents read in step (15.4; packet 7.6g, at the end of the file).
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
 * Accumulators (D231; XSLT 4.0, 18.2.9 and 19.3): the STREAM that opens a
 * document whose accumulators are read (sxml_ir_stream_accumulators)
 * computes their values as the events pass. Each event a STREAM task
 * takes calls the accumulator functions before the task goes on: an
 * element's start in a shallow window of its own (the window the body
 * then sees, so its values are found by the node), its end, a text,
 * comment or processing instruction's start and end, the document's end;
 * content skipped is taken event by event for them. A full window (a
 * selected subtree, a grounding stream) gets its descendants' values at
 * once from descend-K, a traversal of the window. The values of the nodes
 * the windows hold now are kept by depth, and the row
 * accumulator-streamed reads them (sxml_evaluator_stream_find_accumulator_
 * values). A rule's error is kept and raised by the reads of the values it
 * left out (19.5); a rule that reads another accumulator's value of the
 * same node waits until that one is computed.
 * An end rule with capture="yes" (19.9; packet 7.6g) reads the node's
 * subtree: at each element's start the capture function names the
 * accumulators whose such rules match it, and the builder then also sends
 * the element's events to a full window of its own (a tee; nested tees
 * are a stack), which those accumulators' end calls get as the node.
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

#include "memory_internal.h"
#include "processor_internal.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xml.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------ Errors */

static const sxml_static_error sxml_stream_not_well_formed_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A streamed document (of xsl:source-document or the principal source) is not a "
        "well-formed XML document (XSLT 4.0, 18.1; F&O 4.0, fn:doc)"),
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
    /* The element count when it was queued: a text, comment or processing
       instruction is a child of the element at that depth even when the
       start of a following element was queued after it. */
    size_t depth;
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

/* The accumulator values of one node of a streamed document whose
   accumulators are computed as its events pass (D231): the node in the
   window its events were read in, and the values maps (from accumulator
   numbers) after its start and after its end; for an element whose
   subtree went into a full window, that window and the "nodes" map its
   descend call gave. Every item is owned, or no item. */
typedef struct sxml_stream_accumulated {
    sxml_item window;
    sxml_item before;
    sxml_item after;
    sxml_item full_window;
    sxml_item descended;
    /* An element some end rule with capture="yes" matches (19.9): the
       numbers of those accumulators (xs:integer items) and, once its end
       was read, the element in a full window of its own, which their end
       calls get (packet 7.6g). */
    sxml_sequence captures;
    sxml_item captured;
    /* The serial number of the event that started the node. */
    uint64_t serial;
} sxml_stream_accumulated;

/* An element whose events are also sent to a window of its own as they
   pass (capture="yes", 19.9; packet 7.6g): the window being built, the
   element's depth and its index in the window. */
typedef struct sxml_stream_tee {
    sxml_stream_window_build build;
    size_t depth;
    uint32_t node_index;
} sxml_stream_tee;

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
    /* The document computes accumulators (D231); its end was accumulated;
       the rounds of the call running, and whether one runs; set by
       accumulator-streamed when a rule reads a value of the node the call
       has not computed yet (that accumulator waits a round); a call
       failed, so no more are made. */
    bool accumulates;
    bool is_end_accumulated;
    bool has_round_progressed;
    bool is_final_round;
    bool is_computing;
    bool is_waiting;
    bool has_accumulation_failed;
    /* Events taken from the queue, counted (the serial of the last). */
    uint64_t event_serial;
    /* Elements the parse has read, and those whose steps were granted
       (sxml_stream_resume, D295). */
    uint64_t read_element_count;
    uint64_t granted_element_count;
    /* Accumulators computed as the events pass, when accumulates:
       the functions of the OPENS_DOCUMENT STREAM's accumulators, the
       values after the last event taken (owned), the nodes by depth
       (entry 0 the document node, entry d the element open at depth d or
       the last that was) and the last text, comment or processing
       instruction. */
    const sxml_ir_function *accumulator_initial;
    const sxml_ir_function *accumulator_start;
    const sxml_ir_function *accumulator_end;
    const sxml_ir_function *accumulator_descend;
    /* NULL when no end rule captures (packet 7.6g). */
    const sxml_ir_function *accumulator_capture;
    /* The elements being captured for their end rules, outermost first
       (sxml_stream_tee). */
    sxml_xml_array tees;
    sxml_item accumulator_values;
    sxml_xml_array accumulated;
    sxml_stream_accumulated accumulated_leaf;
    /* The accumulators' numbers (owned xs:integer items) in the order of
       the initial values; for the call running, the ones it has computed
       and the one it computes (sxml_stream_next_number). */
    sxml_item *accumulator_numbers;
    bool *is_number_done;
    size_t number_count;
    size_t number_cursor;
    /* The key "v" of descend-K's map (owned). */
    sxml_item value_key;
    /* The error of a call that failed (has_accumulation_failed), a copy
       in failure_text: the values not computed raise it when read
       (19.5). */
    sxml_error_raise_request failure;
    sxml_xml_buffer failure_text;
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

/* Replaces what slot holds by item (retained). */
static void sxml_stream_keep_item(sxml_item *slot, sxml_item item) {
    sxml_item_retain(item);
    sxml_item_release(*slot);
    *slot = item;
}

/* Releases what entry holds and empties it. */
static void sxml_stream_clear_accumulated(sxml_stream_accumulated *entry) {
    sxml_item_release(entry->window);
    sxml_item_release(entry->before);
    sxml_item_release(entry->after);
    sxml_item_release(entry->full_window);
    sxml_item_release(entry->descended);
    sxml_sequence_release(entry->captures);
    sxml_item_release(entry->captured);
    *entry = (sxml_stream_accumulated){.window = {.representation = 0},
                                       .before = {.representation = 0},
                                       .after = {.representation = 0},
                                       .full_window = {.representation = 0},
                                       .descended = {.representation = 0},
                                       .captures = sxml_sequence_empty(),
                                       .captured = {.representation = 0},
                                       .serial = 0};
}

/* The entry of the node at depth (0: the document node), made empty when
   the document has none yet; NULL when no memory is left for it. */
static sxml_stream_accumulated *sxml_stream_accumulated_at(sxml_stream_document *document,
                                                           size_t depth) {
    while (document->accumulated.element_count <= depth) {
        void *memory = NULL;
        if (sxml_xml_array_push(&document->accumulated, &memory) != SXML_STATUS_OK) {
            return NULL;
        }
        sxml_stream_accumulated *entry = memory;
        entry->window = (sxml_item){.representation = 0};
        entry->before = (sxml_item){.representation = 0};
        entry->after = (sxml_item){.representation = 0};
        entry->full_window = (sxml_item){.representation = 0};
        entry->descended = (sxml_item){.representation = 0};
        entry->captures = sxml_sequence_empty();
        entry->captured = (sxml_item){.representation = 0};
        entry->serial = 0;
    }
    return sxml_xml_array_at(&document->accumulated, depth);
}

/* ------------------------------------------------------ Window building */

/* Sends the start of the open element at element_index, with its
   bindings and attributes, to the window being built. */
static sxml_status sxml_stream_replay_element(const sxml_stream_document *document,
                                              sxml_stream_window_build *build,
                                              size_t element_index) {
    const sxml_tree_builder builder = build->builder;
    const sxml_stream_element *element = sxml_stream_element_at(document, element_index);
    sxml_element_start_event start = element->event;
    start.entity_base_uri = sxml_xml_buffer_view(&document->ancestor_text, element->entity_base_uri);
    sxml_status status = builder.operations->start_element(builder.state, &start);
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < element->binding_count; binding_index += 1) {
        const sxml_namespace_binding *binding =
            sxml_xml_array_at(&document->bindings, element->first_binding + binding_index);
        status = builder.operations->add_namespace_binding(builder.state, binding);
    }
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < element->attribute_count;
         attribute_index += 1) {
        const sxml_stream_attribute *attribute =
            sxml_xml_array_at(&document->attributes, element->first_attribute + attribute_index);
        sxml_attribute_event event = attribute->event;
        event.attribute_value = sxml_xml_buffer_view(&document->ancestor_text, attribute->value);
        status = builder.operations->add_attribute(builder.state, &event);
    }
    build->last_element_index = build->next_node_index;
    build->next_node_index += 1U + (uint32_t)element->attribute_count;
    return status;
}

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
        status = sxml_stream_replay_element(document, build, element_index);
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
        status = preparation.operations->prepare_document(
            preparation.state,
            &(sxml_preparation_request){.document_node = document_node, .preparation_number = 0},
            &prepared);
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

/* The builder of the tee at tee_index (packet 7.6g). */
static sxml_tree_builder sxml_stream_tee_builder(const sxml_stream_document *document,
                                                 size_t tee_index) {
    const sxml_stream_tee *tee = sxml_xml_array_at(&document->tees, tee_index);
    return tee->build.builder;
}

/* Ends the window of the tee on top (its element's end was sent to it)
   and keeps its element as the captured node of its depth's entry. */
static sxml_status sxml_stream_finish_tee(sxml_stream_document *document) {
    sxml_stream_tee *tee = sxml_xml_array_at(&document->tees, document->tees.element_count - 1U);
    sxml_stream_window_build build = tee->build;
    const size_t depth = tee->depth;
    const uint32_t node_index = tee->node_index;
    document->tees.element_count -= 1U;
    sxml_item captured = {.representation = 0};
    sxml_status status = sxml_stream_end_window(
        document,
        (sxml_stream_window_end){.build = &build, .open_count = depth - 1U, .node_index = node_index},
        &captured);
    sxml_stream_accumulated *entry =
        status == SXML_STATUS_OK ? sxml_stream_accumulated_at(document, depth) : NULL;
    if (entry == NULL) {
        sxml_item_release(captured);
        return status == SXML_STATUS_OK ? SXML_STATUS_OUT_OF_MEMORY : status;
    }
    sxml_item_release(entry->captured);
    entry->captured = captured;
    return SXML_STATUS_OK;
}

static sxml_status sxml_stream_queue(sxml_stream_document *document, sxml_stream_event event) {
    if (document->queue_count == STREAM_QUEUE_CAPACITY) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    const size_t slot = (document->queue_start + document->queue_count) % STREAM_QUEUE_CAPACITY;
    document->queue[slot] = event;
    document->queue[slot].depth = sxml_stream_depth(document);
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
    document->read_element_count += 1;
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->start_element(tee.state, event);
    }
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->add_namespace_binding(tee.state, binding);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->add_attribute(tee.state, event);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->end_element(tee.state);
    }
    if (status == SXML_STATUS_OK && document->tees.element_count > 0 &&
        ((const sxml_stream_tee *)sxml_xml_array_at(&document->tees,
                                                    document->tees.element_count - 1U))
                ->depth == sxml_stream_depth(document)) {
        status = sxml_stream_finish_tee(document);
    }
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
    for (size_t tee_index = 0; tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        const sxml_status status = tee.operations->add_text(tee.state, event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->add_comment(tee.state, event);
    }
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
    for (size_t tee_index = 0;
         status == SXML_STATUS_OK && tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_tree_builder tee = sxml_stream_tee_builder(document, tee_index);
        status = tee.operations->add_processing_instruction(tee.state, event);
    }
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
   document is not well-formed or its bytes cannot be read. */
static sxml_status sxml_stream_resume(sxml_dynamic_context *dynamic_context,
                                      sxml_stream_document *document) {
    if (document->is_finished) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_status status =
        sxml_xml_incremental_parse_resume(document->parse, &document->is_finished);
    /* Each element read grants its steps (D295): the step bound of a
       streamed evaluation grows with the input it reads. */
    sxml_dynamic_context_grant_steps(
        dynamic_context, (document->read_element_count - document->granted_element_count) *
                             SXML_EVALUATOR_STREAMED_ELEMENT_STEP_COUNT);
    document->granted_element_count = document->read_element_count;
    if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
        status == SXML_STATUS_UNSUPPORTED_FEATURE ||
        status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        /* A byte reader's failed read: the document is not available
           (include/sxml/processor.h, D233). */
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
    document->event_serial += 1;
    if (event->event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
        sxml_stream_pop_element(document);
    }
    return SXML_STATUS_OK;
}

/* Reads on until the element count is below depth: the rest of an
   element's content nobody reads (XSLT 4.0, 19: what a body did not
   consume). */
static sxml_status sxml_stream_skip_events(sxml_dynamic_context *dynamic_context,
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
    if (!is_full && document->accumulates && depth < document->accumulated.element_count) {
        /* The window the element's accumulators were computed in, so the
           node's values are found (D231). */
        const sxml_stream_accumulated *entry = sxml_xml_array_at(&document->accumulated, depth);
        if (entry->serial == document->event_serial && entry->window.representation != 0) {
            sxml_item_retain(entry->window);
            *node = entry->window;
            return SXML_STATUS_OK;
        }
    }
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

/* Sends a text, comment or processing instruction event to builder. */
static sxml_status sxml_stream_add_leaf(const sxml_stream_document *document,
                                        sxml_tree_builder builder, const sxml_stream_event *event) {
    const sxml_byte_view text = sxml_xml_buffer_view(&document->event_text, event->text);
    if (event->event_kind == SXML_STREAM_EVENT_TEXT) {
        return builder.operations->add_text(
            builder.state, &(sxml_text_event){.text = text,
                                              .location = event->location,
                                              .is_cdata_section = event->is_cdata_section,
                                              .disables_output_escaping = false});
    }
    if (event->event_kind == SXML_STREAM_EVENT_COMMENT) {
        return builder.operations->add_comment(
            builder.state, &(sxml_comment_event){.comment = text, .location = event->location});
    }
    return builder.operations->add_processing_instruction(
        builder.state, &(sxml_processing_instruction_event){
                           .target = text,
                           .data = sxml_xml_buffer_view(&document->event_text, event->data),
                           .location = event->location});
}

/* The window of a text, comment or processing instruction just taken. */
static sxml_status sxml_stream_leaf_window(sxml_stream_document *document,
                                           const sxml_stream_event *event, sxml_item *node) {
    if (document->accumulates && document->accumulated_leaf.serial == document->event_serial &&
        document->accumulated_leaf.window.representation != 0) {
        /* As for an element (D231). */
        sxml_item_retain(document->accumulated_leaf.window);
        *node = document->accumulated_leaf.window;
        return SXML_STATUS_OK;
    }
    const size_t depth = event->depth;
    sxml_stream_window_build build;
    sxml_status status = sxml_stream_begin_window(document, depth, &build);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t node_index = build.next_node_index;
    status = sxml_stream_add_leaf(document, build.builder, event);
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(build.arena_tree);
        return status;
    }
    return sxml_stream_end_window(
        document,
        (sxml_stream_window_end){.build = &build, .open_count = depth, .node_index = node_index},
        node);
}

/* The full window of the document node while it is the open node, its
   content not read yet: the whole document, as fn:doc would build it
   (a template rule that reads more than stream points of its own, D228).
   The events queued so far go to the window first, the pending text
   next, then the rest of the parse. */
static sxml_status sxml_stream_document_window(sxml_dynamic_context *dynamic_context,
                                               sxml_stream_document *document, sxml_item *node) {
    sxml_stream_window_build build;
    sxml_status status = sxml_stream_begin_window(document, 0, &build);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    while (status == SXML_STATUS_OK && document->queue_count > 0) {
        const sxml_stream_event *queued = &document->queue[document->queue_start];
        if (queued->event_kind == SXML_STREAM_EVENT_START_ELEMENT) {
            status = sxml_stream_replay_element(document, &build, queued->depth - 1U);
        } else if (queued->event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
            status = build.builder.operations->end_element(build.builder.state);
        } else if (queued->event_kind != SXML_STREAM_EVENT_END_DOCUMENT) {
            status = sxml_stream_add_leaf(document, build.builder, queued);
        }
        sxml_stream_event taken;
        if (status == SXML_STATUS_OK) {
            status = sxml_stream_next_event(dynamic_context, document, &taken);
        }
    }
    if (status == SXML_STATUS_OK && document->has_pending_text) {
        document->has_pending_text = false;
        status = sxml_stream_add_leaf(document, build.builder, &document->pending_text);
    }
    if (status == SXML_STATUS_OK && !document->is_finished) {
        document->capture = build;
        document->mode = SXML_STREAM_MODE_CAPTURE;
        document->mode_depth = 0;
        while (status == SXML_STATUS_OK && !document->is_finished) {
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
    return sxml_stream_end_window(
        document, (sxml_stream_window_end){.build = &build, .open_count = 0, .node_index = 0},
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
    for (size_t tee_index = 0; tee_index < document->tees.element_count; tee_index += 1) {
        const sxml_stream_tee *tee = sxml_xml_array_at(&document->tees, tee_index);
        sxml_arena_tree_destroy(tee->build.arena_tree);
    }
    sxml_xml_array_release(&document->tees);
    if (document->loader != NULL) {
        sxml_processor_document_loader_close_streamed(document->loader, &document->source);
    }
    sxml_item_release(document->open_node);
    for (size_t entry_index = 0; entry_index < document->accumulated.element_count;
         entry_index += 1) {
        sxml_stream_clear_accumulated(sxml_xml_array_at(&document->accumulated, entry_index));
    }
    sxml_xml_array_release(&document->accumulated);
    sxml_stream_clear_accumulated(&document->accumulated_leaf);
    sxml_item_release(document->accumulator_values);
    for (size_t number_index = 0; number_index < document->number_count; number_index += 1) {
        sxml_item_release(document->accumulator_numbers[number_index]);
    }
    sxml_allocator_release(document->allocator, document->accumulator_numbers);
    sxml_allocator_release(document->allocator, document->is_number_done);
    sxml_item_release(document->value_key);
    sxml_xml_buffer_release(&document->failure_text);
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
    new_document->accumulated = sxml_stream_array(allocator, sizeof(sxml_stream_accumulated));
    new_document->tees = sxml_stream_array(allocator, sizeof(sxml_stream_tee));
    new_document->accumulator_values = (sxml_item){.representation = 0};
    new_document->value_key = (sxml_item){.representation = 0};
    new_document->failure_text = sxml_xml_buffer_empty(allocator);
    sxml_stream_clear_accumulated(&new_document->accumulated_leaf);
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
                                .operations = &sxml_stream_builder_operations},
                    .byte_reader = new_document->source.byte_reader},
                &new_document->parse);
        }
        if (status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8 ||
            status == SXML_STATUS_UNSUPPORTED_FEATURE ||
            status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
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

sxml_status sxml_evaluator_stream_check_available(sxml_dynamic_context *dynamic_context,
                                                  sxml_byte_view uri_reference,
                                                  sxml_stream_availability *availability) {
    *availability = SXML_STREAM_AVAILABILITY_UNKNOWN;
    /* The errors of the open are the answer, not the evaluation's: they
       go unreported, as fn:doc-available's do. */
    sxml_error_record *error_record = dynamic_context->error_record;
    dynamic_context->error_record = NULL;
    sxml_stream_document *document = NULL;
    sxml_status status = sxml_stream_document_open(dynamic_context, uri_reference, &document);
    dynamic_context->error_record = error_record;
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        sxml_item_heap_clear_raised_error(dynamic_context->item_heap);
        *availability = SXML_STREAM_AVAILABILITY_UNAVAILABLE;
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK && document != NULL) {
        *availability = SXML_STREAM_AVAILABILITY_AVAILABLE;
        sxml_stream_document_destroy(document);
    }
    return status;
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
    /* A call of an accumulator function ended (D231). */
    SXML_STREAM_PHASE_ACCUMULATED,
} sxml_stream_phase;

/* What the task does once the accumulator calls of an event or a window
   have ended (D231). */
typedef enum sxml_stream_continuation {
    /* sxml_stream_next, the stashed event first. */
    SXML_STREAM_CONTINUE_NEXT,
    /* Skips to continuation_depth, then sxml_stream_next. */
    SXML_STREAM_CONTINUE_SKIP,
    /* The selected node's body. */
    SXML_STREAM_CONTINUE_SELECT,
    /* The body of the document (OPENS_DOCUMENT). */
    SXML_STREAM_CONTINUE_DOCUMENT_BODY,
    /* The body of a grounding stream. */
    SXML_STREAM_CONTINUE_GROUNDING_BODY,
} sxml_stream_continuation;

/* What one call of an accumulator function computes (D231). */
typedef enum sxml_stream_accumulation_kind {
    /* The initial values for the document node. */
    SXML_STREAM_ACCUMULATION_INITIAL,
    /* The values after a node's start event: its before. */
    SXML_STREAM_ACCUMULATION_START,
    /* The values after a node's end event: its after. */
    SXML_STREAM_ACCUMULATION_END,
    /* The values of a subtree in a full window, and after its end. */
    SXML_STREAM_ACCUMULATION_DESCEND,
    /* The accumulators an element's end event reads the subtree for
       (capture="yes", 19.9; packet 7.6g): its events are then captured. */
    SXML_STREAM_ACCUMULATION_CAPTURE,
} sxml_stream_accumulation_kind;

/* One call of an accumulator function: its node (owned) and whose values
   it gives: the entry at depth, or the last leaf. */
typedef struct sxml_stream_accumulation {
    sxml_stream_accumulation_kind accumulation_kind;
    sxml_item node;
    size_t depth;
    bool is_leaf;
} sxml_stream_accumulation;

/* The most calls one event needs: a leaf's start and end. */
#define STREAM_MAXIMUM_ACCUMULATION_COUNT 2U

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
    /* Accumulator calls waiting (D231): call_next of call_count, then the
       task resumes as resume says; an event taken before them is stashed
       for sxml_stream_next. is_suspended is set while they run, so the
       functions that took the event return at once. */
    sxml_stream_accumulation calls[STREAM_MAXIMUM_ACCUMULATION_COUNT];
    size_t call_count;
    size_t call_next;
    sxml_stream_continuation continuation;
    size_t continuation_depth;
    bool is_suspended;
    bool has_stashed_event;
    sxml_stream_event stashed_event;
} sxml_stream_state;

static void sxml_stream_release_state(sxml_dynamic_context *dynamic_context, void *state) {
    sxml_stream_state *stream_state = state;
    sxml_item_release(stream_state->origin);
    sxml_item_release(stream_state->tree_window);
    sxml_item_release(stream_state->candidate);
    for (size_t call_index = stream_state->call_next; call_index < stream_state->call_count;
         call_index += 1) {
        sxml_item_release(stream_state->calls[call_index].node);
    }
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

/* ------------------------------------------------------ Accumulators */

/* The function a call of kind calls (src/ir_internal.h,
   sxml_ir_stream_accumulators). */
static const sxml_ir_function *sxml_stream_accumulation_function(
    const sxml_stream_document *document, sxml_stream_accumulation_kind kind) {
    switch (kind) {
    case SXML_STREAM_ACCUMULATION_INITIAL:
        return document->accumulator_initial;
    case SXML_STREAM_ACCUMULATION_START:
        return document->accumulator_start;
    case SXML_STREAM_ACCUMULATION_END:
        return document->accumulator_end;
    case SXML_STREAM_ACCUMULATION_DESCEND:
        return document->accumulator_descend;
    case SXML_STREAM_ACCUMULATION_CAPTURE:
        return document->accumulator_capture;
    }
    return NULL;
}

/* The value of the accumulator at number_index in the document's values,
   borrowed; the empty sequence when there is none. */
static sxml_sequence sxml_stream_number_value(const sxml_stream_document *document,
                                              size_t number_index) {
    sxml_map_lookup found = {.is_found = false};
    if (document->accumulator_values.representation != 0 &&
        sxml_map_find(document->accumulator_values, document->accumulator_numbers[number_index],
                      &found) == SXML_STATUS_OK &&
        found.is_found) {
        return found.entry.value;
    }
    return sxml_sequence_empty();
}

/* Whether call computes what it computes for every accumulator at once:
   INITIAL and CAPTURE take the node only. */
static bool sxml_stream_is_whole_call(const sxml_stream_accumulation *call) {
    return call->accumulation_kind == SXML_STREAM_ACCUMULATION_INITIAL ||
           call->accumulation_kind == SXML_STREAM_ACCUMULATION_CAPTURE;
}

/* Whether integer is among the xs:integer items of numbers. */
typedef struct sxml_stream_number_question {
    sxml_sequence numbers;
    sxml_item number;
} sxml_stream_number_question;

static bool sxml_stream_has_number(sxml_stream_number_question question) {
    if (sxml_item_get_representation(question.number) != SXML_ITEM_REPRESENTATION_INTEGER) {
        return false;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(question.numbers);
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) != SXML_STATUS_OK ||
            !step.has_item) {
            return false;
        }
        if (sxml_item_get_representation(step.item) == SXML_ITEM_REPRESENTATION_INTEGER &&
            step.item.payload.integer == question.number.payload.integer) {
            return true;
        }
    }
}

/* The node call passes: the element in the full window its events were
   captured into for an end call of an accumulator whose end rule with
   capture="yes" matched it (19.9; packet 7.6g), else the call's node. */
static sxml_item sxml_stream_call_node(const sxml_stream_document *document,
                                       const sxml_stream_accumulation *call) {
    if (call->accumulation_kind != SXML_STREAM_ACCUMULATION_END || call->is_leaf ||
        call->depth >= document->accumulated.element_count ||
        document->number_cursor >= document->number_count) {
        return call->node;
    }
    const sxml_stream_accumulated *entry = sxml_xml_array_at(&document->accumulated, call->depth);
    if (entry->captured.representation == 0 ||
        !sxml_stream_has_number((sxml_stream_number_question){
            .numbers = entry->captures,
            .number = document->accumulator_numbers[document->number_cursor]})) {
        return call->node;
    }
    return entry->captured;
}

/* Pushes a CALL of the call at state->call_next above the task, as
   src/evaluator_template.c pushes a template's: INITIAL of its node; the
   others of its node, the value of the accumulator at number_cursor and
   its number. The task resumes in phase ACCUMULATED. */
static sxml_status sxml_stream_push_accumulation(sxml_dynamic_context *dynamic_context,
                                                 sxml_stream_state *state) {
    const sxml_stream_accumulation *call = &state->calls[state->call_next];
    sxml_stream_document *document = state->document;
    const sxml_ir_function *function =
        sxml_stream_accumulation_function(document, call->accumulation_kind);
    /* INITIAL and CAPTURE give what they give for every accumulator. */
    const bool is_initial = sxml_stream_is_whole_call(call);
    if (function == NULL || function->parameter_count != (is_initial ? 1U : 3U) ||
        (!is_initial && document->number_cursor >= document->number_count)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const size_t argument_count = function->parameter_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        dynamic_context->allocator,
        (sxml_allocation_request){.byte_count = argument_count * sizeof(sxml_sequence),
                                  .alignment = alignof(sxml_sequence)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence *arguments = memory;
    const sxml_item node = sxml_stream_call_node(document, call);
    sxml_item_retain(node);
    arguments[0] = sxml_sequence_from_item(node);
    if (!is_initial) {
        arguments[1] = sxml_stream_number_value(document, document->number_cursor);
        sxml_sequence_retain(arguments[1]);
        const sxml_item number = document->accumulator_numbers[document->number_cursor];
        sxml_item_retain(number);
        arguments[2] = sxml_sequence_from_item(number);
    }
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->phase = (uint32_t)SXML_STREAM_PHASE_ACCUMULATED;
    /* 19.5: an error is raised when a value it leaves out is read. */
    task->catches_errors = true;
    /* The task pointer is invalid after the push. */
    status = sxml_evaluator_machine_push_expression(dynamic_context, task->expression);
    if (status != SXML_STATUS_OK) {
        sxml_evaluator_machine_top(dynamic_context)->catches_errors = false;
        for (size_t argument_index = 0; argument_index < argument_count; argument_index += 1) {
            sxml_sequence_release(arguments[argument_index]);
        }
        sxml_allocator_release(dynamic_context->allocator, arguments);
        return status;
    }
    sxml_evaluator_task *call_task = sxml_evaluator_machine_top(dynamic_context);
    call_task->task_kind = SXML_EVALUATOR_TASK_KIND_CALL;
    call_task->phase = 0;
    call_task->delivers_content = false;
    call_task->details.call = (sxml_call_state){.function_item = {.representation = 0},
                                                .callee = function,
                                                .native_function = NULL,
                                                .arguments = arguments,
                                                .argument_count = argument_count,
                                                .native_call = NULL};
    state->is_suspended = true;
    return SXML_STATUS_OK;
}

/* Starts the call at state->call_next: for each accumulator in turn, but
   for INITIAL, which gives all of them. */
static sxml_status sxml_stream_begin_call(sxml_dynamic_context *dynamic_context,
                                          sxml_stream_state *state) {
    sxml_stream_document *document = state->document;
    if (document == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_stream_accumulation *call = &state->calls[state->call_next];
    if (call->accumulation_kind == SXML_STREAM_ACCUMULATION_DESCEND) {
        /* Its nodes are the window's from now on: a rule that reads a
           value not computed yet waits. */
        sxml_stream_accumulated *entry = sxml_stream_accumulated_at(document, call->depth);
        if (entry == NULL) {
            return SXML_STATUS_OUT_OF_MEMORY;
        }
        sxml_stream_keep_item(&entry->full_window, call->node);
        sxml_item_release(entry->descended);
        entry->descended = (sxml_item){.representation = 0};
    }
    if (!sxml_stream_is_whole_call(call)) {
        for (size_t number_index = 0; number_index < document->number_count; number_index += 1) {
            document->is_number_done[number_index] = false;
        }
        document->number_cursor = 0;
        document->has_round_progressed = false;
        document->is_final_round = false;
    }
    document->is_computing = true;
    return sxml_stream_push_accumulation(dynamic_context, state);
}

/* Moves number_cursor to the next accumulator whose value the call has not
   computed; false when it has them all. An accumulator a rule of another
   needed first is computed in a later round (D231: the values of one node
   depend on each other, 19.4); a round without progress is the last, in
   which the rule's error is raised (XTDE3400). */
static bool sxml_stream_next_number(sxml_stream_document *document) {
    for (size_t number_index = document->number_cursor + 1U;
         number_index < document->number_count; number_index += 1) {
        if (!document->is_number_done[number_index]) {
            document->number_cursor = number_index;
            return true;
        }
    }
    for (size_t number_index = 0; number_index < document->number_count; number_index += 1) {
        if (!document->is_number_done[number_index]) {
            if (document->has_round_progressed) {
                document->has_round_progressed = false;
            } else {
                document->is_final_round = true;
            }
            document->number_cursor = number_index;
            return true;
        }
    }
    return false;
}

/* Adds a call of kind for node (owned) to state's calls. */
typedef struct sxml_stream_call_request {
    sxml_stream_accumulation_kind accumulation_kind;
    sxml_item node;
    size_t depth;
    bool is_leaf;
} sxml_stream_call_request;

static void sxml_stream_add_call(sxml_stream_state *state, sxml_stream_call_request request) {
    state->calls[state->call_count] =
        (sxml_stream_accumulation){.accumulation_kind = request.accumulation_kind,
                                   .node = request.node,
                                   .depth = request.depth,
                                   .is_leaf = request.is_leaf};
    state->call_count += 1;
}

/* Starts the calls added, the task going on as continuation says after
   them; nothing when none was added. */
static sxml_status sxml_stream_start_calls(sxml_dynamic_context *dynamic_context,
                                           sxml_stream_state *state,
                                           sxml_stream_continuation continuation) {
    if (state->call_count == 0) {
        return SXML_STATUS_OK;
    }
    state->call_next = 0;
    state->continuation = continuation;
    return sxml_stream_begin_call(dynamic_context, state);
}

/* The calls event needs, just taken by the task (D231): an element's start
   in a shallow window of its own, its end, a leaf's start and end, the
   document's end; then the event is stashed for sxml_stream_next. */
static sxml_status sxml_stream_plan_event(sxml_dynamic_context *dynamic_context,
                                          sxml_stream_state *state,
                                          const sxml_stream_event *event) {
    sxml_stream_document *document = state->document;
    const size_t depth = sxml_stream_depth(document);
    sxml_status status = SXML_STATUS_OK;
    switch (event->event_kind) {
    case SXML_STREAM_EVENT_START_ELEMENT: {
        sxml_item window = {.representation = 0};
        status = sxml_stream_element_window(
            dynamic_context,
            (sxml_stream_element_request){.document = document, .is_full = false}, &window);
        sxml_stream_accumulated *entry =
            status == SXML_STATUS_OK ? sxml_stream_accumulated_at(document, depth) : NULL;
        if (entry == NULL) {
            sxml_item_release(window);
            return status == SXML_STATUS_OK ? SXML_STATUS_OUT_OF_MEMORY : status;
        }
        sxml_stream_clear_accumulated(entry);
        entry->window = window;
        entry->serial = document->event_serial;
        if (document->accumulator_capture != NULL) {
            /* 19.9: whether its end rules read its subtree, before its
               content is read. */
            sxml_item_retain(window);
            sxml_stream_add_call(state, (sxml_stream_call_request){
                                            .accumulation_kind = SXML_STREAM_ACCUMULATION_CAPTURE,
                                            .node = window,
                                            .depth = depth,
                                            .is_leaf = false});
        }
        sxml_item_retain(window);
        sxml_stream_add_call(state, (sxml_stream_call_request){
                                        .accumulation_kind = SXML_STREAM_ACCUMULATION_START,
                                        .node = window,
                                        .depth = depth,
                                        .is_leaf = false});
        break;
    }
    case SXML_STREAM_EVENT_END_ELEMENT: {
        /* The element ended was at depth + 1 (taken, it is removed). */
        if (depth + 1U < document->accumulated.element_count) {
            const sxml_stream_accumulated *entry =
                sxml_xml_array_at(&document->accumulated, depth + 1U);
            if (entry->window.representation != 0) {
                sxml_item_retain(entry->window);
                sxml_stream_add_call(state, (sxml_stream_call_request){
                                                .accumulation_kind = SXML_STREAM_ACCUMULATION_END,
                                                .node = entry->window,
                                                .depth = depth + 1U,
                                                .is_leaf = false});
            }
        }
        break;
    }
    case SXML_STREAM_EVENT_TEXT:
    case SXML_STREAM_EVENT_COMMENT:
    case SXML_STREAM_EVENT_PROCESSING_INSTRUCTION: {
        sxml_item window = {.representation = 0};
        status = sxml_stream_leaf_window(document, event, &window);
        if (status != SXML_STATUS_OK || window.representation == 0) {
            /* A whitespace text node the document's preparation removes is
               no node (4.2.2): it has no events. */
            return status;
        }
        sxml_stream_clear_accumulated(&document->accumulated_leaf);
        document->accumulated_leaf.window = window;
        document->accumulated_leaf.serial = document->event_serial;
        const sxml_stream_accumulation_kind kinds[] = {SXML_STREAM_ACCUMULATION_START,
                                                       SXML_STREAM_ACCUMULATION_END};
        for (size_t kind_index = 0; kind_index < 2; kind_index += 1) {
            sxml_item_retain(window);
            sxml_stream_add_call(state, (sxml_stream_call_request){
                                            .accumulation_kind = kinds[kind_index],
                                            .node = window,
                                            .depth = 0,
                                            .is_leaf = true});
        }
        break;
    }
    case SXML_STREAM_EVENT_END_DOCUMENT:
        if (!document->is_end_accumulated && document->accumulated.element_count > 0) {
            document->is_end_accumulated = true;
            const sxml_stream_accumulated *entry = sxml_xml_array_at(&document->accumulated, 0);
            if (entry->window.representation != 0) {
                sxml_item_retain(entry->window);
                sxml_stream_add_call(state, (sxml_stream_call_request){
                                                .accumulation_kind = SXML_STREAM_ACCUMULATION_END,
                                                .node = entry->window,
                                                .depth = 0,
                                                .is_leaf = false});
            }
        }
        break;
    case SXML_STREAM_EVENT_NONE:
        break;
    }
    if (document->has_accumulation_failed) {
        /* The nodes are known, their values not (D231). */
        for (size_t call_index = 0; call_index < state->call_count; call_index += 1) {
            sxml_item_release(state->calls[call_index].node);
        }
        state->call_count = 0;
    }
    if (state->call_count == 0) {
        return SXML_STATUS_OK;
    }
    state->has_stashed_event = true;
    state->stashed_event = *event;
    return sxml_stream_start_calls(dynamic_context, state, SXML_STREAM_CONTINUE_NEXT);
}

/* Takes the next event; in a document whose accumulators are computed,
   starts the calls it needs (state->is_suspended is then set: the caller
   returns, and the task goes on as state->continuation says). */
static sxml_status sxml_stream_take_event(sxml_dynamic_context *dynamic_context,
                                          sxml_stream_state *state, sxml_stream_event *event) {
    const sxml_status status = sxml_stream_next_event(dynamic_context, state->document, event);
    if (status != SXML_STATUS_OK || !state->document->accumulates) {
        return status;
    }
    return sxml_stream_plan_event(dynamic_context, state, event);
}

/* Skips the rest of the element at depth. With accumulators computed
   (D231) every event is taken for its calls: the task may suspend, and
   resumes the skip after them. */
static sxml_status sxml_stream_skip_to(sxml_dynamic_context *dynamic_context,
                                       sxml_stream_state *state, size_t depth) {
    sxml_stream_document *document = state->document;
    if (!document->accumulates) {
        return sxml_stream_skip_events(dynamic_context, document, depth);
    }
    while (sxml_stream_depth(document) >= depth) {
        sxml_stream_event event;
        const sxml_status status = sxml_stream_take_event(dynamic_context, state, &event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (state->is_suspended) {
            /* Nothing of the event is read: the skip goes on after it. */
            state->has_stashed_event = false;
            state->continuation = SXML_STREAM_CONTINUE_SKIP;
            state->continuation_depth = depth;
            return SXML_STATUS_OK;
        }
        if (event.event_kind == SXML_STREAM_EVENT_END_DOCUMENT) {
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_OK;
}

/* Starts the descend call of a full window of the element at depth (0:
   the document node) whose start was accumulated, going on as
   continuation says after it; nothing when the document computes no
   accumulators. */
typedef struct sxml_stream_descent {
    sxml_item window;
    size_t depth;
    sxml_stream_continuation continuation;
} sxml_stream_descent;

static sxml_status sxml_stream_descend(sxml_dynamic_context *dynamic_context,
                                       sxml_stream_state *state, sxml_stream_descent descent) {
    if (state->document == NULL || !state->document->accumulates ||
        state->document->has_accumulation_failed || descent.window.representation == 0) {
        return SXML_STATUS_OK;
    }
    sxml_item_retain(descent.window);
    sxml_stream_add_call(state, (sxml_stream_call_request){
                                    .accumulation_kind = SXML_STREAM_ACCUMULATION_DESCEND,
                                    .node = descent.window,
                                    .depth = descent.depth,
                                    .is_leaf = false});
    return sxml_stream_start_calls(dynamic_context, state, descent.continuation);
}

/* Sets key to value (borrowed) in the map *slot holds, an empty map when
   it holds none. */
typedef struct sxml_stream_map_put {
    sxml_item_heap *item_heap;
    sxml_item key;
    sxml_sequence value;
} sxml_stream_map_put;

static sxml_status sxml_stream_put(sxml_item *slot, sxml_stream_map_put put) {
    sxml_status status = SXML_STATUS_OK;
    if (slot->representation == 0) {
        status = sxml_map_make(
            put.item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = NULL, .map_entry_count = 0},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = NULL},
            slot);
    }
    sxml_item updated = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_put(
            put.item_heap,
            &(sxml_map_update){.map = *slot, .key = put.key, .value = put.value}, &updated);
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_release(*slot);
        *slot = updated;
    }
    return status;
}

/* INITIAL ended with the values (owned): the document's values, and the
   accumulators' numbers in their order. */
static sxml_status sxml_stream_apply_initial(sxml_stream_document *document,
                                             sxml_sequence value) {
    sxml_item map = {.representation = 0};
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        map = value.content;
    }
    size_t count = 0;
    sxml_status status = map.representation == 0 ? SXML_STATUS_INVALID_ARGUMENT
                                                 : sxml_map_get_size(map, &count);
    void *numbers = NULL;
    void *done = NULL;
    if (status == SXML_STATUS_OK && count > 0) {
        status = sxml_allocator_allocate(
            document->allocator,
            (sxml_allocation_request){.byte_count = count * sizeof(sxml_item),
                                      .alignment = alignof(sxml_item)},
            &numbers);
    }
    if (status == SXML_STATUS_OK && count > 0) {
        status = sxml_allocator_allocate(
            document->allocator,
            (sxml_allocation_request){.byte_count = count * sizeof(bool),
                                      .alignment = alignof(bool)},
            &done);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(document->allocator, numbers);
        sxml_sequence_release(value);
        return status;
    }
    document->accumulator_numbers = numbers;
    document->is_number_done = done;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status == SXML_STATUS_OK) {
            sxml_item_retain(entry.key);
            document->accumulator_numbers[entry_index] = entry.key;
            document->number_count = entry_index + 1U;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_stream_keep_item(&document->accumulator_values, map);
    }
    sxml_sequence_release(value);
    return status;
}

/* The value (owned) one accumulator's call ended with: the document's
   values, and those of the call's node. */
static sxml_status sxml_stream_apply_number(sxml_stream_state *state, sxml_sequence value) {
    sxml_stream_document *document = state->document;
    const sxml_stream_accumulation *call = &state->calls[state->call_next];
    sxml_stream_accumulated *entry = call->is_leaf
                                         ? &document->accumulated_leaf
                                         : sxml_stream_accumulated_at(document, call->depth);
    if (entry == NULL) {
        sxml_sequence_release(value);
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_item_heap *item_heap = document->processor->item_heap;
    const sxml_item number = document->accumulator_numbers[document->number_cursor];
    sxml_sequence new_value = value;
    sxml_status status = SXML_STATUS_OK;
    if (call->accumulation_kind == SXML_STREAM_ACCUMULATION_DESCEND) {
        /* descend-K's map: "v" the value after the subtree. */
        sxml_map_lookup found = {.is_found = false};
        new_value = sxml_sequence_empty();
        if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
            sxml_item_get_kind(value.content) == SXML_ITEM_KIND_MAP) {
            status = sxml_map_find(value.content, document->value_key, &found);
        }
        if (status == SXML_STATUS_OK && found.is_found) {
            new_value = found.entry.value;
        }
        if (status == SXML_STATUS_OK) {
            sxml_stream_keep_item(&entry->full_window, call->node);
            status = sxml_stream_put(&entry->descended, (sxml_stream_map_put){
                                                             .item_heap = item_heap,
                                                             .key = number,
                                                             .value = value});
        }
    } else if (call->accumulation_kind == SXML_STREAM_ACCUMULATION_START) {
        status = sxml_stream_put(&entry->before, (sxml_stream_map_put){
                                                     .item_heap = item_heap,
                                                     .key = number,
                                                     .value = value});
    } else {
        status = sxml_stream_put(&entry->after, (sxml_stream_map_put){
                                                    .item_heap = item_heap,
                                                    .key = number,
                                                    .value = value});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_put(
            &document->accumulator_values,
            (sxml_stream_map_put){.item_heap = item_heap, .key = number, .value = new_value});
    }
    document->is_number_done[document->number_cursor] = true;
    document->has_round_progressed = true;
    sxml_sequence_release(value);
    return status;
}

/* CAPTURE ended with the numbers (owned) of the accumulators whose end
   rules capture the call's element (19.9; packet 7.6g): none, or its
   events are captured from now on into a full window of its own (begun
   with its ancestors and itself, which are open), ended with its end:
   at once when its end came with its start (an empty element). */
static sxml_status sxml_stream_apply_capture(sxml_stream_state *state, sxml_sequence value) {
    sxml_stream_document *document = state->document;
    const sxml_stream_accumulation *call = &state->calls[state->call_next];
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return SXML_STATUS_OK;
    }
    sxml_stream_accumulated *entry = sxml_stream_accumulated_at(document, call->depth);
    if (entry == NULL || call->depth == 0 || call->depth != sxml_stream_depth(document)) {
        sxml_sequence_release(value);
        return entry == NULL ? SXML_STATUS_OUT_OF_MEMORY : SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence_release(entry->captures);
    entry->captures = value;
    void *memory = NULL;
    sxml_status status = sxml_xml_array_push(&document->tees, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_tee *tee = memory;
    tee->depth = call->depth;
    status = sxml_stream_begin_window(document, call->depth, &tee->build);
    if (status != SXML_STATUS_OK) {
        document->tees.element_count -= 1U;
        return status;
    }
    tee->node_index = tee->build.last_element_index;
    if (document->queue_count > 0 &&
        document->queue[document->queue_start].event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
        /* The element is empty: its end is queued, sent to no tee. */
        status = tee->build.builder.operations->end_element(tee->build.builder.state);
        if (status == SXML_STATUS_OK) {
            status = sxml_stream_finish_tee(document);
        }
    }
    return status;
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

/* The selected candidate's body, at its position. */
static sxml_status sxml_stream_select_body(sxml_dynamic_context *dynamic_context,
                                           sxml_stream_state *state) {
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
            /* D231: the values of its subtree, before the body reads them. */
            const sxml_status descended = sxml_stream_descend(
                dynamic_context, state,
                (sxml_stream_descent){.window = state->candidate,
                                      .depth = state->candidate_depth,
                                      .continuation = SXML_STREAM_CONTINUE_SELECT});
            if (descended != SXML_STATUS_OK || state->is_suspended) {
                return descended;
            }
        }
    }
    return sxml_stream_select_body(dynamic_context, state);
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
    return sxml_stream_skip_to(dynamic_context, state, state->candidate_depth);
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
        sxml_status status = SXML_STATUS_OK;
        if (state->has_stashed_event) {
            /* Taken before its accumulator calls (D231). */
            event = state->stashed_event;
            state->has_stashed_event = false;
        } else {
            status = sxml_stream_take_event(dynamic_context, state, &event);
            if (status != SXML_STATUS_OK || state->is_suspended) {
                return status;
            }
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
                status = sxml_stream_skip_to(dynamic_context, state, depth);
                if (status != SXML_STATUS_OK || state->is_suspended) {
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
                if (status != SXML_STATUS_OK || state->is_suspended) {
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
            if (state->stream->axis == SXML_AXIS_CHILD && event.depth != state->base_depth) {
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
        if (status != SXML_STATUS_OK || state->is_suspended) {
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
        if (status != SXML_STATUS_OK || state->is_suspended) {
            return status;
        }
    }
    return sxml_stream_next(dynamic_context, state);
}

/* The body ended: what is left of the selected node, then the next. */
static sxml_status sxml_stream_take_body(sxml_dynamic_context *dynamic_context,
                                         sxml_stream_state *state) {
    sxml_status status = SXML_STATUS_OK;
    if (state->stream->fold.value != 0) {
        /* The aggregate so far, which the next body reads (D223). */
        sxml_evaluator_machine_bind_slot(dynamic_context, state->stream->fold,
                                         sxml_evaluator_machine_take_value(dynamic_context));
    } else {
        status = sxml_stream_append(dynamic_context, state,
                                    sxml_evaluator_machine_take_value(dynamic_context));
    }
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
            /* The candidate goes first: the skip may suspend (D231). */
            sxml_item_release(state->candidate);
            state->candidate = (sxml_item){.representation = 0};
            status = sxml_stream_skip_to(dynamic_context, state, state->candidate_depth);
            if (status != SXML_STATUS_OK || state->is_suspended) {
                return status;
            }
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

/* OPENS_DOCUMENT: the body, once, with the document node as its focus. */
static sxml_status sxml_stream_push_document_body(sxml_dynamic_context *dynamic_context,
                                                  sxml_stream_state *state) {
    return sxml_stream_push_focused(dynamic_context, state,
                                    (sxml_stream_push){.expression = state->stream->body,
                                                       .position = 1,
                                                       .phase = SXML_STREAM_PHASE_BODY});
}

/* OPENS_DOCUMENT with accumulators (D231): the document computes them as
   its events pass, from the initial values and the document node's start,
   whose calls are added. */
static sxml_status sxml_stream_start_accumulators(sxml_dynamic_context *dynamic_context,
                                                  sxml_stream_state *state) {
    sxml_stream_document *document = state->document;
    const sxml_ir_unit *unit = sxml_evaluator_machine_top(dynamic_context)->function->unit;
    const sxml_ir_stream_accumulators *functions = &state->stream->accumulators;
    document->accumulates = true;
    document->accumulator_initial = sxml_ir_unit_get_function(unit, functions->initial);
    document->accumulator_start = sxml_ir_unit_get_function(unit, functions->start);
    document->accumulator_end = sxml_ir_unit_get_function(unit, functions->end);
    document->accumulator_descend = sxml_ir_unit_get_function(unit, functions->descend);
    document->accumulator_capture = functions->capture.value != 0
                                        ? sxml_ir_unit_get_function(unit, functions->capture)
                                        : NULL;
    sxml_status status = sxml_atomic_item_make(
        document->processor->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = SXML_BYTE_VIEW_FROM_LITERAL("v"),
                                                   .codepoint_count = 1,
                                                   .is_ascii = true}}}},
        &document->value_key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_accumulated *entry = sxml_stream_accumulated_at(document, 0);
    if (entry == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    sxml_stream_keep_item(&entry->window, state->candidate);
    entry->serial = document->event_serial;
    const sxml_stream_accumulation_kind kinds[] = {SXML_STREAM_ACCUMULATION_INITIAL,
                                                   SXML_STREAM_ACCUMULATION_START};
    for (size_t kind_index = 0; kind_index < 2; kind_index += 1) {
        sxml_item_retain(state->candidate);
        sxml_stream_add_call(state, (sxml_stream_call_request){.accumulation_kind = kinds[kind_index],
                                                               .node = state->candidate,
                                                               .depth = 0,
                                                               .is_leaf = false});
    }
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
        if (status == SXML_STATUS_OK && state->stream->accumulators.initial.value != 0) {
            status = sxml_stream_start_accumulators(dynamic_context, state);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->candidate_source = SXML_STREAM_CANDIDATE_LEAF;
    state->selected_count = 1;
    status = sxml_stream_start_calls(dynamic_context, state, SXML_STREAM_CONTINUE_DOCUMENT_BODY);
    if (status != SXML_STATUS_OK || state->is_suspended) {
        return status;
    }
    return sxml_stream_push_document_body(dynamic_context, state);
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

/* A grounding stream's body, in the focus bound. */
static sxml_status sxml_stream_push_grounding_body(sxml_dynamic_context *dynamic_context,
                                                   const sxml_stream_state *state) {
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->phase = (uint32_t)SXML_STREAM_PHASE_BODY;
    if (task->delivers_content) {
        return sxml_evaluator_machine_push_content(dynamic_context, state->stream->body);
    }
    return sxml_evaluator_machine_push_expression(dynamic_context, state->stream->body);
}

/* A grounding stream (axis SELF, D228): the body once, in the focus of
   origin's full window when origin is the open node (its content read
   now, so whoever opened it skips nothing), else of origin. */
static sxml_status sxml_stream_start_grounding(sxml_dynamic_context *dynamic_context,
                                               sxml_stream_state *state) {
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status =
        sxml_evaluator_machine_read_atom(dynamic_context, state->stream->origin, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_stream_document *document = NULL;
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        document = sxml_stream_find_document(dynamic_context, value.content);
    }
    state->candidate_source = SXML_STREAM_CANDIDATE_LEAF;
    state->selected_count = 1;
    const sxml_ir_focus focus = state->stream->focus;
    if (document == NULL) {
        if (focus.context_value.value != 0) {
            sxml_sequence_retain(value);
            sxml_evaluator_machine_bind_slot(dynamic_context, focus.context_value, value);
        }
    } else {
        document->is_open_node_consumed = true;
        if (document->open_depth == 0) {
            status = sxml_stream_document_window(dynamic_context, document, &state->candidate);
        } else if (sxml_stream_depth(document) == document->open_depth) {
            status = sxml_stream_element_window(
                dynamic_context,
                (sxml_stream_element_request){.document = document, .is_full = true},
                &state->candidate);
        } else {
            /* Content of the open node was read before: no window can
               hold it all. */
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (focus.context_value.value != 0) {
            sxml_item_retain(state->candidate);
            sxml_evaluator_machine_bind_slot(dynamic_context, focus.context_value,
                                             sxml_sequence_from_item(state->candidate));
        }
        /* D231: the values of the window's nodes, before the body reads
           them (the document is not this task's). */
        state->document = document;
        status = sxml_stream_descend(
            dynamic_context, state,
            (sxml_stream_descent){.window = state->candidate,
                                  .depth = document->open_depth,
                                  .continuation = SXML_STREAM_CONTINUE_GROUNDING_BODY});
        if (status != SXML_STATUS_OK || state->is_suspended) {
            return status;
        }
    }
    return sxml_stream_push_grounding_body(dynamic_context, state);
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
    if (state->stream->axis == SXML_AXIS_SELF) {
        return sxml_stream_start_grounding(dynamic_context, state);
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

/* Keeps a copy of the error a call failed with (the error record's,
   status caught) for the reads of the values it leaves out (D231). */
static sxml_status sxml_stream_keep_failure(const sxml_dynamic_context *dynamic_context,
                                            sxml_stream_state *state, sxml_status caught) {
    sxml_stream_document *document = state->document;
    document->has_accumulation_failed = true;
    const sxml_stream_accumulation *call = &state->calls[state->call_next];
    if (call->accumulation_kind == SXML_STREAM_ACCUMULATION_DESCEND && !call->is_leaf) {
        /* Its nodes are known, their values not. */
        sxml_stream_accumulated *entry = sxml_stream_accumulated_at(document, call->depth);
        if (entry != NULL) {
            sxml_stream_keep_item(&entry->full_window, call->node);
        }
    }
    sxml_error_code code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("FOER0000"),
                            .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")};
    sxml_byte_view description = SXML_BYTE_VIEW_FROM_LITERAL("An accumulator rule failed");
    const sxml_error_record *record = dynamic_context->error_record;
    if (record != NULL && record->code.local_name.byte_count > 0) {
        code = record->code;
        description = record->diagnostic.description;
    }
    const sxml_byte_view parts[] = {code.namespace_uri, code.local_name, code.namespace_prefix,
                                    description};
    sxml_byte_range ranges[sizeof(parts) / sizeof(parts[0])];
    sxml_status status = SXML_STATUS_OK;
    for (size_t part_index = 0;
         status == SXML_STATUS_OK && part_index < sizeof(parts) / sizeof(parts[0]);
         part_index += 1) {
        status = sxml_stream_keep(&document->failure_text, parts[part_index], &ranges[part_index]);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document->failure = (sxml_error_raise_request){
        .error_status = caught,
        .code = {.namespace_uri = sxml_xml_buffer_view(&document->failure_text, ranges[0]),
                 .local_name = sxml_xml_buffer_view(&document->failure_text, ranges[1]),
                 .namespace_prefix = sxml_xml_buffer_view(&document->failure_text, ranges[2])},
        .description = sxml_xml_buffer_view(&document->failure_text, ranges[3]),
        .has_value = false,
        .value = sxml_sequence_empty(),
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
    return SXML_STATUS_OK;
}

/* An accumulator call ended (D231): its value kept, the next accumulator
   or call, or what the task was doing before them. A rule that read a
   value not computed yet makes its accumulator wait a round; any other
   error is kept for the reads (19.5) and ends the calls. */
static sxml_status sxml_stream_take_accumulation(sxml_dynamic_context *dynamic_context,
                                                 sxml_stream_state *state) {
    sxml_evaluator_machine_top(dynamic_context)->catches_errors = false;
    sxml_stream_document *document = state->document;
    const sxml_status caught = sxml_evaluator_machine_take_caught_status(dynamic_context);
    const sxml_stream_accumulation_kind kind = state->calls[state->call_next].accumulation_kind;
    const bool is_initial = sxml_stream_is_whole_call(&state->calls[state->call_next]);
    const bool was_waiting = document->is_waiting;
    document->is_waiting = false;
    sxml_status status = SXML_STATUS_OK;
    if (caught != SXML_STATUS_OK && was_waiting && !is_initial && !document->is_final_round) {
        /* Computed again after the others. */
        if (sxml_stream_next_number(document)) {
            return sxml_stream_push_accumulation(dynamic_context, state);
        }
    } else if (caught != SXML_STATUS_OK) {
        status = sxml_stream_keep_failure(dynamic_context, state, caught);
        for (size_t call_index = state->call_next + 1U; call_index < state->call_count;
             call_index += 1) {
            sxml_item_release(state->calls[call_index].node);
            state->calls[call_index].node = (sxml_item){.representation = 0};
        }
        state->call_count = state->call_next + 1U;
    } else if (kind == SXML_STREAM_ACCUMULATION_INITIAL) {
        status = sxml_stream_apply_initial(document, sxml_evaluator_machine_take_value(dynamic_context));
    } else if (kind == SXML_STREAM_ACCUMULATION_CAPTURE) {
        status = sxml_stream_apply_capture(state, sxml_evaluator_machine_take_value(dynamic_context));
    } else {
        status = sxml_stream_apply_number(state, sxml_evaluator_machine_take_value(dynamic_context));
        if (status == SXML_STATUS_OK && sxml_stream_next_number(document)) {
            return sxml_stream_push_accumulation(dynamic_context, state);
        }
    }
    document->is_computing = false;
    sxml_item_release(state->calls[state->call_next].node);
    state->calls[state->call_next].node = (sxml_item){.representation = 0};
    state->call_next += 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (state->call_next < state->call_count) {
        return sxml_stream_begin_call(dynamic_context, state);
    }
    state->call_count = 0;
    state->call_next = 0;
    state->is_suspended = false;
    switch (state->continuation) {
    case SXML_STREAM_CONTINUE_NEXT:
        sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_STREAM_PHASE_NEXT;
        return sxml_stream_next(dynamic_context, state);
    case SXML_STREAM_CONTINUE_SKIP:
        sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_STREAM_PHASE_NEXT;
        status = sxml_stream_skip_to(dynamic_context, state, state->continuation_depth);
        if (status != SXML_STATUS_OK || state->is_suspended) {
            return status;
        }
        return sxml_stream_next(dynamic_context, state);
    case SXML_STREAM_CONTINUE_SELECT:
        return sxml_stream_select_body(dynamic_context, state);
    case SXML_STREAM_CONTINUE_DOCUMENT_BODY:
        return sxml_stream_push_document_body(dynamic_context, state);
    case SXML_STREAM_CONTINUE_GROUNDING_BODY:
        return sxml_stream_push_grounding_body(dynamic_context, state);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
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
        if (state->stream->axis == SXML_AXIS_SELF) {
            /* A grounding stream's one body ended. */
            const sxml_status status = sxml_stream_append(
                dynamic_context, state, sxml_evaluator_machine_take_value(dynamic_context));
            return status == SXML_STATUS_OK ? sxml_stream_finish(dynamic_context, state) : status;
        }
        return sxml_stream_take_body(dynamic_context, state);
    case SXML_STREAM_PHASE_ACCUMULATED:
        return sxml_stream_take_accumulation(dynamic_context, state);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------- MERGE */

/*
 * MERGE (src/ir_internal.h; XSLT 4.0, 15.3 to 15.6; packet 7.6g): the
 * cursors of a merge read in step. A cursor of a merge stream is the
 * stream builder of one streamed document (as an OPENS_DOCUMENT STREAM
 * opens it) read by the task itself, one selected node at a time: the
 * select's child steps are matched against the open elements, content
 * no step can select is skipped, and a selected element is grounded in a
 * full window (19.4) before its tuple is computed. A document the
 * resolver gives as a tree is walked instead. So a merge of streamed
 * documents holds, per cursor, the open elements and one window.
 */

static const sxml_static_error sxml_merge_order_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTDE2220"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The input of a merge source is not in merge-key order (XSLT 4.0, 15.4)"),
};

/* The most steps of a streamed merge source's select. */
#define MERGE_MAXIMUM_STEP_COUNT 16U

/* One merge stream, read from its IR: the OPENS_DOCUMENT STREAM and the
   chain of steps under it. */
typedef struct sxml_merge_source {
    const sxml_ir_stream *document_stream;
    const sxml_ir_stream *steps[MERGE_MAXIMUM_STEP_COUNT];
    sxml_stream_test tests[MERGE_MAXIMUM_STEP_COUNT];
    size_t step_count;
} sxml_merge_source;

/* One cursor: of the grounded tuples, or of one anchor document. */
typedef struct sxml_merge_cursor {
    bool is_grounded;
    size_t source_index;
    uint64_t anchor_position;
    /* Owned: the streamed document, NULL once read to its end or when
       the document is a tree; its document node (a window, or the tree's). */
    sxml_stream_document *document;
    sxml_item document_node;
    /* Streamed: how many steps the open elements match. */
    size_t matched_depth;
    /* A tree: the child iterators of the nodes matched, by step. */
    sxml_axis_iterator iterators[MERGE_MAXIMUM_STEP_COUNT];
    size_t iterator_count;
    uint64_t selected_count;
    /* Owned: the current tuple; none once the cursor has ended. */
    sxml_item head;
    bool has_ended;
} sxml_merge_cursor;

/* A tuple of the open group, with the order 15.6 gives it a place by. */
typedef struct sxml_merge_member {
    sxml_item tuple;
    int64_t source_number;
    int64_t anchor_position;
} sxml_merge_member;

typedef enum sxml_merge_phase {
    SXML_MERGE_PHASE_START = 0,
    /* Moves the cursors waiting on; then the order. */
    SXML_MERGE_PHASE_ADVANCE,
    /* The tuple of a cursor's selected node ended. */
    SXML_MERGE_PHASE_TUPLE,
    /* The order of the heads ended. */
    SXML_MERGE_PHASE_ORDERED,
    /* A group's body ended. */
    SXML_MERGE_PHASE_BODY,
} sxml_merge_phase;

typedef struct sxml_merge_state {
    const sxml_ir_merge *merge;
    sxml_merge_source *sources;
    size_t source_count;
    sxml_merge_cursor *cursors;
    size_t cursor_count;
    /* The grounded tuples (owned) and where the grounded cursor is. */
    sxml_sequence grounded;
    sxml_sequence_iterator grounded_iterator;
    /* The cursors to move on, in order: pending[pending_next] first. */
    size_t *pending;
    size_t pending_count;
    size_t pending_next;
    /* The open group: its tuples (owned) and key (owned). */
    sxml_merge_member *members;
    size_t member_count;
    size_t member_capacity;
    sxml_sequence group_key;
    bool has_group;
    uint64_t group_count;
    /* Owned; made on the first value. */
    sxml_sequence_builder *builder;
} sxml_merge_state;

static void sxml_merge_clear_members(sxml_merge_state *state) {
    for (size_t member_index = 0; member_index < state->member_count; member_index += 1) {
        sxml_item_release(state->members[member_index].tuple);
    }
    state->member_count = 0;
    sxml_sequence_release(state->group_key);
    state->group_key = sxml_sequence_empty();
    state->has_group = false;
}

static void sxml_merge_close_cursor(sxml_merge_cursor *cursor) {
    sxml_stream_document_destroy(cursor->document);
    cursor->document = NULL;
    sxml_item_release(cursor->document_node);
    cursor->document_node = (sxml_item){.representation = 0};
    cursor->iterator_count = 0;
}

static void sxml_merge_release_state(sxml_dynamic_context *dynamic_context, void *state) {
    sxml_merge_state *merge_state = state;
    const sxml_allocator allocator = dynamic_context->allocator;
    for (size_t cursor_index = 0; cursor_index < merge_state->cursor_count; cursor_index += 1) {
        sxml_merge_cursor *cursor = &merge_state->cursors[cursor_index];
        sxml_merge_close_cursor(cursor);
        sxml_item_release(cursor->head);
    }
    sxml_merge_clear_members(merge_state);
    sxml_sequence_release(merge_state->grounded);
    sxml_sequence_builder_destroy(merge_state->builder);
    sxml_allocator_release(allocator, merge_state->sources);
    sxml_allocator_release(allocator, merge_state->cursors);
    sxml_allocator_release(allocator, merge_state->pending);
    sxml_allocator_release(allocator, merge_state->members);
    sxml_allocator_release(allocator, merge_state);
}

/* Allocator memory for count elements of size bytes (at least one),
   zeroed. */
typedef struct sxml_merge_array_request {
    size_t element_count;
    size_t element_size;
} sxml_merge_array_request;

static sxml_status sxml_merge_allocate(const sxml_dynamic_context *dynamic_context,
                                       sxml_merge_array_request request, void **memory) {
    *memory = NULL;
    const size_t count = request.element_count == 0 ? 1 : request.element_count;
    const sxml_status status = sxml_allocator_allocate_array(
        dynamic_context->allocator,
        (sxml_array_allocation_request){.element_count = count,
                                        .element_size = request.element_size,
                                        .element_alignment = alignof(max_align_t)},
        memory);
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, count * request.element_size);
    }
    return status;
}

/* Reads the merge stream's chain into source (src/ir_internal.h,
   sxml_ir_merge_stream). */
typedef struct sxml_merge_source_reading {
    const sxml_ir_unit *unit;
    sxml_ir_expression_index stream;
} sxml_merge_source_reading;

static sxml_status sxml_merge_read_source(const sxml_dynamic_context *dynamic_context,
                                          sxml_merge_source_reading reading,
                                          sxml_merge_source *source) {
    const sxml_ir_expression *document = sxml_ir_unit_get_expression(reading.unit, reading.stream);
    if (document->expression_kind != SXML_IR_EXPRESSION_KIND_STREAM ||
        (document->stream.flags & (uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT) == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    source->document_stream = &document->stream;
    source->step_count = 0;
    sxml_ir_expression_index next = document->stream.body;
    for (;;) {
        const sxml_ir_expression *step = sxml_ir_unit_get_expression(reading.unit, next);
        if (step->expression_kind != SXML_IR_EXPRESSION_KIND_STREAM) {
            break;
        }
        if (source->step_count == MERGE_MAXIMUM_STEP_COUNT || step->stream.axis != SXML_AXIS_CHILD ||
            step->stream.predicate.value != 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        const sxml_status status = sxml_stream_resolve_test(
            dynamic_context, step->stream.node_test, &source->tests[source->step_count]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        source->steps[source->step_count] = &step->stream;
        source->step_count += 1;
        if ((step->stream.flags & (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS) == 0) {
            /* The last step's body is the tuple. */
            break;
        }
        next = step->stream.body;
    }
    return source->step_count == 0 ? SXML_STATUS_INVALID_ARGUMENT : SXML_STATUS_OK;
}

/* Opens the cursor's document from uri (an xs:string item), bound to the
   origin of its source's document STREAM first. */
typedef struct sxml_merge_opening {
    sxml_merge_cursor *cursor;
    const sxml_merge_source *source;
    sxml_item uri;
} sxml_merge_opening;

static sxml_status sxml_merge_open(sxml_dynamic_context *dynamic_context,
                                   sxml_merge_opening opening) {
    sxml_merge_cursor *cursor = opening.cursor;
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_atomic_item_get_datum(opening.uri, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_evaluator_machine_raise(dynamic_context, &sxml_stream_no_document_uri_error);
    }
    sxml_item_retain(opening.uri);
    sxml_evaluator_machine_bind_slot(dynamic_context, opening.source->document_stream->origin.slot,
                                     sxml_sequence_from_item(opening.uri));
    const sxml_byte_view uri_reference = datum.datum_details.string.utf8;
    sxml_status status =
        sxml_stream_document_open(dynamic_context, uri_reference, &cursor->document);
    if (status == SXML_STATUS_OK && cursor->document == NULL) {
        /* No processor's loader: fn:doc's tree. */
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
            &cursor->document_node);
    } else if (status == SXML_STATUS_OK && !cursor->document->source.is_streamed) {
        cursor->document_node = cursor->document->source.document_node;
        sxml_item_retain(cursor->document_node);
        sxml_stream_document_destroy(cursor->document);
        cursor->document = NULL;
    } else if (status == SXML_STATUS_OK) {
        sxml_stream_window_build build;
        status = sxml_stream_begin_window(cursor->document, 0, &build);
        if (status == SXML_STATUS_OK) {
            status = sxml_stream_end_window(
                cursor->document,
                (sxml_stream_window_end){.build = &build, .open_count = 0, .node_index = 0},
                &cursor->document_node);
        }
        return status;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A tree: the children of its document node first. */
    sxml_node_reference node;
    status = sxml_item_get_node(cursor->document_node, &node);
    if (status == SXML_STATUS_OK) {
        status = sxml_axis_iterator_start(
            (sxml_axis_request){.origin = node, .axis = SXML_AXIS_CHILD}, &cursor->iterators[0]);
    }
    cursor->iterator_count = status == SXML_STATUS_OK ? 1U : 0U;
    return status;
}

/* Whether the element on top of the open elements matches test. */
typedef struct sxml_merge_element_question {
    sxml_stream_document *document;
    const sxml_stream_test *test;
} sxml_merge_element_question;

static sxml_status sxml_merge_element_matches(sxml_dynamic_context *dynamic_context,
                                              sxml_merge_element_question question,
                                              bool *matches) {
    sxml_stream_document *document = question.document;
    if (question.test->is_simple) {
        const sxml_stream_element *element =
            sxml_stream_element_at(document, sxml_stream_depth(document) - 1U);
        return sxml_stream_test_matches(
            dynamic_context,
            (sxml_stream_candidate_kind){.test = question.test,
                                         .node_kind = SXML_NODE_KIND_ELEMENT,
                                         .node_name = element->event.element_name},
            matches);
    }
    sxml_item window = {.representation = 0};
    sxml_status status = sxml_stream_element_window(
        dynamic_context, (sxml_stream_element_request){.document = document, .is_full = false},
        &window);
    if (status == SXML_STATUS_OK) {
        status = sxml_stream_node_matches(
            dynamic_context,
            (sxml_stream_node_question){.test = question.test, .node_item = window}, matches);
    }
    sxml_item_release(window);
    return status;
}

/* The cursor's next selected node of a streamed document, owned in its
   full window, or none at the document's end. */
typedef struct sxml_merge_reading {
    const sxml_merge_source *source;
    sxml_merge_cursor *cursor;
} sxml_merge_reading;

static sxml_status sxml_merge_next_event_node(sxml_dynamic_context *dynamic_context,
                                              sxml_merge_reading reading, sxml_item *node) {
    const sxml_merge_source *source = reading.source;
    sxml_merge_cursor *cursor = reading.cursor;
    sxml_stream_document *document = cursor->document;
    *node = (sxml_item){.representation = 0};
    for (;;) {
        sxml_stream_event event;
        sxml_status status = sxml_stream_next_event(dynamic_context, document, &event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const size_t depth = sxml_stream_depth(document);
        if (event.event_kind == SXML_STREAM_EVENT_END_DOCUMENT) {
            return SXML_STATUS_OK;
        }
        if (event.event_kind == SXML_STREAM_EVENT_END_ELEMENT) {
            cursor->matched_depth = cursor->matched_depth > depth ? depth : cursor->matched_depth;
            continue;
        }
        if (event.event_kind != SXML_STREAM_EVENT_START_ELEMENT) {
            continue;
        }
        bool matches = false;
        if (depth == cursor->matched_depth + 1U && depth <= source->step_count) {
            status = sxml_merge_element_matches(
                dynamic_context,
                (sxml_merge_element_question){.document = document,
                                              .test = &source->tests[depth - 1U]},
                &matches);
        }
        if (status == SXML_STATUS_OK && !matches) {
            /* No step selects anything inside it. */
            status = sxml_stream_skip_events(dynamic_context, document, depth);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!matches) {
            continue;
        }
        if (depth < source->step_count) {
            cursor->matched_depth = depth;
            continue;
        }
        return sxml_stream_element_window(
            dynamic_context, (sxml_stream_element_request){.document = document, .is_full = true},
            node);
    }
}

/* The cursor's next selected node of a tree, or none at its end: the
   child iterators of the nodes the steps matched, deepest last. */
static sxml_status sxml_merge_next_tree_node(sxml_dynamic_context *dynamic_context,
                                             sxml_merge_reading reading, sxml_item *node) {
    const sxml_merge_source *source = reading.source;
    sxml_merge_cursor *cursor = reading.cursor;
    *node = (sxml_item){.representation = 0};
    while (cursor->iterator_count > 0) {
        const size_t step_index = cursor->iterator_count - 1U;
        sxml_node_iteration_step step = {.has_node = false};
        sxml_status status = sxml_axis_iterator_next(&cursor->iterators[step_index], &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_node) {
            cursor->iterator_count -= 1U;
            continue;
        }
        sxml_item item = sxml_node_item_make(step.node);
        bool matches = false;
        status = sxml_stream_node_matches(
            dynamic_context,
            (sxml_stream_node_question){.test = &source->tests[step_index], .node_item = item},
            &matches);
        if (status != SXML_STATUS_OK || !matches) {
            sxml_item_release(item);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (step_index + 1U == source->step_count) {
            *node = item;
            return SXML_STATUS_OK;
        }
        status = sxml_axis_iterator_start(
            (sxml_axis_request){.origin = step.node, .axis = SXML_AXIS_CHILD},
            &cursor->iterators[step_index + 1U]);
        sxml_item_release(item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        cursor->iterator_count += 1U;
    }
    return SXML_STATUS_OK;
}

/* Pushes the tuple of the cursor's node (owned): its source's last body,
   with the focus of the node and of its document bound. */
typedef struct sxml_merge_tuple_request {
    const sxml_merge_source *source;
    sxml_merge_cursor *cursor;
    sxml_item node;
} sxml_merge_tuple_request;

static sxml_status sxml_merge_push_tuple(sxml_dynamic_context *dynamic_context,
                                         sxml_merge_tuple_request request) {
    const sxml_merge_source *source = request.source;
    sxml_merge_cursor *cursor = request.cursor;
    const sxml_ir_focus document_focus = source->document_stream->focus;
    const sxml_ir_stream *last = source->steps[source->step_count - 1U];
    cursor->selected_count += 1;
    if (document_focus.context_value.value != 0) {
        sxml_item_retain(cursor->document_node);
        sxml_evaluator_machine_bind_slot(dynamic_context, document_focus.context_value,
                                         sxml_sequence_from_item(cursor->document_node));
    }
    if (document_focus.context_position.value != 0) {
        sxml_evaluator_machine_bind_slot(
            dynamic_context, document_focus.context_position,
            sxml_sequence_from_item(
                sxml_atomic_item_from_integer((int64_t)cursor->anchor_position)));
    }
    if (last->focus.context_position.value != 0) {
        sxml_evaluator_machine_bind_slot(
            dynamic_context, last->focus.context_position,
            sxml_sequence_from_item(
                sxml_atomic_item_from_integer((int64_t)cursor->selected_count)));
    }
    sxml_evaluator_machine_bind_slot(dynamic_context, last->focus.context_value,
                                     sxml_sequence_from_item(request.node));
    sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_MERGE_PHASE_TUPLE;
    return sxml_evaluator_machine_push_expression(dynamic_context, last->body);
}

static sxml_status sxml_merge_order(sxml_dynamic_context *dynamic_context,
                                    sxml_merge_state *state);

/* Moves the cursors waiting on: a grounded one to its next tuple, a
   streamed one to its next node, whose tuple is pushed (the task goes on
   in phase TUPLE); the order once none waits. */
static sxml_status sxml_merge_advance(sxml_dynamic_context *dynamic_context,
                                      sxml_merge_state *state) {
    while (state->pending_next < state->pending_count) {
        sxml_merge_cursor *cursor = &state->cursors[state->pending[state->pending_next]];
        sxml_item_release(cursor->head);
        cursor->head = (sxml_item){.representation = 0};
        if (cursor->is_grounded) {
            sxml_iteration_step step = {.has_item = false};
            const sxml_status status = sxml_sequence_iterator_next(
                &state->grounded_iterator, dynamic_context->error_record, &step);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (step.has_item) {
                sxml_item_retain(step.item);
                cursor->head = step.item;
            } else {
                cursor->has_ended = true;
            }
            state->pending_next += 1;
            continue;
        }
        const sxml_merge_source *source = &state->sources[cursor->source_index];
        sxml_item node = {.representation = 0};
        const sxml_merge_reading reading = {.source = source, .cursor = cursor};
        const sxml_status status =
            cursor->document != NULL
                ? sxml_merge_next_event_node(dynamic_context, reading, &node)
                : sxml_merge_next_tree_node(dynamic_context, reading, &node);
        if (status != SXML_STATUS_OK) {
            sxml_item_release(node);
            return status;
        }
        if (node.representation == 0) {
            /* Read to its end (it was well-formed): its memory goes. */
            cursor->has_ended = true;
            sxml_merge_close_cursor(cursor);
            state->pending_next += 1;
            continue;
        }
        return sxml_merge_push_tuple(
            dynamic_context,
            (sxml_merge_tuple_request){.source = source, .cursor = cursor, .node = node});
    }
    state->pending_count = 0;
    state->pending_next = 0;
    return sxml_merge_order(dynamic_context, state);
}

/* The tuple of a cursor's node ended: its head, then the next cursor. */
static sxml_status sxml_merge_take_tuple(sxml_dynamic_context *dynamic_context,
                                         sxml_merge_state *state) {
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    sxml_merge_cursor *cursor = &state->cursors[state->pending[state->pending_next]];
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        sxml_sequence_release(value);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    cursor->head = value.content;
    state->pending_next += 1;
    sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_MERGE_PHASE_ADVANCE;
    return sxml_merge_advance(dynamic_context, state);
}

/* The order of the heads: the open group's first tuple, then each
   cursor's; the task finishes when there is none. */
static sxml_status sxml_merge_order(sxml_dynamic_context *dynamic_context,
                                    sxml_merge_state *state) {
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence_builder *heads = NULL;
    size_t head_count = 0;
    status = sxml_sequence_builder_create(
        dynamic_context->allocator,
        &(sxml_sequence_builder_configuration){.item_heap = dynamic_context->item_heap}, &heads);
    if (status == SXML_STATUS_OK && state->has_group) {
        status = sxml_sequence_builder_append_item(heads, state->members[0].tuple);
        head_count += 1;
    }
    for (size_t cursor_index = 0; status == SXML_STATUS_OK && cursor_index < state->cursor_count;
         cursor_index += 1) {
        const sxml_merge_cursor *cursor = &state->cursors[cursor_index];
        if (cursor->head.representation != 0) {
            status = sxml_sequence_builder_append_item(heads, cursor->head);
            head_count += 1;
        }
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(heads, &value);
    }
    sxml_sequence_builder_destroy(heads);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (head_count == 0) {
        sxml_sequence_release(value);
        sxml_sequence result = sxml_sequence_empty();
        if (state->builder != NULL) {
            status = sxml_sequence_builder_finish(state->builder, &result);
        }
        return status == SXML_STATUS_OK ? sxml_evaluator_machine_finish(dynamic_context, result)
                                        : status;
    }
    sxml_evaluator_machine_bind_slot(dynamic_context, state->merge->heads, value);
    sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_MERGE_PHASE_ORDERED;
    return sxml_evaluator_machine_push_expression(dynamic_context, state->merge->order);
}

/* The integer member at member_index of tuple; 0 when it is none. */
typedef struct sxml_merge_member_read {
    sxml_item tuple;
    size_t member_index;
} sxml_merge_member_read;

static int64_t sxml_merge_integer_member(sxml_merge_member_read read) {
    sxml_sequence member = sxml_sequence_empty();
    if (sxml_array_get_member(read.tuple, read.member_index, &member) != SXML_STATUS_OK ||
        sxml_sequence_get_representation(member) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_representation(member.content) != SXML_ITEM_REPRESENTATION_INTEGER) {
        return 0;
    }
    return member.content.payload.integer;
}

/* Whether two tuples (arrays, so cells) are one item: the order passes
   the heads on, it does not copy them. */
typedef struct sxml_merge_tuple_pair {
    sxml_item first;
    sxml_item second;
} sxml_merge_tuple_pair;

static bool sxml_merge_is_same_tuple(sxml_merge_tuple_pair pair) {
    return pair.first.representation != 0 &&
           pair.first.representation == pair.second.representation &&
           pair.first.payload.cell == pair.second.payload.cell;
}

/* Adds tuple (borrowed) to the open group and has the cursor whose head
   it is move on. */
static sxml_status sxml_merge_take_head(sxml_dynamic_context *dynamic_context,
                                        sxml_merge_state *state, sxml_item tuple) {
    size_t cursor_index = 0;
    while (cursor_index < state->cursor_count &&
           !sxml_merge_is_same_tuple((sxml_merge_tuple_pair){
               .first = state->cursors[cursor_index].head, .second = tuple})) {
        cursor_index += 1;
    }
    if (cursor_index == state->cursor_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (state->member_count == state->member_capacity) {
        const size_t capacity = state->member_capacity == 0 ? 8 : state->member_capacity * 2;
        void *memory = NULL;
        const sxml_status status = sxml_merge_allocate(
            dynamic_context, (sxml_merge_array_request){.element_count = capacity,
                                                        .element_size = sizeof(sxml_merge_member)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (state->member_count > 0) {
            memcpy(memory, state->members, state->member_count * sizeof(sxml_merge_member));
        }
        sxml_allocator_release(dynamic_context->allocator, state->members);
        state->members = memory;
        state->member_capacity = capacity;
    }
    size_t member_count = 0;
    if (sxml_array_get_size(tuple, &member_count) != SXML_STATUS_OK || member_count < 2) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_item_retain(tuple);
    state->members[state->member_count] = (sxml_merge_member){
        .tuple = tuple,
        .source_number =
            sxml_merge_integer_member((sxml_merge_member_read){.tuple = tuple, .member_index = 0}),
        .anchor_position = sxml_merge_integer_member(
            (sxml_merge_member_read){.tuple = tuple, .member_index = member_count - 2U})};
    state->member_count += 1;
    state->pending[state->pending_count] = cursor_index;
    state->pending_count += 1;
    return SXML_STATUS_OK;
}

/* Closes the open group: its tuples in source, anchor and input order
   (15.6: a stable sort, each cursor's tuples taken in order), then the
   body with the group array [tuples, key] and its number. */
static sxml_status sxml_merge_push_body(sxml_dynamic_context *dynamic_context,
                                        sxml_merge_state *state) {
    for (size_t member_index = 1; member_index < state->member_count; member_index += 1) {
        const sxml_merge_member moved = state->members[member_index];
        size_t place = member_index;
        while (place > 0 &&
               (state->members[place - 1U].source_number > moved.source_number ||
                (state->members[place - 1U].source_number == moved.source_number &&
                 state->members[place - 1U].anchor_position > moved.anchor_position))) {
            state->members[place] = state->members[place - 1U];
            place -= 1U;
        }
        state->members[place] = moved;
    }
    void *memory = NULL;
    sxml_status status = sxml_merge_allocate(
        dynamic_context, (sxml_merge_array_request){.element_count = state->member_count,
                                                    .element_size = sizeof(sxml_item)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *items = memory;
    for (size_t member_index = 0; member_index < state->member_count; member_index += 1) {
        items[member_index] = state->members[member_index].tuple;
    }
    sxml_sequence members[2] = {sxml_sequence_empty(), state->group_key};
    status = sxml_sequence_make_vector(
        dynamic_context->item_heap,
        (sxml_item_view){.items = items, .item_count = state->member_count}, &members[0]);
    sxml_allocator_release(dynamic_context->allocator, items);
    sxml_item group = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(dynamic_context->item_heap,
                                 (sxml_sequence_view){.sequences = members, .sequence_count = 2},
                                 &group);
    }
    sxml_sequence_release(members[0]);
    sxml_merge_clear_members(state);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_focus focus = state->merge->focus;
    state->group_count += 1;
    sxml_evaluator_machine_bind_slot(dynamic_context, focus.context_value,
                                     sxml_sequence_from_item(group));
    if (focus.context_position.value != 0) {
        sxml_evaluator_machine_bind_slot(
            dynamic_context, focus.context_position,
            sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)state->group_count)));
    }
    sxml_evaluator_task *task = sxml_evaluator_machine_top(dynamic_context);
    task->phase = (uint32_t)SXML_MERGE_PHASE_BODY;
    if (task->delivers_content) {
        return sxml_evaluator_machine_push_content(dynamic_context, state->merge->body);
    }
    return sxml_evaluator_machine_push_expression(dynamic_context, state->merge->body);
}

/* The order ended with the first group of the heads, [tuples, key]: it
   opens a group, adds to the open one, or closes it (src/ir_internal.h,
   MERGE). */
static sxml_status sxml_merge_take_order(sxml_dynamic_context *dynamic_context,
                                         sxml_merge_state *state) {
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    sxml_status status = SXML_STATUS_OK;
    sxml_sequence tuples = sxml_sequence_empty();
    sxml_sequence key = sxml_sequence_empty();
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_array_get_member(value.content, 0, &tuples) != SXML_STATUS_OK ||
        sxml_array_get_member(value.content, 1, &key) != SXML_STATUS_OK) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    uint64_t tuple_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(tuples, dynamic_context->error_record, &tuple_count);
    }
    sxml_item first = {.representation = 0};
    if (status == SXML_STATUS_OK && tuple_count > 0) {
        status = sxml_sequence_get_item(
            tuples, (sxml_item_request){.item_index = 0, .error_record = NULL}, &first);
    }
    if (status != SXML_STATUS_OK || tuple_count == 0) {
        sxml_sequence_release(value);
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    uint64_t taken_from = 0;
    if (state->has_group) {
        if (!sxml_merge_is_same_tuple(
                (sxml_merge_tuple_pair){.first = first, .second = state->members[0].tuple})) {
            /* A cursor's tuple sorts before the one it had: 15.4. */
            sxml_sequence_release(value);
            return sxml_evaluator_machine_raise(dynamic_context, &sxml_merge_order_error);
        }
        if (tuple_count == 1) {
            sxml_sequence_release(value);
            return sxml_merge_push_body(dynamic_context, state);
        }
        taken_from = 1;
    } else {
        sxml_sequence_retain(key);
        state->group_key = key;
        state->has_group = true;
    }
    for (uint64_t tuple_index = taken_from; status == SXML_STATUS_OK && tuple_index < tuple_count;
         tuple_index += 1) {
        sxml_item tuple = {.representation = 0};
        status = sxml_sequence_get_item(
            tuples, (sxml_item_request){.item_index = tuple_index, .error_record = NULL}, &tuple);
        if (status == SXML_STATUS_OK) {
            status = sxml_merge_take_head(dynamic_context, state, tuple);
        }
    }
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_evaluator_machine_top(dynamic_context)->phase = (uint32_t)SXML_MERGE_PHASE_ADVANCE;
    return sxml_merge_advance(dynamic_context, state);
}

/* A group's body ended: its value, then the next order. */
static sxml_status sxml_merge_take_body(sxml_dynamic_context *dynamic_context,
                                        sxml_merge_state *state) {
    const sxml_sequence value = sxml_evaluator_machine_take_value(dynamic_context);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_evaluator_machine_top(dynamic_context)->delivers_content) {
        status = sxml_evaluator_machine_deliver(dynamic_context, value);
    } else {
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
        sxml_sequence_release(value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_merge_order(dynamic_context, state);
}

/* Reads the grounded tuples and the anchors of each merge stream, opens
   one cursor per anchor and has every cursor move to its first tuple. */
static sxml_status sxml_merge_start(sxml_dynamic_context *dynamic_context,
                                    sxml_evaluator_task *task) {
    const sxml_ir_unit *unit = task->function->unit;
    const sxml_ir_merge *merge = &sxml_ir_unit_get_expression(unit, task->expression)->merge;
    const sxml_ir_merge_stream_view streams = sxml_ir_unit_get_merge_streams(unit, merge->streams);
    void *memory = NULL;
    sxml_status status = sxml_merge_allocate(
        dynamic_context,
        (sxml_merge_array_request){.element_count = 1, .element_size = sizeof(sxml_merge_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_merge_state *state = memory;
    state->merge = merge;
    state->grounded = sxml_sequence_empty();
    state->group_key = sxml_sequence_empty();
    task->details.part.state = state;
    task->details.part.release_state = sxml_merge_release_state;
    sxml_sequence grounded = sxml_sequence_empty();
    status = sxml_evaluator_machine_read_atom(dynamic_context, merge->tuples, &grounded);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(grounded);
    state->grounded = grounded;
    state->grounded_iterator = sxml_sequence_iterator_start(state->grounded);
    /* One cursor per anchor, after the grounded one. */
    size_t cursor_count = 1;
    for (size_t stream_index = 0; status == SXML_STATUS_OK && stream_index < streams.merge_stream_count;
         stream_index += 1) {
        sxml_sequence anchors = sxml_sequence_empty();
        uint64_t anchor_count = 0;
        status = sxml_evaluator_machine_read_atom(
            dynamic_context, streams.merge_streams[stream_index].anchors, &anchors);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(anchors, dynamic_context->error_record, &anchor_count);
        }
        cursor_count += (size_t)anchor_count;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_merge_allocate(
            dynamic_context,
            (sxml_merge_array_request){.element_count = streams.merge_stream_count,
                                       .element_size = sizeof(sxml_merge_source)},
            &memory);
        state->sources = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_merge_allocate(dynamic_context,
                                     (sxml_merge_array_request){.element_count = cursor_count,
                                                                .element_size =
                                                                    sizeof(sxml_merge_cursor)},
                                     &memory);
        state->cursors = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_merge_allocate(
            dynamic_context,
            (sxml_merge_array_request){.element_count = cursor_count, .element_size = sizeof(size_t)},
            &memory);
        state->pending = memory;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    state->cursors[0] = (sxml_merge_cursor){.is_grounded = true};
    state->cursor_count = 1;
    for (size_t stream_index = 0; status == SXML_STATUS_OK && stream_index < streams.merge_stream_count;
         stream_index += 1) {
        sxml_merge_source *source = &state->sources[stream_index];
        status = sxml_merge_read_source(
            dynamic_context,
            (sxml_merge_source_reading){.unit = unit,
                                        .stream = streams.merge_streams[stream_index].stream},
            source);
        state->source_count = stream_index + 1U;
        sxml_sequence anchors = sxml_sequence_empty();
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_machine_read_atom(
                dynamic_context, streams.merge_streams[stream_index].anchors, &anchors);
        }
        sxml_sequence_iterator iterator = sxml_sequence_iterator_start(anchors);
        uint64_t anchor_position = 0;
        while (status == SXML_STATUS_OK && state->cursor_count < cursor_count) {
            sxml_iteration_step step = {.has_item = false};
            status = sxml_sequence_iterator_next(&iterator, dynamic_context->error_record, &step);
            if (status != SXML_STATUS_OK || !step.has_item) {
                break;
            }
            anchor_position += 1;
            sxml_merge_cursor *cursor = &state->cursors[state->cursor_count];
            *cursor = (sxml_merge_cursor){.is_grounded = false,
                                          .source_index = stream_index,
                                          .anchor_position = anchor_position};
            state->cursor_count += 1;
            /* 15.3: the documents are read in anchor order. */
            status = sxml_merge_open(
                dynamic_context,
                (sxml_merge_opening){.cursor = cursor, .source = source, .uri = step.item});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t cursor_index = 0; cursor_index < state->cursor_count; cursor_index += 1) {
        state->pending[cursor_index] = cursor_index;
    }
    state->pending_count = state->cursor_count;
    state->pending_next = 0;
    task->phase = (uint32_t)SXML_MERGE_PHASE_ADVANCE;
    return sxml_merge_advance(dynamic_context, state);
}

/* The step function of MERGE. */
static sxml_status sxml_merge_step(sxml_dynamic_context *dynamic_context,
                                   sxml_evaluator_task *task) {
    sxml_merge_state *state = task->details.part.state;
    switch ((sxml_merge_phase)task->phase) {
    case SXML_MERGE_PHASE_START:
        return sxml_merge_start(dynamic_context, task);
    case SXML_MERGE_PHASE_ADVANCE:
        return sxml_merge_advance(dynamic_context, state);
    case SXML_MERGE_PHASE_TUPLE:
        return sxml_merge_take_tuple(dynamic_context, state);
    case SXML_MERGE_PHASE_ORDERED:
        return sxml_merge_take_order(dynamic_context, state);
    case SXML_MERGE_PHASE_BODY:
        return sxml_merge_take_body(dynamic_context, state);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_evaluator_kind_handler sxml_stream_kind_handlers[] = {
    {.expression_kind = SXML_IR_EXPRESSION_KIND_STREAM, .step = sxml_stream_step},
    {.expression_kind = SXML_IR_EXPRESSION_KIND_MERGE, .step = sxml_merge_step},
};

const sxml_evaluator_part sxml_evaluator_part_stream = {
    .kind_handlers = sxml_stream_kind_handlers,
    .kind_handler_count = sizeof(sxml_stream_kind_handlers) / sizeof(sxml_stream_kind_handlers[0]),
};

/* Only XSLT lowers to STREAM (xsl:source-document streamable="yes"), so
   only XSLT's units name this part, and a module without XSLT does not
   link it (D227). */
const sxml_language_set sxml_evaluator_part_stream_languages = SXML_LANGUAGE_XSLT;

/* ------------------------------------------- Values of streamed nodes */

/* Whether node is item, or (with is_tree) a node of item's tree. */
typedef struct sxml_stream_node_match {
    sxml_node_reference node;
    sxml_item item;
    bool is_tree;
} sxml_stream_node_match;

static bool sxml_stream_holds(sxml_stream_node_match match) {
    sxml_node_reference held;
    if (match.item.representation == 0 || sxml_item_get_node(match.item, &held) != SXML_STATUS_OK) {
        return false;
    }
    return match.is_tree ? held.tree == match.node.tree : sxml_node_is_same_node(held, match.node);
}

/* The values entry gives for node, written to *values when it holds it. */
typedef struct sxml_stream_entry_question {
    const sxml_stream_accumulated *entry;
    sxml_node_reference node;
} sxml_stream_entry_question;

static bool sxml_stream_entry_answers(sxml_stream_entry_question question,
                                      sxml_streamed_accumulator_values *values) {
    const sxml_stream_accumulated *entry = question.entry;
    if (sxml_stream_holds(
            (sxml_stream_node_match){.node = question.node, .item = entry->window, .is_tree = false})) {
        values->is_streamed = true;
        values->before = entry->before;
        values->after = entry->after;
        values->descended = (sxml_item){.representation = 0};
        return true;
    }
    if (sxml_stream_holds((sxml_stream_node_match){
            .node = question.node, .item = entry->full_window, .is_tree = true})) {
        values->is_streamed = true;
        values->before = (sxml_item){.representation = 0};
        values->after = (sxml_item){.representation = 0};
        values->descended = entry->descended;
        return true;
    }
    return false;
}

sxml_status sxml_evaluator_stream_find_accumulator_values(sxml_dynamic_context *dynamic_context,
                                                          sxml_item node,
                                                          sxml_streamed_accumulator_values *values) {
    if (dynamic_context == NULL || values == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *values = (sxml_streamed_accumulator_values){.is_streamed = false,
                                                 .before = {.representation = 0},
                                                 .after = {.representation = 0},
                                                 .descended = {.representation = 0},
                                                 .failure = NULL,
                                                 .applicable = {.representation = 0},
                                                 .waiting = NULL};
    sxml_node_reference node_reference;
    if (sxml_item_get_node(node, &node_reference) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
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
        if (!state->owns_document || document == NULL || !document->accumulates) {
            continue;
        }
        values->failure = document->has_accumulation_failed ? &document->failure : NULL;
        values->applicable = document->accumulator_values;
        values->waiting = document->is_computing && !document->is_final_round
                              ? &document->is_waiting
                              : NULL;
        if (sxml_stream_entry_answers((sxml_stream_entry_question){
                                          .entry = &document->accumulated_leaf,
                                          .node = node_reference},
                                      values)) {
            return SXML_STATUS_OK;
        }
        for (size_t depth = 0; depth < document->accumulated.element_count; depth += 1) {
            if (sxml_stream_entry_answers(
                    (sxml_stream_entry_question){
                        .entry = sxml_xml_array_at(&document->accumulated, depth),
                        .node = node_reference},
                    values)) {
                return SXML_STATUS_OK;
            }
        }
        values->failure = NULL;
        values->applicable = (sxml_item){.representation = 0};
        values->waiting = NULL;
    }
    return SXML_STATUS_OK;
}
