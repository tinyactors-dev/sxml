#pragma once

/*
 * The DUCET the sxml command serves to the UCA collation feature
 * (include/sxml/processor.h, SXML_RESOURCE_KIND_COLLATION_DATA; D213):
 * third_party/unicode-collation/allkeys.txt without its comments, compiled
 * into the program by apps/sxml/embed-collation-data.cmake as string
 * literals whose concatenation is the text. The library itself has no
 * copy (D184, D189); the resolver of query_command.c joins the chunks
 * when the feature first asks.
 */
#include <stddef.h>

typedef struct sxml_cli_text_chunks {
    const char *const *chunks;
    size_t chunk_count;
} sxml_cli_text_chunks;

/* The chunks of the DUCET's text, static. */
sxml_cli_text_chunks sxml_cli_collation_data(void);
