/*
 * Simple values, reports and the ID table of the schema validator
 * (src/schema_validate_internal.h; docs/DECISIONS.md D165, D167, D173).
 *
 * A simple value is assessed by the cast engine (src/cast.c: whiteSpace,
 * the lexical mapping of the primitive type and every facet of the type
 * and its bases, D171), so XSD validation and XPath casting share one
 * lexical-space engine; this file only maps the cast's outcome to XSD's
 * validation rules: String Valid (XSD 1.1 Part 1, 3.16.4) with its
 * ENTITY clause, Datatype Valid (Part 2, 4.1.4: cvc-datatype-valid and the
 * facets' cvc-*-valid names), the schema normalized value (Part 2, 4.3.6)
 * and Validation Root Valid (ID/IDREF Table), cvc-id (3.17.4.2).
 *
 * Golden file: src/schema_simple_type.c (casts of value constraints and
 * the outcome of a cast as an answer, not a failure).
 */
#include "schema_validate_internal.h"

#include "cast_internal.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "sxml/xdm.h"

#include <stdalign.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------------- Reports */

sxml_status sxml_schema_validator_report(sxml_schema_validator *validator,
                                         const sxml_schema_report *report) {
    validator->error_count += 1;
    sxml_error_record *error_record = validator->configuration.error_record;
    if (validator->error_count == 1 && error_record != NULL) {
        *error_record = (sxml_error_record){
            .code = report->code,
            .value = NULL,
            .diagnostic = {.description = report->description, .location = report->location}};
    }
    const sxml_schema_report_sink sink = validator->configuration.report_sink;
    if (sink.operations == NULL) {
        /* include/sxml/schema.h: the null sink stops at the first error. */
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    return sink.operations->receive(sink.state, report);
}

sxml_status sxml_schema_validator_report_error(sxml_schema_validator *validator,
                                               const sxml_schema_validation_error *error) {
    const sxml_schema_report report = {
        .code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                 .local_name = error->constraint_name,
                 .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
        .description = error->description,
        .location = error->location};
    return sxml_schema_validator_report(validator, &report);
}

/* ------------------------------------------------------- Scratch bytes */

sxml_status sxml_schema_scratch_array_append_bytes(sxml_allocator allocator,
                                                   sxml_schema_scratch_array *array,
                                                   sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t needed = 0;
    sxml_status status = sxml_size_add(array->entry_count, bytes.byte_count, &needed);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (needed > array->entry_capacity) {
        size_t capacity = array->entry_capacity == 0 ? 64U : array->entry_capacity;
        while (capacity < needed) {
            if (sxml_size_multiply(capacity, 2U, &capacity) != SXML_STATUS_OK) {
                return SXML_STATUS_LIMIT_EXCEEDED;
            }
        }
        void *memory = NULL;
        status = sxml_allocator_allocate(
            allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1}, &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->entry_count != 0) {
            memcpy(memory, array->bytes, array->entry_count);
        }
        sxml_allocator_release(allocator, array->bytes);
        array->bytes = memory;
        array->entry_capacity = capacity;
    }
    memcpy(array->bytes + array->entry_count, bytes.bytes, bytes.byte_count);
    array->entry_count = needed;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Namespaces */

sxml_status sxml_schema_validator_get_cast_bindings(sxml_schema_validator *validator,
                                                    sxml_namespace_binding_view *bindings) {
    sxml_schema_scratch_array *cast_bindings = &validator->cast_bindings;
    cast_bindings->entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    /* Innermost first: src/cast.c takes the first binding of a prefix. */
    for (size_t binding_index = validator->namespace_bindings.entry_count;
         status == SXML_STATUS_OK && binding_index > 0; binding_index -= 1) {
        status = sxml_schema_scratch_array_append(
            validator->allocator, cast_bindings,
            sxml_schema_scratch_array_at(&validator->namespace_bindings, binding_index - 1));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *bindings = (sxml_namespace_binding_view){
        .namespace_bindings = sxml_schema_scratch_array_at(cast_bindings, 0),
        .namespace_binding_count = cast_bindings->entry_count};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ The ID table */

/* An ID met: its value in value_bytes. */
typedef struct sxml_schema_id_entry {
    size_t value_offset;
    size_t value_byte_count;
} sxml_schema_id_entry;

/* An IDREF met, checked at the end of the validation root. */
typedef struct sxml_schema_idref_entry {
    size_t value_offset;
    size_t value_byte_count;
    sxml_source_location location;
} sxml_schema_idref_entry;

void sxml_schema_id_table_initialize(sxml_schema_id_table *id_table, sxml_allocator allocator) {
    *id_table =
        (sxml_schema_id_table){.allocator = allocator,
                               .value_bytes = {.bytes = NULL,
                                               .entry_byte_count = 1,
                                               .entry_count = 0,
                                               .entry_capacity = 0},
                               .identifiers = {.bytes = NULL,
                                               .entry_byte_count = sizeof(sxml_schema_id_entry),
                                               .entry_count = 0,
                                               .entry_capacity = 0},
                               .slots = NULL,
                               .slot_count = 0,
                               .references = {.bytes = NULL,
                                              .entry_byte_count = sizeof(sxml_schema_idref_entry),
                                              .entry_count = 0,
                                              .entry_capacity = 0}};
}

void sxml_schema_id_table_release(sxml_schema_id_table *id_table) {
    sxml_schema_scratch_array_release(id_table->allocator, &id_table->value_bytes);
    sxml_schema_scratch_array_release(id_table->allocator, &id_table->identifiers);
    sxml_schema_scratch_array_release(id_table->allocator, &id_table->references);
    sxml_allocator_release(id_table->allocator, id_table->slots);
    id_table->slots = NULL;
    id_table->slot_count = 0;
}

/* FNV-1a over the value's bytes. */
static uint64_t hash_value(sxml_byte_view value) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t byte_index = 0; byte_index < value.byte_count; byte_index += 1) {
        hash ^= value.bytes[byte_index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static sxml_byte_view id_value(const sxml_schema_id_table *id_table, size_t offset,
                               size_t byte_count) {
    return (sxml_byte_view){.bytes = id_table->value_bytes.bytes + offset,
                            .byte_count = byte_count};
}

/* The slot of value: the one holding it, or the free one it would take. */
static size_t find_slot(const sxml_schema_id_table *id_table, sxml_byte_view value) {
    const size_t mask = id_table->slot_count - 1U;
    size_t slot = (size_t)hash_value(value) & mask;
    while (id_table->slots[slot] != 0) {
        const sxml_schema_id_entry *entry =
            sxml_schema_scratch_array_at(&id_table->identifiers, id_table->slots[slot] - 1U);
        if (sxml_byte_view_equals(id_value(id_table, entry->value_offset, entry->value_byte_count),
                                  value)) {
            return slot;
        }
        slot = (slot + 1U) & mask;
    }
    return slot;
}

/* Doubles the slots (or makes the first ones) and enters every ID again. */
static sxml_status grow_slots(sxml_schema_id_table *id_table) {
    const size_t slot_count = id_table->slot_count == 0 ? 64U : id_table->slot_count * 2U;
    size_t byte_count = 0;
    if (slot_count <= id_table->slot_count || slot_count > UINT32_MAX ||
        sxml_size_multiply(slot_count, sizeof(uint32_t), &byte_count) != SXML_STATUS_OK) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        id_table->allocator,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    sxml_allocator_release(id_table->allocator, id_table->slots);
    id_table->slots = memory;
    id_table->slot_count = slot_count;
    for (size_t entry_index = 0; entry_index < id_table->identifiers.entry_count;
         entry_index += 1) {
        const sxml_schema_id_entry *entry =
            sxml_schema_scratch_array_at(&id_table->identifiers, entry_index);
        const size_t slot =
            find_slot(id_table, id_value(id_table, entry->value_offset, entry->value_byte_count));
        id_table->slots[slot] = (uint32_t)(entry_index + 1U);
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_id_table_add_id(sxml_schema_id_table *id_table,
                                        const sxml_schema_id_request *request,
                                        sxml_schema_id_outcome *outcome) {
    if ((id_table->identifiers.entry_count + 1U) * 2U > id_table->slot_count) {
        const sxml_status status = grow_slots(id_table);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const size_t slot = find_slot(id_table, request->value);
    if (id_table->slots[slot] != 0) {
        outcome->is_duplicate = true;
        return SXML_STATUS_OK;
    }
    const sxml_schema_id_entry entry = {.value_offset = id_table->value_bytes.entry_count,
                                        .value_byte_count = request->value.byte_count};
    sxml_status status = sxml_schema_scratch_array_append_bytes(
        id_table->allocator, &id_table->value_bytes, request->value);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_schema_scratch_array_append(id_table->allocator, &id_table->identifiers, &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    id_table->slots[slot] = (uint32_t)id_table->identifiers.entry_count;
    outcome->is_duplicate = false;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_id_table_add_idref(sxml_schema_id_table *id_table,
                                           const sxml_schema_id_request *request) {
    const sxml_schema_idref_entry entry = {.value_offset = id_table->value_bytes.entry_count,
                                           .value_byte_count = request->value.byte_count,
                                           .location = request->location};
    const sxml_status status = sxml_schema_scratch_array_append_bytes(
        id_table->allocator, &id_table->value_bytes, request->value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_schema_scratch_array_append(id_table->allocator, &id_table->references, &entry);
}

sxml_status sxml_schema_id_table_check_idrefs(sxml_schema_id_table *id_table,
                                              sxml_schema_validator *validator) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t reference_index = 0;
         status == SXML_STATUS_OK && reference_index < id_table->references.entry_count;
         reference_index += 1) {
        const sxml_schema_idref_entry *entry =
            sxml_schema_scratch_array_at(&id_table->references, reference_index);
        const sxml_byte_view value =
            id_value(id_table, entry->value_offset, entry->value_byte_count);
        if (id_table->slot_count != 0 && id_table->slots[find_slot(id_table, value)] != 0) {
            continue;
        }
        /* 3.17.4.2, Validation Root Valid (ID/IDREF Table), clause 1:
           every IDREF names an ID of the validation root. */
        status = sxml_schema_validator_report_error(
            validator, &(sxml_schema_validation_error){
                           .constraint_name = TEXT("cvc-id.1"),
                           .description = TEXT("An IDREF value names no ID of the document"),
                           .location = entry->location});
    }
    return status;
}

/* --------------------------------------------------------- Simple values */

/* The whiteSpace facet of a built-in atomic type (Part 2, 4.3.6 and the
   built-in definitions of 3.3 and 3.4): preserve for xs:string, replace
   for xs:normalizedString, collapse for xs:token and every type not
   derived from xs:string. */
static sxml_whitespace_facet builtin_whitespace(const sxml_type_registry *registry,
                                                sxml_builtin_type builtin_type) {
    const sxml_type_identifier type = {.index = (uint32_t)builtin_type};
    if (sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){.derived_type = type, .base_type = {.index = SXML_TYPE_XS_TOKEN}})) {
        return SXML_WHITESPACE_FACET_COLLAPSE;
    }
    if (sxml_type_registry_is_derived_from(
            registry, (sxml_type_pair){.derived_type = type,
                                       .base_type = {.index = SXML_TYPE_XS_NORMALIZED_STRING}})) {
        return SXML_WHITESPACE_FACET_REPLACE;
    }
    if (sxml_type_registry_is_derived_from(
            registry,
            (sxml_type_pair){.derived_type = type, .base_type = {.index = SXML_TYPE_XS_STRING}})) {
        return SXML_WHITESPACE_FACET_PRESERVE;
    }
    return SXML_WHITESPACE_FACET_COLLAPSE;
}

/* The whiteSpace facet that gives a simple type's schema normalized
   value: an atomic type's own (src/cast_facet.c), collapse for a list
   (Part 2, 4.3.6.2), preserve for a union, whose members normalize
   their own values. */
static sxml_whitespace_facet whitespace_of(const sxml_schema_validator *validator,
                                           const sxml_schema_type_definition *definition) {
    if (definition->variety == SXML_TYPE_VARIETY_LIST) {
        return SXML_WHITESPACE_FACET_COLLAPSE;
    }
    if (definition->variety != SXML_TYPE_VARIETY_ATOMIC) {
        return SXML_WHITESPACE_FACET_PRESERVE;
    }
    sxml_atomic_target target = {.type = {.index = SXML_TYPE_NONE},
                                 .builtin_type = SXML_TYPE_XS_STRING,
                                 .primitive_type = SXML_TYPE_XS_STRING};
    if (sxml_cast_resolve_atomic_target(validator->type_registry, definition->registered_type,
                                        &target) != SXML_STATUS_OK) {
        return SXML_WHITESPACE_FACET_PRESERVE;
    }
    const sxml_whitespace_facet builtin_facet =
        builtin_whitespace(validator->type_registry, target.builtin_type);
    if (definition->is_builtin) {
        return builtin_facet;
    }
    return sxml_cast_find_whitespace_facet(validator->type_registry, definition->registered_type,
                                           builtin_facet);
}

static bool is_xml_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* Writes the value after whitespace processing (Part 2, 4.3.6) to
   validator->value_bytes, or gives the text itself when it is
   unchanged. */
/* A value and the whiteSpace facet to apply to it. */
typedef struct whitespace_request {
    sxml_whitespace_facet facet;
    sxml_byte_view text;
} whitespace_request;

static sxml_status normalize_value(sxml_schema_validator *validator, whitespace_request request,
                                   sxml_byte_view *normalized) {
    const sxml_whitespace_facet facet = request.facet;
    const sxml_byte_view text = request.text;
    if (facet == SXML_WHITESPACE_FACET_PRESERVE) {
        *normalized = text;
        return SXML_STATUS_OK;
    }
    sxml_schema_scratch_array *bytes = &validator->value_bytes;
    bytes->entry_count = 0;
    bool pending_space = false;
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < text.byte_count;
         byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (is_xml_space(byte)) {
            if (facet == SXML_WHITESPACE_FACET_COLLAPSE) {
                pending_space = bytes->entry_count != 0;
                continue;
            }
            byte = ' ';
        }
        if (pending_space) {
            status = sxml_schema_scratch_array_append_bytes(validator->allocator, bytes, TEXT(" "));
            pending_space = false;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scratch_array_append_bytes(
                validator->allocator, bytes, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *normalized = (sxml_byte_view){.bytes = bytes->bytes, .byte_count = bytes->entry_count};
    return SXML_STATUS_OK;
}

/* The constraint a failed cast breaks: the facet's cvc-*-valid name that
   src/cast_facet.c puts first in its description (static text), else
   cvc-datatype-valid.1.2.1 (Part 2, 4.1.4). */
static sxml_byte_view constraint_of(const sxml_error_record *error_record) {
    const sxml_byte_view description = error_record->diagnostic.description;
    static const uint8_t prefix[] = {'c', 'v', 'c', '-'};
    if (description.byte_count > sizeof(prefix) &&
        memcmp(description.bytes, prefix, sizeof(prefix)) == 0) {
        size_t byte_count = sizeof(prefix);
        while (byte_count < description.byte_count && description.bytes[byte_count] != ':' &&
               description.bytes[byte_count] != ' ') {
            byte_count += 1;
        }
        return (sxml_byte_view){.bytes = description.bytes, .byte_count = byte_count};
    }
    return TEXT("cvc-datatype-valid.1.2.1");
}

static bool is_derived(const sxml_schema_validator *validator, sxml_item item,
                       sxml_builtin_type base_type) {
    return sxml_type_registry_is_derived_from(
        validator->type_registry,
        (sxml_type_pair){.derived_type = sxml_item_get_type_annotation(item),
                         .base_type = {.index = (uint32_t)base_type}});
}

/* True when name is the name of an unparsed entity of the document. */
static bool is_unparsed_entity(const sxml_schema_validator *validator, sxml_byte_view name) {
    const sxml_schema_scratch_array *names = &validator->entity_names;
    size_t start = 0;
    for (size_t byte_index = 0; byte_index < names->entry_count; byte_index += 1) {
        if (names->bytes[byte_index] != 0) {
            continue;
        }
        if (sxml_byte_view_equals(
                (sxml_byte_view){.bytes = names->bytes + start, .byte_count = byte_index - start},
                name)) {
            return true;
        }
        start = byte_index + 1U;
    }
    return false;
}

/* The text of a string-like item (xs:ID, xs:IDREF, xs:ENTITY). */
static sxml_byte_view string_of(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return datum.datum_details.string.utf8;
}

/* What the items of a valid value bring: is-id, is-idrefs, the ID table
   and the ENTITY rule. */
typedef struct value_items {
    sxml_schema_value_outcome *outcome;
    sxml_source_location location;
} value_items;

static sxml_status examine_items(sxml_schema_validator *validator, value_items items) {
    sxml_schema_value_outcome *outcome = items.outcome;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(outcome->value);
    sxml_iteration_step step = {.has_item = false};
    size_t item_count = 0;
    size_t id_count = 0;
    sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
    while (status == SXML_STATUS_OK && step.has_item) {
        item_count += 1;
        const sxml_item item = step.item;
        if (is_derived(validator, item, SXML_TYPE_XS_ID)) {
            id_count += 1;
            sxml_schema_id_outcome id_outcome = {.is_duplicate = false};
            status = sxml_schema_id_table_add_id(
                &validator->id_table,
                &(sxml_schema_id_request){.value = string_of(item), .location = items.location},
                &id_outcome);
            if (status == SXML_STATUS_OK && id_outcome.is_duplicate) {
                /* 3.17.4.2, clause 2: no two IDs are equal. */
                status = sxml_schema_validator_report_error(
                    validator, &(sxml_schema_validation_error){
                                   .constraint_name = TEXT("cvc-id.2"),
                                   .description = TEXT("An ID value is used twice"),
                                   .location = items.location});
            }
        } else if (is_derived(validator, item, SXML_TYPE_XS_IDREF)) {
            outcome->is_idrefs = true;
            status = sxml_schema_id_table_add_idref(
                &validator->id_table,
                &(sxml_schema_id_request){.value = string_of(item), .location = items.location});
        } else if (is_derived(validator, item, SXML_TYPE_XS_ENTITY) &&
                   !is_unparsed_entity(validator, string_of(item))) {
            /* 3.16.4 String Valid, clause 2: an ENTITY value names an
               unparsed entity (Part 2, 3.3.20). */
            outcome->is_valid = false;
            status = sxml_schema_validator_report_error(
                validator, &(sxml_schema_validation_error){
                               .constraint_name = TEXT("cvc-simple-type"),
                               .description = TEXT("An ENTITY value names no unparsed entity"),
                               .location = items.location});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        }
    }
    /* XDM 4.0, 7.5.2.4: is-id when the value is one xs:ID. */
    outcome->is_id = item_count == 1 && id_count == 1;
    return status;
}

/* A cast that failed with an error of the value, not of the run. */
static bool is_value_error(sxml_status status) {
    return status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR ||
           status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_schema_validate_simple_value(sxml_schema_validator *validator,
                                              const sxml_schema_value_request *request,
                                              sxml_schema_value_outcome *outcome) {
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(validator->components, request->simple_type);
    *outcome = (sxml_schema_value_outcome){.is_valid = true,
                                           .normalized_value = request->text,
                                           .value = sxml_sequence_empty(),
                                           .is_id = false,
                                           .is_idrefs = false};
    const sxml_type_identifier registered = definition->registered_type;
    /* xs:anySimpleType and xs:anyAtomicType accept every value (Part 2,
       3.2.1, 3.2.2); a type that did not register was reported by
       compilation. */
    if (registered.index == SXML_TYPE_NONE ||
        registered.index == (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE ||
        registered.index == (uint32_t)SXML_TYPE_XS_ANY_ATOMIC_TYPE) {
        return SXML_STATUS_OK;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_schema_validator_get_cast_bindings(validator, &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_sequence value = sxml_sequence_empty();
    status =
        sxml_cast_make_value_from_text(validator->item_heap,
                                       &(sxml_text_cast_request){.text = request->text,
                                                                 .target_type = registered,
                                                                 .namespace_bindings = bindings,
                                                                 .error_record = &error_record},
                                       &value);
    if (is_value_error(status)) {
        outcome->is_valid = false;
        return sxml_schema_validator_report_error(
            validator, &(sxml_schema_validation_error){
                           .constraint_name = constraint_of(&error_record),
                           .description = error_record.diagnostic.description.byte_count != 0
                                              ? error_record.diagnostic.description
                                              : TEXT("The value is not valid for its type"),
                           .location = request->location});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    outcome->value = value;
    status = normalize_value(
        validator,
        (whitespace_request){.facet = whitespace_of(validator, definition), .text = request->text},
        &outcome->normalized_value);
    if (status == SXML_STATUS_OK) {
        status = examine_items(validator,
                               (value_items){.outcome = outcome, .location = request->location});
    }
    if (status == SXML_STATUS_OK) {
        /* Part 2, 4.3.13: assertion facets, evaluated with $value
           (src/schema_assertion.c, packet 5.9). */
        status = sxml_schema_check_assertions(
            validator, &(sxml_schema_assertion_request){
                           .schema = validator->configuration.schema,
                           .type_definition = request->simple_type,
                           .node = {.representation = SXML_ITEM_REPRESENTATION_NONE},
                           .value = outcome->value});
    }
    if (status != SXML_STATUS_OK || !outcome->is_valid) {
        sxml_sequence_release(outcome->value);
        outcome->value = sxml_sequence_empty();
    }
    return status;
}
