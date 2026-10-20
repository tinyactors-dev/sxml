/*
 * The text form writer (src/text_form_internal.h; D246): the bounded
 * writer the four text forms each had a copy of (D182).
 *
 * Golden file: src/ir.c (its text_writer, where the code began).
 */
#include "text_form_internal.h"

void sxml_text_form_write_bytes(sxml_text_form_writer *writer, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        const uint8_t byte = bytes.bytes[byte_index];
        if (writer->byte_count < writer->output.byte_count) {
            writer->output.bytes[writer->byte_count] = byte;
        }
        writer->byte_count += 1;
        writer->column = byte == '\n' ? 0 : writer->column + 1;
    }
}

// ast-grep-ignore: pointer-parameter-without-length
void sxml_text_form_write_text(sxml_text_form_writer *writer, const char *text) {
    sxml_text_form_write_bytes(writer, sxml_byte_view_from_c_string(text));
}

void sxml_text_form_write_unsigned(sxml_text_form_writer *writer, uint64_t number) {
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + (uint8_t)(number % 10U));
        number /= 10U;
        digit_count += 1;
    } while (number != 0);
    sxml_text_form_write_bytes(writer,
                               (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                                .byte_count = digit_count});
}
