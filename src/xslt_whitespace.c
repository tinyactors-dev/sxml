/*
 * Whitespace stripping of source documents (src/xslt_whitespace_internal.h;
 * XSLT 4.0, 4.2.2; packet 4.12, D150).
 *
 * Golden file: src/tree.c (a tree builder's events; the copy of a node is
 * sxml_tree_builder_copy_node's loop). The copy goes through a filtering
 * tree builder in front of a new arena tree: every event is passed on
 * except a whitespace text node whose parent element strips. The filter
 * keeps one entry per open element on a stack in allocator memory (D71).
 */
#include "xslt_whitespace_internal.h"

#include "xslt_package_internal.h"

#include "sxml/name.h"
#include "sxml/tree.h"

#include <stdalign.h>
#include <string.h>

/* One name test of an xsl:strip-space or xsl:preserve-space. */
typedef struct space_rule {
    sxml_xslt_ast_name_token name_test;
    bool preserves;
    uint32_t import_precedence;
    /* The default priority (6.4) as a rank: 0 for "*", 1 for "p:*" and
       "*:n", 2 for a name. */
    uint32_t priority_rank;
    /* Declaration order: later wins a tie. */
    uint32_t rule_order;
} space_rule;

/* One open element of the copy. */
typedef struct open_element {
    /* The declarations keep its whitespace text children. */
    bool rule_preserves;
    /* xml:space="preserve" is in force: on it or the nearest ancestor
       with xml:space (4.2.2). */
    bool space_preserves;
} open_element;

typedef struct stripping_filter {
    sxml_tree_builder target;
    sxml_allocator allocator;
    sxml_name_pool *name_pool;
    /* For the type annotations of the elements copied. */
    const sxml_type_registry *type_registry;
    space_rule *rules;
    size_t rule_count;
    open_element *elements;
    size_t element_count;
    size_t element_capacity;
    /* xml:space; 0 when never interned (no document has it). */
    sxml_name_identifier space_attribute_name;
    /* The package whose declarations apply, by its top module (4.2.2:
       xsl:strip-space and xsl:preserve-space apply to the documents a
       package loads; D163's identity of a package). */
    uint32_t package_module;
} stripping_filter;

/* ------------------------------------------------------------ The rules */

static bool is_space_declaration(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(ast, node)->kind;
    return kind == SXML_XSLT_AST_KIND_STRIP_SPACE || kind == SXML_XSLT_AST_KIND_PRESERVE_SPACE;
}

static sxml_xslt_ast_name_token_view tests_of(const sxml_xslt_ast *ast,
                                              sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *elements = sxml_xslt_ast_get_attribute(
        ast, sxml_xslt_ast_find_attribute(
                 ast, (sxml_xslt_ast_attribute_query){.node = node,
                                                      .kind = SXML_XSLT_AST_ATTRIBUTE_ELEMENTS}));
    if (elements->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    }
    return sxml_xslt_ast_get_name_tokens(ast, elements->value.payload.name_tokens);
}

static uint32_t rank_of(sxml_xslt_ast_name_token_kind kind) {
    switch (kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME:
        return 0;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD:
        return 1;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NONE:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ALL:
        break;
    }
    return 2;
}

bool sxml_xslt_strips_whitespace(const sxml_xslt_ast *ast) {
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, node)->kind == SXML_XSLT_AST_KIND_STRIP_SPACE &&
            tests_of(ast, node).name_token_count > 0) {
            return true;
        }
    }
    return false;
}

/* XSLT 4.0, 4.1: some module root asks for input-type-annotations="strip"
   (D179). */
bool sxml_xslt_strips_type_annotations(const sxml_xslt_ast *ast) {
    for (uint32_t module_index = 1; module_index < ast->module_count; module_index += 1) {
        const sxml_xslt_ast_module *module =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module_index});
        const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
            ast, sxml_xslt_ast_find_attribute(
                     ast, (sxml_xslt_ast_attribute_query){
                              .node = module->root,
                              .kind = SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS}));
        if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_KEYWORD &&
            attribute->value.payload.keyword == SXML_XSLT_AST_KEYWORD_STRIP) {
            return true;
        }
    }
    return false;
}

bool sxml_xslt_prepares_documents(const sxml_xslt_ast *ast) {
    return sxml_xslt_strips_whitespace(ast) || sxml_xslt_strips_type_annotations(ast);
}

static uint32_t precedence_of(const sxml_xslt_ast *ast, sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_module(ast, sxml_xslt_ast_get_node(ast, node)->module)
        ->import_precedence;
}

/* Two name tests that match the same names (4.2.2: compared after
   expanding prefixes). */
static bool tests_are_same(sxml_xslt_ast_name_token first, sxml_xslt_ast_name_token second) {
    if (first.kind != second.kind) {
        return false;
    }
    switch (first.kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD:
        return first.name.index == second.name.index;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
        return first.namespace_uri.index == second.namespace_uri.index;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NONE:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ALL:
        break;
    }
    return true;
}

/* Whether a strip and a preserve declaration of one precedence share a
   name test. */
typedef struct declaration_pair {
    sxml_xslt_ast_node_index strip;
    sxml_xslt_ast_node_index preserve;
} declaration_pair;

static bool pair_conflicts(const sxml_xslt_ast *ast, declaration_pair pair) {
    if (precedence_of(ast, pair.strip) != precedence_of(ast, pair.preserve)) {
        return false;
    }
    const sxml_xslt_ast_name_token_view strip_tests = tests_of(ast, pair.strip);
    const sxml_xslt_ast_name_token_view preserve_tests = tests_of(ast, pair.preserve);
    for (size_t strip_index = 0; strip_index < strip_tests.name_token_count; strip_index += 1) {
        for (size_t preserve_index = 0; preserve_index < preserve_tests.name_token_count;
             preserve_index += 1) {
            if (tests_are_same(strip_tests.name_tokens[strip_index],
                               preserve_tests.name_tokens[preserve_index])) {
                return true;
            }
        }
    }
    return false;
}

sxml_status sxml_xslt_check_space_declarations(const sxml_xslt_ast *ast,
                                               sxml_xslt_ast_node_index *conflict) {
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index strip = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_ast_get_node(ast, strip)->kind != SXML_XSLT_AST_KIND_STRIP_SPACE) {
            continue;
        }
        for (uint32_t other = 1; sxml_xslt_ast_get_declaration(ast, other).value != 0;
             other += 1) {
            const sxml_xslt_ast_node_index preserve = sxml_xslt_ast_get_declaration(ast, other);
            if (sxml_xslt_ast_get_node(ast, preserve)->kind == SXML_XSLT_AST_KIND_PRESERVE_SPACE &&
                pair_conflicts(ast, (declaration_pair){.strip = strip, .preserve = preserve})) {
                *conflict = preserve;
                return SXML_STATUS_STATIC_ERROR;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* Collects every name test of the stylesheet's declarations into
   filter->rules (allocated; the caller releases them). */
static sxml_status collect_rules(const sxml_xslt_ast *ast, stripping_filter *filter) {
    size_t rule_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (is_space_declaration(ast, node) &&
            sxml_xslt_package_top_module(ast, node).value == filter->package_module) {
            rule_count += tests_of(ast, node).name_token_count;
        }
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        filter->allocator,
        (sxml_allocation_request){.byte_count = (rule_count + 1) * sizeof(space_rule),
                                  .alignment = alignof(space_rule)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    filter->rules = memory;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (!is_space_declaration(ast, node) ||
            sxml_xslt_package_top_module(ast, node).value != filter->package_module) {
            continue;
        }
        const sxml_xslt_ast_name_token_view tests = tests_of(ast, node);
        for (size_t test_index = 0; test_index < tests.name_token_count; test_index += 1) {
            filter->rules[filter->rule_count] = (space_rule){
                .name_test = tests.name_tokens[test_index],
                .preserves = sxml_xslt_ast_get_node(ast, node)->kind ==
                             SXML_XSLT_AST_KIND_PRESERVE_SPACE,
                .import_precedence = precedence_of(ast, node),
                .priority_rank = rank_of(tests.name_tokens[test_index].kind),
                .rule_order = order};
            filter->rule_count += 1;
        }
    }
    return SXML_STATUS_OK;
}

/* Whether rule's name test matches an element name. */
typedef struct element_name {
    sxml_name_identifier name;
    sxml_name_parts parts;
} element_name;

static bool rule_matches(const stripping_filter *filter, const space_rule *rule,
                         const element_name *element) {
    switch (rule->name_test.kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME:
        return true;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
        return rule->name_test.name.index == element->name.index;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
        return rule->name_test.namespace_uri.index == element->parts.namespace_uri.index;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD: {
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        return sxml_name_pool_get_name_parts(filter->name_pool, rule->name_test.name, &parts) ==
                   SXML_STATUS_OK &&
               sxml_byte_view_equals(parts.local_name, element->parts.local_name);
    }
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NONE:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT:
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ALL:
        break;
    }
    return false;
}

static bool comes_after(const space_rule *candidate, const space_rule *best) {
    if (candidate->import_precedence != best->import_precedence) {
        return candidate->import_precedence > best->import_precedence;
    }
    if (candidate->priority_rank != best->priority_rank) {
        return candidate->priority_rank > best->priority_rank;
    }
    return candidate->rule_order >= best->rule_order;
}

/* 4.2.2: an element keeps its whitespace children when no test matches
   it or the best match is an xsl:preserve-space. */
static bool preserves_element(const stripping_filter *filter, sxml_name_identifier name) {
    element_name element = {.name = name,
                            .parts = {.namespace_uri = {.index = 0},
                                      .local_name = {.bytes = NULL, .byte_count = 0}}};
    if (sxml_name_pool_get_name_parts(filter->name_pool, name, &element.parts) != SXML_STATUS_OK) {
        return true;
    }
    const space_rule *best = NULL;
    for (size_t rule_index = 0; rule_index < filter->rule_count; rule_index += 1) {
        const space_rule *rule = &filter->rules[rule_index];
        if (rule_matches(filter, rule, &element) && (best == NULL || comes_after(rule, best))) {
            best = rule;
        }
    }
    return best == NULL || best->preserves;
}

/* 4.2.2: an element whose type annotation is a simple type or a complex
   type with simple content keeps its whitespace text children whatever
   the declarations say. */
static bool has_simple_content(const stripping_filter *filter, sxml_type_identifier type) {
    if (type.index == SXML_TYPE_NONE || type.index == SXML_TYPE_XS_UNTYPED ||
        filter->type_registry == NULL) {
        return false;
    }
    sxml_type_description description;
    memset(&description, 0, sizeof(description));
    if (sxml_type_registry_describe_type(filter->type_registry, type, &description) !=
        SXML_STATUS_OK) {
        return false;
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_COMPLEX:
        return description.variety_details.complex_type.content_type_variety ==
               SXML_CONTENT_TYPE_VARIETY_SIMPLE;
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ENUMERATION:
        return true;
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_CHOICE:
        break;
    }
    return false;
}

/* ------------------------------------------------------------ The filter */

static bool is_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

static sxml_status filter_start_document(void *state, const sxml_document_start_event *event) {
    const stripping_filter *filter = state;
    return filter->target.operations->start_document(filter->target.state, event);
}

static sxml_status filter_end_document(void *state) {
    const stripping_filter *filter = state;
    return filter->target.operations->end_document(filter->target.state);
}

static sxml_status filter_start_element(void *state, const sxml_element_start_event *event) {
    stripping_filter *filter = state;
    if (filter->element_count == filter->element_capacity) {
        const size_t capacity = filter->element_capacity == 0 ? 16 : filter->element_capacity * 2;
        void *memory = NULL;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(open_element), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                filter->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(open_element)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (filter->element_count > 0) {
            memcpy(memory, filter->elements, filter->element_count * sizeof(open_element));
        }
        sxml_allocator_release(filter->allocator, filter->elements);
        filter->elements = memory;
        filter->element_capacity = capacity;
    }
    /* xml:space is inherited until an element sets it again. */
    filter->elements[filter->element_count] = (open_element){
        .rule_preserves = preserves_element(filter, event->element_name) ||
                          has_simple_content(filter, event->type_annotation),
        .space_preserves = filter->element_count > 0 &&
                           filter->elements[filter->element_count - 1].space_preserves};
    filter->element_count += 1;
    return filter->target.operations->start_element(filter->target.state, event);
}

static sxml_status filter_add_namespace_binding(void *state,
                                                const sxml_namespace_binding *namespace_binding) {
    const stripping_filter *filter = state;
    return filter->target.operations->add_namespace_binding(filter->target.state,
                                                            namespace_binding);
}

static sxml_status filter_add_attribute(void *state, const sxml_attribute_event *event) {
    stripping_filter *filter = state;
    if (event->attribute_name.index == filter->space_attribute_name.index &&
        filter->space_attribute_name.index != 0 && filter->element_count > 0) {
        open_element *element = &filter->elements[filter->element_count - 1];
        if (sxml_byte_view_equals(event->attribute_value, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
            element->space_preserves = true;
        } else if (sxml_byte_view_equals(event->attribute_value,
                                         SXML_BYTE_VIEW_FROM_LITERAL("default"))) {
            /* xml:space="default": the declarations decide again. */
            element->space_preserves = false;
        }
    }
    return filter->target.operations->add_attribute(filter->target.state, event);
}

static sxml_status filter_end_element(void *state) {
    stripping_filter *filter = state;
    if (filter->element_count > 0) {
        filter->element_count -= 1;
    }
    return filter->target.operations->end_element(filter->target.state);
}

static sxml_status filter_add_text(void *state, const sxml_text_event *event) {
    const stripping_filter *filter = state;
    if (filter->element_count > 0 && !filter->elements[filter->element_count - 1].rule_preserves &&
        !filter->elements[filter->element_count - 1].space_preserves && is_whitespace(event->text)) {
        return SXML_STATUS_OK;
    }
    return filter->target.operations->add_text(filter->target.state, event);
}

static sxml_status filter_add_comment(void *state, const sxml_comment_event *event) {
    const stripping_filter *filter = state;
    return filter->target.operations->add_comment(filter->target.state, event);
}

static sxml_status filter_add_processing_instruction(void *state,
                                                     const sxml_processing_instruction_event *event) {
    const stripping_filter *filter = state;
    return filter->target.operations->add_processing_instruction(filter->target.state, event);
}

static sxml_status filter_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    const stripping_filter *filter = state;
    return filter->target.operations->add_unparsed_entity(filter->target.state, entity);
}

static const sxml_tree_builder_operations filter_operations = {
    .start_document = filter_start_document,
    .end_document = filter_end_document,
    .start_element = filter_start_element,
    .add_namespace_binding = filter_add_namespace_binding,
    .add_attribute = filter_add_attribute,
    .end_element = filter_end_element,
    .add_text = filter_add_text,
    .add_comment = filter_add_comment,
    .add_processing_instruction = filter_add_processing_instruction,
    .add_unparsed_entity = filter_add_unparsed_entity,
};

/* -------------------------------------------------------------- The copy */

static bool is_document_node(sxml_item item, sxml_node_reference *node) {
    sxml_node_description description;
    return sxml_item_get_node(item, node) == SXML_STATUS_OK &&
           sxml_node_describe(*node, &description) == SXML_STATUS_OK &&
           description.node_kind == SXML_NODE_KIND_DOCUMENT;
}

/* A document to strip by the declarations of one package (its top
   module's index; 0 for the top-level package's). */
typedef struct package_stripping {
    const sxml_xslt_whitespace_stripping *stripping;
    uint32_t package_module;
} package_stripping;

static sxml_status strip_package_document(package_stripping request, sxml_item document_node,
                                          sxml_item *stripped);

sxml_status sxml_xslt_strip_document(const sxml_xslt_whitespace_stripping *stripping,
                                     sxml_item document_node, sxml_item *stripped) {
    return strip_package_document(
        (package_stripping){.stripping = stripping, .package_module = 0}, document_node, stripped);
}

static sxml_status strip_package_document(package_stripping request, sxml_item document_node,
                                          sxml_item *stripped) {
    const sxml_xslt_whitespace_stripping *stripping = request.stripping;
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    const bool strips_annotations = sxml_xslt_strips_type_annotations(stripping->ast);
    if (!sxml_xslt_prepares_documents(stripping->ast) || !is_document_node(document_node, &node)) {
        sxml_item_retain(document_node);
        *stripped = document_node;
        return SXML_STATUS_OK;
    }
    sxml_processor *processor = stripping->processor;
    stripping_filter filter = {.target = {.state = NULL, .operations = NULL},
                               .allocator = processor->allocator,
                               .name_pool = processor->name_pool,
                               .type_registry =
                                   sxml_item_heap_get_type_registry(processor->item_heap),
                               .rules = NULL,
                               .rule_count = 0,
                               .elements = NULL,
                               .element_count = 0,
                               .element_capacity = 0,
                               .space_attribute_name = {.index = 0},
                               .package_module =
                                   request.package_module == 0 ? 1U : request.package_module};
    (void)sxml_name_pool_find_name(
        processor->name_pool,
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML),
                             .local_name = SXML_BYTE_VIEW_FROM_LITERAL("space")},
        &filter.space_attribute_name);
    sxml_status status = collect_rules(stripping->ast, &filter);
    /* The stripped copy keeps the source locations the document kept
       (the copy carries them, D318), so fn:location answers for a
       stylesheet's input. */
    sxml_source_location document_location = {.line_number = 0};
    if (sxml_node_get_location(node, &document_location) != SXML_STATUS_OK) {
        document_location.line_number = 0;
    }
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        /* As sxml_processor_parse_document's trees: destroyed with the last
           reference (D39). */
        status = sxml_arena_tree_create(
            processor->allocator,
            &(sxml_arena_tree_configuration){
                .allocator = processor->allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = processor->item_heap,
                .maximum_node_count = processor->limits.maximum_tree_node_count,
                .maximum_depth = 0,
                .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
                .keeps_locations = document_location.line_number != 0,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        filter.target = sxml_arena_tree_as_tree_builder(arena_tree);
        status = sxml_tree_builder_copy_node(
            (sxml_tree_builder){.state = &filter, .operations = &filter_operations},
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = true,
                                 .inherits_namespaces = true,
                                 /* 4.1: xs:untyped and xs:untypedAtomic. */
                                 .preserves_type_annotations = !strips_annotations});
    }
    sxml_allocator_release(processor->allocator, filter.rules);
    sxml_allocator_release(processor->allocator, filter.elements);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *stripped = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------- Document preparation */

static sxml_status prepare_document(void *state, const sxml_preparation_request *request,
                                    sxml_item *prepared) {
    return strip_package_document(
        (package_stripping){.stripping = state, .package_module = request->preparation_number},
        request->document_node, prepared);
}

static const sxml_document_preparation_operations stripping_operations = {
    .prepare_document = prepare_document};

sxml_document_preparation sxml_xslt_whitespace_stripping_as_document_preparation(
    const sxml_xslt_whitespace_stripping *stripping) {
    /* The operations only read the stripping. */
    return (sxml_document_preparation){.state = (void *)stripping,
                                       .operations = &stripping_operations};
}
