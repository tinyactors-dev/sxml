/*
 * The type part of the function library (packet 2.17): F&O 4.0, section
 * 19.1 "Functions returning type information": fn:type-of,
 * fn:schema-type, fn:atomic-type-annotation and fn:node-type-annotation.
 * One library part, sxml_function_library_type (D101).
 *
 * Golden file: src/function_library_numeric.c (fn:random-number-generator:
 * a built-in record whose function fields are native rows over a captured
 * value).
 *
 * A fn:schema-type-record (F&O 4.0, section 19.1.1) describes a type of
 * the heap's type registry: the built-in schema types, and those a schema
 * adds later. Its function fields (base-type, primitive-type, members,
 * matches) are anonymous function items over the rows below that capture
 * the type's identifier as an xs:integer; constructor is the type's
 * constructor function, as fn:function-lookup gives it. Calling a field
 * makes the next record on demand, so the structure stays acyclic.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"

#include <string.h>

static const sxml_static_error node_kind_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:node-type-annotation takes an element or an attribute (F&O 4.0, "
        "fn:node-type-annotation)"),
};

static const sxml_static_error no_dynamic_context_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The function needs the dynamic context of an evaluation (XPath 4.0, "
        "section 2.2.2)"),
};

static const sxml_type_registry *registry_of(const sxml_function_call *call) {
    return sxml_item_heap_get_type_registry(call->item_heap);
}

/* ---------------------------------------------------------- fn:type-of */

/* Appends the name of a type: "xs:local" in the XML Schema namespace,
   "Q{uri}local" otherwise (F&O 4.0, fn:type-of). */
static sxml_status append_type_name(const sxml_function_call *call, sxml_library_text *text,
                                    sxml_name_identifier type_name) {
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(name_pool, type_name, &parts);
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("xs:"));
    } else {
        status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        }
    }
    return status == SXML_STATUS_OK ? sxml_function_library_text_append(text, parts.local_name)
                                    : status;
}

/* Appends the type string of one item (F&O 4.0, fn:type-of). */
static sxml_status append_item_type(const sxml_function_call *call, sxml_library_text *text,
                                    sxml_item item) {
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_JSON_NODE:
        return sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("jnode()"));
    case SXML_ITEM_KIND_MAP:
        return sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("map(*)"));
    case SXML_ITEM_KIND_ARRAY:
        return sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("array(*)"));
    case SXML_ITEM_KIND_FUNCTION:
        return sxml_function_library_text_append(text, SXML_BYTE_VIEW_FROM_LITERAL("fn(*)"));
    case SXML_ITEM_KIND_NODE: {
        sxml_node_reference node;
        sxml_status status = sxml_item_get_node(item, &node);
        sxml_node_description description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        static const char *const kind_texts[] = {
            [SXML_NODE_KIND_DOCUMENT] = "document-node()",
            [SXML_NODE_KIND_ELEMENT] = "element()",
            [SXML_NODE_KIND_ATTRIBUTE] = "attribute()",
            [SXML_NODE_KIND_TEXT] = "text()",
            [SXML_NODE_KIND_COMMENT] = "comment()",
            [SXML_NODE_KIND_PROCESSING_INSTRUCTION] = "processing-instruction()",
            [SXML_NODE_KIND_NAMESPACE] = "namespace-node()",
        };
        const size_t kind_index = (size_t)description.node_kind;
        if (kind_index >= sizeof(kind_texts) / sizeof(kind_texts[0]) ||
            kind_texts[kind_index] == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return sxml_function_library_text_append(
            text, sxml_byte_view_from_c_string(kind_texts[kind_index]));
    }
    case SXML_ITEM_KIND_ATOMIC:
        break;
    }
    /* An anonymous type stands for its nearest named base type. */
    sxml_type_identifier type = sxml_item_get_type_annotation(item);
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    while (status == SXML_STATUS_OK && description.type_name.index == 0 &&
           description.base_type.index != SXML_TYPE_NONE) {
        type = description.base_type;
        status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    }
    return status == SXML_STATUS_OK ? append_type_name(call, text, description.type_name) : status;
}

/* fn:type-of($value) (F&O 4.0, section 19.1.3): a SequenceType that
   matches $value: the distinct item type strings in order of first
   appearance, joined with "|" in parentheses when there are several, and
   "+" for more than one item. */
static sxml_status call_type_of(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = sxml_function_library_get_argument(call, 0);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, call->error_record, &item_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, item_count);
    }
    if (status == SXML_STATUS_OK && item_count == 0) {
        return sxml_function_library_make_string(
            call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("empty-sequence()"), result);
    }
    /* "|"-joined distinct types, each also ended by "|" while searching. */
    sxml_library_text types = {.item_heap = call->item_heap};
    sxml_library_text current = {.item_heap = call->item_heap};
    size_t type_count = 0;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_function_library_text_release(&current);
        current = (sxml_library_text){.item_heap = call->item_heap};
        status = sxml_function_library_text_append(&current, SXML_BYTE_VIEW_FROM_LITERAL("|"));
        if (status == SXML_STATUS_OK) {
            status = append_item_type(call, &current, step.item);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_function_library_text_append(&current, SXML_BYTE_VIEW_FROM_LITERAL("|"));
        }
        const sxml_byte_view known = sxml_function_library_text_view(&types);
        const sxml_byte_view candidate = sxml_function_library_text_view(&current);
        bool is_known = false;
        for (size_t byte_index = 0; status == SXML_STATUS_OK && !is_known &&
                                    candidate.byte_count <= known.byte_count &&
                                    byte_index + candidate.byte_count <= known.byte_count;
             byte_index += 1) {
            is_known = (byte_index == 0 || known.bytes[byte_index] == '|') &&
                       memcmp(known.bytes + byte_index, candidate.bytes, candidate.byte_count) == 0;
        }
        if (status == SXML_STATUS_OK && !is_known) {
            /* Keep one "|" between types: drop the candidate's first. */
            status = sxml_function_library_text_append(
                &types, type_count == 0 ? candidate
                                        : (sxml_byte_view){.bytes = candidate.bytes + 1,
                                                           .byte_count = candidate.byte_count - 1});
            type_count += 1;
        }
    }
    sxml_function_library_text_release(&current);
    sxml_library_text text = {.item_heap = call->item_heap};
    const sxml_byte_view joined = sxml_function_library_text_view(&types);
    if (status == SXML_STATUS_OK && type_count > 1) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("("));
    }
    if (status == SXML_STATUS_OK) {
        /* Without the outer "|" marks. */
        status = sxml_function_library_text_append(
            &text, (sxml_byte_view){.bytes = joined.bytes + 1, .byte_count = joined.byte_count - 2});
    }
    if (status == SXML_STATUS_OK && type_count > 1) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL(")"));
    }
    if (status == SXML_STATUS_OK && item_count > 1) {
        status = sxml_function_library_text_append(&text, SXML_BYTE_VIEW_FROM_LITERAL("+"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_string(call->item_heap,
                                                   sxml_function_library_text_view(&text), result);
    }
    sxml_function_library_text_release(&text);
    sxml_function_library_text_release(&types);
    return status;
}

/* ------------------------------------------------- Schema type records */

static sxml_status make_type_record(const sxml_function_call *call, sxml_type_identifier type,
                                    sxml_sequence *result);

/* The type identifier a field function captured. */
static sxml_status read_captured_type(const sxml_function_call *call,
                                      sxml_type_identifier *type) {
    sxml_function_item_description description;
    sxml_status status = sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.captured_values.sequence_count != 1) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(description.captured_values.sequences[0].content, &datum);
    if (status != SXML_STATUS_OK || datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
        datum.datum_details.integer < 0 || datum.datum_details.integer > UINT32_MAX) {
        return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
    }
    *type = (sxml_type_identifier){.index = (uint32_t)datum.datum_details.integer};
    return SXML_STATUS_OK;
}

/* "base-type": the record of the base type, or () for xs:anyType. */
static sxml_status call_base_type(const sxml_function_call *call, sxml_sequence *result) {
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    sxml_status status = read_captured_type(call, &type);
    sxml_type_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.base_type.index == SXML_TYPE_NONE) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return make_type_record(call, description.base_type, result);
}

/* "primitive-type": the record of the primitive type. */
static sxml_status call_primitive_type(const sxml_function_call *call, sxml_sequence *result) {
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    sxml_status status = read_captured_type(call, &type);
    sxml_type_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    }
    return status == SXML_STATUS_OK ? make_type_record(call, description.primitive_type, result)
                                    : status;
}

/* "members": the records of a union's member types, or of a list's item
   type. */
static sxml_status call_members(const sxml_function_call *call, sxml_sequence *result) {
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    sxml_status status = read_captured_type(call, &type);
    sxml_type_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.variety == SXML_TYPE_VARIETY_LIST) {
        return make_type_record(call, description.variety_details.list_item_type, result);
    }
    const sxml_type_identifier_view members = description.variety_details.member_types;
    sxml_sequence_builder *builder = NULL;
    status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    for (size_t member_index = 0;
         status == SXML_STATUS_OK && member_index < members.type_identifier_count;
         member_index += 1) {
        sxml_sequence record = sxml_sequence_empty();
        status = make_type_record(call, members.type_identifiers[member_index], &record);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, record, call->error_record);
        }
        sxml_sequence_release(record);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

/* "matches": whether an atomic item is an instance of the type. */
static sxml_status call_matches(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return sxml_function_library_report(call, &no_dynamic_context_error);
    }
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    sxml_status status = read_captured_type(call, &type);
    sxml_type_match match = {.is_match = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_match_sequence_type(
            call->dynamic_context,
            &(sxml_type_match_request){
                .value = sxml_function_library_get_argument(call, 0),
                .sequence_type = {.item_type = type, .occurrence = SXML_OCCURRENCE_EXACTLY_ONE},
                .error_record = call->error_record},
            &match);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_function_library_make_boolean((sxml_boolean_result){.is_true = match.is_match});
    }
    return status;
}

/* The rows of the function fields; their types come from the record's
   fields, so the parameter types here only name what the bodies read. */
#define SXML_TYPE_FIELD_ROW(name_text, parameter_array, parameter_count, result_text, body) \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                                       \
     .properties = 0,                                                                     \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                 \
     .parameters = {(parameter_array), (parameter_count)},                                \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                              \
     .call = (body),                                                                      \
     .resumable_body = NULL}

static const sxml_function_parameter matches_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:anyAtomicType")};

static const sxml_native_function base_type_row = SXML_TYPE_FIELD_ROW(
    "schema-type-base-type", NULL, 0, "fn:schema-type-record?", call_base_type);
static const sxml_native_function primitive_type_row = SXML_TYPE_FIELD_ROW(
    "schema-type-primitive-type", NULL, 0, "fn:schema-type-record", call_primitive_type);
static const sxml_native_function members_row = SXML_TYPE_FIELD_ROW(
    "schema-type-members", NULL, 0, "fn:schema-type-record*", call_members);
static const sxml_native_function matches_row = SXML_TYPE_FIELD_ROW(
    "schema-type-matches", matches_parameters, 1, "xs:boolean", call_matches);

/* The fields of fn:schema-type-record, in record order (F&O 4.0, 19.1.1). */
enum {
    SXML_SCHEMA_TYPE_FIELD_NAME,
    SXML_SCHEMA_TYPE_FIELD_IS_SIMPLE,
    SXML_SCHEMA_TYPE_FIELD_BASE_TYPE,
    SXML_SCHEMA_TYPE_FIELD_PRIMITIVE_TYPE,
    SXML_SCHEMA_TYPE_FIELD_VARIETY,
    SXML_SCHEMA_TYPE_FIELD_MEMBERS,
    SXML_SCHEMA_TYPE_FIELD_SIMPLE_CONTENT_TYPE,
    SXML_SCHEMA_TYPE_FIELD_MATCHES,
    SXML_SCHEMA_TYPE_FIELD_CONSTRUCTOR,
    SXML_SCHEMA_TYPE_FIELD_COUNT,
};

/* A function field to make: its row (NULL for the constructor body), the
   type it describes, and its signature (the field's item type). */
typedef struct field_function_request {
    const sxml_native_function *row;
    sxml_type_identifier type;
    sxml_type_identifier signature;
} field_function_request;

static sxml_status make_field_function(const sxml_function_call *call,
                                       const field_function_request *request,
                                       sxml_sequence *field) {
    const sxml_sequence captured =
        sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)request->type.index));
    sxml_item item = {.representation = 0};
    const sxml_status status = sxml_function_item_make(
        call->item_heap,
        &(sxml_function_item_request){
            .function_name = {.index = 0},
            .signature = request->signature,
            .function_annotations = {.function_annotations = NULL,
                                     .function_annotation_count = 0},
            .captured_values = {.sequences = &captured, .sequence_count = 1},
            /* The constructor is the type's constructor function, as
               src/evaluator_function.c makes xs:T#1. */
            .body = request->row == NULL
                        ? (sxml_function_body){.state = NULL,
                                               .operations =
                                                   &sxml_evaluator_constructor_body_operations}
                        : (sxml_function_body){.state = (void *)request->row,
                                               .operations = &sxml_native_function_body_operations}},
        &item);
    if (status == SXML_STATUS_OK) {
        *field = sxml_sequence_from_item(item);
    }
    return status;
}

/* True for a type derived from xs:QName or xs:NOTATION (or one of them):
   namespace-sensitive types have no constructor field. */
static bool is_namespace_sensitive(const sxml_function_call *call, sxml_type_identifier type) {
    return sxml_type_registry_is_derived_from(
               registry_of(call),
               (sxml_type_pair){.derived_type = type,
                                .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME)}) ||
           sxml_type_registry_is_derived_from(
               registry_of(call),
               (sxml_type_pair){.derived_type = type,
                                .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_NOTATION)});
}

/* The variety field: atomic, list or union; mixed for the complex types
   sxml knows (xs:anyType, xs:untyped); absent for xs:anySimpleType. */
static sxml_byte_view variety_text(sxml_type_variety variety) {
    switch (variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
        return SXML_BYTE_VIEW_FROM_LITERAL("atomic");
    case SXML_TYPE_VARIETY_LIST:
        return SXML_BYTE_VIEW_FROM_LITERAL("list");
    case SXML_TYPE_VARIETY_UNION:
        return SXML_BYTE_VIEW_FROM_LITERAL("union");
    case SXML_TYPE_VARIETY_COMPLEX:
        return SXML_BYTE_VIEW_FROM_LITERAL("mixed");
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        break;
    }
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* The schema-type-record of type (F&O 4.0, section 19.1.1). */
typedef struct type_record_fields {
    sxml_sequence values[SXML_SCHEMA_TYPE_FIELD_COUNT];
} type_record_fields;

static sxml_status fill_type_record_fields(const sxml_function_call *call,
                                           sxml_type_identifier type, type_record_fields *fields) {
    sxml_type_identifier record_type = {.index = SXML_TYPE_NONE};
    sxml_status status = sxml_function_library_find_record_type(
        call->item_heap, SXML_BYTE_VIEW_FROM_LITERAL("schema-type-record"), &record_type);
    sxml_type_description record;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry_of(call), record_type, &record);
    }
    const sxml_record_field_view record_fields = record.variety_details.record_type.record_fields;
    if (status == SXML_STATUS_OK && (record.variety != SXML_TYPE_VARIETY_RECORD ||
                                     record_fields.record_field_count != SXML_SCHEMA_TYPE_FIELD_COUNT)) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_type_variety variety = description.variety;
    sxml_item item = {.representation = 0};
    /* Built-in type names are shown with the xs prefix (#xs:integer). */
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_name_parts name_parts;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    if (description.type_name.index != 0 &&
        sxml_name_pool_get_name_parts(name_pool, description.type_name, &name_parts) ==
            SXML_STATUS_OK &&
        name_parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        status = sxml_name_pool_intern_namespace_prefix(name_pool, SXML_BYTE_VIEW_FROM_LITERAL("xs"),
                                                        &prefix);
    }
    if (status == SXML_STATUS_OK && description.type_name.index != 0) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = description.type_name,
                                                      .namespace_prefix = prefix}}}},
            &item);
        if (status == SXML_STATUS_OK) {
            fields->values[SXML_SCHEMA_TYPE_FIELD_NAME] = sxml_sequence_from_item(item);
        }
    }
    fields->values[SXML_SCHEMA_TYPE_FIELD_IS_SIMPLE] = sxml_function_library_make_boolean(
        (sxml_boolean_result){.is_true = variety != SXML_TYPE_VARIETY_COMPLEX});
    const sxml_byte_view variety_name = variety_text(variety);
    if (status == SXML_STATUS_OK && variety_name.byte_count > 0) {
        status = sxml_function_library_make_string(call->item_heap, variety_name,
                                                   &fields->values[SXML_SCHEMA_TYPE_FIELD_VARIETY]);
    }
    const bool is_atomic = variety == SXML_TYPE_VARIETY_ATOMIC;
    const bool is_any_atomic = type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE;
    const bool is_simple = is_atomic || variety == SXML_TYPE_VARIETY_LIST ||
                           variety == SXML_TYPE_VARIETY_UNION;
    const struct {
        size_t field_index;
        bool is_present;
        const sxml_native_function *row;
    } functions[] = {
        {SXML_SCHEMA_TYPE_FIELD_BASE_TYPE, true, &base_type_row},
        {SXML_SCHEMA_TYPE_FIELD_PRIMITIVE_TYPE, is_atomic && !is_any_atomic, &primitive_type_row},
        {SXML_SCHEMA_TYPE_FIELD_MEMBERS,
         variety == SXML_TYPE_VARIETY_UNION || variety == SXML_TYPE_VARIETY_LIST, &members_row},
        /* A generalized atomic type: atomic, or a union of atomic types. */
        {SXML_SCHEMA_TYPE_FIELD_MATCHES, is_atomic || variety == SXML_TYPE_VARIETY_UNION,
         &matches_row},
        {SXML_SCHEMA_TYPE_FIELD_CONSTRUCTOR,
         is_simple && !is_any_atomic && type.index != SXML_TYPE_XS_NOTATION &&
             !is_namespace_sensitive(call, type),
         NULL},
    };
    for (size_t function_index = 0;
         status == SXML_STATUS_OK && function_index < sizeof(functions) / sizeof(functions[0]);
         function_index += 1) {
        const size_t field_index = functions[function_index].field_index;
        if (functions[function_index].is_present) {
            status = make_field_function(
                call,
                &(field_function_request){
                    .row = functions[function_index].row,
                    .type = type,
                    .signature = record_fields.record_fields[field_index].field_type.item_type},
                &fields->values[field_index]);
        }
    }
    return status;
}

static sxml_status make_type_record(const sxml_function_call *call, sxml_type_identifier type,
                                    sxml_sequence *result) {
    type_record_fields fields;
    for (size_t field_index = 0; field_index < SXML_SCHEMA_TYPE_FIELD_COUNT; field_index += 1) {
        fields.values[field_index] = sxml_sequence_empty();
    }
    sxml_status status = fill_type_record_fields(call, type, &fields);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_make_record(
            call,
            &(sxml_builtin_record_request){
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("schema-type-record"),
                .field_values = {.sequences = fields.values,
                                 .sequence_count = SXML_SCHEMA_TYPE_FIELD_COUNT}},
            result);
    }
    for (size_t field_index = 0; field_index < SXML_SCHEMA_TYPE_FIELD_COUNT; field_index += 1) {
        sxml_sequence_release(fields.values[field_index]);
    }
    return status;
}

/* --------------------------------------------------------------- Rows */

/* fn:schema-type($name) (F&O 4.0, section 19.1.2): the record of the
   schema type of that name in the registry; () for none (and for names of
   record types, which are not schema types). */
static sxml_status call_schema_type(const sxml_function_call *call, sxml_sequence *result) {
    sxml_optional_item name = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &name);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && name.has_item) {
        status = sxml_atomic_item_get_datum(name.item, &datum);
    }
    *result = sxml_sequence_empty();
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    if (status != SXML_STATUS_OK || !name.has_item || datum.datum_kind != SXML_DATUM_KIND_QNAME ||
        !sxml_type_registry_find_type(registry_of(call), datum.datum_details.qname.expanded_name,
                                      &type)) {
        return status;
    }
    sxml_type_description description;
    status = sxml_type_registry_describe_type(registry_of(call), type, &description);
    if (status != SXML_STATUS_OK || description.variety == SXML_TYPE_VARIETY_RECORD) {
        return status;
    }
    return make_type_record(call, type, result);
}

/* fn:atomic-type-annotation($value) (F&O 4.0, section 19.1.4). */
static sxml_status call_atomic_type_annotation(const sxml_function_call *call,
                                               sxml_sequence *result) {
    sxml_optional_item value = {.has_item = false};
    const sxml_status status = sxml_function_library_read_optional_item(call, 0, &value);
    if (status != SXML_STATUS_OK || !value.has_item) {
        return status != SXML_STATUS_OK ? status : SXML_STATUS_INVALID_ARGUMENT;
    }
    return make_type_record(call, sxml_item_get_type_annotation(value.item), result);
}

/* fn:node-type-annotation($node) (F&O 4.0, section 19.1.5): the record of
   an element's or attribute's type annotation (xs:untyped and
   xs:untypedAtomic for unvalidated nodes). */
static sxml_status call_node_type_annotation(const sxml_function_call *call,
                                             sxml_sequence *result) {
    sxml_optional_item argument = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &argument);
    sxml_node_reference node = {.tree = NULL};
    if (status == SXML_STATUS_OK && argument.has_item) {
        status = sxml_item_get_node(argument.item, &node);
    }
    sxml_node_description description;
    if (status == SXML_STATUS_OK && argument.has_item) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!argument.has_item || (description.node_kind != SXML_NODE_KIND_ELEMENT &&
                               description.node_kind != SXML_NODE_KIND_ATTRIBUTE)) {
        return sxml_function_library_report(call, &node_kind_error);
    }
    sxml_type_identifier type = description.type_annotation;
    if (type.index == SXML_TYPE_NONE) {
        type = sxml_builtin_type_identifier(description.node_kind == SXML_NODE_KIND_ELEMENT
                                                ? SXML_TYPE_XS_UNTYPED
                                                : SXML_TYPE_XS_UNTYPED_ATOMIC);
    }
    return make_type_record(call, type, result);
}

static const sxml_function_parameter type_of_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "item()*")};
static const sxml_function_parameter schema_type_parameters[] = {
    SXML_REQUIRED_PARAMETER("name", "xs:QName")};
static const sxml_function_parameter atomic_type_annotation_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "xs:anyAtomicType")};
/* (element() | attribute()), checked by the body (D78: no coercion into
   choice types). */
static const sxml_function_parameter node_type_annotation_parameters[] = {
    SXML_REQUIRED_PARAMETER("node", "xnode()")};

static const sxml_native_function type_functions[] = {
    SXML_FUNCTION_ROW("type-of", type_of_parameters, "xs:string", 0, call_type_of),
    SXML_FUNCTION_ROW("schema-type", schema_type_parameters, "fn:schema-type-record?", 0,
                      call_schema_type),
    SXML_FUNCTION_ROW("atomic-type-annotation", atomic_type_annotation_parameters,
                      "fn:schema-type-record", 0, call_atomic_type_annotation),
    SXML_FUNCTION_ROW("node-type-annotation", node_type_annotation_parameters,
                      "fn:schema-type-record", 0, call_node_type_annotation),
};

const sxml_native_function_view sxml_function_library_type = SXML_LIBRARY_PART(type_functions);
