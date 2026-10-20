/*
 * sxml:parse-code and sxml:code-languages (namespace urn:sxml; packet
 * 7.22, D285): the source code feature's library functions
 * (src/source_code_internal.h, D191). Not a library part of the
 * registry: its row view is sxml_source_code_library_functions, which the
 * processor serves only when its configuration names
 * sxml_source_code_feature(); without the feature both functions are
 * absent (XPST0017).
 *
 * Golden file: src/function_library_ixml.c (a document built in an arena
 * tree of the processor from a parse, steps charged to the call).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "source_code_internal.h"
#include "xdm_internal.h"

/* The options of sxml:parse-code. */
enum {
    SXML_SOURCE_CODE_OPTION_HIGHLIGHTS,
    SXML_SOURCE_CODE_OPTION_POSITIONS,
    SXML_SOURCE_CODE_OPTION_COUNT,
};

static const sxml_option_definition source_code_options[SXML_SOURCE_CODE_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("highlights", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("positions", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
};

/* Fills call->error_record with sxml:unknown-language and returns
   SXML_STATUS_DYNAMIC_ERROR. */
static sxml_status report_unknown_language(const sxml_function_call *call) {
    if (call->error_record != NULL) {
        *call->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:sxml"),
                     .local_name = SXML_BYTE_VIEW_FROM_LITERAL("unknown-language"),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("sxml")},
            .value = NULL,
            .diagnostic = {.description = SXML_BYTE_VIEW_FROM_LITERAL(
                               "the language is not one that sxml:parse-code knows; "
                               "sxml:code-languages() lists them"),
                           .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                        .line_number = 0,
                                        .column_number = 0,
                                        .byte_offset = 0}}};
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* Charges the parse's steps to the call (the evaluation's step limit). */
static sxml_status charge_call_steps(void *state, uint64_t step_count) {
    return sxml_function_library_charge_steps(state, step_count);
}

static const sxml_source_code_step_meter_operations call_meter_operations = {
    .charge = charge_call_steps,
};

/* What one call parses. */
typedef struct code_request {
    const sxml_source_code_language *language;
    sxml_byte_view source_text;
    bool has_highlights;
    bool has_positions;
} code_request;

/* Builds the document of request in an arena tree of the call's
   processor. */
static sxml_status build_document(const sxml_function_call *call, const code_request *request,
                                  sxml_item *document_node) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    if (environment == NULL || environment->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = environment->processor;
    const sxml_allocator allocator = processor->allocator;
    sxml_arena_tree *arena_tree = NULL;
    /* The tree destroys itself with its last reference (D39). */
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){
            .allocator = allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = processor->limits.xml_limits.maximum_depth,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = false,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_source_code_parse(&(sxml_source_code_request){
            .language = request->language,
            .source_text = request->source_text,
            .has_highlights = request->has_highlights,
            .has_positions = request->has_positions,
            .builder = sxml_arena_tree_as_tree_builder(arena_tree),
            .name_pool = processor->name_pool,
            .allocator = sxml_item_heap_as_allocator(call->item_heap),
            .step_meter = {.state = (void *)call, .operations = &call_meter_operations}});
    }
    sxml_tree *finished = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &finished);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make((sxml_node_reference){
        .tree = finished, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(finished);
    return SXML_STATUS_OK;
}

/* Reads the options of a call into request. */
static sxml_status read_code_options(const sxml_function_call *call, code_request *request) {
    sxml_option_values values = {.is_present = {false}};
    const sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 2,
                               .definitions = {source_code_options, SXML_SOURCE_CODE_OPTION_COUNT}},
        &values);
    if (status == SXML_STATUS_OK) {
        request->has_highlights =
            !values.is_present[SXML_SOURCE_CODE_OPTION_HIGHLIGHTS] ||
            sxml_function_library_option_is_true((sxml_option_lookup){
                .values = &values, .option_index = SXML_SOURCE_CODE_OPTION_HIGHLIGHTS});
        request->has_positions = sxml_function_library_option_is_true((sxml_option_lookup){
            .values = &values, .option_index = SXML_SOURCE_CODE_OPTION_POSITIONS});
    }
    sxml_function_library_release_options(&values);
    return status;
}

/* sxml:parse-code($source as xs:string?, $language as xs:string,
   $options as map(*)? := {}) as document-node()? (D285). */
static sxml_status call_parse_code(const sxml_function_call *call, sxml_sequence *result) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item source = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &source);
    sxml_string language_name;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_string_argument(call, 1, &language_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    code_request request = {
        .language = sxml_source_code_find_language(language_name.utf8),
        .source_text = {.bytes = NULL, .byte_count = 0},
        .has_highlights = true,
        .has_positions = false};
    if (request.language == NULL) {
        return report_unknown_language(call);
    }
    if (!source.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_string source_text;
    status = sxml_function_library_read_string_argument(call, 0, &source_text);
    if (status == SXML_STATUS_OK) {
        request.source_text = source_text.utf8;
        status = read_code_options(call, &request);
    }
    sxml_item document_node = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    if (status == SXML_STATUS_OK) {
        status = build_document(call, &request, &document_node);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

/* sxml:code-languages() as xs:string*: the names of the languages, in
   name order. */
static sxml_status call_code_languages(const sxml_function_call *call, sxml_sequence *result) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(call->item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = call->item_heap}, &builder);
    for (size_t language_index = 0;
         status == SXML_STATUS_OK && language_index < sxml_source_code_languages.language_count;
         language_index += 1) {
        sxml_sequence name = sxml_sequence_empty();
        status = sxml_function_library_make_string(
            call->item_heap, sxml_source_code_languages.languages[language_index].language_name,
            &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_sequence(builder, name, call->error_record);
        }
        sxml_sequence_release(name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, result);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

static const sxml_function_parameter parse_code_parameters[] = {
    SXML_REQUIRED_PARAMETER("source", "xs:string?"),
    SXML_REQUIRED_PARAMETER("language", "xs:string"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

/* A row in the urn:sxml namespace. */
#define SXML_SOURCE_CODE_ROW(name_text, parameter_array, parameter_count, result_text, body)      \
    {.function_namespace = SXML_NAMESPACE_SXML,                                                 \
     .properties = SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,                                    \
     .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                       \
     .parameters = {(parameter_array), (parameter_count)},                                      \
     .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text),                                    \
     .call = (body),                                                                            \
     .resumable_body = NULL}

static const sxml_native_function source_code_functions[] = {
    SXML_SOURCE_CODE_ROW("parse-code", parse_code_parameters,
                         sizeof(parse_code_parameters) / sizeof(parse_code_parameters[0]),
                         "document-node()?", call_parse_code),
    SXML_SOURCE_CODE_ROW("code-languages", NULL, 0, "xs:string*", call_code_languages),
};

const sxml_native_function_view sxml_source_code_library_functions = {
    .native_functions = source_code_functions,
    .native_function_count = sizeof(source_code_functions) / sizeof(source_code_functions[0]),
};

const sxml_optional_feature_definition sxml_source_code_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_SOURCE_CODE,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("source-code"),
    .operations = &sxml_source_code_library_functions,
    .finalize_state = NULL,
    .library_functions = &sxml_source_code_library_functions,
};

sxml_optional_feature sxml_source_code_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_source_code_feature_definition};
}
