/*
 * The option parameter conventions (F&O 4.0, section 1.7) for every
 * library function that adopts them (packet 2.17): one reader of options
 * maps, declared in src/function_library_core_internal.h. Not a library
 * part: it defines no row view.
 *
 * Golden file: src/function_library_json.c (json_read_options, which this
 * generalizes from a fixed list of value kinds to sequence types).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"

static const sxml_static_error unknown_option_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The options map has a key that is not an option of this function (F&O 4.0, "
        "section 1.7, option parameter conventions)"),
};

/* The text of a key that may name an option: an xs:string,
   xs:untypedAtomic or xs:anyURI ("the same key as the required string"). */
static bool read_key_text(sxml_item key, sxml_byte_view *text) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(key, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return false;
    }
    *text = datum.datum_details.string.utf8;
    return true;
}

/* True for an xs:QName key with a namespace: an implementation-defined
   option, which sxml ignores. */
static bool key_is_extension(sxml_item key, const sxml_name_pool *name_pool) {
    sxml_atomic_datum datum;
    sxml_name_parts parts;
    return sxml_atomic_item_get_datum(key, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_QNAME &&
           sxml_name_pool_get_name_parts(name_pool, datum.datum_details.qname.expanded_name,
                                         &parts) == SXML_STATUS_OK &&
           parts.namespace_uri.index != SXML_NAMESPACE_NONE;
}

/* The definition a key names, or definition_count. */
static size_t find_definition(sxml_option_definition_view definitions, sxml_item key) {
    sxml_byte_view key_text = {.bytes = NULL, .byte_count = 0};
    if (!read_key_text(key, &key_text)) {
        return definitions.definition_count;
    }
    size_t definition_index = 0;
    while (definition_index < definitions.definition_count &&
           !sxml_byte_view_equals(key_text,
                                  definitions.definitions[definition_index].option_name)) {
        definition_index += 1;
    }
    return definition_index;
}

void sxml_function_library_release_options(sxml_option_values *values) {
    for (size_t option_index = 0; option_index < SXML_FUNCTION_OPTION_LIMIT;
         option_index += 1) {
        sxml_sequence_release(values->values[option_index]);
        values->values[option_index] = sxml_sequence_empty();
        values->is_present[option_index] = false;
    }
}

sxml_status sxml_function_library_read_options(const sxml_function_call *call,
                                               const sxml_option_request *request,
                                               sxml_option_values *values) {
    const sxml_option_definition_view definitions = request->definitions;
    if (definitions.definition_count > SXML_FUNCTION_OPTION_LIMIT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item options = {.has_item = false};
    sxml_status status =
        sxml_function_library_read_optional_item(call, request->argument_index, &options);
    if (status != SXML_STATUS_OK || !options.has_item) {
        return status;
    }
    size_t entry_count = 0;
    status = sxml_map_get_size(options.item, &entry_count);
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(call->item_heap);
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(options.item, entry_index, &entry);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const size_t found = find_definition(definitions, entry.key);
        if (found == definitions.definition_count) {
            if (key_is_extension(entry.key, name_pool)) {
                continue;
            }
            return sxml_function_library_report(call, &unknown_option_error);
        }
        sxml_sequence coerced = sxml_sequence_empty();
        if (call->dynamic_context == NULL) {
            sxml_sequence_retain(entry.value);
            coerced = entry.value;
        } else {
            status = sxml_evaluator_coerce(
                call->dynamic_context,
                &(sxml_coercion_request){.value = entry.value,
                                         .sequence_type = definitions.definitions[found].option_type,
                                         .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
                                         .error_record = call->error_record},
                &coerced);
        }
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(values->values[found]);
            values->values[found] = coerced;
            values->is_present[found] = true;
        }
    }
    return status;
}

/* The datum of a present option of one atomic item. */
static bool read_option_datum(sxml_option_lookup lookup, sxml_atomic_datum *datum) {
    if (lookup.option_index >= SXML_FUNCTION_OPTION_LIMIT ||
        !lookup.values->is_present[lookup.option_index]) {
        return false;
    }
    const sxml_sequence value = lookup.values->values[lookup.option_index];
    return sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM &&
           sxml_item_get_kind(value.content) == SXML_ITEM_KIND_ATOMIC &&
           sxml_atomic_item_get_datum(value.content, datum) == SXML_STATUS_OK;
}

bool sxml_function_library_option_is_true(sxml_option_lookup lookup) {
    sxml_atomic_datum datum;
    return read_option_datum(lookup, &datum) && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
           datum.datum_details.boolean;
}

bool sxml_function_library_option_is_false(sxml_option_lookup lookup) {
    sxml_atomic_datum datum;
    return read_option_datum(lookup, &datum) && datum.datum_kind == SXML_DATUM_KIND_BOOLEAN &&
           !datum.datum_details.boolean;
}

sxml_string sxml_function_library_option_string(sxml_option_lookup lookup) {
    sxml_atomic_datum datum;
    if (read_option_datum(lookup, &datum) && datum.datum_kind == SXML_DATUM_KIND_STRING) {
        return datum.datum_details.string;
    }
    return (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                         .codepoint_count = 0,
                         .is_ascii = true};
}
