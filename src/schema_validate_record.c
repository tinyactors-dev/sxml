/*
 * The subtree recorder and the element copy of type alternatives
 * (src/schema_validate_internal.h; glossary recorded_subtree;
 * docs/DECISIONS.md D167, D173).
 *
 * Assertions and identity constraints evaluate XPath on an element with
 * its validated descendants (XSD 1.1 Part 1, 3.13.4.1 and 3.11.4), and
 * type alternatives on the element with its attributes (3.12.4). The
 * validator records what they read as an ordinary arena tree, built from
 * the annotated events it sends, so typed values derive from the
 * annotations (D165) and the XPath of packets 5.8 and 5.9 runs on it with
 * the one evaluator. The recorder is a tree builder in front of the arena
 * tree that counts the nodes it makes, so each element's node index (and
 * its governing declaration, by node index) is known while the events
 * pass.
 *
 * Golden file: src/xslt_whitespace.c (a builder in front of a new arena
 * tree).
 */
#include "schema_validate_internal.h"

#include "processor_internal.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"

#include <string.h>

static sxml_schema_scratch_array empty_array(size_t entry_byte_count) {
    return (sxml_schema_scratch_array){
        .bytes = NULL, .entry_byte_count = entry_byte_count, .entry_count = 0, .entry_capacity = 0};
}

void sxml_schema_recorder_initialize(sxml_schema_subtree_recorder *recorder,
                                     sxml_allocator allocator) {
    (void)allocator;
    *recorder =
        (sxml_schema_subtree_recorder){.arena_tree = NULL,
                                       .root_frame = 0,
                                       .node_count = 0,
                                       .ends_in_text = false,
                                       .next_declaration = {.value = 0},
                                       .element_declarations = empty_array(sizeof(uint32_t)),
                                       .checks = empty_array(sizeof(sxml_schema_deferred_check))};
}

void sxml_schema_recorder_release(sxml_schema_subtree_recorder *recorder,
                                  sxml_allocator allocator) {
    sxml_arena_tree_destroy(recorder->arena_tree);
    recorder->arena_tree = NULL;
    sxml_schema_scratch_array_release(allocator, &recorder->element_declarations);
    sxml_schema_scratch_array_release(allocator, &recorder->checks);
}

sxml_status sxml_schema_validator_create_tree(const sxml_schema_validator *validator,
                                             sxml_arena_tree **arena_tree) {
    const sxml_processor *processor = validator->configuration.schema->processor;
    return sxml_arena_tree_create(
        validator->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = validator->allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = validator->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = 0,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = true,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = false,
            .error_record = NULL,
            .base_uri = {.bytes = validator->base_uri_bytes.bytes,
                         .byte_count = validator->base_uri_bytes.entry_count}},
        arena_tree);
}

sxml_status sxml_schema_recorder_start(sxml_schema_validator *validator) {
    sxml_schema_subtree_recorder *recorder = &validator->recorder;
    const sxml_status status = sxml_schema_validator_create_tree(validator, &recorder->arena_tree);
    if (status != SXML_STATUS_OK) {
        recorder->arena_tree = NULL;
        return status;
    }
    recorder->root_frame = validator->frames.entry_count;
    recorder->node_count = 0;
    recorder->ends_in_text = false;
    recorder->element_declarations.entry_count = 0;
    recorder->checks.entry_count = 0;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- The recorder */

/* Counts one node of the tree, with its declaration. */
static sxml_status count_node(sxml_schema_validator *validator, uint32_t declaration) {
    sxml_schema_subtree_recorder *recorder = &validator->recorder;
    recorder->node_count += 1;
    recorder->ends_in_text = false;
    return sxml_schema_scratch_array_append(validator->allocator, &recorder->element_declarations,
                                            &declaration);
}

static sxml_tree_builder arena_of(const sxml_schema_validator *validator) {
    return sxml_arena_tree_as_tree_builder(validator->recorder.arena_tree);
}

static sxml_status record_start_document(void *state, const sxml_document_start_event *event) {
    (void)state;
    (void)event;
    /* A recorded subtree is a parentless element. */
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status record_end_document(void *state) {
    (void)state;
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status record_start_element(void *state, const sxml_element_start_event *event) {
    sxml_schema_validator *validator = state;
    sxml_status status = count_node(validator, validator->recorder.next_declaration.value);
    if (status == SXML_STATUS_OK) {
        const sxml_tree_builder arena = arena_of(validator);
        status = arena.operations->start_element(arena.state, event);
    }
    return status;
}

static sxml_status record_add_namespace_binding(void *state,
                                                const sxml_namespace_binding *namespace_binding) {
    const sxml_schema_validator *validator = state;
    const sxml_tree_builder arena = arena_of(validator);
    return arena.operations->add_namespace_binding(arena.state, namespace_binding);
}

static sxml_status record_add_attribute(void *state, const sxml_attribute_event *event) {
    sxml_schema_validator *validator = state;
    sxml_status status = count_node(validator, 0);
    if (status == SXML_STATUS_OK) {
        const sxml_tree_builder arena = arena_of(validator);
        status = arena.operations->add_attribute(arena.state, event);
    }
    return status;
}

static sxml_status record_end_element(void *state) {
    sxml_schema_validator *validator = state;
    validator->recorder.ends_in_text = false;
    const sxml_tree_builder arena = arena_of(validator);
    return arena.operations->end_element(arena.state);
}

static sxml_status record_add_text(void *state, const sxml_text_event *event) {
    sxml_schema_validator *validator = state;
    sxml_status status = SXML_STATUS_OK;
    /* XDM 4.0, 7.5.7.1: adjacent texts make one node, empty text none. */
    if (event->text.byte_count != 0 && !validator->recorder.ends_in_text) {
        status = count_node(validator, 0);
        validator->recorder.ends_in_text = true;
    }
    if (status == SXML_STATUS_OK) {
        const sxml_tree_builder arena = arena_of(validator);
        status = arena.operations->add_text(arena.state, event);
    }
    return status;
}

/* XSD 1.1, 3.13.4.1 clause 1.2: comments and processing instructions are
   excluded from the partial post-schema-validation infoset assertions see
   (the suite's comments-and-PIs-excluded); identity constraints select
   elements and attributes only, so nothing reads them. A text after one
   joins the text before it, as in a tree without them. */
static sxml_status record_add_comment(void *state, const sxml_comment_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status
record_add_processing_instruction(void *state, const sxml_processing_instruction_event *event) {
    (void)state;
    (void)event;
    return SXML_STATUS_OK;
}

static sxml_status record_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    (void)state;
    (void)entity;
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_tree_builder_operations recorder_operations = {
    .start_document = record_start_document,
    .end_document = record_end_document,
    .start_element = record_start_element,
    .add_namespace_binding = record_add_namespace_binding,
    .add_attribute = record_add_attribute,
    .end_element = record_end_element,
    .add_text = record_add_text,
    .add_comment = record_add_comment,
    .add_processing_instruction = record_add_processing_instruction,
    .add_unparsed_entity = record_add_unparsed_entity,
};

sxml_tree_builder sxml_schema_recorder_as_tree_builder(sxml_schema_validator *validator) {
    return (sxml_tree_builder){.state = validator, .operations = &recorder_operations};
}

sxml_status sxml_schema_recorder_defer_check(sxml_schema_validator *validator,
                                             const sxml_schema_deferred_check *check) {
    return sxml_schema_scratch_array_append(validator->allocator, &validator->recorder.checks,
                                            check);
}

/* Runs one deferred check on the finished tree. */
typedef struct check_run {
    const sxml_schema_deferred_check *check;
    sxml_tree *tree;
} check_run;

static sxml_status run_check(sxml_schema_validator *validator, check_run run) {
    const sxml_schema_subtree_recorder *recorder = &validator->recorder;
    const sxml_item element_node = sxml_node_item_make((sxml_node_reference){
        .tree = run.tree, .node_index = {.index = run.check->node_index}, .namespace_number = 0});
    const sxml_schema_recorded_subtree subtree = {
        .element_node = element_node,
        .element_declarations = sxml_schema_scratch_array_at(&recorder->element_declarations, 0),
        .element_declaration_count = recorder->element_declarations.entry_count};
    sxml_status status = SXML_STATUS_OK;
    if (run.check->type_definition.value != 0) {
        /* 3.13.4.1 (packet 5.9). */
        status = sxml_schema_check_assertions(
            validator,
            &(sxml_schema_assertion_request){.schema = validator->configuration.schema,
                                             .type_definition = run.check->type_definition,
                                             .node = element_node,
                                             .value = sxml_sequence_empty()});
    }
    if (status == SXML_STATUS_OK && run.check->element_declaration.value != 0) {
        /* 3.11.4 (packet 5.8). */
        status = sxml_schema_check_identity_constraints(
            validator,
            &(sxml_schema_identity_request){.schema = validator->configuration.schema,
                                            .element_declaration = run.check->element_declaration,
                                            .subtree = &subtree});
    }
    sxml_item_release(element_node);
    return status;
}

sxml_status sxml_schema_recorder_finish(sxml_schema_validator *validator) {
    sxml_schema_subtree_recorder *recorder = &validator->recorder;
    sxml_tree *tree = NULL;
    sxml_status status = sxml_arena_tree_finish(recorder->arena_tree, &tree);
    sxml_tree_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_describe(tree, &description);
    }
    if (status == SXML_STATUS_OK && description.node_count != recorder->node_count) {
        /* The count must be the tree's, or declarations would be read
           for the wrong nodes. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    for (size_t check_index = 0;
         status == SXML_STATUS_OK && check_index < recorder->checks.entry_count; check_index += 1) {
        const sxml_schema_deferred_check *check =
            sxml_schema_scratch_array_at(&recorder->checks, check_index);
        if (check == NULL) {
            break;
        }
        status = run_check(validator, (check_run){.check = check, .tree = tree});
    }
    if (tree != NULL) {
        sxml_tree_release(tree);
    }
    sxml_arena_tree_destroy(recorder->arena_tree);
    recorder->arena_tree = NULL;
    recorder->checks.entry_count = 0;
    recorder->element_declarations.entry_count = 0;
    return status;
}

/* ------------------------------------------------- Type alternatives */

sxml_status sxml_schema_send_in_scope_bindings(sxml_schema_validator *validator,
                                               sxml_tree_builder builder) {
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_schema_validator_get_cast_bindings(validator, &bindings);
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &bindings.namespace_bindings[binding_index];
        bool is_shadowed = false;
        for (size_t inner = 0; inner < binding_index; inner += 1) {
            is_shadowed =
                is_shadowed || bindings.namespace_bindings[inner].namespace_prefix.index ==
                                   binding->namespace_prefix.index;
        }
        if (!is_shadowed && binding->namespace_uri.index != SXML_NAMESPACE_NONE) {
            status = builder.operations->add_namespace_binding(builder.state, binding);
        }
    }
    return status;
}

/* Whether the held-back element has an attribute of the name. */
static bool has_own_attribute(const sxml_schema_validator *validator,
                              sxml_name_identifier attribute_name) {
    for (size_t attribute_index = 0; attribute_index < validator->pending_attributes.entry_count;
         attribute_index += 1) {
        const sxml_schema_pending_attribute *attribute =
            sxml_schema_scratch_array_at(&validator->pending_attributes, attribute_index);
        if (attribute->attribute_name.index == attribute_name.index) {
            return true;
        }
    }
    return false;
}

/* Sends the attributes the held-back element inherits (3.3.5.6) and does
   not have itself, as if they were its own (3.12.4 clause 1.1.3): of the
   inheritable attributes of its ancestors, the innermost one of each
   name. Their prefixes are bound in the element's in-scope namespaces,
   which the copy has, unless an inner element rebinds one (no namespace
   fixup yet). */
static sxml_status send_inherited_attributes(sxml_schema_validator *validator,
                                             sxml_tree_builder builder) {
    const sxml_schema_scratch_array *inherited = &validator->inherited_attributes;
    sxml_status status = SXML_STATUS_OK;
    for (size_t remaining = inherited->entry_count; status == SXML_STATUS_OK && remaining > 0;
         remaining -= 1) {
        const sxml_schema_inherited_attribute *attribute =
            sxml_schema_scratch_array_at(inherited, remaining - 1U);
        bool is_shadowed = has_own_attribute(validator, attribute->attribute_name);
        for (size_t inner = remaining; !is_shadowed && inner < inherited->entry_count;
             inner += 1) {
            const sxml_schema_inherited_attribute *inner_attribute =
                sxml_schema_scratch_array_at(inherited, inner);
            is_shadowed = inner_attribute->attribute_name.index == attribute->attribute_name.index;
        }
        if (is_shadowed) {
            continue;
        }
        status = builder.operations->add_attribute(
            builder.state,
            &(sxml_attribute_event){
                .attribute_name = attribute->attribute_name,
                .namespace_prefix = attribute->namespace_prefix,
                .attribute_value = {.bytes = attribute->value_byte_count == 0
                                                 ? NULL
                                                 : validator->inherited_bytes.bytes +
                                                       attribute->value_offset,
                                    .byte_count = attribute->value_byte_count},
                .type_annotation = {.index = SXML_TYPE_NONE},
                .is_id = false,
                .is_idrefs = false,
                .location = validator->pending.event.location});
    }
    return status;
}

/* Sends the held-back element, its in-scope namespaces, its attributes
   as they appear and the attributes it inherits to builder. */
static sxml_status send_element_copy(sxml_schema_validator *validator, sxml_tree_builder builder) {
    sxml_element_start_event event = validator->pending.event;
    event.type_annotation.index = SXML_TYPE_NONE;
    event.is_id = false;
    event.is_idrefs = false;
    event.is_nilled = false;
    event.ignores_parent_namespaces = true;
    sxml_status status = builder.operations->start_element(builder.state, &event);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_send_in_scope_bindings(validator, builder);
    }
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < validator->pending_attributes.entry_count;
         attribute_index += 1) {
        const sxml_schema_pending_attribute *attribute =
            sxml_schema_scratch_array_at(&validator->pending_attributes, attribute_index);
        status = builder.operations->add_attribute(
            builder.state, &(sxml_attribute_event){
                               .attribute_name = attribute->attribute_name,
                               .namespace_prefix = attribute->namespace_prefix,
                               .attribute_value = {.bytes = attribute->value_byte_count == 0
                                                                ? NULL
                                                                : validator->attribute_bytes.bytes +
                                                                      attribute->value_offset,
                                                   .byte_count = attribute->value_byte_count},
                               .type_annotation = {.index = SXML_TYPE_NONE},
                               .is_id = false,
                               .is_idrefs = false,
                               .location = attribute->location});
    }
    if (status == SXML_STATUS_OK) {
        status = send_inherited_attributes(validator, builder);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_element(builder.state);
    }
    return status;
}

sxml_status sxml_schema_select_type(sxml_schema_validator *validator,
                                    sxml_schema_element_declaration_index element_declaration,
                                    sxml_schema_type_definition_index *selected_type) {
    const sxml_schema_element_declaration *declaration =
        sxml_schema_get_element_declaration(validator->components, element_declaration);
    const sxml_schema_type_alternative_request request = {
        .schema = validator->configuration.schema,
        .element_declaration = element_declaration,
        .element_node = {.representation = SXML_ITEM_REPRESENTATION_NONE}};
    if (declaration->type_alternatives.entry_count == 0) {
        /* 3.3.4.1: without a type table the declared type governs; the
           seam says so too, without a copy. */
        return sxml_schema_select_type_alternative(validator, &request, selected_type);
    }
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_schema_validator_create_tree(validator, &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = send_element_copy(validator, sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status == SXML_STATUS_OK) {
        sxml_schema_type_alternative_request copied = request;
        copied.element_node = sxml_node_item_make(
            (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
        status = sxml_schema_select_type_alternative(validator, &copied, selected_type);
        sxml_item_release(copied.element_node);
        sxml_tree_release(tree);
    }
    sxml_arena_tree_destroy(arena_tree);
    return status;
}
