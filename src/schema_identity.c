/*
 * Identity constraints: compile stage IDENTITY_CONSTRAINTS and their
 * check during validation (src/schema_internal.h; docs/DECISIONS.md D165,
 * D167, D177). Section numbers are of XSD 1.1 Part 1.
 *
 * The stage checks each selector and field against the selector and
 * field subsets of XPath (3.11.6.2, 3.11.6.3) on the XPath front end's
 * AST, compiles it with include/sxml/xpath.h in its namespace context
 * (XPath Valid, 3.13.6.2; xpathDefaultNamespace is the default element
 * namespace) and checks keyrefs against their referenced keys
 * (c-props-correct). There is no second path engine: the subsets are
 * XPath, so the one evaluator runs them.
 *
 * Validation (3.11.4) evaluates them on the recorded subtree of the
 * declaring element (glossary recorded_subtree): the selector with the
 * element as context node gives the target node set, each field with a
 * target node as context node gives at most one node with a simple
 * value, and the typed values of those nodes (D165: derived from the
 * annotations of the recorded tree) make the node's key sequence
 * (glossary key_sequence). Key sequences are compared with XSD's equality
 * (Part 2, 2.2.3): fn:atomic-equal (NaN equal to itself, so "equal or
 * identical") of values of one primitive type. A keyref's referenced key
 * has, at the declaring element, the node table (glossary node_table)
 * made of the qualified node sets of that key at the element and at its
 * descendants (3.11.5): the recorded subtree has them all, so they are
 * evaluated when a keyref needs them, and a key sequence two descendants
 * give with different nodes is resolved level by level as 3.11.5 says,
 * with an explicit work list (D71).
 *
 * Golden files: src/schema_resolve.c (a stage over one table),
 * src/schema_validate_record.c (the recorded subtree).
 */
#include "schema_validate_internal.h"

#include "processor_internal.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "tree_internal.h"
#include "xpath_ast_internal.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ======================================================= Compile stage */

/* Which subset of XPath an expression must be in. */
typedef enum path_subset {
    /* 3.11.6.2: Selector. */
    SXML_PATH_SUBSET_SELECTOR,
    /* 3.11.6.3: a Selector whose last step may be an attribute step. */
    SXML_PATH_SUBSET_FIELD,
} path_subset;

/* What a step of a path is, for the subsets. */
typedef enum step_kind {
    /* Not a step of the subsets. */
    SXML_STEP_KIND_OTHER,
    /* "." */
    SXML_STEP_KIND_SELF,
    /* A NameTest on the child axis, "child::" written or not. */
    SXML_STEP_KIND_CHILD,
    /* "@" NameTest, or "attribute::" NameTest. */
    SXML_STEP_KIND_ATTRIBUTE,
} step_kind;

/* NameTest ::= QName | '*' | NCName ':*' (3.11.6.2, production 4). */
static bool is_subset_name_test(const sxml_ast_node *test) {
    if (test->kind == SXML_AST_KIND_NCNAME || test->kind == SXML_AST_KIND_PREFIXED_NAME) {
        return true;
    }
    return test->kind == SXML_AST_KIND_WILDCARD &&
           (test->detail.name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME ||
            (test->detail.name_test_kind == SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD &&
             (test->flags & (uint32_t)SXML_AST_FLAG_BRACED_URI) == 0));
}

static step_kind classify_step(const sxml_ast *ast, sxml_ast_node_index step) {
    const sxml_ast_node *node = sxml_ast_get_node(ast, step);
    if (node->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE) {
        return SXML_STEP_KIND_SELF;
    }
    if (node->kind != SXML_AST_KIND_AXIS_STEP ||
        sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = step, .field = SXML_AST_FIELD_PREDICATE})
                .value != 0) {
        return SXML_STEP_KIND_OTHER;
    }
    const sxml_ast_node *test = sxml_ast_get_node(
        ast, sxml_ast_find_child(
                 ast, (sxml_ast_child_query){.parent = step, .field = SXML_AST_FIELD_TEST}));
    if (!is_subset_name_test(test)) {
        return SXML_STEP_KIND_OTHER;
    }
    /* 3.11.6.2 clause 2.2 and 3.11.6.3 clause 2.2: the unabbreviated
       child and attribute axes are the same steps. */
    const sxml_ast_axis axis = node->detail.axis;
    if (axis == SXML_AST_AXIS_NONE || axis == SXML_AST_AXIS_CHILD) {
        return SXML_STEP_KIND_CHILD;
    }
    return axis == SXML_AST_AXIS_ATTRIBUTE ? SXML_STEP_KIND_ATTRIBUTE : SXML_STEP_KIND_OTHER;
}

/* One path to check: Path ::= ('.' '//')? Step ('/' Step)*. */
typedef struct path_check {
    sxml_ast_node_index path;
    path_subset subset;
} path_check;

/* A step of a path and where it is. */
typedef struct step_question {
    step_kind kind;
    path_subset subset;
    bool is_last;
} step_question;

static bool is_step_allowed(step_question question) {
    switch (question.kind) {
    case SXML_STEP_KIND_SELF:
    case SXML_STEP_KIND_CHILD:
        return true;
    case SXML_STEP_KIND_ATTRIBUTE:
        /* 3.11.6.3, production 7: only the last step of a field. */
        return question.subset == SXML_PATH_SUBSET_FIELD && question.is_last;
    case SXML_STEP_KIND_OTHER:
        break;
    }
    return false;
}

/* Walks a path from its last step to its first: the front end nests
   "a/b/c" as ((a/b)/c). */
static bool is_path_in_subset(const sxml_ast *ast, path_check check) {
    sxml_ast_node_index current = check.path;
    bool is_last = true;
    while (sxml_ast_get_node(ast, current)->kind == SXML_AST_KIND_RELATIVE_PATH_EXPRESSION) {
        const sxml_ast_node *node = sxml_ast_get_node(ast, current);
        const sxml_ast_node_index left = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = current, .field = SXML_AST_FIELD_LEFT});
        const sxml_ast_node_index right = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = current, .field = SXML_AST_FIELD_RIGHT});
        if (!is_step_allowed((step_question){
                .kind = classify_step(ast, right), .subset = check.subset, .is_last = is_last})) {
            return false;
        }
        if (node->detail.operator== SXML_AST_OPERATOR_DOUBLE_SLASH) {
            /* Only ".//" before the first step. */
            return sxml_ast_get_node(ast, left)->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE;
        }
        if (node->detail.operator!= SXML_AST_OPERATOR_SLASH) {
            return false;
        }
        is_last = false;
        current = left;
    }
    return is_step_allowed((step_question){
        .kind = classify_step(ast, current), .subset = check.subset, .is_last = is_last});
}

/* Selector ::= Path ( '|' Path )*: a union nests as ((p1 | p2) | p3). */
static bool is_expression_in_subset(const sxml_ast *ast, path_subset subset) {
    const sxml_ast_node_index root = ast->root;
    if (sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = root, .field = SXML_AST_FIELD_DECLARATION})
            .value != 0) {
        return false;
    }
    sxml_ast_node_index current = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = root, .field = SXML_AST_FIELD_BODY});
    while (sxml_ast_get_node(ast, current)->kind == SXML_AST_KIND_UNION_EXPRESSION) {
        const sxml_ast_node_index right = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = current, .field = SXML_AST_FIELD_RIGHT});
        if (!is_path_in_subset(ast, (path_check){.path = right, .subset = subset})) {
            return false;
        }
        current = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = current, .field = SXML_AST_FIELD_LEFT});
    }
    return is_path_in_subset(ast, (path_check){.path = current, .subset = subset});
}

/* What the stage keeps for its whole run. */
typedef struct identity_stage {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_processor *processor;
} identity_stage;

static sxml_status report(identity_stage *stage, const sxml_schema_report_request *request) {
    return sxml_schema_compilation_report(stage->compilation, request);
}

/* One selector or field to check and compile. */
typedef struct record_compilation {
    sxml_schema_xpath_record_index record;
    path_subset subset;
} record_compilation;

/* Whether the record's expression is in its subset; a text that is no
   XPath expression is not. */
/* What check_subset found. */
typedef struct subset_outcome {
    bool is_in_subset;
} subset_outcome;

static sxml_status check_subset(identity_stage *stage, record_compilation request,
                                subset_outcome *outcome) {
    const sxml_schema_xpath_record *record =
        sxml_schema_get_xpath_record(stage->components, request.record);
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_ast *ast = NULL;
    const sxml_status status = sxml_xpath_build_ast(
        &(sxml_ast_build_request){.expression_text =
                                      sxml_schema_get_text(stage->components, record->expression),
                                  .source_name = {.bytes = NULL, .byte_count = 0},
                                  .allocator = stage->components->allocator,
                                  .maximum_expression_depth =
                                      sxml_processor_get_maximum_expression_depth(stage->processor),
                                  .error_record = &error_record},
        &ast);
    if (status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_INVALID_UTF8) {
        outcome->is_in_subset = false;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    outcome->is_in_subset = is_expression_in_subset(ast, request.subset);
    sxml_ast_destroy(ast);
    return SXML_STATUS_OK;
}

static sxml_status compile_record(identity_stage *stage, record_compilation request) {
    const sxml_schema_xpath_record *record =
        sxml_schema_get_xpath_record(stage->components, request.record);
    if (request.record.value == 0 || record->compiled_expression != NULL) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view constraint_name = request.subset == SXML_PATH_SUBSET_SELECTOR
                                               ? TEXT("c-selector-xpath")
                                               : TEXT("c-fields-xpath");
    const sxml_schema_position position = record->position;
    subset_outcome subset = {.is_in_subset = false};
    sxml_status status = check_subset(stage, request, &subset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!subset.is_in_subset) {
        return report(stage, &(sxml_schema_report_request){
                                 .constraint_name = constraint_name,
                                 .description = request.subset == SXML_PATH_SUBSET_SELECTOR
                                                    ? TEXT("The selector is not in the selector "
                                                           "subset of XPath")
                                                    : TEXT("The field is not in the field subset "
                                                           "of XPath"),
                                 .detailed_description = {.bytes = NULL, .byte_count = 0},
                                 .position = position});
    }
    /* 3.13.6.2 XPath Valid (an undeclared prefix, say). */
    return sxml_schema_compile_xpath_record(
        stage->compilation,
        &(sxml_schema_xpath_compilation){.record = request.record,
                                         .declares_value = false,
                                         .constraint_name = constraint_name,
                                         .description = TEXT("The expression is not valid XPath")});
}

/* 3.11.6.1 c-props-correct: a keyref refers to a key or unique with as
   many fields. */
static sxml_status check_referenced_key(identity_stage *stage,
                                        const sxml_schema_identity_constraint *constraint) {
    if (constraint->category != SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF ||
        constraint->referenced_key.value == 0) {
        /* Not a keyref, or src-resolve reported it. */
        return SXML_STATUS_OK;
    }
    const sxml_schema_identity_constraint *referenced =
        sxml_schema_get_identity_constraint(stage->components, constraint->referenced_key);
    if (referenced->category == SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF) {
        return report(stage, &(sxml_schema_report_request){
                                 .constraint_name = TEXT("c-props-correct.1"),
                                 .description = TEXT("A keyref refers to a keyref, not to a key "
                                                     "or unique"),
                                 .detailed_description = {.bytes = NULL, .byte_count = 0},
                                 .position = constraint->position});
    }
    if (referenced->fields.entry_count != constraint->fields.entry_count) {
        return report(stage, &(sxml_schema_report_request){
                                 .constraint_name = TEXT("c-props-correct.2"),
                                 .description = TEXT("A keyref has not as many fields as its key"),
                                 .detailed_description = {.bytes = NULL, .byte_count = 0},
                                 .position = constraint->position});
    }
    return SXML_STATUS_OK;
}

static sxml_status compile_constraint(identity_stage *stage, uint32_t constraint_index) {
    const sxml_schema_identity_constraint *constraint = sxml_schema_get_identity_constraint(
        stage->components, (sxml_schema_identity_constraint_index){.value = constraint_index});
    if (constraint->selector.value == 0) {
        /* A ref= one (3.11.2): its definition is compiled. */
        return SXML_STATUS_OK;
    }
    const sxml_schema_identity_constraint copied = *constraint;
    sxml_status status =
        compile_record(stage, (record_compilation){.record = copied.selector,
                                                   .subset = SXML_PATH_SUBSET_SELECTOR});
    const sxml_schema_index_view fields = sxml_schema_get_list(stage->components, copied.fields);
    for (size_t field_index = 0; status == SXML_STATUS_OK && field_index < fields.index_count;
         field_index += 1) {
        status = compile_record(
            stage, (record_compilation){.record = {.value = fields.indexes[field_index]},
                                        .subset = SXML_PATH_SUBSET_FIELD});
    }
    if (status == SXML_STATUS_OK) {
        status = check_referenced_key(stage, &copied);
    }
    return status;
}

sxml_status sxml_schema_compile_identity_constraints(sxml_schema_compilation *compilation) {
    sxml_schema_components *components = compilation->components;
    identity_stage stage = {.compilation = compilation,
                            .components = components,
                            .processor = components->processor};
    const uint32_t constraint_count =
        components->tables[SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT].entry_count;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t constraint_index = 1;
         status == SXML_STATUS_OK && constraint_index < constraint_count; constraint_index += 1) {
        status = compile_constraint(&stage, constraint_index);
    }
    return status;
}

/* ========================================================== Validation */

/* One member of a qualified node set and its key sequence. */
typedef struct key_entry {
    /* The member: an element of the recorded tree, by node index. */
    uint32_t node_index;
    /* The element whose qualified node set it is in. */
    uint32_t source_element;
    /* Its key sequence: field_count item counts from first_count in the
       table's counts, their items from first_item in its items. */
    size_t first_count;
    size_t first_item;
    uint64_t hash;
} key_entry;

/* Key sequences of one identity constraint (glossary key_sequence),
   with a hash index of them. */
typedef struct key_table {
    size_t field_count;
    /* key_entry. */
    sxml_schema_scratch_array entries;
    /* uint32_t per field of each entry. */
    sxml_schema_scratch_array counts;
    /* sxml_item, owned. */
    sxml_schema_scratch_array items;
    /* uint32_t: an entry index + 1, 0 for an empty slot; a power of two
       long. */
    sxml_schema_scratch_array slots;
} key_table;

static key_table empty_table(void) {
    return (key_table){.field_count = 0,
                       .entries = {.bytes = NULL,
                                   .entry_byte_count = sizeof(key_entry),
                                   .entry_count = 0,
                                   .entry_capacity = 0},
                       .counts = {.bytes = NULL,
                                  .entry_byte_count = sizeof(uint32_t),
                                  .entry_count = 0,
                                  .entry_capacity = 0},
                       .items = {.bytes = NULL,
                                 .entry_byte_count = sizeof(sxml_item),
                                 .entry_count = 0,
                                 .entry_capacity = 0},
                       .slots = {.bytes = NULL,
                                 .entry_byte_count = sizeof(uint32_t),
                                 .entry_count = 0,
                                 .entry_capacity = 0}};
}

/* Releases the items from first_item on, and forgets them. */
static void truncate_items(key_table *table, size_t first_item) {
    for (size_t item_index = first_item; item_index < table->items.entry_count; item_index += 1) {
        const sxml_item *item = sxml_schema_scratch_array_at(&table->items, item_index);
        sxml_item_release(*item);
    }
    if (first_item < table->items.entry_count) {
        table->items.entry_count = first_item;
    }
}

static void release_table(sxml_allocator allocator, key_table *table) {
    truncate_items(table, 0);
    sxml_schema_scratch_array_release(allocator, &table->entries);
    sxml_schema_scratch_array_release(allocator, &table->counts);
    sxml_schema_scratch_array_release(allocator, &table->items);
    sxml_schema_scratch_array_release(allocator, &table->slots);
}

/* What one check of an element reads. */
typedef struct identity_check {
    sxml_schema_validator *validator;
    const sxml_schema_recorded_subtree *subtree;
    sxml_tree *tree;
} identity_check;

static sxml_node_reference node_at(const identity_check *check, uint32_t node_index) {
    return (sxml_node_reference){
        .tree = check->tree, .node_index = {.index = node_index}, .namespace_number = 0};
}

static sxml_schema_element_declaration_index declaration_at(const identity_check *check,
                                                            uint32_t node_index) {
    if (node_index >= check->subtree->element_declaration_count) {
        return (sxml_schema_element_declaration_index){.value = 0};
    }
    return check->subtree->element_declarations[node_index];
}

static sxml_source_location location_of(const identity_check *check, uint32_t node_index) {
    sxml_source_location location;
    memset(&location, 0, sizeof(location));
    if (check->tree->operations->get_location != NULL &&
        check->tree->operations->get_location(check->tree->state,
                                              (sxml_node_index){.index = node_index},
                                              &location) != SXML_STATUS_OK) {
        memset(&location, 0, sizeof(location));
    }
    return location;
}

/* A cvc-identity-constraint error at a node. */
typedef struct identity_error {
    const char *constraint_name;
    const char *description;
    uint32_t node_index;
    /* The identity constraint broken, named in the report. */
    const sxml_schema_identity_constraint *constraint;
} identity_error;

/* "element 'E': DESCRIPTION (key 'K')". */
static sxml_status report_error(const identity_check *check, identity_error error) {
    sxml_node_description description;
    sxml_schema_written_name element = {.name = {.index = 0}, .namespace_prefix = {.index = 0}};
    if (sxml_node_describe(node_at(check, error.node_index), &description) == SXML_STATUS_OK) {
        element = (sxml_schema_written_name){.name = description.node_name,
                                             .namespace_prefix = description.namespace_prefix};
    }
    static const sxml_byte_view templates[] = {
        [SXML_SCHEMA_IDENTITY_CONSTRAINT_UNIQUE] = SXML_BYTE_VIEW_INITIALIZER("%e: %r (unique %n)"),
        [SXML_SCHEMA_IDENTITY_CONSTRAINT_KEY] = SXML_BYTE_VIEW_INITIALIZER("%e: %r (key %n)"),
        [SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF] = SXML_BYTE_VIEW_INITIALIZER("%e: %r (keyref %n)"),
    };
    return sxml_schema_validator_report_error(
        check->validator,
        &(sxml_schema_validation_error){
            .constraint_name = sxml_byte_view_from_c_string(error.constraint_name),
            .message = {.text = templates[error.constraint->category],
                        .element = element,
                        .reason = sxml_byte_view_from_c_string(error.description),
                        .name = error.constraint->constraint_name},
            .location = location_of(check, error.node_index)});
}

/* Evaluates expression with the node as context node into *result. */
typedef struct path_evaluation {
    const sxml_xpath_expression *expression;
    uint32_t context_node;
} path_evaluation;

static sxml_status evaluate_path(const identity_check *check, path_evaluation evaluation,
                                 sxml_sequence *result) {
    if (evaluation.expression == NULL) {
        /* IDENTITY_CONSTRAINTS compiles every record of a schema that
           compiled. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_item context_item = sxml_node_item_make(node_at(check, evaluation.context_node));
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    const sxml_status status = sxml_xpath_expression_evaluate(
        evaluation.expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context = {.has_context_value = true,
                                .context_value = sxml_sequence_from_item(context_item)},
            .error_record = &error_record},
        result);
    sxml_item_release(context_item);
    return status;
}

/* How a node a field selects takes part (3.11.4 clause 3). */
typedef enum field_node_kind {
    /* Skipped (not assessed): it takes no part. */
    SXML_FIELD_NODE_SKIPPED,
    /* Its governing type is simple or has simple content. */
    SXML_FIELD_NODE_SIMPLE,
    /* Neither: the field breaks clause 3. */
    SXML_FIELD_NODE_OTHER,
} field_node_kind;

/* A node a field selected. */
typedef struct field_node {
    uint32_t node_index;
    const sxml_node_description *description;
} field_node;

static sxml_status classify_field_node(const identity_check *check, field_node node,
                                       field_node_kind *kind) {
    const sxml_node_description *description = node.description;
    const sxml_type_identifier annotation = description->type_annotation;
    if (description->node_kind == SXML_NODE_KIND_ATTRIBUTE) {
        /* An attribute nothing governed is xs:untypedAtomic (D173). */
        *kind = annotation.index == SXML_TYPE_XS_UNTYPED_ATOMIC ? SXML_FIELD_NODE_SKIPPED
                                                                : SXML_FIELD_NODE_SIMPLE;
        return SXML_STATUS_OK;
    }
    if (description->node_kind != SXML_NODE_KIND_ELEMENT) {
        *kind = SXML_FIELD_NODE_OTHER;
        return SXML_STATUS_OK;
    }
    if (declaration_at(check, node.node_index).value == 0) {
        /* Assessed laxly or skipped: no governing declaration. */
        *kind = SXML_FIELD_NODE_SKIPPED;
        return SXML_STATUS_OK;
    }
    sxml_type_description type;
    const sxml_status status =
        sxml_type_registry_describe_type(check->validator->type_registry, annotation, &type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *kind = type.variety != SXML_TYPE_VARIETY_COMPLEX ||
                    type.variety_details.complex_type.content_type_variety ==
                        SXML_CONTENT_TYPE_VARIETY_SIMPLE
                ? SXML_FIELD_NODE_SIMPLE
                : SXML_FIELD_NODE_OTHER;
    return SXML_STATUS_OK;
}

/* The one node of a field's result with a simple governing type. */
typedef struct field_outcome {
    /* Clause 3 holds: no node of another kind, at most one simple. */
    bool is_valid;
    bool has_node;
    uint32_t node_index;
} field_outcome;

static sxml_status read_field_result(const identity_check *check, sxml_sequence result,
                                     field_outcome *outcome) {
    *outcome = (field_outcome){.is_valid = true, .has_node = false, .node_index = 0};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(result);
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        sxml_node_description description;
        field_node_kind kind = SXML_FIELD_NODE_OTHER;
        status = sxml_item_get_node(step.item, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        if (status == SXML_STATUS_OK) {
            status = classify_field_node(
                check,
                (field_node){.node_index = node.node_index.index, .description = &description},
                &kind);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (kind == SXML_FIELD_NODE_SKIPPED) {
            continue;
        }
        if (kind == SXML_FIELD_NODE_OTHER || outcome->has_node) {
            outcome->is_valid = false;
        }
        outcome->has_node = true;
        outcome->node_index = node.node_index.index;
    }
    return status;
}

static uint64_t mix_hash(uint64_t hash, uint64_t value) {
    /* FNV-1a over the 64-bit words. */
    return (hash ^ value) * 1099511628211ULL;
}

/* The primitive type XSD compares a value in (Part 2, 2.2.3: values of
   different primitive types are never equal); xs:untypedAtomic values
   (of xs:anySimpleType) are strings. */
static sxml_type_identifier comparison_primitive(const identity_check *check, sxml_item item) {
    sxml_type_description description;
    if (sxml_type_registry_describe_type(check->validator->type_registry,
                                         sxml_item_get_type_annotation(item),
                                         &description) != SXML_STATUS_OK) {
        return (sxml_type_identifier){.index = SXML_TYPE_NONE};
    }
    if (description.primitive_type.index == SXML_TYPE_XS_UNTYPED_ATOMIC) {
        return (sxml_type_identifier){.index = SXML_TYPE_XS_STRING};
    }
    return description.primitive_type;
}

/* XSD's equality of two atomic values (Part 2, 2.2.3), with identity:
   NaN is identical to itself, as fn:atomic-equal says. */
typedef struct value_pair {
    sxml_item first;
    sxml_item second;
} value_pair;

static bool values_are_equal(const identity_check *check, value_pair pair) {
    return sxml_atomic_item_is_atomic_equal(pair.first, pair.second) &&
           comparison_primitive(check, pair.first).index ==
               comparison_primitive(check, pair.second).index;
}

/* Two entries of key tables. */
typedef struct entry_pair {
    const key_table *first_table;
    const key_entry *first;
    const key_table *second_table;
    const key_entry *second;
} entry_pair;

/* 3.11.4: key sequences are equal member for member; an atomic value
   equals a list of one item that equals it (the note after 3.11.4). */
static bool keys_are_equal(const identity_check *check, entry_pair pair) {
    if (pair.first->hash != pair.second->hash ||
        pair.first_table->field_count != pair.second_table->field_count) {
        return false;
    }
    size_t first_item = pair.first->first_item;
    size_t second_item = pair.second->first_item;
    for (size_t field_index = 0; field_index < pair.first_table->field_count; field_index += 1) {
        const uint32_t *first_count = sxml_schema_scratch_array_at(
            &pair.first_table->counts, pair.first->first_count + field_index);
        const uint32_t *second_count = sxml_schema_scratch_array_at(
            &pair.second_table->counts, pair.second->first_count + field_index);
        if (*first_count != *second_count) {
            return false;
        }
        for (uint32_t item_index = 0; item_index < *first_count; item_index += 1) {
            const sxml_item *first =
                sxml_schema_scratch_array_at(&pair.first_table->items, first_item + item_index);
            const sxml_item *second =
                sxml_schema_scratch_array_at(&pair.second_table->items, second_item + item_index);
            if (!values_are_equal(check, (value_pair){.first = *first, .second = *second})) {
                return false;
            }
        }
        first_item += *first_count;
        second_item += *first_count;
    }
    return true;
}

/* A field's value to add to a key sequence. */
typedef struct value_request {
    uint32_t node_index;
    /* In/out: the hash of the key sequence so far. */
    uint64_t *hash;
} value_request;

/* Appends the typed value of a field's node to the table's items and its
   item count to the table's counts. */
static sxml_status append_value(const identity_check *check, value_request request,
                                key_table *table) {
    uint64_t *hash = request.hash;
    sxml_sequence typed_value = sxml_sequence_empty();
    sxml_status status = sxml_node_make_typed_value(
        check->validator->item_heap,
        &(sxml_node_value_request){.node = node_at(check, request.node_index),
                                   .error_record = NULL},
        &typed_value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t item_count = 0;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(typed_value);
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = sxml_schema_scratch_array_append(check->validator->allocator, &table->items,
                                                  &step.item);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_item_retain(step.item);
        *hash = mix_hash(*hash, sxml_atomic_item_hash(step.item));
        item_count += 1;
    }
    sxml_sequence_release(typed_value);
    if (status == SXML_STATUS_OK) {
        *hash = mix_hash(*hash, item_count);
        status = sxml_schema_scratch_array_append(check->validator->allocator, &table->counts,
                                                  &item_count);
    }
    return status;
}

/* The qualified node set of a constraint at one element. */
typedef struct collection {
    const sxml_schema_identity_constraint *constraint;
    uint32_t element;
    /* Report what breaks clauses 3 and 4.2 (false for a referenced key
       evaluated for a keyref: its own check reports them). */
    bool reports;
} collection;

/* One member of the target node set: its key sequence, or why it is not
   qualified. */
typedef struct member_request {
    collection request;
    uint32_t node_index;
} member_request;

static sxml_status collect_member(const identity_check *check, member_request member,
                                  key_table *table) {
    const sxml_schema_components *components = check->validator->components;
    const sxml_schema_identity_constraint *constraint = member.request.constraint;
    const bool is_key = constraint->category == SXML_SCHEMA_IDENTITY_CONSTRAINT_KEY;
    const sxml_schema_index_view fields = sxml_schema_get_list(components, constraint->fields);
    key_entry entry = {.node_index = member.node_index,
                       .source_element = member.request.element,
                       .first_count = table->counts.entry_count,
                       .first_item = table->items.entry_count,
                       .hash = 14695981039346656037ULL};
    bool is_qualified = true;
    bool has_reported = false;
    sxml_status status = SXML_STATUS_OK;
    for (size_t field_index = 0;
         status == SXML_STATUS_OK && is_qualified && field_index < fields.index_count;
         field_index += 1) {
        const sxml_schema_xpath_record *record = sxml_schema_get_xpath_record(
            components, (sxml_schema_xpath_record_index){.value = fields.indexes[field_index]});
        sxml_sequence result = sxml_sequence_empty();
        status = evaluate_path(check,
                               (path_evaluation){.expression = record->compiled_expression,
                                                 .context_node = member.node_index},
                               &result);
        if (status != SXML_STATUS_OK) {
            break;
        }
        field_outcome field;
        status = read_field_result(check, result, &field);
        sxml_node_description description;
        memset(&description, 0, sizeof(description));
        if (status == SXML_STATUS_OK && field.has_node) {
            status = sxml_node_describe(node_at(check, field.node_index), &description);
        }
        if (status == SXML_STATUS_OK && !field.is_valid) {
            is_qualified = false;
            if (member.request.reports) {
                has_reported = true;
                status = report_error(
                    check, (identity_error){.constraint_name = "cvc-identity-constraint.3",
                                            .description = "a field selects more than one node, "
                                                           "or a node without a simple type",
                                            .node_index = member.node_index,
                                            .constraint = constraint});
            }
        } else if (status == SXML_STATUS_OK &&
                   (!field.has_node ||
                    (description.node_kind == SXML_NODE_KIND_ELEMENT && description.is_nilled))) {
            /* No [schema actual value]: not qualified (clause 4). */
            is_qualified = false;
        } else if (status == SXML_STATUS_OK) {
            status = append_value(
                check, (value_request){.node_index = field.node_index, .hash = &entry.hash}, table);
            const sxml_schema_element_declaration *declaration =
                sxml_schema_get_element_declaration(components,
                                                    declaration_at(check, field.node_index));
            if (status == SXML_STATUS_OK && is_key && member.request.reports &&
                description.node_kind == SXML_NODE_KIND_ELEMENT && declaration->is_nillable) {
                has_reported = true;
                status = report_error(
                    check, (identity_error){.constraint_name = "cvc-identity-constraint.4.2.3",
                                            .description = "a key field is an element whose "
                                                           "declaration is nillable",
                                            .node_index = field.node_index,
                                            .constraint = constraint});
            }
        }
        sxml_sequence_release(result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_qualified) {
        truncate_items(table, entry.first_item);
        table->counts.entry_count = entry.first_count;
        if (is_key && member.request.reports && !has_reported) {
            /* 4.2.1: every member of a key's target node set is
               qualified. */
            return report_error(check,
                                (identity_error){.constraint_name = "cvc-identity-constraint.4.2.1",
                                                 .description = "a key field has no value",
                                                 .node_index = member.node_index,
                                                 .constraint = constraint});
        }
        return SXML_STATUS_OK;
    }
    return sxml_schema_scratch_array_append(check->validator->allocator, &table->entries, &entry);
}

/* Appends the qualified node set of request.constraint at
   request.element to table (3.11.4 clauses 1 to 4). */
static sxml_status collect(const identity_check *check, collection request, key_table *table) {
    const sxml_schema_components *components = check->validator->components;
    table->field_count = request.constraint->fields.entry_count;
    sxml_sequence targets = sxml_sequence_empty();
    sxml_status status =
        evaluate_path(check,
                      (path_evaluation){.expression = sxml_schema_get_xpath_record(
                                                          components, request.constraint->selector)
                                                          ->compiled_expression,
                                        .context_node = request.element},
                      &targets);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(targets);
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        status = sxml_item_get_node(step.item, &node);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (declaration_at(check, node.node_index.index).value == 0) {
            /* Clause 1: skipped elements are not in the target node
               set. */
            continue;
        }
        status = collect_member(
            check, (member_request){.request = request, .node_index = node.node_index.index},
            table);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    sxml_sequence_release(targets);
    return status;
}

/* Sizes the slots of table for its entries (at least twice as many, a
   power of two) and empties them. */
static sxml_status prepare_slots(sxml_allocator allocator, key_table *table) {
    size_t slot_count = 16;
    while (slot_count < table->entries.entry_count * 2U) {
        slot_count *= 2U;
    }
    table->slots.entry_count = 0;
    const uint32_t empty_slot = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < slot_count;
         slot_index += 1) {
        status = sxml_schema_scratch_array_append(allocator, &table->slots, &empty_slot);
    }
    return status;
}

/* The slot an entry's search starts at, and the next one. */
static size_t first_slot(const key_table *table, uint64_t hash) {
    return (size_t)(hash & (uint64_t)(table->slots.entry_count - 1U));
}

static size_t next_slot(const key_table *table, size_t slot_index) {
    return (slot_index + 1U) & (table->slots.entry_count - 1U);
}

static void insert_entry(key_table *table, uint32_t entry_index) {
    const key_entry *entry = sxml_schema_scratch_array_at(&table->entries, entry_index);
    size_t slot_index = first_slot(table, entry->hash);
    for (;;) {
        uint32_t *slot = sxml_schema_scratch_array_at(&table->slots, slot_index);
        if (*slot == 0) {
            *slot = entry_index + 1U;
            return;
        }
        slot_index = next_slot(table, slot_index);
    }
}

/* 3.11.4 clauses 4.1 and 4.2.2: no two members of the qualified node set
   have equal key sequences. */
static sxml_status check_unique(const identity_check *check, key_table *table,
                                const sxml_schema_identity_constraint *constraint) {
    const bool is_key = constraint->category == SXML_SCHEMA_IDENTITY_CONSTRAINT_KEY;
    sxml_status status = prepare_slots(check->validator->allocator, table);
    for (uint32_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < table->entries.entry_count; entry_index += 1) {
        const key_entry *entry = sxml_schema_scratch_array_at(&table->entries, entry_index);
        bool is_duplicate = false;
        for (size_t slot_index = first_slot(table, entry->hash);;
             slot_index = next_slot(table, slot_index)) {
            const uint32_t *slot = sxml_schema_scratch_array_at(&table->slots, slot_index);
            if (*slot == 0) {
                break;
            }
            const key_entry *other = sxml_schema_scratch_array_at(&table->entries, *slot - 1U);
            if (keys_are_equal(check, (entry_pair){.first_table = table,
                                                   .first = entry,
                                                   .second_table = table,
                                                   .second = other})) {
                is_duplicate = true;
                break;
            }
        }
        if (is_duplicate) {
            status = report_error(
                check,
                (identity_error){.constraint_name = is_key ? "cvc-identity-constraint.4.2.2"
                                                           : "cvc-identity-constraint.4.1",
                                 .description = "its key sequence is that of an earlier member",
                                 .node_index = entry->node_index,
                                 .constraint = constraint});
        } else {
            insert_entry(table, entry_index);
        }
    }
    return status;
}

/* ------------------------------------------------------------ Keyrefs */

/* What an element's node table holds for one key sequence while 3.11.5
   is resolved upwards. */
typedef enum binding_state {
    SXML_BINDING_STATE_NONE,
    SXML_BINDING_STATE_NODE,
    /* Children gave the key sequence with different nodes: no entry. */
    SXML_BINDING_STATE_CONFLICT,
} binding_state;

/* One element on the way up (glossary node_table). */
typedef struct table_binding {
    uint32_t element;
    uint32_t node_index;
    binding_state state;
    /* The entry is the element's own (3.11.5 clause 2): it wins over its
       children's. */
    bool is_own;
} table_binding;

/* Entries of the referenced key with the keyref member's key sequence. */
typedef struct match_resolution {
    /* table_binding, one per element with an own entry. */
    sxml_schema_scratch_array *bindings;
    uint32_t keyref_element;
} match_resolution;

static table_binding *find_binding(sxml_schema_scratch_array *bindings, uint32_t element) {
    for (size_t binding_index = 0; binding_index < bindings->entry_count; binding_index += 1) {
        table_binding *binding = sxml_schema_scratch_array_at(bindings, binding_index);
        if (binding->element == element) {
            return binding;
        }
    }
    return NULL;
}

/* Merges a child's entry into an element's (3.11.5): a conflict among
   children leaves no entry; an own entry stays as it is. */
static void merge_binding(table_binding *parent, table_binding child) {
    if (parent->is_own || child.state != SXML_BINDING_STATE_NODE) {
        return;
    }
    if (parent->state == SXML_BINDING_STATE_NONE) {
        parent->state = SXML_BINDING_STATE_NODE;
        parent->node_index = child.node_index;
    } else if (parent->state == SXML_BINDING_STATE_NODE && parent->node_index != child.node_index) {
        parent->state = SXML_BINDING_STATE_CONFLICT;
    }
}

/* Carries the bindings up to the keyref's element, deepest first (a
   descendant has a larger node index than its ancestors), and writes
   whether its node table has an entry to outcome. */
/* Whether a node table has a key sequence. */
typedef struct key_search_outcome {
    bool is_found;
} key_search_outcome;

static sxml_status resolve_upwards(const identity_check *check, match_resolution resolution,
                                   key_search_outcome *outcome) {
    sxml_schema_scratch_array *bindings = resolution.bindings;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        size_t deepest = bindings->entry_count;
        for (size_t binding_index = 0; binding_index < bindings->entry_count; binding_index += 1) {
            const table_binding *binding = sxml_schema_scratch_array_at(bindings, binding_index);
            const table_binding *best = deepest < bindings->entry_count
                                            ? sxml_schema_scratch_array_at(bindings, deepest)
                                            : NULL;
            if (binding->element != resolution.keyref_element &&
                (best == NULL || binding->element > best->element)) {
                deepest = binding_index;
            }
        }
        if (deepest == bindings->entry_count) {
            break;
        }
        const table_binding child =
            *(const table_binding *)sxml_schema_scratch_array_at(bindings, deepest);
        /* Remove it: the last binding takes its place. */
        const table_binding *last =
            sxml_schema_scratch_array_at(bindings, bindings->entry_count - 1U);
        memcpy(sxml_schema_scratch_array_at(bindings, deepest), last, sizeof(table_binding));
        bindings->entry_count -= 1;
        sxml_node_description description;
        status = sxml_node_describe(node_at(check, child.element), &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!description.has_parent) {
            continue;
        }
        table_binding *parent = find_binding(bindings, description.parent.index);
        if (parent == NULL) {
            const table_binding added = {.element = description.parent.index,
                                         .node_index = 0,
                                         .state = SXML_BINDING_STATE_NONE,
                                         .is_own = false};
            status =
                sxml_schema_scratch_array_append(check->validator->allocator, bindings, &added);
            if (status != SXML_STATUS_OK) {
                break;
            }
            parent = find_binding(bindings, description.parent.index);
        }
        merge_binding(parent, child);
    }
    const table_binding *top = find_binding(bindings, resolution.keyref_element);
    outcome->is_found =
        status == SXML_STATUS_OK && top != NULL && top->state == SXML_BINDING_STATE_NODE;
    return status;
}

/* A keyref member and the referenced key's entries. */
typedef struct keyref_lookup {
    const key_table *keyref_table;
    const key_entry *member;
    const key_table *key_table;
    uint32_t keyref_element;
    /* table_binding: room for the resolution. */
    sxml_schema_scratch_array *bindings;
} keyref_lookup;

/* 3.11.4 clause 4.3: the node table of the referenced key at the
   keyref's element has the member's key sequence. */
static sxml_status find_key(const identity_check *check, keyref_lookup lookup,
                            key_search_outcome *outcome) {
    sxml_schema_scratch_array *bindings = lookup.bindings;
    bindings->entry_count = 0;
    bool has_two_nodes = false;
    uint32_t first_node = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t slot_index = first_slot(lookup.key_table, lookup.member->hash);;
         slot_index = next_slot(lookup.key_table, slot_index)) {
        const uint32_t *slot = sxml_schema_scratch_array_at(&lookup.key_table->slots, slot_index);
        if (*slot == 0) {
            break;
        }
        const key_entry *entry =
            sxml_schema_scratch_array_at(&lookup.key_table->entries, *slot - 1U);
        if (!keys_are_equal(check, (entry_pair){.first_table = lookup.keyref_table,
                                                .first = lookup.member,
                                                .second_table = lookup.key_table,
                                                .second = entry})) {
            continue;
        }
        if (bindings->entry_count == 0) {
            first_node = entry->node_index;
        }
        has_two_nodes = has_two_nodes || entry->node_index != first_node;
        table_binding *binding = find_binding(bindings, entry->source_element);
        if (binding != NULL) {
            /* An element's own qualified node set gives the sequence
               twice: its key is broken, and its table has no entry. */
            if (binding->node_index != entry->node_index) {
                binding->state = SXML_BINDING_STATE_CONFLICT;
            }
            continue;
        }
        const table_binding added = {.element = entry->source_element,
                                     .node_index = entry->node_index,
                                     .state = SXML_BINDING_STATE_NODE,
                                     .is_own = true};
        status = sxml_schema_scratch_array_append(check->validator->allocator, bindings, &added);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (!has_two_nodes) {
        outcome->is_found = bindings->entry_count != 0;
        return SXML_STATUS_OK;
    }
    return resolve_upwards(
        check, (match_resolution){.bindings = bindings, .keyref_element = lookup.keyref_element},
        outcome);
}

/* Whether the declaration of an element lists the constraint. */
static bool declares_constraint(const identity_check *check, uint32_t node_index,
                                sxml_schema_identity_constraint_index constraint) {
    const sxml_schema_element_declaration_index declaration = declaration_at(check, node_index);
    if (declaration.value == 0) {
        return false;
    }
    const sxml_schema_index_view constraints = sxml_schema_get_list(
        check->validator->components,
        sxml_schema_get_element_declaration(check->validator->components, declaration)
            ->identity_constraints);
    for (size_t constraint_index = 0; constraint_index < constraints.index_count;
         constraint_index += 1) {
        if (constraints.indexes[constraint_index] == constraint.value) {
            return true;
        }
    }
    return false;
}

/* A keyref of an element and its qualified node set. */
typedef struct keyref_check {
    const sxml_schema_identity_constraint *keyref;
    uint32_t element;
    const key_table *members;
} keyref_check;

static sxml_status check_keyref(const identity_check *check, keyref_check request) {
    const sxml_schema_components *components = check->validator->components;
    const sxml_schema_identity_constraint_index referenced = request.keyref->referenced_key;
    const sxml_schema_identity_constraint *key =
        sxml_schema_get_identity_constraint(components, referenced);
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node_at(check, request.element), &description);
    key_table keys = empty_table();
    keys.field_count = key->fields.entry_count;
    /* 3.11.5: the tables of the element and of its descendants. */
    for (uint32_t node_index = request.element;
         status == SXML_STATUS_OK && node_index <= description.last_descendant.index;
         node_index += 1) {
        if (declares_constraint(check, node_index, referenced)) {
            status = collect(
                check, (collection){.constraint = key, .element = node_index, .reports = false},
                &keys);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = prepare_slots(check->validator->allocator, &keys);
    }
    for (uint32_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < keys.entries.entry_count; entry_index += 1) {
        insert_entry(&keys, entry_index);
    }
    sxml_schema_scratch_array bindings = {.bytes = NULL,
                                          .entry_byte_count = sizeof(table_binding),
                                          .entry_count = 0,
                                          .entry_capacity = 0};
    for (size_t member_index = 0;
         status == SXML_STATUS_OK && member_index < request.members->entries.entry_count;
         member_index += 1) {
        const key_entry *member =
            sxml_schema_scratch_array_at(&request.members->entries, member_index);
        key_search_outcome found = {.is_found = false};
        status = find_key(check,
                          (keyref_lookup){.keyref_table = request.members,
                                          .member = member,
                                          .key_table = &keys,
                                          .keyref_element = request.element,
                                          .bindings = &bindings},
                          &found);
        if (status == SXML_STATUS_OK && !found.is_found) {
            status = report_error(
                check, (identity_error){.constraint_name = "cvc-identity-constraint.4.3",
                                        .description = "its key sequence is no key sequence of "
                                                       "the referenced key",
                                        .node_index = member->node_index,
                                        .constraint = request.keyref});
        }
    }
    sxml_schema_scratch_array_release(check->validator->allocator, &bindings);
    release_table(check->validator->allocator, &keys);
    return status;
}

/* One identity constraint of the element (3.11.4). */
static sxml_status check_constraint(const identity_check *check,
                                    sxml_schema_identity_constraint_index constraint_index,
                                    uint32_t element) {
    const sxml_schema_identity_constraint *constraint =
        sxml_schema_get_identity_constraint(check->validator->components, constraint_index);
    key_table members = empty_table();
    sxml_status status =
        collect(check, (collection){.constraint = constraint, .element = element, .reports = true},
                &members);
    if (status == SXML_STATUS_OK) {
        if (constraint->category == SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF) {
            status = check_keyref(
                check,
                (keyref_check){.keyref = constraint, .element = element, .members = &members});
        } else {
            status = check_unique(check, &members, constraint);
        }
    }
    release_table(check->validator->allocator, &members);
    return status;
}

sxml_status sxml_schema_check_identity_constraints(sxml_schema_validator *validator,
                                                   const sxml_schema_identity_request *request) {
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        &request->schema->components, request->element_declaration);
    if (declaration->identity_constraints.entry_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_node_reference element;
    sxml_status status = sxml_item_get_node(request->subtree->element_node, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const identity_check check = {
        .validator = validator, .subtree = request->subtree, .tree = element.tree};
    const sxml_schema_index_view constraints =
        sxml_schema_get_list(&request->schema->components, declaration->identity_constraints);
    for (size_t constraint_index = 0;
         status == SXML_STATUS_OK && constraint_index < constraints.index_count;
         constraint_index += 1) {
        status = check_constraint(
            &check,
            (sxml_schema_identity_constraint_index){.value = constraints.indexes[constraint_index]},
            element.node_index.index);
    }
    return status;
}
