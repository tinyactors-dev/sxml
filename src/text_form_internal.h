#pragma once

/*
 * The text form writer (glossary `text_form_writer`; D246): what the text
 * forms of the IR, the XSLT AST, the schema components and the content
 * models (src/ir.c, src/xslt_ast.c, src/schema_text.c,
 * src/schema_content_model.c) write through. It writes into caller memory
 * what fits and counts every byte, so a caller asks for the byte count
 * with an empty output and writes again with enough memory. Internal to
 * sxml; defined in src/text_form.c.
 *
 * Thread safety: no state besides the writer, which is the caller's.
 */
#include "sxml/memory.h"
#include "sxml/view.h"

#include <stddef.h>
#include <stdint.h>

typedef struct sxml_text_form_writer {
    sxml_memory_view output;
    /* Bytes the whole text needs; may exceed the output's byte count. */
    size_t byte_count;
    /* Bytes since the last newline: the column of the next byte. */
    size_t column;
} sxml_text_form_writer;

/* Writes bytes, as far as the output holds them, and counts them. */
void sxml_text_form_write_bytes(sxml_text_form_writer *writer, sxml_byte_view bytes);

/* Writes a NUL-terminated literal of the caller. */
// ast-grep-ignore: pointer-parameter-without-length
void sxml_text_form_write_text(sxml_text_form_writer *writer, const char *text);

/* Writes number in decimal digits. */
void sxml_text_form_write_unsigned(sxml_text_form_writer *writer, uint64_t number);
