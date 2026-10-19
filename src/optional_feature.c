/*
 * Optional features: the definitions' registry, a processor's slots, the
 * feature data path and the test feature (include/sxml/processor.h,
 * "Optional features"; src/optional_feature_internal.h; D183, D184, D189).
 *
 * Golden file: src/processor.c (the clock and the table resolver: a static
 * const operations table behind a function that returns a value).
 */
#include "optional_feature_internal.h"

#include "processor_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------ Views */

bool sxml_optional_feature_view_is_valid(sxml_optional_feature_view view) {
    if (view.optional_feature_count > 0 && view.optional_features == NULL) {
        return false;
    }
    for (size_t feature_index = 0; feature_index < view.optional_feature_count;
         feature_index += 1) {
        const sxml_optional_feature_definition *definition =
            view.optional_features[feature_index].definition;
        if (definition == NULL || (size_t)definition->feature_kind >=
                                      (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT) {
            return false;
        }
    }
    return true;
}

sxml_optional_feature_slots sxml_optional_feature_slots_from_view(
    sxml_optional_feature_view view) {
    sxml_optional_feature_slots slots;
    memset(&slots, 0, sizeof(slots));
    for (size_t feature_index = 0; feature_index < view.optional_feature_count;
         feature_index += 1) {
        const sxml_optional_feature_definition *definition =
            view.optional_features[feature_index].definition;
        slots.slots[definition->feature_kind].definition = definition;
    }
    return slots;
}

void sxml_optional_feature_slots_finalize(sxml_optional_feature_slots *slots,
                                          sxml_allocator allocator) {
    for (size_t kind_index = 0; kind_index < (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT;
         kind_index += 1) {
        sxml_optional_feature_slot *slot = &slots->slots[kind_index];
        if (slot->state != NULL && slot->definition != NULL &&
            slot->definition->finalize_state != NULL) {
            slot->definition->finalize_state(slot->state, allocator);
        }
        slot->state = NULL;
    }
}

sxml_byte_view sxml_optional_feature_get_name(sxml_optional_feature feature) {
    if (feature.definition == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return feature.definition->feature_name;
}

/* ------------------------------------------------------- A processor's */

/* The slot of kind in processor, or NULL for no processor or a kind out of
   range. */
static const sxml_optional_feature_slot *sxml_processor_slot(
    const sxml_processor *processor, sxml_optional_feature_kind feature_kind) {
    if (processor == NULL ||
        (size_t)feature_kind >= (size_t)SXML_OPTIONAL_FEATURE_KIND_COUNT) {
        return NULL;
    }
    return &processor->optional_features.slots[feature_kind];
}

bool sxml_processor_has_optional_feature(const sxml_processor *processor,
                                         sxml_optional_feature feature) {
    if (feature.definition == NULL) {
        return false;
    }
    const sxml_optional_feature_slot *slot =
        sxml_processor_slot(processor, feature.definition->feature_kind);
    return slot != NULL && slot->definition == feature.definition;
}

const void *sxml_processor_find_optional_feature(const sxml_processor *processor,
                                                 sxml_optional_feature_kind feature_kind) {
    const sxml_optional_feature_slot *slot = sxml_processor_slot(processor, feature_kind);
    if (slot == NULL || slot->definition == NULL) {
        return NULL;
    }
    return slot->definition->operations;
}

void *sxml_processor_get_optional_feature_state(const sxml_processor *processor,
                                                sxml_optional_feature_kind feature_kind) {
    const sxml_optional_feature_slot *slot = sxml_processor_slot(processor, feature_kind);
    return slot == NULL ? NULL : slot->state;
}

sxml_status sxml_processor_set_optional_feature_state(sxml_processor *processor,
                                                      sxml_optional_feature_kind feature_kind,
                                                      void *state) {
    const sxml_optional_feature_slot *slot = sxml_processor_slot(processor, feature_kind);
    if (slot == NULL || slot->definition == NULL ||
        slot->definition->finalize_state == NULL || slot->state != NULL || state == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    processor->optional_features.slots[feature_kind].state = state;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Feature data */

/* Reads the data through a loader over the processor's resolver, the one
   compilers read parameter documents with (src/processor_internal.h), so
   URI resolution and the forms a kind allows are checked in one place. */
sxml_status sxml_optional_feature_read_data(const sxml_feature_data_request *request,
                                            sxml_feature_data_outcome *outcome) {
    outcome->is_available = false;
    if (request == NULL || request->processor == NULL ||
        request->reader.operations == NULL || request->reader.operations->read == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor_document_loader loader_memory;
    void *loader_bytes = &loader_memory;
    sxml_processor_document_loader *loader = NULL;
    sxml_status status = sxml_processor_compile_document_loader_initialize(
        (sxml_memory_view){.bytes = loader_bytes, .byte_count = sizeof(loader_memory)},
        request->processor, &loader);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_retrieved_resource retrieved;
    status = sxml_processor_document_loader_retrieve(
        loader,
        &(sxml_resource_retrieval_request){.resource_kind = request->resource_kind,
                                           .uri_reference = request->uri,
                                           .base_uri = {.bytes = NULL, .byte_count = 0},
                                           .encoding_name = {.bytes = NULL, .byte_count = 0}},
        &retrieved);
    if (status == SXML_STATUS_OK) {
        if (retrieved.is_available) {
            status = request->reader.operations->read(request->reader.state, retrieved.content);
            outcome->is_available = status == SXML_STATUS_OK;
        }
        sxml_processor_document_loader_release(loader, &retrieved);
    }
    sxml_processor_document_loader_finalize(loader);
    return status;
}

/* ------------------------------------------------------ The test feature */

/* The state of the test feature in one processor. */
typedef struct sxml_test_feature_state {
    size_t data_byte_count;
} sxml_test_feature_state;

static sxml_status sxml_test_feature_count_bytes(void *state, sxml_byte_view content) {
    size_t *byte_count = state;
    *byte_count = content.byte_count;
    return SXML_STATUS_OK;
}

static const sxml_feature_data_reader_operations sxml_test_feature_reader_operations = {
    .read = sxml_test_feature_count_bytes,
};

static sxml_status sxml_test_feature_read_data_byte_count(sxml_processor *processor,
                                                          size_t *byte_count) {
    const sxml_test_feature_state *kept =
        sxml_processor_get_optional_feature_state(processor, SXML_OPTIONAL_FEATURE_KIND_TEST);
    if (kept != NULL) {
        *byte_count = kept->data_byte_count;
        return SXML_STATUS_OK;
    }
    size_t data_byte_count = 0;
    sxml_feature_data_outcome outcome = {.is_available = false};
    sxml_status status = sxml_optional_feature_read_data(
        &(sxml_feature_data_request){
            .processor = processor,
            .resource_kind = SXML_RESOURCE_KIND_TEXT,
            .uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:sxml:test-feature-data"),
            .reader = {.state = &data_byte_count,
                       .operations = &sxml_test_feature_reader_operations}},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!outcome.is_available) {
        *byte_count = 0;
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_test_feature_state),
                                  .alignment = alignof(sxml_test_feature_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_test_feature_state *state = memory;
    state->data_byte_count = data_byte_count;
    status = sxml_processor_set_optional_feature_state(processor, SXML_OPTIONAL_FEATURE_KIND_TEST,
                                                       state);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, memory);
        return status;
    }
    *byte_count = data_byte_count;
    return SXML_STATUS_OK;
}

static void sxml_test_feature_finalize_state(void *state, sxml_allocator allocator) {
    sxml_allocator_release(allocator, state);
}

static const sxml_test_feature_operations sxml_test_feature_operations_table = {
    .read_data_byte_count = sxml_test_feature_read_data_byte_count,
};

static const sxml_optional_feature_definition sxml_test_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_TEST,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("test"),
    .operations = &sxml_test_feature_operations_table,
    .finalize_state = sxml_test_feature_finalize_state,
    .library_functions = NULL,
};

sxml_optional_feature sxml_test_optional_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_test_feature_definition};
}

/* ------------------------------------------------------- Every feature */

/* One entry per feature, in kind order; each later feature adds its
   definition here (src/optional_feature_internal.h). */
static const sxml_optional_feature sxml_every_optional_feature[] = {
    {.definition = &sxml_test_feature_definition},
    {.definition = &sxml_expath_binary_feature_definition},
    {.definition = &sxml_html_feature_definition},
    {.definition = &sxml_xslt_transform_feature_definition},
    {.definition = &sxml_timezone_database_feature_definition},
    {.definition = &sxml_dtd_validation_feature_definition},
    {.definition = &sxml_uca_collation_feature_definition},
    {.definition = &sxml_full_text_feature_definition},
};

sxml_optional_feature_view sxml_all_optional_features(void) {
    return (sxml_optional_feature_view){
        .optional_features = sxml_every_optional_feature,
        .optional_feature_count =
            sizeof(sxml_every_optional_feature) / sizeof(sxml_every_optional_feature[0])};
}
