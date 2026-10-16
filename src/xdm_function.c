/*
 * Function items (include/sxml/xdm.h; D33, D38, D60): making, describing
 * and calling them, with maps and arrays answering calls as map:get and
 * array:get (D64).
 *
 * Golden file: src/xdm.c (cells, cell classes, failure paths), after
 * src/memory.c.
 *
 * Specification: XDM 4.0, section 8.1 (function items: name, signature,
 * annotations, body, captured context; function identity is withdrawn);
 * XPath 4.0, section 4.5.3 (dynamic function calls: arity check, then the
 * body); XDM 4.0, sections 8.2 and 8.4 with F&O 4.0, map:get and array:get
 * (maps and arrays as functions of arity 1).
 */
#include "xdm_internal.h"

#include <stdalign.h>

/* -------------------------------------------------------- Function cells */

/*
 * A function item: the request's properties, the body (retained through its
 * retain operation) and the captured values, each holding one reference.
 */
typedef struct sxml_function_cell {
    sxml_cell cell;
    sxml_name_identifier function_name;
    sxml_type_identifier signature;
    size_t arity;
    sxml_function_annotation_view function_annotations;
    sxml_function_body body;
    size_t captured_value_count;
    sxml_sequence captured_values[];
} sxml_function_cell;

static sxml_function_cell *sxml_function_cell_from_cell(sxml_cell *cell) {
    return (sxml_function_cell *)cell;
}

static void sxml_function_cell_release_contents(sxml_cell *cell) {
    sxml_function_cell *function = sxml_function_cell_from_cell(cell);
    for (size_t value_index = 0; value_index < function->captured_value_count;
         value_index += 1) {
        sxml_sequence_release(function->captured_values[value_index]);
    }
    if (function->body.operations->release != NULL) {
        function->body.operations->release(function->body.state);
    }
}

static const sxml_cell_class sxml_function_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("function"),
    .release_contents = sxml_function_cell_release_contents,
    .sequence_operations = NULL,
};

/*
 * Writes the arity of signature to *arity: the parameter count of a
 * function type registered in item_heap's registry. function(*) has no
 * arity and is rejected, as is every other type.
 */
static sxml_status
sxml_function_signature_get_arity(const sxml_item_heap *item_heap,
                                  sxml_type_identifier signature,
                                  size_t *arity) {
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(
        sxml_item_heap_get_type_registry(item_heap), signature, &description);
    if (status != SXML_STATUS_OK ||
        description.variety != SXML_TYPE_VARIETY_FUNCTION ||
        description.variety_details.function_type.is_any_function) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *arity = description.variety_details.function_type.parameter_types
                 .sequence_type_count;
    return SXML_STATUS_OK;
}

sxml_status sxml_function_item_make(sxml_item_heap *item_heap,
                                    const sxml_function_item_request *request,
                                    sxml_item *item) {
    if (request->body.operations == NULL ||
        request->body.operations->call == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t arity = 0;
    sxml_status status = sxml_function_signature_get_arity(
        item_heap, request->signature, &arity);
    size_t captured_value_count = request->captured_values.sequence_count;
    size_t values_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(captured_value_count, sizeof(sxml_sequence),
                                    &values_byte_count);
    }
    size_t cell_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sizeof(sxml_function_cell), values_byte_count,
                               &cell_byte_count);
    }
    sxml_cell *cell = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_cell(
            item_heap,
            &(sxml_cell_request){.cell_class = &sxml_function_cell_class,
                                 .byte_count = cell_byte_count,
                                 .alignment = alignof(sxml_function_cell)},
            &cell);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_function_cell *function = sxml_function_cell_from_cell(cell);
    function->function_name = request->function_name;
    function->signature = request->signature;
    function->arity = arity;
    function->function_annotations = request->function_annotations;
    function->body = request->body;
    function->captured_value_count = captured_value_count;
    for (size_t value_index = 0; value_index < captured_value_count;
         value_index += 1) {
        sxml_sequence value = request->captured_values.sequences[value_index];
        sxml_sequence_retain(value);
        function->captured_values[value_index] = value;
    }
    if (request->body.operations->retain != NULL) {
        request->body.operations->retain(request->body.state);
    }
    /* The signature is the type annotation (xdm.h,
       sxml_item_get_type_annotation). */
    *item = sxml_item_from_cell(&(sxml_cell_item_request){
        .representation = SXML_ITEM_REPRESENTATION_FUNCTION_CELL,
        .type_annotation = request->signature,
        .primitive_type = sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .cell = cell});
    return SXML_STATUS_OK;
}

/* ------------------------------------------- Maps and arrays as functions */

static const sxml_static_error sxml_wrong_argument_count_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A function item was called with a number of arguments other than "
        "its arity (XPath 4.0, section 4.5.3)"),
};

static const sxml_static_error sxml_map_key_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A map called as a function needs one atomic item as its key "
        "(F&O 4.0, map:get)"),
};

static const sxml_static_error sxml_array_position_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An array called as a function needs one xs:integer as its "
        "position (F&O 4.0, array:get)"),
};

static const sxml_static_error sxml_array_index_out_of_bounds_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAY0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Array index out of bounds (F&O 4.0, array:get)"),
};

/*
 * True when the call has one argument holding one item, which it writes to
 * *item. The arity was checked by sxml_function_item_call; the bodies check
 * again because a native caller may call them directly.
 */
static bool
sxml_function_call_find_single_argument(const sxml_function_call *call,
                                        sxml_item *item) {
    if (call->arguments.sequence_count != 1) {
        return false;
    }
    sxml_sequence argument = call->arguments.sequences[0];
    if (sxml_sequence_get_representation(argument) !=
        SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        return false;
    }
    *item = argument.content;
    return true;
}

/* map:get($map, $key) with the called map (F&O 4.0, map:get). */
static sxml_status sxml_map_body_call(void *state,
                                      const sxml_function_call *call,
                                      sxml_sequence *result) {
    (void)state;
    sxml_item key = {.representation = 0};
    if (!sxml_function_call_find_single_argument(call, &key) ||
        !sxml_item_is_atomic(key)) {
        return sxml_error_record_report(call->error_record,
                                        &sxml_map_key_type_error);
    }
    sxml_map_lookup lookup;
    sxml_status status = sxml_map_find(call->function_item, key, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* An absent key gives the empty sequence. */
    sxml_sequence value = sxml_sequence_empty();
    if (lookup.is_found) {
        value = lookup.entry.value;
        sxml_sequence_retain(value);
    }
    *result = value;
    return SXML_STATUS_OK;
}

/* True when item is an xs:integer (or of a type derived from it) beyond
   int64_t, which is kept in a cell. */
static bool sxml_item_is_large_integer(const sxml_function_call *call,
                                       sxml_item item) {
    if (call->item_heap == NULL || sxml_item_get_representation(item) !=
                                       SXML_ITEM_REPRESENTATION_ATOMIC_CELL) {
        return false;
    }
    return sxml_type_registry_is_derived_from(
        sxml_item_heap_get_type_registry(call->item_heap),
        (sxml_type_pair){
            .derived_type = sxml_item_get_type_annotation(item),
            .base_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER)});
}

/* array:get($array, $position) with the called array (F&O 4.0,
   array:get): positions count from 1. */
static sxml_status sxml_array_body_call(void *state,
                                        const sxml_function_call *call,
                                        sxml_sequence *result) {
    (void)state;
    sxml_item position = {.representation = 0};
    if (!sxml_function_call_find_single_argument(call, &position)) {
        return sxml_error_record_report(call->error_record,
                                        &sxml_array_position_type_error);
    }
    if (sxml_item_is_large_integer(call, position)) {
        /* Beyond int64_t, so beyond every array's size. */
        return sxml_error_record_report(call->error_record,
                                        &sxml_array_index_out_of_bounds_error);
    }
    if (sxml_item_get_representation(position) !=
        SXML_ITEM_REPRESENTATION_INTEGER) {
        return sxml_error_record_report(call->error_record,
                                        &sxml_array_position_type_error);
    }
    size_t member_count = 0;
    sxml_status status =
        sxml_array_get_size(call->function_item, &member_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    int64_t position_number = position.payload.integer;
    if (position_number < 1 || (uint64_t)position_number > member_count) {
        return sxml_error_record_report(call->error_record,
                                        &sxml_array_index_out_of_bounds_error);
    }
    sxml_sequence member = sxml_sequence_empty();
    status = sxml_array_get_member(call->function_item,
                                   (size_t)(position_number - 1), &member);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(member);
    *result = member;
    return SXML_STATUS_OK;
}

/* The bodies hold no state, so they need no retain or release. */
static const sxml_function_body_operations sxml_map_body_operations = {
    .call = sxml_map_body_call,
    .retain = NULL,
    .release = NULL,
};

static const sxml_function_body_operations sxml_array_body_operations = {
    .call = sxml_array_body_call,
    .retain = NULL,
    .release = NULL,
};

/* The description of a map or an array: arity 1, no name, annotations or
   captured values (xdm.h, sxml_function_item_description). */
static sxml_function_item_description sxml_function_describe_collection(
    sxml_item item, const sxml_function_body_operations *operations) {
    return (sxml_function_item_description){
        .function_name = {.index = 0},
        .signature = sxml_item_get_type_annotation(item),
        .arity = 1,
        .function_annotations = {.function_annotations = NULL,
                                 .function_annotation_count = 0},
        .captured_values = {.sequences = NULL, .sequence_count = 0},
        .body = {.state = NULL, .operations = operations},
    };
}

sxml_status
sxml_function_item_describe(sxml_item item,
                            sxml_function_item_description *description) {
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL: {
        const sxml_function_cell *function =
            sxml_function_cell_from_cell(sxml_item_get_cell(item));
        *description = (sxml_function_item_description){
            .function_name = function->function_name,
            .signature = function->signature,
            .arity = function->arity,
            .function_annotations = function->function_annotations,
            .captured_values = {.sequences = function->captured_values,
                                .sequence_count =
                                    function->captured_value_count},
            .body = function->body,
        };
        return SXML_STATUS_OK;
    }
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
        *description =
            sxml_function_describe_collection(item, &sxml_map_body_operations);
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
        *description = sxml_function_describe_collection(
            item, &sxml_array_body_operations);
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_INTEGER:
    case SXML_ITEM_REPRESENTATION_FLOAT:
    case SXML_ITEM_REPRESENTATION_DOUBLE:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_function_item_call(const sxml_function_call *call,
                                    sxml_sequence *result) {
    sxml_function_item_description description;
    sxml_status status =
        sxml_function_item_describe(call->function_item, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* XPath 4.0, section 4.5.3: the number of arguments must equal the
       arity (XPTY0004). Coercing them to the parameter types is the
       caller's (xdm.h). */
    if (call->arguments.sequence_count != description.arity) {
        return sxml_error_record_report(call->error_record,
                                        &sxml_wrong_argument_count_error);
    }
    return description.body.operations->call(description.body.state, call,
                                             result);
}
