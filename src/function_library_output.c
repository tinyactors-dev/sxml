/*
 * The XSLT 4.0 functions of output (src/xslt_output_internal.h; XSLT 4.0,
 * 26.1.7 and 26.3; docs/DECISIONS.md D101, D156; packet 4.13): one library
 * part, `output`, that only XSLT compilation names, as `xslt` (D149).
 *
 * - fn:current-output-uri reads the current output URI from the result
 *   writer of the transformation (the base output URI, or the absolute
 *   href of the xsl:result-document being evaluated); the empty sequence
 *   outside a transformation and in temporary output state.
 * - fn:character-map returns a named character map of the stylesheet as a
 *   map from one-character strings to their replacements.
 *
 * Golden file: src/function_library_xslt.c (rows of an XSLT-only part).
 */
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "function_library_parts_internal.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "xslt_output_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An xs:string or xs:anyURI item of text, owned. */
typedef struct text_item_request {
    sxml_builtin_type type;
    sxml_byte_view text;
} text_item_request;

static sxml_status make_text_item(sxml_item_heap *item_heap, text_item_request request,
                                  sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(request.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = request.text,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        item);
}

/* fn:current-output-uri() as xs:anyURI? (26.3.1). */
static sxml_status call_current_output_uri(const sxml_function_call *call,
                                           sxml_sequence *result) {
    *result = sxml_sequence_empty();
    const sxml_xslt_current_output_uri current =
        sxml_xslt_result_writer_get_current_output_uri(sxml_xslt_result_writer_from_call(call));
    if (!current.is_present) {
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    const sxml_status status = make_text_item(
        call->item_heap, (text_item_request){.type = SXML_TYPE_XS_ANY_URI, .text = current.uri},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* The UTF-8 bytes of one codepoint. */
typedef struct encoded_codepoint {
    uint8_t bytes[4];
    size_t byte_count;
} encoded_codepoint;

static encoded_codepoint encode_codepoint(uint32_t codepoint) {
    encoded_codepoint encoded = {.bytes = {0, 0, 0, 0}, .byte_count = 0};
    if (codepoint < 0x80U) {
        encoded.bytes[0] = (uint8_t)codepoint;
        encoded.byte_count = 1;
    } else if (codepoint < 0x800U) {
        encoded.bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        encoded.bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 2;
    } else if (codepoint < 0x10000U) {
        encoded.bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded.bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 3;
    } else {
        encoded.bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
        encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
        encoded.bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded.bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded.byte_count = 4;
    }
    return encoded;
}

/* Releases the keys and values of the first entry_count entries. */
typedef struct entry_list {
    sxml_map_entry *entries;
    size_t entry_count;
} entry_list;

static void release_entries(entry_list list) {
    for (size_t entry_index = 0; entry_index < list.entry_count; entry_index += 1) {
        sxml_item_release(list.entries[entry_index].key);
        sxml_sequence_release(list.entries[entry_index].value);
    }
}

/* fn:character-map($name as xs:QName) as map(xs:string, xs:string)?
   (26.1.7.1). */
static sxml_status call_character_map(const sxml_function_call *call, sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_optional_item name = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &name);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && name.has_item) {
        status = sxml_atomic_item_get_datum(name.item, &datum);
    }
    if (status != SXML_STATUS_OK || !name.has_item) {
        return status;
    }
    const sxml_xslt_character_map_answer answer = sxml_xslt_stylesheet_outputs_find_character_map(
        sxml_xslt_result_writer_get_outputs(sxml_xslt_result_writer_from_call(call)),
        datum.datum_details.qname.expanded_name);
    if (!answer.is_found) {
        return SXML_STATUS_OK;
    }
    const size_t mapping_count = answer.character_map.character_mapping_count;
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    void *memory = NULL;
    size_t byte_count = 0;
    status = sxml_size_multiply(mapping_count == 0 ? 1 : mapping_count, sizeof(sxml_map_entry),
                                &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_map_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    entry_list list = {.entries = memory, .entry_count = 0};
    for (size_t mapping_index = 0; status == SXML_STATUS_OK && mapping_index < mapping_count;
         mapping_index += 1) {
        const sxml_character_mapping mapping =
            answer.character_map.character_mappings[mapping_index];
        const encoded_codepoint character = encode_codepoint(mapping.codepoint);
        sxml_item key = {.representation = 0};
        sxml_item value = {.representation = 0};
        status = make_text_item(
            call->item_heap,
            (text_item_request){.type = SXML_TYPE_XS_STRING,
                                .text = {.bytes = character.bytes,
                                         .byte_count = character.byte_count}},
            &key);
        if (status == SXML_STATUS_OK) {
            status = make_text_item(call->item_heap,
                                    (text_item_request){.type = SXML_TYPE_XS_STRING,
                                                        .text = mapping.replacement_text},
                                    &value);
            if (status != SXML_STATUS_OK) {
                sxml_item_release(key);
            }
        }
        if (status == SXML_STATUS_OK) {
            list.entries[list.entry_count] =
                (sxml_map_entry){.key = key, .value = sxml_sequence_from_item(value)};
            list.entry_count += 1;
        }
    }
    sxml_item map = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_map_make(
            call->item_heap,
            &(sxml_map_request){.map_entries = {.map_entries = list.entries,
                                                .map_entry_count = list.entry_count},
                                .duplicate_key_policy = SXML_DUPLICATE_KEY_POLICY_USE_LAST,
                                .error_record = call->error_record},
            &map);
    }
    release_entries(list);
    sxml_allocator_release(allocator, memory);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(map);
    }
    return status;
}

static const sxml_function_parameter character_map_parameters[] = {
    SXML_REQUIRED_PARAMETER("name", "xs:QName"),
};

static const sxml_native_function output_functions[] = {
    SXML_NULLARY_FUNCTION_ROW("current-output-uri", "xs:anyURI?",
                              (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                              call_current_output_uri),
    SXML_FUNCTION_ROW("character-map", character_map_parameters,
                      "map(xs:string, xs:string)?",
                      (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT, call_character_map),
};

const sxml_native_function_view sxml_function_library_output = SXML_LIBRARY_PART(output_functions);

const sxml_language_set sxml_function_library_output_languages = SXML_LANGUAGE_XSLT;
