/*
 * The serialize part of the function library (packet 3.4, D128):
 * fn:serialize (F&O 4.0, 17.2.3). The options become serialization
 * parameters through sxml_serialization_parameter_reader, the input is
 * serialized into an output buffer by sxml_serialize_sequence, and the
 * bytes become the string.
 *
 * Golden file: src/function_library_json.c (fn:xml-to-json writes into an
 * output buffer and returns its bytes as a string).
 */
#include "evaluator_internal.h"
#include "function_library_internal.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "xdm_internal.h"

#include <string.h>

/* The allocator for work memory: the dynamic context's, or the heap's for
   a call from outside an evaluation. */
static sxml_allocator serialize_call_allocator(const sxml_function_call *call) {
    return call->dynamic_context != NULL ? call->dynamic_context->allocator
                                         : sxml_item_heap_as_allocator(call->item_heap);
}

static sxml_sequence serialize_argument(const sxml_function_call *call, size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

/* Re-encodes ISO-8859-1 bytes as UTF-8 into buffer (two bytes at most per
   byte): F&O 4.0, 17.2.3 skips the encoding phase, so the string holds the
   characters, with the encoding's character references. */
typedef struct serialize_latin1_text {
    sxml_byte_view bytes;
    sxml_allocator allocator;
} serialize_latin1_text;

static sxml_status serialize_decode_latin1(serialize_latin1_text text, uint8_t **decoded,
                                           size_t *decoded_count) {
    if (text.bytes.byte_count == 0) {
        *decoded = NULL;
        *decoded_count = 0;
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(text.bytes.byte_count, 2, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            text.allocator, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *output = memory;
    size_t count = 0;
    for (size_t offset = 0; offset < text.bytes.byte_count; offset += 1) {
        const uint8_t byte = text.bytes.bytes[offset];
        if (byte < 0x80U) {
            output[count++] = byte;
        } else {
            output[count++] = (uint8_t)(0xC0U | (byte >> 6));
            output[count++] = (uint8_t)(0x80U | (byte & 0x3FU));
        }
    }
    *decoded = output;
    *decoded_count = count;
    return SXML_STATUS_OK;
}

/* fn:serialize($input as item()*, $options as
   (element(output:serialization-parameters) | map(*))? := {}) as xs:string
   (F&O 4.0, 17.2.3). The choice type of $options is checked by the
   reader (XPTY0004), as D78 leaves choice types to bodies. */
static sxml_status call_serialize(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_allocator allocator = serialize_call_allocator(call);
    sxml_serialization_parameter_reader *reader = NULL;
    sxml_status status = sxml_serialization_parameter_reader_create(
        allocator,
        &(sxml_serialization_parameter_reader_configuration){.allocator = allocator,
                                                             .item_heap = call->item_heap},
        &reader);
    sxml_serialization_parameters parameters = {.serialization_method =
                                                    SXML_SERIALIZATION_METHOD_XML};
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_read(
            reader,
            &(sxml_serialization_parameter_read_request){.options = serialize_argument(call, 1),
                                                         .error_record = call->error_record},
            &parameters);
    }
    /* The encoding phase is skipped: UTF-16 has every character, so the
       string is made as UTF-8; no byte order mark. */
    const bool is_latin1 = parameters.character_encoding == SXML_CHARACTER_ENCODING_ISO_8859_1;
    if (parameters.character_encoding == SXML_CHARACTER_ENCODING_UTF_16) {
        parameters.character_encoding = SXML_CHARACTER_ENCODING_UTF_8;
    }
    parameters.byte_order_mark = SXML_SERIALIZATION_FLAG_NO;
    sxml_output_buffer *output = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator,
            &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &output);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            call->item_heap,
            &(sxml_serialization_request){.value = serialize_argument(call, 0),
                                          .serialization_parameters = parameters,
                                          .error_record = call->error_record},
            sxml_output_buffer_as_output_stream(output));
    }
    uint8_t *decoded = NULL;
    sxml_byte_view bytes = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        bytes = sxml_output_buffer_get_bytes(output);
    }
    if (status == SXML_STATUS_OK && is_latin1) {
        size_t decoded_count = 0;
        status = serialize_decode_latin1(
            (serialize_latin1_text){.bytes = bytes, .allocator = allocator}, &decoded,
            &decoded_count);
        bytes = (sxml_byte_view){.bytes = decoded, .byte_count = decoded_count};
    }
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    if (status == SXML_STATUS_OK) {
        status = sxml_string_from_utf8(bytes, &string);
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
    sxml_allocator_release(allocator, decoded);
    sxml_output_buffer_destroy(output);
    sxml_serialization_parameter_reader_destroy(reader);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

static const sxml_function_parameter serialize_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("input"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
     .parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED},
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("options"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("item()?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY},
};

static const sxml_native_function serialize_functions[] = {
    {.function_namespace = SXML_NAMESPACE_FUNCTIONS,
     .properties = 0,
     .local_name = SXML_BYTE_VIEW_INITIALIZER("serialize"),
     .parameters = {serialize_parameters,
                    sizeof(serialize_parameters) / sizeof(serialize_parameters[0])},
     .result_type = SXML_BYTE_VIEW_INITIALIZER("xs:string"),
     .call = call_serialize,
     .resumable_body = NULL},
};

const sxml_native_function_view sxml_function_library_serialize = {
    .native_functions = serialize_functions,
    .native_function_count = sizeof(serialize_functions) / sizeof(serialize_functions[0]),
};
