/*
 * The hash part of the function library (packet 2.11): fn:hash (F&O 4.0,
 * section 5.3.18 "fn:hash") over the algorithms of src/hash_internal.h.
 *
 * Golden file: src/function_library_math.c (a leaf part: rows, argument
 * reading, errors). The algorithm name is matched by its effective value,
 * fn:upper-case(fn:normalize-space($algorithm)); no name sxml supports is
 * longer than 7 octets, so a longer effective value is FOHA0001 without
 * being built.
 */
#include "function_library_core_internal.h"
#include "hash_internal.h"

static const sxml_static_error unknown_algorithm_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOHA0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The hash algorithm is not one sxml supports: MD5, SHA-1, SHA-256, BLAKE3 "
        "or CRC-32 (F&O 4.0, fn:hash)"),
};

/* The effective name of the algorithm while it is built: at most this many
   octets are kept. */
#define SXML_ALGORITHM_NAME_CAPACITY ((size_t)8)

typedef struct algorithm_name {
    uint8_t bytes[SXML_ALGORITHM_NAME_CAPACITY];
    size_t byte_count;
    /* False once the name is too long or has a character outside ASCII. */
    bool can_match;
} algorithm_name;

static bool is_whitespace(uint32_t codepoint) {
    return codepoint == 0x20 || codepoint == 0x09 || codepoint == 0x0A || codepoint == 0x0D;
}

static void append_name_codepoint(algorithm_name *name, uint32_t codepoint) {
    if (codepoint > 0x7E || name->byte_count == SXML_ALGORITHM_NAME_CAPACITY) {
        name->can_match = false;
        return;
    }
    name->bytes[name->byte_count] = (uint8_t)codepoint;
    name->byte_count += 1;
}

/* fn:upper-case(fn:normalize-space($algorithm)) (F&O 4.0, fn:hash), as far
   as it can name an algorithm. */
static void compute_effective_name(sxml_string text, algorithm_name *name) {
    *name = (algorithm_name){.byte_count = 0, .can_match = true};
    sxml_codepoint_iterator iterator = {.string = text, .byte_offset = 0};
    uint32_t codepoint = 0;
    bool has_pending_space = false;
    while (name->can_match && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        if (is_whitespace(codepoint)) {
            has_pending_space = name->byte_count > 0;
            continue;
        }
        if (has_pending_space) {
            append_name_codepoint(name, ' ');
            has_pending_space = false;
        }
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = codepoint,
                                        .mapping_kind = SXML_CASE_MAPPING_KIND_UPPER},
            &mapping);
        for (size_t mapped_index = 0; mapped_index < mapping.codepoint_count; mapped_index += 1) {
            append_name_codepoint(name, mapping.codepoints[mapped_index]);
        }
    }
}

/* Reads $algorithm (argument 1): MD5 when absent or empty. */
static sxml_status read_algorithm(const sxml_function_call *call,
                                  sxml_hash_algorithm_lookup *lookup) {
    *lookup = (sxml_hash_algorithm_lookup){.is_found = true,
                                           .algorithm = SXML_HASH_ALGORITHM_MD5};
    sxml_optional_item argument;
    sxml_status status = sxml_function_library_read_optional_item(call, 1, &argument);
    if (status != SXML_STATUS_OK || !argument.has_item) {
        return status;
    }
    sxml_string text;
    status = sxml_function_library_read_string_argument(call, 1, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    algorithm_name name;
    compute_effective_name(text, &name);
    lookup->is_found = false;
    if (name.can_match) {
        sxml_hash_find_algorithm((sxml_byte_view){.bytes = name.bytes,
                                                  .byte_count = name.byte_count},
                                 lookup);
    }
    return status;
}

/*
 * fn:hash($value as (xs:string | xs:hexBinary | xs:base64Binary)?,
 * $algorithm as xs:string? := "MD5", $options as map(*)? := {}) as
 * xs:hexBinary? (F&O 4.0, section 5.3.18): the empty sequence for an empty
 * $value; a string hashed as its UTF-8 octets, a binary value as its
 * octets. No option is defined, so any key that is not a QName in a
 * namespace is XPTY0004 (F&O 4.0, section 1.7). One step is charged per
 * 64 octets.
 */
static sxml_status call_hash(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_option_values values = {.is_present = {false}};
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 2, .definitions = {.definitions = NULL,
                                                                    .definition_count = 0}},
        &values);
    sxml_function_library_release_options(&values);
    sxml_hash_algorithm_lookup lookup;
    if (status == SXML_STATUS_OK) {
        status = read_algorithm(call, &lookup);
    }
    if (status == SXML_STATUS_OK && !lookup.is_found) {
        return sxml_function_library_report(call, &unknown_algorithm_error);
    }
    sxml_optional_item value;
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 0, &value);
    }
    if (status != SXML_STATUS_OK || !value.has_item) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(value.item, &datum);
    sxml_byte_view message = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        if (datum.datum_kind == SXML_DATUM_KIND_STRING) {
            message = datum.datum_details.string.utf8;
        } else if (datum.datum_kind == SXML_DATUM_KIND_BINARY) {
            message = datum.datum_details.binary;
        } else {
            /* Coercion to the parameter type allows nothing else. */
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_charge_steps(call, (uint64_t)message.byte_count / 64U + 1U);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_digest digest;
    sxml_hash_compute(&(sxml_hash_request){.algorithm = lookup.algorithm, .message = message},
                      &digest);
    sxml_item item = {.representation = 0};
    status = sxml_atomic_item_make(
        call->item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_HEX_BINARY),
            .datum = {.datum_kind = SXML_DATUM_KIND_BINARY,
                      .datum_details = {.binary = {.bytes = digest.bytes,
                                                   .byte_count = digest.byte_count}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* ------------------------------------------------------------------ Rows */

static const sxml_function_parameter hash_parameters[] = {
    SXML_REQUIRED_PARAMETER("value", "(xs:string | xs:hexBinary | xs:base64Binary)?"),
    SXML_OPTIONAL_PARAMETER("algorithm", "xs:string?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_native_function hash_functions[] = {
    SXML_FUNCTION_ROW("hash", hash_parameters, "xs:hexBinary?", 0, call_hash),
};

const sxml_native_function_view sxml_function_library_hash = SXML_LIBRARY_PART(hash_functions);
