/*
 * Schema validation in the languages (D165, D174), over
 * sxml_schema_validate_node and the in-scope schemas of the call's static
 * context (src/schema_scope_internal.h): the internal native row behind
 * XQuery's validate expression (XQuery 4.0, 4.25 "Validate Expressions"),
 * fn:xsd-validator (F&O 4.0, 17.2.5) and the xsd-validation option of
 * fn:parse-xml (17.2.1). All three are F&O 4.0, 17.2.4 "XSD validation"
 * with the in-scope schema definitions as the effective schema; each
 * names its own errors. Golden: src/xquery_flwor.c (an internal native
 * row), src/function_library_csv.c (a function item in a record).
 */
#include "schema_scope_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "sxml/schema.h"
#include "sxml/tree.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <string.h>

/* ------------------------------------------------------- Validation */

/* What 17.2.4 found before validating: the schema and governing type to
   validate against, or why there is none. */
typedef enum scope_plan_kind {
    SXML_SCOPE_PLAN_VALIDATE = 0,
    /* "type xs:untyped": a copy without annotations. */
    SXML_SCOPE_PLAN_STRIP,
    /* Strict validation of a name no in-scope schema declares. */
    SXML_SCOPE_PLAN_NO_DECLARATION,
    /* By type, of a type no in-scope schema defines. */
    SXML_SCOPE_PLAN_NO_TYPE,
} scope_plan_kind;

typedef struct scope_plan {
    scope_plan_kind plan_kind;
    const sxml_schema *schema;
    sxml_type_identifier governing_type;
} scope_plan;

/* One validation: the operand node (a document, element or attribute;
   the caller checks which it accepts), the mode and, by type, the type's
   name. */
typedef struct scope_validation {
    sxml_node_reference node;
    sxml_schema_validate_mode mode;
    sxml_name_identifier type_name;
} scope_validation;

/* The in-scope schemas the call's code was compiled with. */
static sxml_schema_view in_scope_schemas(const sxml_function_call *call) {
    const sxml_ir_static_context *context =
        call->dynamic_context == NULL
            ? NULL
            : sxml_dynamic_context_get_static_context(call->dynamic_context);
    return context == NULL ? (sxml_schema_view){.schemas = NULL, .schema_count = 0}
                           : context->in_scope_schemas;
}

/* True for xs:untyped. */
static bool is_untyped_name(const sxml_function_call *call, sxml_name_identifier name) {
    sxml_name_parts parts;
    return sxml_name_pool_get_name_parts(sxml_item_heap_get_name_pool(call->item_heap), name,
                                         &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA &&
           sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("untyped"));
}

/* The validation root's name: the element's or attribute's, a document's
   first element child's (the caller checked a document has one). */
static sxml_status validation_root_name(sxml_node_reference node, sxml_name_identifier *name) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *name = description.node_name;
    bool has_child = description.node_kind == SXML_NODE_KIND_DOCUMENT && description.has_children;
    sxml_node_reference child = {.tree = node.tree, .node_index = description.first_child};
    while (has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            *name = child_description.node_name;
            return SXML_STATUS_OK;
        }
        has_child = child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    return SXML_STATUS_OK;
}

/* The schema that defines the type (by type), or declares the root's
   name; for lax validation without one, the first schema (every schema
   has the built-in components, XSD 1.1 Part 1, 3.4.7). */
static sxml_status plan_validation(const sxml_function_call *call,
                                   const scope_validation *validation, scope_plan *plan) {
    const sxml_schema_view schemas = in_scope_schemas(call);
    *plan = (scope_plan){.plan_kind = SXML_SCOPE_PLAN_VALIDATE,
                         .schema = NULL,
                         .governing_type = {.index = SXML_TYPE_NONE}};
    if (validation->mode == SXML_SCHEMA_VALIDATE_MODE_TYPE) {
        sxml_schema_scope_type found;
        if (is_untyped_name(call, validation->type_name)) {
            plan->plan_kind = SXML_SCOPE_PLAN_STRIP;
        } else if (sxml_schema_scope_find_type(schemas, validation->type_name, &found)) {
            plan->schema = found.schema;
            plan->governing_type = found.type;
        } else {
            plan->plan_kind = SXML_SCOPE_PLAN_NO_TYPE;
        }
        return SXML_STATUS_OK;
    }
    sxml_name_identifier name = {0};
    sxml_status status = validation_root_name(validation->node, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_description description;
    status = sxml_node_describe(validation->node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind == SXML_NODE_KIND_ATTRIBUTE) {
        sxml_schema_scope_attribute attribute;
        if (sxml_schema_scope_find_attribute(schemas, name, &attribute)) {
            plan->schema = attribute.schema;
            return SXML_STATUS_OK;
        }
    } else {
        sxml_schema_scope_element element;
        if (sxml_schema_scope_find_element(schemas, name, &element)) {
            plan->schema = element.schema;
            return SXML_STATUS_OK;
        }
    }
    if (validation->mode == SXML_SCHEMA_VALIDATE_MODE_LAX && schemas.schema_count > 0) {
        plan->schema = schemas.schemas[0];
        return SXML_STATUS_OK;
    }
    plan->plan_kind = SXML_SCOPE_PLAN_NO_DECLARATION;
    return SXML_STATUS_OK;
}

/* A copy of node in a new arena tree without its annotations: elements
   xs:untyped, attributes xs:untypedAtomic (XDM 4.0, 6.2.4, 6.3.4). */
static sxml_status copy_untyped(const sxml_function_call *call, sxml_node_reference node,
                                sxml_item *copy) {
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_arena_tree_configuration){
            .allocator = sxml_item_heap_as_allocator(call->item_heap),
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = call->item_heap,
            .maximum_node_count = 0,
            .maximum_depth = 0,
            .maximum_text_byte_count = 0,
            .keeps_locations = false,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(
            sxml_arena_tree_as_tree_builder(arena_tree),
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = true,
                                 .inherits_namespaces = true,
                                 .preserves_type_annotations = false});
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *copy = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* Validates against plan: the validated copy, owned, in *validated; an
   invalid node gives SXML_STATUS_DYNAMIC_ERROR with *schema_error filled
   (its code the first constraint broken), every error going to
   report_sink. */
typedef struct scope_run {
    const scope_validation *validation;
    const scope_plan *plan;
    sxml_schema_report_sink report_sink;
    sxml_error_record *schema_error;
} scope_run;

static sxml_status run_validation(const sxml_function_call *call, const scope_run *run,
                                  sxml_item *validated) {
    if (run->plan->plan_kind == SXML_SCOPE_PLAN_STRIP) {
        return copy_untyped(call, run->validation->node, validated);
    }
    memset(run->schema_error, 0, sizeof *run->schema_error);
    const sxml_item node = sxml_node_item_make(run->validation->node);
    const sxml_status status = sxml_schema_validate_node(
        run->plan->schema,
        &(sxml_schema_validation_request){
            .node = node,
            .validation_mode = run->validation->mode == SXML_SCHEMA_VALIDATE_MODE_LAX
                                   ? SXML_VALIDATION_MODE_LAX
                                   : SXML_VALIDATION_MODE_STRICT,
            .governing_type = run->plan->governing_type,
            .report_sink = run->report_sink,
            .error_record = run->schema_error},
        validated);
    sxml_item_release(node);
    return status;
}

/* A schema error raised again with code (sxml_schema_scope_raise). */
static sxml_status raise_schema_error(const sxml_function_call *call,
                                      const sxml_error_record *schema_error,
                                      sxml_byte_view code) {
    return sxml_schema_scope_raise(&(sxml_schema_scope_error){
        .item_heap = call->item_heap,
        .allocator = sxml_item_heap_as_allocator(call->item_heap),
        .error_status = SXML_STATUS_DYNAMIC_ERROR,
        .error_code = code,
        .schema_error = schema_error,
        .error_record = call->error_record,
        .location = {.line_number = 0}});
}

/* The one node of a sequence and its kind; false for anything else. */
typedef struct single_node {
    sxml_node_reference node;
    sxml_node_kind node_kind;
} single_node;

/* The answer of read_single_node. */
typedef struct single_node_answer {
    single_node *single;
    bool *is_single;
} single_node_answer;

static sxml_status read_single_node(const sxml_function_call *call, sxml_sequence sequence,
                                    single_node_answer answer) {
    single_node *single = answer.single;
    bool *is_single = answer.is_single;
    *is_single = false;
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(sequence, call->error_record, &item_count);
    if (status != SXML_STATUS_OK || item_count != 1) {
        return status;
    }
    sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    status = sxml_sequence_get_item(sequence, (sxml_item_request){.item_index = 0}, &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_item_get_node(item, &single->node) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    sxml_node_description description;
    status = sxml_node_describe(single->node, &description);
    if (status == SXML_STATUS_OK) {
        single->node_kind = description.node_kind;
        *is_single = true;
    }
    return status;
}

/* True when a document node is well-formed (17.2.4): exactly one element
   child and no text children. */
typedef struct document_form {
    bool is_well_formed;
} document_form;

static sxml_status is_well_formed_document(sxml_node_reference node, document_form *form) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(node, &description);
    size_t element_count = 0;
    bool has_text = false;
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_reference child = {.tree = node.tree, .node_index = description.first_child};
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        element_count += child_description.node_kind == SXML_NODE_KIND_ELEMENT ? 1 : 0;
        has_text = has_text || child_description.node_kind == SXML_NODE_KIND_TEXT;
        has_child = child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    form->is_well_formed = element_count == 1 && !has_text;
    return status;
}

/* ------------------------------------------------- The validate row */

static const sxml_static_error validate_operand_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQTY0030"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The operand of a validate expression is not exactly one document or element node "
        "(XQuery 4.0, 4.25)"),
};

static const sxml_static_error validate_document_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0061"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A document node to validate must have exactly one element child and no text "
        "children (XQuery 4.0, 4.25)"),
};

static const sxml_static_error validate_declaration_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0084"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Strict validation of an element whose name has no global element declaration "
        "among the in-scope schema definitions (XQuery 4.0, 4.25)"),
};

static const sxml_static_error validate_type_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0027"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The type of a validate expression is not among the in-scope schema definitions"),
};

/* An optional argument of one atomic item and its datum. */
typedef struct datum_answer {
    sxml_atomic_datum *datum;
    bool *has_datum;
} datum_answer;

static sxml_status read_datum_argument(const sxml_function_call *call, size_t argument_index,
                                       datum_answer answer) {
    sxml_atomic_datum *datum = answer.datum;
    bool *has_datum = answer.has_datum;
    sxml_optional_item argument = {.has_item = false};
    sxml_status status =
        sxml_function_library_read_optional_item(call, argument_index, &argument);
    *has_datum = false;
    if (status == SXML_STATUS_OK && argument.has_item) {
        status = sxml_atomic_item_get_datum(argument.item, datum);
        *has_datum = status == SXML_STATUS_OK;
    }
    return status;
}

/* ------------------------------------------- XSLT's validation rules */

static const sxml_static_error xslt_document_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE1550"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A document node to validate must have exactly one element child and no text "
        "children (XSLT 4.0, 25.2)")};

static const sxml_static_error xslt_declaration_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE1512"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Strict validation of a node whose name has no top-level declaration in the "
        "in-scope schemas (XSLT 4.0, 25.1.1)")};

static const sxml_static_error xslt_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE1540"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The type to validate against is not among the in-scope schema definitions (XSLT "
        "4.0, 25.1.2)")};

static const sxml_static_error xslt_attribute_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XTTE1535"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An attribute is to be validated against a complex type (XSLT 4.0, 25.1.2)")};

/* The code of an invalid node (25.1.1, 25.1.2, 25.2). */
typedef struct invalid_node {
    sxml_schema_validate_mode mode;
    sxml_node_kind node_kind;
    const sxml_error_record *schema_error;
} invalid_node;

static sxml_byte_view xslt_invalid_code(invalid_node invalid) {
    const sxml_byte_view constraint = invalid.schema_error->code.local_name;
    static const sxml_byte_view id_prefix = SXML_BYTE_VIEW_INITIALIZER("cvc-id");
    if (invalid.node_kind == SXML_NODE_KIND_DOCUMENT &&
        constraint.byte_count >= id_prefix.byte_count &&
        memcmp(constraint.bytes, id_prefix.bytes, id_prefix.byte_count) == 0) {
        /* 25.2: Validation Root Valid (ID/IDREF) of a document. */
        return SXML_BYTE_VIEW_FROM_LITERAL("XTTE1555");
    }
    if (invalid.mode == SXML_SCHEMA_VALIDATE_MODE_LAX) {
        return SXML_BYTE_VIEW_FROM_LITERAL("XTTE1515");
    }
    return invalid.mode == SXML_SCHEMA_VALIDATE_MODE_TYPE ? SXML_BYTE_VIEW_FROM_LITERAL("XTTE1540")
                                                          : SXML_BYTE_VIEW_FROM_LITERAL("XTTE1510");
}

/* True when type is a complex type. */
static bool is_complex_type(const sxml_function_call *call, sxml_type_identifier type) {
    sxml_type_description description;
    return sxml_type_registry_describe_type(sxml_item_heap_get_type_registry(call->item_heap),
                                            type, &description) == SXML_STATUS_OK &&
           description.variety == SXML_TYPE_VARIETY_COMPLEX;
}

/* One item of $node under XSLT's rules: *validated, owned, is its
   validated copy, or the item itself for anything but a document,
   element or attribute. */
typedef struct xslt_item {
    sxml_item item;
    sxml_schema_validate_mode mode;
    sxml_name_identifier type_name;
} xslt_item;

static sxml_status validate_xslt_item(const sxml_function_call *call, const xslt_item *request,
                                      sxml_item *validated) {
    sxml_node_reference node = {.tree = NULL};
    sxml_node_description description;
    memset(&description, 0, sizeof description);
    if (sxml_item_get_node(request->item, &node) != SXML_STATUS_OK ||
        sxml_node_describe(node, &description) != SXML_STATUS_OK ||
        (description.node_kind != SXML_NODE_KIND_DOCUMENT &&
         description.node_kind != SXML_NODE_KIND_ELEMENT &&
         description.node_kind != SXML_NODE_KIND_ATTRIBUTE)) {
        sxml_item_retain(request->item);
        *validated = request->item;
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (description.node_kind == SXML_NODE_KIND_DOCUMENT) {
        document_form form = {.is_well_formed = true};
        status = is_well_formed_document(node, &form);
        if (status == SXML_STATUS_OK && !form.is_well_formed) {
            return sxml_function_library_report(call, &xslt_document_error);
        }
    }
    const scope_validation validation = {
        .node = node, .mode = request->mode, .type_name = request->type_name};
    scope_plan plan;
    if (status == SXML_STATUS_OK) {
        status = plan_validation(call, &validation, &plan);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_DECLARATION) {
        return sxml_function_library_report(call, &xslt_declaration_error);
    }
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_TYPE) {
        return sxml_function_library_report(call, &xslt_type_error);
    }
    if (description.node_kind == SXML_NODE_KIND_ATTRIBUTE &&
        plan.plan_kind == SXML_SCOPE_PLAN_VALIDATE && request->mode == SXML_SCHEMA_VALIDATE_MODE_TYPE &&
        is_complex_type(call, plan.governing_type)) {
        return sxml_function_library_report(call, &xslt_attribute_type_error);
    }
    sxml_error_record schema_error;
    memset(&schema_error, 0, sizeof schema_error);
    status = run_validation(call,
                            &(scope_run){.validation = &validation,
                                         .plan = &plan,
                                         .report_sink = {.state = NULL, .operations = NULL},
                                         .schema_error = &schema_error},
                            validated);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        /* XSLT 4.0, 25: these are type errors. */
        return sxml_schema_scope_raise(&(sxml_schema_scope_error){
            .item_heap = call->item_heap,
            .allocator = sxml_item_heap_as_allocator(call->item_heap),
            .error_status = SXML_STATUS_TYPE_ERROR,
            .error_code = xslt_invalid_code((invalid_node){.mode = request->mode,
                                                           .node_kind = description.node_kind,
                                                           .schema_error = &schema_error}),
            .schema_error = &schema_error,
            .error_record = call->error_record,
            .location = {.line_number = 0}});
    }
    return status;
}

/* Every item of $node under XSLT's rules, into *result. */
static sxml_status validate_xslt_items(const sxml_function_call *call, xslt_item request,
                                       sxml_sequence *result) {
    const sxml_sequence items = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(items, call->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count > SIZE_MAX / sizeof(sxml_item)) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = item_count == 0 ? 1 : (size_t)item_count *
                                                                           sizeof(sxml_item),
                                  .alignment = _Alignof(sxml_item)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *validated = memory;
    size_t validated_count = 0;
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        status = sxml_sequence_get_item(items, (sxml_item_request){.item_index = item_index},
                                        &request.item);
        if (status == SXML_STATUS_OK) {
            status = validate_xslt_item(call, &request, &validated[validated_count]);
        }
        if (status == SXML_STATUS_OK) {
            validated_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            call->item_heap, (sxml_item_view){.items = validated, .item_count = validated_count},
            result);
    }
    for (size_t item_index = 0; item_index < validated_count; item_index += 1) {
        sxml_item_release(validated[item_index]);
    }
    sxml_allocator_release(allocator, memory);
    return status;
}

/* validate-items($node, $mode, $type), the mode XSLT_RULES plus a
   validation mode: every item of $node under XSLT's rules
   (src/schema_scope_internal.h). A row of its own, so that what links
   only XQuery's validate does not carry XSLT's rules. */
static sxml_status call_validate_items(const sxml_function_call *call, sxml_sequence *result) {
    sxml_atomic_datum mode_datum;
    sxml_atomic_datum type_datum;
    bool has_mode = false;
    bool has_type = false;
    sxml_status status =
        read_datum_argument(call, 1, (datum_answer){.datum = &mode_datum, .has_datum = &has_mode});
    if (status == SXML_STATUS_OK) {
        status =
            read_datum_argument(call, 2, (datum_answer){.datum = &type_datum, .has_datum = &has_type});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!has_mode || mode_datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
        mode_datum.datum_details.integer < SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES ||
        mode_datum.datum_details.integer >
            SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES + SXML_SCHEMA_VALIDATE_MODE_TYPE ||
        (has_type && type_datum.datum_kind != SXML_DATUM_KIND_QNAME)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return validate_xslt_items(
        call,
        (xslt_item){.item = {.representation = SXML_ITEM_REPRESENTATION_NONE},
                    .mode = (sxml_schema_validate_mode)(mode_datum.datum_details.integer -
                                                        SXML_SCHEMA_VALIDATE_MODE_XSLT_RULES),
                    .type_name = has_type ? type_datum.datum_details.qname.expanded_name
                                          : (sxml_name_identifier){.index = 0}},
        result);
}

/* validate($node, $mode, $type) (src/schema_scope_internal.h). */
static sxml_status call_validate(const sxml_function_call *call, sxml_sequence *result) {
    single_node single = {.node = {.tree = NULL}};
    bool is_single = false;
    sxml_status status =
        read_single_node(call, sxml_function_library_get_argument(call, 0),
                         (single_node_answer){.single = &single, .is_single = &is_single});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_single ||
        (single.node_kind != SXML_NODE_KIND_DOCUMENT && single.node_kind != SXML_NODE_KIND_ELEMENT)) {
        return sxml_function_library_report(call, &validate_operand_error);
    }
    document_form form = {.is_well_formed = true};
    if (single.node_kind == SXML_NODE_KIND_DOCUMENT) {
        status = is_well_formed_document(single.node, &form);
    }
    const bool is_well_formed = form.is_well_formed;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_well_formed) {
        return sxml_function_library_report(call, &validate_document_error);
    }
    sxml_atomic_datum mode_datum;
    sxml_atomic_datum type_datum;
    bool has_mode = false;
    bool has_type = false;
    status = read_datum_argument(call, 1, (datum_answer){.datum = &mode_datum, .has_datum = &has_mode});
    if (status == SXML_STATUS_OK) {
        status = read_datum_argument(call, 2, (datum_answer){.datum = &type_datum, .has_datum = &has_type});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!has_mode || mode_datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
        mode_datum.datum_details.integer < SXML_SCHEMA_VALIDATE_MODE_STRICT ||
        mode_datum.datum_details.integer > SXML_SCHEMA_VALIDATE_MODE_TYPE ||
        (has_type && type_datum.datum_kind != SXML_DATUM_KIND_QNAME)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const scope_validation validation = {
        .node = single.node,
        .mode = (sxml_schema_validate_mode)mode_datum.datum_details.integer,
        .type_name = has_type ? type_datum.datum_details.qname.expanded_name
                              : (sxml_name_identifier){.index = 0}};
    scope_plan plan;
    status = plan_validation(call, &validation, &plan);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_DECLARATION) {
        return sxml_function_library_report(call, &validate_declaration_error);
    }
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_TYPE) {
        return sxml_function_library_report(call, &validate_type_error);
    }
    sxml_error_record schema_error;
    sxml_item validated = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    status = run_validation(call,
                            &(scope_run){.validation = &validation,
                                         .plan = &plan,
                                         .report_sink = {.state = NULL, .operations = NULL},
                                         .schema_error = &schema_error},
                            &validated);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        /* XQuery 4.0, 4.25: invalid is XQDY0027, the constraint named. */
        return raise_schema_error(call, &schema_error, SXML_BYTE_VIEW_FROM_LITERAL("XQDY0027"));
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(validated);
    }
    return status;
}

static const sxml_function_parameter validate_parameters[] = {
    SXML_REQUIRED_PARAMETER("node", "item()*"),
    SXML_REQUIRED_PARAMETER("mode", "xs:integer"),
    SXML_REQUIRED_PARAMETER("type", "xs:QName?"),
};

const sxml_native_function sxml_schema_validate_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* The in-scope schemas of the call's static context. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("validate"),
    .parameters = {validate_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
    .call = call_validate,
    .resumable_body = NULL};

const sxml_native_function sxml_schema_validate_items_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("validate-items"),
    .parameters = {validate_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_validate_items,
    .resumable_body = NULL};

/* ----------------------------------------------- The xsd-validation option */

static const sxml_static_error document_option_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0008"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The xsd-validation option is not strict, lax, skip or \"type\" and the EQName of a "
        "type in the static context (F&O 4.0, 17.2.1)"),
};

static const sxml_static_error document_declaration_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0014"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Strict validation of a document whose element has no declaration in the static "
        "context (F&O 4.0, 17.2.1)"),
};

/* The text after "type" as an EQName Q{uri}local; false otherwise. */
typedef struct eqname_answer {
    sxml_name_identifier *name;
    bool *is_eqname;
} eqname_answer;

static sxml_status read_type_eqname(const sxml_function_call *call, sxml_byte_view text,
                                    eqname_answer answer) {
    sxml_name_identifier *name = answer.name;
    bool *is_eqname = answer.is_eqname;
    *is_eqname = false;
    size_t start = 0;
    while (start < text.byte_count && text.bytes[start] == ' ') {
        start += 1;
    }
    if (text.byte_count < start + 3 || text.bytes[start] != 'Q' || text.bytes[start + 1] != '{') {
        return SXML_STATUS_OK;
    }
    size_t close = start + 2;
    while (close < text.byte_count && text.bytes[close] != '}') {
        close += 1;
    }
    if (close + 1 >= text.byte_count) {
        return SXML_STATUS_OK;
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_namespace_uri_identifier namespace_uri = {0};
    const sxml_byte_view uri = {.bytes = text.bytes + start + 2, .byte_count = close - start - 2};
    if (!sxml_name_pool_find_namespace_uri(name_pool, uri, &namespace_uri)) {
        return SXML_STATUS_OK;
    }
    *is_eqname = sxml_name_pool_find_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri,
                             .local_name = {.bytes = text.bytes + close + 1,
                                            .byte_count = text.byte_count - close - 1}},
        name);
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_scope_validate_document(const sxml_function_call *call,
                                                sxml_byte_view xsd_validation,
                                                sxml_item *document_node) {
    scope_validation validation = {.mode = SXML_SCHEMA_VALIDATE_MODE_STRICT,
                                   .type_name = {.index = 0}};
    const sxml_byte_view type_keyword = SXML_BYTE_VIEW_FROM_LITERAL("type ");
    sxml_status status = SXML_STATUS_OK;
    if (sxml_byte_view_equals(xsd_validation, SXML_BYTE_VIEW_FROM_LITERAL("skip"))) {
        return SXML_STATUS_OK;
    }
    if (sxml_byte_view_equals(xsd_validation, SXML_BYTE_VIEW_FROM_LITERAL("lax"))) {
        validation.mode = SXML_SCHEMA_VALIDATE_MODE_LAX;
    } else if (xsd_validation.byte_count > type_keyword.byte_count &&
               memcmp(xsd_validation.bytes, type_keyword.bytes, type_keyword.byte_count) == 0) {
        validation.mode = SXML_SCHEMA_VALIDATE_MODE_TYPE;
        bool is_eqname = false;
        status = read_type_eqname(
            call,
            (sxml_byte_view){.bytes = xsd_validation.bytes + type_keyword.byte_count,
                             .byte_count = xsd_validation.byte_count - type_keyword.byte_count},
            (eqname_answer){.name = &validation.type_name, .is_eqname = &is_eqname});
        if (status == SXML_STATUS_OK && !is_eqname) {
            status = sxml_function_library_report(call, &document_option_error);
        }
    } else if (!sxml_byte_view_equals(xsd_validation, SXML_BYTE_VIEW_FROM_LITERAL("strict"))) {
        status = sxml_function_library_report(call, &document_option_error);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(*document_node, &validation.node);
    }
    scope_plan plan;
    if (status == SXML_STATUS_OK) {
        status = plan_validation(call, &validation, &plan);
    }
    if (status == SXML_STATUS_OK && plan.plan_kind == SXML_SCOPE_PLAN_NO_TYPE) {
        status = sxml_function_library_report(call, &document_option_error);
    }
    if (status == SXML_STATUS_OK && plan.plan_kind == SXML_SCOPE_PLAN_NO_DECLARATION) {
        status = sxml_function_library_report(call, &document_declaration_error);
    }
    sxml_error_record schema_error;
    sxml_item validated = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK) {
        status = run_validation(call,
                                &(scope_run){.validation = &validation,
                                             .plan = &plan,
                                             .report_sink = {.state = NULL, .operations = NULL},
                                             .schema_error = &schema_error},
                                &validated);
        if (status == SXML_STATUS_DYNAMIC_ERROR) {
            /* F&O 4.0, 17.2.1: invalid content is FODC0014. */
            status =
                raise_schema_error(call, &schema_error, SXML_BYTE_VIEW_FROM_LITERAL("FODC0014"));
        }
    }
    sxml_item_release(*document_node);
    if (status == SXML_STATUS_OK) {
        *document_node = validated;
    }
    return status;
}

/* -------------------------------------------------- fn:xsd-validator */

/* The captured values of a validator V: the mode (an xs:integer), the
   type name (an xs:QName, or empty), return-typed-node and
   return-error-details (xs:boolean). */
enum {
    SXML_VALIDATOR_CAPTURE_MODE = 0,
    SXML_VALIDATOR_CAPTURE_TYPE,
    SXML_VALIDATOR_CAPTURE_TYPED_NODE,
    SXML_VALIDATOR_CAPTURE_ERROR_DETAILS,
    SXML_VALIDATOR_CAPTURE_COUNT,
};

/* The options of fn:xsd-validator (17.2.5). */
enum {
    SXML_VALIDATOR_OPTION_TRUST_EXTERNAL = 0,
    SXML_VALIDATOR_OPTION_USE_IMPORTED_SCHEMA,
    SXML_VALIDATOR_OPTION_SCHEMA,
    SXML_VALIDATOR_OPTION_TARGET_NAMESPACE,
    SXML_VALIDATOR_OPTION_SCHEMA_LOCATION,
    SXML_VALIDATOR_OPTION_USE_XSI_SCHEMA_LOCATION,
    SXML_VALIDATOR_OPTION_XSD_VERSION,
    SXML_VALIDATOR_OPTION_VALIDATION_MODE,
    SXML_VALIDATOR_OPTION_TYPE,
    SXML_VALIDATOR_OPTION_RETURN_TYPED_NODE,
    SXML_VALIDATOR_OPTION_RETURN_ERROR_DETAILS,
    SXML_VALIDATOR_OPTION_COUNT,
};

static const sxml_option_definition validator_options[SXML_VALIDATOR_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("trust-external", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("use-imported-schema", SXML_TYPE_XS_BOOLEAN,
                         SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("schema", SXML_TYPE_ELEMENT_NODE, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("target-namespace", SXML_TYPE_XS_ANY_URI, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("schema-location", SXML_TYPE_XS_ANY_URI, SXML_OCCURRENCE_ZERO_OR_MORE),
    SXML_FUNCTION_OPTION("use-xsi-schema-location", SXML_TYPE_XS_BOOLEAN,
                         SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("xsd-version", SXML_TYPE_XS_DECIMAL, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("validation-mode", SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("type", SXML_TYPE_XS_QNAME, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("return-typed-node", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("return-error-details", SXML_TYPE_XS_BOOLEAN,
                         SXML_OCCURRENCE_EXACTLY_ONE),
};

static const sxml_static_error validator_schema_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0009"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "sxml's fn:xsd-validator validates with the in-scope schema definitions only: the "
        "schema, schema-location, target-namespace and use-xsi-schema-location options "
        "need schema documents read while the query runs"),
};

static const sxml_static_error validator_mode_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The validation-mode option is not strict, lax or by-type (F&O 4.0, 17.2.5)"),
};

static const sxml_static_error validator_type_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0015"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The type option names no type of the schema (F&O 4.0, 17.2.5)"),
};

/* The signature of V: fn((document-node(*) | element() | attribute())?)
   as fn:validation-result-record? (17.2.5). */
static sxml_status validator_signature(const sxml_function_call *call,
                                       sxml_type_identifier *signature) {
    sxml_type_registry *registry = sxml_item_heap_get_type_registry(call->item_heap);
    sxml_type_identifier members[3] = {{.index = SXML_TYPE_NONE},
                                       sxml_builtin_type_identifier(SXML_TYPE_ELEMENT_NODE),
                                       sxml_builtin_type_identifier(SXML_TYPE_ATTRIBUTE_NODE)};
    sxml_status status = sxml_type_registry_register_item_type(
        registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_NODE_TEST,
            .variety_details = {.node_test = {.node_kind = SXML_NODE_KIND_DOCUMENT,
                                              .name_test = {.name_test_kind =
                                                                SXML_NAME_TEST_KIND_ANY_NAME},
                                              .document_element_test =
                                                  sxml_builtin_type_identifier(
                                                      SXML_TYPE_ELEMENT_NODE)}}},
        &members[0]);
    sxml_type_identifier target = {.index = SXML_TYPE_NONE};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_register_item_type(
            registry,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_CHOICE,
                .variety_details = {.member_types = {.type_identifiers = members,
                                                     .type_identifier_count = 3}}},
            &target);
    }
    sxml_type_identifier record = {.index = SXML_TYPE_NONE};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_find_record_type(
            call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("validation-result-record"), &record);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence_type parameter = {.item_type = target,
                                          .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    return sxml_type_registry_register_item_type(
        registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = {.sequence_types = &parameter,
                                                                      .sequence_type_count = 1},
                                                  .result_type = {.item_type = record,
                                                                  .occurrence =
                                                                      SXML_OCCURRENCE_ZERO_OR_ONE}}}},
        signature);
}

/* The error-details maps of V's result: one per report, with the keys
   message (the description) and rule (the constraint name; the form
   17.2.5 suggests is implementation-defined). */
typedef struct error_details {
    const sxml_function_call *call;
    sxml_item *maps;
    size_t map_count;
    size_t map_capacity;
    sxml_status failure;
} error_details;

static sxml_status make_string(sxml_item_heap *item_heap, sxml_byte_view text, sxml_item *item) {
    sxml_sequence sequence = sxml_sequence_empty();
    const sxml_status status = sxml_function_library_make_string(item_heap, text, &sequence);
    if (status == SXML_STATUS_OK) {
        *item = sequence.content;
    }
    return status;
}

static sxml_status receive_report(void *state, const sxml_schema_report *report) {
    error_details *details = state;
    if (details->map_count == details->map_capacity) {
        const size_t capacity = details->map_capacity == 0 ? 8 : details->map_capacity * 2;
        void *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            sxml_item_heap_as_allocator(details->call->item_heap),
            (sxml_allocation_request){.byte_count = capacity * sizeof(sxml_item),
                                      .alignment = _Alignof(sxml_item)},
            &memory);
        if (status != SXML_STATUS_OK) {
            details->failure = status;
            return status;
        }
        if (details->map_count > 0) {
            memcpy(memory, details->maps, details->map_count * sizeof(sxml_item));
        }
        sxml_allocator_release(sxml_item_heap_as_allocator(details->call->item_heap),
                               details->maps);
        details->maps = memory;
        details->map_capacity = capacity;
    }
    sxml_item_heap *item_heap = details->call->item_heap;
    sxml_item keys[2] = {{.representation = SXML_ITEM_REPRESENTATION_NONE},
                         {.representation = SXML_ITEM_REPRESENTATION_NONE}};
    sxml_item values[2] = {{.representation = SXML_ITEM_REPRESENTATION_NONE},
                           {.representation = SXML_ITEM_REPRESENTATION_NONE}};
    sxml_status status = make_string(item_heap, SXML_BYTE_VIEW_FROM_LITERAL("message"), &keys[0]);
    if (status == SXML_STATUS_OK) {
        status = make_string(item_heap, report->description, &values[0]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_string(item_heap, SXML_BYTE_VIEW_FROM_LITERAL("rule"), &keys[1]);
    }
    if (status == SXML_STATUS_OK) {
        status = make_string(item_heap, report->code.local_name, &values[1]);
    }
    sxml_item map = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK) {
        const sxml_map_entry entries[2] = {
            {.key = keys[0], .value = sxml_sequence_from_item(values[0])},
            {.key = keys[1], .value = sxml_sequence_from_item(values[1])}};
        status = sxml_map_make(
            item_heap,
            &(sxml_map_request){
                .map_entries = {.map_entries = entries, .map_entry_count = 2},
                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                .error_record = NULL},
            &map);
    }
    for (size_t entry_index = 0; entry_index < 2; entry_index += 1) {
        sxml_item_release(keys[entry_index]);
        sxml_item_release(values[entry_index]);
    }
    if (status != SXML_STATUS_OK) {
        details->failure = status;
        return status;
    }
    details->maps[details->map_count] = map;
    details->map_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_schema_report_sink_operations error_detail_operations = {
    .receive = receive_report};

static void release_error_details(error_details *details) {
    for (size_t map_index = 0; map_index < details->map_count; map_index += 1) {
        sxml_item_release(details->maps[map_index]);
    }
    sxml_allocator_release(sxml_item_heap_as_allocator(details->call->item_heap), details->maps);
    details->maps = NULL;
    details->map_count = 0;
    details->map_capacity = 0;
}

/* The captured boolean of V. */
static bool captured_is_true(const sxml_function_item_description *description,
                             size_t capture_index) {
    sxml_atomic_datum datum;
    return sxml_atomic_item_get_datum(description->captured_values.sequences[capture_index].content,
                                      &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && datum.datum_details.boolean;
}

/* What one call of V found. */
typedef struct validator_answer {
    bool is_valid;
    sxml_item typed_node;
    bool has_typed_node;
    error_details *details;
} validator_answer;

/* The fn:validation-result-record of answer, owned, in *result. */
static sxml_status make_validator_result(const sxml_function_call *call,
                                         const validator_answer *answer, sxml_sequence *result) {
    sxml_sequence error_maps = sxml_sequence_empty();
    sxml_status status = SXML_STATUS_OK;
    if (answer->details->map_count > 0) {
        status = sxml_sequence_make_vector(
            call->item_heap,
            (sxml_item_view){.items = answer->details->maps,
                             .item_count = answer->details->map_count},
            &error_maps);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence field_values[3] = {
        sxml_function_library_make_boolean((sxml_boolean_result){.is_true = answer->is_valid}),
        answer->has_typed_node ? sxml_sequence_from_item(answer->typed_node)
                               : sxml_sequence_empty(),
        error_maps};
    status = sxml_function_library_make_record(
        call,
        &(sxml_builtin_record_request){
            .local_name = SXML_BYTE_VIEW_FROM_LITERAL("validation-result-record"),
            .field_values = {.sequences = field_values, .sequence_count = 3}},
        result);
    sxml_sequence_release(error_maps);
    return status;
}

/* The validation V's captures describe, of target. */
static sxml_status read_validator_validation(const sxml_function_item_description *description,
                                             const single_node *target,
                                             scope_validation *validation) {
    sxml_atomic_datum mode;
    if (sxml_atomic_item_get_datum(
            description->captured_values.sequences[SXML_VALIDATOR_CAPTURE_MODE].content, &mode) !=
            SXML_STATUS_OK ||
        mode.datum_kind != SXML_DATUM_KIND_INTEGER) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *validation = (scope_validation){.node = target->node,
                                     .mode = (sxml_schema_validate_mode)mode.datum_details.integer,
                                     .type_name = {.index = 0}};
    const sxml_sequence type = description->captured_values.sequences[SXML_VALIDATOR_CAPTURE_TYPE];
    sxml_atomic_datum type_name;
    if (sxml_sequence_get_representation(type) != SXML_SEQUENCE_REPRESENTATION_EMPTY &&
        sxml_atomic_item_get_datum(type.content, &type_name) == SXML_STATUS_OK &&
        type_name.datum_kind == SXML_DATUM_KIND_QNAME) {
        validation->type_name = type_name.datum_details.qname.expanded_name;
    }
    return SXML_STATUS_OK;
}

/* V($target) (17.2.5): the empty sequence for an empty $target, else a
   validation-result-record. */
static sxml_status call_validator(const sxml_function_call *call, sxml_sequence *result) {
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status == SXML_STATUS_OK &&
        description.captured_values.sequence_count != SXML_VALIDATOR_CAPTURE_COUNT) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    single_node target = {.node = {.tree = NULL}};
    bool is_single = false;
    if (status == SXML_STATUS_OK) {
        status = read_single_node(
            call, sxml_function_library_get_argument(call, 0),
            (single_node_answer){.single = &target, .is_single = &is_single});
    }
    if (status != SXML_STATUS_OK || !is_single) {
        /* The signature lets only the empty sequence through here. */
        *result = sxml_sequence_empty();
        return status;
    }
    scope_validation validation;
    status = read_validator_validation(&description, &target, &validation);
    document_form form = {.is_well_formed = true};
    if (status == SXML_STATUS_OK && target.node_kind == SXML_NODE_KIND_DOCUMENT) {
        status = is_well_formed_document(target.node, &form);
    }
    const bool is_well_formed = form.is_well_formed;
    scope_plan plan = {.plan_kind = SXML_SCOPE_PLAN_NO_DECLARATION};
    if (status == SXML_STATUS_OK && is_well_formed) {
        status = plan_validation(call, &validation, &plan);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_TYPE) {
        return sxml_function_library_report(call, &validator_type_error);
    }
    const sxml_schema_view schemas = in_scope_schemas(call);
    if (plan.plan_kind == SXML_SCOPE_PLAN_NO_DECLARATION && schemas.schema_count > 0) {
        /* No declaration of the root's name: the validator says why it
           is invalid (cvc-elt.1), or assesses it by its xsi:type. */
        plan = (scope_plan){.plan_kind = SXML_SCOPE_PLAN_VALIDATE,
                            .schema = schemas.schemas[0],
                            .governing_type = {.index = SXML_TYPE_NONE}};
    }
    const bool wants_details = captured_is_true(&description, SXML_VALIDATOR_CAPTURE_ERROR_DETAILS);
    error_details details = {.call = call, .maps = NULL, .failure = SXML_STATUS_OK};
    validator_answer answer = {.is_valid = false, .has_typed_node = false, .details = &details};
    if (is_well_formed && plan.plan_kind != SXML_SCOPE_PLAN_NO_DECLARATION) {
        sxml_error_record schema_error;
        sxml_item validated = {.representation = SXML_ITEM_REPRESENTATION_NONE};
        status = run_validation(
            call,
            &(scope_run){.validation = &validation,
                         .plan = &plan,
                         .report_sink = {.state = wants_details ? &details : NULL,
                                         .operations = wants_details ? &error_detail_operations
                                                                     : NULL},
                         .schema_error = &schema_error},
            &validated);
        if (status == SXML_STATUS_OK) {
            answer.is_valid = true;
            answer.has_typed_node = captured_is_true(&description, SXML_VALIDATOR_CAPTURE_TYPED_NODE);
            answer.typed_node = validated;
            if (!answer.has_typed_node) {
                sxml_item_release(validated);
            }
        } else if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_STATIC_ERROR) {
            /* Invalid: a result, not an error (17.2.5). */
            sxml_item_heap_clear_raised_error(call->item_heap);
            status = details.failure;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = make_validator_result(call, &answer, result);
    }
    if (answer.has_typed_node) {
        sxml_item_release(answer.typed_node);
    }
    release_error_details(&details);
    return status;
}

static const sxml_function_parameter validator_parameters[] = {
    SXML_REQUIRED_PARAMETER("target", "(document-node(*) | element() | attribute())?"),
};

/* The body of V: not a function of the library, so not in the part's
   rows. */
static const sxml_native_function validator_row = {
    .function_namespace = SXML_NAMESPACE_FUNCTIONS,
    /* The in-scope schemas of the call's static context. */
    .properties = SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("validator"),
    .parameters = {validator_parameters, 1},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("fn:validation-result-record?"),
    .call = call_validator,
    .resumable_body = NULL};

/* The mode the options give (17.2.5: by-type is the default when the
   type option is present, strict otherwise). */
static sxml_status read_validator_mode(const sxml_function_call *call,
                                       const sxml_option_values *values,
                                       sxml_schema_validate_mode *mode) {
    *mode = values->is_present[SXML_VALIDATOR_OPTION_TYPE] &&
                    sxml_sequence_get_representation(values->values[SXML_VALIDATOR_OPTION_TYPE]) !=
                        SXML_SEQUENCE_REPRESENTATION_EMPTY
                ? SXML_SCHEMA_VALIDATE_MODE_TYPE
                : SXML_SCHEMA_VALIDATE_MODE_STRICT;
    if (!values->is_present[SXML_VALIDATOR_OPTION_VALIDATION_MODE]) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text =
        sxml_function_library_option_string(
            (sxml_option_lookup){.values = values,
                                 .option_index = SXML_VALIDATOR_OPTION_VALIDATION_MODE})
            .utf8;
    if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("strict"))) {
        *mode = SXML_SCHEMA_VALIDATE_MODE_STRICT;
    } else if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("lax"))) {
        *mode = SXML_SCHEMA_VALIDATE_MODE_LAX;
    } else if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("by-type"))) {
        *mode = SXML_SCHEMA_VALIDATE_MODE_TYPE;
    } else {
        return sxml_function_library_report(call, &validator_mode_error);
    }
    return SXML_STATUS_OK;
}

/* True when an option asks for schema documents beyond the in-scope
   schemas: schema, schema-location, target-namespace not empty, or
   use-xsi-schema-location true. */
static bool needs_other_schemas(const sxml_option_values *values) {
    const size_t sequence_options[3] = {SXML_VALIDATOR_OPTION_SCHEMA, SXML_VALIDATOR_OPTION_SCHEMA_LOCATION,
                                        SXML_VALIDATOR_OPTION_TARGET_NAMESPACE};
    for (size_t option_index = 0; option_index < 3; option_index += 1) {
        const size_t option = sequence_options[option_index];
        if (values->is_present[option] &&
            sxml_sequence_get_representation(values->values[option]) !=
                SXML_SEQUENCE_REPRESENTATION_EMPTY) {
            return true;
        }
    }
    return sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values,
                             .option_index = SXML_VALIDATOR_OPTION_USE_XSI_SCHEMA_LOCATION});
}

/* fn:xsd-validator($options as map(*)? := {}) as fn((document-node(*) |
   element() | attribute())?) as fn:validation-result-record? (F&O 4.0,
   17.2.5): V validates with the in-scope schema definitions of its
   caller's static context (D174), as the options say. */
static sxml_status call_xsd_validator(const sxml_function_call *call, sxml_sequence *result) {
    sxml_option_values values = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 0,
                               .definitions = {validator_options, SXML_VALIDATOR_OPTION_COUNT}},
        &values);
    sxml_schema_validate_mode mode = SXML_SCHEMA_VALIDATE_MODE_STRICT;
    if (status == SXML_STATUS_OK && needs_other_schemas(&values)) {
        status = sxml_function_library_report(call, &validator_schema_error);
    }
    if (status == SXML_STATUS_OK) {
        status = read_validator_mode(call, &values, &mode);
    }
    sxml_type_identifier signature = {.index = SXML_TYPE_NONE};
    if (status == SXML_STATUS_OK) {
        status = validator_signature(call, &signature);
    }
    sxml_item validator = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK) {
        const bool returns_typed_node = !sxml_function_library_option_is_false(
            (sxml_option_lookup){.values = &values,
                                 .option_index = SXML_VALIDATOR_OPTION_RETURN_TYPED_NODE});
        const bool returns_error_details = sxml_function_library_option_is_true(
            (sxml_option_lookup){.values = &values,
                                 .option_index = SXML_VALIDATOR_OPTION_RETURN_ERROR_DETAILS});
        const sxml_sequence captured[SXML_VALIDATOR_CAPTURE_COUNT] = {
            sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)mode)),
            values.is_present[SXML_VALIDATOR_OPTION_TYPE] ? values.values[SXML_VALIDATOR_OPTION_TYPE]
                                                     : sxml_sequence_empty(),
            sxml_function_library_make_boolean(
                (sxml_boolean_result){.is_true = returns_typed_node}),
            sxml_function_library_make_boolean(
                (sxml_boolean_result){.is_true = returns_error_details})};
        status = sxml_function_item_make(
            call->item_heap,
            &(sxml_function_item_request){
                .function_name = {.index = 0},
                .signature = signature,
                .function_annotations = {.function_annotations = NULL,
                                         .function_annotation_count = 0},
                .captured_values = {.sequences = captured,
                                    .sequence_count = SXML_VALIDATOR_CAPTURE_COUNT},
                .body = {.state = (void *)&validator_row,
                         .operations = &sxml_native_function_body_operations}},
            &validator);
    }
    sxml_function_library_release_options(&values);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(validator);
    }
    return status;
}

static const sxml_function_parameter xsd_validator_parameters[] = {
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_native_function schema_rows[] = {
    SXML_FUNCTION_ROW("xsd-validator", xsd_validator_parameters,
                      "fn((document-node(*) | element() | attribute())?) as "
                      "fn:validation-result-record?",
                      SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_xsd_validator),
};

const sxml_native_function_view sxml_function_library_schema = SXML_LIBRARY_PART(schema_rows);
