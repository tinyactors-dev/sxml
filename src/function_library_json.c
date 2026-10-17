/*
 * The functions on JSON data and fn:parse-xml (F&O 4.0, sections 16.1.1,
 * 17.2.1, 17.4.4, 17.4.6, 17.4.7): one library part,
 * sxml_function_library_json (src/function_library_internal.h, D55, D82).
 * fn:parse-json, fn:json-to-xml and fn:xml-to-json check their options by
 * the option parameter conventions (F&O 4.0, section 1.7) and hand the
 * work to src/json.c; fn:parse-xml goes through the document loader of
 * the dynamic context (D55), so a new tree is made on every call; fn:jtree
 * makes a root JNode (D70).
 *
 * Golden file: src/function_library_map.c.
 */
#include "function_library_internal.h"

#include "evaluator_internal.h"
#include "json_internal.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------- Static errors */

static const sxml_static_error json_unknown_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options map has a key that is not an option of this function "
        "(F&O 4.0, section 1.7)"),
};

static const sxml_static_error json_invalid_option_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "An option has a value that is not allowed for it"),
};

static const sxml_static_error json_validation_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "validate=true needs a schema-aware processor, which sxml is not yet"),
};

static const sxml_static_error xml_unsupported_option_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0013"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The XML parser does not support the requested option (DTD "
        "validation, XInclude, strip-space=\"all\")"),
};

static const sxml_static_error xml_schema_validation_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0009"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "xsd-validation other than \"skip\" needs a schema-aware processor"),
};

static const sxml_static_error xml_wrong_input_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:parse-xml takes xs:string, xs:hexBinary or xs:base64Binary"),
};

/* ------------------------------------------------------------- Helpers */

static sxml_sequence json_argument(const sxml_function_call *call, size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

static bool json_sequence_is_empty(sxml_sequence sequence) {
    return sxml_sequence_get_representation(sequence) == SXML_SEQUENCE_REPRESENTATION_EMPTY;
}

/* The allocator for work memory: the dynamic context's, or the heap's for
   a call from outside an evaluation. */
static sxml_allocator json_call_allocator(const sxml_function_call *call) {
    return call->dynamic_context != NULL ? call->dynamic_context->allocator
                                         : sxml_item_heap_as_allocator(call->item_heap);
}

/* The text of a string argument (already coerced to xs:string?). */
static sxml_status json_read_text(sxml_sequence argument, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    const sxml_status status = sxml_atomic_item_get_datum(argument.content, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *text = datum.datum_details.string.utf8;
    }
    return status;
}

/* ------------------------------------------------------------- Options */

/* The type an option's value is coerced to (F&O 4.0, section 1.7). */
typedef enum json_option_type {
    SXML_JSON_OPTION_TYPE_BOOLEAN,
    SXML_JSON_OPTION_TYPE_STRING,
    /* item()? (the null option). */
    SXML_JSON_OPTION_TYPE_OPTIONAL_ITEM,
    /* A function item or the empty sequence (the fallback option). */
    SXML_JSON_OPTION_TYPE_OPTIONAL_FUNCTION,
} json_option_type;

/* One option a function knows. */
typedef struct json_option_definition {
    const char *option_name;
    json_option_type option_type;
} json_option_definition;

typedef struct json_option_definition_view {
    const json_option_definition *definitions;
    size_t definition_count;
} json_option_definition_view;

/* The most options one function knows. */
enum { SXML_JSON_OPTION_LIMIT = 9 };

/* The options found in a map, by definition position: values owned and
   coerced. */
typedef struct json_option_values {
    bool is_present[SXML_JSON_OPTION_LIMIT];
    sxml_sequence values[SXML_JSON_OPTION_LIMIT];
} json_option_values;

static void json_option_values_release(json_option_values *values) {
    for (size_t option_index = 0; option_index < SXML_JSON_OPTION_LIMIT; option_index += 1) {
        sxml_sequence_release(values->values[option_index]);
        values->values[option_index] = sxml_sequence_empty();
        values->is_present[option_index] = false;
    }
}

/* A key that names an option: an xs:string, xs:untypedAtomic or xs:anyURI
   (F&O 4.0, section 1.7: "the same key as the required string"). */
static bool json_key_text(sxml_item key, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(key, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* True for an xs:QName key with a namespace: an implementation-defined
   option, which is ignored (F&O 4.0, section 1.7). */
static bool json_key_is_extension(sxml_item key, const sxml_name_pool *name_pool) {
    sxml_atomic_datum datum;
    sxml_name_parts parts;
    return sxml_atomic_item_get_datum(key, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_QNAME &&
           sxml_name_pool_get_name_parts(name_pool, datum.datum_details.qname.expanded_name,
                                         &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index != SXML_NAMESPACE_NONE;
}

/* An option value and the type it must have. */
typedef struct json_option_value {
    json_option_type option_type;
    sxml_sequence value;
} json_option_value;

/* Coerces one option value to its type (XPTY0004 when it cannot be). */
static sxml_status json_coerce_option(const sxml_function_call *call,
                                      json_option_value option_value, sxml_sequence *coerced) {
    const json_option_type option_type = option_value.option_type;
    const sxml_sequence value = option_value.value;
    if (option_type == SXML_JSON_OPTION_TYPE_OPTIONAL_FUNCTION) {
        uint64_t item_count = 0;
        sxml_status status = sxml_sequence_count(value, call->error_record, &item_count);
        if (status == SXML_STATUS_OK &&
            (item_count > 1 || (item_count == 1 && !sxml_item_is_function_item(value.content)))) {
            return sxml_error_record_report(call->error_record, &json_unknown_option_error);
        }
        if (status == SXML_STATUS_OK) {
            sxml_sequence_retain(value);
            *coerced = value;
        }
        return status;
    }
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_sequence_type sequence_type =
        option_type == SXML_JSON_OPTION_TYPE_BOOLEAN
            ? (sxml_sequence_type){.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
                                   .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}
        : option_type == SXML_JSON_OPTION_TYPE_STRING
            ? (sxml_sequence_type){.item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                   .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}
            : (sxml_sequence_type){.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                   .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    return sxml_evaluator_coerce(call->dynamic_context,
                                 &(sxml_coercion_request){
                                     .value = value,
                                     .sequence_type = sequence_type,
                                     .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                                     .error_record = call->error_record},
                                 coerced);
}

/* A function's options map to read. */
typedef struct json_option_request {
    /* map(*)? as coerced; empty means no options. */
    sxml_sequence options;
    json_option_definition_view definitions;
} json_option_request;

/* Reads the options map: every key a known option (or an extension
   QName), every value coerced to its type. *values must be zeroed. */
static sxml_status json_read_options(const sxml_function_call *call,
                                     const json_option_request *request,
                                     json_option_values *values) {
    if (json_sequence_is_empty(request->options)) {
        return SXML_STATUS_OK;
    }
    const sxml_item map = request->options.content;
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_byte_view key_text = {.bytes = NULL, .byte_count = 0};
        size_t found = request->definitions.definition_count;
        if (json_key_text(entry.key, &key_text)) {
            for (size_t definition_index = 0;
                 definition_index < request->definitions.definition_count;
                 definition_index += 1) {
                if (sxml_byte_view_equals(
                        key_text, sxml_byte_view_from_c_string(
                                      request->definitions.definitions[definition_index]
                                          .option_name))) {
                    found = definition_index;
                }
            }
        }
        if (found == request->definitions.definition_count) {
            if (json_key_is_extension(entry.key, name_pool)) {
                continue;
            }
            return sxml_error_record_report(call->error_record, &json_unknown_option_error);
        }
        sxml_sequence coerced = sxml_sequence_empty();
        status = json_coerce_option(
            call,
            (json_option_value){.option_type = request->definitions.definitions[found].option_type,
                                .value = entry.value},
            &coerced);
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(values->values[found]);
            values->values[found] = coerced;
            values->is_present[found] = true;
        }
    }
    return status;
}

/* True when a boolean option is present and true. */
static bool json_option_is_true(const json_option_values *values, size_t option_index) {
    sxml_atomic_datum datum;
    return values->is_present[option_index] &&
           sxml_atomic_item_get_datum(values->values[option_index].content, &datum) ==
               SXML_STATUS_OK &&
           datum.datum_details.boolean;
}

/* The string value of a present string option; empty otherwise. */
static sxml_byte_view json_option_text(const json_option_values *values,
                                       size_t option_index) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (values->is_present[option_index]) {
        (void)json_read_text(values->values[option_index], &text);
    }
    return text;
}

static bool json_text_is(sxml_byte_view text, sxml_byte_view expected) {
    return sxml_byte_view_equals(text, expected);
}

/* The options fn:parse-json and fn:json-to-xml share, by position. */
enum {
    SXML_JSON_OPTION_LIBERAL,
    SXML_JSON_OPTION_DUPLICATES,
    SXML_JSON_OPTION_ESCAPE,
    SXML_JSON_OPTION_FALLBACK,
    SXML_JSON_OPTION_NULL,
    SXML_JSON_OPTION_NUMBER_FORMAT,
    SXML_JSON_OPTION_VALIDATE,
};

static const json_option_definition parse_json_options[] = {
    {"liberal", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"duplicates", SXML_JSON_OPTION_TYPE_STRING},
    {"escape", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"fallback", SXML_JSON_OPTION_TYPE_OPTIONAL_FUNCTION},
    {"null", SXML_JSON_OPTION_TYPE_OPTIONAL_ITEM},
    {"number-format", SXML_JSON_OPTION_TYPE_STRING},
};

static const json_option_definition json_to_xml_options[] = {
    {"liberal", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"duplicates", SXML_JSON_OPTION_TYPE_STRING},
    {"escape", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"fallback", SXML_JSON_OPTION_TYPE_OPTIONAL_FUNCTION},
    /* Positions SXML_JSON_OPTION_NULL and SXML_JSON_OPTION_NUMBER_FORMAT are
       fn:parse-json's; these names never match a key. */
    {"", SXML_JSON_OPTION_TYPE_STRING},
    {"", SXML_JSON_OPTION_TYPE_STRING},
    {"validate", SXML_JSON_OPTION_TYPE_BOOLEAN},
};

/* What the shared JSON options become; is_xml says which function. */
typedef struct json_option_reading {
    bool is_xml;
    const json_option_values *values;
} json_option_reading;

/*
 * Turns the coerced options into sxml_json_parse_options (F&O 4.0,
 * sections 17.4.4 and 17.4.6): FOJS0005 for a value not allowed, or for
 * fallback with escape=true; FOJS0004 for validate=true (sxml is not
 * schema-aware). A fallback function is not supported yet: the default
 * fallback, U+FFFD, is the only one.
 */
static sxml_status json_interpret_options(const sxml_function_call *call,
                                          json_option_reading reading,
                                          sxml_json_parse_options *options) {
    const json_option_values *values = reading.values;
    *options = (sxml_json_parse_options){
        .duplicates_policy = reading.is_xml ? SXML_JSON_DUPLICATES_POLICY_RETAIN
                                            : SXML_JSON_DUPLICATES_POLICY_USE_FIRST,
        .keeps_escaped_form = json_option_is_true(values, SXML_JSON_OPTION_ESCAPE),
        .number_format = SXML_JSON_NUMBER_FORMAT_DOUBLE,
        .null_value = values->is_present[SXML_JSON_OPTION_NULL] ? values->values[SXML_JSON_OPTION_NULL]
                                                           : sxml_sequence_empty()};
    if (reading.is_xml && json_option_is_true(values, SXML_JSON_OPTION_VALIDATE)) {
        return sxml_error_record_report(call->error_record, &json_validation_error);
    }
    if (values->is_present[SXML_JSON_OPTION_DUPLICATES]) {
        const sxml_byte_view duplicates = json_option_text(values, SXML_JSON_OPTION_DUPLICATES);
        if (json_text_is(duplicates, SXML_BYTE_VIEW_FROM_LITERAL("reject"))) {
            options->duplicates_policy = SXML_JSON_DUPLICATES_POLICY_REJECT;
        } else if (json_text_is(duplicates, SXML_BYTE_VIEW_FROM_LITERAL("use-first"))) {
            options->duplicates_policy = SXML_JSON_DUPLICATES_POLICY_USE_FIRST;
        } else if (!reading.is_xml && json_text_is(duplicates, SXML_BYTE_VIEW_FROM_LITERAL("use-last"))) {
            options->duplicates_policy = SXML_JSON_DUPLICATES_POLICY_USE_LAST;
        } else if (reading.is_xml && json_text_is(duplicates, SXML_BYTE_VIEW_FROM_LITERAL("retain"))) {
            options->duplicates_policy = SXML_JSON_DUPLICATES_POLICY_RETAIN;
        } else {
            return sxml_error_record_report(call->error_record, &json_invalid_option_error);
        }
    }
    if (!reading.is_xml && values->is_present[SXML_JSON_OPTION_NUMBER_FORMAT]) {
        const sxml_byte_view number_format =
            json_option_text(values, SXML_JSON_OPTION_NUMBER_FORMAT);
        if (json_text_is(number_format, SXML_BYTE_VIEW_FROM_LITERAL("double"))) {
            options->number_format = SXML_JSON_NUMBER_FORMAT_DOUBLE;
        } else if (json_text_is(number_format, SXML_BYTE_VIEW_FROM_LITERAL("decimal"))) {
            options->number_format = SXML_JSON_NUMBER_FORMAT_DECIMAL;
        } else if (json_text_is(number_format, SXML_BYTE_VIEW_FROM_LITERAL("adaptive"))) {
            options->number_format = SXML_JSON_NUMBER_FORMAT_ADAPTIVE;
        } else {
            return sxml_error_record_report(call->error_record, &json_invalid_option_error);
        }
    }
    if (values->is_present[SXML_JSON_OPTION_FALLBACK] &&
        !json_sequence_is_empty(values->values[SXML_JSON_OPTION_FALLBACK])) {
        if (options->keeps_escaped_form) {
            return sxml_error_record_report(call->error_record, &json_invalid_option_error);
        }
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    return SXML_STATUS_OK;
}

/* Which function's options to read, and where the coerced values go
   (owned; the options borrow them). */
typedef struct json_parse_option_target {
    bool is_xml;
    json_option_values *values;
} json_parse_option_target;

/* Reads and interprets the options of fn:parse-json or fn:json-to-xml. */
static sxml_status json_read_parse_options(const sxml_function_call *call,
                                           json_parse_option_target target,
                                           sxml_json_parse_options *options) {
    const bool is_xml = target.is_xml;
    json_option_values *values = target.values;
    const json_option_request request = {
        .options = json_argument(call, 1),
        .definitions = is_xml ? (json_option_definition_view){json_to_xml_options,
                                                              sizeof(json_to_xml_options) /
                                                                  sizeof(json_to_xml_options[0])}
                              : (json_option_definition_view){parse_json_options,
                                                              sizeof(parse_json_options) /
                                                                  sizeof(parse_json_options[0])}};
    sxml_status status = json_read_options(call, &request, values);
    if (status == SXML_STATUS_OK) {
        status = json_interpret_options(
            call, (json_option_reading){.is_xml = is_xml, .values = values}, options);
    }
    return status;
}

/* --------------------------------------------------------------- Bodies */

/* fn:parse-json($value as xs:string?, $options as map(*)? := {}) as
   item()? (F&O 4.0, section 17.4.4). */
static sxml_status call_parse_json(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = json_argument(call, 0);
    if (json_sequence_is_empty(value)) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    json_option_values values;
    memset(&values, 0, sizeof(values));
    sxml_json_parse_options options;
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = json_read_text(value, &text);
    if (status == SXML_STATUS_OK) {
        status = json_read_parse_options(
            call, (json_parse_option_target){.is_xml = false, .values = &values}, &options);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_make_value(call->item_heap,
                                      &(sxml_json_text_request){
                                          .json_text = text,
                                          .options = options,
                                          .allocator = json_call_allocator(call),
                                          .maximum_depth = 0,
                                          .error_record = call->error_record},
                                      result);
    }
    json_option_values_release(&values);
    return status;
}

/* Sends a document of the XML representation of the JSON text to
   arena_tree. */
static sxml_status json_build_document(const sxml_function_call *call,
                                       const sxml_json_text_request *request,
                                       sxml_arena_tree *arena_tree) {
    const sxml_tree_builder builder = sxml_arena_tree_as_tree_builder(arena_tree);
    /* The base URI of the document node is the static base URI of the
       call (F&O 4.0, fn:json-to-xml; D88). */
    const sxml_byte_view base_uri =
        call->dynamic_context == NULL
            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
            : sxml_dynamic_context_get_static_base_uri(call->dynamic_context);
    sxml_status status = builder.operations->start_document(
        builder.state,
        &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                     .base_uri = base_uri,
                                     .location = {.source_name = {.bytes = NULL,
                                                                  .byte_count = 0},
                                                  .line_number = 0,
                                                  .column_number = 0,
                                                  .byte_offset = 0}});
    if (status == SXML_STATUS_OK) {
        status = sxml_json_send_xml_events(call->item_heap, request, builder);
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* fn:json-to-xml($value as xs:string?, $options as map(*)? := {}) as
   document-node(fn:*)? (F&O 4.0, section 17.4.6): a new untyped tree that
   destroys itself when unreferenced, its memory from the heap. */
static sxml_status call_json_to_xml(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = json_argument(call, 0);
    if (json_sequence_is_empty(value)) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    json_option_values values;
    memset(&values, 0, sizeof(values));
    sxml_json_parse_options options;
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = json_read_text(value, &text);
    if (status == SXML_STATUS_OK) {
        status = json_read_parse_options(
            call, (json_parse_option_target){.is_xml = true, .values = &values}, &options);
    }
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            tree_allocator,
            &(sxml_arena_tree_configuration){
                .allocator = tree_allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = call->item_heap,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL},
            &arena_tree);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = json_build_document(call,
                                     &(sxml_json_text_request){
                                         .json_text = text,
                                         .options = options,
                                         .allocator = json_call_allocator(call),
                                         .maximum_depth = 0,
                                         .error_record = call->error_record},
                                     arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    json_option_values_release(&values);
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

static const json_option_definition xml_to_json_options[] = {
    {"escape-solidus", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"indent", SXML_JSON_OPTION_TYPE_BOOLEAN},
};

/* fn:xml-to-json($node as node()?, $options as map(*)? := {}) as
   xs:string? (F&O 4.0, section 17.4.7). indent may insert whitespace; sxml
   inserts none. */
static sxml_status call_xml_to_json(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence node_argument = json_argument(call, 0);
    if (json_sequence_is_empty(node_argument)) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    json_option_values values;
    memset(&values, 0, sizeof(values));
    sxml_status status = json_read_options(
        call,
        &(json_option_request){
            .options = json_argument(call, 1),
            .definitions = {xml_to_json_options,
                            sizeof(xml_to_json_options) / sizeof(xml_to_json_options[0])}},
        &values);
    sxml_node_reference node;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(node_argument.content, &node);
    }
    const sxml_allocator allocator = json_call_allocator(call);
    sxml_output_buffer *output = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator,
            &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &output);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write_xml_representation(
            call->item_heap,
            &(sxml_json_writing_request){
                .node = node,
                .escapes_solidus = !values.is_present[0] || json_option_is_true(&values, 0),
                .allocator = allocator,
                .error_record = call->error_record},
            sxml_output_buffer_as_output_stream(output));
    }
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    if (status == SXML_STATUS_OK) {
        status = sxml_string_from_utf8(sxml_output_buffer_get_bytes(output), &string);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            call->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                          .datum_details = {.string = string}}},
            &item);
    }
    sxml_output_buffer_destroy(output);
    json_option_values_release(&values);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* fn:jtree($input as item()*) as jnode() (F&O 4.0, section 16.1.1). */
static sxml_status call_jtree(const sxml_function_call *call, sxml_sequence *result) {
    sxml_item json_node = {.representation = 0};
    const sxml_status status =
        sxml_json_node_make_root(call->item_heap, json_argument(call, 0), &json_node);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(json_node);
    }
    return status;
}

/* The options of fn:parse-xml (F&O 4.0, section 17.2.1). */
enum {
    SXML_XML_OPTION_BASE_URI,
    SXML_XML_OPTION_DTD_VALIDATION,
    SXML_XML_OPTION_STRIP_SPACE,
    SXML_XML_OPTION_TRUST_EXTERNAL,
    SXML_XML_OPTION_XINCLUDE,
    SXML_XML_OPTION_XSD_VALIDATION,
    SXML_XML_OPTION_USE_XSI_SCHEMA_LOCATION,
    SXML_XML_OPTION_RETAIN_LOCATION,
};

static const json_option_definition parse_xml_options[] = {
    {"base-uri", SXML_JSON_OPTION_TYPE_STRING},
    {"dtd-validation", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"strip-space", SXML_JSON_OPTION_TYPE_STRING},
    {"trust-external", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"xinclude", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"xsd-validation", SXML_JSON_OPTION_TYPE_STRING},
    {"use-xsi-schema-location", SXML_JSON_OPTION_TYPE_BOOLEAN},
    {"retain-location", SXML_JSON_OPTION_TYPE_BOOLEAN},
};

/* Checks the parse-xml options sxml cannot honour: DTD validation and
   XInclude (FODC0013), strip-space="all" (FODC0013: not supported yet),
   XSD validation (FODC0009). An unknown strip-space value is XPTY0004,
   since the option's type is an enumeration. */
static sxml_status xml_check_options(const sxml_function_call *call,
                                     const json_option_values *values) {
    if (json_option_is_true(values, SXML_XML_OPTION_DTD_VALIDATION) ||
        json_option_is_true(values, SXML_XML_OPTION_XINCLUDE)) {
        return sxml_error_record_report(call->error_record, &xml_unsupported_option_error);
    }
    if (values->is_present[SXML_XML_OPTION_STRIP_SPACE]) {
        const sxml_byte_view strip_space = json_option_text(values, SXML_XML_OPTION_STRIP_SPACE);
        if (json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("all"))) {
            return sxml_error_record_report(call->error_record, &xml_unsupported_option_error);
        }
        if (!json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("none")) && !json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("conditional"))) {
            return sxml_error_record_report(call->error_record, &json_unknown_option_error);
        }
    }
    if (values->is_present[SXML_XML_OPTION_XSD_VALIDATION] &&
        !json_text_is(json_option_text(values, SXML_XML_OPTION_XSD_VALIDATION),
                      SXML_BYTE_VIEW_FROM_LITERAL("skip"))) {
        return sxml_error_record_report(call->error_record, &xml_schema_validation_error);
    }
    return SXML_STATUS_OK;
}

/*
 * fn:parse-xml($value as (xs:string | xs:hexBinary | xs:base64Binary)?,
 * $options as map(*)? := {}) as document-node(*)? (F&O 4.0, section
 * 17.2.1): the bytes go to the dynamic context's document loader, which
 * makes a new tree on every call (FODC0006 for a document that is not
 * well-formed). A string is passed as its UTF-8 bytes; binary values are
 * passed as they are, so their byte order mark and encoding declaration
 * count.
 */
static sxml_status call_parse_xml(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence value = json_argument(call, 0);
    if (json_sequence_is_empty(value)) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(value.content, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
        return sxml_error_record_report(call->error_record, &xml_wrong_input_error);
    }
    json_option_values values;
    memset(&values, 0, sizeof(values));
    status = json_read_options(
        call,
        &(json_option_request){.options = json_argument(call, 1),
                               .definitions = {parse_xml_options,
                                               sizeof(parse_xml_options) /
                                                   sizeof(parse_xml_options[0])}},
        &values);
    if (status == SXML_STATUS_OK) {
        status = xml_check_options(call, &values);
    }
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_parse_document(
            call->dynamic_context,
            &(sxml_document_parse_request){
                .xml_bytes = datum.datum_kind == SXML_DATUM_KIND_STRING
                                 ? datum.datum_details.string.utf8
                                 : datum.datum_details.binary,
                /* The base-uri option, else the static base URI (F&O 4.0,
                   fn:parse-xml; D88). */
                .base_uri = values.is_present[SXML_XML_OPTION_BASE_URI]
                                ? json_option_text(&values, SXML_XML_OPTION_BASE_URI)
                                : sxml_dynamic_context_get_static_base_uri(
                                      call->dynamic_context),
                .error_record = call->error_record},
            &document_node);
    }
    json_option_values_release(&values);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

/* ----------------------------------------------------------------- Rows */

#define JSON_PARAMETER(name_text, type_text)                                   \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                  \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                  \
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED}

#define JSON_OPTIONAL_PARAMETER(name_text, type_text)                          \
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                  \
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER(type_text),                  \
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY}

#define JSON_ROW(name_text, parameter_array, result_text, property_bits, body) \
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,                           \
     .properties = (property_bits),                                            \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                      \
     .parameters = {(parameter_array),                                         \
                    sizeof(parameter_array) / sizeof((parameter_array)[0])},   \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                   \
     .call = (body),                                                           \
     .resumable_body = NULL}

static const sxml_function_parameter parse_json_parameters[] = {
    JSON_PARAMETER("value", "xs:string?"),
    JSON_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_function_parameter xml_to_json_parameters[] = {
    JSON_PARAMETER("node", "node()?"),
    JSON_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_function_parameter jtree_parameters[] = {
    JSON_PARAMETER("input", "item()*"),
};

/* The choice type of $value is checked by the body (D78: the evaluator
   does not coerce into choice types), so the parameter is an atomic
   item. */
static const sxml_function_parameter parse_xml_parameters[] = {
    JSON_PARAMETER("value", "xs:anyAtomicType?"),
    JSON_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_native_function json_functions[] = {
    JSON_ROW("parse-json", parse_json_parameters, "item()?", 0, call_parse_json),
    JSON_ROW("json-to-xml", parse_json_parameters, "document-node()?",
             SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC, call_json_to_xml),
    JSON_ROW("xml-to-json", xml_to_json_parameters, "xs:string?", 0, call_xml_to_json),
    JSON_ROW("jtree", jtree_parameters, "jnode()", SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
             call_jtree),
    JSON_ROW("parse-xml", parse_xml_parameters, "document-node()?",
             SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
             call_parse_xml),
};

const sxml_native_function_view sxml_function_library_json = {
    .native_functions = json_functions,
    .native_function_count = sizeof(json_functions) / sizeof(json_functions[0]),
};
