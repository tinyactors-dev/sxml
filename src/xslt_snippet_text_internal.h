#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Snippet text (glossary `snippet`): the UTF-8 text of an XPath snippet
 * that XSLT lowering writes and then parses itself, as the key and
 * accumulator functions do (src/xslt_key.c, src/xslt_accumulator.c;
 * packet 4.17 merged their copies). Internal to sxml.
 *
 * The text grows in its allocator; the first failure is kept in status
 * and later appends do nothing, so a writer appends freely and checks
 * status once.
 */
typedef struct sxml_xslt_snippet_text {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    /* The first failure; later appends do nothing. */
    sxml_status status;
} sxml_xslt_snippet_text;

/* Appends bytes. */
void sxml_xslt_snippet_text_append(sxml_xslt_snippet_text *text, sxml_byte_view bytes);

/* Appends a string literal of C. */
#define SXML_XSLT_SNIPPET_APPEND(text, literal)                                                    \
    sxml_xslt_snippet_text_append((text), SXML_BYTE_VIEW_FROM_LITERAL(literal))

/* Appends number in decimal. */
void sxml_xslt_snippet_text_append_number(sxml_xslt_snippet_text *text, size_t number);

/* Appends value as an XPath string literal: in double quotes, an embedded
   one doubled (XPath 4.0, A.2.1). */
void sxml_xslt_snippet_text_append_string_literal(sxml_xslt_snippet_text *text,
                                                  sxml_byte_view value);

/* Appends QName("uri", "local") for name, read from name_pool. */
void sxml_xslt_snippet_text_append_qname(sxml_xslt_snippet_text *text,
                                         const sxml_name_pool *name_pool,
                                         sxml_name_identifier name);

/* The text so far, borrowed from text. */
sxml_byte_view sxml_xslt_snippet_text_view(const sxml_xslt_snippet_text *text);

/* Releases the bytes; text is empty afterwards (allocator and status
   stay). */
void sxml_xslt_snippet_text_release(sxml_xslt_snippet_text *text);
