/* Small helpers that the book's C examples share. */
#pragma once

#include <stdio.h>

#include "sxml/processor.h"
#include "sxml/serialize.h"

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Reads the file at path into contents, which holds capacity bytes, and
   returns a view of what it read: empty when the file cannot be read or
   does not fit. */
static inline sxml_byte_view read_file(const char *path, unsigned char *contents,
                                       size_t capacity) {
    sxml_byte_view bytes = {.bytes = contents, .byte_count = 0};
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        bytes.byte_count = fread(contents, 1, capacity, file);
        if (!feof(file)) {
            bytes.byte_count = 0;
        }
        fclose(file);
    }
    return bytes;
}

/* Serialized bytes go to an output stream; this one writes to stdout. */
static sxml_status write_to_stdout(void *state, sxml_byte_view bytes) {
    (void)state;
    size_t written = fwrite(bytes.bytes, 1, bytes.byte_count, stdout);
    return written == bytes.byte_count ? SXML_STATUS_OK
                                       : SXML_STATUS_INPUT_OUTPUT_FAILED;
}

static const sxml_output_stream_operations stdout_operations = {
    .write = write_to_stdout,
};

static inline sxml_output_stream stdout_stream(void) {
    return (sxml_output_stream){.operations = &stdout_operations};
}

/* Writes a sequence by the adaptive method, one item on each line. */
static inline sxml_status print_sequence(sxml_processor *processor,
                                         sxml_sequence sequence,
                                         sxml_error_record *error_record) {
    sxml_status status = sxml_serialize_sequence(
        sxml_processor_get_item_heap(processor),
        &(sxml_serialization_request){
            .value = sequence,
            .serialization_parameters =
                {.serialization_method = SXML_SERIALIZATION_METHOD_ADAPTIVE},
            .error_record = error_record,
        },
        stdout_stream());
    putchar('\n');
    return status;
}

/* Writes what went wrong: the status, and the error record when there is
   one. */
static inline void print_failure(sxml_status status,
                                 const sxml_error_record *error_record) {
    sxml_byte_view status_name = sxml_status_name(status);
    sxml_byte_view code = error_record->code.local_name;
    sxml_byte_view description = error_record->diagnostic.description;
    sxml_source_location location = error_record->diagnostic.location;
    printf("%.*s", (int)status_name.byte_count, (const char *)status_name.bytes);
    if (code.byte_count > 0) {
        printf(" err:%.*s", (int)code.byte_count, (const char *)code.bytes);
    }
    if (location.line_number > 0) {
        printf(" at %u:%u", (unsigned)location.line_number,
               (unsigned)location.column_number);
    }
    if (description.byte_count > 0) {
        printf(": %.*s", (int)description.byte_count, (const char *)description.bytes);
    }
    putchar('\n');
}
