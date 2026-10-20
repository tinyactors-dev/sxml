/*
 * The full module's XSD entry points (include/sxml/schema.h): compile a
 * schema once from schema documents, validate documents and elements with
 * it, and hand every report of a compilation or validation to the host
 * as side results. Linked into the full links only (D189's two links).
 *
 * Golden files: sxml_host_xquery.c, and apps/sxml/command_validate.c
 * (the same public API for the CLI: a report sink that keeps every error,
 * the outcome of a validator).
 */
#include "sxml_host_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* As sxml validate: entity expansion is bounded, nesting is not. */
enum { SXML_HOST_VALIDATION_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* A schema handle: made empty, compiled once; and the reports of its last
   compilation or validation. */
struct sxml_host_schema {
    sxml_host_processor *owner;
    sxml_schema *schema;
    sxml_host_side_results side_results;
};

static sxml_host_side_result_target sxml_host_schema_target(
    sxml_host_schema *schema) {
    return (sxml_host_side_result_target){.processor = schema->owner,
                                                .side_results = &schema->side_results};
}

sxml_host_schema *sxml_host_schema_create(sxml_host_processor *processor) {
    sxml_host_schema *schema = sxml_host_allocate_bytes(
        processor->allocator, sizeof(sxml_host_schema), alignof(sxml_host_schema));
    if (schema != NULL) {
        *schema = (sxml_host_schema){
            .owner = processor,
            .schema = NULL,
            .side_results = {
                .side_results = NULL, .side_result_count = 0, .side_result_capacity = 0}};
    }
    return schema;
}

void sxml_host_schema_destroy(sxml_host_schema *schema) {
    if (schema == NULL) {
        return;
    }
    const sxml_host_side_result_target target = sxml_host_schema_target(schema);
    sxml_host_side_results_finalize(&target);
    sxml_schema_destroy(schema->schema);
    sxml_allocator_release(schema->owner->allocator, schema);
}

/* ----------------------------------------------------------------- Reports */

/* Each report as a SCHEMA_REPORT side result; the compilation or
   validation goes on (schema.h, "report sink"). */
static sxml_status sxml_host_receive_report(void *state, const sxml_schema_report *report) {
    return sxml_host_side_results_add(
        state, &(sxml_host_side_result){
                   .side_result_kind = SXML_HOST_SIDE_RESULT_KIND_SCHEMA_REPORT,
                   .uri = report->location.source_name,
                   .text = report->description,
                   .code = report->code.local_name,
                   .first_value = NULL,
                   .second_value = NULL,
                   .line_number = report->location.line_number,
                   .column_number = report->location.column_number,
                   .is_terminating = 0});
}

static const sxml_schema_report_sink_operations sxml_host_report_operations = {
    .receive = sxml_host_receive_report,
};

/* An error that stopped a compilation or validation before the sink saw
   any report (a document that is not a schema document at all, schema.h):
   a report too, so the list is never empty for a failure with a code. */
static sxml_status sxml_host_report_unseen_error(sxml_host_schema *schema) {
    const sxml_error_record *record = &schema->owner->error_record;
    if (schema->side_results.side_result_count > 0 || record->code.local_name.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_host_side_result_target target = sxml_host_schema_target(schema);
    return sxml_host_receive_report(
        (void *)&target, &(sxml_schema_report){.code = record->code,
                                               .description = record->diagnostic.description,
                                               .location = record->diagnostic.location});
}

/* ------------------------------------------------------------ Compilation */

/*
 * Compiles the schema documents of request (XSD 1.1 Part 1, 4.2, 4.3) into
 * schema. Every report goes to the side results written into request; the
 * first is also the processor's error. Returns the status.
 */
uint32_t sxml_host_schema_compile(sxml_host_schema *schema,
                                  sxml_host_schema_compile_request *request) {
    sxml_host_processor *processor = schema->owner;
    sxml_host_begin(processor);
    sxml_host_side_result_target target = sxml_host_schema_target(schema);
    sxml_host_side_results_clear(&target);
    sxml_status status = schema->schema == NULL ? SXML_STATUS_OK : SXML_STATUS_INVALID_ARGUMENT;
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_host_allocate_array(
            processor->allocator,
            (sxml_host_array_request){
                .element = {.byte_count = sizeof(sxml_schema_document),
                            .alignment = alignof(sxml_schema_document)},
                .element_count = request->document_count},
            &memory);
    }
    sxml_schema_document *documents = memory;
    for (size_t document_index = 0; status == SXML_STATUS_OK &&
                                    document_index < request->document_count;
         document_index += 1) {
        const sxml_host_schema_document *source = &request->documents[document_index];
        documents[document_index] = (sxml_schema_document){
            .document_source = source->text.bytes != NULL ? SXML_SCHEMA_DOCUMENT_SOURCE_TEXT
                                                          : SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
            .document_text = source->text,
            .document_node = {.representation = 0},
            .document_uri = source->uri,
            .checks_target_namespace = false,
            .target_namespace_uri = sxml_host_no_text};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_create(
            processor->allocator,
            &(sxml_schema_configuration){
                .processor = processor->processor,
                .schema_documents = {.schema_documents = documents,
                                     .schema_document_count = request->document_count},
                .base_uri = request->base_uri,
                .report_sink = {.state = &target, .operations = &sxml_host_report_operations},
                .maximum_content_model_state_count = 0,
                .error_record = &processor->error_record},
            &schema->schema);
    }
    sxml_allocator_release(processor->allocator, documents);
    if (status != SXML_STATUS_OK) {
        const sxml_status reported = sxml_host_report_unseen_error(schema);
        (void)reported;
        sxml_host_fail(processor, status);
    }
    request->side_results = sxml_host_side_results_view(&schema->side_results);
    return (uint32_t)status;
}

/* ------------------------------------------------------------- Validation */

/* What one validation holds while it runs. */
typedef struct sxml_host_validation {
    sxml_arena_tree *arena_tree;
    sxml_schema_validator *validator;
    /* dm:base-uri of an element to validate, the base URI of its copy. */
    sxml_sequence base_uri;
    /* Where the reports go: the schema's side results. */
    sxml_host_side_result_target target;
} sxml_host_validation;

/* The text of the xs:anyURI in validation->base_uri; empty when absent. */
static sxml_byte_view sxml_host_base_uri_text(const sxml_host_validation *validation) {
    sxml_atomic_datum datum;
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_item(validation->base_uri,
                               (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return sxml_host_no_text;
    }
    return datum.datum_details.string.utf8;
}

/* The type of request->governing_type in schema; SXML_TYPE_NONE for none,
   SXML_STATUS_INVALID_ARGUMENT for a name the schema has no type of. */
static sxml_status sxml_host_governing_type(const sxml_host_schema *schema,
                                            const sxml_host_validate_request *request,
                                            sxml_type_identifier *type) {
    *type = sxml_builtin_type_identifier(SXML_TYPE_NONE);
    sxml_name_identifier name = {.index = 0};
    const sxml_status status =
        sxml_host_intern_name(schema->owner, &request->governing_type, &name);
    if (status != SXML_STATUS_OK || name.index == 0) {
        return status;
    }
    if (!sxml_schema_find_type(schema->schema, name, type)) {
        schema->owner->error_record.diagnostic.description =
            SXML_BYTE_VIEW_FROM_LITERAL("The schema has no type of that name");
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* The node of request->node, when it is a document or element node;
   SXML_STATUS_INVALID_ARGUMENT otherwise. */
static sxml_status sxml_host_node_to_validate(const sxml_host_validate_request *request,
                                              sxml_node_reference *node,
                                              sxml_node_kind *node_kind) {
    sxml_item item = {.representation = 0};
    sxml_status status =
        sxml_sequence_get_item(request->node->sequence,
                               (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(item, node);
    }
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(*node, &description);
    }
    if (status != SXML_STATUS_OK || (description.node_kind != SXML_NODE_KIND_DOCUMENT &&
                                     description.node_kind != SXML_NODE_KIND_ELEMENT)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *node_kind = description.node_kind;
    return SXML_STATUS_OK;
}

/* Makes the arena tree the validated copy goes to and the validator that
   feeds it, as sxml_schema_validate_node does, keeping the validator for
   its outcome (valid, invalid or not known; Part 1, 3.3.5.1), which that
   function does not give. */
static sxml_status sxml_host_start_validation(sxml_host_schema *schema,
                                              const sxml_host_validate_request *request,
                                              sxml_host_validation *validation) {
    sxml_host_processor *processor = schema->owner;
    sxml_type_identifier governing_type = sxml_builtin_type_identifier(SXML_TYPE_NONE);
    sxml_status status = sxml_host_governing_type(schema, request, &governing_type);
    if (status == SXML_STATUS_OK) {
        /* As sxml_processor_parse_document's trees: destroyed with the last
           reference (D39). */
        status = sxml_arena_tree_create(
            processor->allocator,
            &(sxml_arena_tree_configuration){
                .allocator = processor->allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = sxml_processor_get_item_heap(processor->processor),
                .maximum_node_count = 0,
                .maximum_depth = 0,
                .maximum_text_byte_count = 0,
                .keeps_locations = request->node == NULL,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL,
                .base_uri = sxml_host_base_uri_text(validation)},
            &validation->arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_create(
            processor->allocator,
            &(sxml_schema_validator_configuration){
                .schema = schema->schema,
                .validation_mode = (sxml_validation_mode)request->validation_mode,
                .governing_type = governing_type,
                .result_builder = sxml_arena_tree_as_tree_builder(validation->arena_tree),
                .report_sink = {.state = &validation->target,
                                .operations = &sxml_host_report_operations},
                .error_record = &processor->error_record},
            &validation->validator);
    }
    return status;
}

/* Sends the events of request->node through the validator: a copy, so
   XSD assesses the infoset, not earlier annotations (schema.h). */
static sxml_status sxml_host_validate_copy(sxml_host_schema *schema,
                                           const sxml_host_validate_request *request,
                                           sxml_host_validation *validation) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_node_kind node_kind = SXML_NODE_KIND_DOCUMENT;
    sxml_status status = sxml_host_node_to_validate(request, &node, &node_kind);
    if (status == SXML_STATUS_OK && node_kind == SXML_NODE_KIND_ELEMENT) {
        status = sxml_node_make_base_uri(
            sxml_processor_get_item_heap(schema->owner->processor),
            &(sxml_node_value_request){.node = node, .error_record = NULL}, &validation->base_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_host_start_validation(schema, request, validation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(
            sxml_schema_validator_as_tree_builder(validation->validator),
            &(sxml_copy_request){.node = node,
                                 .preserves_namespaces = true,
                                 .inherits_namespaces = true,
                                 .preserves_type_annotations = false,
                                 .strips_identity = false});
    }
    return status;
}

/* Parses request->text into the validator, in one pass, as sxml validate
   does: reports carry the text's locations. External entities are not
   read (the null entity resolver, include/sxml/xml.h). */
static sxml_status sxml_host_validate_text(sxml_host_schema *schema,
                                           const sxml_host_validate_request *request,
                                           sxml_host_validation *validation) {
    sxml_host_processor *processor = schema->owner;
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        processor->allocator,
        &(sxml_xml_parser_configuration){
            .allocator = processor->allocator,
            .name_pool = sxml_processor_get_name_pool(processor->processor),
            .limits = {.maximum_depth = 0,
                       .maximum_entity_expansion_byte_count =
                           SXML_HOST_VALIDATION_EXPANSION_BYTE_COUNT}},
        &parser);
    if (status == SXML_STATUS_OK) {
        status = sxml_host_start_validation(schema, request, validation);
    }
    if (status == SXML_STATUS_OK) {
        sxml_xml_parse_request parse_request;
        memset(&parse_request, 0, sizeof(parse_request));
        parse_request.input = request->text;
        parse_request.source_name = request->document_uri;
        parse_request.document_uri = request->document_uri;
        parse_request.base_uri = request->document_uri;
        parse_request.diagnostic = &processor->error_record.diagnostic;
        status = sxml_xml_parser_parse(parser, &parse_request,
                                       sxml_schema_validator_as_tree_builder(validation->validator));
    }
    sxml_xml_parser_destroy(parser);
    return status;
}

/*
 * Validates request->node, a document or element node, or request->text,
 * and returns the validated copy (Part 1, 5.2; XDM 4.0, 3.3: annotated),
 * or NULL: with the processor's error set when validation failed (text
 * that is not well-formed, a node of another kind, a type the schema does
 * not have, a limit, out of memory), without it when the node is invalid.
 * Writes the validity and every report into request.
 */
sxml_host_value *sxml_host_schema_validate(sxml_host_schema *schema,
                                           sxml_host_validate_request *request) {
    sxml_host_processor *processor = schema->owner;
    sxml_host_begin(processor);
    sxml_host_side_result_target target = sxml_host_schema_target(schema);
    sxml_host_side_results_clear(&target);
    request->validity = (uint32_t)SXML_VALIDITY_INVALID;
    sxml_host_validation validation = {.arena_tree = NULL,
                                             .validator = NULL,
                                             .base_uri = sxml_sequence_empty(),
                                             .target = target};
    sxml_status status = SXML_STATUS_INVALID_ARGUMENT;
    if (schema->schema != NULL) {
        status = request->node != NULL
                     ? sxml_host_validate_copy(schema, request, &validation)
                     : sxml_host_validate_text(schema, request, &validation);
    }
    sxml_validation_outcome outcome = {.validity = SXML_VALIDITY_INVALID, .error_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_validator_get_outcome(validation.validator, &outcome);
    }
    sxml_schema_validator_destroy(validation.validator);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK && outcome.validity != SXML_VALIDITY_INVALID) {
        status = sxml_arena_tree_finish(validation.arena_tree, &tree);
    }
    sxml_sequence_release(validation.base_uri);
    if (tree == NULL) {
        sxml_arena_tree_destroy(validation.arena_tree);
    }
    request->side_results = sxml_host_side_results_view(&schema->side_results);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return NULL;
    }
    request->validity = (uint32_t)outcome.validity;
    if (tree == NULL) {
        return NULL;
    }
    const sxml_item root = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return sxml_host_value_make(processor, sxml_sequence_from_item(root));
}
