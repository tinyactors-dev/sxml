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
        /* The description may be made for this report: the item heap
           keeps a copy for the record (D41). */
        const sxml_status status = sxml_item_heap_raise_error(
            validator->item_heap,
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
        /* include/sxml/schema.h: the null sink stops at the first error. */
        return SXML_STATUS_DYNAMIC_ERROR;
    }
    return sink.operations->receive(sink.state, report);
}

/* ------------------------------------------------- Error descriptions */

/* Most bytes of a value a description quotes, and most expected terms or
   enumeration values it lists. */
enum {
    SXML_SCHEMA_MESSAGE_VALUE_BYTE_LIMIT = 60,
    SXML_SCHEMA_MESSAGE_LIST_LIMIT = 8,
};

static sxml_status append_text(sxml_schema_validator *validator, sxml_byte_view text) {
    return sxml_schema_scratch_array_append_bytes(validator->allocator, &validator->message_bytes,
                                                  text);
}

/* text in quotes, cut at a character boundary past the byte limit. */
static sxml_status append_quoted(sxml_schema_validator *validator, sxml_byte_view text) {
    sxml_byte_view shown = text;
    bool is_cut = false;
    if (shown.byte_count > SXML_SCHEMA_MESSAGE_VALUE_BYTE_LIMIT) {
        size_t byte_count = SXML_SCHEMA_MESSAGE_VALUE_BYTE_LIMIT;
        while (byte_count > 0 && (shown.bytes[byte_count] & 0xC0U) == 0x80U) {
            byte_count -= 1;
        }
        shown.byte_count = byte_count;
        is_cut = true;
    }
    sxml_status status = append_text(validator, TEXT("'"));
    const size_t start = validator->message_bytes.entry_count;
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, shown);
    }
    /* One line: line ends and tabs show as spaces. */
    for (size_t byte_index = start;
         status == SXML_STATUS_OK && byte_index < validator->message_bytes.entry_count;
         byte_index += 1) {
        if (validator->message_bytes.bytes[byte_index] < 0x20U) {
            validator->message_bytes.bytes[byte_index] = ' ';
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, is_cut ? TEXT("...'") : TEXT("'"));
    }
    return status;
}

/* The prefix a namespace has in scope (the innermost binding), 0 when it
   is the default namespace or not bound; *is_bound tells which. */
typedef struct prefix_lookup {
    bool is_bound;
    sxml_namespace_prefix_identifier namespace_prefix;
} prefix_lookup;

static prefix_lookup find_prefix(const sxml_schema_validator *validator,
                                 sxml_namespace_uri_identifier namespace_uri) {
    for (size_t binding_index = validator->namespace_bindings.entry_count; binding_index > 0;
         binding_index -= 1) {
        const sxml_namespace_binding *binding =
            sxml_schema_scratch_array_at(&validator->namespace_bindings, binding_index - 1);
        if (binding->namespace_uri.index == namespace_uri.index) {
            return (prefix_lookup){.is_bound = true, .namespace_prefix = binding->namespace_prefix};
        }
    }
    return (prefix_lookup){.is_bound = false, .namespace_prefix = {.index = 0}};
}

/* A name in quotes: as written (its prefix), else with a prefix in scope
   for its namespace (xs for XSD's), else Q{URI}local. */
static sxml_status append_name(sxml_schema_validator *validator, sxml_schema_written_name name) {
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(validator->name_pool, name.name, &parts);
    if (status != SXML_STATUS_OK) {
        return append_text(validator, TEXT("'?'"));
    }
    sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (name.namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(validator->name_pool, name.namespace_prefix,
                                                     &prefix);
    } else if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        const prefix_lookup lookup = find_prefix(validator, parts.namespace_uri);
        if (lookup.is_bound && lookup.namespace_prefix.index != 0) {
            status = sxml_name_pool_get_namespace_prefix(validator->name_pool,
                                                         lookup.namespace_prefix, &prefix);
        } else if (!lookup.is_bound &&
                   parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA) {
            prefix = TEXT("xs");
        } else if (!lookup.is_bound) {
            status = sxml_name_pool_get_namespace_uri(validator->name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, TEXT("'"));
    }
    if (status == SXML_STATUS_OK && prefix.byte_count != 0) {
        status = append_text(validator, prefix);
        if (status == SXML_STATUS_OK) {
            status = append_text(validator, TEXT(":"));
        }
    } else if (status == SXML_STATUS_OK && namespace_uri.byte_count != 0) {
        status = append_text(validator, TEXT("Q{"));
        if (status == SXML_STATUS_OK) {
            status = append_text(validator, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = append_text(validator, TEXT("}"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, parts.local_name);
    }
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, TEXT("'"));
    }
    return status;
}

static sxml_schema_written_name unwritten(sxml_name_identifier name) {
    return (sxml_schema_written_name){.name = name, .namespace_prefix = {.index = 0}};
}

/* A type definition: its name, or an anonymous one's nearest named base
   (xs:anyType's base is itself, so the walk ends). */
static sxml_status append_type(sxml_schema_validator *validator,
                               sxml_schema_type_definition_index type_definition) {
    const sxml_schema_components *components = validator->components;
    sxml_schema_type_definition_index type = type_definition;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, type);
    if (definition->type_name.index != 0) {
        sxml_status status = append_text(validator, TEXT("type "));
        if (status == SXML_STATUS_OK) {
            status = append_name(validator, unwritten(definition->type_name));
        }
        return status;
    }
    const uint32_t type_count = components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
    for (uint32_t base_count = 0; base_count < type_count && definition->type_name.index == 0 &&
                                  definition->base_type.value != 0 &&
                                  definition->base_type.value != type.value;
         base_count += 1) {
        type = definition->base_type;
        definition = sxml_schema_get_type_definition(components, type);
    }
    if (definition->type_name.index == 0) {
        return append_text(validator, TEXT("an anonymous type"));
    }
    sxml_status status = append_text(validator, TEXT("an anonymous type derived from "));
    if (status == SXML_STATUS_OK) {
        status = append_name(validator, unwritten(definition->type_name));
    }
    return status;
}

/* One expected term: an element declaration's name or a wildcard. */
static sxml_status append_term(sxml_schema_validator *validator,
                               const sxml_schema_content_model_match *term) {
    if (term->match_kind == SXML_SCHEMA_MATCH_ELEMENT_DECLARATION) {
        return append_name(
            validator,
            unwritten(sxml_schema_get_element_declaration(validator->components,
                                                          term->element_declaration)
                          ->element_name));
    }
    const sxml_schema_wildcard *wildcard =
        sxml_schema_get_wildcard(validator->components, term->wildcard);
    switch (wildcard->namespace_constraint_variety) {
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY:
        return append_text(validator, TEXT("any element"));
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION:
        return append_text(validator, TEXT("an element of the wildcard's namespaces"));
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT:
        break;
    }
    return append_text(validator, TEXT("an element of another namespace"));
}

/* The expected terms of a cursor, "'a', 'b' or 'c'". */
static sxml_status append_expected(sxml_schema_validator *validator,
                                   const sxml_schema_content_model_cursor *cursor) {
    sxml_schema_content_model_match terms[SXML_SCHEMA_MESSAGE_LIST_LIMIT + 1];
    sxml_schema_expected_term_buffer buffer = {
        .terms = terms, .term_capacity = SXML_SCHEMA_MESSAGE_LIST_LIMIT + 1, .term_count = 0};
    if (cursor != NULL) {
        sxml_schema_content_model_list_expected(validator->components, cursor, &buffer);
    }
    if (buffer.term_count == 0) {
        return append_text(validator, TEXT("no element"));
    }
    const size_t shown_count = buffer.term_count > SXML_SCHEMA_MESSAGE_LIST_LIMIT
                                   ? SXML_SCHEMA_MESSAGE_LIST_LIMIT
                                   : buffer.term_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t term_index = 0; status == SXML_STATUS_OK && term_index < shown_count;
         term_index += 1) {
        if (term_index > 0) {
            status = append_text(validator, term_index + 1 == shown_count &&
                                                    shown_count == buffer.term_count
                                                ? TEXT(" or ")
                                                : TEXT(", "));
        }
        if (status == SXML_STATUS_OK) {
            status = append_term(validator, &terms[term_index]);
        }
    }
    if (status == SXML_STATUS_OK && shown_count < buffer.term_count) {
        status = append_text(validator, TEXT(" or another"));
    }
    return status;
}

/* The facet kinds a cast's constraint name may name, in the order of
   sxml_facet_kind (src/cast_facet.c's names, cvc-NAME-valid). */
static const sxml_byte_view facet_names[] = {
    [SXML_FACET_KIND_LENGTH] = SXML_BYTE_VIEW_INITIALIZER("length"),
    [SXML_FACET_KIND_MIN_LENGTH] = SXML_BYTE_VIEW_INITIALIZER("minLength"),
    [SXML_FACET_KIND_MAX_LENGTH] = SXML_BYTE_VIEW_INITIALIZER("maxLength"),
    [SXML_FACET_KIND_PATTERN] = SXML_BYTE_VIEW_INITIALIZER("pattern"),
    [SXML_FACET_KIND_ENUMERATION] = SXML_BYTE_VIEW_INITIALIZER("enumeration"),
    [SXML_FACET_KIND_WHITESPACE] = SXML_BYTE_VIEW_INITIALIZER("whiteSpace"),
    [SXML_FACET_KIND_MAX_INCLUSIVE] = SXML_BYTE_VIEW_INITIALIZER("maxInclusive"),
    [SXML_FACET_KIND_MAX_EXCLUSIVE] = SXML_BYTE_VIEW_INITIALIZER("maxExclusive"),
    [SXML_FACET_KIND_MIN_EXCLUSIVE] = SXML_BYTE_VIEW_INITIALIZER("minExclusive"),
    [SXML_FACET_KIND_MIN_INCLUSIVE] = SXML_BYTE_VIEW_INITIALIZER("minInclusive"),
    [SXML_FACET_KIND_TOTAL_DIGITS] = SXML_BYTE_VIEW_INITIALIZER("totalDigits"),
    [SXML_FACET_KIND_FRACTION_DIGITS] = SXML_BYTE_VIEW_INITIALIZER("fractionDigits"),
    [SXML_FACET_KIND_EXPLICIT_TIMEZONE] = SXML_BYTE_VIEW_INITIALIZER("explicitTimezone"),
};

/* "; the minInclusive facet is '0'": the nearest registered type on the
   type's chain that has facets of the kind (Part 2, 4.1.4: those of the
   type and its bases apply; the nearest are the strictest a valid
   derivation allows), all values of a pattern or enumeration step. */
static sxml_status append_facet(sxml_schema_validator *validator,
                                const sxml_schema_message *message) {
    /* cvc-NAME-valid. */
    const sxml_byte_view constraint = message->facet_constraint;
    size_t kind_index = 0;
    const size_t kind_count = sizeof facet_names / sizeof facet_names[0];
    while (kind_index < kind_count &&
           !(constraint.bytes != NULL &&
             constraint.byte_count == facet_names[kind_index].byte_count + 10 &&
             memcmp(constraint.bytes + 4, facet_names[kind_index].bytes,
                    facet_names[kind_index].byte_count) == 0)) {
        kind_index += 1;
    }
    if (kind_index == kind_count || message->type_definition.value == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_facet_kind kind = (sxml_facet_kind)kind_index;
    sxml_type_identifier type =
        sxml_schema_get_type_definition(validator->components, message->type_definition)
            ->registered_type;
    sxml_type_description description;
    bool is_found = false;
    /* A chain of registered types ends at a built-in one (index at most
       the built-in count) or one already seen; it is at most as long as
       the registry, bounded here by the type table. */
    for (uint32_t base_count = 0;
         !is_found && type.index != SXML_TYPE_NONE &&
         base_count <= validator->components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
         base_count += 1) {
        if (sxml_type_registry_describe_type(validator->type_registry, type, &description) !=
            SXML_STATUS_OK) {
            break;
        }
        for (size_t facet_index = 0; facet_index < description.facets.facet_count;
             facet_index += 1) {
            is_found = is_found || description.facets.facets[facet_index].facet_kind == kind;
        }
        if (!is_found) {
            if (description.base_type.index == type.index) {
                break;
            }
            type = description.base_type;
        }
    }
    if (!is_found) {
        return SXML_STATUS_OK;
    }
    size_t value_count = 0;
    for (size_t facet_index = 0; facet_index < description.facets.facet_count; facet_index += 1) {
        value_count += description.facets.facets[facet_index].facet_kind == kind ? 1U : 0U;
    }
    sxml_status status = append_text(validator, TEXT("; the "));
    if (status == SXML_STATUS_OK) {
        status = append_text(validator, facet_names[kind_index]);
    }
    if (status == SXML_STATUS_OK) {
        status =
            append_text(validator, value_count > 1 ? TEXT(" facets are ") : TEXT(" facet is "));
    }
    size_t shown_count = 0;
    for (size_t facet_index = 0; status == SXML_STATUS_OK &&
                                 facet_index < description.facets.facet_count;
         facet_index += 1) {
        const sxml_facet *entry = &description.facets.facets[facet_index];
        if (entry->facet_kind != kind) {
            continue;
        }
        if (shown_count == SXML_SCHEMA_MESSAGE_LIST_LIMIT) {
            status = append_text(validator, TEXT(", ..."));
            break;
        }
        if (shown_count > 0) {
            status = append_text(validator, TEXT(", "));
        }
        if (status == SXML_STATUS_OK) {
            status = append_quoted(validator, entry->value);
        }
        shown_count += 1;
    }
    return status;
}

/* One placeholder of a message (sxml_schema_message). */
static sxml_status append_placeholder(sxml_schema_validator *validator,
                                      const sxml_schema_message *message, uint8_t placeholder) {
    switch (placeholder) {
    case 'e':
    case 'E':
    case 'a': {
        const sxml_status status =
            append_text(validator, placeholder == 'a' ? TEXT("attribute ") : TEXT("element "));
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return append_name(validator, placeholder == 'e'   ? message->element
                                      : placeholder == 'E' ? message->second_element
                                                           : message->attribute);
    }
    case 'n':
        return append_name(validator, unwritten(message->name));
    case 'v':
    case 'V':
        return append_quoted(validator,
                             placeholder == 'v' ? message->value : message->second_value);
    case 't':
    case 'T':
        return append_type(validator,
                           placeholder == 't' ? message->type_definition : message->second_type);
    case 'x':
        return append_expected(validator, message->cursor);
    case 'r':
        return append_text(validator, message->reason);
    case 'f':
        return append_facet(validator, message);
    default:
        return append_text(validator, (sxml_byte_view){.bytes = &placeholder, .byte_count = 1});
    }
}

sxml_status sxml_schema_validator_report_error(sxml_schema_validator *validator,
                                               const sxml_schema_validation_error *error) {
    const sxml_byte_view text = error->message.text;
    validator->message_bytes.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    size_t literal_start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] != '%' || byte_index + 1 == text.byte_count) {
            continue;
        }
        status = append_text(validator, (sxml_byte_view){.bytes = text.bytes + literal_start,
                                                         .byte_count = byte_index - literal_start});
        byte_index += 1;
        if (status == SXML_STATUS_OK) {
            status = append_placeholder(validator, &error->message, text.bytes[byte_index]);
        }
        literal_start = byte_index + 1;
    }
    if (status == SXML_STATUS_OK && literal_start < text.byte_count) {
        status = append_text(validator, (sxml_byte_view){.bytes = text.bytes + literal_start,
                                                         .byte_count = text.byte_count -
                                                                       literal_start});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema_report report = {
        .code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                 .local_name = error->constraint_name,
                 .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
        .description = {.bytes = validator->message_bytes.bytes,
                        .byte_count = validator->message_bytes.entry_count},
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
    /* The element it identifies. */
    uint64_t element_number;
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
        /* XSD 1.1, cvc-id.2: a value identifies one element, perhaps more
           than once. */
        const sxml_schema_id_entry *existing =
            sxml_schema_scratch_array_at(&id_table->identifiers, id_table->slots[slot] - 1U);
        outcome->is_duplicate = existing->element_number != request->element_number;
        return SXML_STATUS_OK;
    }
    const sxml_schema_id_entry entry = {.value_offset = id_table->value_bytes.entry_count,
                                        .value_byte_count = request->value.byte_count,
                                        .element_number = request->element_number};
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
    if (validator->skips_id_constraints) {
        return SXML_STATUS_OK;
    }
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
            validator, &(sxml_schema_validation_error){.constraint_name = TEXT("cvc-id.1"),
                                                       .message = {.text = TEXT(
                                                                       "the IDREF %v names no "
                                                                       "ID of the document"),
                                                                   .value = value},
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
    uint64_t element_number;
    sxml_schema_subject subject;
} value_items;

/* A message about a value's subject: the template of its kind (none,
   element, attribute), the subject's name as its element or
   attribute. */
static sxml_schema_message subject_message(sxml_schema_subject subject,
                                           const sxml_byte_view templates[3]) {
    sxml_schema_message message = {.text = templates[subject.subject_kind]};
    if (subject.subject_kind == SXML_SCHEMA_SUBJECT_ELEMENT) {
        message.element = subject.name;
    } else if (subject.subject_kind == SXML_SCHEMA_SUBJECT_ATTRIBUTE) {
        message.attribute = subject.name;
    }
    return message;
}

static const sxml_byte_view duplicate_id_templates[3] = {
    SXML_BYTE_VIEW_INITIALIZER("the ID %v is used twice in the document"),
    SXML_BYTE_VIEW_INITIALIZER("the ID %v of %e is used twice in the document"),
    SXML_BYTE_VIEW_INITIALIZER("the ID %v of %a is used twice in the document"),
};

static const sxml_byte_view entity_templates[3] = {
    SXML_BYTE_VIEW_INITIALIZER("the ENTITY %v names no unparsed entity"),
    SXML_BYTE_VIEW_INITIALIZER("the ENTITY %v of %e names no unparsed entity"),
    SXML_BYTE_VIEW_INITIALIZER("the ENTITY %v of %a names no unparsed entity"),
};

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
            /* An ID that identifies no element (the content of the
               validation root) binds nothing (3.17.5.2). */
            if (items.element_number != 0) {
                status = sxml_schema_id_table_add_id(
                    &validator->id_table,
                    &(sxml_schema_id_request){.value = string_of(item),
                                              .location = items.location,
                                              .element_number = items.element_number},
                    &id_outcome);
            }
            if (status == SXML_STATUS_OK && id_outcome.is_duplicate &&
                !validator->skips_id_constraints) {
                /* 3.17.4.2, clause 2: no two IDs are equal. */
                sxml_schema_message message =
                    subject_message(items.subject, duplicate_id_templates);
                message.value = string_of(item);
                status = sxml_schema_validator_report_error(
                    validator, &(sxml_schema_validation_error){.constraint_name = TEXT("cvc-id.2"),
                                                               .message = message,
                                                               .location = items.location});
            }
        } else if (is_derived(validator, item, SXML_TYPE_XS_IDREF)) {
            outcome->is_idrefs = true;
            status = sxml_schema_id_table_add_idref(
                &validator->id_table, &(sxml_schema_id_request){.value = string_of(item),
                                                                .location = items.location,
                                                                .element_number = 0});
        } else if (is_derived(validator, item, SXML_TYPE_XS_ENTITY) &&
                   !is_unparsed_entity(validator, string_of(item))) {
            /* 3.16.4 String Valid, clause 2: an ENTITY value names an
               unparsed entity (Part 2, 3.3.20). */
            outcome->is_valid = false;
            sxml_schema_message message = subject_message(items.subject, entity_templates);
            message.value = string_of(item);
            status = sxml_schema_validator_report_error(
                validator,
                &(sxml_schema_validation_error){.constraint_name = TEXT("cvc-simple-type"),
                                                .message = message,
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

/* A value the cast refused: what it is (its subject), its value, the
   type, and why (the facet's description without its constraint name,
   and the facet itself). */
typedef struct invalid_value {
    const sxml_schema_value_request *request;
    sxml_byte_view constraint_name;
    sxml_byte_view cast_description;
} invalid_value;

static const sxml_byte_view invalid_value_templates[3] = {
    SXML_BYTE_VIEW_INITIALIZER("the value %v is not valid for %t%r%f"),
    SXML_BYTE_VIEW_INITIALIZER("%e has the value %v, which is not valid for %t%r%f"),
    SXML_BYTE_VIEW_INITIALIZER("%a has the value %v, which is not valid for %t%r%f"),
};

static sxml_status report_invalid_value(sxml_schema_validator *validator,
                                        const invalid_value *invalid) {
    const sxml_schema_value_request *request = invalid->request;
    /* "cvc-minInclusive-valid: the value is below ...": the reason after
       the constraint name, for a facet; the lexical mapping's own text
       says no more than the constraint. */
    const sxml_byte_view constraint = invalid->constraint_name;
    sxml_schema_message message = subject_message(request->subject, invalid_value_templates);
    const sxml_byte_view cast_description = invalid->cast_description;
    if (!sxml_byte_view_equals(constraint, TEXT("cvc-datatype-valid.1.2.1")) &&
        cast_description.byte_count > constraint.byte_count + 2 &&
        cast_description.bytes[constraint.byte_count] == ':') {
        /* ": the value is below ...": the description from its colon. */
        message.reason = (sxml_byte_view){.bytes = cast_description.bytes + constraint.byte_count,
                                          .byte_count =
                                              cast_description.byte_count - constraint.byte_count};
    }
    message.value = request->text;
    message.type_definition = request->simple_type;
    message.facet_constraint = constraint;
    return sxml_schema_validator_report_error(
        validator, &(sxml_schema_validation_error){.constraint_name = constraint,
                                                   .message = message,
                                                   .location = request->location});
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
    /* A type that did not register was reported by compilation. */
    if (registered.index == SXML_TYPE_NONE) {
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
                                                                 .target_type = sxml_schema_cast_target_of(registered),
                                                                 .namespace_bindings = bindings,
                                                                 .error_record = &error_record},
                                       &value);
    if (is_value_error(status)) {
        outcome->is_valid = false;
        return report_invalid_value(
            validator, &(invalid_value){.request = request,
                                        .constraint_name = constraint_of(&error_record),
                                        .cast_description = error_record.diagnostic.description});
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
        status = examine_items(validator, (value_items){.outcome = outcome,
                                                        .location = request->location,
                                                        .element_number = request->element_number,
                                                        .subject = request->subject});
    }
    if (status == SXML_STATUS_OK) {
        /* Part 2, 4.3.13: assertion facets, evaluated with $value
           (src/schema_assertion.c, packet 5.9); a false one makes the
           value invalid (4.1.4 clause 1: facet-valid). */
        const size_t error_count = validator->error_count;
        status = sxml_schema_check_assertions(
            validator, &(sxml_schema_assertion_request){
                           .schema = validator->configuration.schema,
                           .type_definition = request->simple_type,
                           .node = {.representation = SXML_ITEM_REPRESENTATION_NONE},
                           .value = outcome->value,
                           .text = outcome->normalized_value,
                           .location = request->location});
        if (validator->error_count != error_count) {
            outcome->is_valid = false;
        }
    }
    if (status != SXML_STATUS_OK || !outcome->is_valid) {
        sxml_sequence_release(outcome->value);
        outcome->value = sxml_sequence_empty();
    }
    return status;
}
