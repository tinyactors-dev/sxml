/*
 * The internal native row of simple content (XSLT 4.0, 5.7.2; declared in
 * src/xslt_lower_internal.h, packet 4.6, D142): merge-text-nodes. XSLT
 * lowering calls it before fn:string-join, so a value whose adjacent text
 * nodes would otherwise be joined with the separator gives their text
 * unseparated: `{text{'a'}, text{'b'}}` in a value template is "ab"
 * (D120's deviation, fixed), and zero-length text nodes are dropped first
 * (step 1, D163).
 *
 * Golden file: src/xquery_flwor.c (an internal row in no function table).
 */
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xslt_lower_internal.h"

#include <stdalign.h>
#include <string.h>

/* The string value of item when it is a text node; is_text false
   otherwise. */
typedef struct text_item {
    bool is_text;
    sxml_byte_view content;
} text_item;

static sxml_status read_text_item(sxml_item item, text_item *text) {
    *text = (text_item){.is_text = false, .content = {.bytes = NULL, .byte_count = 0}};
    if (sxml_item_get_kind(item) != SXML_ITEM_KIND_NODE) {
        return SXML_STATUS_OK;
    }
    sxml_node_reference node = {.tree = NULL, .node_index = {0}, .namespace_number = 0};
    sxml_status status = sxml_item_get_node(item, &node);
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_TEXT) {
        return status;
    }
    text->is_text = true;
    return sxml_node_get_content(node, &text->content);
}

/* True when two text nodes are adjacent in value, or value has a
   zero-length text node: either needs merge_runs. */
typedef struct adjacency {
    bool has_adjacent_text;
} adjacency;

static sxml_status find_adjacent_text(const sxml_function_call *call, sxml_sequence value,
                                      adjacency *found) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    bool previous_is_text = false;
    found->has_adjacent_text = false;
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        sxml_status status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            return status;
        }
        text_item text = {.is_text = false};
        status = read_text_item(step.item, &text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (text.is_text && (previous_is_text || text.content.byte_count == 0)) {
            found->has_adjacent_text = true;
            return SXML_STATUS_OK;
        }
        previous_is_text = text.is_text;
    }
}

/* The text of a run of adjacent text nodes, gathered in heap memory. */
typedef struct text_run {
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    size_t node_count;
    /* The run's first node, kept as it is when the run has no other. */
    sxml_item first_node;
} text_run;

static sxml_status append_run_text(sxml_allocator allocator, sxml_byte_view text, text_run *run) {
    if (text.byte_count > run->byte_capacity - run->byte_count) {
        size_t capacity = run->byte_capacity == 0 ? 64U : run->byte_capacity;
        while (capacity - run->byte_count < text.byte_count) {
            if (capacity > SIZE_MAX / 2U) {
                return SXML_STATUS_ARITHMETIC_OVERFLOW;
            }
            capacity *= 2U;
        }
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1}, &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (run->byte_count > 0) {
            memcpy(memory, run->bytes, run->byte_count);
        }
        sxml_allocator_release(allocator, run->bytes);
        run->bytes = memory;
        run->byte_capacity = capacity;
    }
    if (text.byte_count > 0) {
        memcpy(run->bytes + run->byte_count, text.bytes, text.byte_count);
    }
    run->byte_count += text.byte_count;
    return SXML_STATUS_OK;
}

/* Appends the run to builder: its node when it has one, else one
   xs:untypedAtomic of its text (5.7.2, step 2: the merged text node's
   string value is what atomizing it gives). */
typedef struct run_flush {
    sxml_item_heap *item_heap;
    sxml_sequence_builder *builder;
} run_flush;

static sxml_status flush_run(run_flush flush, text_run *run) {
    sxml_status status = SXML_STATUS_OK;
    if (run->node_count == 1) {
        status = sxml_sequence_builder_append_item(flush.builder, run->first_node);
    } else if (run->node_count > 1) {
        sxml_item merged = {.representation = 0};
        status = sxml_atomic_item_make(
            flush.item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_UNTYPED_ATOMIC),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = {.utf8 = {.bytes = run->bytes,
                                                                .byte_count = run->byte_count},
                                                       .codepoint_count = 0,
                                                       .is_ascii = false}}}},
            &merged);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(flush.builder, merged);
            sxml_item_release(merged);
        }
    }
    run->node_count = 0;
    run->byte_count = 0;
    return status;
}

static sxml_status merge_runs(const sxml_function_call *call, sxml_sequence value,
                              sxml_sequence *result) {
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        allocator, &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    text_run run = {.bytes = NULL, .byte_count = 0, .byte_capacity = 0, .node_count = 0};
    const run_flush flush = {.item_heap = call->item_heap, .builder = builder};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        text_item text = {.is_text = false};
        status = read_text_item(step.item, &text);
        if (status == SXML_STATUS_OK && text.is_text && text.content.byte_count == 0) {
            /* 5.7.2, step 1: zero-length text nodes are discarded (D163). */
            continue;
        }
        if (status == SXML_STATUS_OK && text.is_text) {
            if (run.node_count == 0) {
                run.first_node = step.item;
            }
            run.node_count += 1;
            status = append_run_text(allocator, text.content, &run);
            continue;
        }
        if (status == SXML_STATUS_OK) {
            status = flush_run(flush, &run);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = flush_run(flush, &run);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_allocator_release(allocator, run.bytes);
    sxml_sequence_builder_destroy(builder);
    return status;
}

static sxml_status call_merge_text_nodes(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = sxml_function_library_get_argument(call, 0);
    adjacency found = {.has_adjacent_text = false};
    const sxml_status status = find_adjacent_text(call, value, &found);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!found.has_adjacent_text) {
        sxml_sequence_retain(value);
        *result = value;
        return SXML_STATUS_OK;
    }
    return merge_runs(call, value, result);
}

static const sxml_function_parameter merge_text_nodes_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "item()*"),
};

const sxml_native_function sxml_xslt_merge_text_nodes_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = 0,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("merge-text-nodes"),
    .parameters = {merge_text_nodes_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_merge_text_nodes,
    .resumable_body = NULL};
