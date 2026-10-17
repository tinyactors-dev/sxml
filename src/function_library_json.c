/*
 * The functions on JSON data and fn:parse-xml (F&O 4.0, sections 16.1.1,
 * 17.2.1, 17.4.4, 17.4.6, 17.4.7): one library part,
 * sxml_function_library_json (src/function_library_internal.h, D55, D82).
 * fn:parse-json, fn:json-to-xml and fn:xml-to-json check their options by
 * the option parameter conventions (F&O 4.0, section 1.7) and hand the
 * work to src/json.c; fn:parse-xml goes through the document loader of
 * the dynamic context (D55), so a new tree is made on every call; fn:jtree
 * makes a root JNode (D70). From packet 2.17: fn:parse-xml-fragment
 * (17.2.2), strip-space="all" for both parse functions, and
 * fn:system-properties (17.1.9), the other function on the processor's
 * environment that the resource part (2.12) does not hold.
 *
 * Golden file: src/function_library_map.c.
 */
#include "function_library_core_internal.h"

#include "evaluator_internal.h"
#include "json_internal.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/version.h"
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

/* Option definitions (F&O 4.0, section 1.7), read by the library's one
   options reader (src/function_library_options.c). */
#define SXML_JSON_OPTION(name_text, type_index, occurrence_value)             \
    {.option_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                    \
     .option_type = {.item_type = {.index = (type_index)},                    \
                     .occurrence = (occurrence_value)}}
#define SXML_JSON_BOOLEAN_OPTION(name_text)                                   \
    SXML_JSON_OPTION(name_text, SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE)
#define SXML_JSON_STRING_OPTION(name_text)                                    \
    SXML_JSON_OPTION(name_text, SXML_TYPE_XS_STRING, SXML_OCCURRENCE_EXACTLY_ONE)
/* item()? (the null option). */
#define SXML_JSON_ITEM_OPTION(name_text)                                      \
    SXML_JSON_OPTION(name_text, SXML_TYPE_ITEM, SXML_OCCURRENCE_ZERO_OR_ONE)
/* A function item or the empty sequence (the fallback option). */
#define SXML_JSON_FUNCTION_OPTION(name_text)                                  \
    SXML_JSON_OPTION(name_text, SXML_TYPE_FUNCTION, SXML_OCCURRENCE_ZERO_OR_ONE)

/* True when a boolean option is present and true. */
static bool json_option_is_true(const sxml_option_values *values, size_t option_index) {
    return sxml_function_library_option_is_true(
        (sxml_option_lookup){.values = values, .option_index = option_index});
}

/* The string value of a present string option; empty otherwise. */
static sxml_byte_view json_option_text(const sxml_option_values *values,
                                       size_t option_index) {
    return sxml_function_library_option_string(
               (sxml_option_lookup){.values = values, .option_index = option_index})
        .utf8;
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

static const sxml_option_definition parse_json_options[] = {
    SXML_JSON_BOOLEAN_OPTION("liberal"),
    SXML_JSON_STRING_OPTION("duplicates"),
    SXML_JSON_BOOLEAN_OPTION("escape"),
    SXML_JSON_FUNCTION_OPTION("fallback"),
    SXML_JSON_ITEM_OPTION("null"),
    SXML_JSON_STRING_OPTION("number-format"),
};

static const sxml_option_definition json_to_xml_options[] = {
    SXML_JSON_BOOLEAN_OPTION("liberal"),
    SXML_JSON_STRING_OPTION("duplicates"),
    SXML_JSON_BOOLEAN_OPTION("escape"),
    SXML_JSON_FUNCTION_OPTION("fallback"),
    /* Positions SXML_JSON_OPTION_NULL and SXML_JSON_OPTION_NUMBER_FORMAT are
       fn:parse-json's; these names never match a key. */
    SXML_JSON_STRING_OPTION(""),
    SXML_JSON_STRING_OPTION(""),
    SXML_JSON_BOOLEAN_OPTION("validate"),
};

/* What the shared JSON options become; is_xml says which function. */
typedef struct json_option_reading {
    bool is_xml;
    const sxml_option_values *values;
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
    const sxml_option_values *values = reading.values;
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
    sxml_option_values *values;
} json_parse_option_target;

/* Reads and interprets the options of fn:parse-json or fn:json-to-xml. */
static sxml_status json_read_parse_options(const sxml_function_call *call,
                                           json_parse_option_target target,
                                           sxml_json_parse_options *options) {
    const bool is_xml = target.is_xml;
    sxml_option_values *values = target.values;
    const sxml_option_request request = {
        .argument_index = 1,
        .definitions = is_xml ? (sxml_option_definition_view){json_to_xml_options,
                                                              sizeof(json_to_xml_options) /
                                                                  sizeof(json_to_xml_options[0])}
                              : (sxml_option_definition_view){parse_json_options,
                                                              sizeof(parse_json_options) /
                                                                  sizeof(parse_json_options[0])}};
    sxml_status status = sxml_function_library_read_options(call, &request, values);
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
    sxml_option_values values;
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
    sxml_function_library_release_options(&values);
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
    sxml_option_values values;
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
    sxml_function_library_release_options(&values);
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

static const sxml_option_definition xml_to_json_options[] = {
    SXML_JSON_BOOLEAN_OPTION("escape-solidus"),
    SXML_JSON_BOOLEAN_OPTION("indent"),
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
    sxml_option_values values;
    memset(&values, 0, sizeof(values));
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){
            .argument_index = 1,
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
    sxml_function_library_release_options(&values);
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

static const sxml_option_definition parse_xml_options[] = {
    SXML_JSON_STRING_OPTION("base-uri"),
    SXML_JSON_BOOLEAN_OPTION("dtd-validation"),
    SXML_JSON_STRING_OPTION("strip-space"),
    SXML_JSON_BOOLEAN_OPTION("trust-external"),
    SXML_JSON_BOOLEAN_OPTION("xinclude"),
    SXML_JSON_STRING_OPTION("xsd-validation"),
    SXML_JSON_BOOLEAN_OPTION("use-xsi-schema-location"),
    SXML_JSON_BOOLEAN_OPTION("retain-location"),
};

/* Checks the parse-xml options sxml cannot honour: DTD validation and
   XInclude (FODC0013), XSD validation (FODC0009). An unknown strip-space
   value is XPTY0004, since the option's type is an enumeration. Writes
   whether whitespace-only text nodes are stripped. */
typedef struct xml_space_handling {
    bool strips_space;
} xml_space_handling;

static sxml_status xml_check_options(const sxml_function_call *call,
                                     const sxml_option_values *values,
                                     xml_space_handling *handling) {
    handling->strips_space = false;
    if (json_option_is_true(values, SXML_XML_OPTION_DTD_VALIDATION) ||
        json_option_is_true(values, SXML_XML_OPTION_XINCLUDE)) {
        return sxml_error_record_report(call->error_record, &xml_unsupported_option_error);
    }
    if (values->is_present[SXML_XML_OPTION_STRIP_SPACE]) {
        const sxml_byte_view strip_space = json_option_text(values, SXML_XML_OPTION_STRIP_SPACE);
        handling->strips_space = json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("all"));
        if (!handling->strips_space &&
            !json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("none")) &&
            !json_text_is(strip_space, SXML_BYTE_VIEW_FROM_LITERAL("conditional"))) {
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

/* ------------------------------------------------ Whitespace stripping */

/*
 * A tree builder that forwards every event to another but drops
 * whitespace-only text nodes outside the scope of xml:space="preserve"
 * (F&O 4.0, fn:parse-xml, strip-space="all"). The xml:space scope of each
 * open element is kept on an explicit stack in heap memory, one byte per
 * level.
 */
typedef struct space_stripper {
    sxml_tree_builder target;
    sxml_item_heap *item_heap;
    bool knows_space_name;
    sxml_name_identifier space_name;
    /* preserves[level]: 1 when level is under xml:space="preserve". */
    uint8_t *preserves;
    size_t depth;
    size_t capacity;
} space_stripper;

static bool space_stripper_preserves(const space_stripper *stripper) {
    return stripper->depth > 0 && stripper->preserves[stripper->depth - 1] != 0;
}

static sxml_status strip_start_document(void *state, const sxml_document_start_event *event) {
    const space_stripper *stripper = state;
    return stripper->target.operations->start_document(stripper->target.state, event);
}

static sxml_status strip_end_document(void *state) {
    const space_stripper *stripper = state;
    return stripper->target.operations->end_document(stripper->target.state);
}

static sxml_status strip_start_element(void *state, const sxml_element_start_event *event) {
    space_stripper *stripper = state;
    if (stripper->depth == stripper->capacity) {
        const size_t capacity = stripper->capacity == 0 ? 64 : stripper->capacity * 2;
        void *memory = NULL;
        sxml_status status = sxml_item_heap_allocate_bytes(
            stripper->item_heap, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (stripper->depth > 0) {
            memcpy(memory, stripper->preserves, stripper->depth);
        }
        sxml_item_heap_release_bytes(
            stripper->item_heap,
            (sxml_heap_bytes){.memory = stripper->preserves, .byte_count = stripper->capacity});
        stripper->preserves = memory;
        stripper->capacity = capacity;
    }
    stripper->preserves[stripper->depth] = space_stripper_preserves(stripper) ? 1U : 0U;
    stripper->depth += 1;
    return stripper->target.operations->start_element(stripper->target.state, event);
}

static sxml_status strip_add_namespace_binding(void *state,
                                               const sxml_namespace_binding *binding) {
    const space_stripper *stripper = state;
    return stripper->target.operations->add_namespace_binding(stripper->target.state, binding);
}

static sxml_status strip_add_attribute(void *state, const sxml_attribute_event *event) {
    space_stripper *stripper = state;
    if (stripper->knows_space_name && stripper->depth > 0 &&
        event->attribute_name.index == stripper->space_name.index) {
        if (json_text_is(event->attribute_value, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
            stripper->preserves[stripper->depth - 1] = 1U;
        } else if (json_text_is(event->attribute_value, SXML_BYTE_VIEW_FROM_LITERAL("default"))) {
            stripper->preserves[stripper->depth - 1] = 0U;
        }
    }
    return stripper->target.operations->add_attribute(stripper->target.state, event);
}

static sxml_status strip_end_element(void *state) {
    space_stripper *stripper = state;
    if (stripper->depth > 0) {
        stripper->depth -= 1;
    }
    return stripper->target.operations->end_element(stripper->target.state);
}

static sxml_status strip_add_text(void *state, const sxml_text_event *event) {
    const space_stripper *stripper = state;
    bool is_whitespace = true;
    for (size_t byte_index = 0; is_whitespace && byte_index < event->text.byte_count;
         byte_index += 1) {
        const uint8_t byte = event->text.bytes[byte_index];
        is_whitespace = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
    }
    if (is_whitespace && !space_stripper_preserves(stripper)) {
        return SXML_STATUS_OK;
    }
    return stripper->target.operations->add_text(stripper->target.state, event);
}

static sxml_status strip_add_comment(void *state, const sxml_comment_event *event) {
    const space_stripper *stripper = state;
    return stripper->target.operations->add_comment(stripper->target.state, event);
}

static sxml_status strip_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    const space_stripper *stripper = state;
    return stripper->target.operations->add_processing_instruction(stripper->target.state, event);
}

static sxml_status strip_add_unparsed_entity(void *state, const sxml_unparsed_entity *entity) {
    const space_stripper *stripper = state;
    return stripper->target.operations->add_unparsed_entity(stripper->target.state, entity);
}

static const sxml_tree_builder_operations space_stripper_operations = {
    .start_document = strip_start_document,
    .end_document = strip_end_document,
    .start_element = strip_start_element,
    .add_namespace_binding = strip_add_namespace_binding,
    .add_attribute = strip_add_attribute,
    .end_element = strip_end_element,
    .add_text = strip_add_text,
    .add_comment = strip_add_comment,
    .add_processing_instruction = strip_add_processing_instruction,
    .add_unparsed_entity = strip_add_unparsed_entity,
};

/* ---------------------------------------------------- New documents */

/* The children of parent copied into a new document node with base_uri,
   whitespace-only text stripped when strips_space. */
typedef struct document_copy_request {
    sxml_node_reference parent;
    sxml_byte_view base_uri;
    bool strips_space;
} document_copy_request;

static sxml_status json_send_copy(const document_copy_request *request,
                                  sxml_tree_builder builder) {
    sxml_status status = builder.operations->start_document(
        builder.state,
        &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                     .base_uri = request->base_uri,
                                     .location = {.source_name = {.bytes = NULL,
                                                                  .byte_count = 0},
                                                  .line_number = 0,
                                                  .column_number = 0,
                                                  .byte_offset = 0}});
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(request->parent, &description);
    }
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        const sxml_node_reference child_node = {
            .tree = request->parent.tree, .node_index = child, .namespace_number = 0};
        status = sxml_tree_builder_copy_node(builder,
                                             &(sxml_copy_request){.node = child_node,
                                                                  .preserves_namespaces = true,
                                                                  .inherits_namespaces = true,
                                                                  .preserves_type_annotations =
                                                                      false});
        sxml_node_description child_description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(child_node, &child_description);
        }
        has_child = status == SXML_STATUS_OK && child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    return status;
}

/* Makes the new document of request, owned, in *result. */
static sxml_status json_copy_into_document(const sxml_function_call *call,
                                           const document_copy_request *request,
                                           sxml_sequence *result) {
    const sxml_allocator tree_allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        tree_allocator,
        &(sxml_arena_tree_configuration){
            .allocator = tree_allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = call->item_heap,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = call->error_record},
        &arena_tree);
    space_stripper stripper = {
        .target = {.state = NULL, .operations = NULL},
        .item_heap = call->item_heap,
        .knows_space_name = sxml_name_pool_find_name(
            sxml_item_heap_get_name_pool(call->item_heap),
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("space")},
            &stripper.space_name),
        .preserves = NULL,
        .depth = 0,
        .capacity = 0};
    if (status == SXML_STATUS_OK) {
        stripper.target = sxml_arena_tree_as_tree_builder(arena_tree);
        status = json_send_copy(request,
                                request->strips_space
                                    ? (sxml_tree_builder){.state = &stripper,
                                                          .operations = &space_stripper_operations}
                                    : stripper.target);
    }
    sxml_item_heap_release_bytes(call->item_heap, (sxml_heap_bytes){.memory = stripper.preserves,
                                                                   .byte_count = stripper.capacity});
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Parsing XML */

/* What a parse function reads: the input bytes and its options. */
typedef struct xml_input {
    sxml_byte_view xml_bytes;
    sxml_byte_view base_uri;
    bool strips_space;
} xml_input;

/* Reads the input and the options of fn:parse-xml or
   fn:parse-xml-fragment into *input; *values keeps the option values the
   input borrows. */
static sxml_status xml_read_input(const sxml_function_call *call, sxml_option_values *values,
                                  xml_input *input) {
    const sxml_sequence value = json_argument(call, 0);
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(value.content, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
        return sxml_error_record_report(call->error_record, &xml_wrong_input_error);
    }
    status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {parse_xml_options,
                                               sizeof(parse_xml_options) /
                                                   sizeof(parse_xml_options[0])}},
        values);
    xml_space_handling handling = {.strips_space = false};
    if (status == SXML_STATUS_OK) {
        status = xml_check_options(call, values, &handling);
    }
    if (status == SXML_STATUS_OK) {
        *input = (xml_input){
            .xml_bytes = datum.datum_kind == SXML_DATUM_KIND_STRING ? datum.datum_details.string.utf8
                                                                    : datum.datum_details.binary,
            /* The base-uri option, else the static base URI (F&O 4.0,
               fn:parse-xml; D88). */
            .base_uri = values->is_present[SXML_XML_OPTION_BASE_URI]
                            ? json_option_text(values, SXML_XML_OPTION_BASE_URI)
                            : sxml_dynamic_context_get_static_base_uri(call->dynamic_context),
            .strips_space = handling.strips_space};
    }
    return status;
}

/* Parses bytes as a document through the dynamic context's loader and,
   when stripping, copies it into a stripped document. */
static sxml_status xml_parse_document(const sxml_function_call *call, const xml_input *input,
                                      sxml_sequence *result) {
    sxml_item document_node = {.representation = 0};
    sxml_status status = sxml_dynamic_context_parse_document(
        call->dynamic_context,
        &(sxml_document_parse_request){.xml_bytes = input->xml_bytes,
                                       .base_uri = input->base_uri,
                                       .error_record = call->error_record},
        &document_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!input->strips_space) {
        *result = sxml_sequence_from_item(document_node);
        return SXML_STATUS_OK;
    }
    sxml_node_reference document;
    status = sxml_item_get_node(document_node, &document);
    if (status == SXML_STATUS_OK) {
        status = json_copy_into_document(call,
                                         &(document_copy_request){.parent = document,
                                                                  .base_uri = input->base_uri,
                                                                  .strips_space = true},
                                         result);
    }
    sxml_item_release(document_node);
    return status;
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
    if (json_sequence_is_empty(json_argument(call, 0))) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_option_values values = {.is_present = {false}};
    xml_input input = {.xml_bytes = {.bytes = NULL, .byte_count = 0},
                       .base_uri = {.bytes = NULL, .byte_count = 0},
                       .strips_space = false};
    sxml_status status = xml_read_input(call, &values, &input);
    if (status == SXML_STATUS_OK) {
        status = xml_parse_document(call, &input, result);
    }
    sxml_function_library_release_options(&values);
    return status;
}

static const sxml_static_error fragment_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODC0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The text declaration of the fragment needs an encoding and has no standalone "
        "(XML 1.0, [77] TextDecl; F&O 4.0, fn:parse-xml-fragment)"),
};

/* The element the fragment is parsed inside; its end tag cannot be
   closed early without leaving text after the document element. */
#define SXML_FRAGMENT_START "<sxml-fragment>"
#define SXML_FRAGMENT_END "</sxml-fragment>"

static bool bytes_contain(sxml_byte_view text, sxml_byte_view part) {
    for (size_t byte_index = 0; part.byte_count <= text.byte_count &&
                                byte_index <= text.byte_count - part.byte_count;
         byte_index += 1) {
        if (memcmp(text.bytes + byte_index, part.bytes, part.byte_count) == 0) {
            return true;
        }
    }
    return false;
}

/* The entity's content after a UTF-8 byte order mark and a text
   declaration ([78] extParsedEnt ::= TextDecl? content), FODC0006 for a
   text declaration without an encoding or with standalone. */
static sxml_status skip_text_declaration(const sxml_function_call *call, sxml_byte_view *content) {
    sxml_byte_view text = *content;
    if (text.byte_count >= 3 && text.bytes[0] == 0xEF && text.bytes[1] == 0xBB &&
        text.bytes[2] == 0xBF) {
        text = (sxml_byte_view){.bytes = text.bytes + 3, .byte_count = text.byte_count - 3};
    }
    const sxml_byte_view opening = SXML_BYTE_VIEW_FROM_LITERAL("<?xml");
    if (text.byte_count > opening.byte_count &&
        memcmp(text.bytes, opening.bytes, opening.byte_count) == 0 &&
        (text.bytes[opening.byte_count] == ' ' || text.bytes[opening.byte_count] == '\t' ||
         text.bytes[opening.byte_count] == '\n' || text.bytes[opening.byte_count] == '\r')) {
        size_t end = opening.byte_count;
        while (end + 1 < text.byte_count && !(text.bytes[end] == '?' && text.bytes[end + 1] == '>')) {
            end += 1;
        }
        const sxml_byte_view declaration = {.bytes = text.bytes, .byte_count = end};
        if (end + 1 >= text.byte_count ||
            !bytes_contain(declaration, SXML_BYTE_VIEW_FROM_LITERAL("encoding")) ||
            bytes_contain(declaration, SXML_BYTE_VIEW_FROM_LITERAL("standalone"))) {
            return sxml_error_record_report(call->error_record, &fragment_error);
        }
        text = (sxml_byte_view){.bytes = text.bytes + end + 2, .byte_count = text.byte_count - end - 2};
    }
    *content = text;
    return SXML_STATUS_OK;
}

/*
 * fn:parse-xml-fragment($value, $options := {}) (F&O 4.0, section
 * 17.2.2): the external general parsed entity is parsed as the content of
 * an element of a document made around it (so the parser and its limits
 * are the document loader's), and that element's children are copied into
 * a new document node. Binary input is taken as UTF-8.
 */
static sxml_status call_parse_xml_fragment(const sxml_function_call *call,
                                           sxml_sequence *result) {
    if (json_sequence_is_empty(json_argument(call, 0))) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_option_values values = {.is_present = {false}};
    xml_input input = {.xml_bytes = {.bytes = NULL, .byte_count = 0},
                       .base_uri = {.bytes = NULL, .byte_count = 0},
                       .strips_space = false};
    sxml_status status = xml_read_input(call, &values, &input);
    sxml_byte_view content = input.xml_bytes;
    if (status == SXML_STATUS_OK) {
        status = skip_text_declaration(call, &content);
    }
    sxml_library_text wrapped = {.item_heap = call->item_heap};
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&wrapped,
                                                   SXML_BYTE_VIEW_FROM_LITERAL(SXML_FRAGMENT_START));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&wrapped, content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_text_append(&wrapped,
                                                   SXML_BYTE_VIEW_FROM_LITERAL(SXML_FRAGMENT_END));
    }
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_dynamic_context_parse_document(
            call->dynamic_context,
            &(sxml_document_parse_request){.xml_bytes = sxml_function_library_text_view(&wrapped),
                                           .base_uri = input.base_uri,
                                           .error_record = call->error_record},
            &document_node);
    }
    sxml_function_library_text_release(&wrapped);
    sxml_node_reference document;
    sxml_node_description description;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(document_node, &document);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(document, &description);
    }
    if (status == SXML_STATUS_OK) {
        status = json_copy_into_document(
            call,
            &(document_copy_request){.parent = {.tree = document.tree,
                                                .node_index = description.first_child,
                                                .namespace_number = 0},
                                     .base_uri = input.base_uri,
                                     .strips_space = input.strips_space},
            result);
    }
    sxml_item_release(document_node);
    sxml_function_library_release_options(&values);
    return status;
}

/* ------------------------------------------------- fn:system-properties */

/* One built-in system property (F&O 4.0, section 17.1.9): a no-namespace
   name and its value. */
typedef struct system_property {
    const char *property_name;
    sxml_atomic_datum value;
    sxml_builtin_type value_type;
} system_property;

/* sxml is not schema-aware, reads no typed data, has neither XInclude, DTD
   validation, invisible XML, dynamic XQuery or XSLT (yet) nor the EXPath
   modules. */
#define SXML_BOOLEAN_PROPERTY(name_text, truth)                               \
    {name_text,                                                               \
     {.datum_kind = SXML_DATUM_KIND_BOOLEAN, .datum_details = {.boolean = (truth)}}, \
     SXML_TYPE_XS_BOOLEAN}

static const system_property system_properties[] = {
    {"xpath-version",
     {.datum_kind = SXML_DATUM_KIND_DECIMAL,
      .datum_details = {.decimal_text = SXML_BYTE_VIEW_INITIALIZER("4.0")}},
     SXML_TYPE_XS_DECIMAL},
    {"xsd-version",
     {.datum_kind = SXML_DATUM_KIND_DECIMAL,
      .datum_details = {.decimal_text = SXML_BYTE_VIEW_INITIALIZER("1.1")}},
     SXML_TYPE_XS_DECIMAL},
    {"product-name",
     {.datum_kind = SXML_DATUM_KIND_STRING,
      .datum_details = {.string = {.utf8 = SXML_BYTE_VIEW_INITIALIZER("sxml"),
                                   .codepoint_count = 4,
                                   .is_ascii = true}}},
     SXML_TYPE_XS_STRING},
    /* The text is sxml_version_text(), filled in by the body. */
    {"product-version",
     {.datum_kind = SXML_DATUM_KIND_STRING,
      .datum_details = {.string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                                   .codepoint_count = 0,
                                   .is_ascii = true}}},
     SXML_TYPE_XS_STRING},
    SXML_BOOLEAN_PROPERTY("schema-aware", false),
    SXML_BOOLEAN_PROPERTY("accepts-typed-data", false),
    SXML_BOOLEAN_PROPERTY("supports-xinclude", false),
    SXML_BOOLEAN_PROPERTY("supports-dtd-validation", false),
    SXML_BOOLEAN_PROPERTY("supports-dtd-attribute-typing", false),
    SXML_BOOLEAN_PROPERTY("supports-invisible-xml", false),
    SXML_BOOLEAN_PROPERTY("supports-dynamic-xquery", false),
    SXML_BOOLEAN_PROPERTY("supports-dynamic-xslt", false),
    SXML_BOOLEAN_PROPERTY("supports-binary-library", false),
    SXML_BOOLEAN_PROPERTY("supports-file-library", false),
};

#define SXML_SYSTEM_PROPERTY_COUNT (sizeof(system_properties) / sizeof(system_properties[0]))

/* fn:system-properties() as map(xs:QName, xs:anyAtomicType) (F&O 4.0,
   section 17.1.9): the built-in properties, keyed by no-namespace QNames. */
static sxml_status call_system_properties(const sxml_function_call *call,
                                          sxml_sequence *result) {
    sxml_map_entry entries[SXML_SYSTEM_PROPERTY_COUNT];
    size_t entry_count = 0;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && entry_count < SXML_SYSTEM_PROPERTY_COUNT) {
        const system_property *property = &system_properties[entry_count];
        sxml_atomic_datum datum = property->value;
        if (datum.datum_kind == SXML_DATUM_KIND_STRING && datum.datum_details.string.utf8.bytes == NULL) {
            datum.datum_details.string.utf8 = sxml_byte_view_from_c_string(sxml_version_text());
            datum.datum_details.string.codepoint_count = datum.datum_details.string.utf8.byte_count;
        }
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = sxml_byte_view_from_c_string(property->property_name)},
            &name);
        sxml_item key = {.representation = 0};
        sxml_item value = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                call->item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                    .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                              .datum_details = {.qname = {.expanded_name = name,
                                                          .namespace_prefix = {.index = 0}}}}},
                &key);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                call->item_heap,
                &(sxml_atomic_item_request){.type = sxml_builtin_type_identifier(property->value_type),
                                            .datum = datum},
                &value);
        }
        if (status != SXML_STATUS_OK) {
            sxml_item_release(key);
            break;
        }
        entries[entry_count] = (sxml_map_entry){.key = key, .value = sxml_sequence_from_item(value)};
        entry_count += 1;
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = entries, .map_entry_count = entry_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_REJECT,
                                .error_record = call->error_record},
            &map);
    }
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        sxml_item_release(entries[entry_index].key);
        sxml_sequence_release(entries[entry_index].value);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
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
    JSON_ROW("parse-xml-fragment", parse_xml_parameters, "document-node()?",
             SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
             call_parse_xml_fragment),
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("system-properties"),
     .parameters = {NULL, 0},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("map(xs:QName, xs:anyAtomicType)"),
     .call = call_system_properties,
     .resumable_body = NULL},
};

const sxml_native_function_view sxml_function_library_json = {
    .native_functions = json_functions,
    .native_function_count = sizeof(json_functions) / sizeof(json_functions[0]),
};
