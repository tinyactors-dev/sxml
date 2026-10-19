/*
 * The DTD validator (include/sxml/schema.h, "DTD validation"; packet
 * 7.14b, docs/DECISIONS.md D199): the XML parser's sxml_xml_validator over
 * a schema compiled from the schema documents the parser writes from the
 * DTD (src/dtd.c) and a schema validator that only checks.
 *
 * Golden file: src/schema_validate.c (lifecycle, reports through a sink
 * and the error record).
 */
#include "sxml/schema.h"

#include "processor_internal.h"

#include <stdalign.h>
#include <string.h>

/* What reports go through: the configured sink, counted. */
struct sxml_dtd_validator {
    sxml_dtd_validator_configuration configuration;
    /* The allocator of sxml_dtd_validator_create; null otherwise. */
    sxml_allocator owning_allocator;
    sxml_schema *schema;
    sxml_schema_validator *schema_validator;
    /* Errors reported: the parser's, the schema's, the validator's. */
    size_t error_count;
};

static const sxml_byte_view sxml_dtd_validity_code = SXML_BYTE_VIEW_INITIALIZER("xml-validity");

static bool sxml_dtd_configuration_is_valid(const sxml_dtd_validator_configuration *configuration) {
    return configuration != NULL && configuration->processor != NULL;
}

sxml_status
sxml_dtd_validator_memory_requirements(const sxml_dtd_validator_configuration *configuration,
                                       sxml_memory_requirements *requirements) {
    if (!sxml_dtd_configuration_is_valid(configuration) || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_dtd_validator),
                                               .alignment = alignof(sxml_dtd_validator)};
    return SXML_STATUS_OK;
}

sxml_status sxml_dtd_validator_initialize(sxml_memory_view memory,
                                          const sxml_dtd_validator_configuration *configuration,
                                          sxml_dtd_validator **validator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_dtd_validator_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || validator == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_dtd_validator *placed = placement;
    *placed = (sxml_dtd_validator){.configuration = *configuration,
                                   .owning_allocator = {.state = NULL, .operations = NULL},
                                   .schema = NULL,
                                   .schema_validator = NULL,
                                   .error_count = 0};
    *validator = placed;
    return SXML_STATUS_OK;
}

void sxml_dtd_validator_finalize(sxml_dtd_validator *validator) {
    if (validator == NULL) {
        return;
    }
    sxml_schema_validator_destroy(validator->schema_validator);
    sxml_schema_destroy(validator->schema);
    validator->schema_validator = NULL;
    validator->schema = NULL;
}

sxml_status sxml_dtd_validator_create(sxml_allocator allocator,
                                      const sxml_dtd_validator_configuration *configuration,
                                      sxml_dtd_validator **validator) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_dtd_validator_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || validator == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status =
        sxml_allocator_allocate(allocator,
                                (sxml_allocation_request){.byte_count = requirements.byte_count,
                                                          .alignment = requirements.alignment},
                                &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_dtd_validator *created = NULL;
    status = sxml_dtd_validator_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->owning_allocator = allocator;
    *validator = created;
    return SXML_STATUS_OK;
}

void sxml_dtd_validator_destroy(sxml_dtd_validator *validator) {
    if (validator == NULL) {
        return;
    }
    const sxml_allocator allocator = validator->owning_allocator;
    sxml_dtd_validator_finalize(validator);
    sxml_allocator_release(allocator, validator);
}

/* ---------------------------------------------------------- Reports */

/*
 * One report: into the error record when it is the first, then to the
 * configured sink, whose status decides; the null sink stops the parse
 * with SXML_STATUS_DYNAMIC_ERROR (include/sxml/schema.h).
 */
static sxml_status sxml_dtd_report(sxml_dtd_validator *validator,
                                   const sxml_schema_report *report) {
    validator->error_count += 1;
    sxml_error_record *error_record = validator->configuration.error_record;
    if (validator->error_count == 1 && error_record != NULL) {
        /* The item heap keeps a copy of the description (D41). */
        const sxml_status status = sxml_item_heap_raise_error(
            validator->configuration.processor->item_heap,
            &(sxml_error_raise_request){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                                        .code = report->code,
                                        .description = report->description,
                                        .has_value = false,
                                        .value = sxml_sequence_empty(),
                                        .location = report->location},
            error_record);
        if (status != SXML_STATUS_DYNAMIC_ERROR) {
            return status;
        }
    }
    const sxml_schema_report_sink sink = validator->configuration.report_sink;
    if (sink.operations == NULL) {
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    return sink.operations->receive(sink.state, report);
}

/* The sink of the schema and the schema validator: counted, passed on. */
static sxml_status sxml_dtd_receive(void *state, const sxml_schema_report *report) {
    sxml_dtd_validator *validator = state;
    validator->error_count += 1;
    const sxml_schema_report_sink sink = validator->configuration.report_sink;
    if (sink.operations == NULL) {
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    return sink.operations->receive(sink.state, report);
}

static const sxml_schema_report_sink_operations sxml_dtd_sink_operations = {
    .receive = sxml_dtd_receive,
};

/* ------------------------------------------------ The xml.h interface */

/* Compiles the schema documents and starts a schema validator over them
   whose results go nowhere. */
static sxml_status sxml_dtd_start_validation(void *state, const sxml_xml_dtd_schema_view *schema,
                                             sxml_tree_builder *checking_builder) {
    sxml_dtd_validator *validator = state;
    sxml_processor *processor = validator->configuration.processor;
    const sxml_allocator allocator = processor->allocator;
    const size_t document_count = schema->schema_text_count;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(document_count == 0 ? 1 : document_count,
                                            sizeof(sxml_schema_document), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_schema_document)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_document *documents = memory;
    for (size_t document_index = 0; document_index < document_count; document_index++) {
        documents[document_index] =
            (sxml_schema_document){.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
                                   .document_text = schema->schema_texts[document_index],
                                   .document_node = {.representation = 0},
                                   .document_uri = {.bytes = NULL, .byte_count = 0},
                                   .checks_target_namespace = false,
                                   .target_namespace_uri = {.bytes = NULL, .byte_count = 0}};
    }
    const sxml_schema_report_sink counting_sink = {.state = validator,
                                                   .operations = &sxml_dtd_sink_operations};
    sxml_error_record schema_error;
    memset(&schema_error, 0, sizeof schema_error);
    status = sxml_schema_create(
        allocator,
        &(sxml_schema_configuration){.processor = processor,
                                     .schema_documents = {.schema_documents = documents,
                                                          .schema_document_count = document_count},
                                     .base_uri = {.bytes = NULL, .byte_count = 0},
                                     .report_sink = {.state = NULL, .operations = NULL},
                                     .maximum_content_model_state_count = 0,
                                     .error_record = &schema_error},
        &validator->schema);
    sxml_allocator_release(allocator, documents);
    if (status == SXML_STATUS_STATIC_ERROR) {
        /* The DTD does not give a valid schema: the document is not
           valid (include/sxml/schema.h). */
        return sxml_dtd_report(
            validator, &(sxml_schema_report){.code = schema_error.code,
                                             .description = schema_error.diagnostic.description,
                                             .location = schema_error.diagnostic.location});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status =
        sxml_schema_validator_create(allocator,
                                     &(sxml_schema_validator_configuration){
                                         .schema = validator->schema,
                                         .validation_mode = SXML_VALIDATION_MODE_STRICT,
                                         .governing_type = {.index = SXML_TYPE_NONE},
                                         .result_builder = {.state = NULL, .operations = NULL},
                                         .report_sink = counting_sink,
                                         .error_record = validator->configuration.error_record},
                                     &validator->schema_validator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *checking_builder = sxml_schema_validator_as_tree_builder(validator->schema_validator);
    return SXML_STATUS_OK;
}

static sxml_status sxml_dtd_report_validity_error(void *state,
                                                  const sxml_xml_validity_error *error) {
    return sxml_dtd_report(
        state, &(sxml_schema_report){.code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                                              .local_name = sxml_dtd_validity_code,
                                              .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
                                     .description = error->description,
                                     .location = error->location});
}

static const sxml_xml_validator_operations sxml_dtd_validator_operations = {
    .start_validation = sxml_dtd_start_validation,
    .report_validity_error = sxml_dtd_report_validity_error,
};

sxml_xml_validator sxml_dtd_validator_as_xml_validator(sxml_dtd_validator *validator) {
    return (sxml_xml_validator){.state = validator, .operations = &sxml_dtd_validator_operations};
}

sxml_status sxml_dtd_validator_get_outcome(const sxml_dtd_validator *validator,
                                           sxml_validation_outcome *outcome) {
    if (validator == NULL || outcome == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_validation_outcome schema_outcome = {.validity = SXML_VALIDITY_VALID, .error_count = 0};
    if (validator->schema_validator != NULL) {
        const sxml_status status =
            sxml_schema_validator_get_outcome(validator->schema_validator, &schema_outcome);
        if (status != SXML_STATUS_OK && validator->error_count == 0) {
            return status;
        }
    } else if (validator->error_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const bool is_valid =
        validator->error_count == 0 && schema_outcome.validity == SXML_VALIDITY_VALID;
    *outcome = (sxml_validation_outcome){
        .validity = is_valid ? SXML_VALIDITY_VALID : SXML_VALIDITY_INVALID,
        .error_count = validator->error_count > schema_outcome.error_count
                           ? validator->error_count
                           : schema_outcome.error_count};
    return SXML_STATUS_OK;
}
