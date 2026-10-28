/*
 * The abstract syntax tree of the query languages: lifecycle and builder
 * functions (src/xpath_ast_internal.h, docs/DECISIONS.md D24, D52).
 *
 * Golden file: src/memory.c (lifecycle in one caller-provided block,
 * create on top of initialize, outputs untouched on failure, D20).
 *
 * Layout of the block: the sxml_ast header, then node_capacity + 1 nodes,
 * then text_byte_capacity bytes of text. Nothing grows after
 * initialization, so the AST never moves and never allocates again.
 */
#include "xpath_ast_internal.h"

#include <stdalign.h>
#include <string.h>

/* Where the parts of an AST live inside its block. */
typedef struct sxml_ast_layout {
    sxml_memory_requirements requirements;
    size_t nodes_byte_offset;
    size_t text_byte_offset;
} sxml_ast_layout;

static sxml_status
sxml_ast_compute_layout(const sxml_ast_configuration *configuration,
                        sxml_ast_layout *layout) {
    if (configuration->node_capacity > (size_t)UINT32_MAX - 1U ||
        configuration->text_byte_capacity > (size_t)UINT32_MAX) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_memory_requirements requirements = {
        .byte_count = sizeof(sxml_ast),
        .alignment = alignof(sxml_ast),
    };
    /* One more node than the capacity: entry 0 is the "no node" sentinel. */
    size_t nodes_byte_count = 0;
    sxml_status status =
        sxml_size_multiply(configuration->node_capacity + 1U,
                           sizeof(sxml_ast_node), &nodes_byte_count);
    size_t nodes_byte_offset = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = nodes_byte_count,
                                       .alignment = alignof(sxml_ast_node)},
            &nodes_byte_offset);
    }
    size_t text_byte_offset = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count =
                                           configuration->text_byte_capacity,
                                       .alignment = 1},
            &text_byte_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (sxml_ast_layout){.requirements = requirements,
                                .nodes_byte_offset = nodes_byte_offset,
                                .text_byte_offset = text_byte_offset};
    return SXML_STATUS_OK;
}

sxml_status
sxml_ast_memory_requirements(const sxml_ast_configuration *configuration,
                             sxml_memory_requirements *requirements) {
    sxml_ast_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .nodes_byte_offset = 0,
        .text_byte_offset = 0,
    };
    sxml_status status = sxml_ast_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

sxml_status sxml_ast_initialize(sxml_memory_view memory,
                                const sxml_ast_configuration *configuration,
                                sxml_ast **ast) {
    sxml_ast_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .nodes_byte_offset = 0,
        .text_byte_offset = 0,
    };
    sxml_status status = sxml_ast_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* check_requirements has checked size and alignment of the block, and
       the layout aligned each part within it. */
    void *header_memory = memory.bytes;
    void *nodes_memory = memory.bytes + layout.nodes_byte_offset;
    sxml_ast *new_ast = header_memory;
    sxml_ast_node *nodes = nodes_memory;
    memset(&nodes[0], 0, sizeof(nodes[0]));
    *new_ast = (sxml_ast){
        .nodes = nodes,
        .node_count = 1,
        .node_capacity = (uint32_t)configuration->node_capacity,
        .text_bytes = memory.bytes + layout.text_byte_offset,
        .text_byte_count = 0,
        .text_byte_capacity = (uint32_t)configuration->text_byte_capacity,
        .source_name = configuration->source_name,
        .root = {.value = 0},
        .allocator = {.state = NULL, .operations = NULL},
    };
    *ast = new_ast;
    return SXML_STATUS_OK;
}

void sxml_ast_finalize(sxml_ast *ast) {
    if (ast == NULL) {
        return;
    }
    /* Every read through the accessors now ends at the sentinel. */
    ast->node_count = 0;
    ast->text_byte_count = 0;
    ast->root = (sxml_ast_node_index){.value = 0};
}

sxml_status sxml_ast_create(sxml_allocator allocator,
                            const sxml_ast_configuration *configuration,
                            sxml_ast **ast) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_ast_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast *new_ast = NULL;
    status = sxml_ast_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_ast);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_ast->allocator = allocator;
    *ast = new_ast;
    return SXML_STATUS_OK;
}

void sxml_ast_destroy(sxml_ast *ast) {
    if (ast == NULL) {
        return;
    }
    /* Finalizing may not touch the AST afterwards, so keep the allocator. */
    sxml_allocator allocator = ast->allocator;
    sxml_ast_finalize(ast);
    sxml_allocator_release(allocator, ast);
}

/* --------------------------------------------------------------- Builder */

static bool sxml_ast_text_is_in_store(const sxml_ast *ast, sxml_ast_text text) {
    return text.byte_offset <= ast->text_byte_count &&
           text.byte_count <= ast->text_byte_count - text.byte_offset;
}

/* True for the kinds whose payload is a name (sxml_ast_kind). */
static bool sxml_ast_kind_has_name_payload(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_NCNAME ||
           kind == SXML_AST_KIND_PREFIXED_NAME ||
           kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
           kind == SXML_AST_KIND_WILDCARD || kind == SXML_AST_KIND_QNAME;
}

/* True for the kinds whose payload is a text (sxml_ast_kind). */
static bool sxml_ast_kind_has_text_payload(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_INTEGER_LITERAL ||
           kind == SXML_AST_KIND_HEX_INTEGER_LITERAL ||
           kind == SXML_AST_KIND_BINARY_INTEGER_LITERAL ||
           kind == SXML_AST_KIND_DECIMAL_LITERAL ||
           kind == SXML_AST_KIND_DOUBLE_LITERAL ||
           kind == SXML_AST_KIND_STRING_LITERAL ||
           kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART ||
           kind == SXML_AST_KIND_PRAGMA_CONTENTS ||
           kind == SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS ||
           kind == SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT ||
           kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT ||
           kind == SXML_AST_KIND_CDATA_SECTION ||
           kind == SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR;
}

static bool sxml_ast_payload_is_in_store(const sxml_ast *ast,
                                         const sxml_ast_node_request *request) {
    if (sxml_ast_kind_has_name_payload(request->kind)) {
        const sxml_ast_name *name = &request->payload.name;
        return sxml_ast_text_is_in_store(ast, name->namespace_prefix) &&
               sxml_ast_text_is_in_store(ast, name->local_name) &&
               sxml_ast_text_is_in_store(ast, name->namespace_uri);
    }
    if (sxml_ast_kind_has_text_payload(request->kind)) {
        return sxml_ast_text_is_in_store(ast, request->payload.text);
    }
    if (request->kind ==
        SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR) {
        const sxml_ast_processing_instruction *instruction =
            &request->payload.processing_instruction;
        return sxml_ast_text_is_in_store(ast, instruction->target) &&
               sxml_ast_text_is_in_store(ast, instruction->content);
    }
    return true;
}

sxml_status sxml_ast_add_node(sxml_ast *ast,
                              const sxml_ast_node_request *request,
                              sxml_ast_node_index *node) {
    if (request->kind == SXML_AST_KIND_NONE ||
        request->kind >= SXML_AST_KIND_COUNT ||
        !sxml_ast_payload_is_in_store(ast, request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* node_count includes the sentinel, so node_count - 1 nodes are used. */
    if (ast->node_count - 1U >= ast->node_capacity) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    const sxml_ast_node_index index = {.value = ast->node_count};
    ast->nodes[index.value] = (sxml_ast_node){
        .kind = request->kind,
        .field = SXML_AST_FIELD_NONE,
        .detail = request->detail,
        .flags = request->flags,
        .parent = {.value = 0},
        .first_child = {.value = 0},
        .last_child = {.value = 0},
        .next_sibling = {.value = 0},
        .source_span = request->source_span,
        .payload = request->payload,
    };
    ast->node_count += 1U;
    *node = index;
    return SXML_STATUS_OK;
}

static bool sxml_ast_index_is_in_use(const sxml_ast *ast,
                                     sxml_ast_node_index index) {
    return index.value != 0 && index.value < ast->node_count;
}

sxml_status sxml_ast_append_child(sxml_ast *ast,
                                  sxml_ast_child_request request) {
    if (!sxml_ast_index_is_in_use(ast, request.parent) ||
        !sxml_ast_index_is_in_use(ast, request.child) ||
        request.parent.value == request.child.value ||
        request.field == SXML_AST_FIELD_NONE ||
        request.field >= SXML_AST_FIELD_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast_node *child = &ast->nodes[request.child.value];
    if (child->parent.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast_node *parent = &ast->nodes[request.parent.value];
    if (parent->last_child.value == 0) {
        parent->first_child = request.child;
    } else {
        ast->nodes[parent->last_child.value].next_sibling = request.child;
    }
    parent->last_child = request.child;
    child->parent = request.parent;
    child->field = request.field;
    return SXML_STATUS_OK;
}

sxml_status sxml_ast_add_flags(sxml_ast *ast, sxml_ast_flags_request request) {
    if (!sxml_ast_index_is_in_use(ast, request.node)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    ast->nodes[request.node.value].flags |= request.flags;
    return SXML_STATUS_OK;
}

sxml_ast_source_span sxml_ast_moved_source_span(sxml_ast_source_span span,
                                                sxml_ast_source_span origin) {
    if (span.line_number == 0 || origin.line_number == 0) {
        return span;
    }
    sxml_ast_source_span moved = span;
    moved.byte_offset = span.byte_offset > UINT32_MAX - origin.byte_offset
                            ? UINT32_MAX
                            : span.byte_offset + origin.byte_offset;
    if (span.line_number > 1) {
        moved.line_number = span.line_number > UINT32_MAX - origin.line_number
                                ? UINT32_MAX
                                : origin.line_number + span.line_number - 1U;
    } else {
        moved.line_number = origin.line_number;
        moved.column_number = span.column_number > UINT32_MAX - origin.column_number
                                  ? UINT32_MAX
                                  : origin.column_number + span.column_number;
    }
    return moved;
}

void sxml_ast_move_source_spans(sxml_ast *ast, sxml_ast_source_span origin) {
    for (uint32_t node_index = 1; node_index < ast->node_count; node_index += 1) {
        ast->nodes[node_index].source_span =
            sxml_ast_moved_source_span(ast->nodes[node_index].source_span, origin);
    }
}

sxml_status sxml_ast_append_text(sxml_ast *ast, sxml_byte_view bytes,
                                 sxml_ast_text *text) {
    const bool extends_text = text->byte_count != 0;
    if (extends_text &&
        (!sxml_ast_text_is_in_store(ast, *text) ||
         text->byte_offset + text->byte_count != ast->text_byte_count)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (bytes.byte_count >
        (size_t)(ast->text_byte_capacity - ast->text_byte_count)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    const uint32_t start = ast->text_byte_count;
    if (bytes.byte_count != 0) {
        memcpy(ast->text_bytes + start, bytes.bytes, bytes.byte_count);
    }
    /* The capacity check bounds byte_count by a uint32_t difference. */
    const uint32_t appended_byte_count = (uint32_t)bytes.byte_count;
    ast->text_byte_count = start + appended_byte_count;
    if (extends_text) {
        text->byte_count += appended_byte_count;
    } else {
        *text = (sxml_ast_text){.byte_offset = start,
                                .byte_count = appended_byte_count};
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Copies */

sxml_status sxml_ast_copy(const sxml_ast_copy_request *request,
                          sxml_ast **copy) {
    const sxml_ast *original = request->original;
    sxml_ast *made = NULL;
    const sxml_status status = sxml_ast_create(
        request->allocator,
        &(sxml_ast_configuration){
            .node_capacity = original->node_capacity,
            .text_byte_capacity = original->text_byte_capacity,
            .source_name = request->source_name},
        &made);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Nodes refer to each other and to the text store by index, so the
       entries copy as they are; entry 0 is the sentinel in both. */
    memcpy(made->nodes, original->nodes,
           (size_t)original->node_count * sizeof(original->nodes[0]));
    if (original->text_byte_count != 0) {
        memcpy(made->text_bytes, original->text_bytes,
               original->text_byte_count);
    }
    made->node_count = original->node_count;
    made->text_byte_count = original->text_byte_count;
    made->root = original->root;
    *copy = made;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- AST memo */

struct sxml_ast_memo_entry {
    /* 0 for an empty slot. */
    uint64_t hash;
    sxml_ast_build_function build;
    uint32_t maximum_expression_depth;
    bool reads_xml_1_1;
    /* The text, owned (allocated with the memo's allocator; NULL when
       empty). */
    uint8_t *text_bytes;
    size_t text_byte_count;
    /* As the front end made it, with no source name; owned. */
    sxml_ast *ast;
};

sxml_ast_memo sxml_ast_memo_empty(sxml_allocator allocator) {
    return (sxml_ast_memo){
        .allocator = allocator, .slots = NULL, .slot_count = 0, .used_count = 0};
}

void sxml_ast_memo_release(sxml_ast_memo *memo) {
    for (size_t slot_index = 0; slot_index < memo->slot_count;
         slot_index += 1) {
        sxml_ast_memo_entry *entry = &memo->slots[slot_index];
        if (entry->hash != 0) {
            sxml_ast_destroy(entry->ast);
            sxml_allocator_release(memo->allocator, entry->text_bytes);
        }
    }
    sxml_allocator_release(memo->allocator, memo->slots);
    *memo = sxml_ast_memo_empty(memo->allocator);
}

/* The key of a parse: everything the AST depends on but the source name. */
typedef struct sxml_ast_memo_key {
    uint64_t hash;
    sxml_ast_build_function build;
    uint32_t maximum_expression_depth;
    bool reads_xml_1_1;
    sxml_byte_view text;
} sxml_ast_memo_key;

/* FNV-1a over the limits and the text; never 0, which marks an empty
   slot. The front end is compared, not hashed (a function pointer has no
   portable integer value). */
static uint64_t sxml_ast_memo_hash(const sxml_ast_build_request *request) {
    const uint64_t prime = UINT64_C(1099511628211);
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = (hash ^ request->maximum_expression_depth) * prime;
    hash = (hash ^ (request->reads_xml_1_1 ? 1U : 0U)) * prime;
    const sxml_byte_view text = request->expression_text;
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        hash = (hash ^ text.bytes[byte_index]) * prime;
    }
    return hash == 0 ? 1 : hash;
}

/* The slot of key: its entry, or the empty slot where it goes. The table
   is never full. */
static sxml_ast_memo_entry *sxml_ast_memo_find(const sxml_ast_memo *memo,
                                               const sxml_ast_memo_key *key) {
    const size_t mask = memo->slot_count - 1U;
    size_t slot_index = (size_t)(key->hash & mask);
    for (;;) {
        sxml_ast_memo_entry *entry = &memo->slots[slot_index];
        if (entry->hash == 0 ||
            (entry->hash == key->hash && entry->build == key->build &&
             entry->maximum_expression_depth ==
                 key->maximum_expression_depth &&
             entry->reads_xml_1_1 == key->reads_xml_1_1 &&
             sxml_byte_view_equals(
                 (sxml_byte_view){.bytes = entry->text_bytes,
                                  .byte_count = entry->text_byte_count},
                 key->text))) {
            return entry;
        }
        slot_index = (slot_index + 1U) & mask;
    }
}

/* Doubles the slots (64 at first), moving the entries. */
static sxml_status sxml_ast_memo_grow(sxml_ast_memo *memo) {
    const size_t slot_count =
        memo->slot_count == 0 ? 64U : memo->slot_count * 2U;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        slot_count, sizeof(sxml_ast_memo_entry), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            memo->allocator,
            (sxml_allocation_request){
                .byte_count = byte_count,
                .alignment = alignof(sxml_ast_memo_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    sxml_ast_memo_entry *old_slots = memo->slots;
    const size_t old_slot_count = memo->slot_count;
    memo->slots = memory;
    memo->slot_count = slot_count;
    for (size_t slot_index = 0; slot_index < old_slot_count;
         slot_index += 1) {
        const sxml_ast_memo_entry *entry = &old_slots[slot_index];
        if (entry->hash != 0) {
            *sxml_ast_memo_find(
                memo, &(sxml_ast_memo_key){
                          .hash = entry->hash,
                          .build = entry->build,
                          .maximum_expression_depth =
                              entry->maximum_expression_depth,
                          .reads_xml_1_1 = entry->reads_xml_1_1,
                          .text = {.bytes = entry->text_bytes,
                                   .byte_count = entry->text_byte_count}}) =
                *entry;
        }
    }
    sxml_allocator_release(memo->allocator, old_slots);
    return SXML_STATUS_OK;
}

/* Keeps a copy of ast, which build made from text, in the empty slot
   entry. */
typedef struct sxml_ast_memo_addition {
    sxml_ast_memo_entry *entry;
    const sxml_ast_memo_key *key;
    const sxml_ast *ast;
} sxml_ast_memo_addition;

static sxml_status sxml_ast_memo_add(sxml_ast_memo *memo,
                                     sxml_ast_memo_addition addition) {
    const sxml_byte_view text = addition.key->text;
    void *text_memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (text.byte_count != 0) {
        status = sxml_allocator_allocate(
            memo->allocator,
            (sxml_allocation_request){.byte_count = text.byte_count,
                                      .alignment = 1},
            &text_memory);
    }
    sxml_ast *kept = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_copy(
            &(sxml_ast_copy_request){
                .original = addition.ast,
                .allocator = memo->allocator,
                .source_name = {.bytes = NULL, .byte_count = 0}},
            &kept);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(memo->allocator, text_memory);
        return status;
    }
    if (text.byte_count != 0) {
        memcpy(text_memory, text.bytes, text.byte_count);
    }
    *addition.entry = (sxml_ast_memo_entry){
        .hash = addition.key->hash,
        .build = addition.key->build,
        .maximum_expression_depth = addition.key->maximum_expression_depth,
        .reads_xml_1_1 = addition.key->reads_xml_1_1,
        .text_bytes = text_memory,
        .text_byte_count = text.byte_count,
        .ast = kept};
    memo->used_count += 1U;
    return SXML_STATUS_OK;
}

sxml_status sxml_ast_memo_build(sxml_ast_memo *memo,
                                const sxml_ast_memo_request *request,
                                sxml_ast **ast) {
    const sxml_ast_build_request *build_request = request->build_request;
    sxml_status status = SXML_STATUS_OK;
    if ((memo->used_count + 1U) * 2U > memo->slot_count) {
        status = sxml_ast_memo_grow(memo);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast_memo_key key = {
        .hash = sxml_ast_memo_hash(build_request),
        .build = request->build,
        .maximum_expression_depth = build_request->maximum_expression_depth,
        .reads_xml_1_1 = build_request->reads_xml_1_1,
        .text = build_request->expression_text};
    sxml_ast_memo_entry *entry = sxml_ast_memo_find(memo, &key);
    if (entry->hash != 0) {
        return sxml_ast_copy(
            &(sxml_ast_copy_request){.original = entry->ast,
                                     .allocator = build_request->allocator,
                                     .source_name = build_request->source_name},
            ast);
    }
    sxml_ast *built = NULL;
    status = request->build(build_request, &built);
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_memo_add(
            memo, (sxml_ast_memo_addition){.entry = entry, .key = &key, .ast = built});
    }
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(built);
        return status;
    }
    *ast = built;
    return SXML_STATUS_OK;
}
